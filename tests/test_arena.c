/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * test_arena.c - per frame bump allocator.
 *
 * Two properties carry the weight here: allocations must not overlap, and a
 * reset must reuse blocks rather than returning them to the system. The
 * second is what keeps the frame loop free of malloc once it has settled.
 */

#include <stdint.h>
#include <string.h>

#include "greatest.h"
#include "schultz_arena.h"

TEST init_starts_empty(void)
{
    schultz_arena arena;

    ASSERT_EQ(SCHULTZ_OK, schultz_arena_init(&arena, 1024));
    ASSERT_EQ(0u, (unsigned)schultz_arena_used(&arena));
    ASSERT_EQ(0u, schultz_arena_block_count(&arena));

    schultz_arena_free(&arena);
    PASS();
}

TEST init_rejects_null_arena(void)
{
    ASSERT_EQ(SCHULTZ_ERR_INVALID_ARGUMENT, schultz_arena_init(NULL, 1024));
    PASS();
}

TEST alloc_returns_usable_memory(void)
{
    schultz_arena arena;
    unsigned char *memory;

    ASSERT_EQ(SCHULTZ_OK, schultz_arena_init(&arena, 1024));

    memory = (unsigned char *)schultz_arena_alloc(&arena, 64, 0);
    ASSERT(memory != NULL);

    memset(memory, 0xAB, 64);
    ASSERT_EQ(0xAB, memory[0]);
    ASSERT_EQ(0xAB, memory[63]);
    ASSERT(schultz_arena_used(&arena) >= 64u);

    schultz_arena_free(&arena);
    PASS();
}

TEST alloc_rejects_zero_size(void)
{
    schultz_arena arena;

    ASSERT_EQ(SCHULTZ_OK, schultz_arena_init(&arena, 1024));
    ASSERT_EQ(NULL, schultz_arena_alloc(&arena, 0, 0));
    ASSERT_EQ(NULL, schultz_arena_alloc(NULL, 16, 0));

    schultz_arena_free(&arena);
    PASS();
}

TEST alloc_rejects_non_power_of_two_alignment(void)
{
    schultz_arena arena;

    ASSERT_EQ(SCHULTZ_OK, schultz_arena_init(&arena, 1024));
    ASSERT_EQ(NULL, schultz_arena_alloc(&arena, 16, 3));
    ASSERT_EQ(NULL, schultz_arena_alloc(&arena, 16, 24));

    schultz_arena_free(&arena);
    PASS();
}

TEST alloc_honors_requested_alignment(void)
{
    schultz_arena arena;
    size_t alignments[] = { 1, 2, 4, 8, 16, 32, 64, 128 };
    size_t i;

    ASSERT_EQ(SCHULTZ_OK, schultz_arena_init(&arena, 4096));

    for (i = 0; i < sizeof(alignments) / sizeof(alignments[0]); i++) {
        /* An odd size between requests keeps the cursor misaligned. */
        void *filler = schultz_arena_alloc(&arena, 3, 1);
        void *memory = schultz_arena_alloc(&arena, 16, alignments[i]);

        ASSERT(filler != NULL);
        ASSERT(memory != NULL);
        /* Plain ASSERT_EQ: uintptr_t has no portable printf specifier. */
        ASSERT_EQ((uintptr_t)0,
                  (uintptr_t)memory & (uintptr_t)(alignments[i] - 1));
    }

    schultz_arena_free(&arena);
    PASS();
}

TEST allocations_do_not_overlap(void)
{
    schultz_arena arena;
    enum { COUNT = 200 };
    unsigned char *blocks[COUNT];
    int i;
    int j;

    ASSERT_EQ(SCHULTZ_OK, schultz_arena_init(&arena, 512));

    /* Write a distinct pattern into each allocation. */
    for (i = 0; i < COUNT; i++) {
        blocks[i] = (unsigned char *)schultz_arena_alloc(&arena, 32, 0);
        ASSERT(blocks[i] != NULL);
        memset(blocks[i], (unsigned char)(i & 0xFF), 32);
    }

    /* If any two allocations overlapped, an earlier pattern is now gone. */
    for (i = 0; i < COUNT; i++) {
        for (j = 0; j < 32; j++) {
            ASSERT_EQ((unsigned char)(i & 0xFF), blocks[i][j]);
        }
    }

    schultz_arena_free(&arena);
    PASS();
}

TEST alloc_larger_than_block_size_succeeds(void)
{
    schultz_arena arena;
    unsigned char *memory;

    ASSERT_EQ(SCHULTZ_OK, schultz_arena_init(&arena, 64));

    memory = (unsigned char *)schultz_arena_alloc(&arena, 8192, 0);
    ASSERT(memory != NULL);
    memset(memory, 0x5A, 8192);
    ASSERT_EQ(0x5A, memory[8191]);

    schultz_arena_free(&arena);
    PASS();
}

