#include "sublift/test_support.hpp"

#include <string>
#include <string_view>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace sublift::test_support {

std::string utf8_nfc(std::string_view text) {
#if defined(__APPLE__)
  if (text.empty()) {
    return {};
  }
  CFStringRef in = CFStringCreateWithBytes(
      kCFAllocatorDefault, reinterpret_cast<const UInt8*>(text.data()),
      static_cast<CFIndex>(text.size()), kCFStringEncodingUTF8,
      /*isExternalRepresentation=*/false);
  if (in == nullptr) {
    return std::string{text};
  }
  CFMutableStringRef mut =
      CFStringCreateMutableCopy(kCFAllocatorDefault, 0, in);
  CFRelease(in);
  if (mut == nullptr) {
    return std::string{text};
  }
  CFStringNormalize(mut, kCFStringNormalizationFormC);
  CFIndex len = CFStringGetLength(mut);
  CFIndex max_bytes =
      CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
  std::string out(static_cast<std::size_t>(max_bytes), '\0');
  Boolean ok = CFStringGetCString(mut, out.data(), max_bytes,
                                  kCFStringEncodingUTF8);
  CFRelease(mut);
  if (!ok) {
    return std::string{text};
  }
  out.resize(std::char_traits<char>::length(out.c_str()));
  return out;
#else
  return std::string{text};
#endif
}

}  // namespace sublift::test_support
