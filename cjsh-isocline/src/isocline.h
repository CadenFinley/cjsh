/*
  isocline.h

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

#ifndef IC_ISOCLINE_H
#define IC_ISOCLINE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "keybinding/keybindings.h"
#include "keybinding/keycodes.h"

/// Token returned from ic_readline* when Ctrl+C is pressed with an empty buffer.
#define IC_READLINE_TOKEN_CTRL_C "<CTRL+C>"
/// Token returned from ic_readline* when Ctrl+D is pressed with an empty buffer (EOF).
#define IC_READLINE_TOKEN_CTRL_D "<CTRL+D>"

/// Structured disposition reported by ic_readline_with_status().
typedef enum ic_readline_disposition_e {
    /// A normal command/input buffer was submitted.
    IC_READLINE_DISPOSITION_SUBMIT = 0,
    /// The user interrupted input (usually Ctrl+C on an empty buffer).
    IC_READLINE_DISPOSITION_INTERRUPT,
    /// End-of-file was requested (usually Ctrl+D on an empty buffer).
    IC_READLINE_DISPOSITION_EOF,
    /// Readline stopped because the input backend requested termination.
    IC_READLINE_DISPOSITION_STOP,
    /// Readline encountered an unrecoverable error.
    IC_READLINE_DISPOSITION_ERROR,
    /// Readline paused because the configured idle timeout elapsed.
    IC_READLINE_DISPOSITION_IDLE,
} ic_readline_disposition_t;

/// Structured readline return value that includes terminal state metadata.
typedef struct ic_readline_result_s {
    /// Heap-allocated input buffer returned by readline; free with ic_free().
    /// This can contain IC_READLINE_TOKEN_CTRL_C / IC_READLINE_TOKEN_CTRL_D for
    /// compatibility with ic_readline().
    char* input;
    /// Structured return disposition for this readline cycle.
    ic_readline_disposition_t disposition;
    /// True when isocline is currently backed by an interactive TTY reader.
    bool tty_active;
    /// True when the TTY backend detected a lost terminal connection.
    bool tty_lost;
    /// Cursor position in `input` when readline returned.
    size_t cursor_pos;
} ic_readline_result_t;

/*! \mainpage
Isocline C API reference.

Isocline is a pure C library that can be used as an alternative to the GNU
readline library.

See the [Github repository](https://github.com/cadenfinley/isocline#readme)
for general information and building the library.

Contents:
- \ref readline
- \ref bbcode
- \ref history
- \ref completion
- \ref highlight
- \ref options
- \ref helper
- \ref completex
- \ref term
- \ref async
- \ref alloc
*/

/// \defgroup readline Readline
/// The basic readline interface.
/// \{

/// Isocline version: 102 = 1.0.2.
#define IC_VERSION (104)

/// Read input from the user using rich editing abilities.
/// @param prompt_text   The prompt text, can be NULL for the default ("").
///   The displayed prompt becomes `prompt_text` followed by the `prompt_marker`
///   ("> ").
/// @param inline_right_text Optional text to display right-aligned on the same
///   line as the user input. Can be NULL for no right-aligned text. When user
///   input reaches this text, it will be overridden.
/// @param initial_input Optional initial buffer contents. Pass NULL to start
///   with an empty buffer.
/// @returns the heap allocated input on success, which should be `free`d by the
/// caller.
///   Returns NULL on error. When the user presses ctrl+d or ctrl+c on an empty
///   buffer the function returns the tokens `IC_READLINE_TOKEN_CTRL_D` and
///   `IC_READLINE_TOKEN_CTRL_C` respectively so callers can react accordingly.
///
/// If the standard input (`stdin`) has no editing capability
/// (like a dumb terminal (e.g. `TERM`=`dumb`), running in a debugger, a pipe or
/// redirected file, etc.) the input is read directly from the input stream up
/// to the next line without editing capability. See also \a
/// ic_set_prompt_marker(), \a ic_style_def()
///
/// @see ic_set_prompt_marker(), ic_style_def()
char* ic_readline(const char* prompt_text, const char* inline_right_text,
                  const char* initial_input);

/// Read input from the user and return structured status metadata alongside
/// the optional input text.
/// @param prompt_text Same as ic_readline().
/// @param inline_right_text Same as ic_readline().
/// @param initial_input Same as ic_readline().
/// @returns A structured result. If `result.input` is non-NULL it must be
///          released with ic_free().
ic_readline_result_t ic_readline_with_status(const char* prompt_text, const char* inline_right_text,
                                             const char* initial_input);

/// Read input with an explicitly positioned initial cursor.
/// `initial_cursor_pos` is clamped to the length of `initial_input`.
ic_readline_result_t ic_readline_with_status_at_cursor(const char* prompt_text,
                                                       const char* inline_right_text,
                                                       const char* initial_input,
                                                       size_t initial_cursor_pos);

/// Convert an ic_readline_disposition_t into a stable lowercase string.
/// Returns one of: "submit", "interrupt", "eof", "stop", "idle", "error".
const char* ic_readline_disposition_name(ic_readline_disposition_t disposition);

/// Queue a single key event so it is processed before the next read.
/// Returns `false` if the readline environment is not yet initialized.
bool ic_push_key_event(ic_keycode_t key);

/// Notify isocline that the terminal resized.
/// Safe to call from a signal handler; triggers a wakeup of the input loop.
void ic_notify_resize(void);

/// Wake readline to run its event callback between editing operations.
/// Safe to call from a POSIX signal handler; preserves errno. Events are coalesced
/// and deferred until an open menu or bracketed paste finishes.
void ic_notify_readline(void);

typedef void(ic_readline_event_fun_t)(void* arg);
/// The callback runs on the readline thread and may queue notifications. It must
/// not write directly to the terminal or recursively call readline.
void ic_set_readline_event_callback(ic_readline_event_fun_t* callback, void* arg);

/// Copy text for display above the active prompt at the next safe editing boundary.
/// Call only on the readline thread, including from completion/render callbacks.
/// Text is not interpreted as BBCode; a trailing newline is added if needed.
/// Returns false if no editor owns the terminal or the text could not be queued.
bool ic_queue_notification(const char* text);

/// Callback function type for runoff key events.
/// This callback is invoked when a key is bound to `IC_KEY_ACTION_RUNOFF`.
/// The callback receives the keycode and can return true
/// to indicate the key was handled, or false to let isocline's default
/// behavior continue.
/// @param key The keycode bound to the runoff action
/// @param arg User-provided argument passed when setting the callback
/// @returns true if the key was handled by the callback, false otherwise
typedef bool(ic_unhandled_key_fun_t)(ic_keycode_t key, void* arg);

/// Set a callback for runoff key events.
/// This allows applications to handle custom keybindings that are not
/// directly tied to isocline's built-in actions. Bind each custom key to
/// `IC_KEY_ACTION_RUNOFF` (or the named action `"runoff"`) to invoke it.
/// @param callback The callback function to invoke for runoff keys, or NULL to disable
/// @param arg User-provided argument that will be passed to the callback
void ic_set_unhandled_key_handler(ic_unhandled_key_fun_t* callback, void* arg);

/// Temporarily release the terminal owned by an active readline editor.
/// Use this before launching an external interactive program from a runoff callback.
/// @returns true when an active editor was suspended, false otherwise.
bool ic_suspend_readline_terminal(void);

/// Reacquire and redraw a readline editor suspended with
/// `ic_suspend_readline_terminal()`.
/// @returns true when a suspended editor was resumed, false otherwise.
bool ic_resume_readline_terminal(void);

/// Leave input capture modes before executing commands, preserving queued input.
void ic_prepare_terminal_for_command(void);

/// Adopt foreground-command terminal changes and repair the modes needed by the
/// next prompt. Call after reclaiming terminal ownership and before prompt hooks.
void ic_recover_terminal(void);

/// An item displayed by ic_show_menu().
typedef struct ic_menu_item_s {
    /// Primary text displayed for the item. Must not be NULL or empty.
    const char* label;
    /// Optional secondary text displayed after the label and included in searches.
    const char* description;
    /// Optional additional searchable text that is not displayed.
    const char* keywords;
} ic_menu_item_t;

