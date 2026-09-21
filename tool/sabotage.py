#!/usr/bin/env python3
"""Break the code on purpose, one edit at a time, and report what noticed.

project.md sec 12.3 asks every test to be checked by making it fail: a test
that cannot fail is not evidence, and this tree has shipped several that
could not. Doing it by hand is three commands per assertion and an editor
window, which is why it gets skipped exactly when there is least time.

Last run 2026-08-14 against tool/sabotage/windows.toml: all seven edits
caught by the test each one names, from a 366-passing baseline, with the
tree back byte-for-byte afterwards.

This used to say the script had never executed, which was stale rather
than cautious. sec 12.3 of project.md records what earlier runs found,
and the fixes they produced are in the code below -- the mtime stamp in
restore(), the session-and-group kill, the checked spec. restore()'s own
comment describes what a run found while this paragraph was still saying
no run had happened.

Read it before trusting it anyway. What it does when it goes wrong is
leave the source deliberately broken, which is a cost paid by whoever
builds next rather than by whoever ran it.

WHAT THIS HAS TO GET RIGHT, because the hand method got each of them wrong
at least once in a single afternoon:

**The tree must come back.** A sabotage interrupted -- a full disk, a
killed job, a closed terminal -- leaves the source deliberately broken, and
the next person to build it is debugging a defect somebody introduced on
purpose and forgot. That happened twice. Three layers guard it: a finally
block for ordinary errors, SIGINT/SIGTERM handlers because Python does NOT
run finally blocks on a signal, and an on-disk journal for SIGKILL, which
no handler can catch. A later run refuses to start while a journal exists.

**A build failure is not a passing suite.** `make test` that fails to
compile prints no PASS lines and no FAIL lines, which is indistinguishable
from a green run if you only count failures -- so a sabotage would be
reported as "the suite stayed green", i.e. as a missing test, when in fact
nothing ran at all. That is the vacuous pass this project keeps finding
elsewhere, and the first version of this script had it.

**A hang must not be forever.** A suite run gets a deadline. Without one,
one wedged child holds the whole batch until somebody notices.

**Output must appear while it works.** A run is several minutes per edit.
Piping the first version through `tail` buffered every line until the end
and then the run was killed, so hours of work reported nothing at all.

Usage:
    tool/sabotage.py tool/sabotage/<spec>.toml [...]
    tool/sabotage.py --check [tool/sabotage/<spec>.toml ...]

--check is the cheap half: it reads the specs and the tree, builds
nothing, breaks nothing, and answers only whether every spec is still
aimed at something that exists. It is what `make style` runs. What it
cannot answer is whether an edit still BREAKS anything -- see the comment
above check_specs, which says so at length, because a green line from a
cheap check gets quoted for the expensive one's guarantee.

A spec is a list of edits, each naming the test that must notice it:

    [[sabotage]]
    name   = "the last tab may tear out"
    file   = "src/ui/main_window.cpp"
    find   = "if (m_shown.size() < 2) return nullptr;"
    expect = "a_torn_out_tab_becomes_a_window"

`replace` defaults to empty -- deleting the line is the commonest sabotage
by a wide margin -- and `expect` is matched against the failing test names,
so a partial name is enough.
"""
from __future__ import annotations

import functools
import re
import os
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Where originals are parked while an edit is applied. Under build/ because
# that is already ignored and already disposable; a journal that showed up
# in `git status` would be one more thing to explain.
JOURNAL = ROOT / "build" / "sabotage-restore"

# A suite run that has not finished in this long is wedged. Generous: the
# suite takes minutes on a loaded machine, and a deadline that fires on a
# slow run rather than a stuck one is worse than none.
RUN_DEADLINE_S = int(os.environ.get("BSSH_SABOTAGE_DEADLINE", "1800"))

