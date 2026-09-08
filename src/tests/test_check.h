// Shared check/report helpers for the host test executables. No framework:
// this repo has no package manager, and the tests need only assertions.
#ifndef H4CN_TESTS_TEST_CHECK_H_
#define H4CN_TESTS_TEST_CHECK_H_

#include <cstdio>

namespace testcheck {

inline int& Checks() {
  static int n = 0;
  return n;
}

inline int& Failures() {
  static int n = 0;
  return n;
}

inline void Check(bool ok, const char* what, int line) {
  ++Checks();
  if (!ok) {
    ++Failures();
    std::printf("FAIL(line %d): %s\n", line, what);
  }
}

inline int Finish(const char* name) {
  std::printf("%s: %d checks, %d failures\n", name, Checks(), Failures());
  return Failures() == 0 ? 0 : 1;
}

}  // namespace testcheck

#define CHECK(expr) ::testcheck::Check((expr), #expr, __LINE__)

#endif  // H4CN_TESTS_TEST_CHECK_H_
