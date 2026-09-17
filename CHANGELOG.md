# Changelog

Notable changes to cjsh are recorded in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).
Version labels follow the repository's tags; historical releases do not strictly adhere to
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Historical entries are reconstructed from tagged history. Dates use the release publication
date where available and the tag date otherwise, in the tag's local time zone.

## [Unreleased]

## [1.5.8] - 2026-09-16

### Added

- Added opt-in `cjshopt history-directory-parents on|off|status` and matching isocline APIs to include commands from all ancestor directories up to `/` while directory-aware history is enabled. It works independently of `history-directory-subdirs` and excludes sibling branches. `Alt+P` toggles it temporarily inside the history menu.
- Added independent isocline menu-height setters/getters for completion, history, command palette, and custom menus, exposed through `cjshopt completion-menu-max-lines`, `history-menu-max-lines`, `command-palette-max-lines`, and `custom-menu-max-lines` (`<count|status>`). Ctrl+J inside any menu temporarily toggles between its configured limit and all available terminal space; closing the menu resets the toggle.
- Added opt-in `cjshopt completion-auto-menu on|off|status` and matching isocline APIs. Typing shows an unselected, live completion list; Tab activates navigation, mouse interaction, preview, and acceptance without inserting a common prefix or accepting a lone match. With prompt mouse clicking enabled, clicking a passive entry activates and selects it; header/footer clicks select the first entry. The activating click never accepts. After acceptance, refreshed suggestions stay visible in passive mode until Tab or another menu click activates them.
- Added scrollbars to overflowing completion, history, command palette, and custom menus, with click-to-page and thumb-drag scrolling that respect the existing mouse settings.
- Added regression coverage for automatic completion menus, history-based completion ranking, ancestor-directory history, menu sizing and scrollbars, quick history substitution, and isocline performance and allocation-failure handling.

### Changed

- Menu content-row defaults are now 15 for completion, history, command palette, and custom menus. Per-menu `cjshopt` overrides and the temporary Ctrl+J height toggle remain available.
- Completion menus now always use the full single-column list with scrolling, paging, and mouse support.
- Refreshed menu headers, result counts, shortcut footers, and selected-entry styling, with terminal-space calculations that account for wrapped headers and help text.
- Reduced isocline editing overhead with indexed completion deduplication, reusable history snapshots, compact undo/redo storage, geometric string-buffer growth, and faster printable-ASCII width checks.
- Extended the agent waiting-status shimmer across the configured command while preserving literal markup and UTF-8 characters. Color-disabled terminals retain the once-per-second timer.

### Removed

- Removed the shared `ic_set_menu_max_line_count()` / `ic_get_menu_max_line_count()` API and `cjshopt menu-max-lines`. Use the per-menu isocline APIs or `cjshopt` settings instead, and replace the old option in startup files.
- Removed the collapsed completion menu, its expand/collapse controls, and `cjshopt completion-menu-expanded` (including the corresponding isocline API). Remove this setting from existing startup files.

### Fixed

- Quick history substitution now rejects empty search text such as `^^` instead of rerunning the previous command, and preserves text after the closing caret in expressions such as `^old^new^ extra`.
- Successful bare-command history entries now defer to available regular command completions, so commands such as `lazygit` keep their executable description and trailing space as well as their history priority.
- Regular completions now retain priority from matching history when duplicate history suggestions are removed, preserving file and directory suffixes and command, option, subcommand, and value descriptions before applying result limits. Commands used with arguments also boost the bare command name, so `git clean -xdf` in history prioritizes `git`.
- Ranked matching command names shortest first with alphabetical ties before applying completion limits, so commands such as `git` precede `gen_bridge_metadata` for `g` in Tab completion and automatic suggestions.
- Passive automatic completion menus now reuse the prompt's PATH cache and defer manual-page lookups and dynamic completion providers until an explicit Tab request, avoiding expensive searches on each keystroke, especially on WSL.

## [1.5.7] - 2026-09-15

### Added

- Added cursor-aware status-line command hints showing executable paths and available descriptions, builtin and keyword summaries, function sources, and alias or abbreviation expansions. Hints also work after pipes and command separators, disappear in arguments, and respect `status-reporting` settings.
- Added regression coverage for command-hint resolution, cursor movement, literal markup escaping, and agent progress timing, animation, cancellation, and color-disabled behavior.

### Changed

- Replaced the agent waiting indicator with `Running [0s]: <configured command>`, showing the selected executor and elapsed time with a subtle text shimmer. Color-disabled terminals keep only the once-per-second timer, and progress resets for each request and clears on completion or cancellation.
- Styled command-hint sources consistently with completion menus and reused cached or registered descriptions without fetching manual pages or executing prompt input while typing. Cursor-only refreshes avoid repeating syntax validation or user status callbacks.

## [1.5.6] - 2026-09-15

### Added

- Added directory-aware interactive history with `cjshopt history-directory` and `cjshopt history-directory-subdirs`, both off by default, to scope arrow-key recall, fuzzy search, and history completions to the current directory and optionally its descendants. `Alt+D` and `Alt+N` toggle these settings temporarily inside the history menu.
- Added customizable `heredoc-delimiter` highlighting for opening and closing markers, including quoted or escaped delimiters, multiple heredocs, and `<<-` tab stripping. Heredoc bodies now use string styling rather than command highlighting.
- Added regression coverage for directory-scoped history, heredoc highlighting, completion lookup behavior, and glob and extended-glob matching endpoints.

### Changed

- History records now capture the physical working directory before command execution and deduplicate repeated commands per directory. Older records remain available in global history; `history`, `fc`, and history expansion continue to use the full history.
- Improved syntax-highlighting and completion hot paths with bounded per-redraw lookup caches, fewer redundant PATH and filesystem checks, earlier spell-correction filtering, and lighter history metadata parsing.
- Optimized pattern-based parameter trimming and replacement by reusing matching endpoints, and reduced repeated work when matching overlapping or empty extended-glob alternatives.

### Fixed

- Made history-menu mouse-click PTY coverage independent of the parent terminal width, checking both wrapped and single-row history headers.

## [1.5.5] - 2026-09-12

### Added

- Added an `Alt+O` browser shortcut and command-palette action that opens URL input directly or runs a web search for non-URL buffer text using the configurable `BROWSER` launcher.

### Changed

- Moved prompt internals into the core module layout and refreshed related keybinding application paths.
- Improved parser and interpreter hot paths with broader coverage for token dispatch, variable lookup, and redirection-focused regression scenarios.

### Fixed

- Fixed command-substitution evaluation in loop and `select` expressions so nested substitution results are handled consistently.
- Fixed `Alt+O` handling on WSL terminals by recognizing the escape sequence emitted for the key chord.
- Stabilized shell lifecycle and process-cleanup regressions by tightening synchronization in niche race-prone test flows.

## [1.5.4] - 2026-09-10

### Changed

- Updated repository links, badges, Codacy links, and clone commands for the GitHub repository rename to `CadenFinley/cjsh`.
- Standardized the changelog format and documented all tagged releases back to 1.0.0.
- Use each version's changelog entry as its GitHub release notes.

### Fixed

- Fixed GitHub Pages links in the documentation, shell help, and first-run message to use the new `/cjsh/` site URL.
- Restored normal PATH initialization in `--minimal` mode, including system paths for login shells and fallback paths when PATH is missing or empty.

## [1.5.3] - 2026-09-10

### Added

- Added `cjshopt line-wrap-marker <char|''|status>` and matching isocline APIs so wrapped-line markers can be customized with a single printable character or disabled with an empty string.
- Added and stabilized PTY coverage for wrap-marker customization, history navigation behavior, and `vim` profile interactions.

