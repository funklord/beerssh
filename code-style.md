# code-style.md

Code style for BeerSSH. Applies to everything this project writes — the Qt
C++ app (`src/`), the tooling (`tools/`), the build files, and whatever
native dependencies get wrapped later. `project.md` §18 states the rules in
brief and points here for the detail; where the two ever disagree,
`project.md` wins.

Vendored third-party code keeps its upstream style, and is exempt.

**This is the same style as the sibling `fuzzypickles` tree, deliberately.**
Both are Qt Widgets frontends over a C-ish core written by the same author,
and moving between them should cost nothing. `tools/tabify.py` is a
straight copy of that project's; keep the two in sync rather than letting
them drift.

## The three rules

1. **`snake_case`, not `camelCase`,** for identifiers this project defines.
2. **Tabs for indentation, spaces for alignment.**
3. **Lowercase filenames,** unless a tool demands otherwise.

## 1. Naming

`snake_case` for functions, variables, struct/type names and fields.

This holds **even though the whole app is Qt C++**. Qt's own API is
`camelCase` and you call it exactly as it is (`setParent`, `addWidget`,
`horizontalAdvance`) — but names *you* introduce stay `snake_case`. Do not
let the surrounding toolkit's convention pull your own names camelCase; a
file that mixes both is worse than either.

- Types this project owns carry the **`bssh_` prefix**: `bssh_terminal_view`,
  `bssh_session_registry`, `bssh_input_router`. Plain structs and typedefs
  take a `_t` suffix (`bssh_pane_geometry_t`); classes don't need one.
- C++ member variables keep an `m_` prefix but are otherwise snake_case:
  `m_cell_width`, `m_session_registry`.
- **Qt signals and slots are ordinary members** and follow the same rule:
  `grid_resized`, `on_grid_resized` — not `gridResized`.
- Prefer a plain descriptive name over a decorated one. The SSH transport
  interface is `bssh_ssh_session`, not `ISshSession` or
  `AbstractSshBackend`; the `I`-for-interface prefix is not used anywhere
  in this project.
- The component names in `project.md` §3 are the real ones. Use them rather
  than inventing synonyms, and update §3 if one genuinely needs renaming.

## 2. Indentation and alignment

Indent structural nesting with **tab** characters, one tab per level. When
lining up continuation or aligned tokens *within* a line — parameters under
an open paren, a constructor's member-initialiser list, a block comment's
`*` column, an aligned trailing comment — use **spaces** after the indent
tabs.

The point of the split: alignment is expressed relative to the shared
leading tabs, so it survives at any tab width. A tab width of 2 renders the
tree as it reads in review.

```cpp
void bssh_terminal_view::recompute_grid() {
→   const int cols = qMax(1, width() / m_cell_width);
→   if (cols == m_cols && rows == m_rows) return;

→   emit grid_resized(m_cols, m_rows);
}
```

(`→` marks a tab; anything lining up under an open paren is spaces.)

`switch` bodies get a full tab too: a `case`/`default` body is one level
deeper than its brace nesting, since C and C++ labels open no brace.

**A single-statement body wants braces or a one-liner**, not a bare
indented line:

```cpp
if (!view) return;              /* fine */
if (self_test) {                /* fine */
→   QTimer::singleShot(0, &app, &QCoreApplication::quit);
}
```

This is not an aesthetic preference — the indentation model counts brace
nesting, so a braceless body is not a nesting level and `tools/tabify.py`
will indent it with a tab *plus spaces*. Braces or a one-liner keeps the
file conformant and says the same thing.

### clang-format is deliberately NOT used

There is no `.clang-format` in this repo, and adding one would be a mistake
rather than an oversight. Two reasons, both established in the sibling
tree the hard way:

- With no config present, `clang-format` falls back to LLVM defaults, which
  are **spaces**. Running it silently converts tab-indented sources and
  contradicts everything above — this has actually happened over there,
  reverting committed files.
- A config does not fix it either. The alignment above puts continuation
  parameters one column past the open paren, which clang-format cannot
  express (`AlignAfterOpenBracket` aligns *to* the paren). Any config would
  therefore reflow nearly every multi-line signature in the tree.

## 3. Filenames

Lowercase, `snake_case`, for everything this project names itself — sources,
headers, docs. So `main_window.cpp`, not `MainWindow.cpp`, and
`platform_android.cpp`, not `PlatformAndroid.cpp`.

The exception is a name a tool will not accept lowercased: `Makefile`,
`AndroidManifest.xml`, and similar. Note that qmake imposes no such
constraint — `beerssh.pro` is lowercase because nothing forces otherwise.

## 4. Tooling

Indentation is mechanically enforced; everything else here is a review item.

```sh
make style                              # the gate — run it before committing
./tools/check-indent.sh                 # same thing, directly
python3 tools/tabify.py PATHS           # convert to the convention
python3 tools/tabify.py --check PATHS   # report only, don't modify
make -C tools fix                       # tabify every source in src/
```

`tabify.py` rewrites only leading whitespace and preserves the first
non-whitespace column at tab width 2, so it cannot change code; it is
idempotent and safe to re-run. `check-indent.sh` fails on an implausibly
short file list as well as on non-conformant files — a gate that has
quietly stopped looking reads exactly like a clean tree, which is the
failure it most needs to avoid.

`make style` runs in CI (`.github/workflows/build.yml`) alongside both
platform builds.

## 5. Comments

Match the surrounding density; the existing `src/` is the reference. What
this project's comments are *for*, in order of value:

- **Why, not what.** `m_cell_width` being rounded needs a note about
  accumulated rounding error across 200 columns; the assignment itself does
  not need narrating.
- **The tripwire comment.** Where a rule exists to resist a specific future
  temptation — `QT += quick` in `beerssh.pro`, an `#ifdef` outside
  `src/platform/`, a local PTY on desktop — say so at the place someone
  would type it. The rule is in `project.md`; the reminder belongs where
  the mistake happens.
- **A pointer to the spec section** (`project.md` §7, §9.5, ...) rather than
  a paraphrase of it, so the two cannot drift.

## See also

- **`project.md` §18** — the same rules in brief, plus the commit
  conventions (no docs-only commits; no AI-attribution trailers).
- **`project.md` §3** — the component names this document tells you to use.
- **`../fuzzypickles/code-style.md`** — the sibling tree's copy, which this
  one deliberately tracks.
