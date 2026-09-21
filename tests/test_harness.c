/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * test_harness.c - proves the test wiring works.
 *
 * There is no library code to exercise yet, so this file exists to verify
 * that greatest compiles, links, runs, and reports failures correctly. It
 * also serves as the template for real test files: define tests with TEST,
 * group them with SUITE, and list them in main.
 */

#include "greatest.h"

TEST harness_reports_pass(void)
{
    ASSERT_EQ(4, 2 + 2);
    PASS();
}

TEST harness_compares_strings(void)
{
    ASSERT_STR_EQ("schultz", "schultz");
    PASS();
}

SUITE(harness)
{
    RUN_TEST(harness_reports_pass);
    RUN_TEST(harness_compares_strings);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(harness);
    GREATEST_MAIN_END();
}
