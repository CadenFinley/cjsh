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
    HistoryDirectoryParents,
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
    CompletionMenuMaxLines,
    HistoryMenuMaxLines,
    CommandPaletteMaxLines,
    CustomMenuMaxLines,
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
         {CjshoptSubcommand::HistoryDirectoryParents, "history-directory-parents",
          history_directory_parents_command},
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
         {CjshoptSubcommand::CompletionMenuMaxLines, "completion-menu-max-lines",
          completion_menu_max_lines_command},
         {CjshoptSubcommand::HistoryMenuMaxLines, "history-menu-max-lines",
          history_menu_max_lines_command},
         {CjshoptSubcommand::CommandPaletteMaxLines, "command-palette-max-lines",
          command_palette_max_lines_command},
         {CjshoptSubcommand::CustomMenuMaxLines, "custom-menu-max-lines",
          custom_menu_max_lines_command},
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
        "",
        "Configure shell behavior and interactive editing.",
        "Use 'cjshopt <subcommand> --help' for details and examples.",
        "",
        "Completion and hints:",
        "  completion-auto-menu <on|off|status>",
        "    Show completions while typing; Tab activates the menu (default: off).",
        "  auto-tab <on|off|status>",
        "    Automatically start tab completion (default: off).",
        "  completion-preview <on|off|status>",
        "    Preview the selected completion (default: on).",
        "  completion-case <on|off|status>",
        "    Match completions case-sensitively (default: off).",
        "  completion-spell <on|off|status>",
        "    Suggest spelling corrections in completions (default: on).",
        "  completion-spell-enter <on|off|status>",
        "    Apply a single spelling correction on Enter (default: off).",
        "  completion-learning <on|off|status>",
        "    Learn completions automatically from man pages (default: on).",
        "  set-completion-max <number|default|status>",
        "    Limit the number of completion suggestions.",
        "  hint <on|off|status>",
        "    Show inline completion hints (default: on).",
        "  hint-delay <milliseconds|status>",
        "    Set or show the delay before inline hints appear.",
        "",
        "History:",
        "  history-directory <on|off|status>",
        "    Scope interactive history to the current directory (default: off).",
        "  history-directory-subdirs <on|off|status>",
        "    Include nested directories when directory scope is on (default: off).",
        "  history-directory-parents <on|off|status>",
        "    Include all ancestor directories when scope is on (default: off).",
        "  history-search-case <on|off|status>",
        "    Match fuzzy history searches case-sensitively (default: on).",
        "  set-history-max <number|default|status>",
        "    Configure history persistence limits.",
        "",
        "Menus:",
        "  completion-menu-max-lines <count|status>",
        "    Limit completion menu content rows (default: 15).",
        "  history-menu-max-lines <count|status>",
        "    Limit history menu content rows (default: 15).",
        "  command-palette-max-lines <count|status>",
        "    Limit command palette content rows (default: 15).",
        "  custom-menu-max-lines <count|status>",
        "    Limit custom menu content rows (default: 15).",
        "  menu-highlighting <none|single|all|reverse|status>",
        "    Syntax-highlight completion and history menu items (default: none).",
        "",
        "Prompt and multiline input:",
        "  multiline <on|off|status>",
        "    Enable multiline input (default: on).",
        "  multiline-indent <on|off|status>",
        "    Automatically indent multiline input (default: on).",
        "  multiline-start-lines <count|status>",
        "    Set the initial multiline prompt height (default: 1).",
        "  multiline-max-lines <count|status>",
        "    Limit visible multiline input rows (default: 15).",
        "  multiline-bottom-lines <count|status>",
        "    Set the input and menu scroll margin (default: 3).",
        "  prompt-newline <on|off|status>",
        "    Add a newline after command execution (default: off).",
        "  right-prompt-follow-cursor <on|off|status>",
        "    Keep the inline right prompt on the cursor row (default: off).",
        "",
        "Appearance:",
        "  style_def <token_type> <style>",
        "    Define or redefine a syntax highlighting style.",
        "  style_def preview|--reset",
        "    Preview current styles or reset defaults.",
        "  line-numbers <on|off|relative|absolute|status>",
        "    Show line numbers in multiline input (default: on, absolute).",
        "  line-numbers-continuation <on|off|status>",
        "    Keep line numbers when a continuation prompt is active.",
        "  line-numbers-replace-prompt <on|off|status>",
        "    Replace the final prompt line with line numbers (default: off).",
        "  current-line-number-highlight <on|off|status>",
        "    Highlight the current line number (default: on).",
        "  visible-whitespace <on|off|status>",
        "    Show whitespace characters in the editor (default: off).",
        "  line-wrap-marker <marker|status>",
        "    Set a single wrap character; use '' to disable it.",
        "",
        "Status and help:",
        "  status-line <on|off|status>",
        "    Show the status area below the prompt (default: on).",
        "  status-hints <off|normal|transient|persistent|status>",
        "    Control the default status hint banner (default: normal).",
        "  status-reporting <on|off|status>",
        "    Show command validation messages in the status area (default: on).",
        "  status-line-callback <function_name|off|status>",
        "    Run a shell function to supply custom status-line text.",
        "  inline-help <on|off|status>",
        "    Show inline help messages (default: on).",
        "",
        "Keyboard and mouse:",
        "  keybind <subcommand> [...]",
        "    Inspect or modify key bindings; changes apply immediately.",
        "  mouse-clicking <all-off|off|simple|smart|status>",
        "    Configure mouse capture for prompts and menus (default: off).",
        "  mouse-clicking-status-line <on|off|status>",
        "    Show the mouse-clicking status indicator (default: on).",
        "  completion-click-accept <on|off|status>",
        "    Accept completion entries when clicked (default: off).",
        "",
        "Shell behavior and agent mode:",
        "  smart-cd <on|off|status>",
        "    Enable smart cd auto-jumps (default: on).",
        "  extglob <on|off|status>",
        "    Enable extended glob patterns (default: off).",
        "  script-extension-interpreter <on|off|status>",
        "    Infer script runners from file extensions (default: on).",
        "  exit-confirmation <smart|always|never|status>",
        "    Control when exit requires confirmation (default: smart).",
        "  idle-timeout <seconds|off|status>",
        "    Run inactivity hooks after the specified delay (default: off).",
        "  agent-mode <subcommand> [...]",
        "    Configure agent-assisted command writing.",
        "",
        "Startup files:",
        "  generate-env [-f|--force] [--alt]",
        "    Generate ~/.cjshenv.",
        "  generate-profile [-f|--force] [--alt]",
        "    Generate ~/.cjprofile.",
        "  generate-rc [-f|--force] [--alt]",
        "    Generate ~/.cjshrc.",
        "  generate-logout [-f|--force] [--alt]",
        "    Generate ~/.cjlogout.",
        "",
        "Add settings to your startup files (usually ~/.cjshrc) to persist them.",
        "Use 'cjshopt keybind ext --help' for custom command keybindings.",
        "For startup file generators, --force overwrites and --alt uses ~/.config/cjsh.",
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
