#include <cstdio>

#include "sublift/version.hpp"

// Product CLI remains Python until cutover. This binary is a native shell only.
int main() {
  std::printf("sublift %.*s\n", static_cast<int>(sublift::version().size()),
              sublift::version().data());
  return 0;
}
