<!--
  completions.md

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
-->

# Completion Authoring Guide

CJ's Shell ships with a hybrid completion engine: built-ins are documented in-code, while external
commands learn their options and subcommands automatically by reading their manual pages. This guide
explains how that pipeline works and how to author or override completion data when you need to fill
in gaps or add custom behaviour.

At an empty or whitespace-only prompt, `Tab` shows unique history entries up to the
`cjshopt set-completion-max` limit (default: 1000), ordered by most recent use and then frequency
for matching timestamps. Empty-prompt completion shows no candidates when history is disabled or
no eligible history entries exist.

Regular completions that match eligible history entries are offered ahead of unused matches.
When history duplicates a file, directory, command, option, subcommand, or value completion,
the regular result keeps its description and insertion behavior while retaining the history
preference. This also applies to inline hints and automatic menus, before result limits.
History-directory settings and disabled history are respected.
Using a command with arguments also prioritizes its command name: a history entry such as
`git clean -xdf` boosts `git` even if `git` has never been run on its own.
For a bare command such as `lazygit`, the regular command completion takes precedence over
its history duplicate, including after a successful run. It keeps the command description
and trailing space while retaining its history preference.

Within the same history preference, matching command names are ordered shortest first, then
alphabetically using the completion case-sensitivity setting. For example, without matching
history, `g` offers `git` before `gen_bridge_metadata`.
Builtins, keywords, functions, aliases, abbreviations, and PATH commands share this ranking,
which is applied before result limits in Tab completion, inline hints, and automatic menus.

When the cursor is inside an existing recognized command or shell keyword, cjsh offers no
completions for that word. For example, moving just after the `t` in `then` does not suggest
`tests/`. Completion remains available for unfinished words and at the end of a word.

Inline hints and passive automatic menus use cached documentation and static value choices. Press
`Tab` to fetch missing manual-page data or invoke dynamic value providers. Automatic suggestions,
Tab completion, status-line analysis, and command-error suggestions share cached PATH filenames
and check executability only for matching candidates. The next prompt, an explicit Tab request,
or a `PATH` change refreshes these lookups.
Command execution and explicit queries such as `type`, `which`, and `command -v` still validate
executable paths against the filesystem.

## How automatic completions are generated

- **On-demand scraping:** The first time you press `Tab` for an external command that
  resolves in `PATH`, cjsh invokes `man -P cat <command>` (falling back to `man <command>`) and
  scrapes the result. The parser looks for `OPTIONS`, `COMMANDS`, or `SUBCOMMANDS` sections, pulls
  out option switches and subcommand names, preserves option aliases and value metavariables, and
  condenses their descriptions to a single line.
- **Caching:** Parsed data is written to `~/.cache/cjsh/generated_completions/<command>.txt`. The
  next completion request loads this cache instead of invoking `man` again. Cache entries also store
  a short summary that feeds inline help and completion source hints.
- **Nested commands:** When a completion entry includes subcommands, cjsh will look for additional
  caches named `<command>-<subcommand>.txt` and merge their contents. This is how `git-remote`
  completions are chained into `git`.
- **Bulk generation:** The `generate-completions` builtin pre-populates caches for an entire `PATH`
  or a list of commands. By default it forces regeneration (`--force`); pass `--no-force` to keep
  existing manual edits. Use `--subcommands` to also pre-generate discovered nested command caches,
  `-j/--jobs` to parallelise scraping, and `--quiet` to suppress per-command status output.

### Controlling automatic learning

Scraping man pages on the fly is convenient, but it can consume CPU, spawn short-lived `man`
processes, and keep memory allocations around until the session ends. When you prefer a predictable
footprint, turn learning off and rely exclusively on whatever is already cached (or on
`generate-completions`).

- Run `cjshopt completion-learning off` during a session to stop future man-page lookups. The toggle
  persists across restarts if you add `cjshopt completion-learning off` to `~/.cjshrc`.
- Launch cjsh with `--no-completion-learning` to start with learning disabled from the first prompt.
- The `generate-completions` builtin continues to work either way, so you can keep caches warm with a
  one-time run and leave learning off during normal interactive use.
- When `CJSH_MAN_PATH` is set, cjsh always uses that `man` binary (even outside secure mode).
- In secure mode (`--secure`), cjsh only uses `CJSH_MAN_PATH`. If it is unset or invalid, scraping
  is skipped and cjsh relies on cached data.

If a man page cannot be read (missing `man`, atypical formatting, or sandbox restrictions), cjsh
creates an empty cache entry. You can delete that file or replace it with a manual definition.

## Cache layout and file format

Each cache file is a versioned, tab-separated completion specification:

