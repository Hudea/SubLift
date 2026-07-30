#include "process_utils.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <vector>

extern "C" char** environ;

namespace sublift::ffmpeg::detail {

std::string read_stderr_tail(std::string_view stderr_str, std::size_t max_chars) {
  if (stderr_str.empty()) {
    return "";
  }
  std::string_view tail = stderr_str;
  if (tail.size() > max_chars) {
    tail = tail.substr(tail.size() - max_chars);
  }

  std::string result;
  result.reserve(tail.size());
  bool in_space = false;

  for (char c : tail) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      if (!in_space && !result.empty()) {
        result.push_back(' ');
        in_space = true;
      }
    } else {
      result.push_back(c);
      in_space = false;
    }
  }

  if (!result.empty() && result.back() == ' ') {
    result.pop_back();
  }

  return result;
}

SubprocessResult run_subprocess(const std::vector<std::string>& args,
                                std::chrono::milliseconds timeout) {
  SubprocessResult res;
  if (args.empty()) {
    return res;
  }

  int stdout_pipe[2] = {-1, -1};
  int stderr_pipe[2] = {-1, -1};
  if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
    if (stdout_pipe[0] != -1) { close(stdout_pipe[0]); close(stdout_pipe[1]); }
    if (stderr_pipe[0] != -1) { close(stderr_pipe[0]); close(stderr_pipe[1]); }
    return res;
  }

  int devnull_fd = open("/dev/null", O_RDONLY);

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);

  if (devnull_fd >= 0) {
    posix_spawn_file_actions_adddup2(&actions, devnull_fd, STDIN_FILENO);
  }
  posix_spawn_file_actions_adddup2(&actions, stdout_pipe[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, stderr_pipe[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, stdout_pipe[0]);
  posix_spawn_file_actions_addclose(&actions, stderr_pipe[0]);
  if (devnull_fd >= 0) {
    posix_spawn_file_actions_addclose(&actions, devnull_fd);
  }

  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (const auto& arg : args) {
    argv.push_back(const_cast<char*>(arg.c_str()));
  }
  argv.push_back(nullptr);

  pid_t pid = 0;
  int spawn_err = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);

  posix_spawn_file_actions_destroy(&actions);
  if (devnull_fd >= 0) {
    close(devnull_fd);
  }
  close(stdout_pipe[1]);
  close(stderr_pipe[1]);

  if (spawn_err != 0) {
    close(stdout_pipe[0]);
    close(stderr_pipe[0]);
    res.exit_code = -1;
    return res;
  }

  // Set pipe read ends non-blocking
  fcntl(stdout_pipe[0], F_SETFL, O_NONBLOCK);
  fcntl(stderr_pipe[0], F_SETFL, O_NONBLOCK);

  pollfd fds[2];
  fds[0].fd = stdout_pipe[0];
  fds[0].events = POLLIN;
  fds[1].fd = stderr_pipe[0];
  fds[1].events = POLLIN;

  auto start_time = std::chrono::steady_clock::now();
  bool stdout_open = true;
  bool stderr_open = true;

  char buffer[4096];

  while (stdout_open || stderr_open) {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time);
    if (elapsed >= timeout) {
      res.timed_out = true;
      kill(pid, SIGTERM);
      usleep(50000);
      int status = 0;
      if (waitpid(pid, &status, WNOHANG) == 0) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
      }
      break;
    }

    int remaining_ms = static_cast<int>((timeout - elapsed).count());
    if (remaining_ms < 10) remaining_ms = 10;

    int poll_ret = poll(fds, 2, remaining_ms);
    if (poll_ret < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (poll_ret == 0) {
      continue;
    }

    if (fds[0].fd != -1 && (fds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
      ssize_t bytes_read = read(fds[0].fd, buffer, sizeof(buffer));
      if (bytes_read > 0) {
        res.stdout_str.append(buffer, static_cast<size_t>(bytes_read));
      } else if (bytes_read == 0 ||
                 (bytes_read < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        close(fds[0].fd);
        fds[0].fd = -1;
        stdout_open = false;
      }
    }

    if (fds[1].fd != -1 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
      ssize_t bytes_read = read(fds[1].fd, buffer, sizeof(buffer));
      if (bytes_read > 0) {
        res.stderr_str.append(buffer, static_cast<size_t>(bytes_read));
      } else if (bytes_read == 0 ||
                 (bytes_read < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        close(fds[1].fd);
        fds[1].fd = -1;
        stderr_open = false;
      }
    }
  }

  if (fds[0].fd != -1) close(fds[0].fd);
  if (fds[1].fd != -1) close(fds[1].fd);

  int status = 0;
  if (!res.timed_out) {
    if (waitpid(pid, &status, 0) > 0) {
      if (WIFEXITED(status)) {
        res.exit_code = WEXITSTATUS(status);
      } else if (WIFSIGNALED(status)) {
        res.exit_code = 128 + WTERMSIG(status);
      }
    }
  }

  return res;
}

}  // namespace sublift::ffmpeg::detail
