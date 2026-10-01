/*
  script_dispatch.cpp

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

#include "script_dispatch.h"

#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "cjsh_filesystem.h"
#include "string_utils.h"

namespace script_dispatch {

namespace {

std::optional<std::vector<std::string>> bash_shebang_options(std::string_view content) {
    if (content.substr(0, 2) != "#!") {
        return std::nullopt;
    }

    content.remove_prefix(2);
    std::istringstream line{std::string(content.substr(0, content.find('\n')))};
    std::string interpreter;
    if (!(line >> interpreter)) {
        return std::nullopt;
    }
    if (std::filesystem::path(interpreter).filename() == "env") {
        if (!(line >> interpreter)) {
            return std::nullopt;
        }
        if (interpreter == "-S" || interpreter == "--split-string") {
            if (!(line >> interpreter)) {
                return std::nullopt;
            }
        }
    }
    if (std::filesystem::path(interpreter).filename() != "bash") {
        return std::nullopt;
    }

    std::vector<std::string> options;
    std::string option;
    while (line >> option) {
        options.push_back(option);
    }
    return options;
}

std::optional<std::string> interpreter_for_script_extension(const std::filesystem::path& path) {
    std::string extension = string_utils::to_lower_copy(path.extension().string());
    if (extension == ".sh") {
        return std::string("sh");
    }
    if (extension == ".bash") {
        return std::string("bash");
    }
    if (extension == ".zsh") {
        return std::string("zsh");
    }
    if (extension == ".ksh") {
        return std::string("ksh");
    }
    return std::nullopt;
}

bool file_has_shebang(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        return false;
    }
    char prefix[2] = {0, 0};
    (void)file.read(prefix, 2);
    return file.gcount() == 2 && prefix[0] == '#' && prefix[1] == '!';
}

std::optional<std::string> resolve_script_path(const std::vector<std::string>& args,
                                               const char* cached_path) {
    if (args.empty()) {
        return std::nullopt;
    }
    if (cached_path != nullptr && cached_path[0] != '\0') {
        return std::string(cached_path);
    }
    if (args[0].find('/') != std::string::npos) {
        return args[0];
    }
    return std::nullopt;
}

}  // namespace

BashScriptDialectScope::BashScriptDialectScope(std::string_view content) {
    if (!config::is_posix_mode() && bash_shebang_options(content)) {
        previous_dialect_ = config::shell_dialect();
        config::set_shell_dialect(config::ShellDialect::Bash);
    }
}

BashScriptDialectScope::~BashScriptDialectScope() {
    if (previous_dialect_) {
        config::set_shell_dialect(*previous_dialect_);
    }
}

std::optional<std::vector<std::string>> build_bash_shebang_interpreter_args(
    const std::vector<std::string>& args, const char* cached_path) {
    if (config::is_posix_mode()) {
        return std::nullopt;
    }
    auto script_path = resolve_script_path(args, cached_path);
    if (!script_path || access(script_path->c_str(), X_OK) != 0) {
        return std::nullopt;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(*script_path, ec) || ec) {
        return std::nullopt;
    }

    std::ifstream file(*script_path);
    // Avoid reading a whole first line from an executable binary.
    if (!file || file.get() != '#' || file.get() != '!') {
        return std::nullopt;
    }
    std::string first_line;
    (void)std::getline(file, first_line);
    auto options = bash_shebang_options("#!" + first_line);
    if (!options) {
        return std::nullopt;
    }
    auto executable = cjsh_filesystem::resolve_cjsh_executable_path();
    if (executable.empty()) {
        return std::nullopt;
    }

    std::vector<std::string> interpreter_args{executable, "--no-config", "--bash"};
    (void)interpreter_args.insert(interpreter_args.end(), options->begin(), options->end());
    interpreter_args.push_back("--");
    interpreter_args.push_back(*script_path);
    (void)interpreter_args.insert(interpreter_args.end(), args.begin() + 1, args.end());
    return interpreter_args;
}

std::optional<std::vector<std::string>> build_extension_interpreter_args(
    const std::vector<std::string>& args, const char* cached_path) {
    auto script_path = resolve_script_path(args, cached_path);
    if (!script_path) {
        return std::nullopt;
    }

    std::filesystem::path script_fs(*script_path);
    std::error_code ec;
    bool is_regular = std::filesystem::exists(script_fs, ec) && !ec &&
                      std::filesystem::is_regular_file(script_fs, ec) && !ec;
    if (!is_regular) {
        return std::nullopt;
    }

    auto interpreter = interpreter_for_script_extension(script_fs);
    if (!interpreter || file_has_shebang(script_fs)) {
        return std::nullopt;
    }

    std::vector<std::string> interpreter_args;
    interpreter_args.reserve(args.size() + 1);
    interpreter_args.push_back(*interpreter);
    interpreter_args.push_back(*script_path);
    if (args.size() > 1) {
        (void)interpreter_args.insert(interpreter_args.end(), args.begin() + 1, args.end());
    }
    return interpreter_args;
}

}  // namespace script_dispatch
