# BeerSSH

An SSH terminal for **Android and desktop** — background sessions, tabs,
adaptive splits, and a real keyboard. Qt Widgets, C++, one codebase for
both platforms.

Status: **M0–M3 done bar credentials.** The app connects over SSH,
authenticates with a password, opens a PTY channel, and renders the result
through a real VT emulator — colours, attributes, cursor addressing,
scrolling and CJK double-width cells all work:

```sh
./beerssh user@host        # asks for a password, then shows the shell
```

Typing works: keys, modifiers, function keys, arrows in both normal and
application-cursor mode, Meta/Alt, bracketed paste (`Ctrl+Shift+V`), and
input-method commit text. The on-screen key row for touch devices is
built on every platform but hidden unless touch is the primary input.

Modern keyboard protocols are supported so full-screen programs can tell
`Ctrl+I` from `Tab` and `Alt+x` from `Escape`-then-`x`: the **kitty keyboard
protocol** (disambiguate tier, with push/pop/set/query) and **xterm
`modifyOtherKeys` level 2**. `project.md` §7.1 surveys the wider set of
modern terminal extensions and says where each one stands.

Inline images work for all three protocols: the **kitty graphics
protocol**, **iTerm2 inline images** and **sixel** — `kitten icat`,
`imgcat` and `img2sixel` style output all render. Needs `libsixel-dev`
(Debian/Ubuntu) alongside Qt and libssh.

Not built yet: mouse selection and copy (`Ctrl+Shift+C` is wired but has
nothing to copy until then), scrollback, tabs and splits, and the tier-3
extensions in §7.1 (synchronized output, OSC 8 hyperlinks).

Host keys go in BeerSSH's own store (`~/.local/share/beerssh/known_hosts`),
never `~/.ssh/known_hosts`. An unknown or changed key fails the connection
and prints the fingerprint; the trust prompt that would let you accept one
from the UI is M6, so until then an entry gets added by hand.

See `project.md` §14 for the milestone plan.

## Build (desktop)

Needs Qt 6 (6.8 LTS or newer), libssh, and a C++17 compiler.
On Debian/Ubuntu: `qt6-base-dev libssh-dev libsixel-dev`.

libvterm is vendored as a submodule and built from source, so clone with
`--recurse-submodules` (or run `git submodule update --init`). The build
applies the patches in `vterm-patches/` to it; `make veryclean` reverts
them. See `project.md` §7.1 for what they fix and why they are not
committed into the submodule.

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
src/term/            bssh_screen + the libvterm-backed emulator
src/input/           bssh_input_router -- every keystroke becomes bytes here
vterm/               vendored libvterm (submodule, MIT, upstream untouched)
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
