/*
  loop_evaluator.cpp

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

#include "loop_evaluator.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "command_substitution_evaluator.h"
#include "control_flow.h"
#include "error_out.h"
#include "exec.h"
#include "flags.h"
#include "interpreter.h"
#include "interpreter_utils.h"
#include "parser.h"
#include "parser_utils.h"
#include "pipeline_status_utils.h"
#include "quote_state.h"
#include "shell.h"
#include "shell_dialect.h"
#include "shell_env.h"
#include "signal_handler.h"

using shell_script_interpreter::detail::control_flow_pending;
using shell_script_interpreter::detail::strip_inline_comment;
using shell_script_interpreter::detail::trim;

namespace loop_evaluator {

namespace {

constexpr size_t kInlineLoopCacheLimit = 64;

thread_local std::unordered_map<std::string, std::shared_ptr<std::vector<std::string>>>
    g_inline_loop_cache;

std::string extract_loop_keyword(const std::string& segment) {
    std::string trimmed = trim(strip_inline_comment(segment));
    if (trimmed.empty()) {
        return "loop";
    }

    size_t end = trimmed.find_first_of(" \t;");
    if (end == std::string::npos) {
        return trimmed;
    }
    return trimmed.substr(0, end);
}

int report_inline_loop_syntax_error(const std::string& segment, std::string_view missing_token) {
    std::string keyword = extract_loop_keyword(segment);
    std::string message =
        "expected '" + std::string(missing_token) + "' to complete the " + keyword + " loop";
    std::vector<std::string> suggestions = {"Insert '" + std::string(missing_token) +
                                            "' between the loop header and body (e.g. '" + keyword +
                                            " ...; do ...; done')."};
    print_error({ErrorType::SYNTAX_ERROR, ErrorSeverity::ERROR, keyword, message, suggestions});
    return 2;
}

int report_loop_header_error(const std::string& keyword, const std::string& message) {
    print_error({ErrorType::SYNTAX_ERROR, ErrorSeverity::ERROR, keyword, message, {}});
    return 2;
}

std::string expand_loop_substitutions(const std::string& words,
                                      const std::function<int(const std::string&)>& executor) {
    CommandSubstitutionEvaluator evaluator(
        CommandSubstitutionEvaluator::create_command_executor(executor));
    return evaluator.expand_substitutions(words).text;
}

const std::shared_ptr<std::vector<std::string>>& get_cached_inline_loop_body(
    const std::string& body, Parser* parser) {
    static const std::shared_ptr<std::vector<std::string>> kEmptyBody =
        std::make_shared<std::vector<std::string>>();

    if (body.empty() || parser == nullptr) {
        return kEmptyBody;
    }

    auto cache_it = g_inline_loop_cache.find(body);
    if (cache_it != g_inline_loop_cache.end()) {
        return cache_it->second;
    }

    auto parsed_lines = parser->parse_into_lines(body);
    auto parsed_ptr = std::make_shared<std::vector<std::string>>(std::move(parsed_lines));

    if (g_inline_loop_cache.size() >= kInlineLoopCacheLimit) {
        for (auto it = g_inline_loop_cache.begin(); it != g_inline_loop_cache.end(); ++it) {
            if (it->second.use_count() == 1) {
                (void)g_inline_loop_cache.erase(it);
                break;
            }
        }
        if (g_inline_loop_cache.size() >= kInlineLoopCacheLimit) {
            g_inline_loop_cache.clear();
        }
    }

    auto [insert_it, _] = g_inline_loop_cache.emplace(body, std::move(parsed_ptr));
    return insert_it->second;
}

bool check_loop_interrupt(int& rc) {
    if (!shell) {
        return false;
    }

    if (!SignalHandler::has_pending_signals()) {
        return false;
    }

    SignalProcessingResult pending = shell->process_pending_signals();
    int exit_code = shell_script_interpreter::detail::pending_signal_exit_code(pending);
    if (exit_code >= 0) {
        rc = exit_code;
        return true;
    }
    return false;
}

bool matches_keyword_only(const std::string& text, std::string_view keyword) {
    if (text.size() < keyword.size()) {
        return false;
    }
    if (text.compare(0, keyword.size(), keyword) != 0) {
        return false;
    }
    size_t pos = keyword.size();
    while (pos < text.size() && (std::isspace(static_cast<unsigned char>(text[pos])) != 0)) {
        pos++;
    }
    while (pos < text.size() && text[pos] == ';') {
        pos++;
        while (pos < text.size() && (std::isspace(static_cast<unsigned char>(text[pos])) != 0)) {
            pos++;
        }
    }
    while (pos < text.size() && (std::isspace(static_cast<unsigned char>(text[pos])) != 0)) {
        pos++;
    }
    if (pos >= text.size()) {
        return true;
    }

    auto is_redirection_start = [&](size_t start) {
        if (start >= text.size()) {
            return false;
        }
        char ch = text[start];
        if (ch == '<' || ch == '>') {
            return true;
        }
        if ((ch == '&') && start + 1 < text.size()) {
            char next = text[start + 1];
            return next == '>' || next == '<';
        }
        return false;
    };

    if (is_redirection_start(pos)) {
        return true;
    }

    if (std::isdigit(static_cast<unsigned char>(text[pos]))) {
        size_t digit_pos = pos;
        while (digit_pos < text.size() &&
               std::isdigit(static_cast<unsigned char>(text[digit_pos])) != 0) {
            digit_pos++;
        }
        return is_redirection_start(digit_pos);
    }

    return false;
}

std::string get_select_prompt() {
    if (cjsh_env::shell_variable_is_set("PS3")) {
        return cjsh_env::get_shell_variable_value("PS3");
    }
    return "#? ";
}

void print_select_menu(const std::vector<std::string>& items) {
    for (size_t i = 0; i < items.size(); ++i) {
        std::cerr << (i + 1) << ") " << items[i] << '\n';
    }
}

std::optional<size_t> parse_select_choice(const std::string& reply, size_t item_count) {
    std::string candidate = trim(reply);
    if (candidate.empty()) {
        return std::nullopt;
    }
    for (char ch : candidate) {
        if (std::isdigit(static_cast<unsigned char>(ch)) == 0) {
            return std::nullopt;
        }
    }

    try {
        size_t parsed = std::stoull(candidate);
        if (parsed == 0 || parsed > item_count) {
            return std::nullopt;
        }
        return parsed - 1;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

struct CStyleForHeader {
    bool is_c_style = false;
    std::string init_expression;
    std::string condition_expression;
    std::string update_expression;
};

bool split_c_style_for_components(const std::string& text, std::array<std::string, 3>& parts) {
    size_t part_index = 0;
    std::string current;
    int paren_depth = 0;
    bool in_single = false;
    bool in_double = false;
    bool escaped = false;

    for (char ch : text) {
        if (escaped) {
            current += ch;
            escaped = false;
            continue;
        }

        if (ch == '\\' && !in_single) {
            current += ch;
            escaped = true;
            continue;
        }

        if (!in_double && ch == '\'') {
            in_single = !in_single;
            current += ch;
            continue;
        }

        if (!in_single && ch == '"') {
            in_double = !in_double;
            current += ch;
            continue;
        }

        if (in_single || in_double) {
            current += ch;
            continue;
        }

        if (ch == '(') {
            ++paren_depth;
            current += ch;
            continue;
        }

        if (ch == ')') {
            if (paren_depth > 0) {
                --paren_depth;
            }
            current += ch;
            continue;
        }

        if (ch == ';' && paren_depth == 0) {
            if (part_index >= 2) {
                return false;
            }
            parts[part_index++] = trim(current);
            current.clear();
            continue;
        }

        current += ch;
    }

    if (part_index != 2) {
        return false;
    }

    parts[2] = trim(current);
    return true;
}

bool parse_c_style_for_header(const std::string& header, CStyleForHeader& out) {
    out = CStyleForHeader{};
    std::string normalized = trim(header);
    if (normalized.rfind("for", 0) != 0) {
        return false;
    }

    size_t pos = 3;
    while (pos < normalized.size() &&
           (std::isspace(static_cast<unsigned char>(normalized[pos])) != 0)) {
        ++pos;
    }

    if (pos + 1 >= normalized.size() || normalized.compare(pos, 2, "((") != 0) {
        return false;
    }

    size_t content_start = 0;
    size_t content_end = 0;
    size_t after_close = 0;
    if (!parser_find_balanced_double_parens(normalized, pos, content_start, content_end,
                                            after_close)) {
        return false;
    }

    std::string trailing = trim(normalized.substr(after_close));
    if (!trailing.empty()) {
        return false;
    }

    std::array<std::string, 3> parts;
    std::string content = normalized.substr(content_start, content_end - content_start);
    if (!split_c_style_for_components(content, parts)) {
        return false;
    }

    out.is_c_style = true;
    out.init_expression = std::move(parts[0]);
    out.condition_expression = std::move(parts[1]);
    out.update_expression = std::move(parts[2]);
    return true;
}

std::pair<std::string, std::string> split_done_suffix(const std::string& suffix) {
    utils::ShellQuoteState quote_state;
    int paren_depth = 0;

    for (size_t i = 0; i < suffix.size(); ++i) {
        const char ch = suffix[i];
        if (quote_state.consume_forward(ch) == utils::QuoteAdvanceResult::Continue ||
            quote_state.inside_quotes()) {
            continue;
        }

        if (ch == '(') {
            ++paren_depth;
            continue;
        }

        if (ch == ')' && paren_depth > 0) {
            --paren_depth;
            continue;
        }

        if (ch == ';' && paren_depth == 0) {
            std::string redirections = trim(suffix.substr(0, i));
            std::string trailing_commands = trim(suffix.substr(i + 1));
            return {redirections, trailing_commands};
        }
    }

    return {trim(suffix), ""};
}

struct ParsedLoopBlock {
    std::string header;
    std::vector<std::string> body_lines;
    std::string done_redirections;
    std::string trailing_commands;
    size_t end_index = 0;
};

bool parse_multiline_loop_block(const std::vector<std::string>& src_lines, size_t start_index,
                                const std::string& first, Parser* parser, ParsedLoopBlock& parsed) {
    parsed = ParsedLoopBlock{};
    parsed.end_index = start_index;
    if (parser == nullptr) {
        return false;
    }

    std::string header = first;
    size_t do_pos = parser_find_keyword_token(header, "do", 0);
    while (do_pos == std::string::npos && parsed.end_index + 1 < src_lines.size()) {
        header += '\n';
        header += strip_inline_comment(src_lines[++parsed.end_index]);
        do_pos = parser_find_keyword_token(header, "do", 0);
    }
    if (do_pos == std::string::npos) {
        return false;
    }
    parsed.header = trim(header.substr(0, do_pos));

    std::string body = header.substr(do_pos + 2);
    int depth = 1;
    size_t done_pos =
        parser_find_block_end(body, {"for", "select", "while", "until"}, "done", depth);
    while (done_pos == std::string::npos && parsed.end_index + 1 < src_lines.size()) {
        const std::string next = strip_inline_comment(src_lines[++parsed.end_index]);
        const size_t next_done =
            parser_find_block_end(next, {"for", "select", "while", "until"}, "done", depth);
        body += '\n';
        if (next_done != std::string::npos) {
            done_pos = body.size() + next_done;
        }
        body += next;
    }
    if (done_pos == std::string::npos) {
        return false;
    }

    auto suffix_parts = split_done_suffix(trim(body.substr(done_pos + 4)));
    parsed.done_redirections = std::move(suffix_parts.first);
    parsed.trailing_commands = std::move(suffix_parts.second);
    const auto& body_lines = get_cached_inline_loop_body(trim(body.substr(0, done_pos)), parser);
    if (body_lines == nullptr) {
        return false;
    }
    parsed.body_lines = *body_lines;
    return true;
}

bool parse_inline_loop_block(const std::string& first, Parser* parser, ParsedLoopBlock& parsed) {
    return parse_multiline_loop_block({first}, 0, first, parser, parsed);
}

bool trailing_contains_block_closer_segment(const std::string& trailing_commands, Parser* parser) {
    if (trailing_commands.empty()) {
        return false;
    }

    std::vector<std::string> segments;
    if (parser != nullptr) {
        segments = parser->parse_semicolon_commands(trailing_commands);
    }

    if (segments.empty()) {
        segments.push_back(trailing_commands);
    }

    return std::any_of(segments.begin(), segments.end(), [](const auto& segment) {
        std::string trimmed_segment = trim(strip_inline_comment(segment));
        return matches_keyword_only(trimmed_segment, "done") ||
               matches_keyword_only(trimmed_segment, "fi") ||
               matches_keyword_only(trimmed_segment, "esac");
    });
}

int execute_loop_trailing_commands(
    int loop_rc, const std::string& trailing_commands,
    const std::function<int(const std::string&)>& execute_simple_or_pipeline, Parser* parser,
    const std::function<bool()>& should_abort_execution) {
    if (trailing_commands.empty()) {
        return loop_rc;
    }

    if (trailing_contains_block_closer_segment(trailing_commands, parser)) {
        return loop_rc;
    }

    if ((should_abort_execution && should_abort_execution()) || control_flow_pending() ||
        cjsh_env::exit_requested()) {
        return loop_rc;
    }

    if (!execute_simple_or_pipeline) {
        return loop_rc;
    }

    pipeline_status_utils::set_last_status_env(loop_rc);
    return execute_simple_or_pipeline(trailing_commands);
}

int handle_loop_block(const std::vector<std::string>& src_lines, size_t& idx,
                      const std::string& keyword, bool is_until,
                      const std::function<int(const std::vector<std::string>&)>& execute_block,
                      const std::function<int(const std::string&)>& execute_simple_or_pipeline,
                      Parser* parser, const std::function<bool()>& should_abort_execution) {
    // shared while/until evaluator used by interpreter loop dispatch
    std::string first = trim(strip_inline_comment(src_lines[idx]));
    if (first != keyword && first.rfind(keyword + " ", 0) != 0) {
        return 1;
    }

    auto abort_pending = [&] {
        return cjsh_env::exit_requested() || (should_abort_execution && should_abort_execution());
    };

    ParsedLoopBlock parsed_loop;
    if (!parse_multiline_loop_block(src_lines, idx, first, parser, parsed_loop)) {
        idx = parsed_loop.end_index;
        return 1;
    }
    idx = parsed_loop.end_index;
    std::string cond = trim(parsed_loop.header.substr(keyword.size()));
    if (!cond.empty() && cond.back() == ';' && !is_char_escaped(cond, cond.size() - 1)) {
        cond.pop_back();
        cond = trim(cond);
    }
    const auto& body_lines = parsed_loop.body_lines;

    auto run_loop_logic = [&]() -> int {
        // evaluate condition then execute body per iteration until loop termination criteria hit
        int rc = 0;
        while (true) {
            if (check_loop_interrupt(rc)) {
                break;
            }

            int c = 0;
            if (!cond.empty()) {
                {
                    std::optional<ShellScriptInterpreter::LoopScope> loop_scope;
                    if (shell && shell->get_interpreter()) {
                        loop_scope.emplace(*shell->get_interpreter());
                    }
                    Shell::ErrexitScope scope(shell.get());
                    c = execute_simple_or_pipeline(cond);
                }
                if (abort_pending()) {
                    rc = c;
                    break;
                }

                if (control_flow_pending()) {
                    auto outcome = handle_loop_command_result(c, true);
                    rc = outcome.code;
                    if (outcome.flow == LoopFlow::CONTINUE) {
                        continue;
                    }
                    break;
                }

                int signal_rc = 0;
                if (check_loop_interrupt(signal_rc)) {
                    rc = signal_rc;
                    break;
                }
            }

            bool continue_loop = is_until ? (c != 0) : (c == 0);
            if (!continue_loop) {
                break;
            }

            // execute the collected loop body in interpreter context
            rc = execute_block(body_lines);
            auto outcome = handle_loop_command_result(rc, true);
            rc = outcome.code;
            if (abort_pending()) {
                break;
            }
            if (outcome.flow == LoopFlow::BREAK) {
                break;
            }
            if (outcome.flow == LoopFlow::CONTINUE) {
                continue;
            }
        }
        return rc;
    };

    const auto& done_redirections = parsed_loop.done_redirections;
    const auto& trailing_commands = parsed_loop.trailing_commands;

    // Only redirections after `done` belong to the loop itself. Parsing the entire
    // loop here also lifted redirections from its condition/body into this scope.
    if (!done_redirections.empty() && parser && shell && shell->executor) {
        std::vector<Command> redirection_commands;
        try {
            redirection_commands =
                parser->parse_pipeline_with_preprocessing("true " + done_redirections);
        } catch (const std::exception&) {
            // Best-effort parse; fall back to normal loop execution.
        }
        if (!redirection_commands.empty()) {
            const int exit_code = shell->executor->run_with_command_redirections(
                redirection_commands[0], run_loop_logic, keyword, false);
            return execute_loop_trailing_commands(exit_code, trailing_commands,
                                                  execute_simple_or_pipeline, parser,
                                                  should_abort_execution);
        }
    }

    return execute_loop_trailing_commands(run_loop_logic(), trailing_commands,
                                          execute_simple_or_pipeline, parser,
                                          should_abort_execution);
}

}  // namespace

LoopCommandOutcome handle_loop_command_result(int rc, bool allow_error_continue) {
    if (shell && shell->get_interpreter()) {
        auto& control = shell->get_interpreter()->control_flow_state();
        switch (control.consume_loop()) {
            case ControlFlowKind::Return:
                return {LoopFlow::BREAK, control.status()};
            case ControlFlowKind::Break:
                return {LoopFlow::BREAK, 0};
            case ControlFlowKind::Continue:
                return {LoopFlow::CONTINUE, 0};
            case ControlFlowKind::None:
                break;
        }
    }
#ifdef SIGINT
    if (rc == 128 + SIGINT) {
        return {LoopFlow::BREAK, rc};
    }
#endif
#ifdef SIGTERM
    if (rc == 128 + SIGTERM) {
        return {LoopFlow::BREAK, rc};
    }
#endif
#ifdef SIGHUP
    if (rc == 128 + SIGHUP) {
        return {LoopFlow::BREAK, rc};
    }
#endif
    if (rc != 0) {
        if (shell && shell->should_abort_on_nonzero_exit()) {
            return {LoopFlow::BREAK, rc};
        }
        if (!allow_error_continue) {
            return {LoopFlow::BREAK, rc};
        }
    }
    return {LoopFlow::NONE, rc};
}

int handle_for_block(
    const std::vector<std::string>& src_lines, size_t& idx,
    const std::function<int(const std::vector<std::string>&)>& execute_block,
    const std::function<long long(const std::string&)>& evaluate_arithmetic_expression,
    const std::function<int(const std::string&)>& execute_simple_or_pipeline, Parser* parser,
    const std::function<bool()>& should_abort_execution) {
    // main for evaluator called after interpreter classifies a block as for
    std::string first = trim(strip_inline_comment(src_lines[idx]));
    if (!parser_starts_with_keyword_token(first, "for") && first.rfind("for;", 0) != 0) {
        return 1;
    }

    std::string var;
    std::vector<std::string> items;
    CStyleForHeader c_style_header;
    auto abort_pending = [&] {
        return cjsh_env::exit_requested() || (should_abort_execution && should_abort_execution());
    };

    auto finalize_with_trailing_commands = [&](int loop_rc, const std::string& trailing_commands) {
        return execute_loop_trailing_commands(
            loop_rc, trailing_commands, execute_simple_or_pipeline, parser, should_abort_execution);
    };

    auto assign_loop_variable = [&](const std::string& value) {
        if (shell != nullptr &&
            cjsh_env::set_shell_or_local_variable_value(shell.get(), var, value)) {
            return;
        }

        (void)cjsh_env::set_shell_variable_value(var, value);
    };

    // Validate literal syntax before expanding any words in the iteration list.
    auto parse_header = [&](const std::string& header) -> bool {
        std::string normalized_header = trim(header);
        if (!normalized_header.empty() && normalized_header.back() == ';' &&
            !is_char_escaped(normalized_header, normalized_header.size() - 1)) {
            normalized_header.pop_back();
            normalized_header = trim(normalized_header);
        }

        if (parse_c_style_for_header(normalized_header, c_style_header)) {
            if (config::is_posix_mode()) {
                (void)cjsh_env::posix_error_exit(2);
                (void)report_loop_header_error("for", "C-style loops are disabled in POSIX mode");
                return false;
            }
            return true;
        }

        if (trim(normalized_header.substr(3)).rfind("((", 0) == 0) {
            report_loop_header_error(
                "for", "invalid C-style loop header; expected ((init; condition; update))");
            return false;
        }

        const auto parsed = parse_named_loop_header(header, "for", config::is_posix_mode());
        if (!parsed.error.empty()) {
            report_loop_header_error("for", parsed.error);
            return false;
        }
        var = parsed.variable;
        if (parsed.has_in) {
            // Expand the list once, after validating syntax and before tokenization.
            const std::string expanded_words =
                expand_loop_substitutions(parsed.words, execute_simple_or_pipeline);
            // A fixed command prefix keeps list words out of alias/assignment-command handling.
            auto toks = parser->parse_command("for " + var + " in " + expanded_words);
            if (toks.size() < 3) {
                report_loop_header_error("for", "could not parse iteration words after 'in'");
                return false;
            }
            items.assign(toks.begin() + 3, toks.end());
        } else {
            items = flags::get_positional_parameters();
        }
        return true;
    };

    auto execute_for_iterations = [&](const std::function<LoopCommandOutcome()>& run_iteration,
                                      const std::string& trailing_commands,
                                      const std::function<void()>& on_early_return) -> int {
        int rc = 0;

        auto evaluate_arithmetic_or_fail = [&](const std::string& expression,
                                               long long& result_out) -> bool {
            if (!evaluate_arithmetic_expression) {
                print_error({ErrorType::RUNTIME_ERROR,
                             ErrorSeverity::ERROR,
                             "for",
                             "arithmetic evaluator not available for C-style for loop",
                             {"Use a regular 'for var in ...' loop or initialize arithmetic "
                              "support before executing this block."}});
                rc = 1;
                return false;
            }

            try {
                result_out = evaluate_arithmetic_expression(expression);
                return true;
            } catch (const std::exception& e) {
                print_error({ErrorType::RUNTIME_ERROR,
                             ErrorSeverity::ERROR,
                             "for",
                             std::string("failed to evaluate arithmetic expression: ") + e.what(),
                             {"Check C-style for-loop expressions for invalid operators or "
                              "division by zero."}});
                rc = 1;
                return false;
            }
        };

        if (c_style_header.is_c_style) {
            if (!c_style_header.init_expression.empty()) {
                long long ignored_result = 0;
                if (!evaluate_arithmetic_or_fail(c_style_header.init_expression, ignored_result)) {
                    on_early_return();
                    return finalize_with_trailing_commands(rc, trailing_commands);
                }
            }

            while (true) {
                int signal_rc = 0;
                if (check_loop_interrupt(signal_rc)) {
                    rc = signal_rc;
                    break;
                }

                long long condition_value = 1;
                if ((!c_style_header.condition_expression.empty()) &&
                    (!evaluate_arithmetic_or_fail(c_style_header.condition_expression,
                                                  condition_value))) {
                    on_early_return();
                    return finalize_with_trailing_commands(rc, trailing_commands);
                }

                if (condition_value == 0) {
                    break;
                }

                auto outcome = run_iteration();
                rc = outcome.code;
                if (abort_pending()) {
                    break;
                }

                if (outcome.flow == LoopFlow::BREAK) {
                    break;
                }

                if (!c_style_header.update_expression.empty()) {
                    long long ignored_result = 0;
                    if (!evaluate_arithmetic_or_fail(c_style_header.update_expression,
                                                     ignored_result)) {
                        on_early_return();
                        return finalize_with_trailing_commands(rc, trailing_commands);
                    }
                }
            }
        } else {
            for (const auto& it : items) {
                int signal_rc = 0;
                if (check_loop_interrupt(signal_rc)) {
                    rc = signal_rc;
                    break;
                }
                assign_loop_variable(it);

                auto outcome = run_iteration();
                rc = outcome.code;
                if (abort_pending()) {
                    break;
                }
                if (outcome.flow == LoopFlow::NONE || outcome.flow == LoopFlow::CONTINUE) {
                    continue;
                }
                break;
            }
        }

        return finalize_with_trailing_commands(rc, trailing_commands);
    };

    ParsedLoopBlock parsed_loop;
    if (parse_inline_loop_block(first, parser, parsed_loop)) {
        if (!parse_header(parsed_loop.header)) {
            return 2;
        }

        auto run_cached_body = [&]() -> LoopCommandOutcome {
            // execute one iteration body then translate result into loop flow semantics
            int body_rc = execute_block(parsed_loop.body_lines);
            if (abort_pending()) {
                return {LoopFlow::BREAK, body_rc};
            }
            return handle_loop_command_result(body_rc, true);
        };

        if (!parsed_loop.done_redirections.empty() && shell && shell->executor) {
            try {
                auto redir_cmds = parser->parse_pipeline_with_preprocessing(
                    "true " + parsed_loop.done_redirections);
                if (!redir_cmds.empty()) {
                    bool action_invoked = false;
                    int exit_code = shell->executor->run_with_command_redirections(
                        redir_cmds[0],
                        [&] { return execute_for_iterations(run_cached_body, "", [] {}); }, "for",
                        false, &action_invoked);
                    if (!action_invoked) {
                        return exit_code;
                    }
                    return finalize_with_trailing_commands(exit_code,
                                                           parsed_loop.trailing_commands);
                }
            } catch (const std::exception&) {
                // Fall back to normal loop execution if redirection extraction fails.
            }
        }

        return execute_for_iterations(run_cached_body, parsed_loop.trailing_commands, [] {});
    }

    if (!parse_multiline_loop_block(src_lines, idx, first, parser, parsed_loop)) {
        idx = parsed_loop.end_index;
        return report_loop_header_error("for", "expected 'do' and 'done' to complete the loop");
    }
    if (!parse_header(parsed_loop.header)) {
        idx = parsed_loop.end_index;
        return 2;
    }

    auto run_body_and_handle_result = [&]() -> LoopCommandOutcome {
        // execute one iteration body and map break continue behavior
        int body_rc = execute_block(parsed_loop.body_lines);
        if (abort_pending()) {
            return {LoopFlow::BREAK, body_rc};
        }
        return handle_loop_command_result(body_rc, true);
    };

    int rc = execute_for_iterations(run_body_and_handle_result, parsed_loop.trailing_commands,
                                    [&] { idx = parsed_loop.end_index; });
    idx = parsed_loop.end_index;
    return rc;
}

int handle_select_block(const std::vector<std::string>& src_lines, size_t& idx,
                        const std::function<int(const std::vector<std::string>&)>& execute_block,
                        const std::function<int(const std::string&)>& execute_simple_or_pipeline,
                        Parser* parser, const std::function<bool()>& should_abort_execution) {
    if (config::is_posix_mode()) {
        (void)report_loop_header_error("select", "select is disabled in POSIX mode");
        return cjsh_env::posix_error_exit(2);
    }
    std::string first = trim(strip_inline_comment(src_lines[idx]));
    if (!parser_starts_with_keyword_token(first, "select") && first.rfind("select;", 0) != 0) {
        return 1;
    }

    std::string var;
    std::vector<std::string> items;
    auto abort_pending = [&] {
        return cjsh_env::exit_requested() || (should_abort_execution && should_abort_execution());
    };

    auto finalize_with_trailing_commands = [&](int loop_rc, const std::string& trailing_commands) {
        return execute_loop_trailing_commands(
            loop_rc, trailing_commands, execute_simple_or_pipeline, parser, should_abort_execution);
    };

    auto assign_select_variable = [&](const std::string& name, const std::string& value) {
        if (shell != nullptr &&
            cjsh_env::set_shell_or_local_variable_value(shell.get(), name, value)) {
            return;
        }

        (void)cjsh_env::set_shell_variable_value(name, value);
    };

    auto parse_header = [&](const std::string& header) -> bool {
        const auto parsed = parse_named_loop_header(header, "select", config::is_posix_mode());
        if (!parsed.error.empty()) {
            report_loop_header_error("select", parsed.error);
            return false;
        }
        if (parsed.has_in && parsed.words.empty()) {
            report_loop_header_error("select", "expected selection words after 'in'");
            return false;
        }

        var = parsed.variable;
        if (parsed.has_in) {
            const std::string expanded_words =
                expand_loop_substitutions(parsed.words, execute_simple_or_pipeline);
            auto toks = parser->parse_command("select " + var + " in " + expanded_words);
            if (toks.size() < 3) {
                report_loop_header_error("select", "could not parse selection words after 'in'");
                return false;
            }
            items.assign(toks.begin() + 3, toks.end());
        } else {
            items = flags::get_positional_parameters();
        }

        return true;
    };

    auto execute_select_iterations = [&](const std::vector<std::string>& body_lines,
                                         const std::string& trailing_commands) -> int {
        if (items.empty()) {
            return finalize_with_trailing_commands(0, trailing_commands);
        }

        int rc = 0;
        bool show_menu = true;
        while (true) {
            int signal_rc = 0;
            if (check_loop_interrupt(signal_rc)) {
                rc = signal_rc;
                break;
            }

            if (show_menu) {
                print_select_menu(items);
                show_menu = false;
            }

            std::cerr << get_select_prompt() << std::flush;

            std::string reply;
            if (!std::getline(std::cin, reply)) {
                std::cerr << '\n';
                rc = 1;
                break;
            }

            assign_select_variable("REPLY", reply);

            if (reply.empty()) {
                show_menu = true;
                continue;
            }

            std::string selected_value;
            if (auto selected_index = parse_select_choice(reply, items.size())) {
                selected_value = items[*selected_index];
            }
            assign_select_variable(var, selected_value);

            int body_rc = execute_block(body_lines);
            if (abort_pending()) {
                rc = body_rc;
                break;
            }

            auto outcome = handle_loop_command_result(body_rc, true);
            rc = outcome.code;
            if (outcome.flow == LoopFlow::BREAK) {
                break;
            }
            if (outcome.flow == LoopFlow::CONTINUE) {
                continue;
            }
        }

        return finalize_with_trailing_commands(rc, trailing_commands);
    };

    ParsedLoopBlock parsed_loop;
    if (parse_inline_loop_block(first, parser, parsed_loop)) {
        if (!parse_header(parsed_loop.header)) {
            return 2;
        }

        return execute_select_iterations(parsed_loop.body_lines, parsed_loop.trailing_commands);
    }

    if (!parse_multiline_loop_block(src_lines, idx, first, parser, parsed_loop)) {
        idx = parsed_loop.end_index;
        return report_loop_header_error("select", "expected 'do' and 'done' to complete the loop");
    }
    if (!parse_header(parsed_loop.header)) {
        idx = parsed_loop.end_index;
        return 2;
    }

    int rc = execute_select_iterations(parsed_loop.body_lines, parsed_loop.trailing_commands);
    idx = parsed_loop.end_index;
    return rc;
}

int handle_condition_loop_block(
    LoopCondition condition, const std::vector<std::string>& src_lines, size_t& idx,
    const std::function<int(const std::vector<std::string>&)>& execute_block,
    const std::function<int(const std::string&)>& execute_simple_or_pipeline, Parser* parser,
    const std::function<bool()>& should_abort_execution) {
    // thin dispatcher that maps while/until into the shared loop-block implementation
    const char* keyword = condition == LoopCondition::WHILE ? "while" : "until";
    bool is_until = condition == LoopCondition::UNTIL;
    return handle_loop_block(src_lines, idx, keyword, is_until, execute_block,
                             execute_simple_or_pipeline, parser, should_abort_execution);
}

std::optional<int> try_execute_inline_do_block(
    const std::string& first_segment, const std::vector<std::string>& segments,
    size_t& segment_index,
    const std::function<int(const std::vector<std::string>&, size_t&)>& handler) {
    // reconstruct split loop fragments into one executable inline block for a single handler call
    if (first_segment.find("; do") != std::string::npos) {
        return std::nullopt;
    }

    size_t lookahead = segment_index + 1;
    if (lookahead >= segments.size()) {
        return std::optional<int>{report_inline_loop_syntax_error(first_segment, "do")};
    }

    std::string next_segment = trim(strip_inline_comment(segments[lookahead]));
    if (next_segment != "do" && next_segment.rfind("do ", 0) != 0) {
        return std::optional<int>{report_inline_loop_syntax_error(first_segment, "do")};
    }

    std::string body = next_segment.size() > 3 && next_segment.rfind("do ", 0) == 0
                           ? trim(next_segment.substr(3))
                           : "";

    size_t scan = lookahead + 1;
    bool found_done = false;
    std::string done_suffix;
    for (; scan < segments.size(); ++scan) {
        std::string seg = trim(strip_inline_comment(segments[scan]));
        if (seg == "done" || parser_starts_with_keyword_token(seg, "done")) {
            found_done = true;
            done_suffix = trim(seg.substr(4));
            break;
        }
        if (!body.empty()) {
            body += "; ";
        }
        body += seg;
    }

    if (!found_done) {
        return std::optional<int>{report_inline_loop_syntax_error(first_segment, "done")};
    }

    std::string combined = first_segment + "; do";
    if (!body.empty()) {
        combined += " " + body;
    }
    combined += "; done";
    if (!done_suffix.empty()) {
        combined += " " + done_suffix;
    }

    size_t local_idx = 0;
    std::vector<std::string> inline_lines{combined};
    int rc = handler(inline_lines, local_idx);
    segment_index = scan;
    return std::optional<int>{rc};
}

}  // namespace loop_evaluator
