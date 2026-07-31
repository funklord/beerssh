# BeerSSH

An SSH terminal for **Android and desktop** — background sessions, tabs,
adaptive splits, and a real keyboard. Qt Widgets, C++, one codebase for
both platforms.

Status: **M0 done, M1 all but complete.** The app connects over SSH,
authenticates with a password, opens a PTY channel and renders what the
remote sends:

```sh
./beerssh user@host        # asks for a password, then shows the shell
```

Two limits worth knowing before you try it. **You cannot type yet** — the
input router is M3, so this is a read-only view of the remote output. And
output is rendered as plain text with escape sequences dropped, because the
VT emulator is M2; expect a colourless, uncontrolled-cursor approximation
of a terminal, which is exactly what M1 promises and no more.

Host keys go in BeerSSH's own store (`~/.local/share/beerssh/known_hosts`),
never `~/.ssh/known_hosts`. An unknown or changed key fails the connection
and prints the fingerprint; the trust prompt that would let you accept one
from the UI is M6, so until then an entry gets added by hand.

See `project.md` §14 for the milestone plan.

## Build (desktop)

Needs Qt 6 (6.8 LTS or newer), libssh, and a C++17 compiler.
On Debian/Ubuntu: `qt6-base-dev libssh-dev`.

```sh
make          # build ./beerssh
make run      # build and run it
make test     # smoke test + unit tests (offscreen QPA, no server needed)
make style    # indentation gate
```

`make` wraps qmake, which does the actual Qt build in `build/`; everything
else in the tree is a plain Makefile. `make help` lists the targets.

## Build (Android)

Needs a Qt-for-Android kit and an NDK, neither of which the desktop build
requires:

```sh
make android QT_ANDROID_ROOT=$HOME/Qt/6.8.2/android_arm64_v8a
```

This path has not been run yet — the M0 scaffold was written on a machine
with no NDK, so expect the first Android build to need fixing rather than
just working. `android/AndroidManifest.xml` in particular must be checked
against the installed kit's template.

## Layout

```
beerssh.pro          qmake project — the app, both platforms
Makefile             top-level: wraps qmake, plus everything non-Qt
src/main.cpp         entry point (--self-test is what CI runs)
src/ui/              widgets: main window, terminal grid
src/ssh/             bssh_ssh_session -- the SSH transport, behind an interface
src/platform/        bssh_platform + one implementation per platform
tests/               host unit tests + the fake SSH session they drive
android/             AndroidManifest.xml
tools/               indentation gate; font validation lands at M9
```

## Documents

- **`project.md`** — the specification and the source of truth. Read §3
  (architecture and its three core principles) before writing code.
- **`code-style.md`** — snake_case, tabs, lowercase filenames, and why
  `clang-format` is not used here.
