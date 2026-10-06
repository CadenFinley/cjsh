/*
  test_command.cpp

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

#include "test_command.h"
#include "builtin_help.h"
#include "error_out.h"
#include "shell_dialect.h"
#include "test_expression_utils.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

#ifdef __APPLE__
#define ST_MTIM_NSEC st_mtimespec.tv_nsec
#define ST_ATIM_NSEC st_atimespec.tv_nsec
#else
#define ST_MTIM_NSEC st_mtim.tv_nsec
#define ST_ATIM_NSEC st_atim.tv_nsec
#endif

namespace {

bool is_symlink(const char* path) {
    struct stat st;
    return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
}

struct TestContext {
    std::vector<std::string> args;
    size_t pos;

    TestContext(const std::vector<std::string>& a) : args(a), pos(0) {
    }

    bool has_more() const {
        return pos < args.size();
    }
    const std::string& current() const {
        return args[pos];
    }
    void advance() {
        if (pos < args.size()) {
            pos++;
        }
    }
    size_t remaining() const {
        return args.size() - pos;
    }
};

bool evaluate_expression(TestContext& ctx);
bool evaluate_or(TestContext& ctx);
bool evaluate_and(TestContext& ctx);
bool evaluate_term(TestContext& ctx);
bool evaluate_unary(TestContext& ctx);
bool evaluate_binary(TestContext& ctx);

bool evaluate_unary(TestContext& ctx) {
    if (!ctx.has_more()) {
        return false;
    }

    const std::string& op = ctx.current();
    ctx.advance();

    if (!ctx.has_more()) {
        return false;
    }

    const std::string& arg = ctx.current();
    ctx.advance();

    struct stat st;

    if (test_expression_utils::is_basic_unary_operator(op)) {
        return test_expression_utils::evaluate_basic_unary_operator(op, arg);
    } else if (op == "-L" || op == "-h") {
        return is_symlink(arg.c_str());
    } else if (op == "-p") {
        return stat(arg.c_str(), &st) == 0 && S_ISFIFO(st.st_mode);
    } else if (op == "-b") {
        return stat(arg.c_str(), &st) == 0 && S_ISBLK(st.st_mode);
    } else if (op == "-c") {
        return stat(arg.c_str(), &st) == 0 && S_ISCHR(st.st_mode);
    } else if (op == "-S") {
        return stat(arg.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
    } else if (op == "-u") {
        return stat(arg.c_str(), &st) == 0 && (st.st_mode & S_ISUID);
    } else if (op == "-g") {
        return stat(arg.c_str(), &st) == 0 && (st.st_mode & S_ISGID);
    } else if (op == "-k") {
        return stat(arg.c_str(), &st) == 0 && (st.st_mode & S_ISVTX);
    } else if (op == "-O") {
        return stat(arg.c_str(), &st) == 0 && st.st_uid == geteuid();
    } else if (op == "-G") {
        return stat(arg.c_str(), &st) == 0 && st.st_gid == getegid();
    } else if (op == "-N") {
        if (stat(arg.c_str(), &st) != 0) {
            return false;
        }
#ifdef __APPLE__
        return st.st_mtime > st.st_atime ||
               (st.st_mtime == st.st_atime && st.st_mtimespec.tv_nsec > st.st_atimespec.tv_nsec);
#else
        return st.st_mtime > st.st_atime ||
               (st.st_mtime == st.st_atime && st.st_mtim.tv_nsec > st.st_atim.tv_nsec);
#endif
    } else if (op == "-t") {
        try {
            int fd = std::stoi(arg);
            return isatty(fd);
        } catch (...) {
            return false;
        }
    }

    return false;
}

bool evaluate_binary(TestContext& ctx) {
    if (ctx.remaining() < 3) {
        return false;
    }

    const std::string& left = ctx.current();
    ctx.advance();
    const std::string& op = ctx.current();
    ctx.advance();
    const std::string& right = ctx.current();
    ctx.advance();

    if (op == "=" || op == "==") {
        return left == right;
    } else if (op == "!=") {
        return left != right;
    } else if (op == "<") {
        return left < right;
    } else if (op == ">") {
        return left > right;
    }

    auto numeric_result = test_expression_utils::evaluate_numeric_comparison(left, op, right);
    if (numeric_result.has_value()) {
        return *numeric_result;
    }

    struct stat st1, st2;
    if (op == "-ef") {
        return stat(left.c_str(), &st1) == 0 && stat(right.c_str(), &st2) == 0 &&
               st1.st_dev == st2.st_dev && st1.st_ino == st2.st_ino;
    } else if (op == "-nt") {
        if (stat(left.c_str(), &st1) != 0 || stat(right.c_str(), &st2) != 0) {
            return false;
        }
#ifdef __APPLE__
        return st1.st_mtime > st2.st_mtime || (st1.st_mtime == st2.st_mtime &&
                                               st1.st_mtimespec.tv_nsec > st2.st_mtimespec.tv_nsec);
#else
        return st1.st_mtime > st2.st_mtime ||
               (st1.st_mtime == st2.st_mtime && st1.st_mtim.tv_nsec > st2.st_mtim.tv_nsec);
#endif
    } else if (op == "-ot") {
        if (stat(left.c_str(), &st1) != 0 || stat(right.c_str(), &st2) != 0) {
            return false;
        }
#ifdef __APPLE__
        return st1.st_mtime < st2.st_mtime || (st1.st_mtime == st2.st_mtime &&
                                               st1.st_mtimespec.tv_nsec < st2.st_mtimespec.tv_nsec);
#else
        return st1.st_mtime < st2.st_mtime ||
               (st1.st_mtime == st2.st_mtime && st1.st_mtim.tv_nsec < st2.st_mtim.tv_nsec);
#endif
    }

    return false;
}

bool is_binary_op(const std::string& s) {
    return s == "=" || s == "==" || s == "!=" || s == "<" || s == ">" || s == "-eq" || s == "-ne" ||
           s == "-lt" || s == "-le" || s == "-gt" || s == "-ge" || s == "-ef" || s == "-nt" ||
           s == "-ot";
}

bool is_unary_op(const std::string& s) {
    return (s.length() == 2 && s[0] == '-' &&
            (test_expression_utils::is_basic_unary_operator(s) || s == "-L" || s == "-h" ||
             s == "-p" || s == "-b" || s == "-c" || s == "-S" || s == "-u" || s == "-g" ||
             s == "-k" || s == "-O" || s == "-G" || s == "-N" || s == "-t"));
}

bool evaluate_term(TestContext& ctx) {
    if (!ctx.has_more()) {
        return false;
    }

    bool negated = false;
    while (ctx.has_more() && ctx.current() == "!") {
        negated = !negated;
        ctx.advance();
    }

    if (!ctx.has_more()) {
        return false;
    }

    bool result;

    if (ctx.current() == "(") {
        ctx.advance();
        result = evaluate_expression(ctx);
        if (ctx.has_more() && ctx.current() == ")") {
            ctx.advance();
        }
    }

    else if (ctx.remaining() >= 3 && is_binary_op(ctx.args[ctx.pos + 1])) {
        result = evaluate_binary(ctx);
    }

    else if (is_unary_op(ctx.current())) {
        result = evaluate_unary(ctx);
    }

    else {
        result = !ctx.current().empty();
        ctx.advance();
    }

    return negated ? !result : result;
}

bool evaluate_and(TestContext& ctx) {
    bool result = evaluate_term(ctx);

    while (ctx.has_more() && ctx.current() == "-a") {
        ctx.advance();
        result = result && evaluate_term(ctx);
    }

    return result;
}

bool evaluate_or(TestContext& ctx) {
    bool result = evaluate_and(ctx);

    while (ctx.has_more() && ctx.current() == "-o") {
        ctx.advance();
        result = result || evaluate_and(ctx);
    }

    return result;
}

bool evaluate_expression(TestContext& ctx) {
    return evaluate_or(ctx);
}

}  // namespace

int test_command(const std::vector<std::string>& args) {
    if (builtin_handle_help(args, {"Usage: test EXPRESSION",
                                   "   or: test",
                                   "   or: [ EXPRESSION ]",
                                   "   or: [ ]",
                                   "",
                                   "Evaluate conditional expressions.",
                                   "",
                                   "File tests:",
                                   "  -e FILE        FILE exists",
                                   "  -f FILE        FILE exists and is a regular file",
                                   "  -d FILE        FILE exists and is a directory",
                                   "  -h, -L FILE    FILE exists and is a symbolic link",
                                   "  -r FILE        FILE exists and read permission is granted",
                                   "  -w FILE        FILE exists and write permission is granted",
                                   "  -x FILE        FILE exists and execute permission is granted",
                                   "  -s FILE        FILE exists and has size greater than zero",
                                   "",
                                   "String tests:",
                                   "  -z STRING      STRING length is zero",
                                   "  -n STRING      STRING length is non-zero",
                                   "  STRING         equivalent to -n STRING",
                                   "  STR1 = STR2    strings are equal",
                                   "  STR1 != STR2   strings are not equal",
                                   "",
                                   "Integer tests:",
                                   "  INT1 -eq INT2  INT1 is equal to INT2",
                                   "  INT1 -ne INT2  INT1 is not equal to INT2",
                                   "  INT1 -lt INT2  INT1 is less than INT2",
                                   "  INT1 -le INT2  INT1 is less than or equal to INT2",
                                   "  INT1 -gt INT2  INT1 is greater than INT2",
                                   "  INT1 -ge INT2  INT1 is greater than or equal to INT2",
                                   "",
                                   "Logical operators:",
                                   "  ! EXPR         EXPR is false",
                                   "  EXPR1 -a EXPR2 both EXPR1 and EXPR2 are true",
                                   "  EXPR1 -o EXPR2 either EXPR1 or EXPR2 is true",
                                   "  ( EXPR )       value of EXPR"})) {
        return 0;
    }

    const std::string& command_name = args.empty() ? "test" : args[0];
    if (!args.empty() && args[0] == "[" && (args.size() == 1 || args.back() != "]")) {
        print_error({ErrorType::SYNTAX_ERROR, command_name, "missing closing ']'", {}});
        return 2;
    }

    std::vector<std::string> test_args;

    if (args[0] == "[") {
        if (args.size() > 1 && args.back() == "]") {
            test_args = std::vector<std::string>(args.begin() + 1, args.end() - 1);
        } else {
            test_args = std::vector<std::string>(args.begin() + 1, args.end());
        }
    } else if (args[0] == "test") {
        test_args = std::vector<std::string>(args.begin() + 1, args.end());
    } else {
        test_args = std::vector<std::string>(args.begin(), args.end());
    }

    if (test_args.empty()) {
        return 1;
    }

    if (config::is_posix_mode() && test_args.size() == 3 && test_args[1] == "==") {
        print_error(
            {ErrorType::SYNTAX_ERROR, command_name, "'==' is disabled in POSIX mode; use '='", {}});
        return 2;
    }

    if ((test_args.size() == 1 && (is_unary_op(test_args[0]) || is_binary_op(test_args[0]))) ||
        (test_args.size() == 2 && is_binary_op(test_args[1]))) {
        print_error({ErrorType::SYNTAX_ERROR, command_name, "missing operand", {}});
        return 2;
    }

    TestContext ctx(test_args);
    bool result = evaluate_expression(ctx);

    if (ctx.has_more()) {
        print_error({ErrorType::SYNTAX_ERROR, command_name, "unexpected token", {}});
        return 2;
    }

    return result ? 0 : 1;
}
