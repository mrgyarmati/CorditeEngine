# Generic Coding Standards and Best Practices

This language-agnostic standard is a starting point for a team or project. Adopt an established language-specific style guide where applicable, document project-specific decisions, and automate formatting wherever possible.

## Readability and structure

- Use descriptive, consistent names for variables, functions, classes, files, and directories.
- Keep functions focused on one task. Split up code that is difficult to understand, but avoid unnecessary abstractions.
- Prefer straightforward control flow; reduce deep nesting with early returns where appropriate.
- Keep related code together and organize files into logical modules.
- Follow the project formatter for indentation, whitespace, line breaks, and line length.

## Behavior and reliability

- Make inputs, outputs, and side effects clear at module or function boundaries.
- Handle expected errors explicitly. Do not silently ignore failures; provide useful error messages without exposing sensitive details.
- Add or update automated tests when behavior changes. Cover normal cases, important edge cases, and failure paths.
- Document public interfaces and non-obvious decisions. Comments should explain why something is done, not repeat what readable code already says.

## Security and dependencies

- Treat external input as untrusted and validate it at a trusted boundary. Check expected types, lengths, ranges, and formats.
- Do not hardcode credentials or commit secrets. Give components only the permissions they need.
- Keep dependencies deliberate: use supported packages, record versions reproducibly, and review security findings.

## Changes and review

- Keep changes small enough to review effectively, with a clear purpose.
- Run the formatter, linter, and relevant tests before merging; use CI to enforce the checks your team adopts.
- Review changes for design, correctness, complexity, tests, style, and documentation—not just whether they compile.

**Decision rule:** Project standard first, consistency second, personal preference last.