# What counts as "the suite", so a spec whose covering test does not run
# under `make test` can be proved by the runner rather than by hand.
#
# ssh-send-window.toml is the case that asked for this. Its test lives in
# live_ssh_test, which QSKIPs without a server, so under `make test` the
# entry reports MISSED for ever -- and MISSED reads as a missing test.
# Its own comment said to run check-ssh by hand instead, which meant the
# one guard it covers had no way to be checked by the thing built to
# check guards.
#
#   BSSH_SABOTAGE_SUITE="make check-ssh" tool/sabotage.py \
#           tool/sabotage/ssh-send-window.toml
#
# The command has to BUILD as well as run, or the edit is never compiled
# in and every verdict describes the previous binary. `make test` does;
# `make check-ssh` does, through its $(TEST_BIN) prerequisite. A bare
# ./tool/check-ssh.sh does NOT, and would report a green suite over code
# that was never sabotaged -- the vacuous pass, from the one direction
# this tool is supposed to be immune to.
SUITE = shlex.split(os.environ.get("BSSH_SABOTAGE_SUITE", "make test"))


def say(*args: object) -> None:
	"""Prints and flushes.

	Every message here goes through this. The first version used bare
	print(), and piping it through `tail` held all of it in a buffer until
	the process ended -- which it did by being killed, so a run of nearly
	an hour reported nothing whatever.
	"""
	print(*args, flush=True)


class Edit:
	"""One deliberate break, and the test that is supposed to catch it."""

	def __init__(self, spec: dict, source: Path):
		self.name = spec["name"]
		self.path = ROOT / spec["file"]
		self.find = spec["find"]
		self.replace = spec.get("replace", "")
		self.expect = spec["expect"]
		self.source = source
		self.parked: Path | None = None

	def check(self) -> None:
		"""Refuses a spec that does not match EXACTLY ONCE.

		Presence used to be the only test, and apply() replaces the FIRST
		occurrence -- so a find string matching twice sabotaged whichever
		came first, which need not be the line the spec meant, and every
		verdict after it described an edit nobody asked for. A sabotage
		aimed at the wrong line is the vacuous pass in its most expensive
		form: it reports on a test that was never put under any pressure.

		The spec file's own comment already claimed each entry "is checked
		to match exactly once before the runner is started". It was not;
		this is that claim becoming true.

		THE FILE ONLY, deliberately, where problems_with() below also
		consults the build's patch set. The difference is not an
		oversight and is not worth harmonising away: --check answers
		"is this spec still aimed at something", which a patch supplies
		as truly as a file does, while THIS is about to edit the file on
		disk. An anchor that exists only in vterm-patch/ means the tree
		has not been patched, and running a sabotage against unpatched
		libvterm would measure the wrong library. Refusing is right.
		"""
		found = self.path.read_text().count(self.find)
		if found == 1:
			return

		problem = "is not in" if found == 0 else f"appears {found} times in"
		raise SystemExit(
			f"{self.source}: {self.name}: the text to break {problem} "
			f"{self.path.relative_to(ROOT)}.\n"
			f"  Looking for: {self.find[:70]}\n"
			f"  A spec that does not match exactly once is a sabotage that "
			f"tests nothing, or tests the wrong line, so this stops rather "
			f"than reporting a verdict."
		)

	def apply(self) -> None:
		self.check()
		text = self.path.read_text()

		# Journal first, edit second. The other order leaves a window in
		# which the file is broken and nothing on disk says what it was.
		JOURNAL.mkdir(parents=True, exist_ok=True)
		self.parked = JOURNAL / self.path.name
		shutil.copy2(self.path, self.parked)

		self.path.write_text(text.replace(self.find, self.replace, 1))

	def restore(self) -> bool:
		if self.parked is None:
			return True

		try:
			# The CONTENT, and only the content. copy2() also copies the
			# metadata, and copying metadata onto a file somebody else
			# owns raises EPERM -- after the bytes have already landed.
			# That aborted this function between the write and the
			# read-back below, so a restore that had in fact succeeded
			# was announced as "THE TREE IS NOT RESTORED".
			#
			# Measured here: the tree is checked out by one user and the
			# runner was invoked by another, which is ordinary in a
			# shared checkout and says nothing about whether the file
			# came back. git diff was empty and the parked copy was
			# byte-identical while the runner said the opposite.
			#
			# A safety message that cries wolf is worse than none: it
			# teaches whoever sees it next to skip the check, and the
			# real one looks exactly the same. So restoration is defined
			# as the bytes being back, which is what the read-back
			# decides -- ownership and timestamps are a courtesy and
			# cannot fail it.
			#
			# The destination already exists, so it keeps its own mode
			# and there is nothing to copy anyway.
			shutil.copyfile(self.parked, self.path)

			# The mtime still has to move, and for its own reason:
			# copyfile leaves it as it was, which puts the file BEHIND
			# the object built from the sabotaged text -- so make judges
			# that object current and the next `make test` silently runs
			# the sabotage against a tree that reads as clean. Found the
			# hard way: two tiling tests failed after a run whose git
			# status was empty, and the binary still held the last edit.
			#
			# Stamping to NOW rather than to the parked file's time,
			# which is both what the build system needs and the form
			# that works without owning the file: utime(path, None)
			# asks only for write permission, where an explicit time
			# demands ownership.
			os.utime(self.path, None)

			# Read it back. A copy that silently did not land -- a full
			# disk is the obvious way -- would leave the tree broken while
			# this reported success, which is the failure it exists to
			# prevent.
			if self.path.read_text() != self.parked.read_text():
				say(f"  !! {self.path} did not restore cleanly")
				return False

			self.parked.unlink()
			self.parked = None
			return True
		except OSError as error:
			say(f"  !! could not restore {self.path}: {error}")
			return False