### Changed

- Improved interactive history navigation so `Up` and `Down` at the end of the buffer move through history with recalled entries placing the cursor at the end, while `Shift+Up` and `Shift+Down` navigate history from any cursor position.
- Updated wrapped-line marker behavior and documentation so marker width is reserved only when a marker is configured, including support for wide printable Unicode markers.
- Expanded syntax regression coverage with generated validation probes and focused tests for function/control-flow edge cases.

### Fixed

- Fixed wrapped-line rendering and menu layout interactions to avoid an extra padding column and keep redraw behavior consistent when the marker is customized or hidden.
- Fixed multiline function and control-flow parsing and validation so inline and multiline forms, quoted delimiters, and keyword-like arguments are handled correctly in both interactive checks and `-n` syntax mode.

## [1.5.2] - 2026-09-10

### Added

- Added Windows WSL 2 CI coverage for Ubuntu 24.04 x86_64, running the full CTest suite as an unprivileged user from a checkout in the Linux filesystem.

### Changed

- Extended cached PATH filename lookup to Tab completion, status-line analysis, and command-error suggestions, checking executability only for matching candidates and refreshing lookups on explicit Tab requests.
- Kept explicit command queries such as `type`, `which`, and `command -v` validating executable paths against the filesystem while avoiding unnecessary PATH lookups for shell builtins.
- Expanded regression coverage for completion refreshes, executable filtering, command suggestions, explicit command lookup, and PATH restoration.
- Replaced timing assumptions in shell lifecycle and background-job notification tests with synchronization suitable for slower WSL runners.

### Fixed

- Fixed `command -p` PATH restoration so empty and unset values are preserved after command execution and descriptive queries.
- Preserved known command names from searchable PATH directories that cannot be listed so they remain available as completion candidates.

## [1.5.1] - 2026-09-09

### Changed

- Improved command and syntax-highlighting lookup paths to cache PATH command names during interactive redraws, reducing repeated filesystem scans for incomplete prefixes.
- Updated inline completion hints to stay cache-only while typing: hint generation now avoids launching manual-page scraping and dynamic value providers until explicit Tab completion.
- Ran a broad clang-tidy cleanup pass across core and isocline modules to refresh diagnostics and consistency in shared code paths.
- Expanded regression and benchmark coverage for hint-fetch deferral, dynamic-provider gating, interactive PATH cache lifecycle, and cross-shell timing baselines.

### Fixed

- Fixed interactive PATH cache invalidation for relative PATH segments and working-directory changes so executable discovery updates correctly between prompts.
- Fixed cached-name completion filtering so non-executable files are rejected even when name-only PATH indexing is active.

## [1.5.0] - 2026-09-09

### Added

- Added and expanded regression coverage for startup interruption policy, terminal selection/recovery, process-group launch races, and command-palette/menu cleanup scenarios in PTY drivers.

### Changed

- Replaced the legacy `tests/run_shell_tests.sh` flow with CTest-first test execution across developer docs and CI helper scripts.
- Applied broad performance and cleanup passes across startup, history loading, parser/interpreter paths, completion, and isocline rendering.
- Improved musl CI token-validation stability and aligned release/developer documentation with the CTest workflow.

### Removed

- Removed the internal `--startup-test` shell flag and dropped `cjshopt login-startup-arg`, simplifying startup option handling and related documentation/completions.

### Fixed

- Fixed startup hangs triggered by special files and orphaned process groups during startup policy handling.
- Fixed command-palette action execution so the menu is dismissed and redrawn before the action runs, preventing stale UI artifacts on terminal resumes.
- Fixed spurious process-group launch failures on macOS foreground command paths.

## [1.4.15] - 2026-09-08

### Added

- Added `cjshopt menu-max-lines <count|status>` to limit content rows in completion, history, command palette, and custom menus. The default is 50 rows, including expanded item previews; headers and help text use separate rows. Positive counts above 256 are clamped to 256.
- Exposed menu height configuration through the isocline API with `ic_set_menu_max_line_count()` and `ic_get_menu_max_line_count()`.
- Added regression coverage for menu height defaults and bounds, command validation, quiet startup configuration, terminal fitting, scroll margins, paging, and expanded preview rows across menu types.

### Changed

- Applied the shared `multiline-bottom-lines` scroll margin, defaulting to 3 rows, around selected menu items. Menus respect the configured content limit and shrink to fit the terminal.
- Updated builtin help, completions, and editing documentation for menu height and shared scroll-margin settings.

### Fixed

- Corrected menu paging so the selected item stays within the new page's scroll margins and subsequent rendering preserves the requested page, including Page Up in expanded completions.
- Made menu viewport PTY tests drain redraw output as soon as it arrives, preventing macOS CI timeouts during long navigation sequences.

## [1.4.13] - 2026-09-08

### Added

- Added isolated coverage for path-file ordering, literal entries, deduplication, skipped inputs, fallback paths, and startup bypass modes, plus startup and nested-toolchain regressions.

### Changed

- Replaced `--login-path` with `--no-system-paths`. Native login shells now read `/etc/paths`, then non-hidden files in `/etc/paths.d` in filename order, before merging inherited entries without duplicates. Non-login shells initialize PATH only when it is missing or empty.
- Read system path files directly without invoking `path_helper`. Removed the former Linux-specific PATH/MANPATH additions and preserve inherited MANPATH. Updated invocation help, completions, and startup documentation.

### Fixed

- Restored a working PATH when starting cjsh from a terminal without an inherited PATH, using standard defaults when the system path files supply no entries.
- Preserve a nonempty inherited PATH exactly in non-login shells, including ordering, duplicates, and empty components, so nested shells retain virtual-environment and custom-toolchain precedence.

## [1.4.12] - 2026-09-07

### Added

- Added PTY regression coverage for tall and wrapped multiline completion previews, prompt-prefix layouts, resize handling, and acceptance and cancellation behavior while preview shortening is active.

### Changed

- Reworked completion-menu rendering so each candidate stays on one menu row, previewed multiline replacements stay at the prompt, and oversized previews are shortened with `...` while keeping menu controls visible.
- Updated completion-menu documentation to describe single-row entry rendering and shortened preview behavior.

### Fixed

- Limited startup-benchmark binary discovery to the `cjsh` shell executable so helper test binaries such as `cjsh_test_runner` are excluded.

## [1.4.11] - 2026-09-07

### Added

- Added `suspend [-f]` for interactive shells, restoring external terminal modes while the shell is stopped and waiting for foreground ownership when it resumes. Login shells require `-f`; POSIX shells do not support the command.
- Added native startup controls: `--config-dir DIR` and `CJSH_CONFIG_HOME` select the root for native startup files, while `--no-config` skips automatic native, POSIX, and platform-login startup processing. Added explicit `--login-path` platform setup for native login shells.
- Added safe startup policy for POSIX `ENV`, including expansion without field splitting or command evaluation, and reject execution when real and effective user or group IDs differ.
- Added durable concurrent history storage: writers share a lock, atomically commit complete snapshots, preserve metadata and frequency counts, and leave in-progress editor input private to its own session.
- Added permanent terminal-selection regressions for repeated startup benchmarks, redirected PTYs, read-only stdin, and fully redirected stdio.
- Added regression coverage for startup policy, lifecycle and shutdown ordering, terminal contracts and recovery, concurrent history writers, interactive menu interruption, loop syntax, and CTest result summaries.
- Added CMake presets licensing metadata and release-oriented test configuration.

### Changed