/// How a selection was accepted from an Isocline custom menu.
typedef enum ic_menu_accept_e {
    /// No item was accepted (the menu was cancelled or could not be shown).
    IC_MENU_ACCEPT_NONE = 0,
    /// The item was accepted for insertion with Tab or a mouse selection.
    IC_MENU_ACCEPT_INSERT,
    /// The item was accepted for immediate submission with Enter.
    IC_MENU_ACCEPT_SUBMIT,
} ic_menu_accept_t;

/// Show a searchable selection menu in the active readline editor.
/// This function is intended to be called synchronously from an unhandled-key
/// callback after binding a custom key to `IC_KEY_ACTION_RUNOFF`. The current
/// input and cursor position are restored when the menu closes.
///
/// Users can navigate with Up/Down (or Ctrl+P/Ctrl+N), page with Shift+Up/Down,
/// filter by typing, toggle case sensitivity with Alt+C, accept with Enter or
/// Tab, and cancel with Escape or Ctrl+C. Mouse selection follows the current
/// mouse-clicking mode.
///
/// The item strings only need to remain valid for the duration of this call.
/// @param prompt_text Prompt shown while filtering, or NULL for `"select: "`.
/// @param items Items to display.
/// @param count Number of items. Must be greater than zero.
/// @param selected_index Optional destination for the selected original item index.
/// @returns true when an item was selected; false on cancellation, invalid input,
///          allocation failure, or when no readline editor is active.
bool ic_show_menu(const char* prompt_text, const ic_menu_item_t* items, size_t count,
                  size_t* selected_index);

/// Show a custom menu and report how its selected item was accepted.
/// This has the same behavior as ic_show_menu(), with `accept` set to
/// IC_MENU_ACCEPT_INSERT for Tab/mouse, IC_MENU_ACCEPT_SUBMIT for Enter, and
/// IC_MENU_ACCEPT_NONE when no item was selected.
bool ic_show_menu_ex(const char* prompt_text, const ic_menu_item_t* items, size_t count,
                     size_t* selected_index, ic_menu_accept_t* accept);

/// Callback that produces a transient status message below the current input.
/// The callback runs before each key read while readline is active. The returned
/// string is copied immediately, so it does not need to remain valid after the
/// function returns. Return NULL or an empty string to hide the status message.
/// @param input_buffer The current readline buffer (UTF-8, null-terminated).
/// @param arg User-provided argument passed to ic_set_status_message_callback()
/// @returns Pointer to a UTF-8 string or NULL for no message.
typedef const char*(ic_status_message_fun_t)(const char* input_buffer, void* arg);

/// Set the status message callback that renders text below the current input.
/// @param callback The callback that produces the status message, or NULL to disable
/// @param arg User-provided argument forwarded to the callback
void ic_set_status_message_callback(ic_status_message_fun_t* callback, void* arg);

/// Callback invoked when isocline is about to submit the current buffer.
/// Return `true` to exit the readline loop and hand the buffer back to the caller.
/// Return `false` to keep editing; isocline inserts a newline (when multiline editing is enabled)
/// and continues reading input.
/// @param input_buffer Current UTF-8 buffer contents (never NULL; empty string when buffer is
/// empty)
/// @param arg User-provided argument passed to ic_set_check_for_continuation_or_return_callback()
typedef bool(ic_check_for_continuation_or_return_fun_t)(const char* input_buffer, void* arg);

/// Configure the continuation-or-return callback.
/// The callback runs whenever the user presses Enter, when ic_request_submit() fires, and when
/// the initial buffer ends with a newline. Pass NULL to restore the default behavior (always
/// submit immediately).
/// @param callback The callback that decides whether to exit the readline loop.
/// @param arg User-provided argument forwarded to the callback
void ic_set_check_for_continuation_or_return_callback(
    ic_check_for_continuation_or_return_fun_t* callback, void* arg);

/// Controls when the built-in status hint line (the underlined control hints) is displayed.
/// - `IC_STATUS_HINT_OFF`: never show the built-in hints.
/// - `IC_STATUS_HINT_NORMAL`: show only when both the input buffer and the status line are empty.
/// - `IC_STATUS_HINT_TRANSIENT`: show whenever the status line has no other content (default).
/// - `IC_STATUS_HINT_PERSISTENT`: always show and prepend the hints above other status messages.
typedef enum ic_status_hint_mode_e {
    IC_STATUS_HINT_OFF = 0,
    IC_STATUS_HINT_NORMAL,
    IC_STATUS_HINT_TRANSIENT,
    IC_STATUS_HINT_PERSISTENT,
} ic_status_hint_mode_t;

/// Set when the default status hint line should be rendered. Returns the previous mode.
ic_status_hint_mode_t ic_set_status_hint_mode(ic_status_hint_mode_t mode);

/// Get the current status hint rendering mode.
ic_status_hint_mode_t ic_get_status_hint_mode(void);

/// Mouse capture behavior for readline sessions.
/// - `IC_MOUSE_CLICKING_DISABLED`: never capture mouse events, including in menus.
/// - `IC_MOUSE_CLICKING_SIMPLE`: start with mouse capture enabled; only manual toggle changes it.
/// - `IC_MOUSE_CLICKING_SMART`: start enabled and auto-suspend on wheel input or selection starts
///   above the editor, in prompt/gutter cells, in status/helper rows, or on left-button dragging;
///   then auto-resume on a reported left-button release or keyboard/focus-in input. Resuming on
///   release preserves the display until the next click/key. Disabling capture also stops release
///   reports, so resuming on release is best-effort; keyboard input remains the portable fallback.
///   Native selection during the same drag depends on the terminal; some terminals require a
///   second drag after capture is released.
/// - `IC_MOUSE_CLICKING_MENU_ONLY`: leave editing capture off and acquire it only while an
///   completion, history, or command-palette menu is open.
/// While a menu owns mouse capture, clicking outside its selectable items temporarily releases
/// capture to the terminal. Keyboard input or a terminal focus-in event restores it.
typedef enum ic_mouse_clicking_mode_e {
    IC_MOUSE_CLICKING_DISABLED = 0,
    IC_MOUSE_CLICKING_SIMPLE,
    IC_MOUSE_CLICKING_SMART,
    IC_MOUSE_CLICKING_MENU_ONLY,
} ic_mouse_clicking_mode_t;

/// Set mouse capture mode. Invalid values normalize to `IC_MOUSE_CLICKING_SMART`.
/// Returns the previous mode.
ic_mouse_clicking_mode_t ic_set_mouse_clicking_mode(ic_mouse_clicking_mode_t mode);

/// Get the configured mouse capture mode.
ic_mouse_clicking_mode_t ic_get_mouse_clicking_mode(void);

/// Queue multiple key events so they are processed before the next read.
/// Returns `false` if the readline environment is not yet initialized.
bool ic_push_key_sequence(const ic_keycode_t* keys, size_t count);

/// Queue raw terminal bytes (including escape sequences) for processing.
/// Returns `false` if the readline environment is not yet initialized.
bool ic_push_raw_input(const uint8_t* data, size_t length);

/// Callback that decides if isocline may opportunistically capture currently
/// queued terminal input for typeahead.
/// Return true to allow capture, or false to leave stdin untouched.
typedef bool(ic_typeahead_capture_allowed_fun_t)(void* arg);

/// Enable or disable typeahead buffering. Returns the previous enabled state.
/// When enabled, isocline can capture bytes already queued on stdin and seed
/// the next readline session with them. A captured carriage return is replayed
/// as Return (submit), while a captured line feed retains Ctrl+J behavior.
bool ic_enable_typeahead(bool enable);

/// Returns true when typeahead buffering is enabled.
bool ic_typeahead_is_enabled(void);

/// Set a callback used to gate opportunistic typeahead capture.
/// Shells can use this to avoid stealing input while a foreground job owns
/// stdin. Pass NULL to allow capture whenever typeahead is enabled.
void ic_set_typeahead_capture_allowed_callback(ic_typeahead_capture_allowed_fun_t* callback,
                                               void* arg);

/// Capture currently queued terminal input into isocline's typeahead buffer.
/// Returns true when any bytes were captured.
bool ic_typeahead_capture_available_input(void);

/// Clear all pending typeahead state.
void ic_typeahead_clear(void);

