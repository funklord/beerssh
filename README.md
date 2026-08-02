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

Scrollback works — mouse wheel or `Shift+PageUp`/`PageDown`, capped at 5000
lines by default, keeping colours and attributes. Typing returns to the
live screen. The alternate screen is honoured, so `vim` and `less` leave
your shell output as they found it.

Mouse works: drag to select, double-click for a word (paths included),
`Ctrl+Shift+C` to copy, middle-click paste via the primary selection on
X11/Wayland. When a full-screen program asks for the mouse it gets the
clicks instead — hold `Shift` to select anyway.

Closing the window with a session still connected asks whether to keep it
running in the background or disconnect and quit, and remembers the answer.
On desktop "background" means a tray icon and a process that outlives its
window; the Android half of that — the foreground service — is **not
implemented**, and `platform_android.cpp` says so at the call site rather
than stubbing it to look finished.

Multiple sessions work: each lives in a registry rather than in a widget,
with a tab bar that appears once there is more than one. Switching tabs
re-points the view at another session's screen — nothing is copied, and a
session keeps running (and keeps receiving output) while nothing is
showing it.

Split panes follow the window, not the platform: under 600dp it is tabs
only, up to 840dp two panes, above that four. Narrowing the window
collapses the extra panes back to tabs **without closing or disconnecting
anything**, and widening restores the arrangement. The focused pane carries
a border whenever there is more than one — two identical shells side by
side is a real footgun otherwise.

Dropped connections retry on their own: exponential backoff, capped and
jittered so several sessions dropped by one network flap do not all
reconnect in the same millisecond, and an immediate retry when the network
changes (a laptop waking from suspend is the common case). A disconnect
you asked for is never retried.

**A reconnect is a new remote shell.** Plain SSH cannot resume one, so the
running program and working directory are gone; only the local scrollback
survives. Attaching to `tmux` on connect is the available answer until
Mosh is considered in v2.

Not built yet: SSH keepalive, reconnect status in the UI, pane navigation
shortcuts, layout persistence, per-tab status dots,
triple-click line selection, the Android background service, and the
tier-3 extensions in §7.1 (synchronized output, OSC 8 hyperlinks).

Host keys go in BeerSSH's own store (`~/.local/share/beerssh/known_hosts`),
never `~/.ssh/known_hosts`. An unknown or changed key fails the connection
and prints the fingerprint; the trust prompt that would let you accept one
from the UI is M6, so until then an entry gets added by hand.

See `project.md` §14 for the milestone plan.

## The interface

Two front doors over one set of saved hosts (`project.md` §8.5):

- **Desktop borrows PuTTY's layout** — category tree, destination at the
  top, saved sessions with Load/Save/Delete. Familiar without instructions
  if you have ever used PuTTY.
- **Touch gets a mobile list** — full-bleed rows, one tap to connect, a
  floating add button, kinetic scrolling.

Which appears follows the input device rather than the OS, so a
convertible in tablet mode gets the touch one. Hosts live in SQLite under
`AppDataLocation`; the schema has no column a credential could go in.

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