# The suite run in flight, so a signal can kill its whole group rather
# than leaving it orphaned. None between runs.
current: subprocess.Popen | None = None


def kill_group(child: subprocess.Popen) -> None:
	"""Kills the suite and everything it started.

	The GROUP, not the process. subprocess's own timeout= kills only the
	direct child -- here `make` -- and the test binaries it spawned are
	reparented to init and keep running. Measured with a bounded probe
	rather than assumed: after the parent timed out, the grandchild was
	alive with PPID 1.

	That is not a tidiness point. An orphaned test binary holding a log
	file that has since been deleted is how a filesystem fills invisibly,
	and this tool exists to create exactly the conditions -- a deliberately
	broken build, a test that now hangs -- that produce one.
	"""
	try:
		group = os.getpgid(child.pid)
	except ProcessLookupError:
		return

	for number in (signal.SIGTERM, signal.SIGKILL):
		try:
			os.killpg(group, number)
		except ProcessLookupError:
			return
		try:
			child.wait(timeout=10)
			return
		except subprocess.TimeoutExpired:
			say(f"  !! the suite ignored signal {number}; escalating")


def run_suite() -> tuple[bool, int, list[str], int]:
	"""Runs the suite: (did it run at all, passes, failing test names).

	The first element is the one that matters. `make test` that fails to
	build prints neither PASS nor FAIL, which counts identically to a green
	run if you only look at failures -- so a sabotage would be reported as
	uncaught when in truth nothing was ever executed.
	"""
	global current

	with tempfile.NamedTemporaryFile("w+", suffix=".log") as log:
		# Its own session, so the deadline can kill the whole group. This
		# also detaches it from the terminal's foreground group, which is
		# why on_signal below has to pass a Ctrl-C on rather than trusting
		# the child to receive one.
		current = subprocess.Popen(SUITE, cwd=ROOT, stdout=log,
		                            stderr=subprocess.STDOUT, start_new_session=True)
		try:
			current.wait(timeout=RUN_DEADLINE_S)
		except subprocess.TimeoutExpired:
			say(f"  !! the suite did not finish within {RUN_DEADLINE_S}s")
			kill_group(current)
			current = None
			return False, 0, [], 0

		current = None

		log.seek(0)
		text = log.read()

	passes = 0
	skips = 0
	failing = []
	for line in text.splitlines():
		if line.startswith("PASS"):
			passes += 1
		elif line.startswith("SKIP"):
			# A skipped test asserts nothing and reports no failure, so a
			# sabotage that turns one INTO a skip is indistinguishable
			# here from a sabotage nothing noticed -- and it is the worse
			# outcome, because the test is still in the suite claiming to
			# cover the thing it no longer touches. Counted so that the
			# verdict below can say which of the two happened.
			skips += 1
		elif line.startswith("FAIL!"):
			# "FAIL!  : suite::test_name() ..." -- the name a spec matches.
			part = line.split(":", 2)[-1].strip()
			failing.append(part.split("(")[0].strip())

	return (passes + len(failing)) > 0, passes, failing, skips


