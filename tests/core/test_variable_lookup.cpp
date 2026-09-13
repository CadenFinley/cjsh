/*
  test_variable_lookup.cpp

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

#include <fnmatch.h>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "flags.h"
#include "interpreter.h"
#include "parameter_expansion_evaluator.h"
#include "parser.h"
#include "pattern_matcher.h"
#include "shell.h"
#include "shell_env.h"

std::unique_ptr<Shell> g_shell;

namespace {
bool expect(bool condition, const char* message) {
    if (!condition) {
        (void)std::fprintf(stderr, "[FAIL] %s\n", message);
    }
    return condition;
}

bool test_environment_import() {
    bool ok = true;
    const std::string long_value(8192, 'v');
    const std::vector<std::pair<std::string, std::string>> values = {
        {"__import_empty", ""},
        {"__import_equals", "one=two=three"},
        {"__import_long", long_value},
    };
    for (const auto& [name, value] : values) {
        setenv(name.c_str(), value.c_str(), 1);
    }
    cjsh_env::set_shell_variable_value("__import_shell_only", "retained");
    cjsh_env::sync_env_vars_from_system(*g_shell);
    for (const auto& [name, value] : values) {
        setenv(name.c_str(), "changed", 1);
        ok = expect(cjsh_env::shell_variable_is_set(name) &&
                        cjsh_env::get_shell_variable_value(name) == value &&
                        g_shell->get_parser()->parse_command(": \"$" + name + "\"") ==
                            std::vector<std::string>({":", value}),
                    "import owns values and preserves empty values and embedded equals") &&
             ok;
    }
    cjsh_env::sync_env_vars_from_system(*g_shell);
    for (const auto& [name, value] : values) {
        ok = expect(cjsh_env::get_shell_variable_value(name) == "changed",
                    "a subsequent import updates existing variables") &&
             ok;
        unsetenv(name.c_str());
        cjsh_env::unset_shell_variable_value(name);
    }
    ok = expect(cjsh_env::get_shell_variable_value("__import_shell_only") == "retained",
                "import preserves shell variables absent from the process environment") &&
         ok;
    cjsh_env::unset_shell_variable_value("__import_shell_only");
    return ok;
}

bool test_scalar_and_nameref_transitions() {
    VariableManager variables;
    bool ok = true;
    auto check = [&](const std::string& name, const std::string& value, bool present) {
        ok = expect(variables.get_variable_value(name) == value &&
                        variables.variable_is_set(name) == present,
                    ("scalar/nameref transition: " + name).c_str()) &&
             ok;
    };
    variables.set_environment_variable("__transition_value", "first");
    variables.set_environment_variable("__transition_empty", "");
    check(" \t__transition_value\n", "first", true);
    check("__transition_empty", "", true);
    check("__transition_missing", "", false);
    const std::string long_name = "__transition_" + std::string(512, 'x');
    variables.set_environment_variable(long_name, "long");
    check(long_name, "long", true);
    ok = expect(variables.assign_global_array_literal("__transition_array", {"[2]=two"}),
                "array reads work without namerefs") &&
         ok;
    check("__transition_array[2]", "two", true);
    check("__transition_array", "", false);
    check("__transition_array[]", "", false);

    variables.push_scope();
    ok = expect(variables.set_nameref("__transition_ref", "__transition_value"),
                "create local nameref after scalar lookups") &&
         ok;
    check("__transition_ref", "first", true);
    variables.set_environment_variable("__transition_value", "changed");
    check("__transition_ref", "changed", true);
    variables.push_scope();
    check("__transition_ref", "", false);
    check("__transition_value", "changed", true);
    variables.pop_scope();
    check("__transition_ref", "changed", true);
    ok = expect(variables.set_nameref("__transition_ref", "__transition_array"),
                "retarget local nameref to an array") &&
         ok;
    check("__transition_ref[2]", "two", true);
    variables.pop_scope();
    check("__transition_ref", "", false);
    ok = expect(variables.set_nameref("__transition_ref", "__transition_empty", true),
                "create global nameref after leaving local scope") &&
         ok;
    variables.push_scope();
    check("__transition_ref", "", true);
    variables.pop_scope();
    ok = expect(variables.unset_nameref("__transition_ref"), "remove last global nameref") && ok;
    check("__transition_ref", "", false);
    check("__transition_value", "changed", true);
    for (const auto& name :
         {std::string("__transition_value"), std::string("__transition_empty"), long_name}) {
        cjsh_env::unset_shell_variable_value(name);
    }
    return ok;
}

bool test_variable_presence_and_scope() {
    auto& variables = g_shell->get_shell_script_interpreter()->get_variable_manager();
    bool ok = true;
    auto check = [&](const std::string& name, const std::string& value, bool present) {
        ok = expect(variables.get_variable_value(name) == value, (name + " value").c_str()) && ok;
        ok = expect(variables.variable_is_set(name) == present, (name + " presence").c_str()) && ok;
    };
    check("__lookup_missing", "", false);
    variables.set_environment_variable("__lookup_scalar", "");
    check("__lookup_scalar", "", true);
    check("__lookup_scalar[0]", "", true);
    check("__lookup_scalar[1]", "", false);
    variables.set_environment_variable("__lookup_scalar", "outer");
    check("__lookup_scalar[@]", "outer", true);
    setenv("__lookup_process", "", 1);
    check("__lookup_process", "", true);
    unsetenv("__lookup_process");

    ok = expect(variables.assign_global_array_literal("__lookup_array", {"[2]=", "[5]=five"}),
                "create sparse array") &&
         ok;
    check("__lookup_array", "", false);
    check("__lookup_array[2]", "", true);
    check("__lookup_array[1+4]", "five", true);
    check("__lookup_array[4]", "", false);
    check("__lookup_array[@]", " five", true);
    ok =
        expect(variables.assign_global_array_literal("__lookup_empty", {}), "create empty array") &&
        ok;
    check("__lookup_empty[@]", "", false);
    ok = expect(variables.assign_global_associative_literal("__lookup_assoc",
                                                            {"[label]=", "[other]=value"}),
                "create associative array") &&
         ok;
    check("__lookup_assoc[label]", "", true);
    check("__lookup_assoc[other]", "value", true);
    check("__lookup_assoc[missing]", "", false);
    variables.set_environment_variable("__lookup_key", "other");
    check("__lookup_assoc[$__lookup_key]", "value", true);
    ok = expect(variables.set_nameref("__lookup_ref", "__lookup_array", true), "create nameref") &&
         ok;
    check("__lookup_ref[5]", "five", true);

    variables.push_scope();
    variables.set_local_variable("__lookup_scalar", "local");
    check("__lookup_scalar", "local", true);
    variables.set_local_variable("__lookup_array", "");
    ok = expect(variables.assign_array_literal("__lookup_array", {"[1]=local"}),
                "create local array") &&
         ok;
    check("__lookup_ref[1]", "local", true);
    check("__lookup_ref[5]", "", false);
    variables.set_local_variable("__lookup_assoc", "");
    ok = expect(variables.assign_associative_literal("__lookup_assoc", {"[label]=local"}),
                "create local associative array") &&
         ok;
    check("__lookup_assoc[label]", "local", true);
    check("__lookup_assoc[other]", "", false);
    variables.pop_scope();
    check("__lookup_scalar", "outer", true);
    check("__lookup_ref[5]", "five", true);
    check("__lookup_assoc[other]", "value", true);

    flags::set_positional_parameters({"", "second"});
    check("1", "", true);
    check("2", "second", true);
    check("3", "", false);
    check("999999999999999999999999999999999", "", false);
    check("#", "2", true);
    check("@", " second", true);
    return ok;
}

bool test_parameter_expansion_work() {
    size_t presence_checks = 0;
    size_t pattern_calls = 0;
    std::string value(4096, 'a');
    bool present = true;
    ParameterExpansionEvaluator evaluator([&](const std::string&) { return value; },
                                          [&](const std::string&, const std::string& replacement) {
                                              value = replacement;
                                              present = true;
                                          },
                                          [&](const std::string&) {
                                              ++presence_checks;
                                              return present;
                                          },
                                          [&](const std::string& text, const std::string& pattern) {
                                              ++pattern_calls;
                                              return fnmatch(pattern.c_str(), text.c_str(), 0) == 0;
                                          });
    bool ok = true;
    for (const char* expression : {"v##*", "v%%*"}) {
        presence_checks = pattern_calls = 0;
        ok = expect(evaluator.expand(expression).empty(), "longest wildcard removes full value") &&
             ok;
        ok = expect(pattern_calls == 1, "longest wildcard stops after first match") && ok;
        ok = expect(presence_checks == 0, "pattern removal does not query presence") && ok;
    }
    for (const char* expression : {"v", "v#*", "v%*"}) {
        presence_checks = 0;
        ok = expect(evaluator.expand(expression) == value,
                    "plain/shortest expansion retains value") &&
             ok;
        ok = expect(presence_checks == 0, "value-only expansion does not query presence") && ok;
    }
    value = "abcabc";
    ok = expect(evaluator.expand("v##*b") == "c", "longest prefix chooses furthest match") && ok;
    ok = expect(evaluator.expand("v#*b") == "cabc", "shortest prefix chooses nearest match") && ok;
    ok = expect(evaluator.expand("v%%b*") == "a", "longest suffix chooses furthest match") && ok;
    ok = expect(evaluator.expand("v%b*") == "abca", "shortest suffix chooses nearest match") && ok;
    ok = expect(evaluator.expand("v##z*") == value, "unmatched longest prefix retains value") && ok;
    ok = expect(evaluator.expand("v%%*z") == value, "unmatched longest suffix retains value") && ok;
    value.clear();
    ok = expect(evaluator.expand("v-fallback").empty(), "empty binding is set") && ok;
    ok = expect(evaluator.expand("v:-fallback") == "fallback",
                "colon default treats empty as null") &&
         ok;
    present = false;
    ok = expect(evaluator.expand("v-fallback") == "fallback", "unset binding uses default") && ok;
    return ok;
}

bool test_parameter_replacement() {
    std::string value;
    PatternMatcher matcher;
    ParameterExpansionEvaluator evaluator(
        [&](const std::string&) { return value; },
        [&](const std::string&, const std::string& replacement) { value = replacement; },
        [](const std::string&) { return true; },
        [&](const std::string& text, const std::string& pattern) {
            return matcher.matches_pattern(text, pattern);
        });
    const struct {
        const char* value;
        const char* expression;
        const char* expected;
    } cases[] = {
        {"hello hello", "v/hello/hi", "hi hello"},
        {"hello hello", "v//hello/hi", "hi hi"},
        {"aaaaa", "v//aa/X", "XXa"},
        {"aaaa", "v//aa/aaa", "aaaaaa"},
        {"abc", "v//b/", "ac"},
        {"abc", "v//abc/", ""},
        {"abc", "v//longer/X", "abc"},
        {"abcabc", "v/#abc/X", "Xabc"},
        {"abcabc", "v/%abc/X", "abcX"},
        {"aaab", "v/a*/X", "X"},
        {"abbcab", "v/a*b/X", "X"},
        {"abbcab", "v//?/X", "XXXXXX"},
        {"abbcab", "v//[!a]/X", "aXXXaX"},
        {"hello world", "v/[hw]/X", "Xello world"},
        {"a*b*a", "v/\\*/X", "aXb*a"},
        {"a*b*a", "v/'*'/X", "aXb*a"},
        {"root/sub/leaf", "v/\\//-", "root-sub/leaf"},
        {"echo line", "v/echo/a\\/b", "a/b line"},
        {"", "v//a/X", ""},
    };
    bool ok = true;
    for (const auto& entry : cases) {
        value = entry.value;
        ok = expect(evaluator.expand(entry.expression) == entry.expected, entry.expression) && ok;
    }
    value.assign(4096, 'a');
    ok = expect(evaluator.expand("v//missing/X") == value,
                "unmatched literal replacement retains a long value") &&
         ok;

    const bool previous_extglob = config::extglob_enabled;
    config::extglob_enabled = true;
    value = "foo bar foo";
    ok = expect(evaluator.expand("v//@(foo|bar)/X") == "X X X",
                "extended patterns retain global replacement semantics") &&
         ok;
    config::extglob_enabled = previous_extglob;
    return ok;
}