```
generated by cjsh from man page for git
format: 2
summary: the stupid content tracker
path: /usr/bin/git
E	O		--output	Write output to a file	-o	required	file	FILE		--stdout	--format	0	0	project-files	0	0	either
E	S		remote	Manage remote repositories		none	none					0	0		0	0	space
E	P	remote	BRANCH	Branch to operate on		required	branch	BRANCH				0	0	git-branches	1	0	space
```

Key points:

- The header line must stay exactly `generated by cjsh from man page for <command>` or cjsh will
  ignore the file.
- `format: 2` selects the rich format. Version 1 `O` and `S` records remain readable for backward
  compatibility; newly generated files use version 2.
- `summary:` and `path:` supply display metadata. Either value may be empty.
- Rich entries begin with `E`. Their tab-separated fields are, in order: record marker, kind (`O`
  option, `S` subcommand, or `P` positional), parent subcommand scope, canonical text, description,
  aliases, value requirement, value type, value name, enum choices, conflicts, dependencies,
  repeatable, deprecated, dynamic provider, positional index, variadic, and value separator.
- Lists use commas. Literal percent signs, commas, tabs, and newlines inside fields are encoded as
  `%25`, `%2C`, `%09`, and `%0A` respectively.
- A subcommand's nested entries name it in the scope field. Multiple scope components describe a
  deeper tree, for example `remote,add`.
- Value requirements are `none`, `required`, or `optional`. Separators are `space`, `equals`, or
  `either`.
- Value types are `none`, `text`, `file`, `directory`, `enum`, `command`, `branch`, `process`, or
  `custom`. Enum candidates come from the choices field. Other dynamic values use the named
  provider hook, or the provider convention matching their value type.
- Conflicts hide an entry when any named conflicting entry was already used. Dependencies hide an
  entry until every named dependency is present. Non-repeatable entries disappear after use.
- Deprecated entries remain available but are labeled as deprecated in the completion menu.
- Values are case-insensitive in matching but should be written the way you want them to appear to
  users.
- File names are normalised to lower-case with non-alphanumeric characters translated to `_`. Let
  `generate-completions` create the skeleton once if you are unsure about the exact spelling.

Dynamic providers are registered in-process with `register_dynamic_completion_provider()` from
`completion_spec.h`. The request includes the command path, arguments, cursor argument, current
value, working directory, and declared value metadata. Providers return value/description pairs.
Complete in-memory specifications can similarly be installed with `register_command_doc()`. These
are the extension points intended for future Bash and Zsh compatibility workers.

## Authoring or overriding completions manually

1. **Create the cache directory (once):** `generate-completions` or the first automatic scrape will
   do this, but you can also `mkdir -p ~/.cache/cjsh/generated_completions` yourself.
2. **Seed a template (optional):** Run `generate-completions --no-force <command>` to create the
   cache without overwriting existing edits. Even if scraping fails, the command establishes the
   correct file name for you to edit.
3. **Edit the cache file:** Open `~/.cache/cjsh/generated_completions/<sanitised-name>.txt` and
   adjust the summary or add new lines using the format above.
4. **Add multi-level entries:** For subcommand-specific completions (e.g. `kubectl get`), use the
   scope field to keep the nested tree in one rich specification. Separate legacy files such as
   `kubectl-get.txt` remain supported and are stitched together automatically.
5. **Preserve your changes:** Future runs of `generate-completions` default to `--force`. Use
   `generate-completions --no-force ...` when you want to refresh other commands without clobbering
   manual content, or keep a copy of the file under version control and reapply as needed.

You can delete a cache file to force cjsh to rescrape the man page on the next completion request.
This is useful after upgrading a tool with new options.

## Tips and troubleshooting

- **Commands without man pages:** Some utilities only provide `--help`. Create the cache file
  manually or point `generate-completions` at a packaged man page (for example, install the
  corresponding `*-doc` package).
- **Unusual formatting:** If the parser misses options, check the rendered man page. Options that do
  not start with `-` or subcommands listed outside dedicated sections may need to be added manually.
- **Custom summaries:** Inline help uses the summary line. Tailor it to the way you present the
  command in your prompts or completion preview.
- **Refreshing everything:** Remove `~/.cache/cjsh/generated_completions` or run
  `generate-completions --force` to rebuild all caches. Beware this overwrites manual edits.
- **Sandboxed environments:** If `man` is unavailable, completions fall back to whatever data already
  exists. Consider bundling cache files with your dotfiles so they can be copied into the cache
  directory during provisioning.
- **Too many matches?** Reduce menu noise with `cjshopt set-completion-max <number|default|status>`
  (any value >= 1).

With these tools you can match or exceed the curated completion sets provided by fish, bash, or zsh
while keeping cjsh's minimal-runtime-dependency footprint.