def restore_all(edits: list[Edit]) -> bool:
	clean = True
	for edit in edits:
		if not edit.restore():
			clean = False
	return clean


def on_signal(edits: list[Edit], number: int, frame: object) -> None:
	"""Puts the tree back before dying.

	Python runs no finally block when a signal terminates the process, so
	without this a `timeout`, a Ctrl-C or a kill leaves the source broken.
	Believing otherwise is why an interrupted run was reported as having
	restored itself when it had merely happened to be between edits.
	"""
	del frame
	say(f"\nsabotage: signal {number} -- restoring before exit")

	# The suite runs in its own session now, so a Ctrl-C aimed at this
	# process never reaches it. Pass it on, or the tree is restored while
	# a suite carries on building the version that no longer exists.
	if current is not None:
		kill_group(current)

	restore_all(edits)
	sys.exit(128 + number)



# --------------------------------------------------------------------------
# The cheap half: is every spec still aimed at something.
#
# A sabotage that no longer applies is indistinguishable from a check that
# cannot fail, and nothing in a build or a test run says so. Fifteen of
# this tree's anchors had rotted that way before anybody looked (sec 12.3),
# and finding them cost a hand-written script somebody had to think of.
# Running the specs would have found them too, at one suite run per edit --
# which is why nobody did.
#
# WHAT THIS ESTABLISHES: every spec parses, names a file that exists, has a
# find string matching exactly once, and names a test the suite declares.
#
# WHAT IT DOES NOT, and the distinction is the whole reason to write it
# down rather than let a green line be quoted for more than it bought:
#
#   - Whether the edit CHANGES anything a test can see. sec 12.3 records
#     one that matched its anchor for months and broke nothing, because
#     the call it deleted ran on a path no test reaches.
#   - Whether the named test COVERS the defect, or even runs on this
#     machine. Both report as MISSED, which reads like a hole in the code
#     and is not.
#   - Whether the `replace` half is right. Two were wrong while their
#     anchors matched perfectly.
#
# Only running the spec answers those, and this does not replace it.
# --------------------------------------------------------------------------


def declared_tests(root: Path = ROOT) -> set[str]:
	"""Every test method the suite headers declare.

	Read out of the headers rather than out of a built binary, so this
	works on a tree nobody has compiled -- which is the state a cheap
	gate has to run in if it is going to run at all.
	"""
	names: set[str] = set()
	for header in sorted((root / "test").glob("*.h")):
		for line in header.read_text().splitlines():
			stripped = line.strip()
			if not stripped.startswith("void ") or not stripped.endswith("();"):
				continue
			names.add(stripped[len("void "):-len("();")].strip())
	return names


def patch_additions(root: Path, vendored: str) -> list[str]:
	"""What `<vendored>-patch/*.patch` ADDS, as contiguous runs of lines.

	Runs rather than one blob, and this is the whole care in the function:
	joining every added line into one string would let an anchor spanning
	two unrelated hunks match, which is a false PASS -- the direction that
	costs something. Contiguous added lines land contiguous in the file,
	so a run is exactly what a multi-line anchor can legitimately match.
	"""
	runs: list[str] = []
	current: list[str] = []

	for patch in sorted((root / f"{vendored}-patch").glob("*.patch")):
		for line in patch.read_text().splitlines():
			if line.startswith("+") and not line.startswith("+++"):
				current.append(line[1:])
				continue
			if current:
				runs.append("\n".join(current))
			current = []
		if current:
			runs.append("\n".join(current))
		current = []

	return runs


