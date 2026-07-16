# Security policy

## Supported versions

Finer is pre-alpha and has no supported release series yet.

| Version | Security updates |
| --- | --- |
| Current `main` branch | Best effort |
| Older commits or local forks | Not supported |

This policy will be updated when the first tagged release is published.

## Reporting a vulnerability

Do not open a public Issue containing vulnerability details, private paths,
file names, clipboard contents, or proof-of-concept data that could harm a
user.

Use GitHub's private vulnerability reporting for this repository when it is
available. If the private reporting button is unavailable, open a public Issue
containing only a request for a private contact channel. Do not include the
vulnerability details in that Issue.

Please include privately:

- the affected commit or release;
- macOS and Karabiner-Elements versions;
- the security impact;
- minimal reproduction steps;
- whether file contents, paths, clipboard data, or destructive file operations
  are involved;
- a suggested fix, if available.

The maintainer will acknowledge a complete report on a best-effort basis,
confirm whether it is in scope, and coordinate disclosure after a fix is
available. Pre-alpha status means a fixed response deadline cannot yet be
guaranteed.

## In-scope examples

- command or argument injection through a file name or path;
- unintended deletion, overwrite, copy, or move outside the selected targets;
- disclosure of local paths, file names, clipboard contents, or file contents;
- modification of the user's main `karabiner.json` contrary to the installer
  contract;
- unsafe permissions or privilege escalation introduced by Finer;
- remotely exploitable behavior in future distribution or update mechanisms.

Normal functional bugs, performance regressions, unsupported Finder views, and
feature requests should use the public Issue forms unless they also create a
security impact.

## Data handling

Finer does not include telemetry. Diagnostic and benchmark artifacts must be
reviewed before sharing. Replace user names, home paths, private file names, and
clipboard data with synthetic values.
