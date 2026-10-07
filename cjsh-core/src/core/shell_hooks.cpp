/*
  shell_hooks.cpp

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

#include "shell.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "interpreter.h"
#include "numeric_utils.h"
#include "pipeline_status_utils.h"
#include "shell_env.h"

// keep native hook registration and dispatch separate from signal traps and exit
// handlers. callers choose when a hook type applies; this layer preserves command
// status while invoking the registered entries in order.
namespace {

constexpr size_t to_index(HookType type) {
    return static_cast<size_t>(type);
}

constexpr std::array<HookTypeDescriptor, static_cast<size_t>(HookType::Count)>
    kHookTypeDescriptors = {{{HookType::Precmd, "precmd"},
                             {HookType::Preexec, "preexec"},
                             {HookType::Chpwd, "chpwd"},
                             {HookType::Idle, "idle"}}};

}  // namespace

const std::array<HookTypeDescriptor, static_cast<size_t>(HookType::Count)>&
get_hook_type_descriptors() {
    return kHookTypeDescriptors;
}

std::optional<HookType> parse_hook_type(const std::string& name) {
    for (const auto& descriptor : kHookTypeDescriptors) {
        if (name == descriptor.name) {
            return descriptor.type;
        }
    }
    return std::nullopt;
}

// preserve registration order and ignore duplicate entries within a hook type.
// names are resolved at dispatch time, so registration need not follow definition.
void Shell::register_hook(HookType hook_type, const std::string& function_name) {
    if (function_name.empty()) {
        return;
    }

    auto& hook_list = hooks[to_index(hook_type)];

    if (std::find(hook_list.begin(), hook_list.end(), function_name) == hook_list.end()) {
        hook_list.push_back(function_name);
    }
}

void Shell::unregister_hook(HookType hook_type, const std::string& function_name) {
    auto& hook_list = hooks[to_index(hook_type)];
    (void)hook_list.erase(std::remove(hook_list.begin(), hook_list.end(), function_name),
                          hook_list.end());
}

// return a snapshot for callers inspecting hooks, not a mutable registration list.
std::vector<std::string> Shell::get_hooks(HookType hook_type) const {
    return hooks[to_index(hook_type)];
}

void Shell::clear_hooks(HookType hook_type) {
    hooks[to_index(hook_type)].clear();
}

// hook return values must not replace the command status seen by the next hook
// or prompt. other shell side effects, including exit requests, are not rolled back.
void Shell::execute_hooks(HookType hook_type, const std::vector<std::string>& arguments) {
    const auto& hook_list = hooks[to_index(hook_type)];
    if (hook_list.empty()) {
        return;
    }

    // preserve whether PIPESTATUS existed as well as its value; restoring an empty
    // binding would not be equivalent to leaving an originally unset variable unset.
    const std::string saved_status = cjsh_env::get_shell_variable_value("?");
    const int saved_status_code = numeric_utils::parse_exit_status_or(saved_status, 0, false);
    const bool had_pipe_status = cjsh_env::shell_variable_is_set("PIPESTATUS");
    const std::string saved_pipe_status = cjsh_env::get_shell_variable_value("PIPESTATUS");

    // reset status before every entry so each sees the triggering command's result,
    // not the return value of the preceding hook.
    for (const auto& function_name : hook_list) {
        pipeline_status_utils::set_last_status_env(saved_status_code);
        if (had_pipe_status) {
            (void)cjsh_env::set_shell_variable_value("PIPESTATUS", saved_pipe_status);
        } else {
            (void)cjsh_env::unset_shell_variable_value("PIPESTATUS");
        }
        // defined functions receive event arguments as distinct words. otherwise
        // evaluate the registered text as shell code without appending arguments.
        if (shell_script_interpreter != nullptr &&
            shell_script_interpreter->has_function(function_name)) {
            std::vector<std::string> hook_arguments;
            hook_arguments.reserve(arguments.size() + 1);
            hook_arguments.push_back(function_name);
            hook_arguments.insert(hook_arguments.end(), arguments.begin(), arguments.end());
            (void)shell_script_interpreter->invoke_function(hook_arguments);
        } else {
            (void)execute(function_name);
        }
    }

    // leave the caller with the same status bindings after the final hook returns.
    pipeline_status_utils::set_last_status_env(saved_status_code);
    if (had_pipe_status) {
        (void)cjsh_env::set_shell_variable_value("PIPESTATUS", saved_pipe_status);
    } else {
        (void)cjsh_env::unset_shell_variable_value("PIPESTATUS");
    }
}
