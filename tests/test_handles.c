/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * test_handles.c - handle table.
 *
 * The behavior that matters here is stale handle rejection. Every other layer
 * trusts the table to turn a use after free into a returned error, so these
 * tests lean hard on the generation counter.
 */

#include <string.h>

#include "greatest.h"
#include "schultz_handle.h"

/* Distinct addresses to hand the table. Contents are never read. */
static int object_a;
static int object_b;
static int object_c;

TEST init_gives_empty_table_with_capacity(void)
{
    schultz_handle_table table;

    ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_init(&table, 8));
    ASSERT_EQ(0u, schultz_handle_table_count(&table));
    ASSERT_EQ(8u, schultz_handle_table_capacity(&table));

    schultz_handle_table_free(&table);
    PASS();
}

TEST init_zero_capacity_uses_default(void)
{
    schultz_handle_table table;

    ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_init(&table, 0));
    ASSERT(schultz_handle_table_capacity(&table) > 0u);

    schultz_handle_table_free(&table);
    PASS();
}

TEST init_rejects_null_table(void)
{
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT, schultz_handle_table_init(NULL, 8));
    PASS();
}

TEST insert_then_lookup_returns_the_object(void)
{
    schultz_handle_table table;
    schultz_handle handle = SCHULTZ_HANDLE_NONE;
    void *found = NULL;

    ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_init(&table, 8));
    ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_insert(&table, &object_a, &handle));
    ASSERT(handle != SCHULTZ_HANDLE_NONE);
    ASSERT_EQ(1u, schultz_handle_table_count(&table));

    ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_lookup(&table, handle, &found));
    ASSERT_EQ(&object_a, found);

    schultz_handle_table_free(&table);
    PASS();
}

TEST insert_gives_distinct_handles(void)
{
    schultz_handle_table table;
    schultz_handle a = SCHULTZ_HANDLE_NONE;
    schultz_handle b = SCHULTZ_HANDLE_NONE;
    schultz_handle c = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_init(&table, 8));
    ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_insert(&table, &object_a, &a));
    ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_insert(&table, &object_b, &b));
    ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_insert(&table, &object_c, &c));

    ASSERT(a != b);
    ASSERT(b != c);
    ASSERT(a != c);
    ASSERT_EQ(3u, schultz_handle_table_count(&table));

    schultz_handle_table_free(&table);
    PASS();
}

TEST insert_rejects_null_object(void)
{
    schultz_handle_table table;
    schultz_handle handle = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_init(&table, 8));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_handle_table_insert(&table, NULL, &handle));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT,
              schultz_handle_table_insert(&table, &object_a, NULL));

    schultz_handle_table_free(&table);
    PASS();
}

TEST lookup_rejects_the_none_handle(void)
{
    schultz_handle_table table;
    void *found = NULL;

    ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_init(&table, 8));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_handle_table_lookup(&table, SCHULTZ_HANDLE_NONE, &found));

    schultz_handle_table_free(&table);
    PASS();
}

TEST lookup_rejects_out_of_range_index(void)
{
    schultz_handle_table table;
    void *found = NULL;
    schultz_handle bogus = schultz_handle_make(9999u, 1u);

    ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_init(&table, 8));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_handle_table_lookup(&table, bogus, &found));

    schultz_handle_table_free(&table);
    PASS();
}

TEST lookup_rejects_a_free_slot(void)
{
    schultz_handle_table table;
    void *found = NULL;
    schultz_handle never_used = schultz_handle_make(3u, 1u);

    ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_init(&table, 8));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_handle_table_lookup(&table, never_used, &found));

    schultz_handle_table_free(&table);
    PASS();
}

TEST remove_makes_the_handle_stale(void)
{
    schultz_handle_table table;
    schultz_handle handle = SCHULTZ_HANDLE_NONE;
    void *found = NULL;

    ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_init(&table, 8));
    ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_insert(&table, &object_a, &handle));
    ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_remove(&table, handle));
    ASSERT_EQ(0u, schultz_handle_table_count(&table));

    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_handle_table_lookup(&table, handle, &found));

    schultz_handle_table_free(&table);
    PASS();
}

TEST remove_twice_is_rejected(void)
{
    schultz_handle_table table;
    schultz_handle handle = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_init(&table, 8));
    ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_insert(&table, &object_a, &handle));
    ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_remove(&table, handle));
    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE, schultz_handle_table_remove(&table, handle));
    ASSERT_EQ(0u, schultz_handle_table_count(&table));

    schultz_handle_table_free(&table);
    PASS();
}

/*
 * The case the generation counter exists for: a released slot is handed to a
 * new object, and the old handle must not resolve to it.
 */
TEST reused_slot_does_not_resolve_the_old_handle(void)
{
    schultz_handle_table table;
    schultz_handle old_handle = SCHULTZ_HANDLE_NONE;
    schultz_handle new_handle = SCHULTZ_HANDLE_NONE;
    void *found = NULL;

    ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_init(&table, 8));
    ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_insert(&table, &object_a, &old_handle));
    ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_remove(&table, old_handle));
    ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_insert(&table, &object_b, &new_handle));

    /* Same slot, different handle. */
    ASSERT_EQ(schultz_handle_index(old_handle), schultz_handle_index(new_handle));
    ASSERT(old_handle != new_handle);
    ASSERT(schultz_handle_generation(new_handle) >
           schultz_handle_generation(old_handle));

    ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
              schultz_handle_table_lookup(&table, old_handle, &found));

    ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_lookup(&table, new_handle, &found));
    ASSERT_EQ(&object_b, found);

    schultz_handle_table_free(&table);
    PASS();
}

