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

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "string_utils.h"

namespace script_dispatch {

namespace {

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
