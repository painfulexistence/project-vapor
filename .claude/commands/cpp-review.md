---
description: "Strict C++ codebase review. Accepts style flags to control focus areas. Usage: /cpp-review [styles] [--output <file>]"
---

# C++ Codebase Review

Perform a strict, structured code review of the C++ codebase based on the requested styles.

## Arguments

`$ARGUMENTS` — space-separated style flags and options. Parse them as follows:

### Style flags (can combine multiple)

| Flag | Focus area |
|------|-----------|
| `naming` | Function/variable/type naming consistency (casing, prefixes, abbreviations) |
| `design` | System design pattern consistency (error handling, ownership, singletons, APIs) |
| `legacy` | Old/new system mixing (deprecated paths coexisting with replacements) |
| `duplicates` | Duplicate or near-duplicate code (components, logic, system implementations) |
| `cpp` | Bad or outdated C++ patterns (raw pointers, missing [[nodiscard]]/noexcept/const, uninitialized members, C-style APIs, etc.) |
| `security` | Safety issues (UB, uninitialized reads, unchecked array access, signed/unsigned, dangling refs) |
| `all` | All of the above (default when no flags given) |

### Options

| Option | Effect |
|--------|--------|
| `--output <filename>` | Write the report to `<filename>` instead of printing to chat |
| `--lang <ext>` | Override file extension to scan (default: `cpp,hpp,h`) |
| `--dir <path>` | Limit scan to a subdirectory (default: entire repo) |

**Examples:**
```
/cpp-review
/cpp-review naming cpp
/cpp-review design legacy --output design_report.md
/cpp-review all --output review_report.md
/cpp-review duplicates --dir Vapor/src
```

---

## Instructions

### Step 0 — Parse arguments

Read `$ARGUMENTS`. Extract:
- Which style flags are active (if none specified, treat as `all`)
- `--output` path (if given)
- `--dir` scope (if given, restrict all searches to that directory)
- `--lang` extensions (default `cpp,hpp,h`)

Print a one-line summary: "Reviewing: [active styles] | Scope: [dir] | Output: [file or chat]"

---

### Step 1 — Explore codebase structure

Use the Explore agent (or Bash find/grep) to build a map of:
- All source files within the scope directory
- Key header files and their approximate purpose

Keep this brief — you need file paths, not full contents yet.

---

### Step 2 — Run style-specific analysis

For each active style, read the relevant files and apply the checklist below. Spawn parallel Explore agents where the files are independent to save time.

---

#### Style: `naming`

For each file read, collect every instance of:

**A. Case convention inconsistency**
- Types/classes: should all use one convention (PascalCase is standard C++)
- Free functions and methods: should all use one convention (camelCase is common)
- Member variables: should use ONE consistent prefix style per project (`m_`, `_`, none — pick one)
- Enum values: pick one of `PascalCase`, `UPPER_SNAKE_CASE`, or `kPascalCase` — do not mix
- Constants: `UPPER_SNAKE_CASE` or `kCamelCase` — do not mix with enum values

**B. Prefix inconsistency**
- List every member variable prefix style found (`m_`, `_`, `s_`, none)
- Flag any file or class that mixes two or more prefix styles

**C. Boolean naming**
- Boolean members/locals should consistently either use `is`/`has`/`can` prefix or not
- Flag inconsistencies (e.g., `isActive` next to `visible`)

**D. Abbreviations**
- Flag any identifier < 4 chars that is not a loop counter (`i`, `j`, `k`) or a universally understood abbreviation
- Flag domain-specific abbreviations without comments

**E. Class naming**
- Flag any class/struct name that uses `_` (snake_case) instead of PascalCase
- Flag numeric suffixes without semantic meaning (e.g., `set0s`, `set1s`)

---

#### Style: `design`

**A. Singleton pattern**
- List all singletons found. Do they use the same implementation pattern?
- Flag: raw pointer static, vs Meyers singleton, vs other

**B. Error handling strategy**
- Enumerate all error handling approaches found: exceptions, bool returns, error enum, optional, expected
- Flag files/subsystems that use different strategies from each other

**C. Ownership model**
- List uses of: raw pointer, `unique_ptr`, `shared_ptr`, handle types, entity IDs
- Flag any class that mixes multiple ownership models for the same conceptual resource

**D. API symmetry**
- For manager classes (`*Manager`, `*System`), check that `init()`/`deinit()`, `load()`/`unload()`, `create()`/`destroy()` are symmetric and consistent across all managers

**E. Abstract interface vs concrete**
- For abstract base classes (pure virtual), flag if derived classes expose additional public methods that bypass the abstraction
- Flag if two layers of the codebase use incompatible signatures for logically equivalent operations

**F. Callback vs event-driven vs polling**
- Enumerate the different patterns used for inter-system communication
- Flag places where the same concern is handled by different patterns

---

#### Style: `legacy`

Search for evidence of incomplete migrations:

**A. Coexisting old/new APIs for the same operation**
- Look for: two different ways to load assets, two different ways to create entities, two different camera APIs, etc.
- Flag both call sites with file:line references

**B. Comments that admit the code is transitional**
- Grep for: `TODO`, `FIXME`, `HACK`, `legacy`, `old`, `deprecated`, `compatibility`, `kept for`, `will be removed`
- For each, report the comment and whether the surrounding code actually has a replacement visible

**C. Structs/classes marked as legacy but still used**
- Check if any type's comment says "legacy" or "old" but it still appears in non-legacy code

