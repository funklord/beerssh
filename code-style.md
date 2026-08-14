<!-- The three rules and their detail are copied from
     ~/.claude/guidelines/code-style.md -- the source. Keep in sync; fix
     drift the moment you notice it. -->

# code-style.md

Code style for BeerSSH. Applies to everything this project writes — the Qt
C++ app (`src/`), the tooling (`tools/`), the build files, and whatever
native dependencies get wrapped later. `project.md` §18 states the rules in
brief and points here for the detail; where the two ever disagree,
`project.md` wins.

**Above both sits the global source**, `~/.claude/guidelines/code-style.md`,
which applies to every private project. Where this file or `project.md`
disagrees with it, that is **drift to fix, not a local override**. A genuine
divergence needs a technical reason and is signalled to the list in `claude-guidelines`' `project.md` rather
than decided in passing -- and when a conflict between the three actually comes up, stop and
ask instead of picking a winner.

Vendored submodules, generated sources and attic/historical material are
exempt: they keep whatever their upstream or generator produces. Here that
is `vterm/` above all.

**This is the same style as the sibling `fuzzypickles` tree -- which is now a
consequence rather than a coincidence.** Both copy the same source. Both are
Qt Widgets frontends over a C-ish core, and moving between them should cost
nothing. Both now run the same gate, copied verbatim from
`~/.claude/tools/style_gate.py`, so keeping them alike is the sync rule
doing its job rather than two trees agreeing by hand.

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
- **No abbreviations that are not already vocabulary.** `observed`, not
  `obs`; `interface`, not `iff`. This matters most wherever an internal
  name escapes into something you cannot rename later -- a wire format, a
  config key, a CLI output, an on-disk path. Here that means the settings
  keys, the `beerssh_hosts` fields and the command-line options above all:
  a name chosen carelessly in one of those is a name kept forever.
- **One word per concept, everywhere.** The same word in the type name, the
  file path, the subcommand and the documentation. A synonym introduced for
  variety reads as a second concept.
- The component names in `project.md` §3 are the real ones. Use them rather
  than inventing synonyms, and update §3 if one genuinely needs renaming.

### Prefixes, and visibility

Prefixes exist to keep this project's symbols from colliding with a
library's. So they follow **visibility**, and the choice is a matter of
judgement rather than a mechanical rule:

- **Anything with more than small visibility carries the project prefix** --
  the public API, and anything a linker or importer outside its own module
  can reach.
- **Module-private symbols are left unprefixed**, precisely so that the
  absence of a prefix reads as "this does not leave the module."

The middle case decides itself on link safety, not on taste. A symbol that
is internal by intent but still reaches the linker -- cross-file within a
library, not `static`, not part of the API -- is *not* private for this
purpose. Prefix it. A deliberate parallel copy of a function in two
libraries needs a **distinct** name, not the same name in both on the
assumption that nothing will ever link both sides; that assumption fails
later, at a call site that changed nothing, and names files you did not
touch.

Where a language enforces its own scheme, accept it rather than fight it,
and say in the project's copy that the toolchain is doing it:

- **Rust** -- `non_snake_case` and `non_camel_case_types` are on by default,
  so types are `PascalCase` and constants `SCREAMING_SNAKE_CASE`. That is
  the toolchain's, not a choice. Package systems that demand kebab-case
  (Cargo crate names, Debian package names) likewise read back with their
  own spelling; do not invent a third by naming the directory differently
  from the package.
- **Python** -- a leading underscore (`_name`) is the language's private
  marker and stands in for "unprefixed" above.

**beerssh states this as a blanket rule instead**, and it is the same rule's
answer for this codebase rather than a departure from it: every type this
project owns is declared in a header and reachable across modules, so the
visibility test comes out "prefix it" every time. Its 87 header types all
carry `bssh_`; its file-local helpers live in anonymous namespaces, so they
have internal linkage and no prefix, which is what the source means by
leaving module-private symbols unprefixed. The Qt types forward-declared in
headers keep their own names, as the paragraph above requires.

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

**A braceless single-statement body is permitted, and sometimes better.**
It used to be discouraged here for a tool's convenience: the old
`tabify.py` counted braces only, so a body with no brace was not a nesting
level to it and came out indented with a tab *plus spaces*. That was the
tool failing to model C, and the advice had the fault backwards -- it asked
the source to change shape so the checker would stop being wrong. The
shared gate now tracks `if`/`for`/`while`/`else`/`do` bodies whether or not
they carry braces, so write whichever reads better.

### Settled exceptions

Divergence needs a technical reason. These reasons are already accepted and
need no discussion:

- **Makefile recipe lines** -- `make` requires a literal tab. Compliant by
  construction.
- **YAML** -- the spec forbids tabs for indentation outright. Use spaces.
- **Markdown** -- list continuation and code fences are space-indented by
  specification. Exempt.