def skippable_tests(root: Path = ROOT) -> set[str]:
	"""Test methods whose body can QSKIP.

	Named because of the runner's third failure mode, which sec 12.3 calls
	invisible until a spec is executed: an `expect` naming a test that does
	not RUN reports MISSED, and MISSED reads like a hole in the code's
	tests. It is not one -- it is a guard that did not fire on this
	machine -- and nothing in the verdict says which.

	This cannot say whether a guard WILL fire, and does not try: it says
	that one exists, which is enough to stop a MISSED being read as a
	finding without a second look. Reported rather than refused for that
	reason; failing on it would be a false finding on every run.
	"""
	names: set[str] = set()

	for source in sorted((root / "test").glob("*.cpp")):
		text = source.read_text()
		# [before, class, name, body, class, name, body, ...]
		parts = re.split(r"\nvoid\s+(\w+)::(\w+)\s*\(\s*\)\s*\{", text)
		for i in range(1, len(parts) - 2, 3):
			if "QSKIP" in parts[i + 2]:
				names.add(parts[i + 1])

	return names


def problems_with(entry: dict, source: Path, declared: set[str],
                   root: Path = ROOT) -> list[str]:
	"""What is wrong with one spec entry, or an empty list.

	Every arm returns rather than raising, because the caller wants ALL
	of them: a checker that stops at the first bad anchor turns one sweep
	into one fix per run, which is how a rotted set stays rotted.
	"""
	found: list[str] = []

	for field in ("name", "file", "find", "expect"):
		if field not in entry:
			found.append(f"has no `{field}`")
	if found:
		return found

	path = root / entry["file"]
	if not path.is_file():
		return [f"names {entry['file']}, which is not a file"]

	count = path.read_text().count(entry["find"])

	# A vendored tree is patched at BUILD time, so where this runs decides
	# what is in the file. Five anchors live in text vterm-patch/ adds, and
	# `make style` runs before `make` -- so on a developer machine they are
	# in state.c and in the container, which starts from the submodule at
	# its gitlink commit, they are not. Measured: 1 and 0 respectively.
	#
	# Skipping them there would be the vacuous pass this whole check exists
	# against, so the fallback ASKS THE PATCH SET instead: an anchor absent
	# from the file must be supplied by exactly one of the runs those
	# patches add. That is a real check in both states rather than an
	# excuse in one, and it is strictly stronger than reading the file --
	# a patch rewritten without updating the spec goes to zero in both.
	#
	# The file wins when it has the text, or a patched tree would count it
	# twice and report a duplicate that is one line seen from two sides.
	from_patch = 0
	vendored = entry["file"].split("/", 1)[0]
	if count == 0 and (root / f"{vendored}-patch").is_dir():
		from_patch = sum(1 for run in patch_additions(root, vendored)
		                  if entry["find"] in run)

	if count != 1 and from_patch != 1:
		if count == 0 and from_patch == 0:
			was = f"is in neither {entry['file']} nor {vendored}-patch/"
		elif count == 0:
			was = f"is added {from_patch} times by {vendored}-patch/"
		else:
			was = f"appears {count} times in {entry['file']}"
		found.append(f"the text to break {was}")

	# A replace identical to the find is an edit that applies cleanly and
	# changes nothing, so its verdict is MISSED for ever and reads as a
	# hole in the code's tests. There are none today; the arm is here so
	# that one arriving fails as a message to whoever added it rather
	# than being absorbed by a condition that would pass it.
	#
	# Deliberately EXACT equality and nothing cleverer. `replace`
	# containing `find` is the normal way to write a break -- six entries
	# prepend an `if(0)` or a guard and keep the original text -- so a
	# containment test would report six false findings, and a gate with
	# false findings is one somebody switches off by instalments.
	if entry.get("replace", "") == entry["find"]:
		found.append("replaces the text to break with itself, so the edit "
		              "changes nothing and can never fail")

	# Matched the way the runner matches it: `expect` is a substring of a
	# failing test's name, and a spec may spell that qualified or bare.
	wanted = entry["expect"].rsplit("::", 1)[-1]
	if not any(wanted in name for name in declared):
		found.append(f"expects `{entry['expect']}`, which no test declares")

	return found


