---
name: pre-commit-review
description: Mandatory review and verification before any git commit in Cordite. Use every time you are about to commit, when asked to review a change, or when asked whether work is ready to merge.
---

# Pre-commit review

No commit is made in this repository without completing every step below. If a step
fails, fix the cause and start again from step 1: a fix can break something that passed
before.

## 1. Scope

```powershell
git status --short
git diff --stat
```

- Every changed file belongs to the change being committed. Unrelated edits go in their
  own commit.
- Nothing generated or local is staged: no `build/`, `.tools/`, `.cordite/`, captured
  frames, logs or credentials.
- New files are in the right place (see "Repository layout" in `AGENTS.md`).

## 2. Automated checks

Run the gate and read its output. Do not assume a check passed because it passed
earlier; any edit, however small, invalidates the previous run.

```powershell
./scripts/dev.ps1 format     # fix formatting first; the gate only checks it
./scripts/dev.ps1 verify
```

`verify` runs, in order: format-check; MSVC build and tests in `RelWithDebInfo`,
`Release` and `Debug`; clang-cl build and tests; clang-tidy on the files affected by
uncommitted changes. It prints one `[PASS]`/`[FAIL]` line per step, the diagnostics of
each failed step, and ends with `VERIFY PASSED` or `VERIFY FAILED`. Full logs are in
`build/verify/`.

All builds must be free of warnings, all tests must pass and clang-tidy must report no
findings. Fix a tidy finding in the code; add `// NOLINT(check-name)` with a reason
only when the check is wrong for that particular line.

If the change touches a header that many other directories include, also run the full
`./scripts/dev.ps1 tidy`: the changed-files heuristic only covers the header's own
directory. A test that needs a GPU
may be skipped only on a machine without one, and you must say so in the commit
message or hand-off.

## 3. Read the diff as a reviewer

Read every line of `git diff` (and `git diff --cached`), not just the parts you
remember writing. For each file, check:

**Design**
- Does the change belong in this module? Does it respect the layering?
- Is there an existing function, type or dependency that already does this?
- Is anything more general, more configurable or more abstract than the task needs?
- Would a new reader understand the structure without the commit message?

**Correctness**
- Boundaries: empty input, zero, one, maximum, negative, NaN and infinity.
- Integer overflow and narrowing in size and index arithmetic.
- Every error path: is the failure returned, is partial state rolled back, are
  resources released?
- Output parameters are untouched on failure when the documentation says so.
- Does it still behave correctly in `Release`, where assertions are gone?

**Safety**
- External input (files, JSON, scripts, automation requests) is validated before use:
  type, range, length, and that paths stay inside the project.
- No unbounded allocation or recursion driven by input data.
- Lifetimes: no reference, pointer, span or string_view outlives what it points to.
  Pay special attention to lambdas that capture by reference and run later.
- Thread safety: what runs on job threads, and what does it share?

**Standards (`AGENTS.md`)**
- Naming, `Result` versus assertions, `[[nodiscard]]`, include-what-you-use.
- Portability rules: UTF-8 paths, no `std::from_chars` for floats, no `std::format`.
- No leftover debugging output, commented-out code, or TODOs without an owner.

**Documentation and tests**
- Each new public type and function has a `///` comment; each new public header has a
  usage example.
- Tests cover the normal path, the boundaries and every documented failure.
- Tests pin real expected values. A test that cannot fail is worse than no test.
- A new component, property option or other engine feature is used in
  `samples/showcase/showcase.scene.json`; a new editor feature exists as a command, not
  only as a control in a panel.
- `AGENTS.md`, `docs/architecture.md`, `docs/third-party.md`, `README.md` and the skills
  are updated if the change makes any of them stale.

Write down each problem you find, fix it, and repeat from step 2.

## 4. Commit

```powershell
git add <specific files>
git commit
```

Message format:

```
<Imperative summary, at most 72 characters>

<What changed and why, wrapped at 72 columns. Mention anything a reviewer
should look at closely, and anything that was deliberately left out.>

Co-Authored-By: <the agent that wrote the change, if any>
```

Stage files by name. Do not use `git add -A` or `git add .`; they pick up files you did
not review.

## 5. After committing

- `git status` is clean.
- `git show --stat HEAD` lists exactly the files you intended.
- Push only when asked to, or when the task explicitly includes it.
