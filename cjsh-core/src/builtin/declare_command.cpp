/*
  declare_command.cpp

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

#include "declare_command.h"
#include <cstdint>

#include "builtin_help.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

#include "error_out.h"
#include "interpreter.h"
#include "parser_utils.h"
#include "readonly_command.h"
#include "shell.h"
#include "shell_dialect.h"
#include "shell_env.h"

namespace {

enum class AttributeAction : unsigned char {
    NoChange,
    Set,
    Clear,
};

struct DeclareOptions {
    AttributeAction export_action = AttributeAction::NoChange;
    AttributeAction readonly_action = AttributeAction::NoChange;
    AttributeAction array_action = AttributeAction::NoChange;
    AttributeAction associative_action = AttributeAction::NoChange;
    AttributeAction nameref_action = AttributeAction::NoChange;
    bool print_mode = false;
    bool function_mode = false;
    bool function_name_only = false;
    bool global_scope = false;
};

bool is_typeset_alias(const std::string& command_name) {
    return command_name == "typeset";
}

std::string preferred_command_name(const std::string& command_name) {
    return is_typeset_alias(command_name) ? "typeset" : "declare";
}

bool print_invalid_option(const std::string& command_name, const std::string& option) {
    print_error({ErrorType::INVALID_ARGUMENT, command_name, "invalid option: " + option, {}});
    return false;
}

bool print_invalid_option(const std::string& command_name, char prefix, char option) {
    return print_invalid_option(command_name, std::string(1, prefix) + option);
}

bool apply_option_char(char prefix, char option, const std::string& command_name,
                       DeclareOptions& opts) {
    switch (option) {
        case 'p':
            if (prefix == '+') {
                return print_invalid_option(command_name, prefix, option);
            }
            opts.print_mode = true;
            return true;
        case 'f':
            if (prefix == '+') {
                return print_invalid_option(command_name, prefix, option);
            }
            opts.function_mode = true;
            return true;
        case 'F':
            if (prefix == '+') {
                return print_invalid_option(command_name, prefix, option);
            }
            opts.function_mode = true;
            opts.function_name_only = true;
            return true;
        case 'g':
            if (prefix == '+') {
                return print_invalid_option(command_name, prefix, option);
            }
            opts.global_scope = true;
            return true;
        case 'a':
            opts.array_action = (prefix == '-') ? AttributeAction::Set : AttributeAction::Clear;
            return true;
        case 'A':
            opts.associative_action =
                (prefix == '-') ? AttributeAction::Set : AttributeAction::Clear;
            return true;
        case 'n':
            opts.nameref_action = (prefix == '-') ? AttributeAction::Set : AttributeAction::Clear;
            return true;
        case 'x':
            opts.export_action = (prefix == '-') ? AttributeAction::Set : AttributeAction::Clear;
            return true;
        case 'r':
            opts.readonly_action = (prefix == '-') ? AttributeAction::Set : AttributeAction::Clear;
            return true;
        default:
            return print_invalid_option(command_name, prefix, option);
    }
}

bool parse_declare_options(const std::vector<std::string>& args, const std::string& command_name,
                           DeclareOptions& opts, size_t& operand_start) {
    operand_start = 1;

    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& token = args[i];
        if (token == "--") {
            operand_start = i + 1;
            break;
        }

        if (token.rfind("--", 0) == 0) {
            return print_invalid_option(command_name, token);
        }

        if (token.size() <= 1 || (token[0] != '-' && token[0] != '+')) {
            operand_start = i;
            break;
        }

        if (token == "-" || token == "+") {
            operand_start = i;
            break;
        }

        const char prefix = token[0];
        for (size_t j = 1; j < token.size(); ++j) {
            if (!apply_option_char(prefix, token[j], command_name, opts)) {
                return false;
            }
        }

        operand_start = i + 1;
    }

    if (opts.readonly_action == AttributeAction::Clear) {
        print_error({ErrorType::INVALID_ARGUMENT,
                     command_name,
                     "cannot unset readonly attribute",
                     {"Readonly attributes cannot be removed"}});
        return false;
    }

    if (opts.array_action == AttributeAction::Clear) {
        print_error({ErrorType::INVALID_ARGUMENT,
                     command_name,
                     "cannot unset array attribute",
                     {"Array attributes cannot be removed"}});
        return false;
    }
    if (opts.associative_action == AttributeAction::Clear) {
        print_error({ErrorType::INVALID_ARGUMENT,
                     command_name,
                     "cannot unset associative array attribute",
                     {"Associative array attributes cannot be removed"}});
        return false;
    }
    if (opts.array_action == AttributeAction::Set &&
        opts.associative_action == AttributeAction::Set) {
        print_error({ErrorType::INVALID_ARGUMENT,
                     command_name,
                     "cannot combine indexed and associative array attributes",
                     {}});
        return false;
    }
    if (opts.nameref_action == AttributeAction::Set &&
        (opts.array_action == AttributeAction::Set ||
         opts.associative_action == AttributeAction::Set)) {
        print_error({ErrorType::INVALID_ARGUMENT,
                     command_name,
                     "cannot combine nameref and array attributes",
                     {}});
        return false;
    }

    if (opts.function_mode && (opts.array_action == AttributeAction::Set ||
                               opts.associative_action == AttributeAction::Set ||
                               opts.nameref_action == AttributeAction::Set || opts.global_scope ||
                               opts.export_action != AttributeAction::NoChange)) {
        print_error({ErrorType::INVALID_ARGUMENT,
                     command_name,
                     "incompatible option combination for function mode",
                     {"Use -f/-F with optional -r or -p"}});
        return false;
    }

    return true;
}

std::string single_quote_value(const std::string& value) {
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back('\'');
    for (char c : value) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(c);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

bool target_has_index(const std::string& target) {
    size_t left_bracket = target.find('[');
    return left_bracket != std::string::npos && !target.empty() && target.back() == ']';
}

bool print_function_declarations(const std::vector<std::string>& names, const std::string& prefix) {
    for (const std::string& name : names) {
        std::cout << prefix << name << '\n';
    }
    return true;
}

int handle_function_mode(const std::vector<std::string>& args, size_t operand_start,
                         const DeclareOptions& opts, const std::string& command_name,
                         Shell* shell) {
    if (shell == nullptr || shell->get_shell_script_interpreter() == nullptr) {
        print_error(
            {ErrorType::RUNTIME_ERROR, command_name, "shell interpreter not available", {}});
        return 1;
    }

    auto* interpreter = shell->get_shell_script_interpreter();

    bool success = true;
    std::vector<std::string> function_names;
    if (operand_start >= args.size()) {
        function_names = interpreter->get_function_names();
    } else {
        function_names.assign(args.begin() + static_cast<std::ptrdiff_t>(operand_start),
                              args.end());
    }

    std::sort(function_names.begin(), function_names.end());

    for (const std::string& function_name : function_names) {
        if (!interpreter->has_function(function_name)) {
            print_error({ErrorType::INVALID_ARGUMENT,
                         command_name,
                         function_name + ": function not found",
                         {}});
            success = false;
            continue;
        }

        if (opts.readonly_action == AttributeAction::Set) {
            readonly_function_manager_set(function_name);
        }
    }

    if ((operand_start >= args.size() || opts.print_mode || opts.function_name_only ||
         opts.readonly_action == AttributeAction::NoChange) &&
        !function_names.empty()) {
        (void)print_function_declarations(function_names, "declare -f ");
    }

    return success ? 0 : 1;
}

std::vector<std::string> collect_printable_variable_names(ShellScriptInterpreter* interpreter) {
    std::vector<std::string> names;
    if (interpreter == nullptr) {
        return names;
    }

    names = interpreter->get_variable_manager().get_variable_names();
    (void)names.erase(
        std::remove_if(names.begin(), names.end(),
                       [](const std::string& name) { return !is_valid_identifier(name); }),
        names.end());
    std::sort(names.begin(), names.end());
    (void)names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

bool variable_matches_print_filter(const std::string& name, const DeclareOptions& opts) {
    if (opts.readonly_action == AttributeAction::Set && !readonly_manager_is(name)) {
        return false;
    }

    if (opts.export_action == AttributeAction::Set && std::getenv(name.c_str()) == nullptr) {
        return false;
    }

    return true;
}

void print_variable_declaration(const std::string& name, ShellScriptInterpreter* interpreter) {
    std::string attrs;
    auto* variable_manager =
        interpreter == nullptr ? nullptr : &interpreter->get_variable_manager();
    const bool is_nameref = variable_manager != nullptr && variable_manager->is_nameref(name);
    const bool is_associative =
        !is_nameref && variable_manager != nullptr && variable_manager->is_associative_array(name);
    const bool is_indexed =
        !is_nameref && variable_manager != nullptr && variable_manager->is_indexed_array(name);
    if (is_associative) {
        attrs.push_back('A');
    } else if (is_indexed) {
        attrs.push_back('a');
    }
    if (is_nameref) {
        attrs.push_back('n');
    }
    if (readonly_manager_is(name)) {
        attrs.push_back('r');
    }
    if (std::getenv(name.c_str()) != nullptr) {
        attrs.push_back('x');
    }

    std::cout << "declare";
    if (!attrs.empty()) {
        std::cout << " -" << attrs;
    }

    std::string value;
    bool has_value = false;
    if (is_nameref) {
        value = variable_manager->get_nameref_target(name);
        has_value = !value.empty();
    } else if ((is_associative || is_indexed) && variable_manager != nullptr) {
        std::cout << ' ' << name << "=(";
        bool first = true;
        for (const auto& [key, element] : variable_manager->get_array_entries(name)) {
            if (!first) {
                std::cout << ' ';
            }
            first = false;
            std::cout << '[' << single_quote_value(key) << "]=" << single_quote_value(element);
        }
        std::cout << ")\n";
        return;
    } else if (interpreter != nullptr && interpreter->is_local_variable(name)) {
        value = interpreter->get_variable_value(name);
        has_value = true;
    } else if (cjsh_env::shell_variable_is_set(name)) {
        value = cjsh_env::get_shell_variable_value(name);
        has_value = true;
    } else if (const char* env_val = std::getenv(name.c_str()); env_val != nullptr) {
        value = env_val;
        has_value = true;
    }

    if (has_value) {
        std::cout << ' ' << name << '=' << single_quote_value(value);
    } else {
        std::cout << ' ' << name;
    }
    std::cout << '\n';
}

int handle_print_mode(const std::vector<std::string>& args, size_t operand_start,
                      const DeclareOptions& opts, const std::string& command_name,
                      ShellScriptInterpreter* interpreter) {
    std::vector<std::string> all_names = collect_printable_variable_names(interpreter);
    std::unordered_set<std::string> all_name_set(all_names.begin(), all_names.end());

    bool success = true;

    if (operand_start >= args.size()) {
        for (const std::string& name : all_names) {
            if (!variable_matches_print_filter(name, opts)) {
                continue;
            }
            print_variable_declaration(name, interpreter);
        }
        return 0;
    }

    for (size_t i = operand_start; i < args.size(); ++i) {
        const std::string& name = args[i];
        if (!is_valid_identifier(name)) {
            print_error(
                {ErrorType::INVALID_ARGUMENT, command_name, "invalid variable name: " + name, {}});
            success = false;
            continue;
        }

        if (all_name_set.find(name) == all_name_set.end() &&
            !cjsh_env::shell_variable_is_set(name) && std::getenv(name.c_str()) == nullptr) {
            print_error({ErrorType::INVALID_ARGUMENT, command_name, name + ": not found", {}});
            success = false;
            continue;
        }

        if (!variable_matches_print_filter(name, opts)) {
            continue;
        }

        print_variable_declaration(name, interpreter);
    }

    return success ? 0 : 1;
}

void apply_export_attribute(const std::string& name, AttributeAction action, bool local_scope,
                            ShellScriptInterpreter* interpreter, Shell* shell) {
    if (action == AttributeAction::NoChange) {
        return;
    }

    if (action == AttributeAction::Clear) {
        (void)unsetenv(name.c_str());
        return;
    }

    std::string value;
    if (local_scope && interpreter != nullptr) {
        value = interpreter->get_variable_value(name);
        interpreter->mark_local_as_exported(name);
        (void)setenv(name.c_str(), value.c_str(), 1);
        return;
    }

    auto& env_map = cjsh_env::env_vars();
    auto env_it = env_map.find(name);
    if (env_it != env_map.end()) {
        value = env_it->second;
    } else if (const char* env_val = std::getenv(name.c_str()); env_val != nullptr) {
        value = env_val;
    }

    env_map[name] = value;
    (void)setenv(name.c_str(), value.c_str(), 1);
    if (shell != nullptr) {
        cjsh_env::sync_parser_env_var(shell, name);
    }
}

enum class ArrayLiteralKind : std::uint8_t {
    Indexed,
    Associative
};

bool assign_array_literal_for_scope(ArrayLiteralKind kind, ShellScriptInterpreter* interpreter,
                                    bool local_scope, bool force_global,
                                    const std::string& base_name,
                                    const std::vector<std::string>& words, bool append,
                                    const std::string& command_name) {
    if (interpreter == nullptr) {
        print_error(
            {ErrorType::RUNTIME_ERROR, command_name, "shell interpreter not available", {}});
        return false;
    }
    if (local_scope && !interpreter->is_local_variable(base_name)) {
        interpreter->set_local_variable(base_name, "");
    }
    auto& variables = interpreter->get_variable_manager();
    const bool global = force_global && !local_scope;
    if (kind == ArrayLiteralKind::Associative) {
        return global ? variables.assign_global_associative_literal(base_name, words, append)
                      : variables.assign_associative_literal(base_name, words, append);
    }
    return global ? variables.assign_global_array_literal(base_name, words, append)
                  : variables.assign_array_literal(base_name, words, append);
}

bool assign_value_for_scope(VariableManager& variable_manager, bool force_global,
                            const std::string& target, const std::string& value, bool append) {
    return force_global ? variable_manager.assign_global_variable(target, value, append)
                        : variable_manager.assign_variable(target, value, append);
}

}  // namespace

int declare_command(const std::vector<std::string>& args, Shell* shell) {
    const std::string command_name =
        args.empty() ? std::string("declare") : preferred_command_name(args[0]);

    if (builtin_handle_help(
            args, {"Usage: " + command_name + " [-aAnFfgprx] [NAME[=VALUE] ...]",
                   "Set variable attributes and values.", "-a declare indexed arrays.",
                   "-A declare associative arrays.", "-n make names references to variables.",
                   "-f/-F operate on shell functions.", "-g force global scope inside functions.",
                   "-p print declarations.", "-r mark names readonly.", "-x mark names exported.",
                   "+x remove export attribute."})) {
        return 0;
    }

    if (config::is_posix_mode()) {
        print_error({ErrorType::INVALID_ARGUMENT,
                     command_name,
                     "'" + command_name + "' is disabled in POSIX mode",
                     {"Use assignment, 'export', and 'readonly' instead"}});
        return 1;
    }

    if (shell == nullptr || shell->get_shell_script_interpreter() == nullptr) {
        print_error(
            {ErrorType::RUNTIME_ERROR, command_name, "shell interpreter not available", {}});
        return 1;
    }

    auto* interpreter = shell->get_shell_script_interpreter();

    DeclareOptions opts;
    size_t operand_start = 1;
    if (!parse_declare_options(args, command_name, opts, operand_start)) {
        return 2;
    }

    if (opts.function_mode) {
        return handle_function_mode(args, operand_start, opts, command_name, shell);
    }

    if (opts.print_mode) {
        return handle_print_mode(args, operand_start, opts, command_name, interpreter);
    }

    if (operand_start >= args.size()) {
        return handle_print_mode(args, operand_start, opts, command_name, interpreter);
    }

    const bool in_function_scope = interpreter->in_function_scope();
    const bool force_global_scope = in_function_scope && opts.global_scope;
    const bool local_scope = in_function_scope && !opts.global_scope;

    bool all_successful = true;
    auto& variable_manager = interpreter->get_variable_manager();

    for (size_t i = operand_start; i < args.size();) {
        AssignmentOperand operand;
        (void)parse_assignment_operand(args[i], operand, false);

        bool append = false;
        std::string normalized_target = normalize_assignment_target(operand.name, append);
        std::string base_name = assignment_target_base_name(normalized_target);

        if (!is_valid_identifier(base_name)) {
            print_error({ErrorType::INVALID_ARGUMENT,
                         command_name,
                         "invalid variable name: " + operand.name,
                         {}});
            all_successful = false;
            ++i;
            continue;
        }

        if (!operand.has_assignment && target_has_index(normalized_target) &&
            opts.array_action != AttributeAction::Set) {
            print_error({ErrorType::INVALID_ARGUMENT,
                         command_name,
                         "invalid variable name: " + operand.name,
                         {}});
            all_successful = false;
            ++i;
            continue;
        }

        bool assignment_ok = true;

        if (opts.nameref_action == AttributeAction::Set) {
            assignment_ok = variable_manager.set_nameref(
                base_name, operand.has_assignment ? operand.value : std::string{},
                force_global_scope);
            ++i;
        } else

            if (operand.has_assignment && operand.value.empty() && i + 1 < args.size() &&
                args[i + 1] == "(") {
            size_t close_index = find_closing_parenthesis_token(args, i + 1);

            if (close_index == std::string::npos) {
                print_error({ErrorType::INVALID_ARGUMENT,
                             command_name,
                             "unclosed array assignment for " + base_name,
                             {}});
                all_successful = false;
                ++i;
                continue;
            }

            if (!local_scope && !readonly_manager_can_assign(base_name, command_name)) {
                all_successful = false;
                i = close_index + 1;
                continue;
            }

            std::vector<std::string> words(args.begin() + static_cast<std::ptrdiff_t>(i + 2),
                                           args.begin() + static_cast<std::ptrdiff_t>(close_index));

            if (opts.associative_action == AttributeAction::Set ||
                variable_manager.is_associative_array(base_name)) {
                assignment_ok = assign_array_literal_for_scope(
                    ArrayLiteralKind::Associative, interpreter, local_scope, force_global_scope,
                    base_name, words, append, command_name);
            } else {
                assignment_ok = assign_array_literal_for_scope(
                    ArrayLiteralKind::Indexed, interpreter, local_scope, force_global_scope,
                    base_name, words, append, command_name);
            }
            i = close_index + 1;
        } else if (opts.associative_action == AttributeAction::Set) {
            if (!local_scope && operand.has_assignment &&
                !readonly_manager_can_assign(base_name, command_name)) {
                all_successful = false;
                ++i;
                continue;
            }
            if (operand.has_assignment && target_has_index(normalized_target)) {
                if (local_scope && !interpreter->is_local_variable(base_name)) {
                    interpreter->set_local_variable(base_name, "");
                    (void)variable_manager.assign_associative_literal(base_name, {}, false);
                } else if (!variable_manager.is_associative_array(base_name)) {
                    (void)assign_array_literal_for_scope(ArrayLiteralKind::Associative, interpreter,
                                                         local_scope, force_global_scope, base_name,
                                                         {}, false, command_name);
                }
                assignment_ok = assign_value_for_scope(variable_manager, force_global_scope,
                                                       normalized_target, operand.value, append);
            } else {
                std::vector<std::string> words;
                if (operand.has_assignment) {
                    words.push_back(operand.value);
                }
                assignment_ok = assign_array_literal_for_scope(
                    ArrayLiteralKind::Associative, interpreter, local_scope, force_global_scope,
                    base_name, words, append, command_name);
            }
            ++i;
        } else if (opts.array_action == AttributeAction::Set) {
            if (!local_scope && operand.has_assignment &&
                !readonly_manager_can_assign(base_name, command_name)) {
                all_successful = false;
                ++i;
                continue;
            }

            if (operand.has_assignment) {
                if (target_has_index(normalized_target)) {
                    if (local_scope && !interpreter->is_local_variable(base_name)) {
                        interpreter->set_local_variable(base_name, "");
                    }

                    assignment_ok =
                        assign_value_for_scope(variable_manager, force_global_scope,
                                               normalized_target, operand.value, append);
                } else {
                    assignment_ok = assign_array_literal_for_scope(
                        ArrayLiteralKind::Indexed, interpreter, local_scope, force_global_scope,
                        base_name, {operand.value}, append, command_name);
                }
            } else {
                assignment_ok = assign_array_literal_for_scope(
                    ArrayLiteralKind::Indexed, interpreter, local_scope, force_global_scope,
                    base_name, {}, false, command_name);
            }
            ++i;
        } else {
            if (operand.has_assignment) {
                if (!local_scope && !readonly_manager_can_assign(base_name, command_name)) {
                    all_successful = false;
                    ++i;
                    continue;
                }

                if (local_scope && !interpreter->is_local_variable(base_name)) {
                    interpreter->set_local_variable(base_name, "");
                }

                assignment_ok = assign_value_for_scope(variable_manager, force_global_scope,
                                                       normalized_target, operand.value, append);
            } else {
                if (local_scope) {
                    interpreter->set_local_variable(base_name, "");
                } else if (!cjsh_env::shell_variable_is_set(base_name) &&
                           !cjsh_env::set_shell_variable_value(base_name, "")) {
                    print_error({ErrorType::FATAL_ERROR,
                                 command_name,
                                 "shell not initialized properly",
                                 {}});
                    assignment_ok = false;
                }
            }
            ++i;
        }

        if (!assignment_ok) {
            print_error({ErrorType::INVALID_ARGUMENT,
                         command_name,
                         "invalid assignment target: " + operand.name,
                         {}});
            all_successful = false;
            continue;
        }

        if (opts.readonly_action == AttributeAction::Set) {
            readonly_manager_set(base_name);
        }

        apply_export_attribute(base_name, opts.export_action, local_scope, interpreter, shell);
    }

    return all_successful ? 0 : 1;
}
