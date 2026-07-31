# BeerSSH

An SSH terminal for **Android and desktop** — background sessions, tabs,
adaptive splits, and a real keyboard. Qt Widgets, C++, one codebase for
both platforms.

Status: **M0 skeleton.** The app builds and shows an empty character-cell
grid. There is no SSH, no emulator and no session handling yet — see
`project.md` §14 for the milestone plan.

## Build (desktop)

Needs Qt 6 (6.8 LTS or newer) and a C++17 compiler.

```sh
make          # build ./beerssh
make run      # build and run it
make test     # headless smoke test (offscreen QPA)
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
src/platform/        bssh_platform + one implementation per platform
android/             AndroidManifest.xml
tools/               indentation gate; font validation lands at M9
```

## Documents

- **`project.md`** — the specification and the source of truth. Read §3
  (architecture and its three core principles) before writing code.
- **`code-style.md`** — snake_case, tabs, lowercase filenames, and why
  `clang-format` is not used here.