- Updated interactive startup sequencing so handlers are installed before startup files, `-i -c` sources interactive configuration before running its command, and scripts or standard input supplied to an interactive invocation finish without entering the prompt loop.
- Updated exit, signal, and shutdown handling so `cjshexit`, `EXIT` traps, and login logout configuration run once in order for normal exits and untrapped HUP or TERM. Interactive shells now enable `huponexit` by default and resume stopped jobs before sending them SIGHUP.
- Updated history search to retain the live draft separately from committed entries and improved fuzzy matching for history queries.
- Preserved external canonical terminal settings after foreground commands while keeping editor bindings independent; stopped foreground jobs retain their own terminal modes for `fg`.

### Fixed

- Fixed interactive job-control terminal selection to prefer stdin and stdout before `/dev/tty`, preventing PTY-based startup benchmarks from taking the caller's terminal and suspending the benchmark job while retaining support for redirected stdio.
- Restored terminal input that arrives during terminal-query handling, correctly route editor output to the controlling terminal, and preserve signal dispositions across editor use, replacement, restart, and trap changes.
- Fixed `exit` status parsing and confirmation behavior, including Ctrl+D, repeated exits, `--force`, signed status operands, and invalid or extra operands.
- Fixed failed `exec` behavior in noninteractive POSIX shells, preserving shell nesting levels and returning the required 126 or 127 status.
- Fixed logical `cd` normalization, shell invocation identity during startup, unavailable persistence behavior, and completion/history interactions.
- Restored portable Linux CI builds and made startup and terminal-recovery tests resilient to system profile output and busy runners.
- Replaced fixed-delay partial-pipeline assertions with process-state synchronization and child release handshakes, including deliberately delayed startup and exit coverage to prevent intermittent macOS Intel release CI failures.

## [1.4.10] - 2026-09-06

### Added

- Added the `\\p` prompt escape for an abbreviated working directory: parent directories are shortened to their first character while the final directory name remains intact, including correct handling of hidden and UTF-8 directory names.
- Added automatic indentation for incomplete multiline input. Continuation lines inherit their existing indentation and add a level after block-opening delimiters and shell keywords such as `do`, `then`, and `in`.
- Added regression coverage for multiline indentation, custom-menu previews and mouse selection, loop-header syntax, descriptor redirections, terminal recovery, process cleanup, option parsing, prompt startup behavior, and error formatting.
- Added a 1,024-launch regression test for immediate `SIGTERM` delivery to background commands and pipelines under parallel load.

### Changed

- Updated the default primary prompt to use the abbreviated working-directory display. Startup refreshes the formerly built-in `\\W` default while preserving custom inherited prompts and prompts set by startup files.
- Improved custom-menu rendering by expanding the selected item's description into a bounded multiline preview, with mouse selection accounting for the preview's rows.
- Updated release-artifact CI to run the complete CTest suite with bounded parallelism, excluding only build-system configuration tests where appropriate.
- Expanded push CI to match the release workflow's platform and architecture coverage.

### Fixed

- Strengthened `for` and `select` header validation: loops now require a valid variable and either a literal `in` or the end of the header, report malformed headers consistently, and preserve correct empty-list and positional-parameter behavior.
- Corrected redirection backup handling so saved descriptors never occupy descriptors named by the command, preventing invalid descriptor duplications from succeeding. Shell terminal ownership is preserved when commands redirect or close the controlling terminal.
- Fixed foreground-child cleanup and signal handling so non-monitor foreground commands participate in orderly termination without being displayed as jobs; shell shutdown waits for children it terminates.
- Corrected builtin option parsing after `--`, including literal option-like operands for `declare` and `set`, and improved associated diagnostics.
- Prevented early signals, including `SIGTERM`, from being lost during command startup by blocking signals across `fork` until child signal defaults are installed.
- Fixed musl CI and release test permissions by giving the unprivileged test user ownership of the build directory before CTest writes logs and fixtures.

## [1.4.9] - 2026-09-06

### Added

- Added asynchronous job-completion and stop notifications above the interactive prompt while preserving the pending input, cursor position, and undo history. Notifications wait until menus and bracketed paste complete before redrawing.
- Added terminal recovery around prompt and idle hooks, foreground jobs, and monitor-disabled shells so cjsh reclaims the terminal, restores its input settings, and preserves pending input after programs alter terminal state.
- Added regression coverage for asynchronous job notifications, terminal-state recovery, foreground-terminal races, drag detection, mouse-capture resumption, completion context, and parallel CTest scheduling.

### Changed

- Improved smart mouse mode to suspend capture while dragging, including in interactive menus, so the terminal can select text. Capture resumes on a reported release, keyboard input, or supported focus-in events.
- Updated CI and developer guidance to run the complete CTest suite with bounded parallelism, covering shell files alongside focused C, C++, and Python tests.

### Fixed

- Suppressed completions while the cursor is inside a recognized existing command or shell keyword, preventing insertions such as a filesystem candidate into `then` while retaining completion for unfinished words and word endings.
- Corrected job-control synchronization, process-group handling, terminal recapture, and signal delivery across foreground jobs, notifications, and prompt recovery.

## [1.4.8] - 2026-09-05

### Added

- Added `cjsh-widget action <name>` so command-backed widgets and custom key bindings can invoke built-in editor actions directly.
- Added richer hook context: `preexec` functions receive the expanded command as `$1`, and `precmd` functions can read its duration in milliseconds from `CJSH_COMMAND_DURATION_MS` while retaining the command's exit status in `$?`.
- Added regression coverage for hook context, sourced-file returns, editor actions, foreground interactive widgets, login-shell terminal startup, idle timeouts in menus, quiet startup key overrides, and multiline completion metadata.
- Added regression coverage for empty-prompt history ranking, deduplication, configurable completion limits, legacy history metadata, builtin options, and error formatting.

### Changed

- Changed Tab completion at empty or whitespace-only prompts to offer unique history entries ordered by most recent use, then frequency, up to the `cjshopt set-completion-max` limit (default: 1000). No candidates are shown when history is disabled or unavailable.
- Aligned builtin help, completion metadata, and command documentation with supported options, startup modes, and immediately applied key-binding changes.
- Temporarily restore normal terminal mode while external custom-key commands run, then reacquire and redraw the active editor so interactive programs can safely read from the terminal.
- Allowed `return [status]` in sourced files to stop only the sourced file, propagate its requested status, and continue the calling script or function.
- Suppressed expected key-override warnings while startup files configure custom bindings.
- Stabilized interactive agent, idle-hook, shutdown, and signal tests with prompt synchronization, partial PTY-write handling, and reliable process cleanup.

### Fixed

- Corrected `history COUNT` to show the most recent entries and `cjshopt hint-delay status` to report the actual delay, including its 5000-millisecond maximum.
- Fixed `kill -s SIGNAL` and `kill -n SIGNUM` parsing and rejected malformed numeric signal and editor-option values.
- Standardized builtin and job-control diagnostics, rejected unsupported options and extra operands, and preserved operand uses of `-h` in `echo` and `test`.
- Prevented login-shell startup from stopping on `SIGTTIN` or deadlocking while taking ownership of its launcher's foreground terminal.
- Kept idle timeouts active inside completion, history-search, command-palette, and custom menus so they close cleanly before the idle hook runs.
- Collapsed unselected multiline completion-source labels to a single abbreviated line instead of allowing their metadata to occupy multiple menu rows.

## [1.4.7] - 2026-09-04

### Added

