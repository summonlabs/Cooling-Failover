#include "cooling_failover/version.hpp"

#include <string>

#define CF_STRINGIZE_IMPL(x) #x
#define CF_STRINGIZE(x) CF_STRINGIZE_IMPL(x)

namespace cooling_failover {
namespace {

constexpr const char* kCompilerName =
#if defined(_MSC_VER)
    "msvc";
#elif defined(__clang__)
    "clang";
#elif defined(__GNUC__)
    "gcc";
#else
    "unknown";
#endif

constexpr const char* kCompilerVersion =
#if defined(_MSC_VER)
    CF_STRINGIZE(_MSC_FULL_VER);
#elif defined(__clang__)
    __clang_version__;
#elif defined(__GNUC__)
    __VERSION__;
#else
    "0";
#endif

constexpr const char* kConfiguration =
#if defined(NDEBUG)
    "release";
#else
    "debug";
#endif

}  // namespace

std::string_view version_string() noexcept { return "1.0.0"; }

std::string_view build_identity() noexcept {
  static const std::string identity = [] {
    std::string text = "1.0.0+";
    text += kCompilerName;
    text += "-";
    text += kCompilerVersion;
    text += "/";
    text += kConfiguration;
    return text;
  }();
  return identity;
}

}  // namespace cooling_failover
