// tests/main.cpp - the UI mod's test harness entry (task 7.4P1).
//
// The SINGLE doctest implement define for the harrowmere_ui_tests binary
// (mods/harrowmere_ui/tests/CMakeLists.txt carries the harness rules and the
// one-main rationale). Test files keep this define out - the core harness's
// rule, same reason: one binary, one main.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
