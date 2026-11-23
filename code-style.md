<!-- Copied from ~/.claude/guidelines/code-style.md by tool/sync.py.
     Edit the source, not this part: this project's own rules follow
     the end-of-copy line below. -->

# code-style.md

Code style for private projects. This file is the **source**. Every private
project except the one holding it carries a copy at its repo root, written
by `tool/sync.py` -- see *Keeping the copies in sync* at the end.

Vendored submodules, generated sources and attic/historical material are
exempt: they keep whatever their upstream or generator produces. Each
project names its own exempt paths in its part of the file.

## The three rules

1. **`snake_case`, not `camelCase`,** for identifiers this project defines.
2. **Tabs for indentation, spaces for alignment.**
3. **Lowercase filenames,** unless a tool demands otherwise.

Everything below is these three rules in detail, plus the exceptions that
are already settled. An exception not listed here is not yet settled: raise
it rather than deciding in passing.

## 1. Naming

`snake_case` for functions, variables, type names and fields.

This holds **even inside a toolkit whose own API is `camelCase`**. Call the
foreign API exactly as it is spelled (`setParent`, `addWidget`) -- that is
the API's name, not a violation. But names *you* introduce stay
`snake_case`; do not let the surrounding convention pull them across.

- **Name the thing, not its category**: `plan`, not `plan_struct` or
  `plan_result`.
- **No abbreviations that are not already vocabulary.** `observed`, not
  `obs`; `interface`, not `iff`. This matters most where an internal name
  escapes into something you cannot rename later -- a wire format, a config
  key, a CLI output, an on-disk path.
- **One word per concept, everywhere** -- the same word in the type name,
  the file path, the subcommand and the documentation. A synonym introduced
  for variety reads as a second concept.

### Prefixes, and visibility

Prefixes keep this project's symbols from colliding with a library's, so
they follow **visibility**, and the choice is judgement rather than a
mechanical rule:

- **Anything with more than small visibility carries the project prefix**
  -- the public API, and anything a linker or importer outside its own
  module can reach.
- **Module-private symbols are left unprefixed**, so that the absence of a
  prefix reads as "this does not leave the module."

The middle case decides itself on link safety, not taste. A symbol internal
by intent but still reaching the linker -- cross-file within a library, not
`static`, not part of the API -- is *not* private for this purpose: prefix
it. A deliberate parallel copy of a function in two libraries needs a
**distinct** name; the assumption that nothing will ever link both sides
fails later, at a call site that changed nothing, and names files you did
not touch.

Where a language enforces its own scheme, accept it, and say in the
project's part of this file that the toolchain is doing it:

- **Rust** -- `non_snake_case` and `non_camel_case_types` are on by
  default, so types are `PascalCase` and constants `SCREAMING_SNAKE_CASE`
  -- the toolchain's, not a choice. Package systems that demand kebab-case
  (Cargo crates, Debian packages) read back with their own spelling; do not
  invent a third by naming the directory differently from the package.
- **Python** -- a leading underscore (`_name`) is the language's private
  marker and stands in for "unprefixed" above.

## 2. Indentation and alignment

Indent structural nesting with **tab** characters, one tab per level. When
lining up tokens *within* a line -- continuation parameters under an open
paren, a block comment's `*` column, an aligned trailing comment -- use
**spaces**, after the indent tabs. Alignment is then relative to the shared
leading tabs, so it survives at any tab width. No tab width is prescribed;
the viewer decides.

```c
int thing_do(thing_t *thing, const char *name, size_t name_len,
              uint8_t *out, size_t out_cap) {
>---if (!thing) return ERR_MALFORMED;
>---return thing_write(thing, name, name_len, out, out_cap,
>---                    THING_DEFAULT_FLAGS);
}
```

(`>---` marks a tab; everything lining up under `(` is spaces.)

Never mix tabs and spaces *within* the indent itself. Tabs come first and
spaces after; the reverse, or an alternation, breaks at a different tab
width -- and in Python it is a syntax error.

### Settled exceptions

Divergence needs a technical reason. These are accepted:

- **Makefiles** -- a recipe line requires a literal tab, and the body of an
  `ifeq`/`ifdef` outside a recipe may be space-indented, since after a rule
  a tab-led line is read as a recipe. Both are correct in one file, so the
  gate does not apply the tab rule to Makefiles.
- **YAML** -- the spec forbids tabs for indentation. Use spaces.
- **Markdown** -- list continuation and code fences are space-indented by
  specification. Exempt.