- **Debian packaging files** -- exempt, and the two halves are exempt for
  different reasons. `debian/changelog` has a fixed layout that a tab is
  not part of: `dpkg-parsechangelog` calls a tab-indented change line
  "unrecognized" and loses the trailer outright if a tab precedes `--`. A
  deb822 continuation in `control` or `copyright` is the opposite case --
  `deb822(5)` allows a leading SPACE *or* TAB and dpkg round-trips either,
  but that leading whitespace is field syntax rather than indentation, so
  the rule has nothing to say about it and everything past it is
  alignment. Both measured against dpkg rather than read off the manual.
- **Go** -- `gofmt` emits tabs natively. Compliant already.
- **Vendored, generated and attic sources** -- exempt, per the header.

Python deserves a note, because PEP 8 prefers spaces and the tension looks
worse than it is: the language's only hard rule is that indentation must not
be *ambiguous*, and tabs-then-spaces is unambiguous at every tab width.
Continuation lines inside brackets are not indentation-significant at all.
Never a space *before* a tab in leading whitespace -- that is the case that
raises `TabError`.

Anything else that seems to need spaces: signal it to the list in
`claude-guidelines`' `project.md`, follow the rule meanwhile, and it gets
settled and added here in a pass rather than in whichever project met it
first.

## 3. Filenames

**Lowercase, always**, for everything this project names itself. So
`main_window.cpp`, not `MainWindow.cpp`, and `platform_android.cpp`, not
`PlatformAndroid.cpp`.

**The separator follows what the name binds to**, and the two cases are a
technical difference rather than a matter of taste:

- **`snake_case` where the filename becomes an identifier** — a source
  file, a header, a module. A C++ source is not imported by its name, but
  it keeps the spelling so one rule covers the whole of `src/`.
- **`kebab-case` for prose** — documentation, design notes, decision
  records. Nothing imports `code-style.md`, so no identifier is at stake,
  and kebab-case is what markdown and URLs settled on long ago.

The rule used to say `snake_case` for prose too, and every private project
was quietly ignoring it. It was rewritten after measuring all fourteen
trees: of 197 tracked markdown basenames, 174 were already kebab-case. Both
halves already hold here, so nothing has to move.

Settled exceptions:

- **Names a tool will not accept lowercased** -- `Makefile`,
  `AndroidManifest.xml`, and similar. Note that qmake imposes no such
  constraint — `beerssh.pro` is lowercase because nothing forces otherwise.
- **Root files with an established convention** -- `README.md`, `LICENSE`,
  `CHANGELOG.md`, `AUTHORS`. `VERSION` is this project's, and is the same
  case.
- **Package-system spellings** -- kebab-case where Debian requires it.

## Formatters

A formatter is allowed **only if it can be configured to honour the three
rules completely**. Configuration gaps are disqualifying, not something to
work around: a formatter that gets indentation right and alignment wrong
will rewrite the tree on somebody's next save.

So the decision is per tool, per project, and it is a real evaluation:

- If it can be made to comply, use it, and commit the config with a comment
  saying which setting is load-bearing and what happens without it.
- If it cannot, do not run it -- **not even ad hoc on a single file**. The
  failure mode is a silent conversion of files that were already correct,
  discovered later as a reverted commit rather than an error.
- If no existing tool fits and the rule is worth mechanising, write our
  own. A checker that only gates indentation is worth more than a formatter
  that reflows everything.

**Record the decision and the finding that produced it** in the project's
copy of this file -- which tool, what specifically failed, what would change
the answer. A verdict without its evidence gets re-litigated, and a tool
that improves later never gets reconsidered because nobody remembers what
was actually wrong with it.

Naming and filename rules are review items, not automated ones.

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

## 4. Tooling

Indentation and whitespace are mechanically enforced by the shared gate,
copied verbatim into `tools/` from `~/.claude/tools/style_gate.py`:

```sh
make style                        # the gate -- run it before committing
python3 tools/style_gate.py check # same thing, directly
python3 tools/style_gate.py fix   # convert to the convention
python3 tools/style_gate.py list  # what it looks at, and why
```

`.style-gate.toml` says which files in this tree the rules apply to. The
gate refuses to run against a file list that has collapsed below the floor
there, because a gate that has quietly stopped looking reads exactly like a
clean tree.

Each fixer carries a proof rather than a promise: C and C++ must re-expand
to the original text, and Python must keep an identical AST. A fixer that
cannot prove its edit refuses to make it.

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

## Precedence

Three layers, and they are not equals:

1. **The global guidelines** (`~/.claude/CLAUDE.md` and the files it
   imports) -- the source, and they win.
2. **The project's `project.md`** -- project-specific design and conventions.
3. **The project's `code-style.md`** -- this file, copied.

