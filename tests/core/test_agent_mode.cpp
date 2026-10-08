/*
  test_agent_mode.cpp

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

#include <cstdio>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "agent_mode.h"
#include "cjshopt_command.h"
#include "exec.h"
#include "isocline.h"
#include "keybindings.h"
#include "keycodes.h"
#include "shell.h"
#include "shell_env.h"

std::unique_ptr<Shell> shell;

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        (void)std::fprintf(stderr, "[FAIL] %s\n", message);
        return false;
    }
    return true;
}

bool expect_command(const std::vector<std::string>& args, int expected, const char* message) {
    return expect(agent_mode::command(args) == expected, message);
}

std::string capture_command_output(const std::vector<std::string>& args, int* status = nullptr) {
    std::ostringstream output;
    std::streambuf* previous = std::cout.rdbuf(output.rdbuf());
    cjsh_env::set_startup_active(false);
    const int result = agent_mode::command(args);
    cjsh_env::set_startup_active(true);
    std::cout.rdbuf(previous);
    if (status != nullptr) {
        *status = result;
    }
    return output.str();
}

size_t occurrence_count(const std::string& text, const std::string& needle) {
    size_t count = 0;
    size_t position = 0;
    while ((position = text.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

bool key_uses_action(const char* key_spec, ic_key_action_t expected) {
    ic_keycode_t key = IC_KEY_NONE;
    ic_key_action_t actual = IC_KEY_ACTION_NONE;
    return ic_parse_key_spec(key_spec, &key) && ic_get_key_binding(key, &actual) &&
           actual == expected;
}

bool test_parses_flyline_protocol_with_surrounding_prose() {
    const std::string output =
        "Here are two choices:\n```json\n[\n"
        "  {\"command\":\"find . -mtime +3\",\"description\":\"Old files\"},\n"
        "  {\"description\":\"Quoted and Unicode \\u2713\","
        "\"command\":\"printf \\\"ok\\\\n\\\"\",\"ignored\":{\"nested\":[1,true]}}\n"
        "]\n```\nChoose one.";

    std::vector<agent_mode::Suggestion> suggestions;
    std::string error;
    if (!expect(agent_mode::parse_suggestions(output, &suggestions, &error),
                "valid executor JSON should parse")) {
        return false;
    }
    return expect(error.empty(), "successful parse should clear its error") &&
           expect(suggestions.size() == 2, "both suggestions should be retained") &&
           expect(suggestions[0].command == "find . -mtime +3",
                  "the first command should round-trip") &&
           expect(suggestions[1].command == "printf \"ok\\n\"", "JSON escapes should be decoded") &&
           expect(suggestions[1].description == "Quoted and Unicode ✓",
                  "Unicode escapes should become UTF-8");
}

bool test_ignores_entries_without_commands() {
    const std::string output =
        "  [ {\"description\":\"missing\"}, {\"command\":\"   \"}, "
        "{\"command\":\"pwd\"} ]";
    std::vector<agent_mode::Suggestion> suggestions;
    return expect(agent_mode::parse_suggestions(output, &suggestions),
                  "an array with one usable command should parse") &&
           expect(suggestions.size() == 1, "empty commands should be discarded") &&
           expect(suggestions[0].command == "pwd", "usable command should remain");
}

bool test_rejects_malformed_or_unframed_output() {
    std::vector<agent_mode::Suggestion> suggestions;
    std::string error;
    if (!expect(!agent_mode::parse_suggestions("not json", &suggestions, &error),
                "plain prose should be rejected") ||
        !expect(!error.empty(), "rejected prose should explain the failure")) {
        return false;
    }
    if (!expect(!agent_mode::parse_suggestions("prefix [{\"command\":\"pwd\"}]", &suggestions),
                "an inline array should not be mistaken for the protocol")) {
        return false;
    }
    if (!expect(!agent_mode::parse_suggestions("[{\"command\": 42}]", &suggestions),
                "command must be a JSON string")) {
        return false;
    }
    return expect(!agent_mode::parse_suggestions("[]", &suggestions),
                  "an empty suggestion array should be rejected");
}

bool test_limits_suggestions_to_three() {
    const std::string output =
        R"([{"command":"one"},{"command":"two"},{"command":"three"},{"command":"four"}])";
    std::vector<agent_mode::Suggestion> suggestions;
    return expect(agent_mode::parse_suggestions(output, &suggestions),
                  "four valid executor suggestions should parse") &&
           expect(suggestions.size() == 3, "only three suggestions should reach the menu") &&
           expect(suggestions.back().command == "three", "suggestion ordering should be retained");
}

bool test_parser_edge_cases() {
    std::string error;
    if (!expect(!agent_mode::parse_suggestions("[]", nullptr, &error),
                "a null suggestion destination should be rejected") ||
        !expect(!error.empty(), "a null suggestion destination should explain the error")) {
        return false;
    }

    std::vector<agent_mode::Suggestion> suggestions;
    const std::string surrogate_pair =
        R"([{"command":"printf ok","description":"emoji \uD83D\uDE00"}])";
    if (!expect(agent_mode::parse_suggestions(surrogate_pair, &suggestions, &error),
                "a valid Unicode surrogate pair should parse") ||
        !expect(suggestions[0].description == "emoji 😀",
                "a Unicode surrogate pair should decode to UTF-8")) {
        return false;
    }

    if (!expect(!agent_mode::parse_suggestions(R"([{"command":"pwd","description":"\uD83D"}])",
                                               &suggestions),
                "a high surrogate without a low surrogate should be rejected") ||
        !expect(!agent_mode::parse_suggestions(R"([{"command":"pwd","description":"\uDE00"}])",
                                               &suggestions),
                "an unexpected low surrogate should be rejected")) {
        return false;
    }

    std::string deeply_nested = R"([{"command":"pwd","extra":)";
    deeply_nested.append(66, '[');
    deeply_nested += '0';
    deeply_nested.append(66, ']');
    deeply_nested += "}]";
    return expect(!agent_mode::parse_suggestions(deeply_nested, &suggestions),
                  "excessively nested executor JSON should be rejected");
}

bool test_response_types() {
    agent_mode::Response response;
    std::string error = "previous error";
    if (!expect(agent_mode::parse_response(
                    "Answer:\n```json\n{\"text\":\"  First line\\n  Second line \\u2713\\n\","
                    "\"metadata\":{\"sources\":[1,true]}}\n```",
                    &response, &error),
                "a framed informational response should parse") ||
        !expect(response.type == agent_mode::ResponseType::Text && response.suggestions.empty(),
                "informational responses must not become command suggestions") ||
        !expect(response.message == "  First line\n  Second line ✓\n",
                "answers should preserve indentation, newlines, and Unicode") ||
        !expect(error.empty(), "a successful response should clear previous errors")) {
        return false;
    }
    if (!expect(agent_mode::parse_response(R"({"error":"Unable to reach the provider."})",
                                           &response, &error),
                "an agent-reported error should parse") ||
        !expect(response.type == agent_mode::ResponseType::Error &&
                    response.message == "Unable to reach the provider.",
                "agent errors should retain their message and type")) {
        return false;
    }
    return expect(agent_mode::parse_response(R"([{"command":"pwd","description":"Location"}])",
                                             &response, &error),
                  "existing command arrays should still parse as responses") &&
           expect(response.type == agent_mode::ResponseType::Suggestions &&
                      response.suggestions.size() == 1 && response.message.empty(),
                  "command responses should clear earlier text and errors");
}

bool test_rejects_invalid_responses() {
    agent_mode::Response response;
    std::string error;
    if (!expect(!agent_mode::parse_response("{}", nullptr, &error) && !error.empty(),
                "a null response destination should report an error")) {
        return false;
    }
    const std::vector<std::string> invalid = {
        "",
        "unframed answer",
        "{}",
        "[]",
        R"({"text":""})",
        R"({"error":" \n\t"})",
        R"({"text":42})",
        R"({"error":null})",
        R"({"text":"answer","error":"failure"})",
        R"({"text":"first","text":"second"})",
        R"({"text":"answer","command":"pwd"})",
        R"({"text":"answer",})",
        R"({"text":"answer",)",
        R"({"text":"\uD800"})",
        R"({"error":"unterminated})",
        R"({"metadata":{"text":"nested"}})",
    };
    for (const auto& output : invalid) {
        response = {agent_mode::ResponseType::Text, {{"old", "old"}}, "old"};
        if (!expect(!agent_mode::parse_response(output, &response, &error) && !error.empty(),
                    "malformed, empty, or ambiguous responses should report errors") ||
            !expect(response.message.empty() && response.suggestions.empty(),
                    "failed parses must not leave stale or partial responses")) {
            return false;
        }
    }
    return true;
}

bool test_executor_stream_capture() {
    // Both streams exceed a pipe buffer. Reading one to completion before the
    // other would deadlock, and mixing them would corrupt the response protocol.
    const auto failed = exec_utils::execute_command_vector_for_output_with_progress(
        {"/bin/sh", "-c",
         "i=0; while [ \"$i\" -lt 4096 ]; do "
         "printf 'oooooooooooooooooooooooooooooooo'; "
         "printf 'eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee' >&2; i=$((i+1)); done; exit 7"},
        {});
    if (!expect(!failed.success && failed.exit_code == 7,
                "capture should retain executor failure") ||
        !expect(failed.output == std::string(4096 * 32, 'o'),
                "stdout should be drained without stderr contamination") ||
        !expect(failed.error_output == std::string(4096 * 32, 'e'),
                "stderr diagnostics should be drained and retained separately")) {
        return false;
    }
    const auto succeeded = exec_utils::execute_command_vector_for_output_with_progress(
        {"/bin/sh", "-c", "printf '%s' '{\"text\":\"answer\"}'; printf '[diagnostic]' >&2"}, [] {});
    agent_mode::Response response;
    return expect(succeeded.success && succeeded.exit_code == 0,
                  "stderr output alone should not mark a successful executor as failed") &&
           expect(agent_mode::parse_response(succeeded.output, &response) &&
                      response.type == agent_mode::ResponseType::Text &&
                      response.message == "answer",
                  "successful responses should parse independently of stderr diagnostics");
}

bool test_configuration_validation_and_state() {
    bool ok = true;
    ok &= expect_command({"agent-mode", "reset"}, 0, "reset should restore defaults");
    ok &= expect(agent_mode::palette_entry_enabled(), "agent mode should default to enabled");
    ok &= expect(capture_command_output({"agent-mode", "status"}).find("Executors: none") !=
                     std::string::npos,
                 "status should report an empty executor configuration");
    ok &= expect_command({"agent-mode"}, 0, "agent-mode without arguments should show help");
    ok &= expect_command({"agent-mode", "--help"}, 0, "agent-mode --help should succeed");
    ok &= expect_command({"agent-mode", "help"}, 0, "agent-mode help should succeed");
    ok &= expect_command({"agent-mode", "set"}, 1, "set should require an executor command");
    ok &= expect_command({"agent-mode", "set", "--command", "   "}, 1,
                         "set should reject a whitespace-only command");
    ok &= expect_command({"agent-mode", "set", "--command", "''"}, 1,
                         "set should reject a command that tokenizes to no executable");
    ok &= expect_command({"agent-mode", "set", "--command", "true", "--trigger-prefix", ""}, 1,
                         "set should reject an empty trigger prefix");
    ok &=
        expect_command({"agent-mode", "set", "--unknown"}, 1, "set should reject unknown options");

    ok &= expect_command({"agent-mode", "set", "--command=true"}, 0,
                         "set should accept an equals-form fallback command");
    ok &= expect_command(
        {"agent-mode", "set", "--command=true", "--system-prompt=portable", "--trigger-prefix=ai:"},
        0, "set should accept equals-form options");
    ok &= expect(
        agent_mode::matching_trigger_prefix_length("ai:request") == std::optional<std::size_t>(3),
        "configured prefixes should be available to editor integrations");
    ok &= expect(key_uses_action("enter", IC_KEY_ACTION_RUNOFF),
                 "a configured prefix should install the Enter runoff binding");
    ok &= expect(key_uses_action("alt-a", IC_KEY_ACTION_RUNOFF),
                 "enabled agent mode should install its activation runoff binding");

    ok &= expect_command({"agent-mode", "set", "--command=false", "--system-prompt=replaced",
                          "--trigger-prefix=ai:"},
                         0, "setting the same prefix should replace its executor");
    int status = -1;
    const std::string status_output = capture_command_output({"agent-mode", "status"}, &status);
    ok &= expect(status == 0, "status should succeed");
    ok &= expect(status_output.find("Agent assistance: enabled") != std::string::npos,
                 "status should report enabled state");
    ok &= expect(status_output.find("default") != std::string::npos,
                 "status should list the fallback executor");
    ok &= expect(status_output.find("false") != std::string::npos &&
                     status_output.find("system prompt configured") != std::string::npos,
                 "status should show the replacement executor and prompt state");
    ok &= expect(occurrence_count(status_output, "prefix \"ai:\"") == 1,
                 "replacing a prefix should not add a duplicate executor");
    ok &= expect(
        capture_command_output({"agent-mode", "list"}).find("Executors:") != std::string::npos,
        "list should report configured executors");

    ok &= expect_command({"agent-mode", "off", "extra"}, 1, "off should reject extra arguments");
    ok &= expect_command({"agent-mode", "off"}, 0, "off should succeed");
    ok &= expect(!agent_mode::palette_entry_enabled(), "off should hide the palette entry");
    ok &= expect(!agent_mode::matching_trigger_prefix_length("ai:request").has_value(),
                 "off should disable prefix matching");
    ok &= expect(!key_uses_action("enter", IC_KEY_ACTION_RUNOFF),
                 "off should remove the Enter runoff binding");
    ok &= expect(!key_uses_action("alt-a", IC_KEY_ACTION_RUNOFF),
                 "off should remove the activation runoff binding");
    ok &= expect_command({"agent-mode", "on"}, 0, "on should succeed");
    ok &= expect(agent_mode::palette_entry_enabled(), "on should restore the palette entry");
    ok &= expect(key_uses_action("enter", IC_KEY_ACTION_RUNOFF),
                 "on should restore the Enter runoff binding");
    ok &= expect(key_uses_action("alt-a", IC_KEY_ACTION_RUNOFF),
                 "on should restore the activation runoff binding");

    agent_mode::disable_for_startup();
    ok &= expect(!agent_mode::palette_entry_enabled(),
                 "the startup disable should hide the palette entry");
    ok &= expect_command({"agent-mode", "set", "--command=true", "--trigger-prefix=startup:"}, 0,
                         "executors should still be configurable while startup-disabled");
    ok &= expect(!agent_mode::palette_entry_enabled(),
                 "loading an executor should not override the startup disable");
    ok &= expect(!agent_mode::matching_trigger_prefix_length("startup:request").has_value(),
                 "startup-disabled trigger prefixes should remain inactive");
    ok &= expect_command({"agent-mode", "on"}, 0,
                         "an explicit on should override the startup disable");
    ok &= expect(agent_mode::matching_trigger_prefix_length("startup:request").has_value(),
                 "explicitly enabling agent mode should restore startup-loaded prefixes");

    ok &= expect_command({"agent-mode", "on", "extra"}, 1, "on should reject extra arguments");
    ok &= expect_command({"agent-mode", "does-not-exist"}, 1,
                         "unknown subcommands should be rejected");
    ok &=
        expect_command({"agent-mode", "reset", "extra"}, 1, "reset should reject extra arguments");
    return ok;
}

bool test_key_configuration_and_precedence() {
    bool ok = true;
    ok &= expect_command({"agent-mode", "reset"}, 0, "reset should prepare key tests");
    ok &= expect_command({"agent-mode", "key"}, 1, "key should require one argument");
    ok &= expect_command({"agent-mode", "key", "F3", "extra"}, 1,
                         "key should reject extra arguments");
    ok &= expect_command({"agent-mode", "key", "not-a-real-key"}, 1,
                         "key should reject invalid key specifications");
    ok &= expect_command({"agent-mode", "key", "enter"}, 1, "key should reject unmodified Enter");
    ok &= expect_command({"agent-mode", "key", "alt-enter"}, 1, "key should reject modified Enter");
    ok &= expect_command({"agent-mode", "key", "F3"}, 0, "key should accept a function key");
    ok &= expect(key_uses_action("f3", IC_KEY_ACTION_RUNOFF),
                 "a configured agent key should install a runoff binding");
    ok &= expect(
        capture_command_output({"agent-mode", "key", "status"}).find("f3") != std::string::npos,
        "key status should report the configured key");
    ok &= expect_command({"agent-mode", "key", "off"}, 0, "key off should succeed");
    ok &= expect(!key_uses_action("f3", IC_KEY_ACTION_RUNOFF),
                 "key off should remove the configured runoff binding");
    ok &= expect(
        capture_command_output({"agent-mode", "key", "status"}).find("off") != std::string::npos,
        "key status should report disabled activation");
    ok &= expect_command({"agent-mode", "key", "default"}, 0, "key default should restore Alt+A");
    ok &= expect(key_uses_action("alt-a", IC_KEY_ACTION_RUNOFF),
                 "key default should restore the Alt+A runoff binding");
    ok &= expect(
        capture_command_output({"agent-mode", "key", "status"}).find("alt+a") != std::string::npos,
        "key default should be visible in status");

    ic_keycode_t f4 = IC_KEY_NONE;
    ok &= expect(ic_parse_key_spec("F4", &f4), "F4 should parse for precedence testing");
    if (f4 != IC_KEY_NONE) {
        set_custom_keybinding(f4, "true", "custom F4");
        ok &= expect_command({"agent-mode", "key", "F4"}, 0,
                             "agent key configuration should tolerate a custom conflict");
        ok &= expect(has_custom_keybinding(f4),
                     "agent configuration must preserve the custom command binding");
        ok &= expect(capture_command_output({"agent-mode", "status"})
                             .find("overridden by a custom command binding") != std::string::npos,
                     "status should explain when a custom key takes precedence");
        clear_custom_keybinding(f4);
        agent_mode::apply_key_bindings();
        ic_key_action_t action = IC_KEY_ACTION_NONE;
        ok &= expect(ic_get_key_binding(f4, &action) && action == IC_KEY_ACTION_RUNOFF,
                     "clearing a custom conflict should restore the agent runoff binding");
    }
    clear_all_custom_keybindings();
    ok &= expect_command({"agent-mode", "reset"}, 0, "key tests should restore defaults");
    return ok;
}

bool test_clear_selectors() {
    bool ok = true;
    ok &= expect_command({"agent-mode", "reset"}, 0, "reset should prepare clear tests");
    ok &= expect_command({"agent-mode", "set", "--command", "true"}, 0,
                         "clear tests should configure a fallback");
    ok &= expect_command({"agent-mode", "set", "--command", "true", "--trigger-prefix", "one:"}, 0,
                         "clear tests should configure the first prefix");
    ok &= expect_command({"agent-mode", "set", "--command", "true", "--trigger-prefix", "two:"}, 0,
                         "clear tests should configure the second prefix");

    ok &= expect_command({"agent-mode", "clear", "--trigger-prefix"}, 1,
                         "clear should require a prefix value");
    ok &= expect_command({"agent-mode", "clear", "--trigger-prefix="}, 1,
                         "clear should reject an empty prefix");
    ok &= expect_command({"agent-mode", "clear", "--trigger-prefix", "one:", "extra"}, 1,
                         "clear should reject extra prefix arguments");
    ok &= expect_command({"agent-mode", "clear", "--invalid"}, 1,
                         "clear should reject invalid selectors");
    ok &= expect_command({"agent-mode", "clear", "--default"}, 0,
                         "clear --default should remove the fallback");
    ok &= expect_command({"agent-mode", "clear", "--default"}, 1,
                         "clear --default should report a missing fallback");
    ok &= expect_command({"agent-mode", "clear", "--trigger-prefix=one:"}, 0,
                         "clear should accept an equals-form prefix");
    ok &= expect(!agent_mode::matching_trigger_prefix_length("one:request").has_value(),
                 "cleared prefixes should stop matching");
    ok &= expect_command({"agent-mode", "clear", "--trigger-prefix", "missing:"}, 1,
                         "clear should report a missing prefix");
    ok &= expect_command({"agent-mode", "clear", "all"}, 0, "clear should accept the all alias");
    ok &= expect(!agent_mode::matching_trigger_prefix_length("two:request").has_value(),
                 "clear all should remove remaining prefixes");
    ok &= expect(!key_uses_action("enter", IC_KEY_ACTION_RUNOFF),
                 "removing all prefixes should release the Enter runoff binding");
    ok &= expect_command({"agent-mode", "set", "--command", "true", "--trigger-prefix", "all:"}, 0,
                         "clear --all should have an executor to remove");
    ok &= expect_command({"agent-mode", "clear", "--all"}, 0,
                         "clear should accept the explicit --all selector");
    ok &=
        expect_command({"agent-mode", "set", "--command", "true", "--trigger-prefix", "implicit:"},
                       0, "implicit clear-all should have an executor to remove");
    ok &= expect_command({"agent-mode", "clear"}, 0,
                         "clear without a selector should remove all executors");
    ok &= expect_command({"agent-mode", "clear"}, 1,
                         "clearing an already empty configuration should report no match");
    ok &= expect_command({"agent-mode", "reset"}, 0, "clear tests should restore defaults");
    return ok;
}

}  // namespace

int main() {
    cjsh_env::reset_shell_state();
    cjsh_env::set_startup_active(true);
    const std::vector<std::pair<const char*, bool (*)()>> tests = {
        {"parse protocol", test_parses_flyline_protocol_with_surrounding_prose},
        {"ignore empty commands", test_ignores_entries_without_commands},
        {"reject malformed output", test_rejects_malformed_or_unframed_output},
        {"limit suggestions", test_limits_suggestions_to_three},
        {"parser edge cases", test_parser_edge_cases},
        {"response types", test_response_types},
        {"invalid responses", test_rejects_invalid_responses},
        {"executor stream capture", test_executor_stream_capture},
        {"configuration validation and state", test_configuration_validation_and_state},
        {"key configuration and precedence", test_key_configuration_and_precedence},
        {"clear selectors", test_clear_selectors},
    };
    size_t failed = 0;
    for (const auto& [name, test] : tests) {
        if (!test()) {
            (void)std::fprintf(stderr, "[FAIL] agent mode: %s\n", name);
            ++failed;
        }
    }
    const size_t test_count = tests.size();
    if (failed != 0) {
        (void)std::fprintf(stderr, "%zu/%zu agent-mode tests failed\n", failed, test_count);
        return 1;
    }
    std::printf("All %zu agent-mode tests passed\n", test_count);
    return 0;
}