/// Read heredoc content with full editing capabilities.
/// Reads lines from the user until the specified delimiter is encountered on a line by itself.
/// Each line is read with full isocline editing capabilities (history, completion, etc.).
/// The delimiter line is NOT included in the returned content.
/// @param delimiter The heredoc delimiter string (e.g., "EOF").
/// @param strip_tabs If true, leading tabs are stripped from each line (for <<- syntax).
/// @returns A heap-allocated string containing all heredoc lines joined with newlines,
///          or NULL on error/cancellation. The caller must free() the result.
///          If ctrl-C or ctrl-D is pressed, returns NULL.
char* ic_read_heredoc(const char* delimiter, bool strip_tabs);

/// Set the current input buffer to the specified string.
/// This function can only be called while a readline operation is active.
/// @param buffer The string to set as the current input buffer. If NULL, clears the buffer.
/// @returns true if the buffer was successfully set, false if no readline operation is active.
bool ic_set_buffer(const char* buffer);

/// Get the current input buffer content.
/// This function can only be called while a readline operation is active.
/// The returned string is valid until the next edit operation or until readline returns.
/// @returns A pointer to the current buffer content, or NULL if no readline operation is active.
///          The returned string should NOT be freed by the caller.
const char* ic_get_buffer(void);

/// Get the current cursor position in the input buffer.
/// This function can only be called while a readline operation is active.
/// @param out_pos Pointer to receive the cursor position (byte offset from start of buffer).
/// @returns true if the position was successfully retrieved, false if no readline operation is
/// active.
bool ic_get_cursor_pos(size_t* out_pos);

/// Set the current cursor position in the input buffer.
/// This function can only be called while a readline operation is active.
/// @param pos The desired cursor position (byte offset from start of buffer).
/// @returns true if the position was successfully set, false if no readline operation is active.
bool ic_set_cursor_pos(size_t pos);

bool ic_request_submit(void);

/// Preserve the current prompt and input as terminal output, then continue the
/// active readline operation on a fresh copy of the same prompt.
/// @param new_buffer Buffer for the fresh prompt. Pass NULL for an empty buffer.
/// @returns true if the editor was advanced, false if no readline operation is active.
bool ic_current_loop_advance(const char* new_buffer);

/// Like ic_current_loop_advance(), but optionally redraw the prompt being preserved before
/// advancing. The fresh editable prompt restores the prompt and right prompt that were active
/// before this call.
/// @param preserved_prompt Replacement prompt for the preserved input, or NULL to keep it.
/// @param preserved_inline_right Replacement right prompt for the preserved input, or NULL to
/// keep it. Pass an empty string to clear it.
bool ic_current_loop_advance_with_prompt(const char* new_buffer, const char* preserved_prompt,
                                         const char* preserved_inline_right);

bool ic_current_loop_reset(const char* new_buffer, const char* new_prompt,
                           const char* new_inline_right);

/// \}

//--------------------------------------------------------------
/// \defgroup bbcode Formatted Text
/// Formatted text using [bbcode
/// markup](https://github.com/daanx/isocline#bbcode-format).
/// \{

/// Print to the terminal while respection bbcode markup.
/// Any unclosed tags are closed automatically at the end of the print.
/// For example:
/// ```
/// ic_print("[b]bold, [i]bold and italic[/i], [red]red and bold[/][/b]
/// default."); ic_print("[b]bold[/], [i b]bold and italic[/], [yellow on
/// blue]yellow on blue background"); ic_style_add("em","i color=#888800");
/// ic_print("[em]emphasis");
/// ```
/// Properties that can be assigned are:
/// * `color=` _clr_, `bgcolor=` _clr_: where _clr_ is either a hex value
/// `#`RRGGBB or `#`RGB, a
///    standard HTML color name, or an ANSI palette name, like `ansi-maroon`,
///    `ansi-default`, etc.
/// * `bold`,`italic`,`reverse`,`underline`: can be `on` or `off`.
/// * everything else is a style; all HTML and ANSI color names are also a style
/// (so we can just use `red`
///   instead of `color=red`, or `on red` instead of `bgcolor=red`), and there
///   are the `b`, `i`, `u`, and `r` styles for bold, italic, underline, and
///   reverse.
///
/// See [here](https://github.com/daanx/isocline#bbcode-format) for a
/// description of the full bbcode format.
void ic_print(const char* s);

/// Print with bbcode markup ending with a newline.
/// @see ic_print()
void ic_println(const char* s);

/// Print formatted with bbcode markup.
/// @see ic_print()
void ic_printf(const char* fmt, ...);

/// Print formatted with bbcode markup.
/// @see ic_print
void ic_vprintf(const char* fmt, va_list args);

/// Define or redefine a style.
/// @param style_name The name of the style.
/// @param fmt        The `fmt` string is the content of a tag and can contain
///   other styles. This is very useful to theme the output of a program
///   by assigning standard styles like `em` or `warning` etc.
void ic_style_def(const char* style_name, const char* fmt);

/// Start a global style that is only reset when calling a matching
/// ic_style_close().
void ic_style_open(const char* fmt);

/// End a global style.
void ic_style_close(void);

/// \}

//--------------------------------------------------------------
// History
//--------------------------------------------------------------
/// \defgroup history History
/// Readline input history.
/// \{

/// Enable history.
/// Use a \a NULL filename to not persist the history. Use -1 for max_entries to
/// get the default (200).
void ic_set_history(const char* fname, long max_entries);

/// Remove the last entry in the history.
/// The last returned input from ic_readline() is automatically added to the
/// history; this function removes it.
void ic_history_remove_last(void);

/// Clear the history.
void ic_history_clear(void);

/// Metadata key/value pair attached to a history entry.
typedef struct ic_history_metadata_s {
    const char* key;
    const char* value;
} ic_history_metadata_t;

/// Add an entry to the history with custom metadata.
/// Without custom metadata, isocline records `timestamp` and `frequency`. With custom metadata,
/// either key is only recorded when provided; a value of `0` requests isocline's generated value.
void ic_history_add_with_metadata(const char* entry, const ic_history_metadata_t* metadata,
                                  size_t metadata_count);

/// Add an entry to the history
void ic_history_add(const char* entry);
/// Force save in-memory history to the history file.
void ic_history_save(void);

/// Decode one persisted history entry without trimming it. The output buffer must have
/// room for encoded_length + 1 bytes and may be the same buffer as encoded.
/// Return false for malformed escapes or insufficient capacity; decoded_length excludes
/// the terminating zero and is set to zero on failure.
bool ic_history_decode_entry(const char* encoded, size_t encoded_length, char* decoded,
                             size_t decoded_capacity, size_t* decoded_length);

/// \}

//--------------------------------------------------------------
// Basic Completion
//--------------------------------------------------------------

/// \defgroup completion Completion
/// Basic word completion.
/// \{

/// A completion environment
struct ic_completion_env_s;

/// A completion environment
typedef struct ic_completion_env_s ic_completion_env_t;

/// A completion callback that is called by isocline when tab is pressed.
/// It is passed a completion environment (containing the current input and the
/// current cursor position), the current input up-to the cursor (`prefix`) and
/// the user given argument when the callback was set. When using completion
/// transformers, like `ic_complete_quoted_word` the `prefix` contains the the
/// word to be completed without escape characters or quotes.
typedef void(ic_completer_fun_t)(ic_completion_env_t* cenv, const char* prefix);

/// Set the default completion handler.
/// @param completer  The completion function
/// @param arg        Argument passed to the \a completer.
/// There can only be one default completion function, setting it again disables
/// the previous one. The initial completer use `ic_complete_filename`.
void ic_set_default_completer(ic_completer_fun_t* completer, void* arg);

/// In a completion callback (usually from ic_complete_word()), use this
/// function to add a completion. (the completion string is copied by isocline
/// and do not need to be preserved or allocated).
///
/// Returns `true` if the callback should continue trying to find more possible
/// completions. If `false` is returned, the callback should try to return and
/// not add more completions (for improved latency).
bool ic_add_completion(ic_completion_env_t* cenv, const char* completion);

/// In a completion callback (usually from ic_complete_word()), use this
/// function to add a completion. The `display` is used to display the
/// completion in the completion menu, and `help` is displayed for hints for
/// example. Both can be `NULL` for the default. (all are copied by isocline and
/// do not need to be preserved or allocated).
///
/// Returns `true` if the callback should continue trying to find more possible
/// completions. If `false` is returned, the callback should try to return and
/// not add more completions (for improved latency).
bool ic_add_completion_ex(ic_completion_env_t* cenv, const char* completion, const char* display,
                          const char* help);

