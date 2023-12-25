#ifndef WS_TEST_HARNESS_H
#define WS_TEST_HARNESS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct test_table {
  const char *name;
  int (*test)(const char *);
};

// run all tests and exit with EXIT_SUCCESS if all tests pass
static void RUN_TESTS(const char *label, struct test_table *tests, size_t n) {
#define PASS "\033[32m[PASS]\033[0m"
#define FAIL "\033[31m[FAIL]\033[0m"
#define INFO "\033[34m[INFO]\033[0m"

  int *stats = (int *)malloc(sizeof(int) * n);
  if (!stats) {
    perror("malloc");
    exit(EXIT_FAILURE);
  }

  memset(stats, EXIT_FAILURE, sizeof(int) * n);

  printf( INFO " Running %s Test Suite...\n", label);

  for (size_t i = 0; i < n; i++) {
    int ret = tests[i].test(tests[i].name);
    if (ret == EXIT_SUCCESS) {
      stats[i] = EXIT_SUCCESS;
    } else {
      stats[i] = EXIT_FAILURE;
    }
  }

  int suite_status = EXIT_SUCCESS;
  for (size_t i = 0; i < n; i++) {
    printf("%s %s\n", stats[i] == EXIT_SUCCESS ? PASS : FAIL,
           tests[i].name);

    if (stats[i] != EXIT_SUCCESS) {
      suite_status = EXIT_FAILURE;
    }
  }

  free(stats);

  printf(INFO " %s Test Suite Complete.\n", label);
  printf("-----------------------------------------\n");

  exit(suite_status);
}

#endif // WS_TEST_HARNESS_H
