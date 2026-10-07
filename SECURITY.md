<!--
  SECURITY.md

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

# Security Policy

## Reporting a vulnerability

Please report suspected vulnerabilities privately through
[GitHub's Report a vulnerability form](https://github.com/CadenFinley/cjsh/security/advisories/new)
(the repository's **Security → Report a vulnerability** page).
Do not disclose exploit details, sensitive logs, or credentials in public issues or pull requests.
If the form is unavailable, open an issue asking the maintainer to enable private vulnerability
reporting, without including vulnerability details.

Include, where available:

- The affected release or commit, operating system, architecture, and installation method.
- A minimal reproducer, expected behavior, actual behavior, and potential impact.
- Any relevant build options and whether the problem occurs with `--no-config`.
- Sanitized crash reports or diagnostics. Remove secrets, private paths, history, and environment values.

Test only on systems you own or have permission to test. Maintainers will use the private report
for triage, follow-up questions, and coordination of a fix and disclosure. This is a
maintainer-supported project; there is no guaranteed response time, remediation deadline,
bug bounty, or commercial support SLA. Confirmed vulnerabilities may receive a GitHub security
advisory with affected versions, remediation, and reporter credit where requested.

## Supported versions

Security fixes target the latest stable release and the development branch. Older releases do
not have a guaranteed backport or long-term-support commitment; users should upgrade to the
latest fixed stable release. Development builds are not a substitute for a supported release.
Reports affecting older versions are still welcome, especially if the latest release may also
be affected.

## Security boundaries

cjsh is a command interpreter, **not a sandbox**. Commands and scripts run with the user's
permissions and can read or modify files, start processes, and access the network.

- `--secure` skips automatic configuration and disables history and smart cd. It does not make
  untrusted scripts, commands, completions, or agent executors safe to run.
- `--no-config` skips automatic startup configuration; it does not disable explicit `source`,
  `eval`, command substitution, or command execution.
- Startup files, hooks, dynamic completion providers, and agent executors that run commands
  must be trusted. Review configuration before installing it, and protect its ownership and permissions.
- History, environment variables, diagnostic output, and terminal recordings can contain secrets.
  Avoid placing credentials in command lines or sharing these files without reviewing them.
- cjsh refuses shell initialization when real and effective user or group IDs differ. Installing
  cjsh set-user-ID or set-group-ID is not a supported privilege-elevation mechanism. Running as
  root otherwise gives commands root privileges; cjsh does not provide privilege isolation.

Memory-safety bugs, unintended execution from data-only inputs, and bypasses of documented
startup or credential checks are examples of appropriate private security reports. Ordinary
shell execution of an explicitly supplied command is expected behavior. If uncertain, report
privately and let the maintainer help assess the issue.
