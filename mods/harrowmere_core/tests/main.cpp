// The doctest entry point of harrowmere_core_tests (the framework task 0.6
// pins; task 0.7 lands the first pinned test files). Splitting main from the
// test translation units keeps every test file free of the implement define:
// a new test file is just one include of the single header plus its TEST_CASEs.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
