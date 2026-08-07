#!/usr/bin/env python3
"""Break the code on purpose, one edit at a time, and report what noticed.

project.md sec 12.3 asks every test to be checked by making it fail: a test
that cannot fail is not evidence, and this tree has shipped several that
could not. Doing it by hand is three commands per assertion and an editor
window, which is why it gets skipped exactly when there is least time.

NOT YET RUN. Neither this version nor the one before it has ever executed.
Read it before trusting it.

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
    tools/sabotage.py tools/sabotage/<spec>.toml [...]

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
import os
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
RUN_DEADLINE_S = 1800


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

	def apply(self) -> None:
		text = self.path.read_text()
		if self.find not in text:
			raise SystemExit(
				f"{self.source}: {self.name}: the text to break is not in "
				f"{self.path.relative_to(ROOT)}.\n"
				f"  Looking for: {self.find[:70]}\n"
				f"  A spec that no longer matches is a sabotage that tests "
				f"nothing, so this stops rather than reporting a pass."
			)

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
			shutil.copy2(self.parked, self.path)

			# copy2 restores the mtime along with the mode, which puts the
			# file BEHIND the object built from the sabotaged text -- so
			# make judges that object current and the next `make test`
			# silently runs the sabotage against a tree that reads as
			# clean. Found the hard way: two tiling tests failed after a
			# run whose git status was empty, and the binary still held
			# the last edit. Stamp it now, so the restore is a change as
			# far as the build system is concerned.
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


def run_suite() -> tuple[bool, int, list[str]]:
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
		current = subprocess.Popen(["make", "test"], cwd=ROOT, stdout=log,
		                            stderr=subprocess.STDOUT, start_new_session=True)
		try:
			current.wait(timeout=RUN_DEADLINE_S)
		except subprocess.TimeoutExpired:
			say(f"  !! the suite did not finish within {RUN_DEADLINE_S}s")
			kill_group(current)
			current = None
			return False, 0, []

		current = None

		log.seek(0)
		text = log.read()

	passes = 0
	failing = []
	for line in text.splitlines():
		if line.startswith("PASS"):
			passes += 1
		elif line.startswith("FAIL!"):
			# "FAIL!  : suite::test_name() ..." -- the name a spec matches.
			part = line.split(":", 2)[-1].strip()
			failing.append(part.split("(")[0].strip())

	return (passes + len(failing)) > 0, passes, failing


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


def main(argv: list[str]) -> int:
	if len(argv) < 2:
		say(__doc__)
		return 2

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

	for number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
		signal.signal(number, functools.partial(on_signal, edits))

	say(f"sabotage: {len(edits)} edit(s); one suite run each, plus a baseline")

	ran, baseline_passes, baseline_failing = run_suite()
	if not ran:
		say("sabotage: the suite did not run at all -- a build failure, most "
		     "likely. Nothing below would mean anything.")
		return 2
	if baseline_failing:
		say(f"sabotage: the suite ALREADY fails ({len(baseline_failing)}): "
		     f"{', '.join(baseline_failing[:3])}")
		say("sabotage:   nothing here means anything until it is green.")
		return 1
	say(f"  baseline: {baseline_passes} passing")

	results: list[tuple[str, str, str]] = []
	clean = True

	for edit in edits:
		say(f"  breaking: {edit.name}")
		try:
			edit.apply()
			ran, _, failing = run_suite()

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
