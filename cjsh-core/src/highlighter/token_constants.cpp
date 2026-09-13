/*
  token_constants.cpp

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

#include "token_constants.h"
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "redirection_utils.h"

namespace token_constants {

const std::unordered_set<std::string>& comparison_operators() {
    static const std::unordered_set<std::string> kComparisonOperators = {
        "=",   "==",  "!=",  "<",   "<=",  ">",   ">=",  "-eq",
        "-ne", "-gt", "-ge", "-lt", "-le", "-ef", "-nt", "-ot"};
    return kComparisonOperators;
}

const std::unordered_set<std::string>& shell_keywords() {
    static const std::unordered_set<std::string> kShellKeywords = {
        "if",    "then", "else", "elif", "fi",       "case",   "in",   "esac",   "while",
        "until", "for",  "do",   "done", "function", "select", "time", "coproc", ":"};
    return kShellKeywords;
}

const std::vector<std::string>& shell_control_structure_keywords() {
    static const std::vector<std::string> kControlStructureKeywords = {
        "if",  "then",   "elif",  "else",  "fi", "case", "esac",
        "for", "select", "while", "until", "do", "done", "function"};
    return kControlStructureKeywords;
}

const std::unordered_set<std::string>& shell_control_structure_leaders() {
    static const std::unordered_set<std::string> kControlStructureLeaders = {
        "if", "for", "while", "until", "case", "select", "function"};
    return kControlStructureLeaders;
}

const std::unordered_set<std::string>& inline_command_keywords() {
    static const std::unordered_set<std::string> kInlineCommandKeywords = {
        "do", "then", "else", "elif", "if", "while", "until", "time", "coproc"};
    return kInlineCommandKeywords;
}

const std::unordered_set<std::string>& loop_keywords() {
    static const std::unordered_set<std::string> kLoopKeywords = {"for", "while", "until",
                                                                  "select"};
    return kLoopKeywords;
}

const std::unordered_set<std::string>& redirection_operators() {
    static const std::unordered_set<std::string> kRedirectionOperators = [] {
        std::unordered_set<std::string> operators;
        for (std::string_view op : redirection_utils::canonical_operator_spellings()) {
            (void)operators.emplace(op);
        }
        (void)operators.emplace("&>>");
        (void)operators.emplace("|&");
        (void)operators.emplace("1>");
        (void)operators.emplace("1>>");
        (void)operators.emplace("1>&2");
        (void)operators.emplace("1<");
        (void)operators.emplace("2<");
        (void)operators.emplace("0<");
        (void)operators.emplace("0>");
        (void)operators.emplace("3>");
        (void)operators.emplace("4>");
        (void)operators.emplace("5>");
        (void)operators.emplace("6>");
        (void)operators.emplace("7>");
        (void)operators.emplace("8>");
        (void)operators.emplace("9>");
        return operators;
    }();
    return kRedirectionOperators;
}

const std::unordered_map<std::string, std::string>& default_styles() {
    static const std::unordered_map<std::string, std::string> kDefaultStyles = {
        {"unknown-command", "color=#FFFFFF underline underline-color=#FF0000"},
        {"agent-prefix", "bold color=#8BE9FD"},
        {"agent-request", "color=#F8F8F2"},
        {"colon", "bold color=#8BE9FD"},
        {"file-argument", "color=#8BE9FD"},
        {"path-exists", "color=#50FA7B"},
        {"path-not-exists", "color=#FF5555"},
        {"glob-pattern", "color=#F1FA8C"},
        {"operator", "bold color=#FF79C6"},
        {"keyword", "bold color=#BD93F9"},
        {"builtin", "color=#FFB86C"},
        {"system", "color=#50FA7B"},
        {"variable", "color=#8BE9FD"},
        {"assignment-value", "color=#F8F8F2"},
        {"string", "color=#F1FA8C"},
        {"heredoc-delimiter", "bold color=#F1FA8C"},
        {"comment", "color=#6272A4"},
        {"command-substitution", "color=#8BE9FD"},
        {"arithmetic", "color=#FF79C6"},
        {"option", "color=#BD93F9"},
        {"number", "color=#FFB86C"},
        {"function-definition", "bold color=#F1FA8C"},
        {"history-expansion", "bold color=#FF79C6"},
        {"ic-prompt", "ansi-green"},
        {"ic-linenumbers", "ansi-lightgray"},
        {"ic-linenumber-current", "ansi-yellow"},
        {"ic-info", "ansi-darkgray"},
        {"ic-source", "#ffffd7"},
        {"ic-diminish", "ansi-lightgray"},
        {"ic-emphasis", "#ffffd7"},
        {"ic-hint", "ansi-darkgray"},
        {"ic-error", "#d70000"},
        {"ic-bracematch", "ansi-white"},
        {"ic-whitespace-char", "ansi-lightgray"}};
    return kDefaultStyles;
}

}  // namespace token_constants