- **Debian packaging files** -- exempt, for two reasons measured against
  dpkg. `debian/changelog` has a fixed layout with no tab in it:
  `dpkg-parsechangelog` calls a tab-indented change line "unrecognized"
  and loses the trailer if a tab precedes `--`. A deb822 continuation in
  `control` or `copyright` may lead with a space *or* a tab, but that
  whitespace is field syntax rather than indentation, and everything past
  it is alignment.
- **Go** -- `gofmt` emits tabs natively. Compliant already.
- **Shell -- tabs wherever they cannot change what the script does**
  (holder, 2026-11-23). A heredoc body is written out byte for byte and
  a multi-line quoted string is a value, so the spaces in both stay; the
  gate skips them. A script that must stay short, or that nobody reads,
  is exempt by name -- `tab_exempt` in `.style-gate.toml` -- and listing
  it is the tree's claim that it is one of those.
- **Vendored, generated and attic sources** -- exempt, per the header.

**Python**: PEP 8 prefers spaces, but the language's only hard rule is that
indentation must not be *ambiguous*, and tabs-then-spaces is unambiguous at
every tab width. Continuation lines inside brackets are not
indentation-significant at all. Never a space *before* a tab in leading
whitespace -- that is what raises `TabError`.

Anything else that seems to need spaces: signal it to the list in
`claude-guidelines`' `project.md`, follow the rule meanwhile, and it is
settled here in a pass rather than in whichever project met it first.

## 3. Filenames

**Lowercase, always**, for everything the project names itself:
`main_window.cpp`, not `MainWindow.cpp`.

**The separator follows what the name binds to**, which is a technical
difference rather than taste:

- **`snake_case` where the filename becomes an identifier** -- a source
  file, a header, a module. `desired_state.rs` *is* the module
  `desired_state`, and `desired-state.rs` cannot be a module at all, a
  hyphen not being legal in a Rust path; Python imports are the same. That
  is the language's requirement, and not negotiable where it applies.
- **`kebab-case` for prose** -- documentation, design notes, decision
  records. Nothing imports `code-style.md`, so no identifier is at stake,
  and kebab-case is what markdown and URLs settled on. (When this rule said
  `snake_case` for documentation too, 174 of 197 markdown names across the
  workspace were already kebab-case.)

Settled exceptions:

- **Names a tool will not accept lowercased** -- `Makefile`,
  `CMakeLists.txt`, `AndroidManifest.xml`, `Dockerfile`, `Cargo.toml`.
- **Root files with an established convention** -- `README.md`, `LICENSE`,
  `CHANGELOG.md`, `AUTHORS`, `VERSION`. The last is this workspace's own: a
  project that packages or ships a version tracks one, and the build reads
  it for the package version and for what the program prints, so the
  number lives in one place. `claude-guidelines` packages nothing and has
  none.
- **Package-system spellings** -- kebab-case where Cargo or Debian require
  it, which is the spelling prose uses, so a crate directory and the design
  note beside it agree by construction.

### Singular, unless somebody else standardised the plural

**Prefer the singular for a directory this project names itself**:
`helper/`, `doc/`, `fixture/`. The name says what kind of thing lives
there, not how many, and should not have to change when the count does.

Two exceptions, and they are not equal -- the same shape as the lowercase
rule, which yields first to `Makefile` because make will read nothing else,
and only then to `README.md` because the world settled it.

**First: a name a tool requires is not a name we choose.** It outranks the
singular as it outranks lowercase, needs no measurement, and the test is
whether something breaks when the name changes. It is open-ended: a tool
met tomorrow that demands a name gets the same answer, whether the name is
plural, singular, capitalised or none of those. Present here: **Cargo**
looks for `tests/`, `examples/` and `benches/`, and `cargo-fuzz` for
`fuzz_targets/`; **GitHub** requires `.github/workflows/`; **git** keeps
`hooks/`, hence `tool/hooks/`. **A foreign package's directory names are
the same exception one layer out**: the Android SDK's `platform-tools/` and
`build-tools/` are the SDK's spelling. A word-boundary rewrite of `tools/`
once reached into `platform-tools/`, the hyphen being a boundary, and every
adb target in four projects pointed at a binary that was not there -- so a
sweep skips compound names it does not own. Package-system spellings (Cargo
crates, Debian packages) under *Filenames* above are this same carve-out.