bool test_pattern_matching() {
    PatternMatcher matcher;
    bool ok = true;
    // Compare ordinary glob combinations against an independent matcher. Include
    // empty inputs, repeated stars, failed suffixes and character classes.
    std::vector<std::string> patterns{""};
    std::vector<std::string> texts{""};
    for (int length = 0; length < 3; ++length) {
        const auto previous_patterns = patterns;
        const auto previous_texts = texts;
        for (const auto& prefix : previous_patterns) {
            for (const char* token : {"a", "b", "?", "*", "[ab]", "[!a]"}) {
                patterns.push_back(prefix + token);
            }
        }
        for (const auto& prefix : previous_texts) {
            for (char character : {'a', 'b', '.'}) {
                texts.push_back(prefix + character);
            }
        }
    }
    for (const auto& pattern : patterns) {
        for (const auto& text : texts) {
            if (matcher.matches_pattern(text, pattern) !=
                (fnmatch(pattern.c_str(), text.c_str(), 0) == 0)) {
                return expect(false, ("pattern " + pattern + " against " + text).c_str());
            }
        }
    }
    const struct {
        const char* text;
        const char* pattern;
        bool expected;
    } cases[] = {
        {"*?", "'*?'", true},   {"abc", "'*'", false},      {"a*b", "a\\*b", true},
        {"abc", "a**?c", true}, {"ababxc", "*ab?c", true},  {"ababxc", "*ab?d", false},
        {"ab/cd", "a*d", true}, {"a\nb", "a?b", true},      {"é", "??", true},
        {"é", "?", false},      {"7", "[[:digit:]]", true}, {"z", "[![:digit:]]", true},
        {"[", "[", true},       {"]", "[]]", true},         {"|", "'|'", true},
    };
    for (const auto& entry : cases) {
        ok = expect(matcher.matches_pattern(entry.text, entry.pattern) == entry.expected,
                    entry.pattern) &&
             ok;
    }
    const std::string long_value(4096, 'a');
    ok = expect(matcher.matches_pattern(long_value, "*a*"), "long wildcard full match") && ok;
    ok = expect(!matcher.matches_pattern(long_value, "*a*z"), "long wildcard suffix miss") && ok;

    const bool previous_extglob = config::extglob_enabled;
    for (bool enabled : {true, false, true}) {
        config::extglob_enabled = enabled;
        ok = expect(matcher.matches_pattern("foo", "@(foo|bar)") == enabled,
                    "matching reflects changes to extglob") &&
             ok;
    }
    config::extglob_enabled = true;
    ok = expect(matcher.matches_pattern("abab", "+(a|b)"), "extended repetition") && ok;
    ok = expect(matcher.matches_pattern("abc", "!(foo|bar)"), "extended negation") && ok;
    ok = expect(!matcher.matches_pattern("foo", "!(foo|bar)"), "extended negation miss") && ok;
    for (bool alternatives : {true, false, true}) {
        ok = expect(matcher.matches_pattern("foo", "foo|bar", alternatives) == alternatives,
                    "top-level alternatives remain per-call") &&
             ok;
    }
    config::extglob_enabled = previous_extglob;
    return ok;
}

