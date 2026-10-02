#pragma once

// A deliberately tiny test harness: no dependencies, works with
// -fno-exceptions, and one executable per test file.

#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace testing {

struct TestCase {
  const char *name;
  void (*fn)();
};

inline std::vector<TestCase> &registry() {
  static std::vector<TestCase> cases;
  return cases;
}

inline int &failure_count() {
  static int failures = 0;
  return failures;
}

struct Registrar {
  Registrar(const char *name, void (*fn)()) { registry().push_back({name, fn}); }
};

template <typename T> std::string describe(const T &value) {
  if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(value));
  } else if constexpr (std::is_same_v<T, bool>) {
    return value ? "true" : "false";
  } else if constexpr (std::is_arithmetic_v<T>) {
    return std::to_string(value);
  } else {
    return "<value>";
  }
}

template <typename A, typename B>
bool check_eq(const A &lhs, const B &rhs, const char *lhs_text,
              const char *rhs_text, const char *file, int line) {
  if (lhs == rhs) {
    return true;
  }
  ++failure_count();
  std::printf("  FAIL %s:%d: %s == %s  (%s vs %s)\n", file, line, lhs_text,
              rhs_text, describe(lhs).c_str(), describe(rhs).c_str());
  return false;
}

inline bool check(bool condition, const char *text, const char *file,
                  int line) {
  if (condition) {
    return true;
  }
  ++failure_count();
  std::printf("  FAIL %s:%d: %s\n", file, line, text);
  return false;
}

inline int run_all(int argc, char **argv) {
  const char *filter = argc > 1 ? argv[1] : nullptr;
  int ran = 0;
  int failed_cases = 0;
  for (const TestCase &test : registry()) {
    if (filter != nullptr && std::strstr(test.name, filter) == nullptr) {
      continue;
    }
    const int before = failure_count();
    test.fn();
    ++ran;
    const bool ok = failure_count() == before;
    if (!ok) {
      ++failed_cases;
    }
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", test.name);
  }
  std::printf("%d test(s), %d failed\n", ran, failed_cases);
  return failed_cases == 0 ? 0 : 1;
}

} // namespace testing

#define TEST(name)                                                             \
  static void test_##name();                                                   \
  static ::testing::Registrar registrar_##name(#name, &test_##name);           \
  static void test_##name()

#define CHECK(condition)                                                       \
  ::testing::check(static_cast<bool>(condition), #condition, __FILE__, __LINE__)

#define CHECK_EQ(lhs, rhs)                                                     \
  ::testing::check_eq((lhs), (rhs), #lhs, #rhs, __FILE__, __LINE__)

// Stops the current test if the condition does not hold.
#define REQUIRE(condition)                                                     \
  do {                                                                         \
    if (!CHECK(condition)) {                                                   \
      return;                                                                  \
    }                                                                          \
  } while (false)

#define REQUIRE_EQ(lhs, rhs)                                                   \
  do {                                                                         \
    if (!CHECK_EQ(lhs, rhs)) {                                                 \
      return;                                                                  \
    }                                                                          \
  } while (false)

#define TEST_MAIN()                                                            \
  int main(int argc, char **argv) { return ::testing::run_all(argc, argv); }
