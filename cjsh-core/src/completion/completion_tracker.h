/*
  completion_tracker.h

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

#ifndef CJSH_CORE_SRC_COMPLETION_COMPLETION_TRACKER_H
#define CJSH_CORE_SRC_COMPLETION_COMPLETION_TRACKER_H

#include <string>
#include <unordered_set>

#include "isocline.h"

namespace completion_tracker {

struct CompletionTracker {
    std::unordered_set<std::string> added_completions;
    std::unordered_set<std::string> preferred_completions;
    ic_completion_env_t* cenv;
    std::string original_prefix;
    size_t total_completions_added{};

    CompletionTracker(ic_completion_env_t* env, const char* prefix);
    ~CompletionTracker();

    bool has_reached_completion_limit() const;
    std::string calculate_final_result(const char* completion_text, long delete_before = 0) const;
    bool would_create_duplicate(const char* completion_text, long delete_before = 0);
    bool add_completion_prim_with_source_if_unique(const char* completion_text, const char* display,
                                                   const char* help, const char* source,
                                                   long delete_before, long delete_after);
};

void completion_session_begin(ic_completion_env_t* cenv, const char* prefix);
void completion_session_end();

void prioritize_completion(const char* completion_text, long delete_before);
bool is_completion_preferred(const char* completion_text, long delete_before);

bool safe_add_completion_with_source(ic_completion_env_t* cenv, const char* completion_text,
                                     const char* source);
bool safe_add_completion_prim_with_source(ic_completion_env_t* cenv, const char* completion_text,
                                          const char* display, const char* help, const char* source,
                                          long delete_before, long delete_after);

bool completion_limit_hit();

bool set_completion_max_results(long max_results, std::string* error_message = nullptr);
long get_completion_max_results();
long get_completion_default_max_results();
long get_completion_min_allowed_results();

}  // namespace completion_tracker

#endif  // CJSH_CORE_SRC_COMPLETION_COMPLETION_TRACKER_H