TEST table_grows_past_initial_capacity(void)
{
    schultz_handle_table table;
    schultz_handle handles[100];
    void *found = NULL;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_init(&table, 4));

    for (i = 0; i < 100u; i++) {
        ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_insert(&table, &object_a,
                                                &handles[i]));
    }

    ASSERT_EQ(100u, schultz_handle_table_count(&table));
    ASSERT(schultz_handle_table_capacity(&table) >= 100u);

    /* Every handle issued before the growth must still resolve. */
    for (i = 0; i < 100u; i++) {
        ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_lookup(&table, handles[i], &found));
        ASSERT_EQ(&object_a, found);
    }

    schultz_handle_table_free(&table);
    PASS();
}

TEST handles_stay_distinct_across_growth(void)
{
    schultz_handle_table table;
    schultz_handle handles[64];
    uint32_t i;
    uint32_t j;

    ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_init(&table, 2));
    for (i = 0; i < 64u; i++) {
        ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_insert(&table, &object_a,
                                                &handles[i]));
    }
    for (i = 0; i < 64u; i++) {
        for (j = i + 1u; j < 64u; j++) {
            ASSERT(handles[i] != handles[j]);
        }
    }

    schultz_handle_table_free(&table);
    PASS();
}

TEST churn_keeps_the_table_consistent(void)
{
    schultz_handle_table table;
    schultz_handle handles[32];
    void *found = NULL;
    uint32_t round;
    uint32_t i;

    ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_init(&table, 8));

    for (round = 0; round < 50u; round++) {
        for (i = 0; i < 32u; i++) {
            ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_insert(&table, &object_a,
                                                    &handles[i]));
        }
        ASSERT_EQ(32u, schultz_handle_table_count(&table));

        for (i = 0; i < 32u; i++) {
            ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_lookup(&table, handles[i],
                                                    &found));
            ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_remove(&table, handles[i]));
            ASSERT_EQ(SCHULTZ_ERR_INVALID_HANDLE,
                      schultz_handle_table_lookup(&table, handles[i], &found));
        }
        ASSERT_EQ(0u, schultz_handle_table_count(&table));
    }

    schultz_handle_table_free(&table);
    PASS();
}

/*
 * A slot's generation must never wrap to zero, because a zero generation
 * would make a live handle equal to SCHULTZ_HANDLE_NONE.
 */
TEST generation_skips_zero_on_wrap(void)
{
    schultz_handle_table table;
    schultz_handle handle = SCHULTZ_HANDLE_NONE;

    ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_init(&table, 4));
    ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_insert(&table, &object_a, &handle));

    /* Force the slot to the last generation before wrap. */
    table.slots[schultz_handle_index(handle)].generation = UINT32_MAX;
    handle = schultz_handle_make(schultz_handle_index(handle), UINT32_MAX);

    ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_remove(&table, handle));
    ASSERT_EQ(1u, table.slots[schultz_handle_index(handle)].generation);

    ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_insert(&table, &object_b, &handle));
    ASSERT(handle != SCHULTZ_HANDLE_NONE);

    schultz_handle_table_free(&table);
    PASS();
}

TEST pack_and_unpack_round_trip(void)
{
    schultz_handle handle = schultz_handle_make(12345u, 678u);

    ASSERT_EQ(12345u, schultz_handle_index(handle));
    ASSERT_EQ(678u, schultz_handle_generation(handle));

    handle = schultz_handle_make(UINT32_MAX, UINT32_MAX);
    ASSERT_EQ(UINT32_MAX, schultz_handle_index(handle));
    ASSERT_EQ(UINT32_MAX, schultz_handle_generation(handle));
    PASS();
}

TEST free_is_safe_twice_and_on_a_zeroed_table(void)
{
    schultz_handle_table table;
    schultz_handle_table zeroed;

    memset(&zeroed, 0, sizeof(zeroed));
    schultz_handle_table_free(&zeroed);
    schultz_handle_table_free(NULL);

    ASSERT_EQ(SCHULTZ_OK, schultz_handle_table_init(&table, 8));
    schultz_handle_table_free(&table);
    schultz_handle_table_free(&table);
    ASSERT_EQ(0u, schultz_handle_table_capacity(&table));
    PASS();
}

SUITE(handle_table)
{
    RUN_TEST(init_gives_empty_table_with_capacity);
    RUN_TEST(init_zero_capacity_uses_default);
    RUN_TEST(init_rejects_null_table);
    RUN_TEST(insert_then_lookup_returns_the_object);
    RUN_TEST(insert_gives_distinct_handles);
    RUN_TEST(insert_rejects_null_object);
    RUN_TEST(lookup_rejects_the_none_handle);
    RUN_TEST(lookup_rejects_out_of_range_index);
    RUN_TEST(lookup_rejects_a_free_slot);
    RUN_TEST(remove_makes_the_handle_stale);
    RUN_TEST(remove_twice_is_rejected);
    RUN_TEST(reused_slot_does_not_resolve_the_old_handle);
    RUN_TEST(table_grows_past_initial_capacity);
    RUN_TEST(handles_stay_distinct_across_growth);
    RUN_TEST(churn_keeps_the_table_consistent);
    RUN_TEST(generation_skips_zero_on_wrap);
    RUN_TEST(pack_and_unpack_round_trip);
    RUN_TEST(free_is_safe_twice_and_on_a_zeroed_table);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(handle_table);
    GREATEST_MAIN_END();
}
