#pragma once
// Minimal stand-in for the Unity test framework, so the plugin's host tests run with plain g++ (scripts/run_host_tests.sh).
// Inside GaggiMate's `pio test -e native` the real Unity is used and this file is not on the include path.
// A failed assertion jumps out of the test; the run goes on with the next one.
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

namespace unity_shim {
inline jmp_buf& env() { static jmp_buf e; return e; }
inline int& failures() { static int n = 0; return n; }
inline int& tests() { static int n = 0; return n; }
inline const char*& current() { static const char* name = ""; return name; }
[[noreturn]] inline void fail(const char* file, int line, const char* msg) {
    printf("  FAIL %s:%d: %s\n", file, line, msg);
    longjmp(env(), 1);
}
}  // namespace unity_shim

void setUp();
void tearDown();

#define TEST_FAIL_MESSAGE(m) unity_shim::fail(__FILE__, __LINE__, (m))
#define TEST_FAIL() TEST_FAIL_MESSAGE("TEST_FAIL")
#define TEST_ASSERT_TRUE_MESSAGE(c, m) do { if (!(c)) TEST_FAIL_MESSAGE(m); } while (0)
#define TEST_ASSERT_TRUE(c) TEST_ASSERT_TRUE_MESSAGE((c), "expected true: " #c)
#define TEST_ASSERT_FALSE_MESSAGE(c, m) TEST_ASSERT_TRUE_MESSAGE(!(c), (m))
#define TEST_ASSERT_FALSE(c) TEST_ASSERT_TRUE_MESSAGE(!(c), "expected false: " #c)
#define TEST_ASSERT_EQUAL_INT_MESSAGE(e, a, m) do { long long e_ = (e), a_ = (a); \
    if (e_ != a_) { char b_[300]; snprintf(b_, sizeof(b_), "%s (expected %lld, actual %lld)", (m), e_, a_); TEST_FAIL_MESSAGE(b_); } } while (0)
#define TEST_ASSERT_EQUAL_INT(e, a) TEST_ASSERT_EQUAL_INT_MESSAGE((e), (a), #a)
#define TEST_ASSERT_EQUAL_INT32(e, a) TEST_ASSERT_EQUAL_INT((e), (a))
#define TEST_ASSERT_EQUAL_UINT32(e, a) TEST_ASSERT_EQUAL_INT((e), (a))
#define TEST_ASSERT_EQUAL_STRING_MESSAGE(e, a, m) do { const char* e_ = (e); const char* a_ = (a); \
    if (strcmp(e_, a_) != 0) { char b_[600]; snprintf(b_, sizeof(b_), "%s (expected \"%s\", actual \"%s\")", (m), e_, a_); TEST_FAIL_MESSAGE(b_); } } while (0)
#define TEST_ASSERT_EQUAL_STRING(e, a) TEST_ASSERT_EQUAL_STRING_MESSAGE((e), (a), #a)

#define UNITY_BEGIN() (unity_shim::failures() = 0, unity_shim::tests() = 0, 0)
#define RUN_TEST(fn) do { unity_shim::tests()++; unity_shim::current() = #fn; setUp(); \
    if (setjmp(unity_shim::env()) == 0) { fn(); } else { unity_shim::failures()++; printf("%s: FAILED\n", #fn); } tearDown(); } while (0)
#define UNITY_END() (printf("%d tests, %d failed\n", unity_shim::tests(), unity_shim::failures()), unity_shim::failures() ? 1 : 0)