bool test_extended_pattern_frontiers() {
    const bool previous_extglob = config::extglob_enabled;
    config::extglob_enabled = true;
    PatternMatcher matcher;
    bool ok = true;
    const struct {
        const char* text;
        const char* pattern;
        bool expected;
    } cases[] = {
        {"", "*(a|)", true},
        {"", "+(a|)", true},
        {"", "+(a)", false},
        {"aaab", "+(a|aa)b", true},
        {"aaac", "+(a|aa)b", false},
        {"aaab", "*(a|aa|)b", true},
        {"b", "*(a|aa|)b", true},
        {"abab", "+(@(a|ab)|b)", true},
        {"abac", "+(@(a|ab)|b)", false},
        {"abcd", "@(*a*b*|*a*c*)d", true},
        {"abcd", "@(*a*b*|*a*c*)e", false},
        {"ab", "?(a|ab)b", true},
        {"b", "?(a|ab)b", true},
        {"aaab", "!(a|aa)b", true},
        {"aab", "!(a|aa)b", false},
        {"b", "!(|a)b", false},
        {"aab", "!(|a)b", true},
        {"7b", "@([[:digit:]]|[ab])b", true},
        {"a*b", "@(a\\*|b)b", true},
    };
    for (const auto& entry : cases) {
        ok = expect(matcher.matches_pattern(entry.text, entry.pattern, false) == entry.expected,
                    entry.pattern) &&
             ok;
    }
    const std::string repeated(4096, 'a');
    ok = expect(matcher.matches_pattern(repeated + "b", "+(a|aa|)b", false),
                "overlapping repetitions retain all reachable endpoints") &&
         ok;
    ok = expect(!matcher.matches_pattern(repeated + "c", "+(a|aa|)b", false),
                "overlapping repetitions reject a failed suffix") &&
         ok;
    ok = expect(!matcher.matches_pattern(repeated + "c", "@(*a*a*a*a)b", false),
                "equivalent star splits must not be explored repeatedly") &&
         ok;
    ok = expect(matcher.matches_pattern(repeated, "@(" + repeated + ")", false),
                "long sequences inside groups do not recurse per literal") &&
         ok;
    config::extglob_enabled = previous_extglob;
    return ok;
}