A project copy that disagrees with the source is **drift, not an
override**: fix it. A project that genuinely needs to diverge needs a
technical reason, and that is not a decision to make while working
on something else -- signal it to the list in `claude-guidelines`'
`project.md` and keep following the source meanwhile.

**When a conflict between layers actually comes up, stop and ask.** Do not
silently pick a winner, even the global one.

This precedence rule lives here and in the global guidelines only. It does
not belong in a `project.md`.

## Keeping the copies in sync

Each private project keeps a copy of this file at its repo root, opening
with a header that names the source:

```markdown
<!-- Copied from ~/.claude/guidelines/code-style.md -- the source. Keep in
     sync; fix drift the moment you notice it. -->
```

Below the copied rules, a project adds only what is genuinely its own: its
exempt paths, its formatter verdicts, its language-specific notes, its
tooling commands.

**This source is deliberately plain ASCII** -- no em dashes, no section
signs, no arrows -- so that a copy can be byte-verbatim in every project,
including one whose own rules restrict the characters its files may
contain. Keep it that way when editing: a typographic character introduced
here becomes a transliteration problem in every repository that carries a
copy.

Where a copy must still be adapted, **"do not diverge" means semantically
identical, not byte-identical**: a project transliterating to satisfy its
own character-set rule, or renumbering a heading to fit its own structure,
is that project's rule working correctly, **not drift, and not something to
reconcile back**. What must match is every rule and every exception, in
substance.

**If you notice a copy diverging from the source, reconcile it as soon as
you notice** -- do not leave it for later and do not work around it. If the
divergence looks deliberate rather than stale, that is the conflict case
above: ask.

Noticing requires looking. **Re-read this source before writing or
reconciling any project's copy**, rather than working from what was loaded
at the start of the session -- it may have changed since, and a copy
reconciled against a stale source is drift being written rather than
fixed.

The project's `project.md` may state the three rules in brief and point
here for the detail. It does not restate the precedence rule.

## ASCII in source

Source and comments are ASCII. Write `--` where prose would use an em dash,
and "section" for a section sign.

This governs the text the repository writes about itself, not the data the
software handles. Documentation may use typographic punctuation; so may
user-facing text in UI software, and anything that genuinely requires
Unicode.

The check is `ascii_only` in `.style-gate.toml`, and **this project does not
turn it on**. The reason has changed, and the old one no longer holds.

Where a project does enforce it, the check is not a byte scan. In Python and
in C/C++ it enforces exactly the shape above -- ASCII outside string
literals, Unicode allowed inside them. Python is read with `tokenize`; C and
C++ get a scanner written for the purpose, nothing in the standard library
lexing them. Every other language still gets a whole-file byte check, having
no lexer here, and so does a file in either of those two that will not lex:
a file nobody can parse is not a file that has been cleared.

It used to be off here because the check could not tell a character table
from prose, and this is a terminal emulator whose subject matter is
character tables. The C/C++ scanner removes that reason, so the question was
measured rather than argued (2026-08-10, using the gate's own
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
exception covers "a terminal emulator's character tables", but it is written
about *data* and these are *comments*. Either the exception grows to cover a
comment that glosses data it sits beside, or those six get spelled some
other way, or the flag stays off. Seven characters is a small enough job
that the answer should be decided rather than inherited -- unlike the
sibling Qt tree, where the same count came to 854 across 163 files.

It was the whole file for everyone until a project that prints two status
ticks had to switch the check off to keep them, which switched it off for
its comments as well, and an em dash arrived in one. **An exception wider
than its reason is how a rule stops being enforced.**

## The commit-msg hook

The commit-msg hook is `tools/hooks/commit-msg`, installed with `make hooks`.
It rejects generator attribution, a subject over 75 columns, and body prose
over 75 columns. It lives in the tree rather than only in `.git/hooks` so
that it is reviewable and survives a clone; the copy that runs is installed
from it.

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
over text that never reaches the message -- the hook's own diff contains its
own pattern list, so it rejected every commit that edited it.

**Three shapes the length check exempts**, each because wrapping it is the
actual mistake rather than a concession: a *trailer*, since git parses the
block a line at a time and a broken `Link:` stops being a trailer at all; a
line holding a *url*, which no longer works once it is split; and an
*indented* line, which is how a message quotes a compiler error or a stack
trace, where reflowing what you are quoting corrupts the one thing it was
included for. It cannot tell prose opening `Note:` from a trailer, so it
forgives that -- the wrong way round would refuse a real trailer, and that
is the expensive error.

## See also

- **`project.md` §18** — the same rules in brief, plus the commit
  conventions (no docs-only commits; no AI-attribution trailers).
- **`project.md` §3** — the component names this document tells you to use.
- **`~/.claude/guidelines/code-style.md`** -- the source both this file and
  the sibling trees copy from.
- **`../fuzzypickles/code-style.md`** — the sibling tree's copy, which this
  one deliberately tracks.