**D. Two implementations of the same system at different layers**
- E.g., engine layer system + game layer re-implementation with different signatures

---

#### Style: `duplicates`

**A. Near-identical struct/class definitions**
- Search for structs defined in multiple files with same or similar field names
- Report: both file:line locations, which fields differ

**B. Copy-pasted code blocks**
- Look for loops, helper functions, or initialization blocks that appear 2+ times with trivial variation
- Report: locations and what varies between copies

**C. Parallel implementations of the same algorithm or system**
- E.g., the same ECS system implemented in two places with different parameter types
- Report: both locations and their signatures

**D. Stub/empty implementations**
- Search for methods that have a body consisting only of `return`, `{}`, or a single comment
- Report: file:line, method name, and whether a TODO comment exists

---

#### Style: `cpp`

**A. Raw pointers where ownership exists**
- Flag: `T*` used as an owning pointer (not observer). Should be `unique_ptr<T>` or `shared_ptr<T>`
- Exclude: non-owning observer pointers to longer-lived objects (these are acceptable but should be documented)
- Flag: `void*` used for type erasure instead of templates or `std::variant`

**B. Missing modern qualifiers**
- `[[nodiscard]]`: flag all getters and query methods (returning non-void, named `get*`, `is*`, `has*`, `find*`, `try*`) that lack it
- `noexcept`: flag const getters and trivial methods that cannot throw but are not marked `noexcept`
- `const`: flag getter methods that don't modify logical state but are missing `const`
- `constexpr`: flag functions computing compile-time values that could be `constexpr`

**C. Mutable state problems**
- Flag lazy-init getters that modify members but aren't `const` — should use `mutable`
- Flag any `const` method that calls `const_cast` to modify state

**D. Uninitialized members**
- Flag any member variable declared without `= value` or `{}` where the type does not have a default constructor that zero-initializes
- Pay special attention to: `float`, `int`, `bool`, raw pointer members, `uint32_t`

**E. C-style patterns in C++ code**
- `strncpy`, `sprintf`, `printf`, `malloc`/`free`, `memset`, `memcpy` — flag each
- Raw C arrays (`T arr[N]`) where `std::array<T, N>` should be used
- C-style casts `(T)x` instead of `static_cast<T>(x)`
- POSIX macros: `M_PI`, `NULL` instead of `std::numbers::pi`, `nullptr`

**F. Signed/unsigned comparison**
- Flag any comparison of signed integer with `.size()`, `.length()`, or other size methods that return `size_t`

**G. Expensive patterns in hot paths**
- `std::function` members in ECS components (type-erased, heap-allocating)
- `shared_ptr` used where `unique_ptr` or a handle would suffice

**H. Exception safety**
- If exceptions are used anywhere, check for RAII coverage: are resources cleaned up if an exception propagates?
- If exceptions are NOT used, flag any `throw` statements as inconsistent

---

#### Style: `security`

**A. Unchecked array/vector access**
- Flag use of `operator[]` on `std::vector` or `std::unordered_map` without prior bounds/existence check
- Prefer `at()` in non-hot-path code or add explicit guard

**B. Integer overflow/underflow risk**
- Flag arithmetic on user-controlled or external-source integers without overflow check
- Flag index arithmetic (e.g., `ring * (segments + 1) + seg`) without bounds validation

**C. Use-after-free / dangling reference risk**
- Flag lambda captures by `[&]` where the lambda may outlive the captured variables
- Flag observer raw pointers returned from functions where the pointee lifetime is not guaranteed

**D. Unvalidated input at system boundaries**
- Flag functions that accept file paths, user strings, or external data without validation

---

### Step 3 — Compile report

Structure the report as follows. Omit sections for styles not requested.

```markdown
# C++ Code Review Report

**Date:** <today>
**Scope:** <dir or "full codebase">
**Active styles:** <list>

---

## Summary

<2–4 sentence overview of the most important findings>

---

## [Style: Naming] (if active)

### <Sub-category A>
- **<issue title>** — `file.hpp:line` — <one sentence description>
  ```cpp
  // bad
  <snippet>
  ```

### <Sub-category B>
...

---

## [Style: Design] (if active)
...

## [Style: Legacy] (if active)
...

## [Style: Duplicates] (if active)
...

## [Style: C++ Patterns] (if active)
...

## [Style: Security] (if active)
...

---

## Severity Table

| # | Issue | Severity | Location |
|---|-------|----------|----------|
| 1 | ... | CRITICAL/HIGH/MEDIUM/LOW | `file:line` |
...

**Totals:** CRITICAL: N | HIGH: N | MEDIUM: N | LOW: N
```

Severity definitions:
- **CRITICAL** — correctness bug, UB, data corruption, or broken abstraction that affects multiple callers
- **HIGH** — inconsistency or pattern that actively harms maintainability or has UB risk
- **MEDIUM** — style violation with real maintenance cost, or design issue that will cause friction
- **LOW** — style preference, minor nit, or theoretical future concern

---

### Step 4 — Output

If `--output <file>` was given:
- Write the report to that file
- Print to chat: "Report written to `<file>` — N issues found (CRITICAL: N, HIGH: N, MEDIUM: N, LOW: N)"
- Commit and push the file to the current branch

If no `--output`:
- Print the full report to chat

---

## Wrap up

End with one sentence: what the single highest-priority fix would be and why.
