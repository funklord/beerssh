#!/usr/bin/env python3
"""Convert leading indentation to tabs, keeping alignment as spaces.

Project code style (code-style.md sec 2, project.md sec 18): tabs for
indentation, spaces for alignment. Taken from the sibling fuzzypickles
tree, where both projects deliberately share one convention -- keep the
two copies in sync rather than letting them drift.

This rewrites each line's leading whitespace so that structural
brace-nesting indent is tabs and any further alignment (parameters under
an open paren, a block comment's `*` column, aligned trailing tokens) is
spaces after those tabs -- alignment then holds at any tab width because
it is measured from the shared leading tabs.

Model: structural indent = indent_level * INDENT columns -> indent_level
tabs; leading columns beyond that are alignment -> spaces. indent_level is
the brace-nesting depth, plus one for a `switch`'s `case`/`default` body
(C labels open no brace, but their bodies are indented one level deeper,
so that extra level is tracked explicitly and rendered as a tab too).
Braces inside strings, char literals, comments, and preprocessor lines are
not counted.

Content-preserving by construction: only LEADING whitespace is rewritten,
and the visual column of the first non-whitespace character is preserved
at tab width INDENT (a case body's level-N indent is N*INDENT columns,
which N tabs reproduce exactly). `--check` verifies precisely that
(expand-to-INDENT of the output equals expand-to-INDENT of the input) and
additionally reports files that are not already conformant.

Usage:
    tabify.py FILE...            # rewrite files in place
    tabify.py --check FILE...    # don't modify; exit 1 if any file is
                                 # not already tab-indented (or if the
                                 # conversion would ever change content)
"""
import re
import sys

INDENT = 2  # columns of indentation per structural nesting level

# a line (already stripped of leading whitespace) that is a switch label
_CASE_LABEL = re.compile(r"(?:case\b|default\s*:)")


def _is_case_label(rest):
    return _CASE_LABEL.match(rest) is not None


def _split_leading_ws(line):
    """Return (lead_cols, rest) where lead_cols is the visual column of the
    first non-whitespace char (tabs expanded at width INDENT) and rest is
    the remainder of the line. A blank/whitespace-only line yields rest ""."""
    col = 0
    i = 0
    for ch in line:
        if ch == " ":
            col += 1
        elif ch == "\t":
            col += INDENT - (col % INDENT)
        else:
            break
        i += 1
    return col, line[i:]