**Second: a plural an ecosystem has settled** -- a convention rather than a
requirement: nothing breaks, but a reader would be surprised. Cargo
workspaces keep members in `crates/`. **These need measuring**, and the
project's part of this file names what it measured against, so the next
reader does not reopen it.

Say which kind is being claimed: confusing them lands on whoever renames a
directory that looked like a convention and finds the build broken. And an
inventory name in `harmonization.md` moves only when its owner says so, in
a deliberate pass with each project's gates run against the result -- as
`tools/`, `docs/` and `docs/decisions/` became `tool/`, `doc/` and
`doc/decision/` -- never as a side effect of a style rule.

## ASCII in source

Source and comments are ASCII. Write `--` where prose would use an em dash,
and "section" for a section sign.

This governs **the text the repository writes about itself**, not the data
the software handles. Three exceptions, which are the rule's shape rather
than holes in it:

- **Documentation.** Markdown may use typographic punctuation.
- **User-facing text in UI software.** A tick a program prints is output,
  not prose -- `GREEN('gpg ')` is correct as it stands.
- **Anything that genuinely requires Unicode**: a fixture for a UTF-8
  parser, a terminal emulator's character tables, a font tool.

Where a project needs the rule enforced, `ascii_only` in `.style-gate.toml`
turns it on. In Python and in C/C++ it enforces exactly that shape -- ASCII
outside string literals, Unicode allowed inside -- reading Python with
`tokenize` and C and C++ with a scanner written for the purpose. Every
other language gets a whole-file byte check, and so does a file in those
two that will not lex: a file nobody can parse is not a file that has been
cleared.

**An exception wider than its reason is how a rule stops being enforced.**
The check was once whole-file for everyone, so a project printing two
status ticks switched it off, and an em dash arrived in a comment; in a Qt
tree that kept it off for its toolbar glyphs, 854 of 1114 non-ASCII
characters turned out to be prose in comments.

## Formatters

A formatter is allowed **only if it can be configured to honour the three
rules completely**. Configuration gaps are disqualifying: a formatter that
gets indentation right and alignment wrong rewrites the tree on somebody's
next save. So the decision is per tool, per project, and a real evaluation:

- If it can be made to comply, use it, and commit the config with a comment
  saying which setting is load-bearing and what happens without it.
- If it cannot, do not run it -- **not even ad hoc on a single file**. The
  failure is a silent conversion of files that were already correct,
  found later as a reverted commit rather than an error.
- If no existing tool fits and the rule is worth mechanising, write our
  own. A checker that only gates indentation is worth more than a formatter
  that reflows everything.

**Record the decision and the finding that produced it** in the project's
part of this file -- which tool, what failed, what would change the answer.
A verdict without its evidence gets re-litigated, and a tool that improves
later is never reconsidered because nobody remembers what was wrong with
it.

Naming and filename rules are review items, not automated ones.

## Precedence

Three layers, and they are not equals:

1. **The global guidelines** (`~/.claude/CLAUDE.md` and the files it
   imports) -- the source, and they win.
2. **The project's `project.md`** -- project-specific design and
   conventions.
3. **The project's `code-style.md`** -- this file, copied, and the
   project's own part after it.

A project file that disagrees with the source is **drift, not an
override**: fix it. A project that genuinely needs to diverge needs a
technical reason, and that is not a decision to make while working on
something else -- signal it to the list in `claude-guidelines`'
`project.md` and keep following the source meanwhile.

**When a conflict between layers actually comes up, stop and ask.** Do not
silently pick a winner, even the global one.

This precedence rule lives here and in the global guidelines only. It does
not belong in a `project.md`.

## Keeping the copies in sync

Each private project keeps `code-style.md` at its root, in two parts: this
source, verbatim, under a header naming it; then, after a line marking the
end of the copy, **the project's own part**, holding only what is genuinely
its own -- its exempt paths, formatter verdicts, language-specific notes
and tooling commands, under its own headings. `claude-guidelines` is the
exception: it holds the source at `guidelines/code-style.md`, and its root
file points here.

**`tool/sync.py` writes the copied part and never touches the project's
part**, the same way it spreads the gate. So a copy cannot drift, and a
project that wants a rule to read differently changes the source, through
the signal list, rather than its copy. Until 2026-11-20 copies were adapted
by hand and audited only for their headings, and most had drifted in prose
no audit could see.

**Keep a census out of this document.** A count written here is copied
into every tree and goes stale in all of them at once; state the rule and
the criterion that decides it, and keep the count where it can be
recounted once.

