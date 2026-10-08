/*
  browser.cpp

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

#include "browser.h"

#include <string>
#include <string_view>
#include <vector>

#include "cjshopt_command.h"
#include "error_out.h"
#include "isocline.h"
#include "keybindings.h"
#include "keycodes.h"
#include "prompt.h"
#include "shell.h"
#include "shell_env.h"
#include "string_utils.h"

namespace browser {
namespace {

constexpr ic_keycode_t kBrowserKey = IC_KEY_WITH_ALT('o');
constexpr const char* kDefaultSearchUrl = "https://www.google.com/search?q=";

std::string encode_query(std::string_view query) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (unsigned char ch : query) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded += static_cast<char>(ch);
        } else {
            encoded += '%';
            encoded += hex[ch >> 4];
            encoded += hex[ch & 0x0F];
        }
    }
    return encoded;
}

std::string browser_url(const std::string& input) {
    if (input.find_first_of(" \t\r\n") == std::string::npos) {
        if (string_utils::starts_with_case_insensitive(input, "https://") ||
            string_utils::starts_with_case_insensitive(input, "http://") ||
            string_utils::starts_with_case_insensitive(input, "file://")) {
            return input;
        }
        if (string_utils::starts_with_case_insensitive(input, "www.")) {
            return "https://" + input;
        }
    }
    std::string search_url = cjsh_env::get_shell_variable_value("CJSH_BROWSER_SEARCH_URL");
    if (search_url.empty()) {
        search_url = kDefaultSearchUrl;
    }
    return search_url + encode_query(input);
}

}  // namespace

void apply_key_bindings() {
    ic_key_action_t current = IC_KEY_ACTION_NONE;
    if (!has_custom_keybinding(kBrowserKey) && !ic_get_key_binding(kBrowserKey, &current)) {
        (void)ic_bind_key(kBrowserKey, IC_KEY_ACTION_RUNOFF);
    }
}

bool handle_runoff_key(ic_keycode_t key) {
    ic_key_action_t current = IC_KEY_ACTION_NONE;
    return key == kBrowserKey && !has_custom_keybinding(key) && ic_get_key_binding(key, &current) &&
           current == IC_KEY_ACTION_RUNOFF && open_buffer();
}

bool open_buffer() {
    const char* raw_buffer = ic_get_buffer();
    if (raw_buffer == nullptr || shell == nullptr) {
        return false;
    }
    const std::string input = string_utils::trim_ascii_whitespace_copy(raw_buffer);
    if (input.empty()) {
        return true;
    }

    std::string browser_command = cjsh_env::get_shell_variable_value("BROWSER");
    if (browser_command.empty()) {
#ifdef __APPLE__
        browser_command = "open";
#else
        browser_command = "xdg-open";
#endif
    }
    std::vector<std::string> args = cjsh_env::parse_shell_command(browser_command);
    if (!prompt::advance_with_transient_final_prompt("")) {
        return false;
    }
    const bool terminal_suspended = ic_suspend_readline_terminal();
    if (args.empty() || args.front().empty()) {
        print_error({ErrorType::INVALID_ARGUMENT,
                     "browser",
                     "BROWSER contains no command",
                     {"Set BROWSER to a browser executable and optional arguments."}});
    } else {
        args.push_back(browser_url(input));
        const int exit_code = shell->execute_command(args, false);
        if (exit_code != 0) {
            print_error({ErrorType::RUNTIME_ERROR,
                         "browser",
                         "Browser command exited with status " + std::to_string(exit_code),
                         {"Set BROWSER to a browser executable and optional arguments."}});
        }
    }
    if (terminal_suspended) {
        (void)ic_resume_readline_terminal();
    }
    return true;
}

}  // namespace browser
