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

namespace sublift::adapters::ffmpeg::detail {

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
                                std::chrono::milliseconds timeout,
                                const std::atomic<bool>* cancelled,
                                const std::function<void(std::string_view)>& stdout_chunk,
                                const std::function<bool()>& should_cancel) {
  SubprocessResult res;
  if ((cancelled && cancelled->load(std::memory_order_acquire)) ||
      (should_cancel && should_cancel())) {
    res.cancelled = true;
    return res;
  }
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
  posix_spawn_file_actions_addclose(&actions, stdout_pipe[1]);
  posix_spawn_file_actions_addclose(&actions, stderr_pipe[1]);
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

  struct OwnedProcess {
    pid_t pid;
    int out;
    int err;
    bool reaped{false};
    bool complete{false};
    ~OwnedProcess() {
      if (out >= 0) ::close(out);
      if (err >= 0) ::close(err);
      if (!complete) ::kill(pid, SIGKILL);
      if (!reaped) {
        while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
      }
    }
  } owned{pid, stdout_pipe[0], stderr_pipe[0]};

  fcntl(owned.out, F_SETFL, O_NONBLOCK);
  fcntl(owned.err, F_SETFL, O_NONBLOCK);
  const auto start_time = std::chrono::steady_clock::now();
  int status = 0;
  char buffer[4096];
  while (owned.out >= 0 || owned.err >= 0 || !owned.reaped) {
    res.cancelled = (cancelled && cancelled->load(std::memory_order_acquire)) ||
                    (should_cancel && should_cancel());
    res.timed_out = std::chrono::steady_clock::now() - start_time >= timeout;
    if (res.cancelled || res.timed_out) {
      res.exit_code = -1;
      break;
    }
    if (!owned.reaped) {
      const auto result = ::waitpid(pid, &status, WNOHANG);
      if (result == pid) {
        owned.reaped = true;
        res.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) :
                        WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
      } else if (result < 0 && errno != EINTR) {
        break;
      }
    }
    if (owned.out < 0 && owned.err < 0 && owned.reaped) {
      break;
    }
    pollfd fds[2]{{owned.out, POLLIN, 0}, {owned.err, POLLIN, 0}};
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time);
    const auto remaining = std::clamp<std::int64_t>((timeout - elapsed).count(), 1, 100);
    const int result = ::poll(fds, 2, static_cast<int>(remaining));
    if (result < 0) {
      if (errno == EINTR) continue;
      break;
    }
    for (int i = 0; i < 2; ++i) {
      int& fd = i == 0 ? owned.out : owned.err;
      if (fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) {
        continue;
      }
      const auto count = ::read(fd, buffer, sizeof(buffer));
      if (count > 0) {
        if (i == 0 && stdout_chunk) {
          stdout_chunk(std::string_view(buffer, static_cast<std::size_t>(count)));
        }
        auto& content = i == 0 ? res.stdout_str : res.stderr_str;
        content.append(buffer, static_cast<std::size_t>(count));
        if ((i == 1 || stdout_chunk) && content.size() > 65536) {
          content.erase(0, content.size() - 65536);
        }
      } else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        ::close(fd);
        fd = -1;
      }
    }
  }

  owned.complete = owned.reaped && owned.out < 0 && owned.err < 0 && !res.cancelled && !res.timed_out;
  return res;
}

}  // namespace sublift::adapters::ffmpeg::detail