/// In a completion callback (usually from ic_complete_word()), use this
/// function to add a completion with source information. The `display` is used
/// to display the completion in the completion menu, `help` is displayed for
/// hints, and `source` indicates where the completion came from (e.g.,
/// "history", "file", "command"). All parameters can be `NULL` for the default.
/// (all are copied by isocline and do not need to be preserved or allocated).
///
/// Returns `true` if the callback should continue trying to find more possible
/// completions. If `false` is returned, the callback should try to return and
/// not add more completions (for improved latency).
bool ic_add_completion_ex_with_source(ic_completion_env_t* cenv, const char* completion,
                                      const char* display, const char* help, const char* source);

/// In a completion callback (usually from ic_complete_word()), use this
/// function to add completions. The `completions` array should be terminated
/// with a NULL element, and all elements are added as completions if they start
/// with `prefix`.
///
/// Returns `true` if the callback should continue trying to find more possible
/// completions. If `false` is returned, the callback should try to return and
/// not add more completions (for improved latency).
bool ic_add_completions(ic_completion_env_t* cenv, const char* prefix, const char** completions);

/// Complete a filename.
/// Complete a filename given a semi-colon separated list of root directories
/// `roots` and semi-colon separated list of possible extensions (excluding
/// directories). If `roots` is NULL, the current directory is the root (".").
/// If `extensions` is NULL, any extension will match.
/// Each root directory should _not_ end with a directory separator.
/// If a directory is completed, the `dir_separator` is added at the end if it
/// is not `0`. Usually the `dir_separator` is `/` but it can be set to `\\` on
/// Windows systems. For example:
/// ```
/// /ho         --> /home/
/// /home/.ba   --> /home/.bashrc
/// ```
/// (This already uses ic_complete_quoted_word() so do not call it from inside a
/// word handler).
void ic_complete_filename(ic_completion_env_t* cenv, const char* prefix, char dir_separator,
                          const char* roots, const char* extensions);

/// Function that returns whether a (utf8) character (of length `len`) is in a
/// certain character class
/// @see ic_char_is_separator() etc.
typedef bool(ic_is_char_class_fun_t)(const char* s, long len);

/// Complete a _word_ (i.e. _token_).
/// Calls the user provided function `fun` to complete on the
/// current _word_. Almost all user provided completers should use this
/// function. If `is_word_char` is NULL, the default `&ic_char_is_nonseparator`
/// is used. The `prefix` passed to `fun` is modified to only contain the
/// current word, and any results from `ic_add_completion` are automatically
/// adjusted to replace that part. For example, on the input "hello w", a the
/// user `fun` only gets `w` and can just complete with "world" resulting in
/// "hello world" without needing to consider `delete_before` etc.
/// @see ic_complete_qword() for completing quoted and escaped tokens.
void ic_complete_word(ic_completion_env_t* cenv, const char* prefix, ic_completer_fun_t* fun,
                      ic_is_char_class_fun_t* is_word_char);

/// Complete a quoted _word_.
/// Calls the user provided function `fun` to complete while taking
/// care of quotes and escape characters. Almost all user provided completers
/// should use this function. The `prefix` passed to `fun` is modified to be
/// unquoted and unescaped, and any results from `ic_add_completion` are
/// automatically quoted and escaped again. For example, completing `hello
/// world`, the `fun` always just completes `hel` or `hello w` to `hello world`,
/// but depending on user input, it will complete as:
/// ```
/// hel        -->  hello\ world
/// hello\ w   -->  hello\ world
/// hello w    -->                   # no completion, the word is just 'w'>
/// "hel       -->  "hello world"
/// "hello w   -->  "hello world"
/// ```
/// with proper quotes and escapes.
/// If `is_word_char` is NULL, the default `&ic_char_is_nonseparator` is used.
/// @see ic_complete_quoted_word() to customize the word boundary, quotes etc.
void ic_complete_qword(ic_completion_env_t* cenv, const char* prefix, ic_completer_fun_t* fun,
                       ic_is_char_class_fun_t* is_word_char);

/// Complete a _word_.
/// Calls the user provided function `fun` to complete while taking
/// care of quotes and escape characters. Almost all user provided completers
/// should use this function. The `is_word_char` is a set of characters that are
/// part of a "word". Use NULL for the default (`&ic_char_is_nonseparator`). The
/// `escape_char` is the escaping character, usually `\` but use 0 to not have
/// escape characters. The `quote_chars` define the quotes, use NULL for the
/// default `"\'\""` quotes.
/// @see ic_complete_word() which uses the default values for `non_word_chars`,
/// `quote_chars` and `\` for escape characters.
void ic_complete_qword_ex(ic_completion_env_t* cenv, const char* prefix, ic_completer_fun_t fun,
                          ic_is_char_class_fun_t* is_word_char, char escape_char,
                          const char* quote_chars);

/// \}

//--------------------------------------------------------------
/// \defgroup highlight Syntax Highlighting
/// Basic syntax highlighting.
/// \{

/// A syntax highlight environment
struct ic_highlight_env_s;
typedef struct ic_highlight_env_s ic_highlight_env_t;

/// A syntax highlighter callback that is called by readline to syntax highlight
/// user input.
typedef void(ic_highlight_fun_t)(ic_highlight_env_t* henv, const char* input, void* arg);

/// Set a syntax highlighter.
/// There can only be one highlight function, setting it again disables the
/// previous one.
void ic_set_default_highlighter(ic_highlight_fun_t* highlighter, void* arg);

/// Set the style of characters starting at position `pos`.
void ic_highlight(ic_highlight_env_t* henv, long pos, long count, const char* style);

/// Experimental: Convenience callback for a function that highlights `s` using
/// bbcode's. The returned string should be allocated and is free'd by the
/// caller.
typedef char*(ic_highlight_format_fun_t)(const char* s, void* arg);

/// Experimental: Convenience function for highlighting with bbcodes.
/// Can be called in a `ic_highlight_fun_t` callback to colorize the `input`
/// using the the provided `formatted` input that is the styled `input` with
/// bbcodes. The content of `formatted` without bbcode tags should match `input`
/// exactly.
void ic_highlight_formatted(ic_highlight_env_t* henv, const char* s, const char* fmt);

/// \}

//--------------------------------------------------------------
// Readline with a specific completer and highlighter
//--------------------------------------------------------------

/// \defgroup readline
/// \{

/// Read input from the user using rich editing abilities,
/// using a particular completion function and highlighter for this call only.
/// both can be NULL in which case the defaults are used.
/// @see ic_readline(), ic_set_prompt_marker(), ic_set_default_completer(),
/// ic_set_default_highlighter().
char* ic_readline_ex(const char* prompt_text, ic_completer_fun_t* completer, void* completer_arg,
                     ic_highlight_fun_t* highlighter, void* highlighter_arg);

/// \}

//--------------------------------------------------------------
// Options
//--------------------------------------------------------------

/// \defgroup options Options
/// \{

/// Set a prompt marker and a potential marker for extra lines with multiline
/// input. Pass \a NULL for the `prompt_marker` for the default marker (`"> "`).
/// Pass \a NULL for continuation prompt marker to make it equal to the
/// `prompt_marker`.
void ic_set_prompt_marker(const char* prompt_marker, const char* cprompt_marker);

/// Get the current prompt marker.
const char* ic_get_prompt_marker(void);

/// Get the current continuation prompt marker.
const char* ic_get_continuation_prompt_marker(void);

/// Set or clear the marker used when preserving partial pre-prompt output.
/// Pass NULL to restore the built-in default marker.
void ic_set_prompt_eol_mark(const char* eol_mark);

/// Get the current partial-line marker override.
/// Returns NULL when the built-in default marker is active.
const char* ic_get_prompt_eol_mark(void);

/// Set the prompt shown while fuzzy-searching history.
/// Pass NULL to restore the default ("history search: ").
void ic_set_history_search_prompt(const char* prompt_text);

/// Get the current history search prompt.
const char* ic_get_history_search_prompt(void);

/// Set the prompt shown while searching the command palette.
/// Pass NULL to restore the default ("command palette: ").
void ic_set_command_palette_prompt(const char* prompt_text);

/// Get the current command palette prompt.
const char* ic_get_command_palette_prompt(void);