**This source is plain ASCII**, so that it is valid in a tree whose own
rules restrict the characters its files may contain. Keep it so.

**Re-read this source before writing a project's own part, or anything
about the rules**, rather than working from what was loaded at the start of
the session: text written against a stale source is drift being written.
**A copied part that differs from the source** -- a tree the spread has not
reached, a hand edit -- is put right with `sync.py --commit <tree>`, never
by editing the copy; `sync.py --check` lists any that differ. A project's
`project.md` may state the three rules in brief and point here; it does not
restate the precedence rule.

<!-- End of the copied part. What follows is this project's own. -->

## This project

Code style for BeerSSH. Applies to everything this project writes — the Qt
C++ app (`src/`), the tooling (`tool/`), the build files, and whatever
native dependencies get wrapped later. `project.md` §18 states the rules in
brief and points here for the detail; where the two ever disagree,
`project.md` wins.

Vendored sources are exempt: here that is `vterm/` above all.

**This is the same style as the sibling `fuzzypickles` tree -- which is now
a consequence rather than a coincidence.** Both copy the same source. Both
are Qt Widgets frontends over a C-ish core, and moving between them should
cost nothing. Both now run the same gate, copied verbatim from
`~/.claude/tool/style_gate.py`, so keeping them alike is the sync rule
doing its job rather than two trees agreeing by hand.

### Naming in a Qt C++ app

This holds **even though the whole app is Qt C++**. Qt's own API is
`camelCase` and you call it exactly as it is (`setParent`, `addWidget`,
`horizontalAdvance`) — but names *you* introduce stay `snake_case`. Do not
let the surrounding toolkit's convention pull your own names camelCase; a
file that mixes both is worse than either.

- Types this project owns carry the **`bssh_` prefix**:
  `bssh_terminal_view`, `bssh_session_registry`, `bssh_input_router`.
  Plain structs and typedefs take a `_t` suffix (`bssh_pane_geometry_t`);
  classes don't need one.
- C++ member variables keep an `m_` prefix but are otherwise snake_case:
  `m_cell_width`, `m_session_registry`.
- **Qt signals and slots are ordinary members** and follow the same rule:
  `grid_resized`, `on_grid_resized` — not `gridResized`.
- Prefer a plain descriptive name over a decorated one. The SSH transport
  interface is `bssh_ssh_session`, not `ISshSession` or
  `AbstractSshBackend`; the `I`-for-interface prefix is not used anywhere
  in this project.
- **No abbreviations that are not already vocabulary** -- here that means
  the settings keys, the `beerssh_hosts` fields and the command-line
  options above all: a name chosen carelessly in one of those is a name
  kept forever.
- The component names in `project.md` §3 are the real ones. Use them
  rather than inventing synonyms, and update §3 if one genuinely needs
  renaming.

#### Prefixes, and visibility: a blanket rule here

**beerssh states this as a blanket rule instead**, and it is the same
rule's answer for this codebase rather than a departure from it: every type
this project owns is declared in a header and reachable across modules, so
the visibility test comes out "prefix it" every time. Its 87 header types
all carry `bssh_`; its file-local helpers live in anonymous namespaces, so
they have internal linkage and no prefix, which is what the source means by
leaving module-private symbols unprefixed. The Qt types forward-declared in
headers keep their own names, as the paragraph above requires.

### Indentation in C++

Alignment within a line also covers a constructor's member-initialiser
list. No tab width is prescribed; reviews here have used 2, as a viewing
choice only.

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

**A braceless single-statement body is permitted, and sometimes better.**
It used to be discouraged here for a tool's convenience: the old
`tabify.py` counted braces only, so a body with no brace was not a nesting
level to it and came out indented with a tab *plus spaces*. That was the
tool failing to model C, and the advice had the fault backwards -- it asked
the source to change shape so the checker would stop being wrong. The
shared gate now tracks `if`/`for`/`while`/`else`/`do` bodies whether or not
they carry braces, so write whichever reads better.

### Filenames in this tree

`platform_android.cpp`, not `PlatformAndroid.cpp`. A C++ source is not
imported by its name, but it keeps the `snake_case` spelling so one rule
covers the whole of `src/`. Both halves of the separator rule already hold
here, so nothing has to move.

qmake imposes no capitalisation constraint — `beerssh.pro` is lowercase
because nothing forces otherwise. `VERSION` is this project's, and is the
same case as the other root files with an established convention.

