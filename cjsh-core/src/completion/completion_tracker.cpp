/*
  completion_tracker.cpp

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

#include "completion_tracker.h"

#include <atomic>
#include <cctype>
#include <cstddef>
#include <limits>
#include <string>

#include "isocline.h"
#include "string_utils.h"

namespace completion_tracker {

namespace {

constexpr size_t kDefaultMaxCompletions = 1000;
constexpr size_t kMinMaxCompletions = 1;
constexpr size_t kTrackerEntryMultiplier = 2;

std::atomic<size_t> g_configured_max_completions{kDefaultMaxCompletions};
thread_local CompletionTracker* g_current_completion_tracker = nullptr;

size_t configured_completion_limit() {
    size_t limit = g_configured_max_completions.load(std::memory_order_relaxed);
    if (limit < kMinMaxCompletions) {
        return kMinMaxCompletions;
    }
    return limit;
}

size_t tracker_entry_cap() {
    size_t limit = configured_completion_limit();
    const size_t max_allowed = std::numeric_limits<size_t>::max() / kTrackerEntryMultiplier;
    if (limit > max_allowed) {
        return std::numeric_limits<size_t>::max();
    }
    size_t cap = limit * kTrackerEntryMultiplier;
    return cap > 0 ? cap : std::numeric_limits<size_t>::max();
}

std::string canonicalize_final_result(const std::string& result) {
    return string_utils::trim_right_ascii_whitespace_copy(result);
}

}  // namespace

CompletionTracker::CompletionTracker(ic_completion_env_t* env, const char* prefix)
    : cenv(env), original_prefix(prefix) {
    added_completions.reserve(128);
}

CompletionTracker::~CompletionTracker() {
    added_completions.clear();
}

bool CompletionTracker::has_reached_completion_limit() const {
    return total_completions_added >= configured_completion_limit();
}

std::string CompletionTracker::calculate_final_result(const char* completion_text,
                                                      long delete_before) const {
    std::string prefix_str = original_prefix;

    if (delete_before > 0 && delete_before <= static_cast<long>(prefix_str.length())) {
        const size_t delete_length = static_cast<size_t>(delete_before);
        prefix_str = prefix_str.substr(0, prefix_str.length() - delete_length);
    }

    return prefix_str + completion_text;
}

bool CompletionTracker::would_create_duplicate(const char* completion_text, long delete_before) {
    if (added_completions.size() >= tracker_entry_cap()) {
        return true;
    }

    std::string final_result = calculate_final_result(completion_text, delete_before);
    std::string canonical_result = canonicalize_final_result(final_result);
    return added_completions.find(canonical_result) != added_completions.end();
}

bool CompletionTracker::add_completion_prim_with_source_if_unique(
    const char* completion_text, const char* display, const char* help, const char* source,
    long delete_before, long delete_after) {
    if (has_reached_completion_limit()) {
        return true;
    }

    if (would_create_duplicate(completion_text, delete_before)) {
        return true;
    }

    std::string final_result = calculate_final_result(completion_text, delete_before);
    (void)added_completions.insert(canonicalize_final_result(final_result));
    total_completions_added++;
    return ic_add_completion_prim_with_source(cenv, completion_text, display, help, source,
                                              delete_before, delete_after);
}

void completion_session_begin(ic_completion_env_t* cenv, const char* prefix) {
    delete g_current_completion_tracker;
    g_current_completion_tracker = new CompletionTracker(cenv, prefix);
}

void completion_session_end() {
    if (g_current_completion_tracker != nullptr) {
        delete g_current_completion_tracker;
        g_current_completion_tracker = nullptr;
    }
}

void prioritize_completion(const char* completion_text, long delete_before) {
    if (g_current_completion_tracker != nullptr) {
        auto& tracker = *g_current_completion_tracker;
        tracker.preferred_completions.insert(canonicalize_final_result(
            tracker.calculate_final_result(completion_text, delete_before)));
    }
}

bool is_completion_preferred(const char* completion_text, long delete_before) {
    if (g_current_completion_tracker == nullptr ||
        g_current_completion_tracker->preferred_completions.empty()) {
        return false;
    }
    const auto& tracker = *g_current_completion_tracker;
    return tracker.preferred_completions.count(canonicalize_final_result(
               tracker.calculate_final_result(completion_text, delete_before))) != 0;
}

bool safe_add_completion_with_source(ic_completion_env_t* cenv, const char* completion_text,
                                     const char* source) {
    if ((g_current_completion_tracker != nullptr) &&
        g_current_completion_tracker->has_reached_completion_limit()) {
        return true;
    }
    return ic_add_completion_ex_with_source(cenv, completion_text, nullptr, nullptr, source);
}

bool safe_add_completion_prim_with_source(ic_completion_env_t* cenv, const char* completion_text,
                                          const char* display, const char* help, const char* source,
                                          long delete_before, long delete_after) {
    if (g_current_completion_tracker != nullptr) {
        return g_current_completion_tracker->add_completion_prim_with_source_if_unique(
            completion_text, display, help, source, delete_before, delete_after);
    }
    return ic_add_completion_prim_with_source(cenv, completion_text, display, help, source,
                                              delete_before, delete_after);
}

bool completion_limit_hit() {
    return (g_current_completion_tracker != nullptr) &&
           g_current_completion_tracker->has_reached_completion_limit();
}

bool set_completion_max_results(long max_results, std::string* error_message) {
    if (max_results < static_cast<long>(kMinMaxCompletions)) {
        if (error_message != nullptr) {
            *error_message =
                "value must be greater than or equal to " + std::to_string(kMinMaxCompletions);
        }
        return false;
    }

    g_configured_max_completions.store(static_cast<size_t>(max_results), std::memory_order_relaxed);
    if (error_message != nullptr) {
        error_message->clear();
    }
    return true;
}

long get_completion_max_results() {
    return static_cast<long>(configured_completion_limit());
}

long get_completion_default_max_results() {
    return static_cast<long>(kDefaultMaxCompletions);
}

long get_completion_min_allowed_results() {
    return static_cast<long>(kMinMaxCompletions);
}

}  // namespace completion_tracker