/// Disable or enable multi-line input (enabled by default).
/// Returns the previous setting.
bool ic_enable_multiline(bool enable);
/// Return the current setting without changing it.
bool ic_multiline_is_enabled(void);

/// Configure whether Enter retains the trailing continuation character when it starts a new
/// multiline input row. When disabled (default), the continuation character is replaced by the
/// inserted newline. When enabled, the newline is inserted after it so the returned input keeps
/// the continuation character. Returns the previous setting.
bool ic_enable_multiline_continuation_retention(bool enable);

/// Disable or enable sound (enabled by default).
/// A beep is used when tab cannot find any completion for example.
/// Returns the previous setting.
bool ic_enable_beep(bool enable);

/// Disable or enable color output (enabled by default).
/// Returns the previous setting.
bool ic_enable_color(bool enable);

/// Automatically record submitted readline input (default: true). Disable when
/// the host records commands with metadata after execution. Drafts stay private.
/// Returns the previous setting.
bool ic_enable_history_auto_add(bool enable);

/// Disable or enable duplicate entries in the history (disabled by default).
/// Returns the previous setting.
bool ic_enable_history_duplicates(bool enable);

/// Configure whether the fuzzy history search menu matches case-sensitively (default: enabled).
/// Returns the previous setting.
bool ic_enable_history_fuzzy_case_sensitive(bool enable);

/// Report whether the fuzzy history search menu currently matches case-sensitively.
bool ic_history_fuzzy_search_is_case_sensitive(void);

/// Scope interactive history recall to the directory supplied by the host (default: false).
/// Entries without cwd metadata are excluded while scoped. Returns the previous setting.
bool ic_enable_history_directory(bool enable);
bool ic_history_directory_is_enabled(void);

/// Include descendants of the current directory when scoped (default: false).
/// Returns the previous setting.
bool ic_enable_history_directory_subdirs(bool enable);
bool ic_history_directory_subdirs_is_enabled(void);

/// Set the physical absolute directory used for history recall. The string is copied.
/// NULL or empty means unknown; scoped recall then returns no stored entries.
bool ic_set_history_directory(const char* directory);

/// Test decoded cwd metadata against the active scope (for host-provided history completions).
bool ic_history_matches_directory(const char* directory);

/// History search menu sort arrangements.
typedef enum ic_history_search_sort_e {
    /// Sort matches by the most recent time the command was run.
    IC_HISTORY_SEARCH_SORT_RECENT = 0,
    /// Sort matches alphabetically by command text, ascending.
    IC_HISTORY_SEARCH_SORT_COMMAND_ASC = 1,
    /// Sort matches alphabetically by command text, descending.
    IC_HISTORY_SEARCH_SORT_COMMAND_DESC = 2,
    /// Sort matches by a history metadata key, ascending.
    IC_HISTORY_SEARCH_SORT_METADATA_ASC = 3,
    /// Sort matches by a history metadata key, descending.
    IC_HISTORY_SEARCH_SORT_METADATA_DESC = 4,
} ic_history_search_sort_t;

/// Configure the default fuzzy history search sort.
/// For metadata modes, `metadata_key` names any built-in or custom metadata key.
/// Missing metadata values sort after present values. Metadata values sort
/// numerically when both values are valid integers, otherwise lexicographically.
/// Returns false if the mode or metadata key is invalid.
bool ic_set_history_search_sort(ic_history_search_sort_t sort, const char* metadata_key);

/// Return the current default fuzzy history search sort.
/// If `metadata_key` is not NULL, it receives the configured metadata key for
/// metadata modes, or NULL for non-metadata modes. The returned key is owned by
/// isocline and remains valid until the sort is changed.
ic_history_search_sort_t ic_get_history_search_sort(const char** metadata_key);

/// Disable or enable automatic tab completion after a completion
/// to expand as far as possible if the completions are unique. (disabled by
/// default). Returns the previous setting.
bool ic_enable_auto_tab(bool enable);
/// Return the current setting without changing it.
bool ic_auto_tab_is_enabled(void);

/// Disable or enable preview of a completion selection (enabled by default)
/// Returns the previous setting.
bool ic_enable_completion_preview(bool enable);
/// Return the current setting without changing it.
bool ic_completion_preview_is_enabled(void);

/// Show an unselected completion menu while editing (disabled by default).
/// Tab activates the menu without accepting or extending the input, even for a single match.
/// Until activation, navigation edits the input and menu scrolling/click acceptance are disabled.
/// With prompt mouse capture enabled, a click activates and selects the clicked entry (or the
/// first entry for a header/footer click), never accepting it on that activating click.
/// After acceptance, suggestions refresh and remain passive until Tab or a click activates them.
/// Empty lines or no matches hide the menu; Escape hides it until the next edit.
/// The passive menu replaces inline hints, independently of the hint and preview settings.
/// Returns the previous setting.
bool ic_enable_completion_auto_menu(bool enable);

/// Is the automatic, passive completion menu enabled?
bool ic_completion_auto_menu_is_enabled(void);

/// Enable or disable click-to-accept for completion candidates (disabled by default).
/// Returns the previous setting.
bool ic_enable_completion_click_accept(bool enable);

/// Returns whether click-to-accept for completion candidates is currently enabled.
bool ic_completion_click_accept_is_enabled(void);

/// Menu item syntax-highlighting mode for completion and history search menus.
typedef enum ic_menu_highlight_mode_e {
    IC_MENU_HIGHLIGHT_NONE = 0,
    IC_MENU_HIGHLIGHT_SINGLE = 1,
    IC_MENU_HIGHLIGHT_ALL = 2,
    IC_MENU_HIGHLIGHT_REVERSE = 3,
} ic_menu_highlight_mode_t;

/// Configure syntax highlighting for completion and history menu items.
/// `none` preserves the legacy unhighlighted menu, `single` highlights only
/// the selected item, `all` highlights every rendered item, and `reverse`
/// highlights every rendered item except the selected item.
/// Returns the previous mode.
ic_menu_highlight_mode_t ic_set_menu_highlight_mode(ic_menu_highlight_mode_t mode);

/// Returns the current completion/history menu item highlighting mode.
ic_menu_highlight_mode_t ic_get_menu_highlight_mode(void);

/// Disable or enable automatic indentation of continuation lines in multiline
/// input. This aligns continuation prompts and indents lines created by
/// automatic continuation. (enabled by default)
/// Returns the previous setting.
bool ic_enable_multiline_indent(bool enable);
/// Return the current setting without changing it.
bool ic_multiline_indent_is_enabled(void);

/// Configure how many lines the editor should preallocate when multiline editing is enabled.
/// The default is 1, which means the cursor starts on the first prompt line. Larger values
/// insert blank newline-separated rows before the cursor so that editing begins on the last
/// preallocated line (for example, 2 renders the "2|" line immediately). Values below 1 are
/// clamped to 1.
/// Returns the previous configured line count.
size_t ic_set_multiline_start_line_count(size_t line_count);

/// Get the current number of preallocated lines for multiline editing.
size_t ic_get_multiline_start_line_count(void);

/// Configure the maximum number of input rows shown while editing a multiline command.
/// The default is 15. When the input needs more rows, the editor scrolls the viewport to keep
/// the cursor visible. Values below 1 are clamped to 1 and values above 256 are clamped to 256.
/// Returns the previous configured line count.
size_t ic_set_multiline_max_line_count(size_t line_count);

/// Get the current maximum number of visible input rows for multiline editing.
size_t ic_get_multiline_max_line_count(void);

/// Configure the symmetric scroll margin for multiline input and menus. The editor retains up to
/// this many existing rows below the cursor or selection when moving down and above it when moving
/// up. The default is 3. Blank rows are never inserted to satisfy the margin. A value of 0 disables
/// the margin, and values above 256 are clamped to 256.
/// Returns the previous configured line count.
size_t ic_set_multiline_bottom_line_count(size_t line_count);

/// Get the preferred number of content rows retained around the cursor or menu selection.
size_t ic_get_multiline_bottom_line_count(void);

/// Configure the maximum visible content rows in completion, history, command palette, and custom
/// menus, including expanded item previews. The default is 50. Headers and help text use separate
/// rows, and menus shrink to fit the terminal. Values are clamped to the range 1 through 256.
/// Returns the previous configured line count.
size_t ic_set_menu_max_line_count(size_t line_count);

