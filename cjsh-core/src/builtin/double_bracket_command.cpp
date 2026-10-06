/*
  double_bracket_command.cpp

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

#include "double_bracket_command.h"

#include "builtin_help.h"
#include "error_out.h"
#include "pattern_matcher.h"
#include "shell_env.h"
#include "test_expression_utils.h"

#include <cstdlib>
#include <cstring>
#include <regex>
#include <string>
#include <vector>

namespace {

PatternMatcher g_pattern_matcher;

bool has_pattern_syntax(const std::string& pattern) {
    if (pattern.find('*') != std::string::npos || pattern.find('?') != std::string::npos ||
        pattern.find('[') != std::string::npos) {
        return true;
    }
    if (!config::extglob_enabled) {
        return false;
    }
    return pattern.find("+(") != std::string::npos || pattern.find("@(") != std::string::npos ||
           pattern.find("!(") != std::string::npos;
}

int evaluate_expression(const std::vector<std::string>& tokens) {
    if (tokens.empty()) {
        return 1;
    }

    if (tokens.size() == 1) {
        return tokens[0].empty() ? 1 : 0;
    }

    if (tokens.size() == 2) {
        const std::string& op = tokens[0];
        const std::string& arg = tokens[1];

        if (test_expression_utils::is_basic_unary_operator(op)) {
            return test_expression_utils::evaluate_basic_unary_operator(op, arg) ? 0 : 1;
        } else if (op == "!") {
            return arg.empty() ? 0 : 1;
        }
    }

    if (tokens.size() == 3) {
        if (tokens[0] == "!") {
            std::vector<std::string> sub_tokens(tokens.begin() + 1, tokens.end());
            return evaluate_expression(sub_tokens) == 0 ? 1 : 0;
        }

        const std::string& arg1 = tokens[0];
        const std::string& op = tokens[1];
        const std::string& arg2 = tokens[2];

        if (op == "=" || op == "==") {
            if (!has_pattern_syntax(arg2)) {
                return arg1 == arg2 ? 0 : 1;
            }
            return g_pattern_matcher.matches_pattern(arg1, arg2) ? 0 : 1;
        } else if (op == "!=") {
            if (!has_pattern_syntax(arg2)) {
                return arg1 != arg2 ? 0 : 1;
            }
            return g_pattern_matcher.matches_pattern(arg1, arg2) ? 1 : 0;
        } else if (op == "=~") {
            try {
                std::regex re(arg2);
                return std::regex_search(arg1, re) ? 0 : 1;
            } catch (const std::regex_error&) {
                return 1;
            }
        }

        auto numeric_result = test_expression_utils::evaluate_numeric_comparison(arg1, op, arg2);
        if (numeric_result.has_value()) {
            return *numeric_result ? 0 : 1;
        }
    }

    if (tokens.size() == 4 && tokens[0] == "!") {
        std::vector<std::string> sub_tokens(tokens.begin() + 1, tokens.end());
        return evaluate_expression(sub_tokens) == 0 ? 1 : 0;
    }

    return 1;
}

bool is_binary_op(const std::string& op) {
    return op == "=" || op == "==" || op == "!=" || op == "=~" || op == "-eq" || op == "-ne" ||
           op == "-lt" || op == "-le" || op == "-gt" || op == "-ge" || op == "-ef" || op == "-nt" ||
           op == "-ot";
}

}  // namespace

int double_bracket_command(const std::vector<std::string>& args) {
    if (builtin_handle_help(
            args,
            {"Usage: [[ EXPRESSION ]]",
             "Evaluate a conditional expression with pattern matching and logical operators."})) {
        return 0;
    }
    if (args.empty()) {
        return 1;
    }

    if (args[0] == "[[" && (args.size() == 1 || args.back() != "]]")) {
        print_error({ErrorType::SYNTAX_ERROR, "[[", "missing closing ']]'", {}});
        return 2;
    }

    std::vector<std::string> expression_args = args;

    if (args[0] == "[[" && args.size() > 1 && args.back() == "]]") {
        expression_args.pop_back();
        (void)expression_args.erase(expression_args.begin());
    } else if (args[0] == "[[") {
        (void)expression_args.erase(expression_args.begin());
    }

    if (expression_args.empty()) {
        return 1;
    }

    if ((expression_args.size() == 1 && is_binary_op(expression_args[0])) ||
        (expression_args.size() == 2 && is_binary_op(expression_args[1]))) {
        print_error({ErrorType::SYNTAX_ERROR, "[[", "missing operand", {}});
        return 2;
    }

    std::vector<std::vector<std::string>> expressions;
    std::vector<std::string> operators;
    std::vector<std::string> current_expr;

    for (const auto& token : expression_args) {
        if (token == "&&" || token == "||") {
            if (!current_expr.empty()) {
                expressions.push_back(current_expr);
                current_expr.clear();
            }
            operators.push_back(token);
        } else {
            current_expr.push_back(token);
        }
    }

    if (!current_expr.empty()) {
        expressions.push_back(current_expr);
    }

    if (expressions.empty()) {
        return 1;
    }

    int result = evaluate_expression(expressions[0]);

    for (size_t i = 0; i < operators.size() && i + 1 < expressions.size(); ++i) {
        if (operators[i] == "&&") {
            if (result == 0) {
                result = evaluate_expression(expressions[i + 1]);
            }

        } else if ((operators[i] == "||") && (result != 0)) {
            result = evaluate_expression(expressions[i + 1]);
        }
    }

    return result;
}
