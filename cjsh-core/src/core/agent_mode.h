/*
  agent_mode.h

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

#ifndef CJSH_CORE_SRC_CORE_AGENT_MODE_H
#define CJSH_CORE_SRC_CORE_AGENT_MODE_H

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using ic_keycode_t = unsigned int;

namespace agent_mode {

struct Suggestion {
    std::string command;
    std::string description;
};

enum class ResponseType {
    Suggestions,
    Text,
    Error
};

struct Response {
    ResponseType type{ResponseType::Suggestions};
    std::vector<Suggestion> suggestions;
    std::string message;
};

// Parse command suggestions, {"text":"..."} answers, or {"error":"..."} errors.
// Prose and Markdown fences may appear around the JSON response.
bool parse_response(const std::string& output, Response* response,
                    std::string* error_message = nullptr);

// Parse the executor protocol: a JSON array of objects containing a non-empty
// `command` and an optional `description`. Prose and Markdown fences may appear
// around the array.
bool parse_suggestions(const std::string& output, std::vector<Suggestion>* suggestions,
                       std::string* error_message = nullptr);

int command(const std::vector<std::string>& args);

// Disable agent mode for this startup. Loading executor definitions must not
// implicitly re-enable it; an explicit `cjshopt agent-mode on` still can.
void disable_for_startup();

// Install the runoff bindings implied by the current cjshopt configuration.
// This is intentionally safe to call after a keymap reset or profile change.
void apply_key_bindings();

// Handle an isocline runoff key. Returns true only when the feature consumed it.
bool handle_runoff_key(ic_keycode_t key);

// Run agent assistance from the command palette.
bool handle_palette_entry();

// Whether a command-palette entry should currently be advertised.
bool palette_entry_enabled();

// Return the length of the longest enabled trigger prefix matching the start
// of the buffer. The syntax highlighter uses this to render agent requests as
// natural language instead of attempting to parse them as shell commands.
std::optional<std::size_t> matching_trigger_prefix_length(std::string_view buffer);

}  // namespace agent_mode

#endif  // CJSH_CORE_SRC_CORE_AGENT_MODE_H