/// Get the current maximum number of visible menu content rows.
size_t ic_get_menu_max_line_count(void);

/// Enable or disable line numbers in multiline input mode. (enabled by default)
/// When enabled, each line will be prefixed with a line number (e.g., "2| ", "3| ", etc.).
/// The first line continues to use the regular prompt.
/// Returns the previous setting.
bool ic_enable_line_numbers(bool enable);

/// Enable or disable relative numbering for multiline line numbers.
/// Relative numbering displays the distance from the active cursor line instead of the absolute
/// line index (the current line shows "0"). Enabling relative numbering automatically enables
/// line numbers.
/// Returns the previous relative-numbering state.
bool ic_enable_relative_line_numbers(bool enable);

/// Returns whether line numbers are currently enabled (either absolute or relative).
bool ic_line_numbers_are_enabled(void);

/// Returns whether relative line numbers are currently active.
bool ic_line_numbers_are_relative(void);

/// Allow line numbers to remain visible even when a continuation prompt marker is configured.
/// When disabled (default), setting a continuation prompt hides multiline line numbers.
/// Returns the previous state.
bool ic_enable_line_numbers_with_continuation_prompt(bool enable);

/// Returns whether line numbers stay visible when a continuation prompt is set.
bool ic_line_numbers_with_continuation_prompt_are_enabled(void);

/// Replace the final line of a multi-line prompt with the line-number column.
/// When enabled, the last PS1 line will be dropped whenever line numbers are
/// displayed (e.g., when line numbers are active and either no PS2 is set or
/// continuation line numbers are forced). This makes the `1|` gutter appear in
/// place of the final prompt line so that every editable row shares a common
/// prefix.
bool ic_enable_line_number_prompt_replacement(bool enable);

/// Returns whether the last line of a multi-line prompt is replaced by the
/// line-number column.
bool ic_line_number_prompt_replacement_is_enabled(void);

/// Enable or disable highlighting of the current line number.
/// When enabled (default), the line number for the line containing the cursor
/// is displayed in a different color ("ic-linenumber-current" style) compared to
/// other line numbers ("ic-linenumbers" style).
/// Returns the previous setting.
bool ic_enable_current_line_number_highlight(bool enable);

/// Returns whether current line number highlighting is enabled.
bool ic_current_line_number_highlight_is_enabled(void);

/// Set the marker at the end of soft-wrapped editor rows. The string is copied and must be
/// empty (disable the marker) or exactly one printable UTF-8 code point with positive display
/// width. Passing NULL restores the default: a return symbol on macOS, a left arrow elsewhere.
/// The marker reserves its display width; an empty marker lets input use the full terminal width.
/// Non-ASCII markers are shown only on UTF-8 terminals. Input contents and the newline printed
/// after submitting input are unaffected. Returns false without changing the marker on failure.
bool ic_set_line_wrap_marker(const char* marker);
/// Get the current marker (empty when disabled). The returned string is owned by isocline and
/// remains valid until the next successful setter call. Returns NULL if initialization fails.
const char* ic_get_line_wrap_marker(void);

/// Enable or disable visualization of plain space characters inside the buffer (disabled by
/// default). When enabled, every space is rendered using the whitespace marker returned by
/// `ic_get_whitespace_marker()`. Returns the previous state.
bool ic_enable_visible_whitespace(bool enable);
/// Return the current setting without changing it.
bool ic_visible_whitespace_is_enabled(void);

/// Set the marker string used when visualizing spaces. The string is copied so the caller retains
/// ownership. Passing NULL or an empty string restores the default middle-dot marker.
void ic_set_whitespace_marker(const char* marker);

/// Get the marker string currently used for visible whitespace (defaults to a middle dot).
const char* ic_get_whitespace_marker(void);

/// Disable or enable display of short help messages for history search etc. (enabled by default)
/// (full help is always dispayed when pressing F1 regardless of this setting)
/// @returns the previous setting.
bool ic_enable_inline_help(bool enable);
/// Return the current setting without changing it.
bool ic_inline_help_is_enabled(void);

/// Enable or disable cursor-tracking for the inline right prompt (RPS1).
/// When enabled, the right-aligned prompt is re-rendered on the same terminal row as the cursor
/// instead of remaining pinned to the first editable row. Returns the previous setting.
bool ic_enable_inline_right_prompt_cursor_follow(bool enable);

/// Returns whether the right-aligned prompt follows the cursor height.
bool ic_inline_right_prompt_follows_cursor(void);

/// Legacy compatibility helper for older callers.
/// `true` enables per-prompt mouse capture defaults (equivalent to SIMPLE mode startup behavior);
/// `false` disables startup capture while preserving the current mode.
/// Returns the previous startup-default setting.
bool ic_enable_mouse_clicking(bool enable);

/// Enable or disable the status-line indicator that says mouse clicking is enabled.
/// Disabling this hides the indicator text but does not disable mouse clicking support itself.
/// Returns the previous setting.
bool ic_enable_mouse_reporting_status_line(bool enable);
/// Return the current setting without changing it.
bool ic_mouse_reporting_status_line_is_enabled(void);

/// Disable or enable hinting (enabled by default)
/// Shows a hint inline when there is a single possible completion.
/// @returns the previous setting.
bool ic_enable_hint(bool enable);
/// Return the current setting without changing it.
bool ic_hint_is_enabled(void);

/// Disable or enable spell correction in completion (enabled by default).
/// When enabled and no completion matches, tab will try to correct the
/// current token to the closest available completion.
/// @returns the previous setting.
bool ic_enable_spell_correct(bool enable);

/// Disable or enable spell correction when submitting with Enter (disabled by default).
/// When enabled, pressing Enter applies a single available spell correction suggestion
/// before accepting the line.
/// @returns the previous setting.
bool ic_enable_spell_correct_on_enter(bool enable);

/// Set millisecond delay before a hint is displayed. Can be zero. (0ms by
/// default).
long ic_set_hint_delay(long delay_ms);

/// Return the current hint display delay in milliseconds.
long ic_get_hint_delay(void);

/// Set the inactivity timeout for the active readline loop in milliseconds.
/// Zero disables idle timeouts. Returns the previous timeout.
long ic_set_idle_timeout(long timeout_ms);

/// Disable or enable syntax highlighting (enabled by default).
/// This applies regardless whether a syntax highlighter callback was set
/// (`ic_set_highlighter`) Returns the previous setting.
bool ic_enable_highlight(bool enable);

/// Set millisecond delay for reading escape sequences in order to distinguish
/// a lone ESC from the start of a escape sequence. The defaults are 100ms and
/// 10ms, but it may be increased if working with very slow terminals.
void ic_set_tty_esc_delay(long initial_delay_ms, long followup_delay_ms);

/// Enable highlighting of matching braces (and error highlight unmatched
/// braces).`
bool ic_enable_brace_matching(bool enable);

/// Set matching brace pairs.
/// Pass \a NULL for the default `"()[]{}"`.
void ic_set_matching_braces(const char* brace_pairs);

/// Enable automatic brace insertion (enabled by default).
bool ic_enable_brace_insertion(bool enable);

/// Set matching brace pairs for automatic insertion.
/// Pass \a NULL for the default `()[]{}\"\"''`
void ic_set_insertion_braces(const char* brace_pairs);

/// Register or update a fish-style abbreviation.
/// Abbreviations expand when the trigger is followed by a space, newline, or when
/// submitting the line. The trigger must not contain whitespace.
/// Returns `true` on success.
bool ic_add_abbreviation(const char* trigger, const char* expansion);

/// Remove a previously registered abbreviation.
/// Returns `true` if the abbreviation existed and was removed.
bool ic_remove_abbreviation(const char* trigger);

/// Remove all registered abbreviations.
void ic_clear_abbreviations(void);

/// \}

//--------------------------------------------------------------
// Key bindings
//--------------------------------------------------------------
/// \defgroup keybinding Key binding
/// Manage interactive key bindings.
/// \{

/// Bind a key to an action.
/// @param key The key code to bind.
/// @param action The action to perform when the key is pressed.
/// @returns `true` if the binding was successful, `false` otherwise.
bool ic_bind_key(ic_keycode_t key, ic_key_action_t action);

/// Clear a key binding.
/// @param key The key code whose binding should be removed.
/// @returns `true` if the binding was cleared, `false` if no binding existed.
bool ic_clear_key_binding(ic_keycode_t key);