bool test_literal_pattern_removal() {
    std::string value;
    PatternMatcher matcher;
    ParameterExpansionEvaluator evaluator(
        [&](const std::string&) { return value; },
        [&](const std::string&, const std::string& replacement) { value = replacement; },
        [](const std::string&) { return true; },
        [&](const std::string& text, const std::string& pattern) {
            return matcher.matches_pattern(text, pattern);
        });
    const struct {
        const char* value;
        const char* pattern;
        const char* prefix;
        const char* suffix;
    } cases[] = {
        {"abcabc", "abc", "abc", "abc"},
        {"abc", "abc", "", ""},
        {"abc", "abcd", "abc", "abc"},
        {"abc", "missing", "abc", "abc"},
        {"abc", "bc", "abc", "a"},
        {"abc", "ab", "c", "abc"},
        {"", "abc", "", ""},
        {"abc", "", "abc", "abc"},
        {"a.b/a.b", "a.b", "/a.b", "a.b/"},
        {"échoé", "é", "choé", "écho"},
        {"*abc*", "\\*", "abc*", "*abc"},
        {"*abc*", "'*'", "abc*", "*abc"},
        {"?abc?", "\"?\"", "abc?", "?abc"},
        {"abc", "[ac]", "bc", "ab"},
    };
    bool ok = true;
    for (const auto& entry : cases) {
        value = entry.value;
        for (const char* op : {"#", "##", "%", "%%"}) {
            const std::string expression = std::string("v") + op + entry.pattern;
            ok =
                expect(evaluator.expand(expression) == (op[0] == '#' ? entry.prefix : entry.suffix),
                       expression.c_str()) &&
                ok;
        }
    }
    value.assign(16384, 'a');
    ok = expect(evaluator.expand("v##missing") == value, "long literal prefix miss") && ok;
    ok = expect(evaluator.expand("v%%missing") == value, "long literal suffix miss") && ok;
    ok = expect(evaluator.expand("v/#a/X") == "X" + value.substr(1),
                "anchored prefix replacement") &&
         ok;
    ok = expect(evaluator.expand("v/%a/X") == value.substr(1) + "X",
                "anchored suffix replacement") &&
         ok;
    const bool previous_extglob = config::extglob_enabled;
    config::extglob_enabled = true;
    value = "foobarfoo";
    ok = expect(evaluator.expand("v##@(foo|bar)") == "barfoo", "extended prefix removal") && ok;
    ok = expect(evaluator.expand("v%%@(foo|bar)") == "foobar", "extended suffix removal") && ok;
    config::extglob_enabled = previous_extglob;
    return ok;
}