- Added `idle` shell hooks and `cjshopt idle-timeout` to run foreground idle actions after a configurable period without terminal input, then restore the pending editor buffer and cursor.
- Expanded job-control interfaces with `set -m` / `set +m`, `jobs -lprs`, `wait -fn -p`, running-job selection and hangup protection for `disown`, and PID support for foreground and job-management commands.
- Added dynamic work scheduling for `generate-completions --subcommands`, allowing discovered subcommands to be generated concurrently while deduplicating targets and reporting their progress.
- Added regression coverage for idle-hook interactions, function-definition syntax, generated completion scheduling, startup semantics, history/menu rendering, and job-control edge cases.

### Changed

- Improved history-search menus with compact metadata, expanded selected-entry metadata, multiline previews, and width-aware truncation.
- Updated `cjsh -i -c` startup behavior to source `.cjshrc` before running the command without entering the interactive loop; logout hooks now run consistently at shell shutdown.
- Improved interactive prompt handling for hooks and idle processing, including reliable restoration of terminal/editor state.
- Moved release helper scripts under `.github/scripts` and allow release validation to skip build-system tests where appropriate.

### Fixed

- Corrected function-definition validation and highlighting for `name() ( ... )` syntax.
- Strengthened process-group, foreground-terminal, stopped-job, pipeline, and signal handling across job-control operations.

## [1.4.6] - 2026-09-04

### Added

- Added completion-context and full-buffer regression coverage for ordinary, append, and indexed assignments; quoted and invalid assignment-like text; existing-word boundaries; and assignment-value completion.

### Fixed

- Prevented command and control-structure completions while the cursor is within an assignment name, avoiding corrupt insertions such as replacing the `i` in `i=$((i+1))` with an `if` block.
- Suppressed insertion completions when the cursor is immediately before an existing shell word, preventing unrelated text from being inserted before tokens such as a loop-closing `done`, while preserving completion within words and assignment values.

## [1.4.5] - 2026-09-04

### Added

- Added automated, tag-driven release builds for macOS Intel, Apple Silicon, and Universal2 plus glibc and static musl Linux builds on x86-64 and ARM64.
- Added release archive validation, SHA-256 checksums, build provenance attestations, generated release notes, and a non-publishing manual dry-run mode.
- Added Ctrl+C cancellation for in-flight agent executors, including process-group cleanup and restoration of the original editor request.

### Changed

- Consolidated release validation around the comprehensive shell suite, which includes the CTest-backed component and interactive tests without running CTest separately.
- Set the Intel and Universal2 macOS deployment target to macOS 12 while retaining native Apple Silicon builds for macOS 15 and newer.

### Fixed

- Made release integration fixtures independent of the working directory, root privileges, and host-provided setuid files.
- Guarded glibc-only memory trimming so static musl builds compile and run correctly.
- Stabilized interactive agent-mode synchronization on slower hosted Intel macOS runners.

## [1.4.4] - 2026-09-03

### Added

- Added a `firstboot` builtin that atomically creates the first-boot marker and suppresses the welcome banner, with help, completion, documentation, and regression coverage.
- Added regression coverage for agent-mode tool-use prompting, history exit-code ordering, filesystem deduplication, and the `firstboot` builtin.
- Added PTY regression coverage for terminal-height caps, resized multiline layouts, truncated history previews, and multiline history editing.

### Changed

- Ranked successful history completions ahead of filesystem matches and failed history entries after them, while omitting history suggestions duplicated by file or directory completions.
- Limited the history candidates considered per completion pass to keep mixed completion results focused.
- Allowed agent-mode command-writing executors to use tools while retaining the required JSON-only response format.
- Updated first-run guidance to use the new `firstboot` builtin instead of exposing the marker-file implementation.

### Fixed

- Capped selected multiline history previews to the available terminal rows, marked truncated previews with an ellipsis, and preserved the complete command when accepted.
- Kept multiline editor and menu rendering within the physical terminal height after accounting for prompt-prefix rows, including after terminal-height changes.
- Placed the cursor at the end of the first line when editing a multiline history-search result.

## [1.4.3] - 2026-09-02

### Changed

- Expanded command-substitution regression coverage to 40 cases spanning escaped operators, nested and backtick substitutions, quoted and unquoted output, `nounset`, brace expansion, pipelines, multiline Bash scripts, and byte-exact installer payloads.
- Verified the Homebrew installer payload remains byte-identical through command substitution and parses successfully when passed to Bash.

### Fixed

- Fixed quoted command-substitution output containing escaped quotes and pipeline syntax being reparsed as outer-shell structure, including installer commands such as `/bin/bash -c "$(curl ...)"`.
- Preserved parameter, arithmetic, command, and brace syntax emitted by command substitutions instead of expanding it a second time in the parent shell.
- Kept unquoted substitution output opaque until the correct field-splitting and pathname-expansion stages, including within pipelines and `case` values.
- Corrected delimiter matching for quoted substitution commands whose content ends with an even run of backslashes.

## [1.4.2] - 2026-09-02

### Added

- Added `--no-agent` to disable agent-assisted command writing for the current startup, including its activation keys, trigger prefixes, and command-palette entry.
- Added `--no-agent` support to persistent login startup arguments, shell help, command completions, and the user documentation.
- Added regression coverage for the startup disable state, option completion metadata, and interactive prompt synchronization.

### Changed

- Kept agent mode disabled while executor definitions load after `--no-agent`; it can still be deliberately restored with `cjshopt agent-mode on`.

## [1.4.1] - 2026-09-02

### Added

- Added PTY regression coverage for completion and history footers in standard-width and short multiline viewports.

### Fixed

- Restored navigation footers across completion menus, including menus where every candidate fits.
- Fixed history-search footers being clipped when headers, mouse status, or footer text wrap across terminal rows.
- Corrected completion and history mouse targeting when wrapped menu headers shift candidate rows.

## [1.4.0] - 2026-09-02

### Added

- Added provider-neutral agent-assisted command writing with configurable executors, trigger prefixes, activation keys, and a review-before-execution suggestion menu.
- Added associative arrays, namerefs, coprocesses, opt-in extended globs, case continuation, brace-range strides, and pattern replacement.
- Added `select` with `PS3`, tracing prompt support through `PS4`, and configurable `PS5` / `PS6` prompts for history search and the command palette.
- Added versioned rich completion specifications with nested subcommands, typed values, aliases, constraints, positional arguments, and dynamic providers.
- Added `cjshopt exit-confirmation`, multiline viewport controls, additional mouse-capture behavior, and interactive progress display for `generate-completions`.
- Added configurable syntax highlighting for completion and history menus, including single, all, and reverse modes.
- Added extensive coverage for agent mode, language compatibility, `select` and prompt variables, rich completions, isocline PTY behavior, job-control races, shutdown, parameter expansion, and POSIX regressions.
- Added a language compatibility inventory and substantially updated command, completion, editing, feature, theme, and non-POSIX documentation.

### Changed

- Improved completion context detection, man-page metadata extraction, file highlighting, multiline candidate display, and completion-menu rendering.
- Changed history search to support sortable fuzzy results, richer metadata, selected-entry previews, and dedicated prompt handling.
- Changed isocline menus to temporarily release mouse capture after off-target clicks and restore it on keyboard or focus-in input.
- Moved typeahead handling into isocline and reorganized completion, menu, fuzzy-search, and terminal internals into focused modules.
- Strengthened `--posix` mode rejection of non-POSIX syntax and builtins while documenting supported language boundaries.
- Hardened temporary-file creation and fatal diagnostics, enabled `_FORTIFY_SOURCE=2` outside Debug builds, and increased release optimization to `-O3`.

### Fixed