def control(declared: set[str]) -> bool:
	"""Proves the checker can speak, before it is believed when silent.

	The corpus is expected to be clean, so a run over it reports nothing
	whether the checker works or not -- `evidence.md`'s static detector
	over an all-clean corpus, which is a green light with no demonstrated
	ability to be anything else. These fixtures are deliberately wrong in
	each of the four ways, plus one that is right, and every verdict is
	asserted before a real spec is read.

	Inside the tool rather than beside it, because a control that lives
	beside a tool is run when somebody remembers.
	"""
	good = sorted(declared)[0] if declared else ""

	with tempfile.TemporaryDirectory(prefix="sabotage-control-") as where:
		root = Path(where)
		(root / "test").mkdir()
		(root / "src").mkdir()
		(root / "src" / "thing.cpp").write_text("alpha\nbeta\nalpha\ngamma\n")

		# A vendored tree as the container gets it -- pristine, with the
		# build's patches not yet applied -- so the fallback that reads
		# them is exercised rather than assumed. Without this the five
		# real vterm anchors would be the only thing that ever ran it,
		# and they pass, which is the state a control exists to refuse.
		(root / "vend").mkdir()
		(root / "vend" / "file.c").write_text("upstream line\n")
		(root / "vend-patch").mkdir()
		(root / "vend-patch" / "0001-x.patch").write_text(
			"--- a/file.c\n+++ b/file.c\n@@\n"
			" upstream line\n+ours first\n+ours second\n")

		spec = Path("control.toml")
		cases = [
			("a sound entry", {
				"name": "x", "file": "src/thing.cpp",
				"find": "gamma", "expect": good,
			}, False),
			("a missing file", {
				"name": "x", "file": "src/gone.cpp",
				"find": "gamma", "expect": good,
			}, True),
			("an anchor that matches nothing", {
				"name": "x", "file": "src/thing.cpp",
				"find": "delta", "expect": good,
			}, True),
			("an anchor that matches twice", {
				"name": "x", "file": "src/thing.cpp",
				"find": "alpha", "expect": good,
			}, True),
			("an expect naming no test", {
				"name": "x", "file": "src/thing.cpp",
				"find": "gamma",
				"expect": "no_test_is_called_this_and_none_ever_will_be",
			}, True),
			("an entry missing a field", {
				"name": "x", "file": "src/thing.cpp", "find": "gamma",
			}, True),
			("an anchor the build's patches supply", {
				"name": "x", "file": "vend/file.c",
				"find": "ours first\nours second", "expect": good,
			}, False),
			("an anchor in neither the file nor the patches", {
				"name": "x", "file": "vend/file.c",
				"find": "ours third", "expect": good,
			}, True),
			("a replace that changes nothing", {
				"name": "x", "file": "src/thing.cpp",
				"find": "gamma", "replace": "gamma", "expect": good,
			}, True),
			("a replace that keeps the text and adds to it", {
				"name": "x", "file": "src/thing.cpp",
				"find": "gamma", "replace": "if(0) gamma", "expect": good,
			}, False),
		]

		for label, entry, should_complain in cases:
			complained = bool(problems_with(entry, spec, declared, root))
			if complained != should_complain:
				say(f"sabotage: THE CONTROL FAILED -- {label} was "
				     f"{'not ' if should_complain else ''}reported.")
				say("sabotage:   No result below would mean anything, so "
				     "nothing was checked.")
				return False

	return True


