# AGENTS.md

## Communication

- Respond in Japanese when the user writes in Japanese. When the user writes in English, answer in English.
- Prefer direct, task-focused answers over broad generic summaries.
- When explaining a specific requested subsection, answer that subsection directly.

## Git Workflows

- When the user asks what is not pushed, check both committed-but-unpushed changes and uncommitted working-tree changes.
- Explain the exact local-vs-remote state and the concrete changed files.
- When the user asks to commit and push, complete staging, commit, push, and final verification unless blocked.

## File Access

- When the user provides a file path, first verify whether it is readable.
- If a Mail Downloads attachment is not readable due to macOS permissions, ask the user to copy it into the working directory instead of repeatedly retrying the same path.

## When to edit files

- When the user tells you "提案して。", do not edit files unless clearly instructed.

## Finer Project

- Before changing Finer rules, helpers, scripts, packaging, or architecture, read `docs/FINDER_VIM_SPEC.md` completely.
- Treat `docs/FINDER_VIM_SPEC.md` as the source of truth for product requirements and architecture.
- If an implementation decision changes the specification, update the specification and its decision log in the same change.
- Do not turn `~/.config/karabiner` into the public source repository. It is the installed dogfood environment.
- The public source repository should live in a separate working directory and install generated artifacts into this directory.