/// Reset all key bindings to their default values.
void ic_reset_key_bindings(void);

/// Get the action bound to a specific key.
/// @param key The key code to query.
/// @param out_action Pointer to receive the action bound to the key.
/// @returns `true` if a binding exists, `false` otherwise.
bool ic_get_key_binding(ic_keycode_t key, ic_key_action_t* out_action);

/// List all current key bindings.
/// @param buffer Buffer to receive the key binding entries.
/// @param capacity Maximum number of entries the buffer can hold.
/// @returns The number of entries written to the buffer.
size_t ic_list_key_bindings(ic_key_binding_entry_t* buffer, size_t capacity);

/// Get a key action from its name.
/// @param name The name of the action.
/// @returns The corresponding key action, or a default value if not found.
ic_key_action_t ic_key_action_from_name(const char* name);

/// Get the name of a key action.
/// @param action The key action.
/// @returns The name of the action, or NULL if invalid.
const char* ic_key_action_name(ic_key_action_t action);

/// Execute a named editor action in the active readline session.
/// This is intended for command-backed widgets that need to delegate to a
/// built-in editing behavior without synthesizing a key press.
/// @param action The editor action to execute.
/// @returns `true` if an editor is active and the action was handled.
bool ic_execute_key_action(ic_key_action_t action);

/// Parse a key specification string into a keycode.
/// @param spec The key specification string (e.g., "ctrl-c", "alt-x").
/// @param out_key Pointer to receive the parsed keycode.
/// @returns `true` if parsing was successful, `false` otherwise.
bool ic_parse_key_spec(const char* spec, ic_keycode_t* out_key);

/// Bind a key to an action using string names.
/// @param key_spec The key specification string (e.g., "ctrl-c").
/// @param action_name The name of the action to bind.
/// @returns `true` if the binding was successful, `false` otherwise.
bool ic_bind_key_named(const char* key_spec, const char* action_name);

/// Format a keycode into a human-readable specification string.
/// @param key The keycode to format.
/// @param buffer Buffer to receive the formatted string.
/// @param buflen Length of the buffer.
/// @returns `true` if formatting was successful, `false` if the buffer was too small.
bool ic_format_key_spec(ic_keycode_t key, char* buffer, size_t buflen);

/// Set the active key binding profile.
/// @param name The name of the profile to activate (e.g., "emacs", "vi").
/// @returns `true` if the profile was set successfully, `false` otherwise.
bool ic_set_key_binding_profile(const char* name);

/// Get the name of the current key binding profile.
/// @returns The name of the active profile.
const char* ic_get_key_binding_profile(void);

/// List all available key binding profiles.
/// @param buffer Buffer to receive the profile information.
/// @param capacity Maximum number of profiles the buffer can hold.
/// @returns The number of profiles written to the buffer.
size_t ic_list_key_binding_profiles(ic_key_binding_profile_info_t* buffer, size_t capacity);

/// Get the default key specification strings for a given action.
/// @param action The key action to query.
/// @returns A string containing the default key specs, or NULL if none exist.
const char* ic_key_binding_profile_default_specs(ic_key_action_t action);

/// Custom command palette entry metadata.
/// These entries are shown alongside built-in editing actions when the command
/// palette opens.
typedef struct ic_command_palette_entry_s {
    /// Stable identifier passed back to the handler callback.
    const char* id;
    /// Display name shown in the command palette list.
    const char* name;
    /// One-line description shown next to the name.
    const char* description;
    /// Extra searchable terms to improve natural-language matching.
    const char* keywords;
} ic_command_palette_entry_t;

/// Callback invoked when a custom command palette entry is selected.
/// Return `true` if the entry was handled successfully.
/// Inside this callback you can use editor APIs such as ic_set_buffer(),
/// ic_set_cursor_pos(), and ic_request_submit() to apply snippets or trigger
/// workflow actions.
typedef bool(ic_command_palette_entry_handler_t)(const ic_command_palette_entry_t* entry,
                                                 void* arg);

/// Replace all registered custom command palette entries.
/// Passing `entries == NULL` and `count == 0` clears all custom entries.
/// Returns `false` on invalid input or allocation failure.
bool ic_set_command_palette_entries(const ic_command_palette_entry_t* entries, size_t count);

/// Clear all registered custom command palette entries.
void ic_clear_command_palette_entries(void);

/// List currently registered custom command palette entries.
/// If `buffer` is NULL or `capacity` is 0, returns the number of entries.
/// Otherwise writes up to `capacity` entries and returns the number written.
size_t ic_list_command_palette_entries(ic_command_palette_entry_t* buffer, size_t capacity);

/// Set the callback that executes custom command palette entries.
/// Pass NULL to disable custom entry execution.
void ic_set_command_palette_entry_handler(ic_command_palette_entry_handler_t* handler, void* arg);

/// \}

//--------------------------------------------------------------
// Advanced Completion
//--------------------------------------------------------------

/// \defgroup completex Advanced Completion
/// \{

/// Get the raw current input (and cursor position if `cursor` != NULL) for the
/// completion. Usually completer functions should look at their `prefix` though
/// as transformers like `ic_complete_word` may modify the prefix (for example,
/// unescape it).
const char* ic_completion_input(ic_completion_env_t* cenv, long* cursor);

/// Get the completion argument passed to `ic_set_completer`.
void* ic_completion_arg(const ic_completion_env_t* cenv);

/// Is this an automatic inline hint? Completers should avoid expensive work
/// such as launching processes when this returns true.
bool ic_completion_is_hint(const ic_completion_env_t* cenv);

/// Do we have already some completions?
bool ic_has_completions(const ic_completion_env_t* cenv);

/// Do we already have enough completions and should we return if possible? (for
/// improved latency)
bool ic_stop_completing(const ic_completion_env_t* cenv);

/// Primitive completion, cannot be used with most transformers (like
/// `ic_complete_word` and `ic_complete_qword`). When completed, `delete_before`
/// _bytes_ are deleted before the cursor position, `delete_after` _bytes_ are
/// deleted after the cursor, and finally `completion` is inserted. The
/// `display` is used to display the completion in the completion menu, and
/// `help` is displayed with hinting. Both `display` and `help` can be NULL.
/// (all are copied by isocline and do not need to be preserved or allocated).
///
/// Returns `true` if the callback should continue trying to find more possible
/// completions. If `false` is returned, the callback should try to return and
/// not add more completions (for improved latency).
bool ic_add_completion_prim(ic_completion_env_t* cenv, const char* completion, const char* display,
                            const char* help, long delete_before, long delete_after);

/// Primitive completion with source information. When completed,
/// `delete_before` _bytes_ are deleted before the cursor position,
/// `delete_after` _bytes_ are deleted after the cursor, and finally
/// `completion` is inserted. The `display` is used to display the completion in
/// the completion menu, `help` is displayed with hinting, and `source`
/// indicates where the completion came from. All of `display`, `help`, and
/// `source` can be NULL. (all are copied by isocline and do not need to be
/// preserved or allocated).
///
/// Returns `true` if the callback should continue trying to find more possible
/// completions. If `false` is returned, the callback should try to return and
/// not add more completions (for improved latency).
bool ic_add_completion_prim_with_source(ic_completion_env_t* cenv, const char* completion,
                                        const char* display, const char* help, const char* source,
                                        long delete_before, long delete_after);

/// \}

//--------------------------------------------------------------
/// \defgroup helper Character Classes.
/// Convenience functions for character classes, highlighting and completion.
/// \{

/// Convenience: return the position of a previous code point in a UTF-8 string
/// `s` from postion `pos`. Returns `-1` if `pos <= 0` or `pos > strlen(s)` (or
/// other errors).
long ic_prev_char(const char* s, long pos);

/// Convenience: return the position of the next code point in a UTF-8 string
/// `s` from postion `pos`. Returns `-1` if `pos < 0` or `pos >= strlen(s)` (or
/// other errors).
long ic_next_char(const char* s, long pos);

/// Convenience: does a string `s` starts with a given `prefix` ?
bool ic_starts_with(const char* s, const char* prefix);

/// Convenience: does a string `s` starts with a given `prefix` ignoring (ascii)
/// case?
bool ic_istarts_with(const char* s, const char* prefix);