- Fixed job-control reporting and foreground-terminal races, including killed jobs and non-interactive execution stealing terminal control.
- Fixed queued-input loss across readline interrupts and several typeahead, line-feeding, and multiline prompt redraw issues.
- Fixed exit behavior, loop and subshell-grouping edge cases, and POSIX parsing, arithmetic, quoting, and expansion regressions.
- Fixed completion menus so the first candidate is selected immediately when multiple results are shown.
- Fixed completion-menu glitches, history metadata and preview handling, OSC 133 prompt markers, redirect highlighting, and incorrect auto-CD or spell suggestions.

## [1.3.3] - 2026-07-08

### Added

- Added `cjshopt completion-click-accept` (`on|off|status`) plus completion/help metadata and isocline plumbing for click-accept behavior.
- Added `cjshopt menu-highlighting` (`none|single|all|reverse|status`) to syntax-highlight completion and history menu items with the existing buffer highlighter.
- Added transient prompt variables `PS1_FINAL` and `RPS1_FINAL` to restyle submitted prompt lines after Enter.
- Added the `file-argument` syntax-highlighting style and broader path-argument highlighting for existing file arguments.
- Added split-unknown command merge completions to recover from accidentally separated command tokens.
- Added richer history-search metadata rendering with relative timestamps, exit-code suffixes, and metadata-key previews.

### Changed

- Changed `cjshopt mouse-clicking` from boolean toggles to `disabled|simple|smart|status` modes with explicit smart suspend/resume behavior.
- Changed completion and history menus to handle multiline candidates and entries with inline previews while preserving scroll/selection behavior.
- Changed history deduplication to prefer entries that carry exit-code metadata when duplicate commands are collapsed.
- Changed docs/reference coverage across commands, editing, features, and themes for the new completion click mode, prompt-final variables, and prompt-layout model.
- Simplified `README.md` by removing duplicated preset/testing/doc-preview sections now covered in dedicated docs.
- Expanded completion/highlighter/isocline regression coverage for click-accept toggles, multiline menus, metadata filtering, and mouse behavior.
- Continued cleanup/refactor passes across filesystem setup, parser helpers, loop handling, exec flow, and completion generation internals.

### Removed

- Removed `cjshopt prompt-cleanup`, `prompt-cleanup-newline`, `prompt-cleanup-empty-line`, and `prompt-cleanup-truncate`; use `PS1_FINAL` and `RPS1_FINAL` for submitted-prompt styling.

### Fixed

- Fixed history staging so interactive commands are recorded using expanded command text, with new regression coverage for `!!` replay behavior.
- Fixed completion-application edge cases where accepted completions could duplicate already-present single-line or multiline suffix text.
- Fixed unknown-command suggestion and highlighting paths to better handle accidentally split command tokens.
- Fixed status-line unknown-command suggestions by merging split-token candidates with standard suggestion output.
- Fixed multiline mouse-targeting and menu interaction edge cases in isocline PTY flows.

## [1.3.2] - 2026-07-07

### Added

- Added `cjshopt completion-spell-enter` (`on|off|status`) plus builtin completion/help metadata for the new toggle.
- Added isocline spell-on-enter plumbing (`ic_enable_spell_correct_on_enter`) and PTY coverage for Enter-time spell correction behavior.
- Added a dedicated function-pipeline job-control regression suite (`tests/core/test_function_pipeline_job_control.py`) and wired it into `tests/run_shell_tests.sh`.

### Changed

- Changed completion context scoping to track the innermost unclosed command substitution when completing inside `$(...)`.
- Refactored arithmetic-command parsing helpers into shared parser utilities and reused them across interpreter, conditional, and loop paths.
- Updated `exec`, `read`, and `generate-completions` internals for stricter option/error handling and cleaner execution flow.
- Synced local isocline behavior with newer upstream-oriented updates across completion, help, string-buffer, and TTY handling paths.
- Expanded shell/isocline/completion regression coverage for spell-enter toggles, kitty control-sequence decoding, mouse-status rendering, command-substitution safety, and function syntax cases.
- Continued cleanup passes across interpreter, exec, and builtin implementations.

### Removed

- Removed the deprecated release workflow file.

### Fixed

- Fixed Enter-submit behavior so spell correction stays opt-in while still allowing single-match auto-correction when explicitly enabled.
- Fixed command-substitution regressions where prior builtin stdout could leak into `$()`/backtick results and break follow-on redirections.
- Fixed negated command output handling so `!` forms respect quiet redirections without runtime noise.
- Fixed subshell-style function parsing/execution edge cases, including inline semicolon forms and variable-scope preservation.
- Fixed interactive job-control handling for function pipelines so foreground control and `tostop` scenarios do not spuriously stop jobs.

## [1.3.1] - 2026-07-04

### Added

- Added a command palette action (`command-palette`) with default `Alt+P` binding and searchable built-in actions.
- Added `cjshopt keybind ext` support for custom command bindings and palette-only snippets with optional titles.
- Added `cjshopt status-line-callback` for per-refresh shell-function status messages via `CJSH_STATUS_INPUT` / `CJSH_STATUS_OUTPUT`.
- Added `cjshopt completion-menu-expanded` to set expanded completion menus as the default behavior.
- Added `cjshopt mouse-clicking` and `cjshopt mouse-clicking-status-line` to control prompt-level mouse defaults and indicator visibility.
- Added new completion and PTY/isocline coverage for status-line controls, mouse toggles, command palette paths, and expanded completion defaults.
- Added automated release workflow support (`.github/workflows/release.yml`).
- Added `cjshopt status-reporting` to control validation output while retaining status-line hints.

### Changed

- Changed status-line composition to combine callback output, spell hints, and validation summaries with safer refresh/state handling.
- Updated completion/history/help UI copy and menu headers to surface mouse-active context and improve interaction hints.
- Refined keybinding and palette plumbing across core/isocline integration for startup-driven customization.
- Updated documentation for new 1.3.1 `cjshopt` toggles, callback behavior, and command-driven keybinding flows.
- Landed cleanup passes in filesystem internals and broad formatting normalization across touched modules.

### Fixed

- Fixed mouse click cursor placement when hints/completions add extra rendered lines in the editor view.
- Fixed mouse menu interaction edge cases around completion rendering/scrolling paths.
- Fixed command-palette entry import bounds/validation in isocline options handling.

## [1.3.0] - 2026-07-02

### Added

- Added `declare` / `typeset` builtin support with attribute flags (including indexed array declarations and function-aware modes).
- Added `approot` builtin for jumping to or printing resolved cjsh paths (`config`, `cache`, `history`, `cjshenv`, profile/rc/logout files, and executable location).
- Added `restart` builtin for in-place shell re-exec with optional startup-flag reset via `--no-flags`.
- Added expanded keybinding behavior in isocline (including additional Alt bindings and stronger `Ctrl+A`/`Ctrl+E` handling).
- Added mouse-aware line-editor/menu interactions with runtime-togglable behavior and menu selection support.
- Added broader line-reflow, keybinding, and PTY integration coverage for isocline behavior.
- Added stronger startup and regression coverage across shell/interactive paths.

### Changed

- Renamed/restructured source layout toward `cjsh-core` and updated build wiring around the new module naming.
- Refactored execution internals into a more explicit exec subsystem and tightened call paths across main loop, parser, and interpreter layers.
- Updated version/pre-release metadata flow in CMake/build metadata generation.
- Improved bash-compat behavior across core builtins and refreshed long-option/help output handling.
- Improved `ulimit` behavior and completion metadata paths.
- Reworked history frequency ranking implementation and follow-up tuning.
- Improved shell test reporting and refreshed deprecated workflow usage in CI.
- Landed a broad optimization/cleanup pass touching parser, expansions, interpreter loops, and startup-related hot paths.

### Fixed