def check_specs(paths: list[Path]) -> int:
	"""Checks every spec and says what it checked. Non-zero on any problem."""
	declared = declared_tests()
	if not declared:
		say("sabotage: no test declarations found under test/ -- the expect "
		     "check would pass everything, so this refuses rather than "
		     "reporting a clean sweep.")
		return 2

	if not control(declared):
		return 2

	edits = 0
	bad: list[str] = []

	# (file, find, replace) -> where it came from. Two entries breaking
	# exactly the same thing spend two suite runs to learn one fact, and
	# the second is usually a copy somebody meant to re-aim. None today.
	seen: dict[tuple[str, str, str], str] = {}

	# Kept so the skip cross-reference below can be made over exactly the
	# entries that were checked, rather than over a second walk of the
	# directory that could disagree with this one.
	entries_seen: list[tuple[Path, dict]] = []

	for path in paths:
		try:
			spec = tomllib.loads(path.read_text())
		except (OSError, tomllib.TOMLDecodeError) as error:
			bad.append(f"{path}: will not parse: {error}")
			continue

		entries = spec.get("sabotage", [])
		if not entries:
			bad.append(f"{path}: holds no [[sabotage]] entry")
			continue

		for entry in entries:
			edits += 1
			entries_seen.append((path, entry))
			where = f"{path}: {entry.get('name', '<unnamed>')}"

			for problem in problems_with(entry, path, declared):
				bad.append(f"{where}: {problem}")

			if all(field in entry for field in ("file", "find")):
				target = (entry["file"], entry["find"], entry.get("replace", ""))
				if target in seen:
					bad.append(f"{where}: breaks exactly what "
					            f"{seen[target]} already breaks")
				else:
					seen[target] = where

	for line in bad:
		say(f"sabotage: {line}")

	if bad:
		say(f"sabotage: {len(bad)} problem(s) in {len(paths)} spec(s), "
		     f"{edits} edit(s)")
		return 1

	say(f"sabotage: {len(paths)} spec(s), {edits} edit(s): every anchor "
	     f"matches once and every expect names a declared test")
	say("sabotage:   which does NOT say the edits still break anything -- "
	     "only running them says that")

	# The one thing that CAN be said in advance about the runner's third
	# failure mode. Reported, never failed on: a guard that exists is not
	# a guard that fires, and refusing here would be a false finding on
	# every run.
	skippable = skippable_tests()
	if not skippable:
		say("sabotage:   no QSKIP found anywhere under test/, which is not "
		     "credible -- the skip cross-reference below is not being made")
		return 2

	ambiguous = sorted(
		f"{path.name}: {entry.get('name', '<unnamed>')}"
		for path, entry in entries_seen
		if any(entry.get("expect", "").rsplit("::", 1)[-1] in name
		        for name in skippable)
	)
	if ambiguous:
		say(f"sabotage:   {len(ambiguous)} of them name a test that can "
		     f"QSKIP, so a MISSED there may be the guard rather than a gap:")
		for line in ambiguous:
			say(f"sabotage:     {line}")

	return 0