/// Convenience: character class for whitespace `[ \t\r\n]`.
bool ic_char_is_white(const char* s, long len);

/// Convenience: character class for non-whitespace `[^ \t\r\n]`.
bool ic_char_is_nonwhite(const char* s, long len);

/// Convenience: character class for separators.
/// (``[ \t\r\n,.;:/\\(){}\[\]]``.)
/// This is used for word boundaries in isocline.
bool ic_char_is_separator(const char* s, long len);

/// Convenience: character class for non-separators.
bool ic_char_is_nonseparator(const char* s, long len);

/// Convenience: character class for letters (`[A-Za-z]` and any unicode >
/// 0x80).
bool ic_char_is_letter(const char* s, long len);

/// Convenience: character class for digits (`[0-9]`).
bool ic_char_is_digit(const char* s, long len);

/// Convenience: character class for hexadecimal digits (`[A-Fa-f0-9]`).
bool ic_char_is_hexdigit(const char* s, long len);

/// Convenience: character class for identifier letters (`[A-Za-z0-9_-]` and any
/// unicode > 0x80).
bool ic_char_is_idletter(const char* s, long len);

/// Convenience: character class for filename letters (_not in_ "
/// \t\r\n`@$><=;|&\{\}\(\)\[\]]").
bool ic_char_is_filename_letter(const char* s, long len);

/// Convenience: If this is a token start, return the length. Otherwise return
/// 0.
long ic_is_token(const char* s, long pos, ic_is_char_class_fun_t* is_token_char);

/// Convenience: Does this match the specified token?
/// Ensures not to match prefixes or suffixes, and returns the length of the
/// match (in bytes). E.g.
/// `ic_match_token("function",0,&ic_char_is_letter,"fun")` returns 0. while
/// `ic_match_token("fun x",0,&ic_char_is_letter,"fun"})` returns 3.
long ic_match_token(const char* s, long pos, ic_is_char_class_fun_t* is_token_char,
                    const char* token);

/// Convenience: Do any of the specified tokens match?
/// Ensures not to match prefixes or suffixes, and returns the length of the
/// match (in bytes). E.g.
/// `ic_match_any_token("function",0,&ic_char_is_letter,{"fun","func",NULL})`
/// returns 0. while `ic_match_any_token("func
/// x",0,&ic_char_is_letter,{"fun","func",NULL})` returns 4.
long ic_match_any_token(const char* s, long pos, ic_is_char_class_fun_t* is_token_char,
                        const char** tokens);

/// \}

//--------------------------------------------------------------
/// \defgroup term Terminal
///
/// Experimental: Low level terminal output.
/// Ensures basic ANSI SGR escape sequences are processed
/// in a portable way (e.g. on Windows)
/// \{

/// Initialize for terminal output.
/// Call this before using the terminal write functions (`ic_term_write`)
/// Does nothing on most platforms but on Windows it sets the console to UTF8
/// output and possible enables virtual terminal processing.
void ic_term_init(void);

/// Call this when done with the terminal functions.
void ic_term_done(void);

/// Flush the terminal output.
/// (happens automatically on newline characters ('\n') as well).
void ic_term_flush(void);

/// Enable or disable OSC 133 semantic marking for prompt, input, and command-output regions.
/// The option is disabled by default and emits nothing for non-interactive terminals.
/// Prompt/input boundaries are emitted automatically by readline. The embedding shell should
/// call ic_mark_command_start() before execution and ic_mark_command_finished() afterward.
/// Returns the previous setting.
bool ic_enable_terminal_region_marking(bool enable);

/// Returns whether OSC 133 semantic terminal-region marking is enabled.
bool ic_terminal_region_marking_is_enabled(void);

/// Mark the end of editable command input and the beginning of command output.
/// This is a no-op when terminal-region marking is disabled or output is non-interactive.
void ic_mark_command_start(void);

/// Mark the end of command output and report its exit status.
/// Exit statuses outside the portable 0-255 range are clamped before emission.
/// This is a no-op when no semantic input/output region is active.
void ic_mark_command_finished(int exit_status);

/// Write a string to the console (and process CSI escape sequences).
void ic_term_write(const char* s);

/// Write a string to the console and end with a newline
/// (and process CSI escape sequences).
void ic_term_writeln(const char* s);

/// Print prompt to the console with the proper styling.
/// @param prompt_text   The prompt text, can be NULL for the default ("").
/// @param continuation_line   If true, uses the continuation prompt marker
/// instead of the primary one.
void ic_print_prompt(const char* prompt_text, bool continuation_line);

/// Write a formatted string to the console.
/// (and process CSI escape sequences)
void ic_term_writef(const char* fmt, ...);

/// Write a formatted string to the console.
void ic_term_vwritef(const char* fmt, va_list args);

/// Set text attributes from a style.
void ic_term_style(const char* style);

/// Set text attribute to bold.
void ic_term_bold(bool enable);

/// Set text attribute to underline.
void ic_term_underline(bool enable);

/// Set text attribute to italic.
void ic_term_italic(bool enable);

/// Set text attribute to reverse video.
void ic_term_reverse(bool enable);

/// Set text attribute to ansi color palette index between 0 and 255 (or 256 for
/// the ANSI "default" color). (auto matched to smaller palette if not
/// supported)
void ic_term_color_ansi(bool foreground, int color);

/// Set text attribute to 24-bit RGB color (between `0x000000` and `0xFFFFFF`).
/// (auto matched to smaller palette if not supported)
void ic_term_color_rgb(bool foreground, uint32_t color);

/// Set underline color using an ANSI palette index between 0 and 255
/// (or 256 for the ANSI "default" color). (auto matched to smaller palette if
/// not supported)
void ic_term_underline_color_ansi(int color);

/// Set underline color using a 24-bit RGB color (between `0x000000` and
/// `0xFFFFFF`). (auto matched to smaller palette if not supported)
void ic_term_underline_color_rgb(uint32_t color);

/// Reset the text attributes.
void ic_term_reset(void);

/// Get the palette used by the terminal:
/// This is usually initialized from the COLORTERM environment variable. The
/// possible values of COLORTERM for each palette are given in parenthesis.
///
/// - 1: monochrome (`monochrome`)
/// - 3: old ANSI terminal with 8 colors, using bold for bright
/// (`8color`/`3bit`)
/// - 4: regular ANSI terminal with 16 colors.     (`16color`/`4bit`)
/// - 8: terminal with ANSI 256 color palette.     (`256color`/`8bit`)
/// - 24: true-color terminal with full RGB colors.
/// (`truecolor`/`24bit`/`direct`)
int ic_term_get_color_bits(void);

/// Mark whether the current line has visible content.
void ic_term_mark_line_visible(bool visible);

/// \}

//--------------------------------------------------------------
/// \defgroup async ASync
/// Async support
/// \{

/// Thread-safe way to asynchronously unblock a readline.
/// Behaves as if the user pressed the `ctrl-C` character
/// (resulting in returning NULL from `ic_readline`).
/// Returns `true` if the event was successfully delivered.
/// (This may not be supported on all platforms, but it is
/// functional on Linux, macOS and Windows).
bool ic_async_stop(void);

/// Thread-safe way to asynchronously unblock a getline operation.
/// Can be used to interrupt getline from another thread when reading from
/// stdin. Returns `true` if the event was successfully delivered.
bool ic_async_interrupt_getline(void);

/// \}

//--------------------------------------------------------------
/// \defgroup alloc Custom Allocation
/// Register allocation functions for custom allocators
/// \{

typedef void*(ic_malloc_fun_t)(size_t size);
typedef void*(ic_realloc_fun_t)(void* p, size_t newsize);
typedef void(ic_free_fun_t)(void* p);

/// Initialize with custom allocation functions.
/// This must be called as the first function in a program!
void ic_init_custom_alloc(ic_malloc_fun_t* custom_malloc, ic_realloc_fun_t* custom_realloc,
                          ic_free_fun_t* custom_free);

/// Free a potentially custom alloc'd pointer (in particular, the result
/// returned from `ic_readline`)
void ic_free(void* p);

/// Allocate using the current memory allocator.
void* ic_malloc(size_t sz);

/// Duplicate a string using the current memory allocator.
const char* ic_strdup(const char* s);

/// \}

#ifdef __cplusplus
}
#endif

#endif  /// IC_ISOCLINE_H
