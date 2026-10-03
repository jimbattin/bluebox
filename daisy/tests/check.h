#ifndef TESTS_CHECK_H
#define TESTS_CHECK_H
#include <stdio.h>
static int gChecks = 0, gFailures = 0;
#define CHECK(cond) do { gChecks++; if (!(cond)) { gFailures++; \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } } while (0)
// Prints the summary; returns the process exit code.
static int CheckSummary(const char* suite) {
    printf("%s: %d checks, %d failed\n", suite, gChecks, gFailures);
    return gFailures ? 1 : 0;
}
#endif
