/*
  test_isocline_optimizations.c

  This file is part of cjsh, CJ's Shell

  MIT License

  Copyright (c) 2026 Caden Finley

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common.h"
#include "completions.h"
#include "history.h"
#include "isocline.h"
#include "stringbuf.h"
#include "undo.h"
#include "unicode.h"

#define CHECK(condition)                                                          \
    do {                                                                          \
        if (!(condition)) {                                                       \
            (void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            exit(EXIT_FAILURE);                                                   \
        }                                                                         \
    } while (0)

// Track requested memory rather than wall-clock thresholds, so regression checks
// remain deterministic under ASan and on slow/shared CI runners.
typedef union allocation_header_u {
    size_t size;
    max_align_t alignment;
} allocation_header_t;

static struct {
    size_t attempts;
    size_t reallocations;
    size_t requested;
    size_t live_bytes;
    size_t peak_bytes;
    size_t live_blocks;
    size_t fail_at;
    size_t fail_size;
} allocation;

static bool allocation_fails(size_t size) {
    allocation.attempts++;
    return allocation.attempts == allocation.fail_at ||
           (allocation.fail_size != 0 && size == allocation.fail_size);
}

static void record_allocation(size_t old_size, size_t size) {
    allocation.requested += size;
    allocation.live_bytes = allocation.live_bytes - old_size + size;
    if (allocation.live_bytes > allocation.peak_bytes) {
        allocation.peak_bytes = allocation.live_bytes;
    }
}

static void* tracked_malloc(size_t size) {
    if (allocation_fails(size)) {
        return NULL;
    }
    allocation_header_t* header = malloc(sizeof(*header) + size);
    if (header == NULL) {
        return NULL;
    }
    header->size = size;
    allocation.live_blocks++;
    record_allocation(0, size);
    return header + 1;
}

static void* tracked_realloc(void* ptr, size_t size) {
    allocation.reallocations++;
    if (ptr == NULL) {
        return tracked_malloc(size);
    }
    if (allocation_fails(size)) {
        return NULL;
    }
    allocation_header_t* old = (allocation_header_t*)ptr - 1;
    size_t old_size = old->size;
    allocation_header_t* header = realloc(old, sizeof(*header) + size);
    if (header == NULL) {
        return NULL;
    }
    header->size = size;
    record_allocation(old_size, size);
    return header + 1;
}

static void tracked_free(void* ptr) {
    if (ptr != NULL) {
        allocation_header_t* header = (allocation_header_t*)ptr - 1;
        allocation.live_bytes -= header->size;
        allocation.live_blocks--;
        free(header);
    }
}

static alloc_t allocator = {tracked_malloc, tracked_realloc, tracked_free};

static void reset_measurements(void) {
    allocation.attempts = 0;
    allocation.reallocations = 0;
    allocation.requested = 0;
    allocation.peak_bytes = allocation.live_bytes;
    allocation.fail_at = 0;
    allocation.fail_size = 0;
}

typedef struct completion_fixture_s {
    completions_t* cms;
    ssize_t count;
    bool failed;
    bool recover_index;
} completion_fixture_t;

static void large_completer(ic_completion_env_t* cenv, const char* prefix) {
    (void)prefix;
    completion_fixture_t* fixture = cenv->arg;
    for (ssize_t i = 0; i < fixture->count; ++i) {
        if (fixture->recover_index && i == 96) {
            allocation.fail_size = 0;
        }
        char name[64];
        (void)snprintf(name, sizeof(name), "common-prefix-%08zd", i);
        if (!completions_add(fixture->cms, name, NULL, "first", "original", 2, 1)) {
            fixture->failed = true;
        }
        // First-wins metadata and insertion order are part of completion behavior.
        if (!completions_add(fixture->cms, name, "duplicate", "second", "duplicate", 9, 9)) {
            fixture->failed = true;
        }
    }
}

static bool verify_completions(completions_t* cms, ssize_t count) {
    CHECK(completions_count(cms) == count);
    completions_sort(cms);
    for (ssize_t i = 0; i < count; ++i) {
        char name[64];
        (void)snprintf(name, sizeof(name), "common-prefix-%08zd", i);
        CHECK(strcmp(completions_get_replacement(cms, i), name) == 0);
        const char* help = NULL;
        CHECK(strcmp(completions_get_display(cms, i, &help), name) == 0);
        CHECK(strcmp(help, "first") == 0);
        CHECK(strcmp(completions_get_source(cms, i), "original") == 0);
        const char* replacement = NULL;
        ssize_t start = -1;
        ssize_t after = -1;
        CHECK(completions_get_apply_range(cms, i, "abcd", 2, &replacement, &start, &after));
        CHECK(start == 0 && after == 1 && strcmp(replacement, name) == 0);
    }
    return true;
}

static bool test_large_completions(void) {
    completions_t* cms = completions_new(&allocator);
    CHECK(cms != NULL);
    completion_fixture_t fixture = {cms, 10000, false, false};
    completions_set_completer(cms, large_completer, &fixture);
    clock_t start = clock();
    CHECK(completions_generate(NULL, cms, "", 0, fixture.count * 2) == fixture.count);
    (void)printf("BENCH completion_10000_with_duplicates_ms=%.3f\n",
                 1000.0 * (double)(clock() - start) / CLOCKS_PER_SEC);
    CHECK(!fixture.failed && verify_completions(cms, fixture.count));
    // Exercise clearing/reuse after a large generation, and regeneration as hints.
    for (int round = 0; round < 3; ++round) {
        completions_clear(cms);
        fixture.count = (round == 1 ? 2000 : 2);
        CHECK(completions_generate_hint(NULL, cms, "", 0, fixture.count * 2) == fixture.count);
        CHECK(!fixture.failed && verify_completions(cms, fixture.count));
    }
    completions_free(cms);
    return true;
}

static void edge_completer(ic_completion_env_t* cenv, const char* prefix) {
    (void)prefix;
    completion_fixture_t* fixture = cenv->arg;
    const char* names[] = {"", "", NULL, NULL, "Case", "case", "\xCE\xBB", "\xCE\xBB", "[x]\\"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        (void)completions_add(fixture->cms, names[i], NULL, NULL, NULL, 0, 0);
    }
}

static bool test_completion_edge_cases_and_budget(void) {
    completions_t* cms = completions_new(&allocator);
    CHECK(cms != NULL);
    completion_fixture_t fixture = {cms, 0, false, false};
    completions_set_completer(cms, edge_completer, &fixture);
    CHECK(completions_generate(NULL, cms, "", 0, 9) == 7);
    CHECK(strcmp(completions_get_replacement(cms, 0), "") == 0);
    CHECK(completions_get_replacement(cms, 1) == NULL);
    CHECK(completions_get_replacement(cms, 2) == NULL);
    CHECK(strcmp(completions_get_replacement(cms, 3), "Case") == 0);
    CHECK(strcmp(completions_get_replacement(cms, 4), "case") == 0);
    CHECK(strcmp(completions_get_replacement(cms, 5), "\xCE\xBB") == 0);
    CHECK(strcmp(completions_get_display(cms, 6, NULL), "\\[x]\\\\") == 0);
    // Duplicate attempts still consume the caller's completion budget.
    CHECK(completions_generate(NULL, cms, "", 0, 2) == 1);
    CHECK(!completions_add(cms, "extra", NULL, NULL, NULL, 0, 0));
    completions_free(cms);
    return true;
}

static bool test_completion_allocation_failures(void) {
    completions_t* cms = completions_new(&allocator);
    CHECK(cms != NULL);
    completion_fixture_t fixture = {cms, 300, false, true};
    completions_set_completer(cms, large_completer, &fixture);
    // Refuse the initial index repeatedly, then permit it after fallback inserts.
    allocation.fail_size = 128 * sizeof(char*);
    CHECK(completions_generate(NULL, cms, "", 0, fixture.count * 2) == fixture.count);
    CHECK(!fixture.failed && verify_completions(cms, fixture.count));
    completions_free(cms);
    for (size_t fail = 1; fail <= 240; ++fail) {
        reset_measurements();
        cms = completions_new(&allocator);
        CHECK(cms != NULL);
        fixture = (completion_fixture_t){cms, 40, false, false};
        completions_set_completer(cms, large_completer, &fixture);
        allocation.fail_at = allocation.attempts + fail;
        (void)completions_generate(NULL, cms, "", 0, 80);
        allocation.fail_at = 0;
        CHECK(completions_count(cms) <= 40);
        for (ssize_t i = 0; i < completions_count(cms); ++i) {
            const char* name = completions_get_replacement(cms, i);
            CHECK(name != NULL);
            for (ssize_t j = 0; j < i; ++j) {
                CHECK(strcmp(name, completions_get_replacement(cms, j)) != 0);
            }
        }
        // A failed insertion or index allocation must not poison the next generation.
        fixture.failed = false;
        CHECK(completions_generate(NULL, cms, "", 0, 80) == 40);
        CHECK(!fixture.failed && verify_completions(cms, 40));
        completions_free(cms);
        CHECK(allocation.live_blocks == 0);
    }
    return true;
}

static bool test_undo_memory_growth(void) {
    editstate_t* states = NULL;
    char input[10001] = {0};
    clock_t start = clock();
    for (ssize_t i = 0; i < 10000; ++i) {
        CHECK(editstate_capture(&allocator, &states, input, i));
        input[i] = 'x';
        input[i + 1] = '\0';
    }
    (void)printf("BENCH undo_10000_ms=%.3f requested_bytes=%zu peak_bytes=%zu\n",
                 1000.0 * (double)(clock() - start) / CLOCKS_PER_SEC, allocation.requested,
                 allocation.peak_bytes);
    CHECK(allocation.peak_bytes < 2 * 1024 * 1024);
    CHECK(allocation.requested < 3 * 1024 * 1024);
    for (ssize_t i = 9999; i >= 0; --i) {
        const char* restored = NULL;
        ssize_t pos = -1;
        CHECK(editstate_restore(&allocator, &states, &restored, &pos));
        input[i] = '\0';
        CHECK(pos == i && strcmp(restored, input) == 0);
        tracked_free((void*)restored);
    }
    CHECK(states == NULL);
    return true;
}

typedef struct model_state_s {
    char* input;
    ssize_t pos;
} model_state_t;

typedef struct model_stack_s {
    editstate_t* actual;
    model_state_t expected[4096];
    size_t count;
} model_stack_t;

static bool model_capture(model_stack_t* stack, const char* input, ssize_t pos) {
    CHECK(stack->count < 4096);
    char* copy = malloc(strlen(input) + 1);
    CHECK(copy != NULL);
    memcpy(copy, input, strlen(input) + 1);
    CHECK(editstate_capture(&allocator, &stack->actual, input, pos));
    stack->expected[stack->count++] = (model_state_t){copy, pos};
    return true;
}

static bool model_restore(model_stack_t* stack, char* input, ssize_t* pos) {
    CHECK(stack->count > 0);
    model_state_t expected = stack->expected[--stack->count];
    const char* restored = NULL;
    CHECK(editstate_restore(&allocator, &stack->actual, &restored, pos));
    CHECK(*pos == expected.pos && strcmp(restored, expected.input) == 0);
    memcpy(input, restored, strlen(restored) + 1);
    tracked_free((void*)restored);
    free(expected.input);
    return true;
}

static void model_clear(model_stack_t* stack) {
    editstate_done(&allocator, &stack->actual);
    while (stack->count > 0) {
        free(stack->expected[--stack->count].input);
    }
}

static uint32_t next_random(uint32_t* state) {
    *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
    return *state;
}

static bool test_undo_randomized_roundtrips(void) {
    model_stack_t undo = {0};
    model_stack_t redo = {0};
    char input[1024] = "start \xCE\xBB \xF0\x9F\x8C\x8D\nend";
    ssize_t pos = 0;
    uint32_t rng = UINT32_C(731);
    for (size_t step = 0; step < 12000; ++step) {
        unsigned action = next_random(&rng) % 10;
        if (action < 3 && undo.count > 0) {
            CHECK(model_capture(&redo, input, pos));
            CHECK(model_restore(&undo, input, &pos));
        } else if (action < 5 && redo.count > 0) {
            CHECK(model_capture(&undo, input, pos));
            CHECK(model_restore(&redo, input, &pos));
        } else {
            CHECK(model_capture(&undo, input, pos));
            model_clear(&redo);
            size_t len = strlen(input);
            size_t at = next_random(&rng) % (len + 1);
            size_t removed = next_random(&rng) % (len - at + 1);
            size_t added = next_random(&rng) % 16;
            CHECK(len - removed + added < sizeof(input));
            memmove(input + at + added, input + at + removed, len - at - removed + 1);
            for (size_t i = 0; i < added; ++i) {
                input[at + i] = (i % 7 == 0 ? '\n' : (char)('a' + next_random(&rng) % 26));
            }
            pos = (ssize_t)(at + added);
        }
        if (undo.count > 3000) {
            model_clear(&undo);
        }
    }
    while (undo.count > 0) {
        CHECK(model_restore(&undo, input, &pos));
    }
    while (redo.count > 0) {
        CHECK(model_restore(&redo, input, &pos));
    }
    model_clear(&undo);
    model_clear(&redo);
    return true;
}

static bool test_undo_allocation_failures(void) {
    char large[1024];
    memset(large, 'y', sizeof(large) - 1);
    large[sizeof(large) - 1] = '\0';
    for (size_t fail = 1; fail <= 3; ++fail) {
        editstate_t* states = NULL;
        CHECK(editstate_capture(&allocator, &states, "older", 2));
        CHECK(editstate_capture(&allocator, &states, "abc", 1));
        allocation.fail_at = allocation.attempts + fail;
        CHECK(!editstate_capture(&allocator, &states, large, 300));
        allocation.fail_at = 0;
        const char* restored = NULL;
        ssize_t pos = -1;
        CHECK(editstate_restore(&allocator, &states, &restored, &pos));
        CHECK(pos == 1 && strcmp(restored, "abc") == 0);
        tracked_free((void*)restored);
        CHECK(editstate_restore(&allocator, &states, &restored, &pos));
        CHECK(pos == 2 && strcmp(restored, "older") == 0);
        tracked_free((void*)restored);
        CHECK(states == NULL);
    }
    editstate_t* states = NULL;
    CHECK(editstate_capture(&allocator, &states, NULL, 0));
    CHECK(editstate_capture(&allocator, &states, "\xCE\xBB\nsecond", 2));
    const char* restored = "sentinel";
    ssize_t pos = 42;
    allocation.fail_at = allocation.attempts + 1;
    CHECK(!editstate_restore(&allocator, &states, &restored, &pos));
    CHECK(pos == 42 && strcmp(restored, "sentinel") == 0);
    // Discarding a completion preview/failed redo capture must need no allocation.
    size_t attempts = allocation.attempts;
    editstate_forget(&allocator, &states);
    CHECK(allocation.attempts == attempts);
    allocation.fail_at = 0;
    CHECK(editstate_restore(&allocator, &states, &restored, &pos));
    CHECK(pos == 0 && strcmp(restored, "") == 0);
    tracked_free((void*)restored);
    CHECK(states == NULL);
    CHECK(!editstate_restore(&allocator, &states, &restored, &pos));
    editstate_forget(&allocator, &states);
    return true;
}

static bool test_stringbuf_growth_and_failure(void) {
    stringbuf_t* input = sbuf_new(&allocator);
    CHECK(input != NULL);
    const ssize_t count = 1024 * 1024;
    for (ssize_t i = 0; i < count; ++i) {
        CHECK(sbuf_insert_char_at(input, (char)('a' + i % 26), i) == i + 1);
    }
    (void)printf("BENCH stringbuf_1MiB_reallocations=%zu requested_bytes=%zu\n",
                 allocation.reallocations, allocation.requested);
    CHECK(allocation.reallocations < 40);
    CHECK(allocation.requested < 5 * 1024 * 1024);
    CHECK(sbuf_len(input) == count && sbuf_string(input)[count] == '\0');
    for (ssize_t i = 0; i < count; ++i) {
        CHECK(sbuf_char_at(input, i) == (char)('a' + i % 26));
    }
    CHECK(sbuf_insert_at(input, "\xCE\xBB", count / 2) == count / 2 + 2);
    sbuf_delete_at(input, count / 2, 2);
    CHECK(sbuf_char_at(input, count / 2) == (char)('a' + (count / 2) % 26));
    sbuf_free(input);

    input = sbuf_new(&allocator);
    CHECK(input != NULL);
    CHECK(sbuf_append(input, "unchanged") == 9);
    char large[4096];
    memset(large, 'z', sizeof(large) - 1);
    large[sizeof(large) - 1] = '\0';
    allocation.fail_at = allocation.attempts + 1;
    CHECK(sbuf_insert_at(input, large, 3) == 3);
    CHECK(sbuf_len(input) == 9 && strcmp(sbuf_string(input), "unchanged") == 0);
    allocation.fail_at = 0;
    CHECK(sbuf_insert_at(input, large, 3) == 4098);
    CHECK(sbuf_len(input) == 4104 && strcmp(sbuf_string(input) + 4098, "hanged") == 0);
    sbuf_free(input);
    return true;
}

static bool test_ascii_and_unicode_widths(void) {
    for (int c = 0; c < 128; ++c) {
        CHECK(unicode_codepoint_width((unicode_codepoint_t)c) == (c >= 0x20 && c < 0x7F ? 1 : 0));
    }
    const struct {
        const char* text;
        ssize_t width;
    } examples[] = {
        {"", 0},
        {"plain ASCII 123 ~", 17},
        {"\x1b[31mred\x1b[0m", 3},
        {"\x1b]0;title\x07text", 4},
        {"a\t\nb\r", 2},
        {"e\xCC\x81", 1},
        {"a\xE7\x8C\xAB"
         "b",
         4},
        {"\xCE\xBB\xF0\x9F\x8C\x8D", 3},
        {"x\xE2\x80\x8D"
         "y",
         2},
        {"x\xFF"
         "y",
         3},
    };
    for (size_t i = 0; i < sizeof(examples) / sizeof(examples[0]); ++i) {
        CHECK(str_column_width(examples[i].text) == examples[i].width);
    }
    // A length-limited substring need not be terminated at its end.
    CHECK(str_column_width_n("abc\xE7\x8C\xAB", 3) == 3);
    CHECK(str_column_width_n("abc\xE7\x8C\xAB", 6) == 5);
    return true;
}

static bool test_history_cache_reuse(void) {
    const char* filename = "./isocline_optimization_history.log";
    FILE* file = fopen(filename, "w");
    CHECK(file != NULL);
    for (int i = 0; i < 5000; ++i) {
        CHECK(fprintf(file, "git checkout branch-%08d\n", i) > 0);
    }
    CHECK(fclose(file) == 0);
    history_t* history = history_new(&allocator);
    CHECK(history != NULL);
    history_load_from(history, filename, 5000);
    history_snapshot_t snap = {0};
    CHECK(history_snapshot_refresh(history, &snap, true));
    CHECK(history_snapshot_count(&snap) == 5000);
    history_entry_t* original = snap.entries;
    reset_measurements();
    clock_t start = clock();
    for (int i = 0; i < 1000; ++i) {
        CHECK(history_snapshot_refresh(history, &snap, true));
        CHECK(snap.entries == original);
    }
    (void)printf("BENCH cached_history_refresh_ms=%.6f allocations=%zu\n",
                 (double)(clock() - start) / CLOCKS_PER_SEC, allocation.attempts);
    CHECK(allocation.attempts == 0);
    history_snapshot_free(history, &snap);
    history_free(history);
    CHECK(remove(filename) == 0);
    return true;
}

static bool test_history_cache_invalidation(void) {
    const char* filename = "./isocline_optimization_invalidation.log";
    (void)remove(filename);
    history_t* reader = history_new(&allocator);
    history_t* writer = history_new(&allocator);
    CHECK(reader != NULL && writer != NULL);
    history_load_from(reader, filename, 32);
    history_load_from(writer, filename, 32);
    (void)history_enable_duplicates(reader, true);
    (void)history_enable_duplicates(writer, true);
    const ic_history_metadata_t first_dir[] = {{"cwd", "/work"}};
    const ic_history_metadata_t nested_dir[] = {{"cwd", "/work/sub"}};
    CHECK(history_push_with_metadata(writer, "first", first_dir, 1));
    CHECK(history_push_with_metadata(writer, "first", first_dir, 1));
    CHECK(history_push_with_metadata(writer, "second", nested_dir, 1));
    history_snapshot_t snap = {0};
    CHECK(history_snapshot_refresh(reader, &snap, false));
    CHECK(snap.count == 3);
    CHECK(history_snapshot_refresh(reader, &snap, true));
    CHECK(snap.count == 3 && snap.dedup);  // explicit duplicate allowance takes precedence
    (void)history_enable_duplicates(reader, false);
    CHECK(history_snapshot_refresh(reader, &snap, true));
    CHECK(snap.count == 2);
    CHECK(history_set_directory(reader, "/work"));
    (void)history_enable_directory(reader, true);
    CHECK(history_snapshot_refresh(reader, &snap, true));
    CHECK(snap.count == 1 && strcmp(history_snapshot_get(&snap, 0)->command, "first") == 0);
    (void)history_enable_directory_subdirs(reader, true);
    CHECK(history_snapshot_refresh(reader, &snap, true));
    CHECK(snap.count == 2);
    CHECK(history_push_with_metadata(writer, "third", first_dir, 1));
    CHECK(!history_snapshot_is_current(reader, &snap));
    CHECK(history_snapshot_refresh(reader, &snap, true));
    CHECK(snap.count == 3 && strcmp(history_snapshot_get(&snap, 0)->command, "third") == 0);
    history_begin_edit(reader);
    CHECK(history_update(reader, "draft"));
    CHECK(history_snapshot_refresh(reader, &snap, true));
    CHECK(snap.had_pending && strcmp(history_snapshot_get(&snap, 0)->command, "draft") == 0);
    CHECK(history_update(reader, "changed draft"));
    CHECK(history_snapshot_refresh(reader, &snap, true));
    CHECK(strcmp(history_snapshot_get(&snap, 0)->command, "changed draft") == 0);
    history_end_edit(reader, NULL);
    CHECK(history_snapshot_refresh(reader, &snap, true));
    CHECK(!snap.had_pending && snap.count == 3);
    (void)history_enable_directory(reader, false);
    (void)history_enable_duplicates(reader, false);
    CHECK(history_snapshot_refresh(reader, &snap, false));
    CHECK(snap.count == 3);
    (void)history_enable_duplicates(reader, true);
    CHECK(!history_snapshot_is_current(reader, &snap));
    CHECK(history_snapshot_refresh(reader, &snap, false));
    CHECK(snap.count == 4);
    history_load_from(reader, filename, 1);
    CHECK(history_snapshot_refresh(reader, &snap, true));
    CHECK(snap.count == 1);
    history_load_from(reader, NULL, 0);
    CHECK(history_snapshot_refresh(reader, &snap, true));
    CHECK(snap.count == 0 && snap.disabled);
    history_load_from(reader, filename, 32);
    CHECK(history_snapshot_refresh(reader, &snap, true));
    CHECK(snap.count == 1 && !snap.disabled);
    CHECK(remove(filename) == 0);
    CHECK(history_snapshot_refresh(reader, &snap, true));
    CHECK(snap.count == 0);
    CHECK(history_push(writer, "recreated"));
    CHECK(history_snapshot_refresh(reader, &snap, true));
    CHECK(snap.count == 1 && strcmp(history_snapshot_get(&snap, 0)->command, "recreated") == 0);
    history_snapshot_free(reader, &snap);
    history_free(reader);
    history_free(writer);
    CHECK(remove(filename) == 0);
    return true;
}

int main(void) {
    const struct {
        const char* name;
        bool (*run)(void);
    } tests[] = {
        {"large_completions", test_large_completions},
        {"completion_edge_cases_and_budget", test_completion_edge_cases_and_budget},
        {"completion_allocation_failures", test_completion_allocation_failures},
        {"undo_memory_growth", test_undo_memory_growth},
        {"undo_randomized_roundtrips", test_undo_randomized_roundtrips},
        {"undo_allocation_failures", test_undo_allocation_failures},
        {"stringbuf_growth_and_failure", test_stringbuf_growth_and_failure},
        {"ascii_and_unicode_widths", test_ascii_and_unicode_widths},
        {"history_cache_reuse", test_history_cache_reuse},
        {"history_cache_invalidation", test_history_cache_invalidation},
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        reset_measurements();
        if (!tests[i].run() || allocation.live_blocks != 0 || allocation.live_bytes != 0) {
            (void)fprintf(stderr, "FAIL %s (live blocks: %zu, bytes: %zu)\n", tests[i].name,
                          allocation.live_blocks, allocation.live_bytes);
            return 1;
        }
        (void)printf("PASS %s\n", tests[i].name);
    }
    (void)printf("All %zu isocline optimization tests passed\n", sizeof(tests) / sizeof(tests[0]));
    return 0;
}
