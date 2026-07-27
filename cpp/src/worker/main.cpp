#include <cstdio>

#include "sublift/version.hpp"

// UDS/JSON worker loop is feat-065xx. This shell only proves the target links.
int main() {
  std::printf("sublift-worker %.*s\n", static_cast<int>(sublift::version().size()),
              sublift::version().data());
  return 0;
}
