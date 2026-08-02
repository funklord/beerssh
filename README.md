# BeerSSH

An SSH terminal for **Android and desktop** — background sessions, tabs,
adaptive splits, and a real keyboard. Qt Widgets, C++, one codebase for
both platforms.

Status: **verified against a real host.** The app connects over SSH,
authenticates with a password, opens a PTY channel, and renders the result
through a real VT emulator — colours, attributes, cursor addressing,
scrolling and CJK double-width cells all work:

```sh
./beerssh user@host        # tries your SSH key, asks for a password only if it fails
```

Public-key authentication is tried first — the agent, then your default
identities — so a key-authenticated session holds no credential at all and
reconnects on its own after a network drop. If a key exists but does not
get in unaided it offers to unlock it, because for a server that only
accepts keys a password prompt is not a fallback but a prompt that cannot
succeed. The passphrase is used and not kept.

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
triple-click for a whole line,
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

`Ctrl+Alt+Left`/`Right` move focus between panes, and the status line
reports reconnection as it happens rather than leaving a silent terminal
that looks identical to a quiet one.

Each tab carries a status dot — connected, connecting, disconnected, or a
bell that rang while you were looking at another tab. Dragging a divider
snaps to whole character cells, so no pane renders half a glyph at its
edge.

Which hosts were open, how they were arranged and which was active are
remembered across restarts. The connections are not — plain SSH cannot
resume a shell — so the front door reopens with the last host selected
rather than pretending to have restored anything.

Not built yet: SSH keepalive (see `project.md` §4 — libssh turns out not
to expose one), reopening every remembered host at once (needs key auth),
triple-click line selection, the Android background service, and the
tier-3 extensions in §7.1 (synchronized output, OSC 8 hyperlinks).

A password can be remembered per host — off by default, and only for a
saved host, since the secret is keyed to it. Remembered passwords are
replayed on reconnect, so a password-authenticated session survives a
network drop without holding the credential in memory the whole time.

Credentials go to the **system keyring** (GNOME Keyring, KWallet, anything
speaking Secret Service) when one is actually running — no extra
passphrase, since your login session already unlocked it. Where none
answers, they go to a passphrase-encrypted file
(`~/.local/share/beerssh/secrets.enc`, PBKDF2 + AES-256-GCM via OpenSSL).
The choice is made at runtime, and the UI names the backend rather than
calling everything "encrypted" — a passphrase file and a session keyring
are not the same promise.

Host keys go in BeerSSH's own store (`~/.local/share/beerssh/known_hosts`),
never `~/.ssh/known_hosts`. An unknown host shows its fingerprint and waits
for you to accept before anything is written; a *changed* key gets a
louder, differently-worded warning and defaults to cancelling.

See `project.md` §14 for the milestone plan.

## Presets

A preset is a reusable set of terminal settings — `TERM`, scrollback,
colours, keepalive, an on-connect command — attached to many hosts, so
editing one changes all of them. Six starter presets ship (Default Linux,
Raspberry Pi/SBC, OpenWrt/BusyBox, BSD, network switch, tmux-first), and a
host may override any single field with a one-click revert.

**A preset is structurally incapable of holding a secret** — no hostname,
username, password or key reference — so exporting one and sending it to a
colleague cannot leak your infrastructure. That is enforced by the type
having nowhere to put such a thing, and asserted by a test on the exported
keys.

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
export ANDROID_NDK_ROOT=$HOME/android-ndk-r29
tools/build-deps-android.sh arm64-v8a          # cross-builds libssh + OpenSSL
make android QT_ANDROID_ROOT=$HOME/Qt/6.11.1/android_arm64_v8a
```

Verified for `arm64-v8a` and `x86_64` (the emulator's ABI). Packaging into
an APK additionally needs the Android **SDK**, which is separate from the
NDK.

No Qt account is needed for the kit — `aqtinstall` fetches the same
official binaries:

```sh
python3 -m venv ~/.venv/aqt && ~/.venv/aqt/bin/pip install aqtinstall
~/.venv/aqt/bin/aqt install-qt linux desktop 6.8.2            # host tools
~/.venv/aqt/bin/aqt install-qt linux android 6.8.2 android_arm64_v8a
```

**The native build works**; packaging has never run. The manifest has been
checked against the installed kit's own template and is parsed as part of
the build, but it has still not been through `androiddeployqt`, and
nothing has run on a device or an emulator.

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