def main(argv: list[str]) -> int:
	if len(argv) < 2:
		say(__doc__)
		return 2

	# --check reads and builds nothing, so it is safe to run anywhere and
	# is what `make style` calls. With no specs named it takes the whole
	# directory, because a checker somebody has to list the files for is a
	# checker that goes out of date with the directory.
	if argv[1] == "--check":
		named = [Path(name) for name in argv[2:]]
		return check_specs(named or sorted((ROOT / "tool" / "sabotage").glob("*.toml")))

	if JOURNAL.exists() and any(JOURNAL.iterdir()):
		say(f"sabotage: {JOURNAL.relative_to(ROOT)} still holds originals from "
		     f"a run that did not finish.")
		say("sabotage:   The tree may still be sabotaged. Copy them back, or "
		     "check them against git, before running anything here.")
		for parked in sorted(JOURNAL.iterdir()):
			say(f"sabotage:     {parked.name}")
		return 2

	edits: list[Edit] = []
	for name in argv[1:]:
		path = Path(name)
		spec = tomllib.loads(path.read_text())
		for entry in spec.get("sabotage", []):
			edits.append(Edit(entry, path))

	if not edits:
		say("sabotage: no edits in the given specs")
		return 2

	# Every spec checked before anything is built or broken. Checking at
	# edit time instead means a stale find string surfaces at edit five,
	# having already spent five suite runs to get there -- and two entries
	# in windows.toml have been rewritten for exactly that staleness, so
	# this is the common case rather than the paranoid one.
	for edit in edits:
		edit.check()

	for number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
		signal.signal(number, functools.partial(on_signal, edits))

	# Named, because a verdict means something different under a different
	# suite and a report that does not say which is a report nobody can
	# re-take. sec 12.3's rule about recording the method beside the fact.
	say(f"sabotage: {len(edits)} edit(s); one run of `{' '.join(SUITE)}` "
	     f"each, plus a baseline")

	ran, baseline_passes, baseline_failing, baseline_skips = run_suite()
	if not ran:
		say("sabotage: the suite did not run at all -- a build failure, most "
		     "likely. Nothing below would mean anything.")
		return 2
	if baseline_failing:
		say(f"sabotage: the suite ALREADY fails ({len(baseline_failing)}): "
		     f"{', '.join(baseline_failing[:3])}")
		say("sabotage:   nothing here means anything until it is green.")
		return 1
	# The skips are printed even when there are none, because the useful
	# moment for this number is when somebody compares two runs: a
	# baseline that quietly counts one test fewer than the suite's own
	# Totals is a test that skipped, and without this the only way to
	# learn that is to notice the arithmetic disagreeing.
	say(f"  baseline: {baseline_passes} passing, {baseline_skips} skipped")

	results: list[tuple[str, str, str]] = []
	clean = True

	for edit in edits:
		say(f"  breaking: {edit.name}")
		try:
			edit.apply()
			ran, _, failing, skips = run_suite()

			if not ran:
				# The sabotage stopped it compiling. That is not a test
				# noticing anything, and reporting it as one would be a
				# lie in the direction of comfort.
				verdict, detail = "NO BUILD", "the edit broke the build"
			elif any(edit.expect in name for name in failing):
				verdict, detail = "caught", f"{len(failing)} failed"
			elif failing:
				verdict = "WRONG TEST"
				detail = f"{len(failing)} failed: {', '.join(failing[:2])}"
			elif skips > baseline_skips:
				# Nothing failed AND a test stopped running. Reporting
				# this as a plain MISSED would hide the more alarming
				# half: the guard is still listed in the suite and has
				# quietly stopped asserting anything.
				verdict = "MISSED"
				detail = (f"the suite stayed green and {skips - baseline_skips} "
				           f"more test(s) SKIPPED than at baseline")
			else:
				verdict, detail = "MISSED", "the suite stayed green"

			results.append((edit.name, verdict, detail))
			say(f"    {verdict}: {detail}")
		finally:
			if not edit.restore():
				clean = False

	width = max(len(name) for name, _, _ in results)
	say("")
	for name, verdict, detail in results:
		say(f"  {name.ljust(width)}  {verdict:<10}  {detail}")

	missed = [name for name, verdict, _ in results if verdict != "caught"]
	say("")
	if missed:
		say(f"sabotage: {len(missed)} of {len(results)} not caught by the test "
		     f"that claimed to cover it")
	else:
		say(f"sabotage: all {len(results)} caught")

	if not clean:
		say("sabotage: THE TREE IS NOT RESTORED -- check git status before "
		     "building")
		return 2

	if JOURNAL.exists() and not any(JOURNAL.iterdir()):
		JOURNAL.rmdir()

	return 1 if missed else 0


if __name__ == "__main__":
	sys.exit(main(sys.argv))