- Fixed hint-buffer clearing when returning to an empty interactive buffer.
- Fixed history-expansion regressions and associated parser integration edge cases (including issue #28 follow-up).
- Fixed completion/menu rendering issues, including scrolling behavior and small-menu mouse-toggle edge cases.
- Fixed POSIX control-flow/case handling regressions and additional parser validation corner cases.
- Fixed Linux linking issues around history-expansion tests and additional CI portability failures.
- Fixed build-setting regression tracked in issue #29.

## [1.2.0] - 2026-04-04

### Added

- Added `CJSH_HISTORY_FILE` support for explicit history-file routing.
- Added `--no-history` startup mode.
- Added command-not-found handling path.
- Added indexed array support.
- Added C-style arithmetic loop support.
- Added support to clear custom job names.
- Added mouse-wheel support in history search and expanded completion menus.
- Added dedicated build-system tests and updated CI wiring around the new layout.

### Changed

- Reorganized source layout into dedicated components (including split `cjsh` and `cjsh-isocline` trees) and updated CMake/build plumbing.
- Stopped sourcing `.profile` by default and introduced updated environment-startup handling (`cjshenv` flow).
- Consolidated builtin/job-control internals and reduced global-context usage through common utility extraction.
- Centralized `setenv` operations through the variable manager.
- Improved auto-`cd` handling for `-` with matching syntax-highlighting/status-line behavior.
- Improved prompt/menu collapse handling and terminal-health checks.
- Expanded build-system, PTY/isocline, array, arithmetic, hash, quoting, and regression test suites.

### Fixed

- Improved loop/signal handling (including stronger SIGSTP coverage).
- Improved job-selection defaults when only one background/stopped job exists.
- Improved error consistency by routing more `perror`/error paths through `error_out`.
- Improved completion generation and duplicate-suppression behavior.
- Improved key-sequence and visible-character handling in typeahead/editline paths.

## [1.1.6] - 2026-02-14

### Added

- Added script-dispatch support for cjsh scripting workflows.
- Added `--no-exec` handling (including pipeline behavior).
- Added automatic backgrounding support (`autobg`) with tests/docs (merged via PR #26).
- Added flags for disabling error suggestions and controlling prompt behavior in minimal modes.
- Added more completion coverage for variables and builtin-specific suggestions (`type`/`which`).

### Changed

- Updated Linux build behavior to support dynamic build flows.
- Adjusted default prompt/right-prompt behavior for safer minimal and non-set scenarios.
- Updated login/startup argument parsing to include additional flag handling.
- Continued POSIX-mode groundwork (skeleton + blocker tracking updates).
- Expanded syntax-highlighting and completion test coverage significantly.
- Stopped building tests by default in local builds while ensuring tests run in CI.
- Improved error-header consistency and removed duplicated version/header fragments.

### Fixed

- Fixed jobs mutex/race issues and improved invalid-argument handling for job commands.
- Fixed abbreviation expansion from command-front positions.
- Fixed bracket/case validation edge cases and several parser/highlighter regressions.
- Fixed TTY/typeahead synchronization issues, including line-editing/newline edge behavior.
- Fixed export error handling and improved source-command error output.

## [1.1.5] - 2026-02-06

### Added

- Added continuation callbacks through isocline integration.
- Added case-sensitivity toggles for history search.
- Added `set globstar`, `readonly -f`, `read -t`, and `set -o` feature work.
- Added explicit fatal-error path handling and structured error logging.

### Changed

- Moved status-line handling into a dedicated module and surfaced unknown-command errors there.
- Switched completion caching to an LRU-based strategy and improved suggestion filtering quality.
- Prioritized executable files for `./` completion scenarios.
- Introduced full async prompt behavior with non-blocking TTY reads and session gating for prompt refresh.
- Centralized command validation for both syntax-highlighting and status-line evaluation.
- Refactored expansion/conditional evaluation paths and consolidated parameter/variable management.
- Expanded shell/isocline coverage across redirection, signal, readonly, and process-control paths.
- Per-file license/header cleanup and documentation refresh shipped alongside refactor work.

### Removed

- Removed direct `getenv` usage in favor of variable-manager pathways.
- Removed timeout-based test workarounds and tightened CI behavior.

### Fixed

- Fixed duplicate status-line redraw artifacts during terminal resize/reflow.
- Fixed execution-error routing so failures consistently pass through `error_out`.
- Fixed job-control exit/cleanup races (including immediate job-table updates after signals).
- Fixed background builtin behavior and force-exit semantics around logout/exit traps.
- Fixed Linux/macOS portability regressions introduced during strict-compile cleanup.

## [1.1.4] - 2026-01-30

### Added

- Added fuller status-line control surface through `cjshopt`-driven configuration updates.
- Reintroduced terminal window-title handling.

### Changed

- Changed `hash -r` with no arguments to perform a full reset.
- Updated help/startup messaging for clearer boot-time guidance.
- Updated syntax-highlighting color choices and prompt-created-line context defaults.
- Continued main-namespace cleanup and directory/name organization updates.

### Removed

- Removed deprecated `--no-smart-cd` support.
- Removed prompt-item configuration from startup flags and shifted configuration responsibility.
- Removed noisy shutdown output and cleaned older help text variants.

### Fixed

- Prevented unnecessary hash rebuilds on hash command invocation.
- Removed arbitrary completion sorting in isocline to preserve stronger ordering guarantees.

## [1.1.3] - 2026-01-28

### Added

- Added job resolution by `+` and `-` selectors in job-control workflows.
- Added safer `cwd`/`pwd` handling paths.
- Added clearer completion source tagging.

### Changed

- Reorganized first-boot/startup sequence and timing paths.
- Reimplemented command hashing and tied completion lookups more directly to path hash behavior.
- Refactored validator modules with broad multiline-validation updates.
- Disabled completion learning behavior to keep completion output more deterministic.
- Expanded regression coverage for control-flow and filesystem edge cases.
- Included test harness cleanup after a revert/reapply cycle during this release window.

### Fixed

- Fixed right-aligned prompt rendering, including follow-cursor behavior.
- Fixed status-line cleanup before returning terminal control to child processes/callers.
- Fixed completion-menu collapse behavior on control-key interaction (`Ctrl+J`).

## [1.1.2] - 2026-01-27

### Added

- Added full secondary prompt (`PS2`) support, including a setting to disable PS2 line-number overrides.
- Added completions for all `cjshopt` options and extra job metadata in completion output.
- Added job-control quality-of-life features: `fg`, `bg`, and `kill` by command name, plus `kill` by job name.
- Added PID reporting when jobs are started in the background.
- Added invoked-as-`sh` warning with explicit suppression support.
- Added script line-continuation support.

### Changed

- Reworked completion behavior: configurable completion count, adjusted ordering/highlighting, and removed arbitrary completion/history limits.
- Split job-control builtins into dedicated files and moved shell hooks to a separate module.
- Improved help/status-line presentation with optional underline hints and cleaner keybinding hint display.
- Reworked startup/main-loop organization to reduce duplication and global-state coupling.
- Added/updated behavior tests for isocline integration, `read`, control structures, and validation edge cases.
- Vendored isocline-related code was normalized with explicit license coverage and follow-up cleanups.

### Fixed

- Fixed completion memory leaks and crash cases (including long file completion entries).
- Fixed terminal-resize handling and prompt/menu interaction bugs.
- Fixed control-structure and loop redirection validation edge cases.
- Fixed fake error emission in inlined subshell loop callbacks.
- Fixed background signal flow after `waitpid`, plus safer handling for quickly exiting jobs.
- Fixed alias behavior through pipelines and several non-interactive invocation edge cases.

## [1.1.1] - 2025-12-06

### Added

- Added parenthesized function definitions and support for functions in pipelines.
- Added hangup options and Shift+Arrow access to the expanded completion menu.

### Changed

- Improved typeahead interpretation and distinguished logical input lines from physical terminal rows in the editor.
- Simplified builtin error messages and error formatting.

### Fixed

- Restored exit confirmation when stopped jobs exist and corrected completed-job tracking in interactive and noninteractive shells.
- Improved `SIGTTOU` handling and corrected function-pipeline parsing and editor display behavior.

## [1.1.0] - 2025-11-13

### Added

- Added `PS1` and right-aligned prompt support, including empty prompts and integration scripts for Starship and zoxide.
- Added history exit-code metadata and filtering by exit status.
- Added live validation messages below the input buffer and automatic matching-quote insertion.

### Changed

- Reworked completion menus, disabled inline hints while the menu is open, and prioritized scripts and executables in `./` completions.
- Centralized error reporting and improved heredoc validation, terminal color detection, and prompt cleanup.
- Enabled link-time optimization in non-Debug builds and improved PowerPC and 32-bit build compatibility.

### Removed

- Removed the earlier `--posix` flag, which did not provide full POSIX compliance.
- Removed the deprecated bookmark database and its smart-directory navigation implementation.
- Removed the syntax and validation builtins in favor of the shared error reporter.

### Fixed

- Fixed foreground-job restoration and loss of terminal control after stdin, stdout, or stderr redirection.
- Fixed redirected commands incorrectly running in the background and `read` behavior in pipelines.
- Prevented auto-CD from taking precedence over commands and corrected directory completion fallbacks.
- Fixed empty prompts when colors are disabled, variable handling, and builtin output flushing in command substitutions.

## [1.0.11] - 2025-10-30

### Added

- Added man-page-based completion generation, command summaries, and the `generate-commands` builtin.
- Added visible-whitespace controls, configurable initial multiline height, scrollable history search, and grid-menu arrow navigation.
- Added a `--posix` mode with `sh` invocation detection, pipeline negation, and a `ulimit` builtin.
- Added alternate startup-file locations and matching file-generation options.

### Changed

- Replaced the nob build system with CMake and moved continuous integration to GitHub Actions.
- Improved builtin compatibility for `echo`, `printf`, `pwd`, `test`, and `umask`, and routed error messages to stderr.
- Reworked completion and history menus, added alias and abbreviation candidates, and completed common prefixes on the first Tab press.
- Improved parsing and arithmetic performance and reduced redraw work during bracketed paste.

### Removed

- Removed the custom `ls` implementation.

### Fixed

- Fixed history expansion with history synchronized from other sessions.
- Fixed recursion stack overflows, nested-loop execution, arithmetic and quoting regressions, and command-substitution output handling.
- Improved signal interruption of loops and `SIGPIPE` handling.
- Fixed completion-menu input buffering, prompt preservation during history search, and PowerPC and older macOS build failures.

## [1.0.10] - 2025-10-15

### Added

- Added Bash-style history expansion, fuzzy history search, prefix navigation with previews, and history synchronization between sessions.
- Added fish-style abbreviations and an initial command-widget interface.
- Added the `command` builtin, optional automatic directory changes, and right-aligned Starship prompts.
- Added variable string manipulation, octal arithmetic, and custom `IFS` splitting in `read`.

### Changed

- Disabled automatic Tab completion by default and improved spelling correction and directory previews.
- Expanded editor key bindings and prompt/input callbacks, and centralized variable management.

### Fixed

- Fixed inline and multiline `elif` parsing and condition evaluation in recursive functions.
- Fixed local-variable shadowing, `getopts`, builtin redirection flushing, and piping into loops.
- Corrected subshell grouping, arithmetic expansion, and multiline input handling.

## [1.0.9] - 2025-10-08

### Added

- Added configurable line numbers, relative numbering, and current-line highlighting.
- Added the `fc` history-editing builtin and expanded `cjshopt` access to editor settings.

### Changed

- Split parser and interpreter responsibilities into focused modules for variables, functions, loops, conditions, and expansions.
- Improved multiline completion display, newline highlighting, and heredoc history storage.

### Fixed

- Fixed multiline expressions read from stdin and interactive heredoc processing.
- Corrected line-number placement with indentation and preserved highlighting on submitted input.
- Fixed alphabetic brace expansion and handling of invalid ranges.

## [1.0.8] - 2025-10-05

### Added

- Added a Vim key-binding profile, transient prompt styling, and a `--no-prompt` startup flag.
- Added basic shell hooks, history suggestions when other completions are unavailable, and completion spelling correction.
- Added a directory blacklist for automatic bookmarks.

### Changed

- Changed theme loading to use sourced files and moved configuration away from the previous `.config` layout.
- Improved `test` and `set` behavior and expanded editing and configuration documentation.

### Removed

- Removed the legacy plugin system, built-in AI features, and the theme-management command.

### Fixed

- Fixed a completion memory leak, verbose `set -v` behavior, and escaping of `$#`.
- Corrected directory-blacklist handling and spelling correction for paths in `cd` commands.
- Improved Ctrl+C/Ctrl+D handling and terminal output flushing.

## [1.0.7] - 2025-10-04

### Added

- Added custom editor key bindings that can be changed after startup.
- Added configurable history and bookmark limits and a minimal build option.

### Changed

- Reduced startup memory reservations, bounded prompt caches, and improved login startup and typeahead handling.
- Expanded builtin help and added a Debug build option.

### Security

- Restricted files written through the shell's file-content helper to owner read/write permissions, including existing files.
- Hardened filesystem, command-execution, and Git prompt helper paths.

## [1.0.6] - 2025-10-02

### Added

- Added `cjshopt` configuration, case-sensitive completion controls, and the `builtin` command.
- Added recursive functions, pipelines within functions, POSIX heredocs, and builtin file redirections and process substitutions.
- Added smart history search, automatic executable-list refreshes, and pruning of broken directory bookmarks.
- Added logout-file handling, startup-file generators, a first-run welcome message, and build details in `version` output.

### Changed

- Replaced JSON themes with sourceable `.cjsh` theme definitions supporting variables and reusable segments.
- Improved prompt timing precision, startup configuration, terminal-health checks, and typeahead sanitization.
- Added build dependency tracking and improved build progress reporting.

### Removed

- Removed the JSON and utf8proc dependencies and the old installation script.

### Fixed

- Corrected command-not-found, non-executable, and signal exit statuses and error propagation.
- Fixed function scoping around builtins that change directories and prevented unexported variables from leaking into child environments.
- Corrected WSL permission handling, prompt alignment, and shutdown behavior.

## [1.0.5] - 2025-09-27

### Added

- Added typeahead buffering so input entered during command execution can be retained for the next prompt.
- Added the `which` builtin and explicit control over the custom `ls` implementation.
- Added theme gradients, conditional prompt segments, right-aligned inline segments, and theme hot reloads.

### Changed

- Replaced CMake with the nob build system.
- Deferred plugin, theme, and AI initialization until needed and cached language-version information for prompts.
- Reorganized parser and interpreter handling for quoting, expansions, control flow, and runtime errors.

### Fixed

- Fixed expansions in inline `case` statements and quoting of command-substitution results.
- Corrected literal-marker cleanup in output and diagnostics and improved custom `ls` portability.

## [1.0.4] - 2025-09-24

### Added

- Added completion source labels and priorities, highlighting for function definitions and sourced functions, and startup timing display.
- Added minimal-mode and custom-`ls` disable flags and a control for limiting smart directory changes.

### Changed

- Loaded plugins on demand and improved directory bookmarks, including paths below bookmarked directories.
- Improved syntax diagnostics and standardized builtin errors and completion filtering.

### Fixed

- Fixed duplicate completions, repeated AI messages, and memory cleanup.
- Corrected handling of spaces in directory names, bookmark collisions, and tilde expansion in pipelines.
- Restored Ctrl+J handling and fixed signal behavior that could leave looping processes unresponsive.

## [1.0.3] - 2025-09-19

### Added

- Added multiline and nested loops, `[[ ... ]]` expressions, the `local` builtin, and expanded function-definition support.
- Added literal brace expansion, more arithmetic and parameter expansions, here-string variable expansion, and `noclobber` with explicit overwrite support.
- Added syntax linting with detailed errors, command-failure suggestions, execution timing in prompts, and syntax-highlighting/completion disable flags.
- Added local directory bookmarks, the `..` navigation builtin, and installation/uninstallation helpers.

### Changed

- Improved syntax highlighting, command-specific completions, history completions, and multiline prompt rendering.
- Centralized filesystem operations and improved parser performance and runtime diagnostics.

### Fixed

- Fixed Ctrl+C terminating the main shell loop and improved foreground-job signal handling.
- Corrected redirections, alias expansion in pipelines, quoting, arithmetic precedence, and `case` execution.
- Fixed scripts ending prematurely after loops and prevented a child process exiting with status 127 from being misreported as command-not-found.

## [1.0.2] - 2025-09-11

### Added

- Added numeric and alphabetic brace ranges, syntax validation, and conditional startup flags for login shells.
- Added Unicode display-width calculations using utf8proc.

### Changed

- Reworked `trap` toward POSIX behavior and improved memory use, shutdown cleanup, and script-comment handling.
- Improved installation safety and build portability.

### Removed

- Removed the `restart` command, the startup tutorial, remote theme/plugin features, and the curl dependency.
- Removed job-priority controls.

### Fixed

- Corrected startup profile flag handling and an AI-help error.

## [1.0.1] - 2025-09-09

### Added

- Added directory sizes to the custom `ls` output.

### Changed

- Improved syntax highlighting and completion responsiveness.
- Improved POSIX compatibility of the custom `ls` command.

## [1.0.0] - 2025-09-08

### Added

- First release in the current tag history, with an interactive login shell, scripting support, syntax highlighting, and programmable completions.
- Included JSON prompt themes, a shared-library plugin engine, and an optional built-in AI assistant.
- Included CMake builds, installation helpers, and shell compatibility tests.

[Unreleased]: https://github.com/CadenFinley/cjsh/compare/v1.5.8...HEAD
[1.5.8]: https://github.com/CadenFinley/cjsh/compare/v1.5.7...v1.5.8
[1.5.7]: https://github.com/CadenFinley/cjsh/compare/v1.5.6...v1.5.7
[1.5.6]: https://github.com/CadenFinley/cjsh/compare/v1.5.5...v1.5.6
[1.5.5]: https://github.com/CadenFinley/cjsh/compare/v1.5.4...v1.5.5
[1.5.4]: https://github.com/CadenFinley/cjsh/compare/v1.5.3...v1.5.4
[1.5.3]: https://github.com/CadenFinley/cjsh/compare/v1.5.2...v1.5.3
[1.5.2]: https://github.com/CadenFinley/cjsh/compare/v1.5.1...v1.5.2
[1.5.1]: https://github.com/CadenFinley/cjsh/compare/v1.5.0...v1.5.1
[1.5.0]: https://github.com/CadenFinley/cjsh/compare/v1.4.15...v1.5.0
[1.4.15]: https://github.com/CadenFinley/cjsh/compare/v1.4.13...v1.4.15
[1.4.13]: https://github.com/CadenFinley/cjsh/compare/v1.4.12...v1.4.13
[1.4.12]: https://github.com/CadenFinley/cjsh/compare/v1.4.11...v1.4.12
[1.4.11]: https://github.com/CadenFinley/cjsh/compare/v1.4.10...v1.4.11
[1.4.10]: https://github.com/CadenFinley/cjsh/compare/v1.4.9...v1.4.10
[1.4.9]: https://github.com/CadenFinley/cjsh/compare/v1.4.8...v1.4.9
[1.4.8]: https://github.com/CadenFinley/cjsh/compare/v1.4.7...v1.4.8
[1.4.7]: https://github.com/CadenFinley/cjsh/compare/v1.4.6...v1.4.7
[1.4.6]: https://github.com/CadenFinley/cjsh/compare/v1.4.5...v1.4.6
[1.4.5]: https://github.com/CadenFinley/cjsh/compare/v1.4.4...v1.4.5
[1.4.4]: https://github.com/CadenFinley/cjsh/compare/v1.4.3...v1.4.4
[1.4.3]: https://github.com/CadenFinley/cjsh/compare/v1.4.2...v1.4.3
[1.4.2]: https://github.com/CadenFinley/cjsh/compare/v1.4.1...v1.4.2
[1.4.1]: https://github.com/CadenFinley/cjsh/compare/v1.4.0...v1.4.1
[1.4.0]: https://github.com/CadenFinley/cjsh/compare/v1.3.3...v1.4.0
[1.3.3]: https://github.com/CadenFinley/cjsh/compare/v1.3.2...v1.3.3
[1.3.2]: https://github.com/CadenFinley/cjsh/compare/v1.3.1...v1.3.2
[1.3.1]: https://github.com/CadenFinley/cjsh/compare/v1.3.0...v1.3.1
[1.3.0]: https://github.com/CadenFinley/cjsh/compare/v1.2.0...v1.3.0
[1.2.0]: https://github.com/CadenFinley/cjsh/compare/v1.1.6...v1.2.0
[1.1.6]: https://github.com/CadenFinley/cjsh/compare/v1.1.5...v1.1.6
[1.1.5]: https://github.com/CadenFinley/cjsh/compare/v1.1.4...v1.1.5
[1.1.4]: https://github.com/CadenFinley/cjsh/compare/v1.1.3...v1.1.4
[1.1.3]: https://github.com/CadenFinley/cjsh/compare/v1.1.2...v1.1.3
[1.1.2]: https://github.com/CadenFinley/cjsh/compare/v1.1.1...v1.1.2
[1.1.1]: https://github.com/CadenFinley/cjsh/compare/v1.1.0...v1.1.1
[1.1.0]: https://github.com/CadenFinley/cjsh/compare/v1.0.11...v1.1.0
[1.0.11]: https://github.com/CadenFinley/cjsh/compare/v1.0.10...v1.0.11
[1.0.10]: https://github.com/CadenFinley/cjsh/compare/v1.0.9...v1.0.10
[1.0.9]: https://github.com/CadenFinley/cjsh/compare/v1.0.8...v1.0.9
[1.0.8]: https://github.com/CadenFinley/cjsh/compare/v1.0.7...v1.0.8
[1.0.7]: https://github.com/CadenFinley/cjsh/compare/v1.0.6...v1.0.7
[1.0.6]: https://github.com/CadenFinley/cjsh/compare/v1.0.5...v1.0.6
[1.0.5]: https://github.com/CadenFinley/cjsh/compare/v1.0.4...v1.0.5
[1.0.4]: https://github.com/CadenFinley/cjsh/compare/v1.0.3...v1.0.4
[1.0.3]: https://github.com/CadenFinley/cjsh/compare/v1.0.2...v1.0.3
[1.0.2]: https://github.com/CadenFinley/cjsh/compare/v1.0.1...v1.0.2
[1.0.1]: https://github.com/CadenFinley/cjsh/compare/v1.0.0...v1.0.1
[1.0.0]: https://github.com/CadenFinley/cjsh/releases/tag/v1.0.0
