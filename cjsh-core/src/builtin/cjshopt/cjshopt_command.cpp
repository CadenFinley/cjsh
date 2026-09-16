/*
  cjshopt_command.cpp

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

#include "cjshopt_command.h"

#include "builtin_help.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "agent_mode.h"
#include "cjshopt_command.h"
#include "error_out.h"
#include "shell_env.h"

namespace {

enum class CjshoptSubcommand : std::uint8_t {
    StyleDef,
    CompletionCase,
    HistorySearchCase,
    HistoryDirectory,
    HistoryDirectorySubdirs,
    CompletionSpell,
    CompletionSpellEnter,
    CompletionLearning,
    ExitConfirmation,
    SmartCd,
    Extglob,
    ScriptExtensionInterpreter,
    LineNumbers,
    LineNumbersContinuation,
    LineNumbersReplacePrompt,
    CurrentLineNumberHighlight,
    MultilineStartLines,
    MultilineMaxLines,
    MenuMaxLines,
    MultilineBottomLines,
    HintDelay,
    IdleTimeout,
    CompletionPreview,
    CompletionAutoMenu,
    CompletionClickAccept,
    MenuHighlighting,
    VisibleWhitespace,
    LineWrapMarker,
    Hint,
    MultilineIndent,
    Multiline,
    InlineHelp,
    StatusHints,
    StatusLine,
    StatusReporting,
    StatusLineCallback,
    MouseClicking,
    MouseClickingStatusLine,
    AutoTab,
    PromptNewline,
    RightPromptFollowCursor,
    AgentMode,
    Keybind,
    GenerateProfile,
    GenerateEnv,
    GenerateRc,
    GenerateLogout,
    SetHistoryMax,
    SetCompletionMax,
    Count
};

using SubcommandHandler = int (*)(const std::vector<std::string>& args);

struct CjshoptSubcommandDescriptor {
    CjshoptSubcommand command;
    const char* name;
    SubcommandHandler handler;
};

constexpr std::array<CjshoptSubcommandDescriptor, static_cast<size_t>(CjshoptSubcommand::Count)>
    kCjshoptSubcommandDescriptors = {
        {{CjshoptSubcommand::StyleDef, "style_def", style_def_command},
         {CjshoptSubcommand::CompletionCase, "completion-case", completion_case_command},
         {CjshoptSubcommand::HistorySearchCase, "history-search-case", history_search_case_command},
         {CjshoptSubcommand::HistoryDirectory, "history-directory", history_directory_command},
         {CjshoptSubcommand::HistoryDirectorySubdirs, "history-directory-subdirs",
          history_directory_subdirs_command},
         {CjshoptSubcommand::CompletionSpell, "completion-spell", completion_spell_command},
         {CjshoptSubcommand::CompletionSpellEnter, "completion-spell-enter",
          completion_spell_enter_command},
         {CjshoptSubcommand::CompletionLearning, "completion-learning",
          completion_learning_command},
         {CjshoptSubcommand::ExitConfirmation, "exit-confirmation", exit_confirmation_command},
         {CjshoptSubcommand::SmartCd, "smart-cd", smart_cd_command},
         {CjshoptSubcommand::Extglob, "extglob", extglob_command},
         {CjshoptSubcommand::ScriptExtensionInterpreter, "script-extension-interpreter",
          script_extension_interpreter_command},
         {CjshoptSubcommand::LineNumbers, "line-numbers", line_numbers_command},
         {CjshoptSubcommand::LineNumbersContinuation, "line-numbers-continuation",
          line_numbers_continuation_command},
         {CjshoptSubcommand::LineNumbersReplacePrompt, "line-numbers-replace-prompt",
          line_numbers_replace_prompt_command},
         {CjshoptSubcommand::CurrentLineNumberHighlight, "current-line-number-highlight",
          current_line_number_highlight_command},
         {CjshoptSubcommand::MultilineStartLines, "multiline-start-lines",
          multiline_start_lines_command},
         {CjshoptSubcommand::MultilineMaxLines, "multiline-max-lines", multiline_max_lines_command},
         {CjshoptSubcommand::MenuMaxLines, "menu-max-lines", menu_max_lines_command},
         {CjshoptSubcommand::MultilineBottomLines, "multiline-bottom-lines",
          multiline_bottom_lines_command},
         {CjshoptSubcommand::HintDelay, "hint-delay", hint_delay_command},
         {CjshoptSubcommand::IdleTimeout, "idle-timeout", idle_timeout_command},
         {CjshoptSubcommand::CompletionPreview, "completion-preview", completion_preview_command},
         {CjshoptSubcommand::CompletionAutoMenu, "completion-auto-menu",
          completion_auto_menu_command},
         {CjshoptSubcommand::CompletionClickAccept, "completion-click-accept",
          completion_click_accept_command},
         {CjshoptSubcommand::MenuHighlighting, "menu-highlighting", menu_highlighting_command},
         {CjshoptSubcommand::VisibleWhitespace, "visible-whitespace", visible_whitespace_command},
         {CjshoptSubcommand::LineWrapMarker, "line-wrap-marker", line_wrap_marker_command},
         {CjshoptSubcommand::Hint, "hint", hint_command},
         {CjshoptSubcommand::MultilineIndent, "multiline-indent", multiline_indent_command},
         {CjshoptSubcommand::Multiline, "multiline", multiline_command},
         {CjshoptSubcommand::InlineHelp, "inline-help", inline_help_command},
         {CjshoptSubcommand::StatusHints, "status-hints", status_hints_command},
         {CjshoptSubcommand::StatusLine, "status-line", status_line_command},
         {CjshoptSubcommand::StatusReporting, "status-reporting", status_reporting_command},
         {CjshoptSubcommand::StatusLineCallback, "status-line-callback",
          status_line_callback_command},
         {CjshoptSubcommand::MouseClicking, "mouse-clicking", mouse_clicking_command},
         {CjshoptSubcommand::MouseClickingStatusLine, "mouse-clicking-status-line",
          mouse_clicking_status_line_command},
         {CjshoptSubcommand::AutoTab, "auto-tab", auto_tab_command},
         {CjshoptSubcommand::PromptNewline, "prompt-newline", prompt_newline_command},
         {CjshoptSubcommand::RightPromptFollowCursor, "right-prompt-follow-cursor",
          right_prompt_follow_cursor_command},
         {CjshoptSubcommand::AgentMode, "agent-mode", agent_mode::command},
         {CjshoptSubcommand::Keybind, "keybind", keybind_command},
         {CjshoptSubcommand::GenerateProfile, "generate-profile", generate_profile_command},
         {CjshoptSubcommand::GenerateEnv, "generate-env", generate_env_command},
         {CjshoptSubcommand::GenerateRc, "generate-rc", generate_rc_command},
         {CjshoptSubcommand::GenerateLogout, "generate-logout", generate_logout_command},
         {CjshoptSubcommand::SetHistoryMax, "set-history-max", set_history_max_command},
         {CjshoptSubcommand::SetCompletionMax, "set-completion-max", set_completion_max_command}}};

std::optional<CjshoptSubcommandDescriptor> parse_cjshopt_subcommand(const std::string& subcommand) {
    for (const auto& descriptor : kCjshoptSubcommandDescriptors) {
        if (subcommand == descriptor.name) {
            return descriptor;
        }
    }
    return std::nullopt;
}

const std::vector<std::string>& cjshopt_usage_lines() {
    static const std::vector<std::string> kUsage = {
        "Usage: cjshopt <subcommand> [options]",
        "Available subcommands:",
        "  style_def <token_type> <style>   Define or redefine a syntax highlighting style",
        "  style_def preview|--reset        Preview current styles or reset defaults",
        std::string("  completion-case <on|off|status>  Configure completion case sensitivity ") +
            "(default: disabled)",
        std::string(
            "  history-search-case <on|off|status>  Configure fuzzy history case sensitivity ") +
            "(default: enabled)",
        "  history-directory <on|off|status>  Scope history to the current directory (default: "
        "disabled)",
        "  history-directory-subdirs <on|off|status>  Include nested directories (default: "
        "disabled)",
        std::string("  completion-spell <on|off|status> Configure completion spell correction ") +
            "(default: enabled)",
        std::string(
            "  completion-spell-enter <on|off|status> Auto-apply single spell corrections on ") +
            "Enter (default: disabled)",
        std::string("  smart-cd <on|off|status>         Configure smart cd auto-jumps ") +
            "(default: enabled)",
        std::string("  extglob <on|off|status>          Configure extended glob patterns ") +
            "(default: disabled)",
        std::string("  script-extension-interpreter <on|off|status> Configure extension-based ") +
            "script runners (default: enabled)",
        std::string("  completion-learning <on|off|status> Toggle automatic completion learning ") +
            "(default: enabled)",
        std::string("  exit-confirmation <smart|always|never|status> Control when exit requires ") +
            "confirmation (default: smart)",
        std::string("  line-numbers <on|off|relative|absolute|status>    Configure line numbers ") +
            "in multiline input (default: enabled)",
        "  line-numbers-continuation <on|off|status> Control line numbers when a continuation "
        "prompt is active",
        std::string(
            "  line-numbers-replace-prompt <on|off|status>       Replace the final prompt line ") +
            "with line numbers (default: disabled)",
        std::string(
            "  current-line-number-highlight <on|off|status>    Configure current line number ") +
            "highlighting (default: enabled)",
        std::string("  multiline-start-lines <count|status> Configure default multiline prompt ") +
            "height (default: 1)",
        std::string("  multiline-max-lines <count|status> Limit visible multiline input rows ") +
            "(default: 15)",
        "  menu-max-lines <count|status>    Limit visible menu content rows (default: 50)",
        std::string(
            "  multiline-bottom-lines <count|status> Set the input and menu scroll margin ") +
            "(default: 3)",
        "  hint-delay <milliseconds|status> Set or show the hint display delay",
        "  idle-timeout <seconds|off|status> Configure inactivity hooks (default: off)",
        std::string("  completion-preview <on|off|status> Configure completion preview ") +
            "(default: enabled)",
        "  completion-auto-menu <on|off|status> Show completions while typing; Tab activates (off)",
        std::string("  completion-click-accept <on|off|status> Control click-to-accept behavior ") +
            "for completion entries (default: disabled)",
        std::string("  menu-highlighting <none|single|all|reverse|status> Syntax-highlight ") +
            "completion and history menu items (default: none)",
        std::string("  visible-whitespace <on|off|status> Configure visible whitespace ") +
            "characters (default: disabled)",
        "  line-wrap-marker <marker|status> Set a single wrap character ('' to disable)",
        "  hint <on|off|status>            Configure inline hints (default: enabled)",
        std::string("  multiline-indent <on|off|status> Configure auto-indent in multiline ") +
            "(default: enabled)",
        "  multiline <on|off|status>       Configure multiline input (default: enabled)",
        std::string("  inline-help <on|off|status>     Configure inline help messages ") +
            "(default: enabled)",
        std::string(
            "  status-hints <off|normal|transient|persistent|status>  Control the default ") +
            "status hint banner (default: normal)",
        std::string("  status-line <on|off|status>    Hide or show the status area below the ") +
            "prompt (default: enabled)",
        std::string("  status-reporting <on|off|status>  Disable cjsh validation output while ") +
            "keeping status-hints (default: enabled)",
        std::string("  status-line-callback <function_name|off|status>  Run a shell function ") +
            "to publish custom status-line text",
        std::string(
            "  mouse-clicking <all-off|off|simple|smart|status>  Configure mouse capture ") +
            "behavior for new prompts and menus (default: off)",
        std::string(
            "  mouse-clicking-status-line <on|off|status>  Show or hide the mouse clicking ") +
            "status indicator (default: enabled)",
        std::string("  auto-tab <on|off|status>        Configure automatic tab completion ") +
            "(default: disabled)",
        std::string("  prompt-newline <on|off|status>  Add a newline after command execution ") +
            "(default: disabled)",
        std::string("  right-prompt-follow-cursor <on|off|status>  Re-anchor the inline right ") +
            "prompt to the cursor row (default: disabled)",
        "  agent-mode <subcommand> [...]  Configure agent-assisted command writing",
        "  keybind <subcommand> [...]       Inspect or modify key bindings",
        "    - Changes apply immediately; add the same command to ~/.cjshrc to persist",
        "    - Use 'cjshopt keybind ext' for custom command keybindings",
        "  generate-profile [-f|--force] [--alt]    Create or overwrite ~/.cjprofile",
        "  generate-env [-f|--force] [--alt]        Create or overwrite ~/.cjshenv",
        "  generate-rc [-f|--force] [--alt]         Create or overwrite ~/.cjshrc",
        "  generate-logout [-f|--force] [--alt]     Create or overwrite ~/.cjlogout",
        "  set-history-max <number|default|status> Configure history persistence",
        "  set-completion-max <number|default|status> Limit completion suggestions",
        "Use 'cjshopt <subcommand> --help' to see usage for a specific subcommand.",
    };
    return kUsage;
}

void print_cjshopt_usage() {
    for (const auto& line : cjshopt_usage_lines()) {
        std::cout << line << '\n';
    }
}

std::string available_subcommands_message() {
    std::string message = "Available subcommands: ";
    for (size_t i = 0; i < kCjshoptSubcommandDescriptors.size(); ++i) {
        if (i != 0) {
            message += ", ";
        }
        message += kCjshoptSubcommandDescriptors[i].name;
    }
    return message;
}
}  // namespace

int cjshopt_command(const std::vector<std::string>& args) {
    if (builtin_handle_help_with_startup_guard(args, {}, BuiltinHelpScanMode::FirstArgument)) {
        if (!cjsh_env::startup_active()) {
            print_cjshopt_usage();
        }
        return 0;
    }

    if (args.size() < 2) {
        print_error({ErrorType::INVALID_ARGUMENT, "cjshopt", "Missing subcommand argument",
                     cjshopt_usage_lines()});

        return 1;
    }

    const std::string& subcommand = args[1];
    auto descriptor = parse_cjshopt_subcommand(subcommand);
    if (descriptor.has_value()) {
        return descriptor->handler(std::vector<std::string>(args.begin() + 1, args.end()));
    }
    print_error({ErrorType::INVALID_ARGUMENT,
                 "cjshopt",
                 "unknown subcommand '" + subcommand + "'",
                 {available_subcommands_message()}});

    return 1;
}
