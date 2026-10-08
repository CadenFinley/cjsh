/*
  printf_command.cpp

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

#include "printf_command.h"

#include "builtin_help.h"

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "error_out.h"
#include "parser_utils.h"

namespace {

static int exit_status = 0;
static bool output_stopped = false;

constexpr long long kMaxPrintfFieldWidth = 1'000'000;
constexpr long long kMaxPrintfPrecision = 1'000'000;
constexpr long long kMaxPrintfPositionalIndex = 1024;

static void report_format_error(const std::string& message) {
    print_error({ErrorType::INVALID_ARGUMENT, "printf", message, {}});
    exit_status = 1;
}

static void report_character_constant_warning() {
    print_error({ErrorType::INVALID_ARGUMENT,
                 ErrorSeverity::WARNING,
                 "printf",
                 "character(s) following character constant have been ignored",
                 {}});
}

static inline bool is_octal_digit(char c) {
    return c >= '0' && c <= '7';
}

static inline int from_octal(char c) {
    return c - '0';
}

static void verify_numeric(const char* s, char* end, const std::string& original) {
    if (s == end) {
        print_error(
            {ErrorType::INVALID_ARGUMENT, "printf", "expected a numeric value: " + original, {}});
        exit_status = 1;
    } else if (errno != 0) {
        print_error({ErrorType::INVALID_ARGUMENT, "printf", original, {}});
        exit_status = 1;
    } else if (*end != '\0') {
        print_error({ErrorType::INVALID_ARGUMENT,
                     "printf",
                     "value not completely converted: " + original,
                     {}});
        exit_status = 1;
    }
}

template <typename Numeric, typename Parser>
Numeric parse_printf_numeric(const char* s, Parser parse) {
    if (!s || !*s) {
        return Numeric{};
    }

    char* end = nullptr;
    Numeric val{};

    if ((*s == '"' || *s == '\'') && *(s + 1)) {
        char quote = *s;
        unsigned char ch = static_cast<unsigned char>(*++s);
        val = static_cast<Numeric>(ch);

        if (MB_CUR_MAX > 1 && *(s + 1) && *(s + 1) != quote) {
            mbstate_t mbstate;
            (void)memset(&mbstate, 0, sizeof(mbstate));
            wchar_t wc;
            size_t slen = strlen(s);
            size_t bytes = mbrtowc(&wc, s, slen, &mbstate);
            if (bytes > 0 && bytes != (size_t)-1 && bytes != (size_t)-2) {
                val = static_cast<Numeric>(wc);
                s += bytes - 1;
            }
        }

        ++s;
        if (*s != '\0' && *s != quote) {
            report_character_constant_warning();
        }
    } else {
        errno = 0;
        val = static_cast<Numeric>(parse(s, &end));
        verify_numeric(s, end, s);
    }
    return val;
}

static intmax_t vstrtoimax(const char* s) {
    return parse_printf_numeric<intmax_t>(
        s, [](const char* value, char** end) { return strtoll(value, end, 0); });
}

static uintmax_t vstrtoumax(const char* s) {
    return parse_printf_numeric<uintmax_t>(
        s, [](const char* value, char** end) { return strtoull(value, end, 0); });
}

static long double vstrtold(const char* s) {
    return parse_printf_numeric<long double>(
        s, [](const char* value, char** end) { return strtold(value, end); });
}

static void print_esc_char(char c) {
    switch (c) {
        case 'a':
            (void)putchar('\a');
            break;
        case 'b':
            (void)putchar('\b');
            break;
        case 'c':
            output_stopped = true;
            break;
        case 'e':
            (void)putchar('\x1B');
            break;
        case 'f':
            (void)putchar('\f');
            break;
        case 'n':
            (void)putchar('\n');
            break;
        case 'r':
            (void)putchar('\r');
            break;
        case 't':
            (void)putchar('\t');
            break;
        case 'v':
            (void)putchar('\v');
            break;
        default:
            (void)putchar(c);
            break;
    }
}

static void print_unicode_char(uint32_t code) {
    if (code <= 0x7F) {
        (void)putchar(static_cast<int>(code));
    } else if (code <= 0x7FF) {
        (void)putchar(static_cast<int>(0xC0 | (code >> 6)));
        (void)putchar(static_cast<int>(0x80 | (code & 0x3F)));
    } else if (code <= 0xFFFF) {
        (void)putchar(static_cast<int>(0xE0 | (code >> 12)));
        (void)putchar(static_cast<int>(0x80 | ((code >> 6) & 0x3F)));
        (void)putchar(static_cast<int>(0x80 | (code & 0x3F)));
    } else if (code <= 0x10FFFF) {
        (void)putchar(static_cast<int>(0xF0 | (code >> 18)));
        (void)putchar(static_cast<int>(0x80 | ((code >> 12) & 0x3F)));
        (void)putchar(static_cast<int>(0x80 | ((code >> 6) & 0x3F)));
        (void)putchar(static_cast<int>(0x80 | (code & 0x3F)));
    }
}

static int print_esc(const char* escstart, bool octal_0) {
    const char* p = escstart + 1;
    int esc_value = 0;
    int esc_length;

    if (*p == 'x') {
        for (esc_length = 0, ++p; esc_length < 2 && is_hex_digit(*p); ++esc_length, ++p) {
            esc_value = esc_value * 16 + from_hex_digit(*p);
        }
        if (esc_length == 0) {
            print_error({ErrorType::INVALID_ARGUMENT,
                         "printf",
                         "missing hexadecimal number in escape",
                         {}});
            exit_status = 1;
            output_stopped = true;
            return static_cast<int>(p - escstart - 1);
        }
        (void)putchar(esc_value);
    } else if (is_octal_digit(*p)) {
        for (esc_length = 0, p += octal_0 && *p == '0'; esc_length < 3 && is_octal_digit(*p);
             ++esc_length, ++p) {
            esc_value = esc_value * 8 + from_octal(*p);
        }
        (void)putchar(esc_value);
    } else if (*p && strchr("\"\\abcefnrtv", *p)) {
        print_esc_char(*p++);
    } else if (*p == 'u' || *p == 'U') {
        char esc_char = *p;
        uint32_t uni_value = 0;

        for (esc_length = (esc_char == 'u' ? 4 : 8), ++p; esc_length > 0; --esc_length, ++p) {
            if (!is_hex_digit(*p)) {
                print_error({ErrorType::INVALID_ARGUMENT,
                             "printf",
                             "missing hexadecimal number in escape",
                             {}});
                exit_status = 1;
                output_stopped = true;
                return static_cast<int>(p - escstart - 1);
            }
            uni_value = uni_value * 16 + static_cast<uint32_t>(from_hex_digit(*p));
        }

        if (uni_value >= 0xD800 && uni_value <= 0xDFFF) {
            print_error(
                {ErrorType::INVALID_ARGUMENT, "printf", "invalid universal character name", {}});
            exit_status = 1;
            output_stopped = true;
            return static_cast<int>(p - escstart - 1);
        }

        print_unicode_char(uni_value);
    } else {
        (void)putchar('\\');
        if (*p) {
            (void)putchar(*p);
            p++;
        }
    }
    return static_cast<int>(p - escstart - 1);
}

static void print_esc_string(const char* str) {
    for (; *str && !output_stopped; str++) {
        if (*str == '\\') {
            str += print_esc(str, true);
        } else {
            (void)putchar(*str);
        }
    }
}

static std::string shell_escape(const std::string& str) {
    std::ostringstream result;
    bool needs_quotes = false;

    for (char c : str) {
        if (!isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '/' &&
            c != '.' && c != ',') {
            needs_quotes = true;
            break;
        }
    }

    if (!needs_quotes && !str.empty()) {
        return str;
    }

    result << "'";
    for (char c : str) {
        if (c == '\'') {
            result << "'\\''";
        } else {
            result << c;
        }
    }
    result << "'";
    return result.str();
}

template <typename T>
static void printf_value(const std::string& fmt, T value) {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
    (void)printf(fmt.c_str(), value);
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
}

static void print_direc(const char* start, char conversion, bool have_field_width, int field_width,
                        bool have_precision, int precision, const char* argument) {
    std::ostringstream fmt;
    const char* p = start;

    fmt << "%";
    ++p;

    while (*p && strchr("-+ #0'I", *p)) {
        fmt << *p++;
    }

    if (have_field_width) {
        fmt << field_width;

        if (*p == '*') {
            ++p;
        } else {
            while (*p && *p >= '0' && *p <= '9') {
                ++p;
            }
        }
    } else {
        while (*p && *p >= '0' && *p <= '9') {
            fmt << *p++;
        }
    }

    if (*p == '.') {
        if (have_precision) {
            fmt << "." << precision;
            ++p;

            if (*p == '*') {
                ++p;
            } else {
                while (*p && *p >= '0' && *p <= '9') {
                    ++p;
                }
            }
        } else {
            fmt << *p++;
            while (*p && *p >= '0' && *p <= '9') {
                fmt << *p++;
            }
        }
    }

    while (*p && strchr("hlLjzt", *p)) {
        ++p;
    }

    switch (conversion) {
        case 'd':
        case 'i':
        case 'o':
        case 'u':
        case 'x':
        case 'X': {
            fmt << "ll" << conversion;
            if (conversion == 'd' || conversion == 'i') {
                long long val = argument ? vstrtoimax(argument) : 0;
                printf_value(fmt.str(), val);
            } else {
                unsigned long long val = argument ? vstrtoumax(argument) : 0;
                printf_value(fmt.str(), val);
            }
            break;
        }
        case 'a':
        case 'A':
        case 'e':
        case 'E':
        case 'f':
        case 'F':
        case 'g':
        case 'G': {
            fmt << "L" << conversion;
            long double val = argument ? vstrtold(argument) : 0.0;
            printf_value(fmt.str(), val);
            break;
        }
        case 'c': {
            fmt << "c";
            int val;
            if (argument) {
                if ((*argument == '"' || *argument == '\'') && *(argument + 1)) {
                    val = static_cast<int>(vstrtoimax(argument));
                } else {
                    char* end;
                    errno = 0;
                    long num = strtol(argument, &end, 0);

                    if (errno == 0 && *end == '\0') {
                        val = static_cast<int>(num);
                    } else {
                        val = (unsigned char)argument[0];
                    }
                }
            } else {
                val = 0;
            }
            printf_value(fmt.str(), val);
            break;
        }
        case 's': {
            fmt << "s";
            const char* val = argument ? argument : "";
            printf_value(fmt.str(), val);
            break;
        }
    }
}

struct arg_cursor {
    const char* f;
    int curr_arg;
    int curr_s_arg;
    int end_arg;
    int direc_arg;
    bool error;
    std::string error_msg;
};

static arg_cursor get_curr_arg(int pos, arg_cursor ac) {
    ac.error = false;
    ac.error_msg.clear();
    int arg = 0;
    const char* f = ac.f;

    if (pos < 3 && std::isdigit(static_cast<unsigned char>(*f)) != 0) {
        long long value = 0;
        bool overflow = false;
        const char* digit_ptr = f;

        do {
            int digit = *digit_ptr - '0';
            if (!overflow) {
                if (value > (kMaxPrintfPositionalIndex - digit) / 10) {
                    overflow = true;
                } else {
                    value = value * 10 + digit;
                }
            }
            ++digit_ptr;
        } while (std::isdigit(static_cast<unsigned char>(*digit_ptr)) != 0);

        if (*digit_ptr == '$') {
            if (overflow || value <= 0 || value > kMaxPrintfPositionalIndex) {
                ac.error = true;
                ac.error_msg = "positional argument index too large";
                return ac;
            }
            arg = static_cast<int>(value);
            f = digit_ptr;
        }
    }

    if (arg > 0) {
        arg--;
        ac.f = f + 1;
        if (pos == 0) {
            ac.direc_arg = arg;
        }
    } else {
        arg = (pos == 0                      ? (ac.direc_arg = -1)
               : pos < 3 || ac.direc_arg < 0 ? ++ac.curr_s_arg
                                             : ac.direc_arg);
    }

    if (arg >= 0) {
        ac.curr_arg = arg;
        if (arg > ac.end_arg) {
            ac.end_arg = arg;
        }
    }
    return ac;
}

static int print_formatted(const char* format, int argc, char** argv) {
    arg_cursor ac;
    ac.curr_arg = ac.curr_s_arg = ac.end_arg = ac.direc_arg = -1;
    ac.error = false;
    ac.error_msg.clear();
    const char* direc_start;
    bool have_field_width;
    int field_width = 0;
    bool have_precision;
    int precision = 0;
    bool ok[256] = {false};

    for (ac.f = format; *ac.f && !output_stopped; ac.f++) {
        if (*ac.f == '%') {
            direc_start = ac.f;
            ac.f++;
            have_field_width = have_precision = false;

            if (*ac.f == '%') {
                (void)putchar('%');
                continue;
            }

            ac = get_curr_arg(0, ac);
            if (ac.error) {
                report_format_error(ac.error_msg);
                return -1;
            }

            if (*ac.f == 'b') {
                ac = get_curr_arg(3, ac);
                if (ac.error) {
                    report_format_error(ac.error_msg);
                    return -1;
                }
                if (ac.curr_arg < argc) {
                    print_esc_string(argv[ac.curr_arg]);
                }
                if (output_stopped) {
                    break;
                }
                continue;
            }

            if (*ac.f == 'q') {
                ac = get_curr_arg(3, ac);
                if (ac.error) {
                    report_format_error(ac.error_msg);
                    return -1;
                }
                if (ac.curr_arg < argc) {
                    std::cout << shell_escape(argv[ac.curr_arg]);
                }
                continue;
            }

            (void)memset(ok, 0, sizeof(ok));
            ok['a'] = ok['A'] = ok['c'] = ok['d'] = ok['e'] = ok['E'] = ok['f'] = ok['F'] =
                ok['g'] = ok['G'] = ok['i'] = ok['o'] = ok['s'] = ok['u'] = ok['x'] = ok['X'] =
                    true;

            while (*ac.f && strchr("-+ #0'I", *ac.f)) {
                switch (*ac.f) {
                    case '\'':
                        ok['a'] = ok['A'] = ok['c'] = ok['e'] = ok['E'] = ok['o'] = ok['s'] =
                            ok['x'] = ok['X'] = false;
                        break;
                    case '#':
                        ok['c'] = ok['d'] = ok['i'] = ok['s'] = ok['u'] = false;
                        break;
                    case '0':
                        ok['c'] = ok['s'] = false;
                        break;
                }
                ac.f++;
            }

            if (*ac.f == '*') {
                ac.f++;
                ac = get_curr_arg(1, ac);
                if (ac.error) {
                    report_format_error(ac.error_msg);
                    return -1;
                }
                long long width_value = 0;
                if (ac.curr_arg < argc) {
                    width_value = vstrtoimax(argv[ac.curr_arg]);
                }
                if (std::llabs(width_value) > kMaxPrintfFieldWidth) {
                    report_format_error("field width too large");
                    return -1;
                }
                field_width = static_cast<int>(width_value);
                have_field_width = true;
            } else {
                const char* width_start = ac.f;
                long long literal_width = 0;
                bool width_overflow = false;
                while (std::isdigit(static_cast<unsigned char>(*ac.f)) != 0) {
                    int digit = *ac.f - '0';
                    if (!width_overflow) {
                        if (literal_width > (kMaxPrintfFieldWidth - digit) / 10) {
                            width_overflow = true;
                        } else {
                            literal_width = literal_width * 10 + digit;
                        }
                    }
                    ++ac.f;
                }
                if ((ac.f > width_start) &&
                    (width_overflow || literal_width > kMaxPrintfFieldWidth)) {
                    report_format_error("field width too large");
                    return -1;
                }
            }

            if (*ac.f == '.') {
                ok['c'] = false;
                ac.f++;
                if (*ac.f == '*') {
                    ac.f++;
                    ac = get_curr_arg(2, ac);
                    if (ac.error) {
                        report_format_error(ac.error_msg);
                        return -1;
                    }
                    long long precision_value = 0;
                    if (ac.curr_arg < argc) {
                        precision_value = vstrtoimax(argv[ac.curr_arg]);
                        if (precision_value < 0) {
                            precision_value = -1;
                        }
                    }
                    if (precision_value >= 0 && precision_value > kMaxPrintfPrecision) {
                        report_format_error("precision too large");
                        return -1;
                    }
                    precision = static_cast<int>(precision_value);
                    have_precision = true;
                } else {
                    const char* precision_start = ac.f;
                    long long literal_precision = 0;
                    bool precision_overflow = false;
                    while (std::isdigit(static_cast<unsigned char>(*ac.f)) != 0) {
                        int digit = *ac.f - '0';
                        if (!precision_overflow) {
                            if (literal_precision > (kMaxPrintfPrecision - digit) / 10) {
                                precision_overflow = true;
                            } else {
                                literal_precision = literal_precision * 10 + digit;
                            }
                        }
                        ++ac.f;
                    }
                    if ((ac.f > precision_start) &&
                        (precision_overflow || literal_precision > kMaxPrintfPrecision)) {
                        report_format_error("precision too large");
                        return -1;
                    }
                }
            }

            while (*ac.f && strchr("hlLjzt", *ac.f)) {
                ac.f++;
            }

            unsigned char conversion = static_cast<unsigned char>(*ac.f);
            if (!ok[conversion]) {
                print_error({ErrorType::INVALID_ARGUMENT,
                             "printf",
                             "invalid conversion specification",
                             {}});
                return -1;
            }

            ac = get_curr_arg(3, ac);
            if (ac.error) {
                report_format_error(ac.error_msg);
                return -1;
            }

            print_direc(direc_start, static_cast<char>(conversion), have_field_width, field_width,
                        have_precision, precision,
                        ac.curr_arg < argc ? argv[ac.curr_arg] : nullptr);

        } else if (*ac.f == '\\') {
            ac.f += print_esc(ac.f, false);
        } else {
            (void)putchar(*ac.f);
        }
    }

    return ac.end_arg + 1;
}

}  // namespace

int printf_command(const std::vector<std::string>& args) {
    if (builtin_handle_help(
            args,
            {"Usage: printf FORMAT [ARGUMENT]...",
             "Format and print ARGUMENT(s) according to FORMAT.",
             "",
             "FORMAT controls the output as in C printf. Interpreted sequences:",
             "  \\\"       double quote",
             "  \\\\       backslash",
             "  \\a       alert (BEL)",
             "  \\b       backspace",
             "  \\c       produce no further output",
             "  \\e       escape",
             "  \\f       form feed",
             "  \\n       new line",
             "  \\r       carriage return",
             "  \\t       horizontal tab",
             "  \\v       vertical tab",
             "  \\NNN     byte with octal value NNN (1 to 3 digits)",
             "  \\xHH     byte with hexadecimal value HH (1 to 2 digits)",
             "  \\uHHHH   Unicode character with hex value HHHH (4 digits)",
             "  \\UHHHHHHHH Unicode character with hex value HHHHHHHH (8 digits)",
             "",
             "  %%       a single %",
             "  %b       ARGUMENT as a string with '\\' escapes interpreted",
             "  %q       ARGUMENT is printed in shell-quoted format",
             "",
             "All C format specifications ending with diouxXfeEgGaAcs are supported,",
             "with ARGUMENTs converted to proper type first. Variable widths are handled."})) {
        return 0;
    }

    if (args.size() < 2) {
        print_error({ErrorType::INVALID_ARGUMENT, "printf", "missing operand", {}});
        return 1;
    }

    exit_status = 0;
    output_stopped = false;

    const std::string& format = args[1];
    std::vector<std::string> argv_storage;
    std::vector<char*> argv_ptrs;

    for (size_t i = 2; i < args.size(); ++i) {
        argv_storage.push_back(args[i]);
    }

    argv_ptrs.reserve(argv_storage.size());
    for (const auto& stored_arg : argv_storage) {
        argv_ptrs.push_back(const_cast<char*>(stored_arg.c_str()));
    }

    int argc = static_cast<int>(argv_ptrs.size());
    char** argv = argv_ptrs.empty() ? nullptr : argv_ptrs.data();

    int args_used;
    do {
        args_used = print_formatted(format.c_str(), argc, argv);
        if (args_used < 0) {
            return 1;
        }
        // Missing operands use default values but still count as format slots.
        // Stop before advancing beyond the supplied array, including a null argv.
        if (output_stopped || args_used >= argc) {
            break;
        }
        argc -= args_used;
        argv += args_used;
    } while (args_used > 0 && argc > 0);

    (void)fflush(stdout);
    return exit_status;
}
