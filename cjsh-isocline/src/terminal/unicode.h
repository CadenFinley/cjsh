/*
  unicode.h

  This file is part of isocline

  MIT License

  Copyright (c) 2026 Caden Finley
  Copyright (c) 2021 Daan Leijen
  Largely modified for CJ's Shell

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

#ifndef IC_UNICODE_H
#define IC_UNICODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t unicode_codepoint_t;

// Core UTF-8 encoding/decoding functions
bool unicode_decode_utf8(const uint8_t* data, ssize_t length, unicode_codepoint_t* codepoint,
                         ssize_t* bytes_read);
int unicode_encode_utf8(unicode_codepoint_t codepoint, uint8_t out[4]);
bool unicode_is_valid_codepoint(unicode_codepoint_t codepoint);

// Character width and properties
int unicode_codepoint_width(unicode_codepoint_t codepoint);
bool unicode_is_combining_codepoint(unicode_codepoint_t codepoint);
bool unicode_is_control_codepoint(unicode_codepoint_t codepoint);

// Display width calculation
// Calculates the display width of a UTF-8 string with ANSI escape sequences
// If count_ansi_chars is not NULL, it will be set to the number of ANSI escape characters
// If count_visible_chars is not NULL, it will be set to the number of visible characters
size_t unicode_calculate_display_width(const char* str, size_t str_len, size_t* count_ansi_chars,
                                       size_t* count_visible_chars);

// Calculates the display width of a UTF-8 string without ANSI escape sequences
size_t unicode_calculate_utf8_width(const char* str, size_t str_len);

#ifdef __cplusplus
}
#endif

#endif  // IC_UNICODE_H