bool test_pattern_endpoints_and_expansion() {
    PatternMatcher matcher;
    bool ok = true;
    const bool previous_extglob = config::extglob_enabled;
    config::extglob_enabled = true;
    std::vector<std::string> patterns{"",  "'a*'", "\\*",   "[[:digit:]]", "[!a]",  "[]a]",
                                      "[", "a|b",  "**a**", "*a*b*",       "@(a|b)"};
    const std::vector<std::string> atoms{"a", "b", "?", "*", "[ab]"};
    for (const auto& first : atoms) {
        patterns.push_back(first);
        for (const auto& second : atoms) {
            patterns.push_back(first + second);
            for (const auto& third : atoms) {
                patterns.push_back(first + second + third);
            }
        }
    }
    std::vector<std::string> values{"", "a*b", "a1b", "a|b", "aaaaab", "ababab"};
    for (char first : {'a', 'b', '.'}) {
        values.emplace_back(1, first);
        for (char second : {'a', 'b', '.'}) {
            values.push_back(std::string{first, second});
            for (char third : {'a', 'b', '.'}) {
                values.push_back(std::string{first, second, third});
            }
        }
    }
    std::string value;
    auto reader = [&](const std::string&) { return value; };
    auto writer = [](const std::string&, const std::string&) {};
    auto checker = [](const std::string&) { return true; };
    auto match = [&](const std::string& text, const std::string& pattern) {
        return matcher.matches_pattern(text, pattern);
    };
    ParameterExpansionEvaluator reference(reader, writer, checker, match);
    ParameterExpansionEvaluator optimized(
        reader, writer, checker, match, nullptr, nullptr, nullptr, nullptr,
        [&](const std::string& text, const std::string& pattern, bool longest) {
            return matcher.match_end_positions(text, pattern, longest);
        });
    for (const auto& pattern : patterns) {
        for (const auto& text : values) {
            value = text;
            for (bool longest : {false, true}) {
                const auto ends = matcher.match_end_positions(text, pattern, longest);
                if (pattern == "@(a|b)") {
                    ok = expect(!ends, "extended groups request the general matcher") && ok;
                    continue;
                }
                if (!expect(ends && ends->size() == text.size() + 1, "endpoint table size")) {
                    config::extglob_enabled = previous_extglob;
                    return false;
                }
                for (size_t begin = 0; begin <= text.size(); ++begin) {
                    size_t expected = std::string::npos;
                    for (size_t end = begin; end <= text.size(); ++end) {
                        if (matcher.matches_pattern(text.substr(begin, end - begin), pattern)) {
                            expected = end;
                            if (!longest) {
                                break;
                            }
                        }
                    }
                    ok = expect((*ends)[begin] == expected,
                                ("endpoint semantics: " + pattern + " on " + text).c_str()) &&
                         ok;
                }
            }
            for (const char* op : {"#", "##", "%", "%%", "/", "//", "/#", "/%"}) {
                const std::string expression =
                    std::string("v") + op + pattern + (op[0] == '/' ? "/XY" : "");
                ok = expect(optimized.expand(expression) == reference.expand(expression),
                            ("expansion semantics: " + expression + " on " + text).c_str()) &&
                     ok;
            }
        }
    }
    // Cache options must be honored by both APIs when the same text is reused.
    config::extglob_enabled = false;
    ok = expect(matcher.match_end_positions("@(a|b)", "@(a|b)", true).has_value(),
                "disabled extglob is eligible for ordinary matching") &&
         ok;
    config::extglob_enabled = previous_extglob;
    value = std::string(8192, 'a') + 'b';
    ok = expect(optimized.expand("v//z*/X") == value, "long wildcard replacement miss") && ok;
    ok = expect(optimized.expand("v//[a]/X") == std::string(8192, 'X') + 'b',
                "many global character-class replacements") &&
         ok;
    ok = expect(optimized.expand("v%%*z") == value, "long wildcard suffix miss") && ok;
    return ok;
}

}  // namespace

int main() {
    cjsh_env::reset_shell_state();
    config::interactive_mode = false;
    config::force_interactive = false;
    g_shell = std::make_unique<Shell>();
    g_shell->set_interactive_mode(false);
    const bool transitions_ok = test_scalar_and_nameref_transitions();
    const bool lookup_ok = test_variable_presence_and_scope();
    const bool expansion_ok = test_parameter_expansion_work();
    const bool replacement_ok = test_parameter_replacement();
    const bool removal_ok = test_literal_pattern_removal();
    const bool import_ok = test_environment_import();
    const bool pattern_ok = test_pattern_matching();
    const bool endpoints_ok = test_pattern_endpoints_and_expansion();
    const bool frontiers_ok = test_extended_pattern_frontiers();
    g_shell.reset();
    if (!lookup_ok || !expansion_ok || !replacement_ok || !removal_ok || !import_ok ||
        !pattern_ok || !transitions_ok || !endpoints_ok || !frontiers_ok) {
        return 1;
    }
    std::puts("All 9 variable lookup and expansion tests passed");
    return 0;
}