### Formatters: clang-format is deliberately NOT used

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

### Tooling

Indentation and whitespace are mechanically enforced by the shared gate,
copied verbatim into `tool/` from `~/.claude/tool/style_gate.py`:

```sh
make style                        # the gate -- run it before committing
python3 tool/style_gate.py check # same thing, directly
python3 tool/style_gate.py fix   # convert to the convention
python3 tool/style_gate.py list  # what it looks at, and why
```

`.style-gate.toml` says which files in this tree the rules apply to. The
gate refuses to run against a file list that has collapsed below the floor
there, because a gate that has quietly stopped looking reads exactly like a
clean tree.

Each fixer carries a proof rather than a promise: C and C++ must re-expand
to the original text, and Python must keep an identical AST. A fixer that
cannot prove its edit refuses to make it.

### Comments

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
- **A pointer to the spec section** (`project.md` §7, §9.5, ...) rather
  than a paraphrase of it, so the two cannot drift.

### ASCII in source: not turned on here, and an open decision

The check is `ascii_only` in `.style-gate.toml`, and **this project does
not turn it on**. The reason has changed, and the old one no longer holds.

It used to be off here because the check could not tell a character table
from prose, and this is a terminal emulator whose subject matter is
character tables. The C/C++ scanner removes that reason, so the question
was measured rather than argued (2026-08-10, using the gate's own
`c_ascii_problems` over its own file list). Of **28 non-ASCII characters in
the 152 C and C++ files the gate reads, 21 are inside literals** and are
data the emulator handles; **7 in 4 files** are what turning the flag on
would report.

Only one of those seven is what the rule is aimed at -- an em dash in a
comment in `src/platform/secret_store.h`. The other six are **glosses on
escaped bytes**: `/* 中文 */` written beside
`QByteArrayLiteral("\xe4\xb8\xad\xe6\x96\x87")` in the emulator and view
tests, and `"héllo"` and `"é"` naming what a split UTF-8 sequence and a
compose-key sequence are meant to produce. Those comments are the only
thing making the escapes readable, and a rule that deleted them would be
buying its own enforcement with the tests' legibility.

**So this is a decision, not a chore, and it is open.** The source's third
exception covers "a terminal emulator's character tables", but it is
written about *data* and these are *comments*. Either the exception grows
to cover a comment that glosses data it sits beside, or those six get
spelled some other way, or the flag stays off. Seven characters is a small
enough job that the answer should be decided rather than inherited --
unlike the sibling Qt tree, where the same count came to 854 across 163
files.

### The commit-msg hook

The commit-msg hook is `tool/hooks/commit-msg`, installed with
`make hooks`. It rejects generator attribution, a subject over 75 columns,
and body prose over 75 columns. It lives in the tree rather than only in
`.git/hooks` so that it is reviewable and survives a clone; the copy that
runs is installed from it.

The body limit was stated long before anything checked it, and only the
subject was checked -- so a body line at 76 columns went through while a
subject at 76 was refused.

What it deliberately does not reject, in three groups.

**Three names are spared**, so a message may say where the shared tooling
comes from: the directory `.claude`, the file `CLAUDE.md`, and
`claude-guidelines`, the repository the guidelines live in. The ban is on
crediting a generator and none of the three is a spelling of that. Only the
names are neutralised, never the token around them -- a vendor word at the
end of a path under the tree is still refused.

**What git is about to discard**: comment lines, and the diff that
`git commit -v` puts below the scissors line. Reading those refused commits
over text that never reaches the message -- the hook's own diff contains
its own pattern list, so it rejected every commit that edited it.

**Three shapes the length check exempts**, each because wrapping it is the
actual mistake rather than a concession: a *trailer*, since git parses the
block a line at a time and a broken `Link:` stops being a trailer at all; a
line holding a *url*, which no longer works once it is split; and an
*indented* line, which is how a message quotes a compiler error or a stack
trace, where reflowing what you are quoting corrupts the one thing it was
included for. It cannot tell prose opening `Note:` from a trailer, so it
forgives that -- the wrong way round would refuse a real trailer, and that
is the expensive error.

### See also

- **`project.md` §18** — the same rules in brief, plus the commit
  conventions (no docs-only commits; no AI-attribution trailers).
- **`project.md` §3** — the component names this document tells you to
  use.
- **`../fuzzypickles/code-style.md`** — the sibling tree's copy, which this
  one deliberately tracks.
