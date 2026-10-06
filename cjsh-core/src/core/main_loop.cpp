/*
  main_loop.cpp

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

#include "main_loop.h"
#include <sys/types.h>

#include <signal.h>
#include <unistd.h>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "isocline/isocline.h"
#include "keybindings.h"
#include "signal_handler.h"

#ifdef __APPLE__
#include <AvailabilityMacros.h>
#include <malloc/malloc.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif

#include "agent_mode.h"
#include "browser.h"
#include "cjsh_completions.h"
#include "cjsh_filesystem.h"
#include "cjsh_syntax_highlighter.h"
#include "cjshopt_command.h"
#include "error_out.h"
#include "exec.h"
#include "exit_command.h"
#include "interpreter.h"
#include "job_control.h"
#include "keycodes.h"
#include "parser.h"
#include "parser_utils.h"
#include "pipeline_status_utils.h"
#include "prompt.h"
#include "shell.h"
#include "shell_env.h"
#include "status_line.h"
#include "string_utils.h"
#include "trap_command.h"
#include "version_command.h"

std::chrono::steady_clock::time_point& startup_begin_time() {
    static std::chrono::steady_clock::time_point value;
    return value;
}

namespace {

bool last_prompt_started_with_newline = false;

void refresh_command_palette_entries();

bool parent_process_alive() {
    if (!config::interactive_mode) {
        return true;
    }

    const pid_t parent_pid = getppid();
    return parent_pid != 1 && (kill(parent_pid, 0) != -1 || errno != ESRCH);
}

bool typeahead_capture_allowed(void*) {
    return !JobManager::instance().foreground_job_reads_stdin();
}

void recover_prompt_terminal() {
    g_shell->recover_prompt_terminal();
}

struct CommandProcessResult {
    bool exit_requested;
    int exit_status;
};

std::string history_working_directory() {
    std::error_code ec;
    const auto directory = std::filesystem::current_path(ec);
    return ec ? std::string() : directory.string();
}

CommandProcessResult process_command_line(const std::string& command) {
    // this condition theoretically should never be hit due to earlier checks, but just in case
    if (command.empty()) {
        return {cjsh_env::exit_requested(), 0};
    }

    // tracking for exit commands
    cjsh_env::increment_command_sequence();

    // handle history expansion early before any tokenization or parsing
    std::string expanded_command = command;
    Parser* parser = (g_shell != nullptr) ? g_shell->get_parser() : nullptr;
    if (parser != nullptr) {
        auto expansion_result = parser->perform_history_expansion(command);
        if (expansion_result.has_error) {
            print_error({ErrorType::RUNTIME_ERROR,
                         ErrorSeverity::ERROR,
                         "history-expansion",
                         expansion_result.error_message,
                         {"Review your history expansion syntax or disable '!' expansions."}});
            pipeline_status_utils::set_last_status_env(1);
            return {cjsh_env::exit_requested(), 1};
        }

        if (expansion_result.was_expanded) {
            expanded_command = expansion_result.expanded_command;
            if (expansion_result.should_echo) {
                std::cout << expanded_command << '\n';
            }
        }
    }

    // execute preexec hooks and debug traps
    if (!config::is_posix_mode()) {
        g_shell->execute_hooks(HookType::Preexec, {expanded_command});
    }
    trap_manager_execute_debug_trap();

    // actually execute the command now
    const std::string command_directory = history_working_directory();
    const auto command_start_time = std::chrono::steady_clock::now();
    int exit_code = g_shell->execute(expanded_command);
    g_shell->set_last_interactive_command(expanded_command);
    const auto command_end_time = std::chrono::steady_clock::now();
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(command_end_time - command_start_time)
            .count();

    recover_prompt_terminal();

    // handle post command execution tasks
    Exec* exec_ptr = (g_shell && g_shell->shell_exec) ? g_shell->shell_exec.get() : nullptr;
    pipeline_status_utils::apply_execution_status_env(exit_code, exec_ptr);
    (void)cjsh_env::set_shell_variable_value("CJSH_COMMAND_DURATION_MS",
                                             std::to_string(static_cast<long long>(elapsed_ms)));

    // add to history
    if (config::history_enabled) {
        const std::string exit_code_str = std::to_string(exit_code);
        const std::string elapsed_ms_str = std::to_string(static_cast<long long>(elapsed_ms));
        const ic_history_metadata_t metadata[] = {
            {"timestamp", "0"},
            {"frequency", "0"},
            {"code", exit_code_str.c_str()},
            {"ms", elapsed_ms_str.c_str()},
            {"cwd", command_directory.c_str()},
        };
        ic_history_add_with_metadata(expanded_command.c_str(), metadata,
                                     sizeof(metadata) / sizeof(metadata[0]));
    }
    // Amortize allocator maintenance across commands instead of trimming after each builtin.
    static auto last_memory_cleanup = command_end_time;
    if (command_end_time - last_memory_cleanup >= std::chrono::seconds(5)) {
#if defined(__APPLE__) && MAC_OS_X_VERSION_MAX_ALLOWED >= 1070
        (void)malloc_zone_pressure_relief(nullptr, 0);
#elif defined(__GLIBC__)
        (void)malloc_trim(0);
#endif
        last_memory_cleanup = command_end_time;
    }

    return {cjsh_env::exit_requested(), exit_code};
}

void update_job_management(bool at_prompt = true) {
    SignalHandler::reap_pending_children(g_shell->shell_exec.get(), true);
    JobManager::instance().cleanup_finished_jobs(at_prompt);
}

void handle_readline_event(void*) {
    if (cjsh_env::exit_requested() || SignalHandler::has_pending_termination_signal()) {
        (void)ic_push_key_event(IC_KEY_EVENT_STOP);
        return;
    }
    if (SignalHandler::interrupt_pending()) {
        (void)ic_push_key_event(IC_KEY_EVENT_INTERRUPT);
        return;
    }
    // Poll children without executing signal traps inside the editor. Notifications
    // are copied into isocline's queue and printed after this callback returns.
    update_job_management(false);
}

std::string generate_prompt() {
    return prompt::render_primary_prompt();
}

struct ReadlinePromptState {
    std::string prompt_text;
    std::string inline_right_text;
};

ReadlinePromptState prepare_readline_prompt() {
    if (!config::is_posix_mode()) {
        prompt::execute_prompt_command();
        recover_prompt_terminal();
        prompt::apply_terminal_window_title();
    }
    (void)cjsh_env::update_terminal_dimensions();

    ReadlinePromptState state;
    state.prompt_text = generate_prompt();
    std::string prompt_eol_mark = prompt::render_prompt_eol_mark();
    ic_set_prompt_eol_mark(prompt_eol_mark.c_str());
    last_prompt_started_with_newline =
        (!state.prompt_text.empty() && state.prompt_text.front() == '\n');
    state.inline_right_text = prompt::render_right_prompt();

    std::string continuation_prompt = prompt::render_secondary_prompt();
    if (continuation_prompt.empty()) {
        ic_set_prompt_marker("", nullptr);
    } else {
        ic_set_prompt_marker("", continuation_prompt.c_str());
    }
    std::string history_search_prompt = prompt::render_history_search_prompt();
    std::string command_palette_prompt = prompt::render_command_palette_prompt();
    ic_set_history_search_prompt(history_search_prompt.c_str());
    ic_set_command_palette_prompt(command_palette_prompt.c_str());
    return state;
}

std::optional<std::string> get_next_command() {
    // main input getting
    std::string command_to_run;

    recover_prompt_terminal();

    // handle hooks
    if (!config::is_posix_mode()) {
        g_shell->execute_hooks(HookType::Precmd);
        recover_prompt_terminal();
    }

    thread_local static size_t consecutive_readline_errors = 0;
    std::string resume_input;
    size_t resume_cursor_pos = 0;
    bool resuming_after_idle = false;
    ReadlinePromptState prompt_state = prepare_readline_prompt();

    while (true) {
        // Prompt expansion and idle hooks may also execute external programs.
        recover_prompt_terminal();
        const char* inline_right_ptr = prompt_state.inline_right_text.empty()
                                           ? nullptr
                                           : prompt_state.inline_right_text.c_str();

        long idle_timeout_ms = 0;
        if (!config::is_posix_mode() && !config::secure_mode && config::idle_timeout_seconds > 0 &&
            !g_shell->get_hooks(HookType::Idle).empty()) {
            idle_timeout_ms = config::idle_timeout_seconds * 1000;
        }
        (void)ic_set_idle_timeout(idle_timeout_ms);

        refresh_command_palette_entries();
        cjsh_filesystem::reset_interactive_path_cache();
        (void)ic_set_history_directory(history_working_directory().c_str());
        prompt::set_prompt_refresh_allowed(true);
        ic_readline_result_t readline_result =
            resuming_after_idle ? ic_readline_with_status_at_cursor(
                                      prompt_state.prompt_text.c_str(), inline_right_ptr,
                                      resume_input.c_str(), resume_cursor_pos)
                                : ic_readline_with_status(prompt_state.prompt_text.c_str(),
                                                          inline_right_ptr, nullptr);
        prompt::set_prompt_refresh_allowed(false);
        ic_prepare_terminal_for_command();
        g_shell->mark_terminal_dirty();

        char* input = readline_result.input;
        if (readline_result.disposition == IC_READLINE_DISPOSITION_IDLE) {
            resume_input = (input == nullptr ? "" : input);
            resume_cursor_pos = readline_result.cursor_pos;
            resuming_after_idle = true;
            if (input != nullptr) {
                ic_free(input);
            }

            g_shell->execute_hooks(HookType::Idle);
            recover_prompt_terminal();
            if (cjsh_env::exit_requested()) {
                return std::nullopt;
            }
            update_job_management();
            continue;
        }

        if (readline_result.disposition == IC_READLINE_DISPOSITION_STOP ||
            (readline_result.disposition == IC_READLINE_DISPOSITION_ERROR &&
             readline_result.tty_lost)) {
            consecutive_readline_errors = 0;
            if (input != nullptr) {
                ic_free(input);
            }
            (void)g_shell->process_pending_signals();
            if (readline_result.tty_lost) {
                cjsh_env::request_exit();
            }
            return std::nullopt;
        }

        if (readline_result.disposition == IC_READLINE_DISPOSITION_EOF) {
            if (input != nullptr) {
                ic_free(input);
            }
            if (g_shell->get_shell_option(ShellOption::Ignoreeof)) {
                std::cerr << "Use 'exit' to leave the shell.\n";
                continue;
            }
            cjsh_env::increment_command_sequence();
            (void)exit_command({"exit"});
            return std::nullopt;
        }

        // handle empty buffer and exit early
        if (input == nullptr) {
            if (readline_result.disposition == IC_READLINE_DISPOSITION_ERROR) {
                consecutive_readline_errors += 1;
            } else {
                consecutive_readline_errors = 0;
            }

            if (consecutive_readline_errors >= 2) {
                cjsh_env::request_exit();
            }

            return std::nullopt;
        }

        consecutive_readline_errors = 0;

        // there was actual input, assign it to command to run
        (void)command_to_run.assign(input);
        ic_free(input);

        if (readline_result.disposition == IC_READLINE_DISPOSITION_INTERRUPT) {
            return std::nullopt;
        }

        // if none early exit hits then we actually have a command to run
        return command_to_run;
    }
}

bool execute_custom_editor_command(const std::string& command) {
    if (command.empty() || g_shell == nullptr) {
        return false;
    }

    const char* buffer = ic_get_buffer();
    size_t cursor_pos = 0;
    (void)ic_get_cursor_pos(&cursor_pos);

    std::string original_buffer = buffer ? buffer : "";

    (void)cjsh_env::set_shell_variable_value("CJSH_LINE", original_buffer);
    (void)cjsh_env::set_shell_variable_value("CJSH_POINT", std::to_string(cursor_pos));

    const bool terminal_suspended = ic_suspend_readline_terminal();
    (void)g_shell->execute(command);
    if (terminal_suspended) {
        (void)ic_resume_readline_terminal();
    }

    if (cjsh_env::shell_variable_is_set("CJSH_LINE")) {
        std::string new_buffer_env = cjsh_env::get_shell_variable_value("CJSH_LINE");
        if (original_buffer != new_buffer_env) {
            (void)ic_set_buffer(new_buffer_env.c_str());
        }
    }

    if (cjsh_env::shell_variable_is_set("CJSH_POINT")) {
        std::string new_point_env = cjsh_env::get_shell_variable_value("CJSH_POINT");
        char* endptr;
        long new_pos = strtol(new_point_env.c_str(), &endptr, 10);
        if (endptr != new_point_env.c_str() && new_pos >= 0 &&
            static_cast<size_t>(new_pos) != cursor_pos) {
            (void)ic_set_cursor_pos((size_t)new_pos);
        }
    }

    (void)cjsh_env::unset_shell_variable_value("CJSH_LINE");
    (void)cjsh_env::unset_shell_variable_value("CJSH_POINT");
    return true;
}

bool execute_custom_keybinding_command(ic_keycode_t key) {
    if (!has_custom_keybinding(key)) {
        return false;
    }
    return execute_custom_editor_command(get_custom_keybinding(key));
}

bool execute_custom_palette_command(const std::string& palette_id) {
    if (palette_id.empty() || !has_custom_palette_command(palette_id)) {
        return false;
    }
    return execute_custom_editor_command(get_custom_palette_command(palette_id));
}

bool handle_command_palette_entry(const ic_command_palette_entry_t* entry, void*) {
    if (entry == nullptr || entry->id == nullptr || entry->id[0] == '\0') {
        return false;
    }

    constexpr const char* kExtKeyPrefix = "ext-key:";
    constexpr const char* kExtPalettePrefix = "ext-cmd:";
    std::string id(entry->id);

    if (id == "agent-mode") {
        return agent_mode::handle_palette_entry();
    }
    if (id == "browser") {
        return browser::open_buffer();
    }

    if (id.rfind(kExtKeyPrefix, 0) == 0) {
        std::string key_spec = id;
        (void)key_spec.erase(0, std::strlen(kExtKeyPrefix));
        ic_keycode_t key = IC_KEY_NONE;
        if (!ic_parse_key_spec(key_spec.c_str(), &key)) {
            return false;
        }
        return execute_custom_keybinding_command(key);
    }

    if (id.rfind(kExtPalettePrefix, 0) == 0) {
        std::string palette_id = id;
        (void)palette_id.erase(0, std::strlen(kExtPalettePrefix));
        return execute_custom_palette_command(palette_id);
    }

    return false;
}

void refresh_command_palette_entries() {
    static std::optional<std::pair<std::uint64_t, bool>> installed_revision;
    const auto revision =
        std::make_pair(custom_command_bindings_revision(), agent_mode::palette_entry_enabled());
    if (installed_revision == revision) {
        return;
    }
    auto custom_bindings = list_custom_keybindings();
    auto palette_bindings = list_custom_palette_commands();
    const bool show_agent_entry = revision.second;

    std::vector<std::string> ids;
    std::vector<std::string> names;
    std::vector<std::string> descriptions;
    std::vector<std::string> keywords;
    const size_t total_entries =
        custom_bindings.size() + palette_bindings.size() + (show_agent_entry ? 1 : 0) + 1;
    ids.reserve(total_entries);
    names.reserve(total_entries);
    descriptions.reserve(total_entries);
    keywords.reserve(total_entries);

    constexpr size_t kMaxPreview = 48;

    if (show_agent_entry) {
        (void)ids.emplace_back("agent-mode");
        (void)names.emplace_back("Ask agent");
        (void)descriptions.emplace_back("Answer a question or write a CJSH command");
        (void)keywords.emplace_back(
            "agent ai assistant natural language command writing question answer help");
    }

    (void)ids.emplace_back("browser");
    (void)names.emplace_back("Open buffer in browser");
    (void)descriptions.emplace_back("Search the web or open the current URL");
    (void)keywords.emplace_back("browser web search url open buffer");

    for (const auto& [key, binding] : custom_bindings) {
        char key_spec_buffer[64];
        if (!ic_format_key_spec(key, key_spec_buffer, sizeof(key_spec_buffer))) {
            continue;
        }

        std::string command_preview = binding.command;
        if (command_preview.size() > kMaxPreview) {
            command_preview = command_preview.substr(0, kMaxPreview - 3) + "...";
        }

        std::string title = binding.title.empty() ? command_preview : binding.title;

        (void)ids.emplace_back(std::string("ext-key:") + key_spec_buffer);
        (void)names.emplace_back(title);
        (void)descriptions.emplace_back(std::string("[") + key_spec_buffer + "] (custom)");
        (void)keywords.emplace_back(std::string("custom keybinding snippet widget command ") +
                                    key_spec_buffer + " " + title + " " + binding.command);
    }

    for (const auto& [id, binding] : palette_bindings) {
        std::string command_preview = binding.command;
        if (command_preview.size() > kMaxPreview) {
            command_preview = command_preview.substr(0, kMaxPreview - 3) + "...";
        }

        std::string title = binding.title.empty() ? id : binding.title;

        (void)ids.emplace_back(std::string("ext-cmd:") + id);
        (void)names.emplace_back(title);
        (void)descriptions.emplace_back("");
        keywords.emplace_back("palette snippet custom command ");
        keywords.back().append(id).append(" ").append(title).append(" ").append(command_preview);
    }

    std::vector<ic_command_palette_entry_t> entries(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        entries[i].id = ids[i].c_str();
        entries[i].name = names[i].c_str();
        entries[i].description = descriptions[i].c_str();
        entries[i].keywords = keywords[i].c_str();
    }

    if (ic_set_command_palette_entries(entries.data(), entries.size())) {
        installed_revision = revision;
    } else {
        ic_clear_command_palette_entries();
        installed_revision.reset();
    }
}

bool handle_runoff_bind(ic_keycode_t key, void*) {
    // handle custom keybindings from the user
    if (key == IC_KEY_EVENT_PROMPT_REFRESH) {
        return prompt::handle_async_prompt_refresh();
    }

    if (has_custom_keybinding(key)) {
        return execute_custom_keybinding_command(key);
    }

    return agent_mode::handle_runoff_key(key) || browser::handle_runoff_key(key);
}

bool should_show_creator_line() {
    // only used during startup for the title line if you want to see the creator line
    if (!cjsh_env::shell_variable_is_set("CJSH_SHOW_CREATED")) {
        return false;
    }

    std::string value = cjsh_env::get_shell_variable_value("CJSH_SHOW_CREATED");
    if (value.empty()) {
        return false;
    }
    value = string_utils::to_lower_copy(value);

    (void)cjsh_env::unset_shell_variable_value("CJSH_SHOW_CREATED");

    return value == "1" || value == "true" || value == "yes" || value == "on";
}

bool buffer_has_line_continuation_suffix(const std::string& buffer) {
    return has_line_continuation_suffix(buffer, true);
}

bool buffer_requires_additional_input(const std::string& buffer) {
    // does input need continuation
    if (buffer.empty()) {
        return false;
    }

    if (buffer_has_line_continuation_suffix(buffer)) {
        return true;
    }

    if (g_shell == nullptr) {
        return false;
    }

    Parser* parser = g_shell->get_parser();
    ShellScriptInterpreter* interpreter = g_shell->get_shell_script_interpreter();
    if (parser == nullptr || interpreter == nullptr) {
        return false;
    }

    // Validation can parse function bodies and replace the parser's cache.
    const auto lines = parser->prepare_interactive_input(buffer);
    if (lines.empty()) {
        return false;
    }

    return interpreter->needs_additional_input(lines);
}

bool continuation_or_return_callback(const char* input_buffer, void*) {
    // handle if input buffer needs continuation
    if (input_buffer == nullptr) {
        return true;
    }

    std::string buffer(input_buffer);
    bool should_submit = !buffer_requires_additional_input(buffer);
    if (should_submit) {
        (void)prompt::apply_transient_final_prompt_if_configured();
    }
    return should_submit;
}

}  // namespace

void initialize_isocline() {
    // setup isocline environment and ui styling
    // Defer editor initialization until after the shell owns the terminal.
    if (config::minimal_mode) {
        (void)ic_enable_line_numbers(false);
    }
    initialize_completion_system();
    SyntaxHighlighter::initialize_syntax_highlighting();
    (void)ic_enable_history_duplicates(false);
    (void)ic_enable_history_auto_add(false);
    (void)ic_enable_multiline_continuation_retention(true);
    ic_set_prompt_marker("", nullptr);
    ic_set_unhandled_key_handler(handle_runoff_bind, nullptr);
    ic_set_readline_event_callback(handle_readline_event, nullptr);
    ic_set_command_palette_entry_handler(handle_command_palette_entry, nullptr);
    refresh_command_palette_entries();
    (void)ic_bind_key(IC_KEY_EVENT_PROMPT_REFRESH, IC_KEY_ACTION_RUNOFF);
    agent_mode::apply_key_bindings();
    browser::apply_key_bindings();
    ic_set_status_message_callback(status_line::create_below_syntax_message, nullptr);
    ic_set_check_for_continuation_or_return_callback(continuation_or_return_callback, nullptr);
    ic_set_typeahead_capture_allowed_callback(typeahead_capture_allowed, nullptr);
    (void)ic_enable_terminal_region_marking(true);
    if (!config::status_line_enabled) {
        (void)ic_set_status_hint_mode(IC_STATUS_HINT_OFF);
    }
}

void main_process_loop() {
    // enable isocline-owned typeahead capture for this interactive loop
    (void)ic_enable_typeahead(true);

    std::string command_to_run;
    // main input loop, runs until exit
    while (true) {
        // handle any pending signals before each prompt
        (void)g_shell->process_pending_signals();

        if (cjsh_env::exit_requested()) {
            break;
        }

        // if our parent is gone, exit cleanly
        if (!parent_process_alive()) {
            cjsh_env::request_exit();
            break;
        }

        // check job statuses
        update_job_management();

        // fetch the next command from the user
        std::optional<std::string> next_command = get_next_command();

        if (cjsh_env::exit_requested()) {
            break;
        }

        if (!next_command.has_value()) {
            continue;
        }

        command_to_run = std::move(*next_command);

        // handle the command from the user
        ic_mark_command_start();
        CommandProcessResult command_result = process_command_line(command_to_run);

        // handle styling configuration
        if (!command_result.exit_requested && !cjsh_env::exit_requested() &&
            config::newline_after_execution && command_to_run != "clear" &&
            !last_prompt_started_with_newline) {
            (void)std::fputc('\n', stdout);
            (void)std::fflush(stdout);
        }

        ic_mark_command_finished(command_result.exit_status);
        if (command_result.exit_requested || cjsh_env::exit_requested()) {
            break;
        }
    }

    (void)ic_enable_typeahead(false);
}

void start_interactive_process() {
    g_shell->begin_interactive_input();
    // activate the line editor
    initialize_isocline();
    (void)g_shell->process_pending_signals();
    if (SignalHandler::startup_interrupted()) {
        pipeline_status_utils::set_last_status_env(128 + SIGINT);
    }
    cjsh_env::set_startup_active(false);
    bool first_boot = cjsh_filesystem::is_first_boot();

    // calculate startup time
    std::chrono::microseconds startup_duration(0);
    if (config::show_startup_time || first_boot) {
        auto startup_end_time = std::chrono::steady_clock::now();
        startup_duration = std::chrono::duration_cast<std::chrono::microseconds>(
            startup_end_time - startup_begin_time());
    }

    // display title line and creator line if configured
    if (config::show_title_line) {
        const bool show_creator_line = should_show_creator_line();
        std::cout << " CJ's Shell v" << get_version() << " - Caden J Finley (c) 2026" << '\n';
        if (show_creator_line) {
            // cjsh first started as part of an undergrad project at my alma mater, ACU ( abilene
            // christian univeristy ), to create some shell paradigms and shell/ gnu builtins, and
            // eventually a full shell project and i fell in love with the project. That is the
            // reason for this line. I wanted to give the school credit that helped me fall in love
            // with my main project. Most people couldn't care less which is why this is guareded
            // behind a hidden option. But for those who do care like myself the option is still
            // there to have this line appear in the title line during startup.
            std::cout << " Created 2024 @ \033[1;35mAbilene Christian University\033[0m" << '\n';
        }
        std::cout << "\n";
    }

    // display first boot message if this is the first time cjsh has been run
    if (first_boot) {
        std::cout << " Be sure to give us a star on GitHub!" << '\n';
        std::cout << " Type 'help' to see available commands and options." << '\n';
        std::cout << " For additional help and documentation, please visit: "
                  << " https://cadenfinley.github.io/cjsh/" << '\n';
        std::cout << '\n';

        std::cout << " To suppress this help message run the command: 'firstboot'" << '\n';
        std::cout << " To suppress the title line, launch cjsh with: '--no-titleline'" << '\n';
        std::cout << " You can find many more toggles like this to fully customize your cjsh "
                     "experience with: 'cjshopt --help'\n";
        std::cout << '\n';
        std::cout << " cjsh uses a very complex, but very smart completions system.\n";
        std::cout << " During shell use it learns about the commands you use and provides better "
                     "completions as you use cjsh.\n";
        std::cout << " If you would like to skip the learning process and make all completions "
                     "faster please see: 'generate-completions --help'\n";
        std::cout << "\n";
    }

    // calculate the display for startup time
    if (config::show_startup_time || first_boot) {
        long long microseconds = startup_duration.count();
        std::string startup_time_str;
        if (microseconds < 1000) {
            startup_time_str = std::to_string(microseconds) + "μs";
        } else if (microseconds < 1000000) {
            double milliseconds = static_cast<double>(microseconds) / 1000.0;
            char buffer[32];
            (void)snprintf(buffer, sizeof(buffer), "%.2fms", milliseconds);
            startup_time_str = buffer;
        } else {
            double seconds = static_cast<double>(microseconds) / 1000000.0;
            char buffer[32];
            (void)snprintf(buffer, sizeof(buffer), "%.2fs", seconds);
            startup_time_str = buffer;
        }
        std::cout << " Started in " << startup_time_str << '\n';
        std::cout << "\n";
    }

    // go into read input loop
    main_process_loop();
}
