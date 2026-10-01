/*
  read_command.cpp

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

#include "read_command.h"

#include "builtin_help.h"
#include "builtin_option_parser.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "error_out.h"
#include "numeric_utils.h"
#include "readonly_command.h"
#include "shell.h"
#include "shell_env.h"
#include "signal_handler.h"

namespace {

constexpr const char* kReadCommandName = "read";
constexpr const char* kDefaultReplyVariable = "REPLY";
constexpr const char* kDefaultDelimiter = "\n";

enum class ReadInputStatus : std::uint8_t {
    Success,
    TimeoutNoData,
    EndOfFileNoData,
    EndOfFile,
    Error,
};

struct ReadOptions {
    bool raw_mode = false;
    int nchars = -1;
    std::string prompt;
    std::string delim = kDefaultDelimiter;
    bool has_timeout = false;
    double timeout_seconds = 0.0;
    int input_fd = STDIN_FILENO;
};

const std::vector<std::string>& read_help_lines() {
    static const std::vector<std::string> kHelpLines = {
        "Usage: read [-r] [-p prompt] [-n nchars] [-d delim] [-t timeout] [-u fd] [name ...]",
        "Read a line from standard input and split it into fields.",
        "",
        "Options:",
        "  -r            do not allow backslashes to escape any characters",
        "  -p prompt     output PROMPT without a trailing newline before reading",
        "  -n nchars     return after reading NCHARS characters rather than waiting for a "
        "newline",
        "  -d delim      continue until the first character of DELIM is read, rather than "
        "newline",
        "  -t timeout    time out after TIMEOUT seconds (fractional allowed)",
        "  -u fd         read from file descriptor FD instead of standard input"};
    return kHelpLines;
}

void clear_input_stream_state() {
    std::cin.clear();
    clearerr(stdin);
}

bool parse_read_options(const std::vector<std::string>& args, size_t& start_index,
                        ReadOptions& options) {
    std::vector<BuiltinParsedShortOption> parsed_options;
    const bool options_ok = builtin_parse_short_options_ex(
        args, start_index, kReadCommandName,
        [](char option) {
            return option == 'r' || option == 'n' || option == 'p' || option == 'd' ||
                   option == 't' || option == 'u';
        },
        [](char option) {
            return option == 'n' || option == 'p' || option == 'd' || option == 't' ||
                   option == 'u';
        },
        parsed_options);
    if (!options_ok) {
        return false;
    }

    for (const auto& option : parsed_options) {
        if (config::is_posix_mode() && option.option != 'r') {
            print_error({ErrorType::INVALID_ARGUMENT,
                         "read",
                         "option is disabled in POSIX mode: -" + std::string(1, option.option),
                         {}});
            return false;
        }
        const std::string option_value = option.value.value_or("");
        switch (option.option) {
            case 'r':
                options.raw_mode = true;
                break;
            case 'n':
                if (!numeric_utils::parse_int_strict(option_value, options.nchars)) {
                    print_error({ErrorType::INVALID_ARGUMENT,
                                 kReadCommandName,
                                 "invalid number of characters: " + option_value,
                                 {}});
                    return false;
                }
                break;
            case 'p':
                options.prompt = option_value;
                break;
            case 'd':
                options.delim = option.value.value_or(kDefaultDelimiter);
                break;
            case 't':
                try {
                    options.timeout_seconds = std::stod(option_value);
                } catch (const std::exception&) {
                    print_error({ErrorType::INVALID_ARGUMENT,
                                 kReadCommandName,
                                 "invalid timeout: " + option_value,
                                 {}});
                    return false;
                }
                if (options.timeout_seconds < 0.0) {
                    print_error({ErrorType::INVALID_ARGUMENT,
                                 kReadCommandName,
                                 "invalid timeout: " + option_value,
                                 {}});
                    return false;
                }
                options.has_timeout = true;
                break;
            case 'u':
                if (!numeric_utils::parse_int_strict(option_value, options.input_fd) ||
                    options.input_fd < 0 || fcntl(options.input_fd, F_GETFD) < 0) {
                    print_error({ErrorType::INVALID_ARGUMENT,
                                 kReadCommandName,
                                 "invalid file descriptor: " + option_value,
                                 {}});
                    return false;
                }
                break;
            default:
                break;
        }
    }

    return true;
}

bool read_interrupted() {
    return SignalHandler::has_pending_termination_signal() ||
           (SignalHandler::startup_interrupted() && !SignalHandler::executing_trap());
}

bool wait_for_input(const std::optional<std::chrono::steady_clock::time_point>& deadline,
                    int input_fd) {
    if (!deadline.has_value()) {
        return true;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline.value()) {
        return false;
    }

    const auto remaining_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline.value() - now).count();
    if (remaining_ms < 0) {
        return false;
    }

    const int timeout_ms =
        static_cast<int>(std::min<long long>(remaining_ms, std::numeric_limits<int>::max()));

    struct pollfd pfd{input_fd, POLLIN, 0};

    int poll_result = 0;
    do {
        poll_result = poll(&pfd, 1, timeout_ms);
    } while (poll_result < 0 && errno == EINTR && !read_interrupted());

    return poll_result > 0;
}

ReadInputStatus collect_input(const ReadOptions& options, std::string& input) {
    std::optional<std::chrono::steady_clock::time_point> deadline;
    if (options.has_timeout) {
        deadline = std::chrono::steady_clock::now() +
                   std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                       std::chrono::duration<double>(options.timeout_seconds));
    }

    bool timed_out = false;
    bool reached_eof = false;
    char buffer[4096];
    size_t buffered = 0;
    size_t consumed = 0;
    struct stat input_stat{};
    // Pipes and terminals must leave subsequent readers' bytes in the kernel.
    // Only seekable regular files can safely restore unread buffered bytes.
    const bool buffered_file = !options.has_timeout && fstat(options.input_fd, &input_stat) == 0 &&
                               S_ISREG(input_stat.st_mode) &&
                               lseek(options.input_fd, 0, SEEK_CUR) >= 0;
    auto read_char = [&](char& out) -> bool {
        if (read_interrupted()) {
            return false;
        }
        if (!wait_for_input(deadline, options.input_fd)) {
            timed_out = true;
            return false;
        }
        if (consumed < buffered) {
            out = buffer[consumed++];
            return true;
        }
        ssize_t count = 0;
        do {
            count = read(options.input_fd, buffer, buffered_file ? sizeof(buffer) : 1);
        } while (count < 0 && errno == EINTR && !read_interrupted());
        reached_eof = count == 0;
        if (count <= 0) {
            return false;
        }
        buffered = static_cast<size_t>(count);
        consumed = 1;
        out = buffer[0];
        return true;
    };

    char c = 0;
    int chars_read = 0;

    if (options.nchars > 0) {
        while (chars_read < options.nchars && read_char(c)) {
            input += c;
            chars_read++;
        }
    } else {
        bool escaped = false;
        while (read_char(c)) {
            if (options.delim.find(c) != std::string::npos && !escaped) {
                break;
            }
            input += c;
            escaped = config::is_posix_mode() && !options.raw_mode && c == '\\' && !escaped;
        }
    }

    if (consumed < buffered &&
        lseek(options.input_fd, -static_cast<off_t>(buffered - consumed), SEEK_CUR) < 0) {
        print_error({ErrorType::RUNTIME_ERROR,
                     kReadCommandName,
                     "could not restore the input file position",
                     {}});
        return ReadInputStatus::Error;
    }

    if (timed_out && input.empty()) {
        return ReadInputStatus::TimeoutNoData;
    }

    if (reached_eof && input.empty()) {
        return ReadInputStatus::EndOfFileNoData;
    }
    if (reached_eof && config::is_posix_mode()) {
        return ReadInputStatus::EndOfFile;
    }

    return ReadInputStatus::Success;
}

std::string process_backslash_escapes(const std::string& input) {
    std::string processed;
    processed.reserve(input.length());

    for (size_t i = 0; i < input.length(); ++i) {
        if (input[i] == '\\' && i + 1 < input.length()) {
            const char next = input[i + 1];
            switch (next) {
                case 'n':
                    processed += '\n';
                    i++;
                    break;
                case 't':
                    processed += '\t';
                    i++;
                    break;
                case 'r':
                    processed += '\r';
                    i++;
                    break;
                case 'b':
                    processed += '\b';
                    i++;
                    break;
                case 'a':
                    processed += '\a';
                    i++;
                    break;
                case 'v':
                    processed += '\v';
                    i++;
                    break;
                case 'f':
                    processed += '\f';
                    i++;
                    break;
                case '\\':
                    processed += '\\';
                    i++;
                    break;
                default:
                    processed += input[i];
                    break;
            }
        } else {
            processed += input[i];
        }
    }

    return processed;
}

bool is_all_whitespace_ifs(const std::string& ifs) {
    return std::all_of(ifs.begin(), ifs.end(),
                       [](char c) { return c == ' ' || c == '\t' || c == '\n'; });
}

std::vector<std::string> split_with_whitespace_ifs(const std::string& input,
                                                   const std::string& ifs) {
    std::vector<std::string> fields;
    size_t start = 0;

    while (start < input.length() && ifs.find(input[start]) != std::string::npos) {
        start++;
    }

    std::string current_field;
    for (size_t i = start; i < input.length(); ++i) {
        if (ifs.find(input[i]) != std::string::npos) {
            if (!current_field.empty()) {
                fields.push_back(current_field);
                current_field.clear();
            }
        } else {
            current_field += input[i];
        }
    }

    if (!current_field.empty()) {
        fields.push_back(current_field);
    }

    return fields;
}

std::vector<std::string> split_with_general_ifs(const std::string& input, const std::string& ifs) {
    std::vector<std::string> fields;
    std::string current_field;

    for (char c : input) {
        if (ifs.find(c) != std::string::npos) {
            fields.push_back(current_field);
            current_field.clear();
        } else {
            current_field += c;
        }
    }
    fields.push_back(current_field);

    size_t first_non_empty = 0;
    while (first_non_empty < fields.size() && fields[first_non_empty].empty()) {
        first_non_empty++;
    }

    size_t one_past_last_non_empty = fields.size();
    while (one_past_last_non_empty > first_non_empty &&
           fields[one_past_last_non_empty - 1].empty()) {
        one_past_last_non_empty--;
    }

    return std::vector<std::string>(fields.begin() + static_cast<long>(first_non_empty),
                                    fields.begin() + static_cast<long>(one_past_last_non_empty));
}

std::vector<std::string> split_fields(const std::string& input, const std::string& ifs) {
    if (ifs.empty()) {
        return {input};
    }

    if (is_all_whitespace_ifs(ifs)) {
        return split_with_whitespace_ifs(input, ifs);
    }

    return split_with_general_ifs(input, ifs);
}

std::vector<std::string> collect_variable_names(const std::vector<std::string>& args,
                                                size_t start_index) {
    std::vector<std::string> var_names;
    for (size_t i = start_index; i < args.size(); ++i) {
        var_names.push_back(args[i]);
    }

    if (var_names.empty()) {
        var_names.push_back(kDefaultReplyVariable);
    }

    return var_names;
}

std::string join_remaining_fields(const std::vector<std::string>& fields, size_t start_index) {
    std::string joined;
    for (size_t i = start_index; i < fields.size(); ++i) {
        if (i > start_index) {
            joined += ' ';
        }
        joined += fields[i];
    }
    return joined;
}

bool assign_fields_to_variables(const std::vector<std::string>& var_names,
                                const std::vector<std::string>& fields, Shell* shell) {
    for (size_t i = 0; i < var_names.size(); ++i) {
        const std::string& var_name = var_names[i];
        if (!readonly_manager_can_assign(var_name, kReadCommandName)) {
            return false;
        }

        std::string value;
        if (i < fields.size()) {
            const bool is_last_var = i == var_names.size() - 1;
            const bool has_extra_fields = fields.size() > var_names.size();
            if (is_last_var && has_extra_fields) {
                value = join_remaining_fields(fields, i);
            } else {
                value = fields[i];
            }
        }

        if (!cjsh_env::set_shell_or_local_variable_value(shell, var_name, value)) {
            print_error(
                {ErrorType::FATAL_ERROR, kReadCommandName, "shell not initialized properly", {}});
            return false;
        }
    }

    return true;
}

bool assign_posix_fields(const std::vector<std::string>& var_names, const std::string& raw,
                         bool raw_mode, Shell* shell) {
    std::string input;
    std::vector<bool> quoted;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (!raw_mode && raw[i] == '\\' && i + 1 < raw.size()) {
            if (raw[++i] != '\n') {
                input += raw[i];
                quoted.push_back(true);
            }
        } else {
            input += raw[i];
            quoted.push_back(false);
        }
    }
    const std::string ifs = cjsh_env::get_ifs_delimiters();
    auto delimiter = [&](size_t pos) {
        return pos < input.size() && !quoted[pos] && ifs.find(input[pos]) != std::string::npos;
    };
    auto whitespace = [&](size_t pos) {
        return delimiter(pos) && (input[pos] == ' ' || input[pos] == '\t' || input[pos] == '\n');
    };
    std::vector<std::pair<size_t, size_t>> fields;
    size_t pos = 0;
    while (whitespace(pos)) {
        ++pos;
    }
    size_t start = pos;
    while (pos < input.size()) {
        if (!delimiter(pos)) {
            ++pos;
            continue;
        }
        fields.emplace_back(start, pos);
        if (whitespace(pos)) {
            while (whitespace(pos)) {
                ++pos;
            }
            if (delimiter(pos)) {
                ++pos;
            }
        } else {
            ++pos;
        }
        while (whitespace(pos)) {
            ++pos;
        }
        start = pos;
    }
    if (start < input.size()) {
        fields.emplace_back(start, input.size());
    }
    size_t end = input.size();
    while (end > 0 && whitespace(end - 1)) {
        --end;
    }
    std::vector<std::string> values(var_names.size());
    for (size_t i = 0; i < var_names.size() && i < fields.size(); ++i) {
        const size_t field_end =
            i + 1 == var_names.size() && i + 1 < fields.size() ? end : fields[i].second;
        values[i] = input.substr(fields[i].first, field_end - fields[i].first);
    }
    return assign_fields_to_variables(var_names, values, shell);
}

}  // namespace

int read_command(const std::vector<std::string>& args, Shell* shell) {
    if (builtin_handle_help(args, read_help_lines(), BuiltinHelpScanMode::AnyArgument)) {
        return 0;
    }

    if (shell == nullptr) {
        print_error(
            {ErrorType::FATAL_ERROR, kReadCommandName, "shell not initialized properly", {}});
        return 1;
    }

    clear_input_stream_state();

    ReadOptions options;
    size_t start_index = 1;
    if (!parse_read_options(args, start_index, options)) {
        return 1;
    }

    std::vector<std::string> var_names = collect_variable_names(args, start_index);

    if (!options.prompt.empty()) {
        std::cout << options.prompt << std::flush;
    }

    std::string input;
    const ReadInputStatus read_status = collect_input(options, input);
    if (SignalHandler::startup_interrupted() && !SignalHandler::executing_trap()) {
        return 128 + SIGINT;
    }
    if (read_status != ReadInputStatus::Success) {
        if (config::is_posix_mode() && (read_status == ReadInputStatus::EndOfFile ||
                                        read_status == ReadInputStatus::EndOfFileNoData)) {
            (void)assign_posix_fields(var_names, input, options.raw_mode, shell);
        }
        return 1;
    }

    if (config::is_posix_mode()) {
        return assign_posix_fields(var_names, input, options.raw_mode, shell) ? 0 : 1;
    }
    if (!options.raw_mode) {
        input = process_backslash_escapes(input);
    }

    std::vector<std::string> fields = split_fields(input, cjsh_env::get_ifs_delimiters());
    if (!assign_fields_to_variables(var_names, fields, shell)) {
        return 1;
    }

    return 0;
}