def convert(text):
    lines = text.split("\n")
    out = []
    # one frame per open brace `{`. is_switch: the block is a switch body;
    # in_case: a case/default label has appeared, so this switch body's
    # lines are one level deeper than the brace nesting.
    stack = []         # list of [is_switch, in_case]
    state = "normal"   # normal | block_comment | string | char
    pp_cont = False    # inside a backslash-continued preprocessor line
    pending_switch = False  # saw the `switch` keyword, awaiting its `{`
    ident = ""         # current identifier run (NORMAL state) for keyword detect

    def case_extra(frames):
        # each enclosing switch that is in a case body adds one indent level
        return sum(1 for f in frames if f[0] and f[1])

    for line in lines:
        state_at_start = state
        lead_cols, rest = _split_leading_ws(line)

        if rest == "":
            # blank / whitespace-only line: leave exactly as-is, no churn
            new = line
        elif state_at_start == "string":
            # continuation of a multi-line string literal: leading
            # whitespace is part of the string -- must not touch it
            new = line
        elif state_at_start == "normal" and not pp_cont and rest[0] == "#":
            # preprocessor directive: leave leading whitespace as-is
            new = line
        else:
            top_is_switch = bool(stack) and stack[-1][0]
            is_close = state_at_start == "normal" and rest[0] == "}"
            is_label = state_at_start == "normal" and top_is_switch and _is_case_label(rest)
            if is_close:
                # a leading close brace dedents to the enclosing level, and
                # is not itself inside the block it closes
                frames = stack[:-1]
                level = len(stack) - 1 + case_extra(frames)
            elif is_label:
                # a case/default label sits at the switch body's own level
                # (its own switch frame's case-extra does not apply to it),
                # but any OUTER switch case bodies still indent it
                frames = stack[:-1]
                level = len(stack) + case_extra(frames)
            else:
                frames = stack
                level = len(stack) + case_extra(frames)
            if level < 0:
                level = 0
            if lead_cols >= INDENT * level:
                new = "\t" * level + " " * (lead_cols - INDENT * level) + rest
            else:
                # indented less than its level (odd label/continuation):
                # tabify what's there without overrunning it
                new = "\t" * (lead_cols // INDENT) + " " * (lead_cols % INDENT) + rest
            if is_label:
                stack[-1][1] = True  # this switch body is now in a case body

        out.append(new)

        # ---- advance lexer state over this line's content ----
        counting = not (pp_cont or (state_at_start == "normal" and rest[:1] == "#"))
        i = 0
        n = len(line)
        while i < n:
            c = line[i]
            if state == "normal":
                if c.isalnum() or c == "_":
                    ident += c
                    i += 1
                    continue
                if counting and ident == "switch":
                    pending_switch = True  # keyword seen; its `{` starts a switch body
                ident = ""
                if c == "/" and i + 1 < n and line[i + 1] == "/":
                    break  # line comment: rest of line ignored
                if c == "/" and i + 1 < n and line[i + 1] == "*":
                    state = "block_comment"
                    i += 2
                    continue
                if c == '"':
                    state = "string"
                elif c == "'":
                    state = "char"
                elif counting and c == "{":
                    stack.append([pending_switch, False])
                    pending_switch = False
                elif counting and c == "}":
                    if stack:
                        stack.pop()
                elif counting and c == ";":
                    pending_switch = False  # a plain statement, not a switch block
                i += 1
            elif state == "block_comment":
                if c == "*" and i + 1 < n and line[i + 1] == "/":
                    state = "normal"
                    i += 2
                    continue
                i += 1
            else:  # string or char
                if c == "\\":
                    i += 2
                    continue
                if (state == "string" and c == '"') or (state == "char" and c == "'"):
                    state = "normal"
                i += 1

        # a `switch` keyword ending the line (before its `(...)` on the next)
        if state == "normal" and counting and ident == "switch":
            pending_switch = True
        ident = ""

        # an unterminated string/char without a trailing backslash reverts
        if state in ("string", "char") and not line.endswith("\\"):
            state = "normal"
        # preprocessor continuation across a backslash-newline
        if pp_cont or (state_at_start == "normal" and rest[:1] == "#"):
            pp_cont = line.endswith("\\")
        else:
            pp_cont = False

    return "\n".join(out)


def _expand(text):
    """Expand leading tabs to spaces at width INDENT, for content comparison."""
    out = []
    for line in text.split("\n"):
        cols, rest = _split_leading_ws(line)
        out.append(" " * cols + rest if rest != "" else line)
    return "\n".join(out)


def main(argv):
    check = False
    files = []
    for a in argv:
        if a == "--check":
            check = True
        elif a in ("-h", "--help"):
            print(__doc__)
            return 0
        else:
            files.append(a)
    if not files:
        print(__doc__, file=sys.stderr)
        return 2

    rc = 0
    for path in files:
        with open(path, "r", newline="") as f:
            src = f.read()
        dst = convert(src)
        # safety invariant: rewriting only leading whitespace must never
        # change content at tab width INDENT
        if _expand(dst) != _expand(src):
            print("ERROR: %s: conversion would change content (tool bug)" % path, file=sys.stderr)
            rc = 2
            continue
        if check:
            if dst != src:
                print("not tab-indented: %s" % path)
                rc = 1
        elif dst != src:
            with open(path, "w", newline="") as f:
                f.write(dst)
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
