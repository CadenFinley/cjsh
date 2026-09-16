/*
  cjshopt_command.h

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

#ifndef CJSH_CORE_SRC_BUILTIN_CJSHOPT_CJSHOPT_COMMAND_H
#define CJSH_CORE_SRC_BUILTIN_CJSHOPT_CJSHOPT_COMMAND_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using ic_keycode_t = unsigned int;

struct custom_command_binding_t {
    std::string title;
    std::string command;
};

int cjshopt_command(const std::vector<std::string>& args);
int keybind_ext_command(const std::vector<std::string>& args);
int completion_case_command(const std::vector<std::string>& args);
int history_search_case_command(const std::vector<std::string>& args);
int history_directory_command(const std::vector<std::string>& args);
int history_directory_subdirs_command(const std::vector<std::string>& args);
int completion_spell_command(const std::vector<std::string>& args);
int completion_spell_enter_command(const std::vector<std::string>& args);
int completion_learning_command(const std::vector<std::string>& args);
int exit_confirmation_command(const std::vector<std::string>& args);
int smart_cd_command(const std::vector<std::string>& args);
int extglob_command(const std::vector<std::string>& args);
int script_extension_interpreter_command(const std::vector<std::string>& args);
int line_numbers_command(const std::vector<std::string>& args);
int line_numbers_continuation_command(const std::vector<std::string>& args);
int line_numbers_replace_prompt_command(const std::vector<std::string>& args);
int current_line_number_highlight_command(const std::vector<std::string>& args);
int hint_delay_command(const std::vector<std::string>& args);
int idle_timeout_command(const std::vector<std::string>& args);
int completion_preview_command(const std::vector<std::string>& args);
int completion_click_accept_command(const std::vector<std::string>& args);
int menu_highlighting_command(const std::vector<std::string>& args);
int visible_whitespace_command(const std::vector<std::string>& args);
int line_wrap_marker_command(const std::vector<std::string>& args);
int hint_command(const std::vector<std::string>& args);
int multiline_indent_command(const std::vector<std::string>& args);
int multiline_command(const std::vector<std::string>& args);
int multiline_start_lines_command(const std::vector<std::string>& args);
int multiline_max_lines_command(const std::vector<std::string>& args);
int menu_max_lines_command(const std::vector<std::string>& args);
int multiline_bottom_lines_command(const std::vector<std::string>& args);
int inline_help_command(const std::vector<std::string>& args);
int status_hints_command(const std::vector<std::string>& args);
int status_line_command(const std::vector<std::string>& args);
int status_reporting_command(const std::vector<std::string>& args);
int status_line_callback_command(const std::vector<std::string>& args);
int mouse_clicking_command(const std::vector<std::string>& args);
int mouse_clicking_status_line_command(const std::vector<std::string>& args);
int auto_tab_command(const std::vector<std::string>& args);
int prompt_newline_command(const std::vector<std::string>& args);
int right_prompt_follow_cursor_command(const std::vector<std::string>& args);
int style_def_command(const std::vector<std::string>& args);
void apply_custom_style(const std::string& token_type, const std::string& style);
int generate_profile_command(const std::vector<std::string>& args);
int generate_env_command(const std::vector<std::string>& args);
int generate_rc_command(const std::vector<std::string>& args);
int generate_logout_command(const std::vector<std::string>& args);
int keybind_command(const std::vector<std::string>& args);
int set_history_max_command(const std::vector<std::string>& args);
int set_completion_max_command(const std::vector<std::string>& args);
std::string get_custom_keybinding(ic_keycode_t key);
std::string get_custom_keybinding_title(ic_keycode_t key);
bool has_custom_keybinding(ic_keycode_t key);
void set_custom_keybinding(ic_keycode_t key, const std::string& command,
                           const std::string& title = "");
void clear_custom_keybinding(ic_keycode_t key);
void clear_all_custom_keybindings();
std::vector<std::pair<ic_keycode_t, custom_command_binding_t>> list_custom_keybindings();
std::string get_custom_palette_command(const std::string& id);
std::string get_custom_palette_command_title(const std::string& id);
bool has_custom_palette_command(const std::string& id);
void set_custom_palette_command(const std::string& id, const std::string& command,
                                const std::string& title = "");
void clear_custom_palette_command(const std::string& id);
void clear_all_custom_palette_commands();
std::vector<std::pair<std::string, custom_command_binding_t>> list_custom_palette_commands();
std::uint64_t custom_command_bindings_revision();

#endif  // CJSH_CORE_SRC_BUILTIN_CJSHOPT_CJSHOPT_COMMAND_H
