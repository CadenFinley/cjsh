/*
  external_sub_completions.h

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

#ifndef CJSH_CORE_SRC_COMPLETION_EXTERNAL_SUB_COMPLETIONS_H
#define CJSH_CORE_SRC_COMPLETION_EXTERNAL_SUB_COMPLETIONS_H

#include <string>
#include <vector>

#include "completion_context.h"
#include "completion_spec.h"
#include "isocline.h"

void handle_external_sub_completions(ic_completion_env_t* cenv,
                                     const completion_context::CommandLineContext& command_context);
completion_specs::CommandDoc parse_man_page_completion_spec(const std::string& command,
                                                            const std::string& man_text);
std::string get_command_summary(const std::string& command, bool allow_fetch = true);
class ScopedCompletionDocumentationLookup {
   public:
    ScopedCompletionDocumentationLookup();
    ~ScopedCompletionDocumentationLookup();
    ScopedCompletionDocumentationLookup(const ScopedCompletionDocumentationLookup&) = delete;
    ScopedCompletionDocumentationLookup& operator=(const ScopedCompletionDocumentationLookup&) =
        delete;
};
struct CompletionCacheTargetResult {
    bool generated{false};
    std::vector<std::string> discovered_targets;
};

CompletionCacheTargetResult regenerate_external_completion_cache_target(
    const std::string& target, bool force_refresh = true, bool discover_subcommands = false);

#endif  // CJSH_CORE_SRC_COMPLETION_EXTERNAL_SUB_COMPLETIONS_H
