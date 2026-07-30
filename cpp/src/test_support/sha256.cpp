#include "sublift/test_support.hpp"

#include "sublift/hash.hpp"

namespace sublift::test_support {

std::string sha256_prefixed(const std::uint8_t* data, std::size_t len) {
  return sublift::sha256_prefixed(data, len);
}

}  // namespace sublift::test_support