TEST reset_clears_used_but_keeps_blocks(void)
{
    schultz_arena arena;
    uint32_t blocks_before;

    ASSERT_EQ(SCHULTZ_OK, schultz_arena_init(&arena, 256));

    /* Force several blocks. */
    ASSERT(schultz_arena_alloc(&arena, 200, 0) != NULL);
    ASSERT(schultz_arena_alloc(&arena, 200, 0) != NULL);
    ASSERT(schultz_arena_alloc(&arena, 200, 0) != NULL);
    ASSERT(schultz_arena_used(&arena) >= 600u);

    blocks_before = schultz_arena_block_count(&arena);
    ASSERT(blocks_before > 1u);

    schultz_arena_reset(&arena);

    ASSERT_EQ(0u, (unsigned)schultz_arena_used(&arena));
    ASSERT_EQ(blocks_before, schultz_arena_block_count(&arena));
    ASSERT(schultz_arena_capacity(&arena) > 0u);

    schultz_arena_free(&arena);
    PASS();
}

TEST reset_hands_back_the_same_memory(void)
{
    schultz_arena arena;
    void *first;
    void *again;

    ASSERT_EQ(SCHULTZ_OK, schultz_arena_init(&arena, 1024));

    first = schultz_arena_alloc(&arena, 64, 16);
    ASSERT(first != NULL);

    schultz_arena_reset(&arena);

    again = schultz_arena_alloc(&arena, 64, 16);
    ASSERT_EQ(first, again);

    schultz_arena_free(&arena);
    PASS();
}

/*
 * The property the frame loop depends on: after the first few frames the
 * arena stops asking the system for memory.
 */
TEST repeated_frames_stop_allocating_blocks(void)
{
    schultz_arena arena;
    uint32_t settled_blocks;
    int frame;
    int i;

    ASSERT_EQ(SCHULTZ_OK, schultz_arena_init(&arena, 1024));

    for (frame = 0; frame < 3; frame++) {
        for (i = 0; i < 100; i++) {
            ASSERT(schultz_arena_alloc(&arena, 48, 0) != NULL);
        }
        schultz_arena_reset(&arena);
    }

    settled_blocks = schultz_arena_block_count(&arena);

    for (frame = 0; frame < 20; frame++) {
        for (i = 0; i < 100; i++) {
            ASSERT(schultz_arena_alloc(&arena, 48, 0) != NULL);
        }
        ASSERT_EQ(settled_blocks, schultz_arena_block_count(&arena));
        schultz_arena_reset(&arena);
    }

    schultz_arena_free(&arena);
    PASS();
}

TEST reused_blocks_still_do_not_overlap(void)
{
    schultz_arena arena;
    enum { COUNT = 40 };
    unsigned char *blocks[COUNT];
    int frame;
    int i;
    int j;

    ASSERT_EQ(SCHULTZ_OK, schultz_arena_init(&arena, 256));

    for (frame = 0; frame < 5; frame++) {
        for (i = 0; i < COUNT; i++) {
            blocks[i] = (unsigned char *)schultz_arena_alloc(&arena, 24, 0);
            ASSERT(blocks[i] != NULL);
            memset(blocks[i], (unsigned char)(i & 0xFF), 24);
        }
        for (i = 0; i < COUNT; i++) {
            for (j = 0; j < 24; j++) {
                ASSERT_EQ((unsigned char)(i & 0xFF), blocks[i][j]);
            }
        }
        schultz_arena_reset(&arena);
    }

    schultz_arena_free(&arena);
    PASS();
}

TEST free_is_safe_twice_and_on_a_zeroed_arena(void)
{
    schultz_arena arena;
    schultz_arena zeroed;

    memset(&zeroed, 0, sizeof(zeroed));
    schultz_arena_free(&zeroed);
    schultz_arena_free(NULL);

    ASSERT_EQ(SCHULTZ_OK, schultz_arena_init(&arena, 512));
    ASSERT(schultz_arena_alloc(&arena, 64, 0) != NULL);
    schultz_arena_free(&arena);
    schultz_arena_free(&arena);

    ASSERT_EQ(0u, schultz_arena_block_count(&arena));
    PASS();
}

TEST accessors_tolerate_null(void)
{
    ASSERT_EQ(0u, (unsigned)schultz_arena_used(NULL));
    ASSERT_EQ(0u, (unsigned)schultz_arena_capacity(NULL));
    ASSERT_EQ(0u, schultz_arena_block_count(NULL));
    schultz_arena_reset(NULL);
    PASS();
}

SUITE(arena)
{
    RUN_TEST(init_starts_empty);
    RUN_TEST(init_rejects_null_arena);
    RUN_TEST(alloc_returns_usable_memory);
    RUN_TEST(alloc_rejects_zero_size);
    RUN_TEST(alloc_rejects_non_power_of_two_alignment);
    RUN_TEST(alloc_honors_requested_alignment);
    RUN_TEST(allocations_do_not_overlap);
    RUN_TEST(alloc_larger_than_block_size_succeeds);
    RUN_TEST(reset_clears_used_but_keeps_blocks);
    RUN_TEST(reset_hands_back_the_same_memory);
    RUN_TEST(repeated_frames_stop_allocating_blocks);
    RUN_TEST(reused_blocks_still_do_not_overlap);
    RUN_TEST(free_is_safe_twice_and_on_a_zeroed_arena);
    RUN_TEST(accessors_tolerate_null);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(arena);
    GREATEST_MAIN_END();
}
