/*
  keybindings.h

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

/* ----------------------------------------------------------------------------
  Key binding type declarations for CJ's Shell Isocline integration.
  Keeping these in a dedicated header keeps the public API surface of
  `isocline.h` lighter for callers that don't need key binding metadata.
-----------------------------------------------------------------------------*/
#ifndef CJSH_ISOCLINE_SRC_KEYBINDING_KEYBINDINGS_H
#define CJSH_ISOCLINE_SRC_KEYBINDING_KEYBINDINGS_H
#ifdef __cplusplus
#include <cstdint>
#else
#include <stdint.h>
#endif
#ifndef IC_KEYBINDINGS_H
#define IC_KEYBINDINGS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "keycodes.h"

/// Key action identifiers returned by key binding queries and APIs.
typedef enum ic_key_action_e
#ifdef __cplusplus
    : std::uint8_t
#endif
{
    IC_KEY_ACTION_NONE = 0,
    IC_KEY_ACTION_COMPLETE,
    IC_KEY_ACTION_HISTORY_SEARCH,
    IC_KEY_ACTION_HISTORY_PREV,
    IC_KEY_ACTION_HISTORY_NEXT,
    IC_KEY_ACTION_CLEAR_SCREEN,
    IC_KEY_ACTION_UNDO,
    IC_KEY_ACTION_REDO,
    IC_KEY_ACTION_SHOW_HELP,
    IC_KEY_ACTION_CURSOR_LEFT,
    IC_KEY_ACTION_CURSOR_RIGHT_OR_COMPLETE,
    IC_KEY_ACTION_CURSOR_UP,
    IC_KEY_ACTION_CURSOR_DOWN,
    IC_KEY_ACTION_CURSOR_LINE_START,
    IC_KEY_ACTION_CURSOR_LINE_END,
    IC_KEY_ACTION_CURSOR_WORD_PREV,
    IC_KEY_ACTION_CURSOR_WORD_NEXT_OR_COMPLETE,
    IC_KEY_ACTION_CURSOR_INPUT_START,
    IC_KEY_ACTION_CURSOR_INPUT_END,
    IC_KEY_ACTION_CURSOR_MATCH_BRACE,
    IC_KEY_ACTION_DELETE_BACKWARD,
    IC_KEY_ACTION_DELETE_FORWARD,
    IC_KEY_ACTION_DELETE_WORD_END,
    IC_KEY_ACTION_DELETE_WORD_START_WS,
    IC_KEY_ACTION_DELETE_WORD_START,
    IC_KEY_ACTION_DELETE_LINE_START,
    IC_KEY_ACTION_DELETE_LINE_END,
    IC_KEY_ACTION_TRANSPOSE_CHARS,
    IC_KEY_ACTION_YANK_LAST_ARG,
    IC_KEY_ACTION_INSERT_NEWLINE,
    IC_KEY_ACTION_TOGGLE_MOUSE_REPORTING,
    IC_KEY_ACTION_COMMAND_PALETTE,
    IC_KEY_ACTION_RUNOFF,  // Pass key to unhandled key handler (runoff handler)
    IC_KEY_ACTION__MAX
} ic_key_action_e;

#ifdef __cplusplus
typedef ic_key_action_e ic_key_action_t;
#else
typedef uint8_t ic_key_action_t;
#endif

/// Entry describing a single key binding.
typedef struct ic_key_binding_entry_s {
    ic_keycode_t key;
    ic_key_action_t action;
} ic_key_binding_entry_t;

/// Human-readable information about a key binding profile.
typedef struct ic_key_binding_profile_info_s {
    const char* name;
    const char* description;
} ic_key_binding_profile_info_t;

#ifdef __cplusplus
}
#endif

#endif  // IC_KEYBINDINGS_H

#endif  // CJSH_ISOCLINE_SRC_KEYBINDING_KEYBINDINGS_H
