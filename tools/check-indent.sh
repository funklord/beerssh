#!/bin/sh
# Check that this project's own C++ sources follow the indentation style
# (code-style.md sec 2): tabs for indentation, spaces for alignment.
#
# Runs tools/tabify.py --check over every .cpp/.h this project owns. Prints
# each non-conformant file and exits non-zero if any are found, so it works
# as a CI gate and as a pre-commit hook; "fix" the reported paths by
# re-running tools/tabify.py without --check (or `make -C tools fix`).
#
# Adapted from the sibling fuzzypickles tree. The one real difference: that
# copy enumerates files with `git ls-files`, which needs a git repo and
# gains the ability to skip vendored submodules and ignored build output.
# Here the source tree is small, self-contained and not necessarily a repo
# yet, so it walks src/ directly -- but it prefers git when there IS a repo,
# so that ignored build output and any future vendored subtree drop out of
# the list automatically rather than being re-excluded by hand.
set -e

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(cd -- "$here/.." && pwd)
cd -- "$root"

if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
	# --cached AND --others: tracked files plus untracked-but-not-ignored
	# ones. Checking only tracked files silently exempts every NEW file
	# until after it has been committed, which is exactly backwards for a
	# pre-commit gate.
	files=$(git ls-files --cached --others --exclude-standard \
		'src/*.cpp' 'src/*.h' 'tests/*.cpp' 'tests/*.h')
else
	files=$(find src tests -name '*.cpp' -o -name '*.h')
fi

# An empty or implausibly short list is a FAILURE, not a pass.
#
# "Found nothing" cannot mean "there is nothing to check" in a tree that
# has sources -- it can only mean the pathspec, the glob or the file walk
# stopped matching. Reported as success, that is the worst outcome
# available: a gate that has quietly stopped looking is indistinguishable
# from a clean tree, and stays that way until someone thinks to doubt it.
#
# The floor is deliberately below the real count and is not meant to track
# it. It exists to catch a list that has collapsed, not to police a number
# -- raise it when it stops being obviously safe, not every time a file is
# added.
#
# `|| true` because grep -c exits 1 on a count of zero, which under set -e
# would abort here with no message at all: failing for the right reason and
# saying nothing about it, which is most of what this block exists to stop.
count=$(printf '%s\n' $files | grep -c . || true)
if [ "$count" -lt 5 ]; then
	echo "check-indent: found $count sources, expected more --"
	echo "check-indent:   the file list is broken, not the tree."
	echo "check-indent:   check the pathspec in the file walk above."
	exit 1
fi

# shellcheck disable=SC2086  # word-splitting the file list is intended here
if python3 "$here/tabify.py" --check $files; then
	echo "check-indent: all $count sources are tab-indented"
else
	rc=$?
	echo "check-indent: some sources are not tab-indented (see above)"
	echo "check-indent: fix with:  python3 tools/tabify.py <paths>"
	exit "$rc"
fi
