#include <catch2/catch_test_macros.hpp>

#include "sublift/test_support.hpp"

#include <cstdint>
#include <string>
#include <vector>

TEST_CASE("sha256_prefixed known answers (FIPS vectors)", "[test_support][sha256]") {
  // empty message
  REQUIRE(sublift::test_support::sha256_prefixed(nullptr, 0) ==
          "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

  // "abc"
  const std::uint8_t abc[] = {'a', 'b', 'c'};
  REQUIRE(sublift::test_support::sha256_prefixed(abc, 3) ==
          "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  // "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"
  const std::string longer =
      "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  REQUIRE(sublift::test_support::sha256_prefixed(
              reinterpret_cast<const std::uint8_t*>(longer.data()), longer.size()) ==
          "sha256:248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}
