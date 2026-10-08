/*
  cjshopt_registry.h

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

#ifndef CJSH_CORE_SRC_BUILTIN_CJSHOPT_CJSHOPT_REGISTRY_H
#define CJSH_CORE_SRC_BUILTIN_CJSHOPT_CJSHOPT_REGISTRY_H

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

struct CjshoptCompletionValue {
    const char* text;
    const char* description;
    bool option = false;
};

struct CjshoptUsage {
    const char* arguments;
    std::vector<const char*> description;
};

struct CjshoptSubcommandDescriptor {
    const char* name;
    int (*handler)(const std::vector<std::string>&);
    const char* section;
    size_t help_order;
    std::vector<CjshoptUsage> usage;
    const char* summary;
    const char* value_summary;
    std::vector<CjshoptCompletionValue> values;
};

const std::vector<CjshoptSubcommandDescriptor>& cjshopt_subcommands();
const CjshoptSubcommandDescriptor* find_cjshopt_subcommand(std::string_view name);
std::string cjshopt_subcommand_usage(std::string_view name);

#endif  // CJSH_CORE_SRC_BUILTIN_CJSHOPT_CJSHOPT_REGISTRY_H
