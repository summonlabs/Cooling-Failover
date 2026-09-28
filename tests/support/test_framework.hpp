// Minimal deterministic test framework. No external dependencies, no timeouts.
#pragma once

#include "cooling_failover/canonical.hpp"
#include "cooling_failover/status.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace cf_test {

class TestFailure : public std::exception {
 public:
  explicit TestFailure(std::string message) : message_(std::move(message)) {}
  [[nodiscard]] const char* what() const noexcept override { return message_.c_str(); }

 private:
  std::string message_;
};

template <class T, class = void>
struct is_streamable : std::false_type {};

template <class T>
struct is_streamable<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

template <class T>
[[nodiscard]] std::string describe(const T& value) {
  if constexpr (std::is_same_v<T, cooling_failover::Status>) {
    return value.canonical_line();
  } else if constexpr (std::is_same_v<T, cooling_failover::Digest>) {
    return value.to_hex();
  } else if constexpr (std::is_same_v<T, bool>) {
    return value ? "true" : "false";
  } else if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(value));
  } else if constexpr (is_streamable<T>::value) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
  } else {
    return "<value>";
  }
}

struct TestCase {
  std::string suite;
  std::string name;
  void (*body)();
};

class Registry {
 public:
  static Registry& instance() {
    static Registry registry;
    return registry;
  }

  void add(const char* suite, const char* name, void (*body)()) {
    cases_.push_back(TestCase{suite, name, body});
  }

  [[nodiscard]] const std::vector<TestCase>& cases() const noexcept { return cases_; }

  int run(int argc, char** argv) {
    std::string filter;
    for (int index = 1; index < argc; ++index) {
      const std::string argument = argv[index];
      if (argument.rfind("--filter=", 0) == 0) {
        filter = argument.substr(9);
      }
    }
    std::size_t passed = 0;
    std::size_t failed = 0;
    std::size_t skipped = 0;
    for (const TestCase& test : cases_) {
      const std::string full = test.suite + "." + test.name;
      if (!filter.empty() && full.find(filter) == std::string::npos) {
        ++skipped;
        continue;
      }
      try {
        test.body();
        ++passed;
      } catch (const TestFailure& failure) {
        ++failed;
        std::cout << "FAIL " << full << "\n  " << failure.what() << "\n";
      } catch (const std::exception& error) {
        ++failed;
        std::cout << "FAIL " << full << "\n  unexpected exception: " << error.what() << "\n";
      } catch (...) {
        ++failed;
        std::cout << "FAIL " << full << "\n  unexpected non-standard exception\n";
      }
    }
    std::cout << (failed == 0 ? "PASS" : "FAIL") << " " << passed << " passed, " << failed
              << " failed, " << skipped << " filtered out\n";
    return failed == 0 ? 0 : 1;
  }

 private:
  std::vector<TestCase> cases_{};
};

struct Registrar {
  Registrar(const char* suite, const char* name, void (*body)()) {
    Registry::instance().add(suite, name, body);
  }
};

}  // namespace cf_test

#define CF_TEST(suite, name)                                                            \
  static void cf_test_body_##suite##_##name();                                          \
  static const ::cf_test::Registrar cf_test_reg_##suite##_##name(#suite, #name,          \
                                                                 &cf_test_body_##suite##_##name); \
  static void cf_test_body_##suite##_##name()

#define CF_FAIL(message)                                                              \
  throw ::cf_test::TestFailure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
                               ": " + (message))

#define CF_FAIL_STREAM(expr)        \
  do {                              \
    std::ostringstream cf_stream;   \
    cf_stream << expr;              \
    CF_FAIL(cf_stream.str());       \
  } while (false)

#define CF_CHECK(condition)                                                    \
  do {                                                                         \
    if (!(condition)) {                                                        \
      CF_FAIL(std::string("check failed: ") + #condition);                     \
    }                                                                          \
  } while (false)

#define CF_CHECK_MSG(condition, message)                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      CF_FAIL(std::string("check failed: ") + #condition + " (" + (message) + ")"); \
    }                                                                          \
  } while (false)

#define CF_CHECK_EQ(lhs, rhs)                                                            \
  do {                                                                                   \
    const auto cf_lhs = (lhs);                                                           \
    const auto cf_rhs = (rhs);                                                           \
    if (!(cf_lhs == cf_rhs)) {                                                           \
      CF_FAIL(std::string("expected ") + #lhs + " == " + #rhs + "\n    lhs = " +          \
              ::cf_test::describe(cf_lhs) + "\n    rhs = " + ::cf_test::describe(cf_rhs)); \
    }                                                                                    \
  } while (false)

#define CF_CHECK_NE(lhs, rhs)                                                   \
  do {                                                                          \
    if ((lhs) == (rhs)) {                                                       \
      CF_FAIL(std::string("expected ") + #lhs + " != " + #rhs);                 \
    }                                                                           \
  } while (false)

/// Asserts that an operation failed with exactly the expected primary code.
#define CF_CHECK_ERROR(expression, expected_code)                                        \
  do {                                                                                   \
    auto cf_result = (expression);                                                       \
    if (cf_result.has_value()) {                                                         \
      CF_FAIL(std::string("expected failure ") + #expected_code + " from " + #expression); \
    }                                                                                    \
    if (cf_result.status().code() != (expected_code)) {                                  \
      CF_FAIL(std::string("expected ") + #expected_code + " from " + #expression +        \
              " but got " + cf_result.status().canonical_line());                        \
    }                                                                                    \
  } while (false)

/// Binds a successful Result to a local, failing the test otherwise.
#define CF_ASSIGN_OR_FAIL(var, expr)                                                        \
  auto cf_tmp_##var = (expr);                                                               \
  if (!cf_tmp_##var.has_value()) {                                                          \
    CF_FAIL(std::string("unexpected failure from ") + #expr + ": " +                        \
            cf_tmp_##var.status().canonical_line());                                        \
  }                                                                                         \
  auto&& var = *cf_tmp_##var

#define CF_CHECK_OK(expression)                                                          \
  do {                                                                                   \
    auto cf_result = (expression);                                                       \
    if (!cf_result.has_value()) {                                                        \
      CF_FAIL(std::string("expected success from ") + #expression + " but got " +        \
              cf_result.status().canonical_line());                                      \
    }                                                                                    \
  } while (false)
