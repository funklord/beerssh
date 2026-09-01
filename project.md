# Project: BeerSSH — Qt SSH Terminal for Android and desktop

> Planning / specification document. **No implementation code here.** It is the
> source of truth for building the app. Decisions marked **[DECIDED]** are
> locked; **[OPEN]** items need a call during implementation.
>
> Revision 4 — **desktop is a first-class target alongside Android** (§1, §2,
> §5.5, §12), and §18 states the code style, which follows `fuzzypickles`'
> `code-style.md`. Rev 3 named the project **BeerSSH** (§0.1); rev 2 added
> tabs/adaptive splits (§8), curated terminal fonts (§9), and the host preset
> system (§10).

---

## 0.1 Name and identity — **[DECIDED]**

| Field | Value |
|---|---|
| **Project / app name** | **BeerSSH** |
| **Android package ID** | `se.vibes.beerssh` |
| **Desktop binary / repo dir** | `beerssh` (lowercase, per §18) |
| **Desktop app ID** | `se.vibes.beerssh` — same reverse-DNS string; it is also the Freedesktop app ID (`se.vibes.beerssh.desktop`, matching `.metainfo.xml` and icon names) |
| **Play Store title** (≤30 chars) | `BeerSSH — SSH Terminal` |
| **Short pitch** | An actively maintained SSH terminal for Android and desktop — background sessions, tabs, real keyboard. |
| **Store description opener** | *"The actively maintained SSH client for JuiceSSH refugees."* |

**Rationale (do not relitigate this — it's settled):** BeerSSH follows the proven
`WordSSH` pattern used by JuiceSSH and, loosely, Termius: a short ownable word
carries the brand identity while the `SSH` suffix carries the meaning and the Play
Store search keyword. The prefix is *not* meant to describe a feature — that's the
pattern working as intended, not a flaw.

**Availability checked (July 2026):** no Google Play app, no GitHub project, and no
trademark conflict found under this name. Nearest neighbour is *BeerSmith*
(home-brewing software), a different category entirely. **Re-check the desktop
namespaces before the first desktop release** — Flathub app ID, distro/AUR package
name, and the `beerssh` binary name against anything already in `$PATH` on a
normal Linux install. A collision there is cheap to find now and expensive after
users have the package.

**Notes:**
- The package ID uses the `vibes.se` domain the author owns, so reverse-DNS
  collision is impossible regardless of app name.
- "Beer" here reads in the root-beer / ginger-beer sense — a beverage name, not an
  alcohol claim. The app contains no alcohol-related content; answer Play's IARC
  content-rating questionnaire straightforwardly and expect an *Everyone* rating.
- Keep the JuiceSSH-successor framing in the **store listing and README only**,
  never in the app name — it aids discovery without making the brand a dependent
  of someone else's product.
- **The placeholder icon now ships on Android too**, which it did not: the APK
  carried **no launcher icon at all** and the packaged manifest read
  `icon=''`, so the phone showed a system default while the desktop `.desktop`
  entry had an icon since 2026-08-03. `android/AndroidManifest.xml` still held
  androiddeployqt's `%%INSERT_APP_ICON%%` placeholder with nothing to insert,
  because there was no `android/res/` at all.
  **This settles nothing about branding.** The SVG's own comment says it is a
  placeholder and that the direction is the [OPEN] item below; it also gives
  the argument that applies here, having anticipated exactly this case — *"a
  .desktop entry without an icon is a defective .desktop entry: software
  centres show a blank tile and some refuse the launcher altogether."* A
  launcher with no icon is the same defect on the other platform, so shipping
  the existing placeholder is consistency rather than a decision.
  The five `mipmap-*/ic_launcher.png` are **generated from that one SVG** and
  committed so the build needs no rasteriser. If the SVG changes they must be
  regenerated, and the command is recorded here so that is a copy-paste rather
  than a rediscovery:

      for d in "mdpi 48" "hdpi 72" "xhdpi 96" "xxhdpi 144" "xxxhdpi 192"; do
          set -- $d
          inkscape --export-type=png \
                   --export-filename="android/res/mipmap-$1/ic_launcher.png" \
                   -w $2 -h $2 packaging/se.vibes.beerssh.svg
      done

  **Verified on the artifact, not on a device**: the APK carries all five
  densities and `aapt2 dump badging` reports
  `icon='res/mipmap-mdpi-v4/ic_launcher.png'` where it previously reported
  `icon=''`. The phone disconnected before the launcher itself could be
  looked at, so what the launcher draws is unconfirmed.
- **[OPEN]** Icon/branding direction (a mug, a bottle cap, foam, or something
  abstract) — worth deciding before store assets are produced.

---

## 0. Feasibility verdict

**Yes, it can be done.** Android is the constraining platform; desktop is the
same code with fewer obstacles. Every hard requirement has a proven path:

- **Qt on Android** is a first-class, supported target (Qt 6.8 LTS and newer,
  with ongoing Android 15/16 support). Qt Widgets run on Android. The same
  Widgets code builds natively on Linux, and on macOS/Windows.
- **SSH** works via `libssh` or `libssh2`, both cross-compilable with the
  Android NDK + CMake and both packaged on every desktop platform.
- **Background running** is achievable with an Android **foreground service**
  bridged from Qt via JNI, keeping the SSH socket and session alive with the
  screen off or the app backgrounded. On desktop the process simply keeps
  running — the whole problem collapses to "don't quit on last window closed"
  (§5.5).
- **Full keyboard** (physical/Bluetooth keyboards + a custom on-screen key row
  for Ctrl/Alt/Esc/Fn/arrows) is doable, and is the single biggest UX risk on
  Android — see §6. On desktop it is a solved problem, which makes desktop the
  place to develop and debug the input layer.

The two areas that will cost the most engineering effort are **(a) keyboard /
input handling on Android** and **(b) the background service + reconnection
lifecycle**. Both are Android costs; the desktop build gets them for nearly
free once the abstractions are right. Terminal emulation is well-trodden but
the no-local-PTY constraint (§3) means the existing desktop terminal widgets
can't be dropped in unmodified on either platform.

---

## 1. Goals and non-goals

### Goals (v1)
- **One codebase, two shipping platforms: Android and desktop.** A session
  connected on a phone and one connected on a Linux desktop run the same
  emulator, the same input router, and the same connection state machine.
  Platform-specific code is confined to the thin layer described in §3
  (core principle 3).
- Connect to SSH servers (password + public-key auth) with a full interactive
  PTY shell.
- Correct VT100/xterm terminal emulation: colors, cursor addressing, scrollback,
  UTF-8, resize (`SIGWINCH` via `window-change`).
- **Background persistence**: sessions survive app-switch, screen-off, and (per
  §5) network changes, with auto-reconnect.
- **Full keyboard support**: external physical/Bluetooth keyboards fully usable,
  plus a custom on-screen modifier/key row layered over the soft keyboard.
- **Multiple concurrent sessions** as tabs, with **split panes on large screens**
  (tablets, foldables, desktop/connected-display mode) — see §8.
- **Curated bundled terminal fonts** — only fonts genuinely suitable for a
  terminal, vetted for monospace metrics and glyph coverage — see §9.
- **Reusable host preset templates** layered over per-host profiles — see §10.
- Secure credential/key storage: Android Keystore on Android, the platform
  secret store on desktop — both behind one interface (§11).
- Ships to **Google Play** (target SDK compliant, correct foreground-service
  declarations) **and as a desktop package** (§12.2).

**Platform priority — [DECIDED]:** **Linux desktop and Android are both v1
targets**; macOS and Windows are best-effort (they should build, and breakage
there is a bug, but no release is blocked on them). Develop each feature on
Linux first because the iteration loop is seconds rather than a deploy cycle,
then make it work on Android before the feature counts as done. "Works on my
desktop" is not done, and neither is an Android-only hack that desktop can't
run.

### Non-goals (v1, revisit later)
- SFTP file browser / port forwarding / SSH tunneling (design so they can be
  added; don't build yet).
- Mosh protocol (note as a future resilience option in §5).
- ~~Local terminal / on-device shell~~ — **this non-goal was reversed
  deliberately.** It read: "this is an SSH client, not Termux and not a desktop
  terminal emulator. Tempting on desktop, where a local PTY is right there;
  still out of scope for v1, and §3's no-PTY principle exists partly to stop it
  creeping in."
  The decision now is that **the desktop opens a local shell by default** and
  Android does not. It is kept here rather than deleted, because a non-goal
  that quietly vanishes is one nobody can tell was ever considered — and the
  reasoning it gave still holds for the platform it was written about. Android
  gets no local shell: an unprivileged app sandbox has no useful userland,
  which is what Termux exists to supply, so it opens its configuration instead.
  **What the no-PTY principle protected is kept.** The PTY lives behind
  `bssh_ssh_session` with the SSH transport, so the emulator, the renderer, the
  input router and the scrollback still know nothing about one. §3's principle
  is amended below rather than dropped: the *core* stays PTY-free; the desktop
  gains a transport that is not a socket.
- iOS (keep the core portable, but don't spend effort on it).
- Arbitrary recursive tmux-style split trees (v1 caps pane count — see §8).
- User-imported arbitrary fonts (v1 ships a curated set only; import is a v2
  candidate, and if added must run the §9.3 suitability check).

---

## 2. Platform / toolchain decisions

| Area | Decision | Notes |
|---|---|---|
| Qt version | **[DECIDED]** Qt **6.8 LTS** (or latest 6.x LTS at build time) | LTS for stability + longest Android support window. Same version on desktop — don't let the two builds drift onto different Qt majors. |
| UI toolkit | **[DECIDED]** **Qt Widgets, no QtQuick/QML — ever**, not even for one screen | Per requirement. See §6 caveats on Android IME. Touch problems get fixed *inside* Widgets (subclassing, `QProxyStyle`, `QScroller`), never by reaching for QML for "just that screen". |
| Language | C++17 (Qt), plus the minimum Java/Kotlin Android's platform model forces | JNI bridge between them; see §5.2 for what "minimum" means. No JVM code at all in the desktop build. |
| Build system | **[DECIDED]** **qmake** (`.pro`/`.pri`) for every target + Gradle for Android packaging only | One project file, scoped per platform; `androiddeployqt` drives the Android side. **Not CMake** — the sibling `fuzzypickles` GUI is qmake, and one build system across both trees is worth more than CMake being Qt 6's default. |
| Desktop targets | **[DECIDED]** Linux (primary), macOS + Windows best-effort | Linux is the development platform; see §1. |
| Min SDK | **[DECIDED]** API **26** (Android 8) | Below 26, foreground-service + Keystore APIs differ; 26 keeps code simple. Android-only concern. Set as `ANDROID_MIN_SDK_VERSION` in `beerssh.pro`, which is what settled it. |
| Target SDK | **[DECIDED]** API **35 (Android 15)** | Required for new Google Play apps in 2026; new apps must target API 35. |
| ABIs | arm64-v8a (primary), armeabi-v7a, x86_64 (emulator) | Google Play requires 64-bit; ship arm64 + optionally armv7. Desktop: x86_64 + aarch64. |
| Package format | **AAB** (Android App Bundle); desktop per §12.2 | AAB required by Play, and enables per-ABI + asset-pack splits (see §9.4). |
| Code style | **[DECIDED]** see **§18** | `snake_case`, tabs for indent, lowercase filenames — same rules as `fuzzypickles`. |

---

## 3. High-level architecture

Identifiers below are the real names to use in code — `snake_case` with a
`bssh_` prefix, per §18, on both platforms.

**They are checked against the tree, because for a long time they were not.**
Five of them named nothing that existed: `bssh_tab_bar`, `bssh_pane_view`,
`bssh_font_manager`, `bssh_preset_store` and `bssh_settings_ui` had all been
built under other names or not as classes at all, while §18.2 went on telling
readers these were the real ones and to update this section if a rename was
genuinely needed. A naming rule nobody can check is a naming rule that has
already drifted; the fix here was to take the code's answer, since the code is
what a reader greps.

Two of them are worth spelling out, because the diagram cannot:

- **There is no tab-bar class.** It is a plain `QTabBar` owned by
  `bssh_main_window`, and the per-tab status dot lives in the
  `bssh_tab_status` *namespace* (`state_for`, `colour_for`, `dot`) so the rule
  deciding which state wins can be tested apart from the painting (§8.3).
- **There is no separate preset store.** `bssh_profile_store` holds hosts,
  presets and layout together — one store over the three INI files of §10.6 —
  which is why `bssh_preset_t` is a plain struct with no storage of its own.

**The two columns group by role, not by directory.** `bssh_font_catalog` is
drawn in the UI column and lives under `src/term` beside `bssh_emulator` and
`bssh_screen`, which is where the renderer needs it. The split this section is
about is the one that matters — a session is not a view — not the folder a
file sits in.

```
┌──────────────────────────────────────────────────────────────────────────┐
│  Qt / C++ layer (portable core — identical on Android and desktop)       │
│                                                                          │
│  ┌─ UI ──────────────────────────────┐   ┌─ Domain ────────────────────┐ │
│  │ bssh_main_window (owns QTabBar)   │   │ bssh_session_registry       │ │
│  │ bssh_layout_manager (tabs⇄splits) │◀─▶│  owns bssh_session[]        │ │
│  │   └ bssh_pane_container           │   │  (survives view teardown)   │ │
│  │       └ bssh_terminal_view[]      │◀─▶│   └ bssh_emulator (VT)      │ │
│  │ bssh_input_router (focused pane)  │──▶│     bssh_screen + scrollback│ │
│  │ bssh_key_row (touch only)         │   │   └ bssh_ssh_session        │ │
│  │ bssh_session_dialog (desktop)     │   │ bssh_conn_manager (FSM)     │ │
│  │ bssh_host_list (touch)            │   │ bssh_profile_store          │ │
│  │ bssh_host_editor, bssh_font_picker│   │  hosts, presets and layout  │ │
│  │ bssh_preset_editor                │   │ bssh_settings_resolver      │ │
│  │ bssh_app_settings_view            │   │ bssh_ssh_config (import)    │ │
│  │ bssh_font_catalog, bssh_theme     │   │                             │ │
│  └───────────────────────────────────┘   └─────────────────────────────┘ │
│                                                                          │
│  ┌─ bssh_platform (one implementation chosen at build time) ───────────┐ │
│  │  bssh_platform_android           │  bssh_platform_desktop           │ │
│  │   - foreground service (JNI)     │   - stay-resident / tray         │ │
│  │   - notification                 │   - desktop notification         │ │
│  │   - connectivity callbacks       │   - QNetworkInformation          │ │
│  │   - keystore-wrapped secrets     │   - platform secret store        │ │
│  └──────────────────────────────────┴──────────────────────────────────┘ │
└──────────────────┬─────────────────────────────────────┬─────────────────┘
                   │ JNI (Android only)                  │ socket
                   ▼                                     ▼
       ┌───────────────────────┐            ┌────────────────────────────┐
       │ Android service shim  │            │  TCP socket / DNS          │
       │ (JVM, minimal — §5.2) │            └────────────────────────────┘
       └───────────────────────┘
```

**Core principle 1 (amended):** keep SSH + terminal emulation in **portable C++**
with **no local PTY dependency in the core**. The desktop now opens a local
shell by default (§2), and it does so through a `bssh_ssh_session`
implementation like any other transport — so the emulator, renderer, input
router and scrollback still have no idea a PTY exists, which is what this
principle was actually protecting. The original wording, and the reason it was
written, follow unchanged. The SSH channel *is* the byte stream that feeds the
emulator; there is no `/dev/pts` on Android for an unprivileged app. This is why
desktop widgets like `qtermwidget` (Konsole-derived, PTY-oriented) are a poor
drop-in and a lighter VT emulator is preferred (see §7). Note that the desktop
build *could* open a PTY — it must not, because the moment the emulator can be
fed from two kinds of source the Android path stops being the tested one.

**Core principle 2 — sessions are not views.** A `bssh_session` (SSH connection +
emulator state + scrollback) lives in the **`bssh_session_registry`**, independent
of any widget. `bssh_pane_container`/`bssh_terminal_view` **attach to** a session and
can be destroyed and recreated freely. This one decision makes three otherwise-
painful features fall out almost for free: collapsing splits back to tabs on a
window-size change (§8.2), surviving Android activity recreation (rotation,
config change), and staying alive in the background (§5). **Do not** let session
state live in a widget.

**Core principle 3 — platform code lives behind `bssh_platform`, not behind
`#ifdef`s scattered through the tree.** Everything that differs between Android
and desktop — keeping the process alive, notifications, connectivity change
events, secret storage, where files go — is an interface method with one
implementation per platform, in its own file (`platform_android.cpp`,
`platform_desktop.cpp`). Two consequences worth stating outright:

- A conditional in domain or UI code is a design smell. If a feature reads
  "on Android, do X" outside the platform layer, the interface is missing a
  method.
- **Neither platform is allowed to be the special case that skips a rule.**
  The registry, the router, the layout manager and the emulator are the same
  objects on a phone and on a workstation; only their platform backend and
  their window size differ.

---

## 4. SSH backend: libssh vs libssh2 (comparison + recommendation)

Both cross-compile for Android with the NDK; both need a crypto backend
(OpenSSL, mbedTLS, or wolfSSL). Build the chosen crypto lib for Android too.

| Criterion | **libssh** | **libssh2** |
|---|---|---|
| API level | Higher-level, more batteries-included | Lower-level, more manual plumbing |
| PTY / interactive channel | `ssh_channel_request_pty`, `window-change`, easy | Supported but more manual state handling |
| Keepalive | **Not exposed** — see the correction below | `libssh2_keepalive_config`/`_send` |
| SFTP (future) | Built-in SFTP subsystem | `libssh2_sftp_*` available |
| Known-hosts handling | Built-in helpers | More manual |
| Threading model | Callbacks + blocking/non-blocking modes; be careful | Non-blocking friendly, integrates with a socket loop |
| Footprint | Larger | Smaller / leaner |
| License | LGPLv2.1 | BSD-3 |
| Android build notes | CMake toolchain; static link recommended | CMake/autotools; static link recommended |
| Desktop build notes | Packaged on every distro; link the system library where one exists | Likewise |

**Correction, found while building M5:** this table originally credited
libssh with built-in keepalive. It does not have one. libssh 0.11's public
headers expose no client keepalive API — no option, no send call — only a
`SSH_GLOBAL_REQUEST_KEEPALIVE` constant for requests arriving the other
way. libssh2 does have `libssh2_keepalive_config`/`libssh2_keepalive_send`.

That matters because §5.3 wants keepalive to detect a dead socket quickly,
and it is one of the reasons this table gave for choosing libssh.

**[DECIDED]** — the third route, strengthened, and no vendoring or library
switch. Two mechanisms, because the problem is two problems:

- **NAT mappings** are kept alive by `ssh_send_ignore()`, which IS public
  libssh API (SSH_MSG_IGNORE), on the host's `keepalive_seconds` interval.
  This is what a carrier NAT dropping an idle mapping needs, and it is the
  common case on a phone.
- **A peer that has gone** is detected by TCP keepalive, configured on the
  socket from `ssh_get_fd()`: idle 30s, three probes 10s apart, plus
  `TCP_USER_TIMEOUT` so unacknowledged *keystrokes* also time out rather
  than being retransmitted for minutes while the terminal merely looks
  slow. The system default is two hours, which is a session that stays
  "connected" all afternoon.

The honest limit: SSH_MSG_IGNORE is never answered, so a successful send
proves only that the local kernel took the bytes — the round trip that
actually detects a dead peer is TCP's, not ours. If ~60s proves too slow
in practice, route one (our own global request with `want_reply`, through
the vendored-and-patched route) is the escalation, and the
`bssh_ssh_session` interface still makes route two possible.
**Do not treat the transport choice as settled on the strength of a row
that was wrong.**

**Recommendation: `libssh`** for v1 — its higher-level PTY/keepalive/known-hosts
handling reduces the surface area for subtle bugs, and its built-in SFTP eases
the future non-goal. `libssh2` is the better pick only if we later need very
fine-grained non-blocking control or the smallest possible binary.

**Design constraint:** isolate the SSH library behind a **`bssh_ssh_session`
interface** (`connect`, `authenticate`, `open_shell`, `read`, `write`, `resize`,
`disconnect`, events). Both libraries must be implementable behind it, so the
choice stays swappable and testable. Do **not** let libssh types leak into the
terminal or UI layers. A useful side effect: a fake implementation of this one
interface, replaying a captured byte stream, is how the emulator gets tested
without a server (§12.3).

**LGPL note:** libssh is LGPLv2.1 — **link dynamically**, or satisfy LGPL relinking
obligations if static. Resolve this before shipping to Play. (libssh2's BSD-3
licence avoids the question entirely; if the legal overhead annoys you, that's a
real argument for libssh2.) On desktop this is a non-issue in practice: link the
distro's shared `libssh` and the obligation is already satisfied. **Android is
where it bites**, since an AAB wants everything statically linked — so decide the
question on the Android build's terms, not the desktop one's.

**Crypto backend: [DECIDED] OpenSSL** — best-tested with libssh; mbedTLS and
wolfSSL are lighter but add integration risk. Settled at build setup, which is
where this said it would be: desktop links the system OpenSSL, and
`tool/build-deps-android.sh` cross-builds a pinned copy for Android.

---

## 5. Background running & reconnection (a core requirement)

Requirement chosen: **full reconnect / session persistence**, not just a brief
keep-alive.

The *behaviour* is identical on both platforms — sessions outlive the window
they were opened in, and reconnect on their own. Only the mechanism differs:
Android needs a foreground service to be allowed to keep running (§5.1–5.2),
desktop needs a decision about not quitting when the last window closes (§5.5).
`bssh_conn_manager` (§5.3) is shared and knows about neither.

**Network-change reconnect is one implementation, not two.** This section used
to promise `ConnectivityManager.NetworkCallback` on Android and
`QNetworkInformation` on desktop; only the desktop half existed, so the platform
§5 exists *for* was the one that waited out the backoff and the TCP timeout on a
Wi-Fi-to-mobile handover, while a laptop waking from suspend reconnected at
once. No JNI and no second Java file were needed after all — Qt ships an Android
backend for `QNetworkInformation` that is `ConnectivityManager` underneath — so
the watch now lives in the shared base class. Two things had to be added to the
curated Android deployment list (§12.2) for it to work: the plugin *and* its
JAR. Without the JAR the plugin loads, fails to find its Java class, and Qt falls
back to a "dummy" backend that reports nothing — no error, no missing feature,
just a signal that never fires. **Measured on a device:** with both, a session
whose network is dropped and restored reconnects within about five seconds.

### 5.1 Foreground service (mandatory on modern Android)
A backgrounded app cannot reliably hold a socket open; Android will kill it.
Solution: a **foreground service** with an ongoing notification, started when the
first session connects and stopped when the last disconnects.

**Stopping it is a START.** `startForegroundService()` opens a contract: the
service has about five seconds to call `startForeground()` or Android throws
`RemoteServiceException` and kills the process. A plain `stopService()` arriving
first does not release that contract — it destroys the service before
`onStartCommand` ever runs, so `startForeground()` is never called. That is
exactly the shape of a connection that fails immediately: the session goes live,
the service is asked to start, the connection is refused a moment later, and the
service is asked to stop before it existed. **Found on a device**, where any
refused connection killed the app. So a stop is delivered as another start
carrying a `STOP` action, and `onStartCommand` honours the contract first and
stops itself second — and the C++ side only crosses JNI when the state actually
changes, since the window recomputes it on every session event.

**Foreground-service TYPE — the choice is MADE; what is [OPEN] is whether it
survives adjudication.** This entry read as an undecided question between three
candidates for some time after one of them had been implemented, which sends a
reader looking for a decision that `46e197c` had already taken with reasons.
The manifest declares `android:foregroundServiceType="connectedDevice"`, with
`FOREGROUND_SERVICE_CONNECTED_DEVICE` and the qualifying `CHANGE_NETWORK_STATE`
this section names as its requirement.
**What remains open is narrower and is validation rather than design.** The
type has never been adjudicated by a system that enforces it: `46e197c` verified
the service on an API 30 emulator, where the Android 14 rules do not apply, and
the emulator images on this machine are **API 33** — the rules bite at 34. So
the experiment that would settle it is named and cannot be run here: install on
API 34 or later and observe whether the service starts at all. Play's review of
the justification is the second half, and no local test answers it.
Whether to keep `connectedDevice` or move to `specialUse` stays the copyright
holder's call. The candidates below are kept because that decision may still be
made, not because it has not been.

Android 14+ requires a declared `foregroundServiceType`, and Google Play reviews
the justification. Candidates for a persistent network/SSH connection:

- **`connectedDevice`** — framed as interacting with an external device over a
  network connection. **No runtime time limit.** Requires one of the qualifying
  permissions (e.g. `CHANGE_NETWORK_STATE`). Reviewers understand it, but "device"
  framing for a remote server is a slight stretch.
- **`dataSync`** — data transfer framing. **Caution: Android 15 imposes a time
  limit** on `dataSync` foreground services (roughly ~6h/24h window) after which
  the system stops them — a poor fit for a shell you leave open all day.
- **`specialUse`** — most honest for "keep an interactive SSH session alive."
  Requires a manifest `PROPERTY_SPECIAL_USE_FGS_SUBTYPE` explanation and a
  written justification during Play submission; approval is case-by-case.

**Plan:** prototype with `connectedDevice` (no time limit, lowest friction), and
prepare a `specialUse` justification as the fallback if Play pushes back. Avoid
`dataSync` as the primary type because of the Android 15 time limit. **Verify the
exact current Play policy at implementation time** — these rules change.

**Built, and verified on an emulator.** The service starts when the first
session becomes live and stops when the last ends — §5.1's own wording, and
notably *not* tied to the window closing, which is a desktop idea Android
does not have. Confirmed: `isForeground=true`, a notification reading "1
session connected", the process surviving the home button, and both service
and notification going away with the session.

**Two things that emulator could not test**, and both matter for shipping:
it runs API 30, where the `foregroundServiceType` rules Android 14+ enforces
do not apply, so `connectedDevice` was never actually adjudicated; and Play
review's opinion of the framing remains entirely unknown.

Required manifest permissions (indicative): `FOREGROUND_SERVICE`, the
type-specific `FOREGROUND_SERVICE_*` permission, `INTERNET`,
`ACCESS_NETWORK_STATE`, `POST_NOTIFICATIONS` (Android 13+), and
`WAKE_LOCK` if a partial wake lock is needed during active transfer.

### 5.2 Qt ↔ service bridge (Android only)
- The SSH/socket work runs in a **Qt worker thread** inside the process — the
  same thread arrangement as on desktop. The JVM foreground service exists only
  to keep that **process alive and de-prioritized from being killed**, hold the
  notification, and receive OS callbacks.
- **Write as little JVM code as the platform forces, and no more.** Android has
  no way to run a background component without a manifest-registered `Service`
  subclass — that part is mandatory and cannot be engineered around. Everything
  else is not. The service's entire body should be: build the mandatory
  notification, make one JNI call into C++ to start, one to stop. No
  networking, no protocol handling, no session logic, no UI logic in the JVM
  layer, including logic that merely glues two native calls together. If a
  design needs more Java/Kotlin than that, look harder for the C++/JNI way
  rather than accepting the convenience.
- Bridge via **JNI** (`QJniObject`/`QJniEnvironment`, or Qt's current Android
  patterns — **check what Qt 6 already provides before hand-rolling a service
  template**, this area has changed across Qt versions). Keep the bridge thin:
  start/stop service, update notification text (e.g. "2 sessions connected"),
  forward network-change events.
- Notification should reflect the **`bssh_session_registry`** count, not the view
  state — panes and tabs come and go, sessions are what matter.

### 5.3 Reconnection state machine (in `bssh_conn_manager`)
States: `disconnected → resolving → connecting → authenticating → connected →
degraded → reconnecting → failed`. State is **per session**, so a split view can
show one healthy and one reconnecting pane simultaneously. This class is
portable — it must contain no platform conditionals; it consumes network-change
events from `bssh_platform` (§3) without caring which platform produced them.

- **A shell that exits is not a connection that dropped.** The transport must
  report the two separately, or reconnection reopens the shell the user just
  left — endlessly, which is exactly what happened the first time this was run
  against a real host. `exit` at the prompt, a command finishing, and a
  non-zero exit status are all *endings*; a reset socket and a vanished
  network are *drops*. Only the second kind is retried.
- **Keepalive/heartbeat** to detect dead sockets fast (SSH-level keepalive +
  optional app-level ping).
- **Network-change awareness**: on Android, subscribe to
  `ConnectivityManager.NetworkCallback` via the service; on desktop, use
  `QNetworkInformation` (reachability/transport changes — suspend/resume from
  a laptop lid is the common case, and a VPN coming up is the annoying one).
  Both feed the same `bssh_platform` event. On a network switch, trigger a
  reconnect rather than waiting for a TCP timeout.
- **Backoff**: exponential backoff with jitter, capped; user-visible status.
- **Reconnect storm control:** with many sessions open, a network flap must not
  fire N simultaneous reconnects. Stagger with per-session jitter.
- **Session continuity gap:** plain SSH cannot resume a dropped shell — a reconnect
  starts a *new* remote shell; scrollback is preserved locally but remote state
  (running program, cwd) is lost. Document this clearly. **Mosh** is the only real
  fix and is a **v2** consideration (adds a UDP protocol + server-side `mosh-server`
  dependency). For v1, offer optional integration with `tmux`/`screen` guidance in
  the UI/help instead — and note that a preset's **on-connect command** (§10.2) is
  the natural place to auto-attach tmux.

### 5.4 Battery / OEM caveats (Android)
Aggressive OEM battery managers (e.g. some Chinese OEM skins) may kill the
service anyway. Add an in-app prompt to whitelist the app from battery
optimization (`REQUEST_IGNORE_BATTERY_OPTIMIZATIONS`) — but note Play restricts
this permission's use, so gate it behind clear user action.

### 5.5 The desktop equivalent — **[DECIDED, and built]**
No service, no notification channel, no OS permission: a desktop process that
holds a socket open simply keeps holding it. What still needs deciding is what
"backgrounded" means when there is no Android lifecycle to tell you.

- **`setQuitOnLastWindowClosed(false)`** while any session is connected, with a
  **system tray icon** as the way back to the window. Both live behind
  `bssh_platform::set_keep_alive()`, so the caller states the intent and never
  learns whether it got a tray icon or a foreground service. Sessions belong to the
  registry, not to a window (§3), so closing the window must not touch them —
  the same rule Android's activity recreation forces, arriving here as a
  deliberate choice rather than an OS requirement.
- **Closing the last window with sessions still connected must be a prompt**,
  not a silent background residency: "3 sessions still connected — keep running
  in the tray / disconnect and quit". A terminal that vanishes into the tray
  unannounced is how users lose work they thought they'd closed. Remember the
  answer as a setting.
- **"Out of sight" is not the same operation on the two platforms**, and
  assuming it was cost the Android build its way back in. Hiding the window is
  the *desktop's* mechanism — the tray icon is the route back. On Android a
  hidden top-level widget leaves the Activity showing nothing, and the launcher
  then **resumes** that Activity rather than starting it, so nothing ever calls
  `show()` again: choosing "keep running" left a blank white screen that no
  amount of relaunching recovered, only a force-stop. Android's own answer is
  `moveTaskToBack`, exactly what the Home button does, and the window goes with
  the task intact. Both live behind `bssh_platform::send_to_background()`, which
  answers whether the platform did it; false means the caller hides its own
  window. Found by driving the prompt on a device — the desktop half is right
  either way, which is why nothing on a desktop could have caught it.
- **Suspend/resume, not screen-off, is the desktop analogue** of the Android
  background problem: a laptop lid closing kills every TCP connection in a way
  that looks exactly like a network flap on wake. Treat resume as a
  network-change event (§5.3) and reconnect rather than waiting for keepalive
  timeouts to notice.
- **Inhibit sleep? [OPEN]** — a long-running remote build is an argument for it
  (`org.freedesktop.PowerManagement.Inhibit`/`systemd-inhibit`, per-platform).
  Leaning **no** by default: silently keeping a laptop awake because a shell is
  open is worse behaviour than the reconnect. Consider an explicit per-session
  "keep awake" toggle instead.
- **Desktop notifications** (via `QSystemTrayIcon`, or the platform's native
  service) carry the same events the Android notification does: reconnecting,
  reconnect failed, bell rang in an unfocused pane.

---

## 6. Keyboard & input (the hardest UX problem)

A terminal needs keys a normal Android text field never sends: **Ctrl, Alt, Esc,
Tab, arrows, Fn, PageUp/Down, Home/End, function keys**, and correct handling of
**physical/Bluetooth keyboards**.

**Desktop's role here is not "the easy platform" — it's the reference.** On
desktop every one of those keys arrives as a plain `QKeyEvent`, which makes the
desktop build the place to get the key→bytes tables *right* and to test them
(§12.3): if `Ctrl+Alt+F3` produces the wrong sequence, that's a bug you can see
in seconds rather than after a deploy. Android then becomes a delivery problem
(§6.1) rather than a correctness one. Build the tables on desktop; port the
quirks on Android.

### 6.1 Physical / Bluetooth keyboards
- Physical keys arrive as `QKeyEvent`; capture them via an **event filter** on
  the terminal widget and translate to the correct escape sequences (e.g. Ctrl-C
  → 0x03, arrow up → `ESC [ A`, in application-cursor mode → `ESC O A`). This
  path is shared: a Bluetooth keyboard on a phone and a USB keyboard on a
  workstation must produce identical bytes.
- **Known Qt/Android quirk:** soft-keyboard vs hardware-key event delivery on
  Android is historically finicky (some keys only arrive as IME commit text, not
  key events). Budget time to test on real hardware keyboards and handle both
  `keyPressEvent` and `inputMethodEvent` paths. This is a documented rough edge,
  and it is Android-specific — do not let the workaround for it distort the
  shared translation layer.
- **Desktop has its own delivery quirks**, smaller but real: Wayland vs X11
  differ on which chords ever reach the app, compositors and window managers
  steal some (`Ctrl+Alt+F*` most notoriously), and dead keys/compose go through
  `inputMethodEvent` on desktop too. Test on both Wayland and X11 before
  calling the input layer done.
- Support **key repeat**, modifier chords, and a configurable **Caps→Ctrl / map
  keys** option (power-user expectation).
  **Caps→Ctrl is built, and it is honestly partial.** Caps Lock arms Ctrl for
  the next key — the same sticky modifier the on-screen row uses (§6.2), so a
  user meets one behaviour rather than two — and a second press disarms it.
  Off by default, stored in `app.ini` beside the key row and the theme, because
  it is a fact about the keyboard in front of the user rather than about the
  machine at the other end.
  **Sticky rather than hold-to-Ctrl, and that is forced rather than chosen:**
  only key *presses* reach `bssh_input_router` — the terminal view implements
  `keyPressEvent` and not `keyReleaseEvent` — so a hold reading would set a
  modifier with nothing able to clear it, and every key after it would become a
  control character.
  **What it cannot do is stop the OS toggling its own caps state.** The key
  reaches the compositor before any application sees it, so the lock still
  flips and still affects other programs; no Qt application can prevent that. A
  permanent swap belongs in the keyboard layout (`setxkbmap`, `/etc/default/
  keyboard`), and the setting's own tooltip says so rather than letting the
  user discover it. This is the terminal doing what it can inside its own
  window.
  **The wiring is tested, not just the behaviour**, because that is the half
  this project keeps getting wrong: §6.2 records the key row supporting three
  visibility modes since M3 with nothing ever setting one, and §10.1 records
  the override badge built on one editor while the other resolved against the
  app defaults. A router test and a persistence test both pass with the window
  never calling the setter, so a third test opens a session through an actual
  window and asks the session's own router.
  Still not built: general key remapping. `map keys` above is broader than one
  key, and nothing yet lets a user say which.
- **Pane navigation shortcuts** once splits exist (§8): e.g. `Ctrl+Alt+Arrow` to
  move focus between panes, `Ctrl+Shift+T` new tab. These must be **intercepted
  before** the byte translation and be **user-remappable**, because any chord you
  steal is a chord the remote shell can no longer receive. Keep the stolen set
  minimal and document it.
  **The set is eight chords, and it is now written down exactly once.**
  Ctrl+Shift+C, Ctrl+Shift+V, Shift+PageUp, Shift+PageDown,
  Ctrl+Alt+Left/Right, and — Konsole's, and named here long before anything
  took them — Ctrl+Shift+T and Ctrl+Shift+N for another session in a tab or
  a window. Without those two, a desktop user's only route to a second
  session was the context menu, the front door and a dialog: three steps for
  the commonest thing a terminal does. What they open is what the device
  opens with (§8.5), which is what makes the chord predictable; duplicating
  the host you are on is a different intent and has its own entry on the tab
  menu. That list lived only as an if-ladder inside
  `handle_key()`, so the only way to find out which chords never reach your
  shell was to press one and notice — this section asked for it to be
  documented and nothing did. It is `bssh_input_router::stolen_chords()`
  now, which is **what the router matches against**, and "This device"
  renders that same table under *Keys this app keeps*. A list written
  anywhere else would be a second copy free to drift from the behaviour,
  which is the failure this document keeps unpicking; a test walks the
  table in both directions, asserting every entry really is intercepted and
  sends the remote nothing, and that a near miss like Ctrl+Alt+Up still
  gets through.
  **Matching is exact, against the modifiers that mean something.** Qt sets
  `Qt::KeypadModifier` on the keypad's own PageUp and `GroupSwitchModifier`
  on X11 at the third level, and neither says anything about which chord was
  pressed — so comparing the whole modifier set would steal Shift+PageUp on
  the main block and pass it through on the keypad of the same keyboard.
  **Remapping is built, and giving a chord BACK is the point of it.** Each
  chord has a stable name — `copy`, `new-tab` — and app.ini's `[chords]`
  section binds it, so a hand-edited file survives the list being reordered.
  A name bound to **nothing** is not stolen at all and reaches the remote
  shell, which is what this section has always implied: "any chord you steal
  is a chord the remote shell can no longer receive", so handing one back has
  to be sayable. An unknown name or an unparseable sequence leaves the
  built-in, because a hand-edited file with a typo in it should behave like
  the default rather than lose the binding. *Keys this app keeps* is a column
  of `QKeySequenceEdit`s now — the control the platform already has for this,
  which records the chord you press rather than making you spell it — and it
  is built from the **defaults** rather than from the effective list, or an
  action whose chord had been given back would vanish from the page and could
  never be bound again.

### 6.2 On-screen keyboard strategy (touch platforms)
Android soft keyboards don't expose Ctrl/Esc/etc. Plan a **custom on-screen key
row** (a strip of buttons above the soft keyboard) providing: `Esc Tab Ctrl Alt
↑↓←→ Fn - / | ~` and a Ctrl/Alt "sticky modifier" toggle. This is what ConnectBot
/ Termux do and is expected by users.

- Render it as a Qt Widget docked above the input area; feed synthetic key events
  into `bssh_input_router`.
- The row layout is a **preset-owned setting** (§10.2), so different server types
  get different key rows. **[BUILT]** Five layouts, curated on the same pattern
  as the fonts (§9.2) and colour schemes (§10.2):
  **Default** (what the row always held), **Network switch** (`?` and `|` for a
  console CLI, no Alt), **tmux** (the Ctrl-B prefix as a single key, plus what
  usually follows it), **vi / vim** (Esc first, then `:` `/` `$` `^`), and
  **Minimal** (four keys and the arrows, for a narrow screen). Every layout is
  checked to carry Escape and a modifier — a row without those cannot drive a
  terminal at all. Still open: letting a user compose their own row.
- **The key row did not fit a phone, and never had.** Its own key list at §6.2's
  own 48dp touch floor is twelve keys before `Fn` was added; the default row
  measured **652px of minimum width against a 360px screen**, and all five
  curated layouts were over. `Fn` took it from 604 to 652 and did not cause it.
  **The symptom was on screen and did not look like a layout bug.** Qt squeezes
  the buttons past their minimum and the labels elide, so `Ctrl` drew as `Ctr`
  and the row read as merely cramped rather than as a widget demanding nearly
  twice the width it has. It is visible in this project's own device
  screenshots.
  **Nothing caught it because the key row was not in
  `every_touch_surface_fits_a_phone`** — which holds the host list, both
  editors, settings, licences and the font picker to 360×640, and omitted the
  one widget on that list existing *only* for touch. Every other surface has a
  desktop form somebody looks at; this one is seen on a device or not at all.
  **The keys scroll horizontally now**, as the tab bar does with the same
  problem (§8.3) and as ConnectBot and Termux do with this exact strip.
  Shrinking was not available: under the touch floor a key is one you cannot
  reliably hit, which is worse than one you have to scroll to. 652px of minimum
  width becomes **92**.
  Two things had to be understood first, and the first guess was wrong. A
  `QScroller` on the viewport was blamed for a crash in the first attempt; a
  20-line probe building and destroying that exact widget shape, with and
  without the gesture, survived both — so the gesture was innocent and the
  crash was elsewhere in that attempt, which was reverted rather than committed
  half-working. And `check_fits`'s overflow walk flags any child wider than its
  parent, which is what a scroll area's content legitimately *is*; it skips
  descendants of a `QAbstractScrollArea` now, or it would fail the fix rather
  than the bug.
  The test asserts both directions, because the obvious one is not enough: the
  row must fit, **and** its keys must still run past the edge. A row that had
  quietly lost its keys would satisfy the first on its own — cutting the
  default layout to four keys fails with *"the keys end at 200px; nothing is
  being scrolled"*.
- **`Fn` was listed above and was not in any of them, so F1–F12 were
  unreachable on a phone.** The emulator has understood `KEY_FUNCTION_BASE + n`
  since M3 and the router has translated physical F1–F35 for as long; the only
  missing piece was a way for a thumb to ask, and without it htop, `mc` and
  vim's help were simply off the table on the platform this row exists for.
  `Fn` swaps the row for F1–F12 and back, and stays on the row in both states
  or there would be no way back to the keys that send Escape. A **toggle rather
  than a sticky modifier**, because there is nothing on this row for it to
  modify: the digits it would combine with live two layers down a soft
  keyboard, which is the whole reason the row exists.
- **The 48dp floor is one number now, and it is checked.** It had been a
  static in seven files under four names — `BSSH_TOUCH_HEIGHT_DP` in the font
  picker, the password prompt and the preset editor, `BSSH_TOUCH_TARGET_DP` in
  the window, the key row and the pane container, `BSSH_FIELD_HEIGHT_DP` in the
  host editor — plus a bare `48` fifteen times over. The duplication was not
  the danger; a rule written in seven places is one that can be changed in six.
  It lives in `ui/touch_metrics.h` with the 64dp list row beside it (§8.5).
  **What the one number made possible is the check**, and the check found a
  real defect immediately: the *Let this host set my clipboard* checkbox on the
  touch preset editor stood **23dp**, half the floor, on the one layout built
  for a thumb. `every_touch_surface_fits_a_phone` now holds every control a
  finger aims at — a button, a field, a box you pick from, a switch — to the
  floor, on every touch surface at once.
  **The cause was a list, and the list is gone.** That page applied the floor
  by naming fourteen controls, and the comment above it already recorded Import
  and Export having been left out of that same list once before — so the
  clipboard checkbox was the second to be missed the same way.
  `bssh_apply_touch_heights()` walks the form instead, and a page that grows a
  control no longer has to remember a second place.
  **The check was wrong once before it was right**, which is worth recording
  because it is the shape `evidence.md` warns about: it first flagged the
  `QLineEdit` *inside* a `QSpinBox` at 42dp. The spin box was a correct 48 and
  the 42 was its frame — you aim at the outer control — so the check skips
  anything nested inside another target, walking the whole ancestor chain
  rather than testing one parent.
- **A row is a target, and a form must fit the width it is given.** Two
  defects that no check could have caught, both found by looking at the
  screenshots. The **licences list stood at 17dp rows** and the combo boxes'
  **dropdowns at 25dp**: rows are not widgets, so everything that looks for
  child widgets walks straight past them, and a combo builds its popup lazily
  so it does not exist when a form is constructed. Rows take the floor from an
  item delegate, which composes with the stylesheet a list may already carry
  (§8.5's host list draws its separators with one) and leaves its deliberately
  taller 64dp rows alone.
  **And the settings page did not fit a phone** — 386px of a 346px screen, its
  notes cut mid-word — while every width assertion passed, because
  `check_fits` skips whatever is inside a `QScrollArea`. That exemption exists
  for the key row's scrolling content and was hiding a form wider than the
  screen; it is a *height* exemption now. A page may scroll down. A form that
  scrolls sideways on a phone is a form nobody reads, and the key row is the
  one exception because sideways is its whole design.
  **The cause was an empty label**: `addRow(QString(), widget)` reserves the
  label column's width — 145px — for nothing, next to a checkbox asking 241.
  Every checkbox and every note on that page had been paying it. The check
  names the widest widget now, because a number on its own sends the reader
  hunting; it was what turned two wrong guesses into the answer.
- **Hidden by default on desktop, but not `#ifdef`-ed out.** Gate it on
  "touch-primary input", not on the OS — a convertible laptop in tablet mode and
  an Android device with a Bluetooth keyboard are the two cases an OS check gets
  wrong in opposite directions. Make it a three-state setting (auto / always /
  never); auto follows the primary input device. **[BUILT]** The key row has
  supported all three modes since M3 and nothing ever set one, so AUTO was the
  only reachable state. It is now a device-level setting in `app.ini` (§10.6),
  edited from "This device" in the desktop dialog and from the Hosts overflow
  menu on Android — the first app-level setting, and the store M11 will need.
- **Light and dark, defaulting to the system** — the second device-level setting,
  same three states and the same reasoning: "follow the desktop" is a real answer
  and the one most people want, but it is not the only one. It sits beside the
  key row in `app.ini` and on the same two screens.
  **Detection is the hard half, and Qt alone does not do it.** `QStyleHints::
  colorScheme()` is authoritative where a platform theme is engaged and returns
  `Unknown` where none is — which on this very machine meant Qt reported
  `Unknown` and handed back a *light* Fusion palette while the XDG portal
  reported `prefer-dark`. Believing Qt there paints a light window on a dark
  desktop. So `bssh_platform::system_colour_scheme()` asks in order: Qt's hint,
  then the portal's `org.freedesktop.appearance color-scheme` over DBus, then the
  lightness of whatever palette we were handed, and only then guesses — **light**,
  because guessing dark paints white text onto whatever the platform actually
  drew if the guess is wrong, which is the more damaging way round.
  **Applying it is ours too.** `QStyleHints::setColorScheme()` looks like the
  right call and does nothing on Linux — measured under both offscreen and xcb,
  with and without a platform theme: no signal, no palette change. `bssh_theme`
  builds the palette itself, from fixed values rather than by darkening whatever
  Qt started with, so the same setting gives the same result on a Fusion session,
  a GTK-themed one and Android. Qt is still *told*, so anything asking it rather
  than us agrees.
  Both change sources are watched — Qt's signal and the portal's
  `SettingChanged` — because either may be the one that notices, and a theme that
  is only right at startup is one a user calls broken.
  **On Android, Qt is right once and then stale.** Verified on an emulator with
  the app running: `QStyleHints::colorScheme()` reports the correct value at
  startup, and after the system switches to dark it goes on reporting Light while
  Qt delivers a `ThemeChange` to the application. Right at startup and wrong
  afterwards is the worst of the two — the app looks correct until the moment the
  user changes their mind. So Android asks its **own** `Configuration.uiMode &
  UI_MODE_NIGHT_MASK`, which is what the platform itself consults and is therefore
  right whenever it is asked, and re-reads it on the theme event Qt does deliver.
  No Java is added for it (§17): a field read through JNI, not glue that would be
  shorter in Kotlin.
  The last-resort palette check uses the palette captured at **construction**,
  before the app has applied one of its own. Reading the current palette is a trap
  that only shows up once the app can set it: "what is the system asking for"
  would answer with what the app last decided, so forcing Light once would make
  the fallback report Light as the system preference for the rest of the session.
- **What the theme touched that nobody had looked at.** Adding a light palette
  is the easy half; what it exposes is everything that was only ever seen against
  a dark one. The four tab status dots (§8.3) — the only thing on a tab that says
  whether a session is alive — **all failed WCAG's 3:1 for a graphical indicator**
  on the light window: green 1.83:1, grey 2.22:1, and "connecting" yellow at
  1.44:1, which is as good as invisible. They have light variants now, same hue
  and saturation darkened until they pass, so green still reads as green in both.
  A test asserts the ratio against the palette actually in force, rather than
  against a table of constants.
  The accent `#2b90d9` had been written in **three** files by the time the theme
  landed; the focus border and the add button read it from `QPalette::Highlight`
  now. A stylesheet cannot read the palette and does not re-evaluate when it
  changes, so the add button is restyled on `PaletteChange` — and the tab dots are
  pixmaps already handed to the tab bar, so those are rebuilt on the same event.
  A repaint does not redraw a pixmap somebody else is holding.
- **The terminal's own colours are deliberately NOT tied to this.** A host's
  `colour_scheme` (§10.2) describes what the remote's output should look like,
  and a dark terminal in a light window is a common and deliberate choice. Making
  it follow the desktop would silently overrule a per-host decision the user has
  already made. **[OPEN]** whether the *default* scheme alone should follow —
  there is currently one light scheme in the set (§10.2), so a light default
  would need one designing first.
- Keeping it buildable on desktop is also what makes it *testable* — a synthetic
  key row press is the cheapest way to exercise the router's sticky-modifier
  logic in a unit test.
- Decide **[OPEN]** whether to also ship an optional bundled soft-keyboard (like
  Termux's Hacker's Keyboard approach) — probably **no** for v1; rely on the key
  row + external keyboards.

### 6.3 Input routing
Central **`bssh_input_router`** converts (key, modifiers, terminal mode) → bytes,
using the emulator's current modes (application cursor keys, keypad mode,
bracketed paste). This must be the single source of truth so physical keys, the
on-screen row, and paste all go through one place — and it must be
platform-free, so the bytes a keystroke produces can never depend on which
platform is running.

**With splits, the router is focus-aware:** exactly one pane holds input focus and
receives all key bytes. Focus must be unambiguous and *visibly* indicated (§8.3) —
two identical-looking shells side by side is a genuine footgun if the user can't
tell which one is about to receive `rm -rf`.

### 6.4 Pointer, selection and clipboard
The two platforms want genuinely different gestures for the same operations, and
this is the one place where duplicating the interaction rather than sharing it is
correct — a long-press-to-select model on a mouse is as bad as a drag-to-select
model on a finger.

| Operation | Desktop | Touch |
|---|---|---|
| Select text | Click-drag, double-click for word, triple-click for line — **all done** | Long-press then drag handles |
| Copy | `Ctrl+Shift+C` (never bare `Ctrl+C` — that's `SIGINT`) | Selection toolbar |
| Paste | `Ctrl+Shift+V`, middle-click on X11/Wayland | Selection toolbar / key row |
| Scroll | Wheel, `Shift+PgUp/PgDn` | Kinetic touch scroll (`QScroller`) |
| Drop a file | Pastes its path, shell-quoted — **built** | No system drag source in practice |

- **Paste always goes through the router** so bracketed-paste mode applies. A
  paste that bypasses it will silently break `vim` and any shell with bracketed
  paste enabled.
- **Mouse reporting** (SGR 1006 et al.) matters mainly on desktop, where TUIs
  expect clicks to reach the remote app. When the remote enables it, clicks go
  to the remote — with a modifier (conventionally `Shift`) to override and select
  locally. **Done**, encoding delegated to libvterm, which already tracks
  which mouse modes the remote enabled.
- **"On touch, keep local selection winning" was this document's answer and it
  was wrong — the code was right and the rule was not.** Settled 2026-08-27,
  after the two had contradicted each other for as long as both existed.
  Refusing to forward a tap would mean a phone could never use `mc`, `htop` or
  a mouse-aware `vim` at all, which is worse than what it was protecting
  against; the code forwards taps exactly as it forwards clicks, and that part
  stands.
  **What was actually broken is that the override did not exist on touch.**
  Shift is the conventional way out of a program that grabbed the mouse, and
  every override in the pointer path was gated on it — on a surface with no
  Shift to hold. Measured before deciding: inside a mouse-reporting program a
  phone had **no selection, no clipboard, no context menu and no way back**
  short of the program exiting, because the long press was suppressed by the
  same guard as the right button.
  Two changes, and each is about a gesture rather than a platform. **The key
  row's sticky modifier now reaches the pointer**, so sticky-Shift then tap
  selects, and it is consumed on use exactly as a key consumes it — one shot
  means one. **And a long press is not the remote's to take**: no terminal
  protocol carries one, so unlike a right-click it is not something a program
  that grabbed the mouse asked for or can act on. The right-click guard is
  unchanged and still correct.
  The general form, because this document was the one that had to give way:
  an override that only one input device can produce is not an override on the
  other, and the rule that assumed it was had answered the wrong question —
  not "who owns the tap" but "what can the user still reach".
- **Every view onto the profile store followed only its own edits.**
  `bssh_profile_store::changed()` had one listener — the session refresh
  above — while three objects hold a copy of what the store contains. So
  with two desktop windows open, adding or deleting a host in one left the
  other's list showing something that was no longer there, and that window's
  next save wrote from a list the store had moved past.
  **Recorded first as a design question and then withdrawn as one**, which is
  the part worth keeping. It looked like a decision about how much
  multi-window state should be pushed versus pulled — and the project had
  already made that decision, in §8.4, for the tab bar: *rebuilt from the
  registry rather than edited alongside it, because two lists kept in step by
  hand drift.* The store is the same shape with a different source, both
  `reload()` functions already existed, and neither touches the edit form. It
  was a wiring gap wearing a design question's clothes, and treating it as
  the latter would have left it open indefinitely for a decision that had
  been taken already.
  The selection is carried across the reload, because the reload can now be
  caused by somebody else: a list that jumps under the user's cursor because
  another window saved would be a new annoyance put in place of an old
  staleness.
  **And it did not fix the two-window case it opens with, which stood as a
  false claim in this document until it was measured.** That wiring joined
  each window's views to *that window's* store, and the window constructed
  its own — `new bssh_profile_store(QString(), this)` — so there was no
  shared object for another window's `changed()` to arrive on. Fixed
  properly by `bssh_profile_store::shared()`; the measurement and the
  credential consequence are in §8.4. Worth leaving both halves visible:
  the entry read as complete because its opening sentence named the
  two-window symptom and its closing sentence named a two-window
  consideration, so the middle — which only ever addressed three views
  inside one window — was never re-read against them.
- **The selection was answerable and never announced — and finding that
  out established that NO announcement this program makes is testable.**
  §14's M11 exposes the selection through the text interface, and
  `selectionCount()`/`selection()` answer correctly whenever asked.
  Nothing emitted an event when it changed, so an assistive client
  learned of one only by re-querying. The whole program emitted exactly
  **one** accessibility event, the text update for damage; this is the
  second. `note_selection_changed()` carries it at all five emit sites,
  so they cannot diverge — the same structural remedy as
  `note_content_changed()` above, adopted deliberately rather than
  discovered again.
  **The measurement matters more than the fix.** Both emitters are
  guarded on `QAccessible::isActive()`, which is false with no client
  attached — deliberately, since flattening the screen for an event
  nobody receives would put that cost on every drag. Under the offscreen
  platform the suite runs on, `QAccessible::setActive(true)` **does not
  take at all**: measured, not assumed, after a test that failed *with
  the fix applied* and a signal spy that proved the path ran. So the
  interface is testable because tests query it directly and bypass the
  guard, while the NOTIFICATIONS are not, and never have been — the
  pre-existing one included. That is how a whole missing event class
  survived M11, and it is worth knowing before anyone concludes from a
  green suite that this area is covered.
  The test is kept and skips with that reason. A skip naming its
  measurement is a marker; one saying "not supported" would be false
  comfort, and this one starts asserting the moment it runs somewhere
  accessibility can be switched on.

- **And the same signal's SECOND consumer was never wired either, so a
  screen reader went on describing dropped history.** The entry below
  fixed `scrollback_changed` for the scrollbar. The accessibility layer
  caches its flattened text against the view's content revision, and that
  revision was bumped by `on_screen_damaged()` and nowhere else — so the
  one content change that damages nothing left a reader holding text for
  lines that no longer existed, with every offset past the trim wrong.
  `characterRect` made it worse rather than better: it asks the view for
  geometry using `history_base()`, which a trim DOES move, so the
  rectangles pointed at the wrong rows as well. Stale text and stale
  geometry, disagreeing with each other.
  **A fix that reaches one consumer of a signal is the shape to distrust**,
  and this is the second time this exact signal has taught it: it had no
  listener at all, then gained one, and stopping there looked like
  completion. Counting the consumers is the only form of the question that
  can fail — the same lesson §14 records for `term` and
  `scrollback_lines` reaching *something* that used them.
  `note_content_changed()` is now the one function both causes call.
  Damage is one cause, a trim is another, and the divergence was two
  copies of "what follows from the content changing" where only one was
  kept up to date.
  **On who is in this condition**, which `working-practice.md` warns gets
  read backwards: it is not a narrow case. It is people who need the
  screen described to them, reading a terminal whose history a per-host
  setting (§10.2) just shortened.

- **The view did not follow the history when the history changed SIZE.**
  `bssh_emulator::scrollback_changed` had no listener anywhere in the tree.
  It went unnoticed because the two paths that fire it most — a line pushed
  off the top, a line popped back — happen to damage the screen as well, so
  `on_screen_damaged()` kept the scrollbar honest by accident. The third
  path does not: `set_scrollback_lines()` trims the ring and damages
  nothing. So lowering a host's `scrollback_lines` while the view was
  scrolled back left the bar advertising history that had been dropped and
  the offset pointing past the end of what was left — blank rows under a
  scrollbar claiming otherwise, until the next byte of output repaired it.
  Wired where the view attaches to its emulator, beside the alternate-screen
  re-clamp, since it is the same rule: what the view may show is the
  emulator's to announce and the view's to obey.
  **A signal kept honest by accident is the hardest kind to notice**, and
  the tell was the listener count rather than any symptom: this was found by
  asking which signals have no connections, not by anything going wrong.
- **The override badge and the write it describes disagreed, in two ways at
  once.** §10 says a host's Appearance page must say whether it follows its
  preset or overrides it, and the badge exists so the screen and the store
  cannot say different things. Both halves of that were false.
  **The touch page never re-read the badge.** Nothing connected the picker to
  it, so choosing a font left it reading `Following preset "X"` while
  `profile()` went on to write a `font_family` override — and the revert
  button is shown from the same badge, so the override just made was
  unreachable until the page was left and reopened. The desktop dialog has
  had that connection since the badge was built; this was the last surviving
  instance of the desktop/touch asymmetry §10 already records once.
  **And the badge weighed less than the write did.**
  `bssh_appearance_overrides()` records the font, the size *and* the colour
  scheme; the badge weighed the font and the size. So changing nothing but
  the colours wrote an override under a badge that said "Following" — the
  same disagreement, one field over, and present on **both** front doors
  because it was in the shared helper rather than in either page.
  `bssh_font_override_badge` is `bssh_appearance_override_badge` now, taking
  the same three fields the writer takes, and the two are commented to
  diverge only together. `scheme_chosen` had no listener anywhere in the
  tree; it has one on each front door.
  **The test asserts the AGREEMENT rather than either value** —
  `badge.overridden` against whether the writer put `colour_scheme` in the
  object — so it does not go stale the next time the write path learns a
  fourth field. And it drives the dialog as well as the helper, because the
  helper agreeing with the writer says nothing about whether anything asks
  the helper: the first version tested only the helper, and the sabotage
  runner reported the desktop connection as MISSED.
- **Split, drop and reorder were all broken at once, reported from use, and
  the suite was green throughout.** One cause between them: **the tests drove
  the model and never the route.**
  **Split did nothing.** Both menu items called `split_with(m_current)`, and
  `m_current` is by construction always in the layout intent — `show_session()`
  puts it there and `m_current` is read back out of the visible panes — so
  `split_with`'s "already showing" guard was unconditionally taken. The action
  was enabled, clickable and silent. The guard is right for its other caller,
  a drop, which must decline a session this window already shows; it was never
  a sensible question to ask of the current session, because a terminal cannot
  go beside itself. Split makes a NEW terminal beside now, which is what a
  split means everywhere else; tiling an existing session stays the tab
  menu's "Show beside current". The existing tests checked the action
  **existed** and never triggered it.
  **Drops never arrived.** `bssh_terminal_view` calls `setAcceptDrops(true)`
  and refused a session payload, with a comment saying it was refused "so that
  it reaches the window". Qt does not work that way: `findDnDTarget()` picks
  the target *before* delivery by walking up to the first widget with
  `acceptDrops()`, and a child that ignores the event does **not** hand it to
  an ancestor. The whole drag was ignored, so `exec()` returned `IgnoreAction`
  and `start_tab_drag` read that as "nobody wanted it" and tore the tab out —
  dropping a tab on another window produced a new window every time, and
  §8.4's tile-by-dropping could not be reached at all. Every drop test passed
  because each sent its events straight at the window, which is the one place
  Qt would never have delivered them.
  **Reorder turned into tear-out.** The tear-out margin measured 40dp past
  `m_tabs->width()`, with a comment saying it used the bar rather than the
  last tab so that "dragging along a half-empty strip is still reordering".
  There is no half-empty strip: the row lays the bar out at stretch 0 with the
  stretch after the "+", so `QTabBar` is exactly as wide as its tabs and that
  measured distance past the **last tab** — the thing the comment said it
  avoided. Dragging a tab to the final position starts inside the margin.
  Measured against `m_tab_row` now, which is the strip the comment meant.
  **The lesson is the coverage, not the three bugs.** A suite that drives the
  model tests what the code computes; only a test that drives the route tests
  what the framework will deliver. All three defects lived in that gap, and
  two of them were *stated in a comment that was wrong about the framework* —
  which is the one kind of error reading the code more carefully cannot find.
- **The wheel changed settings instead of scrolling the page.** Reported as
  "suddenly the text became dark when looking through config menus", and that
  is exactly what it was: a `QComboBox` handles a wheel notch by changing its
  value, whether or not it has focus, so scrolling the device settings with the
  pointer over the **Appearance** row moved it from "Follow the system" to
  "Light" and on to "Dark" -- one notch each, applied immediately by `commit()`
  and written to `app.ini`, so it survived a restart. The text-brightness
  slider one row down is the same accident with a different victim.
  What makes it keep happening rather than happen once: **the page does not
  move**, because the control ate the event. The natural reaction to a page
  that will not scroll is to wheel again, which walks the control further. It
  is not recoverable by scrolling back either, since scrolling back is what
  did it.
  The wheel now always goes to the enclosing scroll area, for every combo,
  spin box and slider on a scrolling form -- not "unless it has focus", which
  leaves the same accident available one click later, and a user who has just
  picked a value from a combo is the user most likely to wheel next. Nothing
  is taken away: those controls are still set by clicking, by dragging, and
  from the keyboard once focused.
  **The page is scrolled by moving its bar, not by forwarding the event.**
  Forwarding was the obvious first version and it recursed until the stack ran
  out -- SIGSEGV on the first notch, measured on a fixture of a combo, a
  scroll area and forty rows.
  **The explanation of that was wrong for a day, and correcting it is the part
  worth reading.** It said Qt remembers which widget a scroll gesture belongs
  to, so a wheel handed to the viewport came back to the control it was taken
  from. Measured since: it does not. A wheel sent to the viewport reaches the
  viewport, the page scrolls, and the combo sees nothing.
  What actually recursed was the filter itself, through the **scroll bars**.
  Qt parents a `QAbstractScrollArea`'s bars to an internal container widget
  rather than to the area, so the exclusion written to skip them -- which
  tested the parent -- never fired, and every bar in the tree was filtered.
  Forwarding a notch made the scroll area send it to its own vertical bar,
  which was filtered, which forwarded to the viewport again. Measured with a
  depth counter: combo, bar, bar, bar, until the stack ended. The exclusion
  tests the widget now.
  Two things follow, and the second is why this is here rather than only in
  the source. The dead exclusion survived because the replacement **cannot
  recurse whatever it collects** -- a fix that removes a symptom can hide the
  thing that caused it, and only re-deriving the mechanism found it. And it
  was re-derived only because the sibling `qtty` reported the same Qt
  behaviour from its own end: until then a plausible mechanism nobody had
  reason to doubt sat in a source comment with the authority of a diagnosis,
  pointing the next reader at the wrong thing.
  The test's third assertion is the load-bearing one -- that the scroll
  position **increased**. A fix that merely swallows the wheel satisfies "the
  combo did not move" and "the setting did not change" while leaving the page
  unscrollable, which is a different bug wearing this one's clothes.
- **Closing one window closed both windows' sessions.** Reported as "when I
  close one window it closes both". `closeEvent` ended with
  `for (session : registry->sessions()) session->disconnect_from_host()` --
  every session in the process, whichever window the user shut. It is the
  same defect §8.4 already names for the tab bar ("closing one anywhere would
  have closed it everywhere"), arriving through the other half of the window:
  the bar had been fixed for two windows and the close had not, because in a
  single-window app the registry and `m_shown` are the same set and the line
  was correct when it was written.
  Two more followed from reading it. **The question was asked of the wrong
  set too** -- `live_count()` is process-wide, so closing an idle window
  raised "a session is still connected" about a *different* window's shell
  and offered a "disconnect and quit" that would have taken it down.
  **And residency was asserted rather than read:** `set_keep_alive(false)`
  unconditionally, when keep-alive is what stops Qt quitting as the last
  *visible* window closes -- so shutting an idle window would have quit an
  app whose other window was hidden in the tray with a build running. It
  comes off the registry now, which is the process-wide fact it was always
  meant to express.
  A window's own sessions are **closed**, not merely disconnected: nothing
  shows them once the window is gone (§8.4, exactly one pane at a time), so
  a disconnect left them in the registry -- counted by the tray, written
  into the saved layout, reachable from nothing.
  The test needed one session live and one not, and the *closing* window is
  the dead one: that is both the sharpest form of the fault (the window with
  nothing running took down the one that had something) and the only version
  runnable unattended, since a live session raises a modal box.
- **The local pty, audited against three ways a sibling's harness had lied to
  it**, offered by `qtty` in the order it met them: a descriptor pointing at
  the wrong terminal (which reads as a *library* defect, and is the dangerous
  one because it produces a plausible wrong answer rather than a failure),
  stdout buffered into the pty (reads as the test never running), and a
  blocking read on a master (reads as a hang).
  **Two do not apply and one was already guarded, which is the honest result
  of the audit** -- `forkpty()` owns the child's descriptors so nothing here
  names a fixed one, and nothing writes through stdio. What was worth reading
  was the guard for the third: `if (flags >= 0) fcntl(...)` proceeded to
  install the notifier on a **blocking** descriptor whenever `F_GETFL` failed,
  and said nothing. The whole point of the flag is that a read on the UI
  thread must not block, so carrying on without it keeps exactly the hazard
  the line exists to remove, and the symptom would be the window freezing a
  long way from here. `evidence.md`'s rule -- a guard's failure must be the
  caller's failure -- so the shell now refuses to open, with a reason.
  It has no test and no sabotage entry: `F_GETFL` does not fail on a
  descriptor `forkpty` has just returned, so the branch is unreachable from a
  test and an entry would report MISSED for ever. Recorded rather than left
  as a silent absence, which is the same call as the wheel guard's exclusion
  and the XTGETTCAP consume.

- **Two menus on the touch front door were half the size a finger needs**, and
  only a real phone showed it. Measured on a Galaxy Note 9 at 420dpi
  (2.625 px/dp): the licences list's rows were **48dp**, exactly the floor its
  item delegate sets — and the host list's overflow menu rows were **23dp**.
  `bssh_host_list` builds two menus, the overflow and the per-host
  Edit/Delete, and neither called the shared touch-height treatment. That
  helper lived on `bssh_main_window`, which the touch front door cannot reach
  without depending backwards on the desktop window — so it is in
  `touch_metrics` now, beside `bssh_apply_touch_heights`, which was already
  the shared home for exactly this.
  §6.2 records this same defect happening once before, to the tab menu, and
  says the helper was pulled out *because* "every context menu in this app has
  to change the same way". Two menus then never called it. **A shared helper
  does not make a rule; a caller does** — and the sabotage entries this
  project relies on cannot see a caller that was never written.
  Confirmed on the device after the fix: the same menu measures **64dp** —
  48dp of min-height plus the 8dp padding above and below.
  It is also the clearest case this session for testing on hardware. Every
  desktop test passed throughout, the suite never dropped below green, and no
  assertion anywhere was about a menu's row height in device pixels.

- **A new `#include` does not update qmake's dependencies, so a later header
  change silently does not rebuild.** qmake scans includes when it *generates*
  the Makefile; the generated file then carries a static dependency list rather
  than reading `-MMD` `.d` files back. Add an `#include` to an existing source
  and that object's dependency list is missing the new header until qmake runs
  again — and the top-level Makefile only regenerates when the `.pro` changes,
  which adding an include does not.
  Measured: after `src/ui/licences_view.cpp` gained `#include
  "model/command_line.h"`, `grep` for that header in the generated
  `licences_view.o` rule returned **0**, and touching the header rebuilt
  nothing. Touching the `.pro` files fixed it and the count went to 1.
  **The symptom is what makes it expensive**, and it is §12.3's own warning
  about stale artifacts arriving as a confusing failure far from the cause: a
  test asserted a string, and the failure printed a *different* string that was
  in no source file in the tree — the previous build's copy, surviving in an
  object nothing knew to rebuild. Two runs went into disbelieving the source
  before the dependency list was suspected.
  It is a property of qmake rather than a defect here, and no rule in
  `build-and-commit.md` covers it: that section is about hand-written
  Makefiles reading `.d` files back, and this is the generated one not having
  been regenerated. The practical rule is narrow — **after adding an
  `#include` to a file, touch the `.pro`** — and it is written here because
  the failure gives no hint of it.

- **Nothing asserted that resizing a pane tells the remote its size.** The
  fake transport has counted window-changes since it was written; the only
  caller that ever read the count is `live_ssh_test`, which needs a real
  server and does not run under `make check`.
  Found by **finishing a lens rather than stopping where it last paid** — the
  emitters that decide *whether* to write, not what. `qtty` found the mirror
  in its own tree on the same day: its backend delivering a resize checked in
  one suite, its router resizing a window in another, and nothing running
  SIGWINCH through both.
  The assertions are a pair and neither is sufficient. That a real change is
  sent is the **feature**; that an unchanged size is *not* sent is the
  **gate**, and that is the half a test written to the feature's name would
  never ask — while being the half that fails silently, since a gate
  comparing the wrong fields swallows real resizes and the remote lays out for
  the old shape for the rest of the session.
  **The sabotage runner then rejected the first version**, and the reason is
  the recurring one: every change the test made moved the *columns*, so a gate
  comparing only columns satisfied all of them. The assertions were a case
  where the right answer and the wrong one agree. A rows-only change
  discriminates — and a terminal made shorter without being made narrower is
  not exotic, it is dragging the bottom edge of a window, or a tab bar
  appearing above every pane.

- **A change detector that did not detect the change.** The accessibility
  layer caches the flattened scrollback-plus-screen text and invalidates it on
  the view's `content_revision` — and only `on_screen_damaged()` moved that.
  So a view pointed at a *different* screen went on reporting the previous
  session's text to a screen reader until something happened to damage the new
  one.
  **Not reachable in the app**, and recording that is the point rather than a
  hedge: `bssh_pane_container::rebuild()` constructs a fresh view per pane, so
  nothing is ever re-attached and the cache is new each time. The contract was
  true by an accident of how panes are rebuilt rather than by anything in the
  view — which is this section's own scrollbar, tracking its history correctly
  because two of three emit sites happened to damage the screen as a side
  effect. **A consumer that is right by coincidence looks exactly like a wired
  one**, and the way to tell them apart is to drive the path the coincidence
  does not cover.
  The lens is `qtty`'s again, sharpened: **ask the question of the GATE, not
  the payload.** Its frame scheduler compared image *counts* to decide whether
  to send a frame, so a picture repainted in place was computed correctly and
  never transmitted — the same fault in the same position, a change detector
  reading a property that does not change when the content does. The payload
  is what tests get written about.
  The test reads once before re-attaching, because nothing is cached until
  somebody asks and a fresh cache reads the new screen correctly — without
  that first read it would pass against the defect.

- **`cursor_style` was serialised four ways and drawn one.** Round-tripped
  through JSON, through the INI, through the preset editor and through the
  settings resolver — and nothing asserted it ever changed what is *painted*.
  The renderer has three branches, block, underline and bar; only the block
  was ever painted in a test, and only by being the default. Two of the three
  had never been drawn at all, in a function edited twice today.
  It is §10.2's own class, recorded there twice already — a setting stored,
  read and applied to nothing — and what made it invisible is that the four
  serialisation layers *are* the well-tested half. Third application of the
  same search key, third find.
  The assertions are on the **shape of the ink** rather than its presence,
  because "there is a cursor" is what the setting's name suggests and passes
  for all three values. A block fills the cell, an underline is a thin band at
  the bottom, a bar is a thin column at the left — measured as a bounding box
  of non-background pixels, so the test never re-derives the renderer's own
  arithmetic for where a column starts.

- **Nothing asserted that pressing a key sends a byte.** The most fundamental
  thing a terminal does. The input router has thirty-two tests — keys in,
  bytes out, every modifier and both keyboard protocols — and the transport
  has `session_records_what_the_client_wrote`, which calls `send()` on the
  fake and checks the fake recorded it. That test's own comment calls itself
  *"the seam M3's input router gets tested through"*, and no router, no view
  and no key event appear anywhere in it. Both halves exhaustively covered;
  the chain between them — key event, view, router, emulator, session,
  transport — covered nowhere.
  The same search key as the image gap, applied a second time and paying out
  a second time: **find the exhaustively-tested translator, then ask what
  carries its output and whether anything asserts the carrying.**
  The test drives real key events at the focused pane and reads the wire. It
  asserts three, and the second two are the discriminating ones: `Ctrl+C` must
  arrive as `0x03`, which no amount of forwarding a key event's `text()`
  produces, and an arrow must arrive as `ESC [ A`, which no keystroke carries
  as text at all. A plain letter alone would pass against a view that wrote
  the event's text straight out with the router cut out of the path.
  Its sabotage found something the tests here had all been avoiding: breaking
  the session's hand-off to the transport failed **no** case before today,
  because every mouse-report test in this tree reads the emulator's own output
  buffer rather than the wire. Those pass whether or not anything carries the
  bytes.
  **A click now has its own wire test, added beside those rather than by
  widening them.** The reason is `qtty`'s and it is worth keeping: those tests
  are a *measurement of where this suite's attention was*, and once they
  assert delivery too, nobody can tell that they never did. The record is
  worth more than the coverage, and the coverage can be added without losing
  it. A click also takes a different route from a keystroke — view, emulator,
  where a key goes view, router, emulator — so only the tail is shared and the
  keystroke test does not cover it. Its assertion is the full SGR report
  including the **cell**, since a report of the right shape naming the wrong
  cell satisfies any "something was sent" check, and the column is exactly
  where a scrollbar's width goes wrong.

- **An early return I wrote an hour earlier left a forked shell behind.** The
  `O_NONBLOCK` guard above was fixed to fail loudly instead of proceeding
  silently — and the `return` that fix introduced sat *below* `forkpty()`, so
  it closed the master and left a child running with `m_child` set, relying on
  a `disconnect_from_host()` nobody calls on a session that has just announced
  it never opened.
  The trap is `qtty`'s, named after it nearly did the same thing while fixing
  its own inert guard: **the instinct the lesson creates is "return on
  failure", and returning is how you acquire the next one.** Its tell is the
  sharp part — the danger is highest where the function has *later* side
  effects that are not obviously part of the same job, which is exactly a
  forked child sitting above a line about a descriptor flag. The child is
  hung up and collected on that path now, `collect()` being bounded.

- **Thirteen image tests and none that looked at the screen.** Every test in
  `graphics_test` stopped at the image store -- bytes in, placement out -- and
  the single test that asserted pixels built its placement by hand and never
  went near a parser. Both halves covered, the chain between them not.
  Found by applying a lens the sibling `qtty` offered after hitting three
  input faults of one shape in one function: **the parse being right is what
  hides the delivery being wrong**, because the parse is the half that is easy
  to assert, and a test proving a decoder decodes is what makes an area look
  covered. It described this file's own gap exactly.
  Not hypothetical, either: the kitty `a=q` defect fixed earlier lived in
  precisely that gap. A capability *query* fell through to the transmit path,
  was decoded as a 1x1 image and placed, and every test here went on passing
  because each was asking the store what it held rather than asking the screen
  what it showed.
  The new test feeds a real escape sequence to a real emulator with a real
  view attached, and asserts the pixel. It samples **before** as well, so what
  is asserted is that the image arrived rather than that the terminal is red.
  **The sabotage runner then refused half of what was written for it**, and
  that refusal is the lens pointed at itself. An entry breaking `images.place()`
  came back WRONG TEST, failing six cases including the parse tests -- which
  already assert the store's contents. It would have claimed for this test
  coverage that thirteen others already provided. The absence is recorded with
  that reason, and what the test genuinely adds is the half nobody had: the
  pixels.
- **A session torn into another window was still wired to the window it came
  from.** The per-session wiring hung off `session_added`, which fires once,
  at creation — so `adopt_session()`, the road a torn-out or dropped tab
  arrives by, wired nothing at all. Every signal kept pointing at the old
  window, and the worst of it was the shortcut router: `handle_shortcut()`
  acts on `m_terminal_view` and has no idea which window asked, so
  **Ctrl+Shift+V in the new window pasted into the old window's session**,
  Ctrl+Shift+C copied the old window's selection, and the new window's own
  chords did nothing. Quieter with it: OSC 52 reached nobody, a connection
  failure printed its error in the window the user was not looking at, and
  "reconnecting in 4s" never appeared anywhere — §5.3's rule that a
  reconnection is only real if it can be seen, exactly inverted.
  One `wire_session()` now, called from both roads, guarded by a set of
  what this window has already wired — **not** `Qt::UniqueConnection`, which
  is refused with a lambda and has cost this project a feature twice.
  **Every existing test passed for the reason that hid it.** The OSC 52 test
  creates the session in the window it then asserts on, so `session_added`
  fires and the window is wired; the tear-out test asserts tab counts and
  registry membership and never asks whether the new window can hear the
  session. The new test adopts from a *different* window, which is the only
  arrangement in which the question can be asked at all.
- **A host edited while it was open reached nothing, and one of the ways it
  failed lost data.** `bssh_session` takes a copy of its profile row at
  construction and there was no way to refresh it — `bssh_profile_store`'s
  `changed()` signal had **no listener anywhere in `src/`**. So an edit made
  while connected was inert until the tab was closed, and three things were
  worse than inert: a reconnect went on dialling the old host, identity file
  and jump host; a later preset edit re-resolved against the **stale**
  overrides and put the reverted override back for good; and storing a
  password took `session->profile()`, set one flag and wrote the **whole
  stale row** back, silently undoing a hostname or port corrected minutes
  earlier.
  Wired once, to the store's own signal, rather than at each of the four
  places that write a row — the desktop dialog, the touch editor, the
  `ssh_config` import and the password write-back. Four call sites are four
  chances to forget, which is the lesson §6.4 paid for already. The password
  path also reads its row back out of the store rather than from the session,
  because it writes a whole row and taking it from anywhere but the row
  itself is how an edit in between gets reverted.
  The test drives the window's **own** store, and says so: a second store
  pointing at the same file is a different object, emits nothing that window
  hears, and would have passed against no wiring at all.
- **`term` had two consumers and only one was wired.** The channel got the
  resolved `TERM` while the emulator's own copy — kept so XTGETTCAP's `TN`
  answers with the same string — stayed at its default. `set_settings()`
  applies that block only when a transport exists, and settings resolve
  before anyone presses connect, so `connect_to_host()` re-applies every
  member of it by hand; this was the one line not copied down. A preset
  naming `screen-256color` therefore put that in the environment while the
  terminal answered `xterm-256color` when asked, which §7.1 calls worse than
  declining to answer.
  **The test that exists to catch exactly this came within one line of it.**
  `every_resolved_setting_reaches_something_that_uses_it` set `term`,
  connected, and checked the transport — the wired consumer. It asks the
  emulator now, the way a client does, through XTGETTCAP rather than a
  getter; and it reads the reply off the transport, because a live session
  drains the emulator into the channel and asking the emulator returns
  nothing, which would have passed against any answer at all.
- **A release answers the PRESS, which it did not.** It hardcoded button 1
  whatever had been pressed, and re-read `Shift` at release time rather than
  honouring what the press decided. libvterm keeps a button **mask**, so a
  release naming a button that was never pressed clears the wrong bit and
  leaves the real one set — after which its "no change" early return swallows
  every later press of that button. **Right-click worked exactly once per
  session** inside a mouse-reporting program, and the remote never saw the
  release at all, so `mc` and `vim` sat holding a button down. The press now
  records what it reported and the release names that; it is deliberately
  *not* re-gated on `mouse_reporting()`, because if the remote turned the mode
  off in between, the release still has to run — libvterm drops the bytes but
  clears the mask first, and that is what stops the bit being stranded.
- **A click reports a LIVE SCREEN row, not a view row.** The mapping went
  through `line_for_row()`, which carries the scroll offset, so a click while
  scrolled back named a row that far down the screen and a mouse-aware program
  on the primary screen acted on the wrong line. `screen_row_for_line()` is
  the conversion, named once, because it is not `line_for_row`'s inverse and
  the difference is exactly the offset.
- **The wheel is the remote's on the same terms, and it was not.** Clicks were
  gated on the mode and the wheel was not, so inside `less`, `vim` or `htop`
  the wheel did nothing whatever — not "scrolled the wrong thing", nothing.
  Those programs are on the alternate screen, where this terminal has no
  history to offer instead, so the gesture had no local meaning either: the
  one place a user most expects to scroll was the one place that could not.
  Buttons 4 and 5, one report per notch, **press only** — a wheel has no
  release, and libvterm will emit one for those buttons if asked, which a
  client reads as a second turn. The position goes first, because libvterm
  reports the wheel wherever it was last told the pointer was, so a wheel with
  no move ahead of it lands where the previous click did. `Shift` overrides,
  the same escape hatch a click has.
- **The alternate screen has no history, and offering the shell's was worse
  than offering none.** libvterm pushes nothing off the alternate buffer, so
  everything in the scrollback belongs to the shell *underneath* the program
  — scrolling back drew that shell's output over the program's screen rather
  than showing anything earlier. The lines are kept rather than dropped:
  leaving the program gives them back, which is what the alternate screen is
  for. The scrollbar's range goes with it, since a bar offering lines the view
  will refuse is a control that does not work.
  A view **already** scrolled back when a program starts is brought forward,
  and that arrives as a signal from the emulator rather than as something
  noticed on the next damage — a program that takes the screen and has not
  drawn into it yet produces no damage to notice it on. It is wired where the
  view is attached to the emulator rather than beside the damage wiring,
  because the rule is the view's own: any caller that did not happen to
  connect `damaged()` would otherwise leave the view holding an offset it is
  no longer entitled to. That seam is not hypothetical — it is how the test
  for it failed first.
- **Selection is anchored to absolute lines**, not to view rows: it spans the
  fold into scrollback, and scrolling must not drag the highlight across the
  text. It renders by **inverting** the cell rather than tinting it — a fixed
  highlight colour cannot stay legible against 256 possible backgrounds.
- **Multi-line paste is a footgun**: a paste containing newlines executes
  commands. Warn on multi-line paste by default (a setting), on both platforms.
  **[BUILT]**, and it had been asked for here and built nowhere: a paste
  carrying five commands ran all five. The rule is `paste_needs_confirming()`,
  a pure decision beside `close_action()` and testable without a message box,
  and every paste goes through one path — the chord, the context menu and the
  middle button were three call sites reaching the router directly, which is
  three places to forget a warning and all three had.
  **More than one LINE of content, not merely a trailing newline.** Copying a
  command with its line ending is the ordinary case, and a warning that fires
  on it is one people turn off — after which it protects nobody from the case
  that matters. Cancel is the default button, so a stray Return dismisses the
  dialog rather than running whatever was on the clipboard, and the dialog
  carries "Do not ask again" so the setting is discoverable where it is
  relevant rather than only on a settings page.
  **It does not know whether bracketed paste is on**, and that would be the
  better rule: with bracketing the newlines do not execute, so the question is
  unnecessary. libvterm handles the mode internally and exposes no way to ask —
  checked against the vendored headers, which have no getter — so the warning
  is unconditional and the setting is how somebody who lives inside bracketed
  paste turns it off.

**OSC 52 had never once run, and neither had the settings handler beside it.**
Both were wired with `Qt::UniqueConnection` and a lambda — a combination Qt
**refuses**: in a release build it warns and hands back an invalid connection,
and in a debug build it asserts. Measured with a ten-line probe against this Qt
rather than taken from the warning text: `connection valid: false`, handler ran
`0` times. So the preset switch existed, the emulator emitted the signal, and
nothing was listening; a host the user had deliberately trusted could not set
the clipboard, and a preset edit never reached the key row, the geometry or the
status line. Appearance survived only because `bssh_pane_container` makes its
own per-view connection.

This is the **second** time that combination has cost this project a feature.
The first was the link prompt and the primary-selection copy, wired the same
way, and the `m_wired_view` guard written to fix them sits twenty lines below
the two connections that still had it. That first occurrence was **never
written down anywhere** — this sentence used to say §8.4 recorded it, and §8.4
says nothing about it; nor did any other section, which is a fair part of why
the lesson did not carry the twenty lines it needed to. The lesson: `UniqueConnection`
is not a way to make repeated wiring safe, it is a thing that only works with a
pointer-to-member. **Wire once, where the object arrives once.** Both moved to
the registry's `session_added`, which fires exactly once per session, so no
uniqueness trick is needed at all.

Only the window **showing** a session honours its clipboard request. A session
is on screen in exactly one place (§8.4), so one remote request produces one
clipboard write and one line of status rather than one per window that has ever
shown it.

**Dropping a file pastes its path — [BUILT].** Every desktop terminal since the
1990s does this, it is how most people move a long path into a command line, and
this one dropped everything on the floor. What arrives is text, so it goes
through `paste_into_current()` like every other paste: bracketed paste applies,
and the multi-line question above is asked. Four decisions, none of them free:

- **Shell-quoted, and only when it needs to be.** `/etc/hosts` pastes as itself;
  `/home/nabbe/my notes.txt` gains single quotes. The escape for an embedded
  quote is the standard close-reopen (`'\''`), because a filename that ends the
  quoting early hands its own tail to the shell **as code** — a file called
  `';rm -rf ~;'` is a legal filename and dropping one must not be an execution.
  Quoting unconditionally would be safer still and is rejected: a quoted path
  cannot be tab-completed or edited comfortably, and the common case is a path
  with nothing wrong with it.
- **Several files join with spaces, never newlines.** They are arguments to one
  command. Newline-separated, each would *run*.
- **A URL wins over the text beside it.** File managers attach both, and the
  text is `file:///home/nabbe/notes.txt`, which no shell can act on. A *remote*
  URL is pasted as it stands — the machine at the far end may well be able to
  fetch it, and rewriting it to a local path would be a guess about a filesystem
  this app is not on.
- **A session payload is refused here**, so a tab dragged from another window
  falls through to the window, which is what knows how to adopt one (§8.4).

The drop takes focus the way a click does, because the pane under the pointer
need not be the focused one and the window pastes into the session it considers
current — without that, a drop on the left pane lands in the right one.

**The test asserts the wiring, not a synthetic event, and that is measured.**
A `QDropEvent` sent with `QApplication::sendEvent` never reaches `dropEvent`:
checked with a twenty-line probe against a plain `QWidget` with
`acceptDrops(true)`, which counted **zero** calls, because `QApplication::notify`
wants a live drag session behind it. The first version of the test did exactly
that and would have passed while asserting nothing — the vacuous-pass shape
§12.3 keeps finding. So the test covers what is ours: the payload-to-text
mapping, and that a `paste_requested` reaches a fake transport as bytes.

**It also cost three unrelated tests, which is the more useful finding.** The
scrollbar's default position is a process-wide static, a window built by a test
sets it from the settings, and it stays set — so adding one window-building test
gave every later view in the file a scrollbar it never asked for, and three
geometry tests failed reading as grid-arithmetic bugs. The fix is a `cleanup()`
slot that resets it after **every** test in the class rather than after the
guilty one: the next window-building test would otherwise rediscover this the
same way, and the failure points nowhere near its cause.

**[BUILT] The terminal menu carries the session verbs too.** Split, New Tab and
New Window sat only on the TAB bar's menu — a different target, and one a window
with a single session hides entirely, so from a lone terminal there was no route
to a split but a chord nobody had been told about. Reported as exactly that.
They come after the edit verbs and a separator, in the order the rest of this
menu already follows, and Split is shown disabled rather than hidden when the
size class has no room for another pane.

**The terminal's own context menu — [DECIDED].**
Right-click on the desktop, press-and-hold on touch: **Copy, Paste, Select All**.
There was none at all before — the tab bar and the host list had menus and the
thing the app is *for* did not, and right-click in the terminal did nothing
whatsoever.

**The desktop terminals of 1995-2010 never agreed about the right button**, and
that disagreement is the design question. xterm gave it selection extension and
put its menus behind Ctrl+click; PuTTY made it *paste*, in the Windows console
tradition, and offered a three-way "Action of mouse buttons" setting because
even it could not pick one; Konsole and GNOME Terminal put a menu there, and by
the mid-2000s that had won on the desktop this app is built for. So: a menu —
and PuTTY's answer to the disagreement, *a setting*, is the part worth copying
rather than its default.

**The order and the details are the Windows Interface Guidelines' (1995)**,
which all of those programs were written against and which have not been
improved on: edit verbs first in their canonical order, object-specific
commands after a separator, access keys on every item, and **nothing hidden** —
Copy with no selection is shown disabled rather than removed, so the menu is
the same shape every time and the hand learns where Paste is.

**Touch changes the metrics and nothing else.** iOS and Android converged on the
same three verbs from a long press, with much larger targets, so the rows grow
to §6.2's 48dp floor and the keyboard shortcuts drop off the labels — they are
telling somebody with no keyboard which chord to press. Which verbs they are,
and in what order, does not change between the two. Since that is *all* touch
changes, it is one function (`apply_menu_layout`) applied to every context menu
this window raises, rather than a rule written out inside each menu builder —
the tab menu had pointer-height rows until it was pulled out.

**Press-and-hold is ours, because Qt does not provide it.** Qt raises
`customContextMenuRequested` from a right-click and does **not** synthesise a
context-menu event from a long press on Android — checked on a device, where
holding a host-list row simply opened that host. So the touch half of §8.3's
"long-press (touch) or right-click (desktop)" had no implementation at all,
here or on the host list, for as long as that sentence has been in this
document. `bssh_long_press` is one event filter used by all three surfaces that
carry a menu — the terminal, the host list and the tab bar.
Two things it had to learn on a device. **It fires when the finger LIFTS, not
when the hold matures**: a popup opened under a finger that is still down is
dismissed by that finger's own release, so the menu appeared and vanished
inside the same gesture and every screenshot showed nothing. And **it consumes
that release**, or the widget behind it treats the gesture as a click too —
which is how holding a host row opened the host, and how holding in the
terminal left a selection behind the menu.

**Switchable, defaulting per device** (`Terminal menu` under This device):
Automatic / Pointer-sized / Touch-sized, the same three-state shape the key row
and the theme use. Automatic asks `is_touch_primary()`, which is what makes the
default differ between a desktop and a phone; the override exists because one
device can be both.

**Middle-click pastes the primary selection**, X11's own convention and older
than any of these menus. The *publishing* half was already there — selecting
text made it available to other programs — so the convention worked in one
direction and looked broken in the other. It pastes PRIMARY, never the
clipboard: conflating the two is what makes middle-click surprising.

**A program that asked for the mouse keeps the right button.** No menu is raised
while mouse reporting is on, since a full-screen program that requested button
events is entitled to them; Shift is the conventional override there, exactly as
it is for selection.

---

## 7. Terminal emulation

- Implement/adapt a **VT100/VT220/xterm** emulator: parser (ground/escape/CSI/OSC
  states), screen buffer (primary + alternate), scrollback ring buffer, SGR
  colors (16 / 256 / truecolor), UTF-8, cursor addressing, scroll regions,
  bracketed paste, application cursor/keypad modes, and OSC title.
- **Do not** reuse `qtermwidget` directly (Konsole-derived, assumes a local PTY
  and desktop conventions). It would look like the obvious shortcut for the
  desktop build specifically — taking it would mean two different emulators
  across platforms, which is the worst possible outcome for a project whose
  whole premise is one core.
- **[DECIDED] Vendor `libvterm`** (C library, PTY-agnostic parser) and write only
  the Qt rendering + input on top. Pinned as a submodule and built from source,
  which is what makes one answer serve both platforms — there is no system
  libvterm in an NDK sysroot, so a distro package could only ever have solved
  the desktop half. The alternatives considered and rejected: writing a focused
  emulator ourselves (most control, but weeks of edge cases libvterm already
  handles), and porting a lightweight VT widget (faster start, gaps on advanced
  sequences).
- **libvterm integration constraint, learned the hard way:** it does **not**
  tolerate a UTF-8 sequence split across two `vterm_input_write()` calls — the
  halves decode to U+FFFD. SSH reads split wherever the network put a packet
  boundary, so the binding holds an incomplete trailing sequence back until the
  rest arrives. Any future parser swap must keep doing this or non-ASCII output
  breaks intermittently, which is the worst way for it to break.
- **Scrollback** is a **bounded ring of cells**, not of text — scrolled-back
  output keeps its colours, which is the point of having it. The cap is not
  optional: a terminal left on a chatty log is the normal case, and on a phone
  an unbounded buffer is the difference between working and being killed. Size
  becomes a preset-owned setting at M10 (§10.2).
- **The alternate screen must be explicitly enabled** in libvterm
  (`vterm_screen_enable_altscreen`) — it does not allocate the buffer
  otherwise, and DECSET 1049 then silently does nothing, so `vim` and `less`
  redraw over the primary buffer and destroy the shell output beneath them
  with nothing to restore on exit. Found by a scrollback test that noticed an
  editor's redraws in history; the visible symptom is much worse than the one
  that caught it.
- **Rendering:** custom `QWidget::paintEvent` drawing a monospaced grid; cache
  glyph runs per row. See §9 for the font pipeline, fallback chain, and
  double-width (CJK) cell handling.
- **Resize:** on view resize / font change / **pane split or collapse**, recompute
  cols×rows and send `window-change` **for that session only**.
- **OSC title** feeds the tab label (§8.3) — wire it through `bssh_session`, not
  the widget. On desktop it should also drive the **window title**; that is a
  platform-layer call, not something the emulator knows about.

### 7.1 Modern terminal extensions — **[DECIDED to support, tiered]**

VT100/xterm is the floor, not the target. Terminals have moved a long way
since, and the extensions below are what a 2026 terminal is expected to
speak. **Support is tiered by value-per-effort, not by novelty.** The list
exists because most of these are unfamiliar unless you have implemented one:
each says what it *is*, who needs it, and where it stands here.

**Tier 1 — a terminal is broken without these.**

| Extension | What it is | Status |
|---|---|---|
| **Truecolor** (SGR 38;2;r;g;b) | 24-bit colour instead of a 256-entry palette | **Done** — §7, carried per cell |
| **Bracketed paste** (DEC 2004) | Wraps pasted text so a shell knows it was pasted rather than typed, instead of executing every newline | **Done** — §6.4 |
| **SGR mouse** (1006) | Mouse reports that work past column 223, unlike the original X10 encoding | **Done** — libvterm encodes, §6.4 decides local-vs-remote |
| **Focus reporting** (1004) | Tells the remote when the terminal gains/loses focus, so editors can reload changed files | **Done** — wired per pane, see below |

**Focus reporting was the only tier-1 gap, and it was wired to nothing.**
libvterm has parsed mode 1004 and carried `vterm_state_focus_in/out` since it
was vendored; no code in this project ever called either, so a remote could
enable the mode and never hear a thing — indistinguishable, from the far end,
from a terminal that does not support it. What wants it is an editor reloading
a file changed underneath it: `vim`'s `autoread` does nothing useful without it,
because it only looks when something tells it to.
Reported **per pane**, because that is what the remote is asking about: an
editor in one pane wants to know when *that* pane is being looked at, not when
some other one is. It hangs off the view's focus events rather than the pane
container's own idea of focus, which means Qt's focus-out on **window
deactivation** carries it too — switching to another application reports
focus-out, which is exactly the case mode 1004 exists for.
Silent unless the remote asked, and that half is tested as carefully as the
other: a terminal that volunteers `CSI I` to a shell which never enabled 1004
is writing stray bytes onto that shell's command line.

**Tier 2 — keyboard disambiguation. This is the big one.**

Legacy terminal input cannot express what a modern keyboard sends. `Ctrl+I`
and `Tab` are the same byte. `Alt+x` and `Escape` then `x` are the same two
bytes, and *no* decoder can separate them — which is why terminal apps have
an ESC timeout at all. Three tiers exist, and a terminal should offer the
best one it can:

- **Kitty keyboard protocol** (`CSI > flags u` to push, `CSI < u` to pop,
  `CSI ? u` to query). Progressive enhancement flags; the first,
  "disambiguate escape codes", is the one that matters. **Implemented** —
  the disambiguate tier, plus the push/pop/query/set controls.
- **xterm `modifyOtherKeys`** (`CSI > 4 ; 2 m`, queried with `CSI ? 4 m`).
  The middle tier, and what a terminal that predates kitty's protocol
  offers. **Implemented at level 2.** Level 3 is deliberately not offered:
  it encodes *unmodified* keys too, and XTQMODKEYS reports only the current
  value rather than the maximum supported, so a client asking for 3 and
  silently getting 2 could not tell the difference.
- **Legacy** — what everything falls back to, with its ambiguities intact.

The sibling `fuzzypickles` TUI probes exactly these tiers from the client
side (see its `gui/project.md`); BeerSSH is the other half of that
conversation, and should let it reach its best tier.

**Tier 3 — worth having, and all four are now built.** The heading said "not
yet built" over a table of four **Done** rows for as long as the last of them
took to land, which is the shape of staleness this document is most prone to:
a summary line that stops being true while the detail under it is updated.

| Extension | What it is | Status |
|---|---|---|
| **Synchronized output** (DEC 2026) | Remote brackets a screen update so the terminal paints it in one frame instead of mid-redraw; removes tearing in full-screen apps | **Done** — with a 150 ms safety release, since a program that sets it and dies would otherwise freeze the display |
| **OSC 8 hyperlinks** | Marks a run of cells as a clickable URL | **Done** — per-cell link id, underlined, opened on Ctrl+click after a confirmation |
| **OSC 52 clipboard** | Remote sets the local clipboard | **Done, write only.** See below |
| **XTGETTCAP / DECRQM** | Lets a remote query what the terminal supports rather than guessing from `TERM` | **Done.** See below |

**DECRQM, and how little of it is ours.** libvterm answers DECRQM itself for
every mode it models — the cursor, autowrap, each mouse mode, bracketed paste —
and it knows their state better than the layer above does, so almost nothing
belongs on this side. A second opinion here would be the "two places computing
one thing" this project keeps having to unpick. What libvterm *cannot* answer
for is a mode it does not model, which is exactly the set an application
implements on top of it: **DEC 2026, synchronised output, is the live example
and the one clients actually probe.**
And it was answering **"0 — not recognised"** for it, which is worse than
silence: a client probes so it can use a feature without guessing from `TERM`,
and told 0 it takes the terminal at its word and falls back. The implementation
behind it was never reached. `request_dec_mode()` answered and returned without
ever consulting the fallback — the same defect `0002` had already fixed for
`set_dec_mode()`, on the other half of the same feature, quietly undoing it.
**`vterm-patch/0004`** hands the query to the fallback in its own shape
(leader `?`, intermediate `$`, command `p`) before answering 0. Worth sending
upstream alongside 0002.

**XTGETTCAP answers only what is true of this terminal wherever it runs** —
`TN`, `Co`, `RGB` — and returns the invalid form for everything else rather
than guessing. A capability database is a promise: a client told `bce` will use
it, and a terminal that answered by pattern-matching what terminals like it
usually support has broken the program that believed it. `TN` reports the TERM
the app actually told the remote, mirrored into the emulator for the purpose,
because a terminal that names itself one thing in the environment and another
when asked is worse than one that will not say.
One thing had to be fixed to reach it at all: sixel and XTGETTCAP are both DCS
sequences ending in `q`, and the sixel handler tested only that last byte — so
`DCS + q` was being fed to the sixel parser, decoding to nothing and answered
with silence.

**OSC 52, reviewed.** The write half is offered **per host and off by
default**, because the useful case (yanking in vim over SSH and pasting
locally) and the hazard (a compromised host replacing what you think you
copied, then waiting for you to paste it) are the same mechanism pointed at
different machines — which makes it a question of trust in *that host*, and
trust in a host is what a preset already expresses. Every honoured write says
so on the status line: a clipboard that changed without the user acting is the
entire risk, and the legitimate case is one they just caused deliberately.

**The read half is not implemented and will not be.** A query asks the terminal
to transmit the local clipboard to the remote, turning every connection into an
exfiltration channel for whatever was last copied — a password, a token,
someone else's message. No setting makes that acceptable, because the risk is
not the user's to accept on behalf of something they copied ten minutes ago and
have forgotten about. The query is answered with silence, which is what a
terminal without the feature does.

Payloads are capped at 64 KB and an oversized one is dropped **whole**: the
bytes are attacker-controlled, and half a clipboard is not a smaller version of
what was asked for — it is a different string that someone might paste.

**Switching them off, selectively — [DECIDED]**

Asked for by the author of a TUI library, and the reason is the one this
section could not have supplied for itself: a library has to work against
terminals that *lack* each of these, and the only honest way to find out is to
run it against a terminal that really lacks them rather than against the
library's own idea of one. `--term-features=-sixel,-kitty-graphics`, or
`--term-features=none,+bracketed-paste` for a terminal with almost nothing.
`--term-features=?` lists the names. The same string is a preset field, so a
host that always wants a reduced terminal says it once.

**The rule that makes it worth building: switching a feature off changes what
the terminal ANSWERS about it, not only what it does.** This section has
already paid for both polarities of getting that wrong — DEC 2026 answering
"not recognised" while the implementation existed, and the warning above that
"a capability database is a promise". A feature that is off but still
advertised is the more expensive direction, because the client is told to go
ahead and then nothing happens. So each switch silences the probe in that
protocol's own spelling: DECRPM `0` for 2026, which is a real machine-readable
*no*; silence for kitty's `CSI ? u`, for XTQMODKEYS and for XTGETTCAP, because
those have no negative form and silence is what a terminal without them does.
Answering kitty's query with zero flags would say something different and
wrong — that the protocol is spoken and sitting at its lowest tier.

**All twelve switch, including the three libvterm models** — bracketed paste,
the mouse family and focus reporting. Those were refused at first, because
libvterm parses the mode, holds the state and answers DECRQM from it, so
suppressing only the behaviour would leave this terminal reporting a mode as
present while ignoring it.
**The fix was not a new patch but a two-line move in the two we had.** `0002`
and `0004` already existed to hand a DEC-mode set and a DEC-mode query to the
application; they simply did it in each function's `default` branch, which is
reached only for a mode libvterm does *not* model. Consulting the fallback at
the **top** of each function subsumes that case exactly — an application that
declines gets libvterm's own handling, unchanged — and adds the one this
needed: an application can now refuse a mode libvterm *does* model. Looking
for the smallest new patch found a smaller change than a patch.
**The answer has to be `0`, not `2`.** DECRPM `0` is not-recognised and is the
only definite no; `2` is recognised-but-currently-reset, which leaves a client
believing the mode exists and enabling it again. That distinction came from
the client end — qtty acts on it — and a veto that produced `2` would have
been no better than none.
**Verified from the client end, 2026-08-31, and it supplied the argument
this section was missing.** The paragraph above pins `0` against `2`; qtty
pins it against *silence*, which is the third option and the one a uniform
reading of "a switch silences the probe" would produce. Its `mode_usable()`
treats silence as "learned nothing, keep the assumption you arrived with"
and `0` as a definite no — so a terminal going quiet on a disabled DEC mode
leaves the client believing whatever it believed before, while answering `0`
actually turns the capability off. **Silence for a claim, a definite no for a
question that has one.** DECRQM is the only probe here that has one, which is
why it is the only carve-out — and the two arguments together close all three
alternatives, which neither does alone.
The carve-out is defended by name rather than by this paragraph:
`a_mode_libvterm_models_can_still_be_declined` asserts 1006, 1000, 2004 and
1004 each answer `;0$y` with their own feature off, and
`a_switched_off_feature_is_not_advertised_either` does the same for 2026. A
later pass applying silence uniformly fails those, which is the point of
writing the exact byte rather than "not empty".
**The measurement, so it can be re-taken.** `qtty-negotiate --probes` reports
each probe answered or silent and prints the raw bytes — an instrument built
because its ordinary output deliberately cannot tell the two apart. Totals:
568 B with everything on; `-kitty-graphics` drops exactly the 12-byte `a=q`
OK; `-sixel` turns DA1 `?1;2;4c` into `?1;2c`; `-xtgettcap` drops both DCS
replies; `-window-size` drops `CSI 14t` and `16t`; `-colour-query` drops OSC
11 and all sixteen OSC 4 replies, leaving 121 B; `none` leaves 51 B. Nothing
answered-but-inert anywhere. The byte count is the evidence rather than the
pass/fail, because a reply that *shrank* rather than vanished would still be
a reply.
**And the scope of it, which the first version of this paragraph left
out.** qtty re-checked its own provenance and found its beerssh binary
dated 2026-08-30 17:35, so the run covers the feature switches and the four
probe fixes — which landed in `3525de0`, earlier — and says **nothing about
`a8b4187` or anything after it.** Their document had named a commit inferred
from the binary's mtime, which is a bare commit reading as provenance it does
not have; the honest form is that the binary postdates `395f354` and predates
`71eefea` *unless it was built from uncommitted work*, which nothing on
either side can rule out. Recorded because a verification without its build
is a fact without its method, and this one would otherwise be quoted as
covering commits it cannot see. Re-take it when the emulator, the negotiation
or the vendored patches move — not when the build tooling does, which is all
that has changed since.
The mouse family is **one** switch (1000, 1002, 1003, 1005, 1006, 1015).
Gating the SGR encoding alone would let a client enable tracking and then find
no encoding it could use, which is a stranger terminal than either having a
mouse or not having one, and not a terminal anybody has tested against.
An unknown name is still refused, and the message lists what would have been
accepted.

**The command line outranks the preset here**, which is the only place in this
project it does. A reduced terminal is an instruction about *this run*, typed
by whoever is running the test, so it applies to every session the run opens
including tabs opened later, and a stored preset must not quietly restore a
feature the tester asked to be without.

**Four advertise/implement mismatches were found while surveying for this.
Two are fixed and two are open**, and all four are listed because each
misleads a client that probes — which is the population this switch exists to
serve.

**The counterpart TUI is `qtty`**, which renders an unmodified Qt Widgets
application on a character-cell terminal, and it is where two of these were
found.

**What a consumer on the other side CANNOT check, and this bounds the whole
arrangement.** Named from the other end: a client comparing a reported colour
against pixels that arrive through the same terminal has no way to see a
divergence — a palette reported wrongly looks exactly like a theme. The same
holds for the cell size, where a client scales to whatever it is told and the
picture is simply slightly wrong. So having a real consumer does not cover
these and never will; the report-versus-paint invariants are only checkable
from **inside** this tree, and `tool/sabotage/report-matches-paint.toml` is
what does it. Every entry there breaks a *connection* rather than a function,
because each function is individually correct and a test exercising one alone
stays green.
Two things came out of writing it. The first probe sampled three pixels in
from the bottom right and read `f3f3f3` against a reported `000000` — the
**scrollbar**, a child widget drawn in the application palette. The probe was
measuring a place the fault could not appear in and would have failed
identically against correct code. And the cell metrics reach the emulator by
**three** routes, so breaking any one call site reports MISSED: the invariant
is "the reported cell tracks the view's", not "this line runs", and the
sabotage had to break the setter to say so.

**The two projects coordinate directly, by the copyright holder's
instruction** — session to session, rather than by each writing notes the
other may not read. The division of labour is `harmonization.md`'s and it is
worth stating here because this pair will keep using it: **qtty holds the
reproduction and BeerSSH holds the reasons.** A probe that gets a wrong answer
is reported from the end that sent it, citing the sequence and what was
expected back, and it is treated as a defect *here* — a terminal is the party
that decides what is true about a terminal. Neither tree edits the other; a
fix that is separable travels as a patch against the receiving tree, which is
the same route `vterm-patch/` already uses upstream.
**Confirm the counterparty before relying on an answer.** `evidence.md`
records a project that asked another for a wire layout, was answered helpfully
several times, and had been addressing a tree that owns no layout at all — the
failure mode is politeness, not noise, so the quality of the replies is
precisely what suppresses the question. The cheap check is to ask the other
side to name the thing only it can own: for qtty, the file its probes live in
and the function that decides a tier. Reading the consumer rather than this tree is what found them: qtty
probes, and both answers were wrong in the same direction — a feature this
terminal *has*, reported as absent. §7.1 already calls that worse than
silence, because a client takes the terminal at its word and falls back.
(§7.1 previously named `fuzzypickles` as the client-side prober. Both are
real; `fuzzypickles` probes the keyboard tiers, `qtty` the graphics and mode
tiers.)

- **Kitty graphics had no `a=q` reply — fixed.** The query fell through to the
  transmit path, which decoded its four bytes into a 1x1 image and placed it,
  so the probe produced no reply at all and left a stray pixel behind. It is
  the sharpest case of this whole class, because kitty graphics has **no
  terminfo capability and no DA parameter**: the query is the only way a
  client can find out. qtty sends `a=q,t=d,f=24,s=1,v=1` from
  `src/backend/ansi/term_caps.cpp` and looks for the reply in `find_kitty()`,
  so every application it rendered was falling back to half-blocks against a
  terminal that can draw pixels. The test uses qtty's own bytes rather than a
  probe composed here, since a probe written from this side would be testing
  what we *think* a client sends — which is the assumption that produced the
  gap.
- **`CSI ? 1049 $p` answered "not recognised" — fixed.** libvterm implements
  1049 and its DECRQM table stops at 1047. It costs more than it looks: 1049
  is what a full-screen program uses to avoid destroying the user's
  scrollback, so a client told the terminal lacks it either gives up the
  alternate screen or falls back to clearing the primary one. Answered from
  the mirror this class already keeps, rather than by asking libvterm a second
  way.
- **XTVERSION answered `DCS >|libvterm(0.3) ST` — fixed** by
  `vterm-patch/0006`. It named the library rather than the program the user
  ran, which is precisely what XTVERSION exists to prevent, and anything
  keying off it believed it was talking to libvterm. The test asserts the
  **name** and not the version string: the version moves with every release,
  and a test that pinned it would fail for the one reason that is not a
  defect.
- **`Tc` was declined while `RGB` was answered — fixed.** They are the same
  claim in two spellings, both true of this terminal (§7), and a client that
  asked the second way was told there was no truecolor. It surfaced only
  because qtty asks for both in one query and reported back what each
  answered — which is what a consumer is for. `terminfo_capability()` could
  not express a *boolean* capability at all before this: it returned a value
  and treated empty as "no", so the only two answers available were a string
  and a refusal. A flag now replies with the name and no `=value`, which is
  what terminfo means by one.
- **DECSCUSR and DECSCNM are answered by DECRQSS/DECRQM but have no effect** —
  open, and a design question rather than a defect to fix in passing. Cursor
  shape comes from the preset (§10.2); whether a remote gets to override it is
  the same question §7.1 asks about every other negotiable property, and it
  interacts with the cursor rules recorded in §10.2. Reverse video is simply
  unconsumed.

**DA1 is the other half of the graphics story — fixed by the same patch.** It
answered `?1;2c`, with no parameter 4, so a client probing for sixel the
conventional way was told there was none while this terminal decodes it; qtty
reads DA1 for exactly that parameter and was compositing in half-blocks
against a terminal that can draw pixels. It answers `?1;2;4c` now, and **not**
where the build has no decoder — the Android build claims nothing, because
claiming 4 there would be this same defect pointed the other way.
DA1 is also the one advertisement here that is a **positive claim** rather
than an answer to a direct question, which makes it the one most able to
outlive what it claims: it stops offering 4 when `-sixel` switches sixel off,
and that pair has its own sabotage entry.

**Two more questions are answered because qtty asked for them by name**, and
both were on the silence list above. Neither is required of a terminal;
each is cheap, true of this one, and ranked by the party that needs it —
which is a better argument than novelty and is why they were built ahead of
the tier-1 switches.

- **OSC 10 and OSC 11**, the terminal's own foreground and background. Every
  graphics tier below kitty composites an image's alpha against the terminal
  background, and with no answer qtty assumed dark grey — which haloes every
  translucent edge on a light theme. A visible artefact caused entirely by an
  unanswered question. The reply is xterm's `rgb:RRRR/GGGG/BBBB` with each
  channel **doubled** to sixteen bits rather than shifted, or `0xff` comes
  back `0xff00` and white is not quite white. Only the `?` form is answered:
  a payload that *sets* a colour is a remote repainting the terminal it was
  given, and that is not a power this grants.
  The colours come from the **view**, told to the emulator beside the cell
  metrics and on the same events, because they are the same kind of fact —
  something the view knows that the emulator has to answer for. They are the
  colours after the contrast floor and the brightness slider, since a client
  compositing against the background wants what is on screen rather than
  what is in the scheme file.
- **OSC 4**, what colour a palette index is — ranked first on the asker's
  second pass. Indices 16–255 are a formula every terminal computes
  identically; **0–15 are the user's scheme**, and that is the half a client
  cannot derive, so asking is the only way to find out.
  **The justification this was built on was too broad, and the correction is
  qtty's own.** It was requested as "every quantised colour I emit is computed
  against the wrong table". Reading its own code before wiring ours in, it
  found that its 256-colour matcher builds its candidate list from 16–255
  *only* — the low sixteen are deliberately excluded, with a comment saying
  why — so a user's scheme cannot affect that path at all. What it affects is
  the 16-colour fallback, which picks the nearest of the sixteen for a colour
  carrying no authored role: a documented last resort rather than the primary
  route. Narrower than claimed, and still real — picking "nearest" against the
  wrong sixteen is how a fallback lands somewhere the user can see is wrong.
  Recorded because the overstated version was written into this document
  first: a number taken from another tree is a measurement not taken here, and
  it was repeated rather than checked.
  Verified end to end from that side afterwards, with the binary's mtime read
  before *and* after the run: our default scheme puts index 1 at `#aa0000`
  where qtty's built-in table assumed `#800000`. A default build differs on
  the very first index, so the difference shows on an ordinary run rather than
  an exotic one.
  Answered as the palette **is**, before the brightness slider dims anything:
  the slider is a per-cell display adjustment applied to a foreground but not
  to the terminal's own background, so there is no single dimmed value an
  index *has*. OSC 4 means the mapping from index to colour.
  The 16–255 formula is now **shared** with the renderer as
  `bssh_palette_colour()` rather than copied: a client quantising into this
  palette relies on what we paint and what we report agreeing exactly, and
  two copies of a formula is how they stop.
- **`CSI 14 t` and `CSI 16 t`**, the text area and one cell in pixels.
  Without the cell size an image's aspect ratio cannot be preserved: a
  half-block pixel is one cell wide and half a cell tall, so assuming 1:2
  squashes every picture on a terminal whose cells are not that shape. Height
  comes before width, which is xterm's order and the reverse of everything
  else here.
  **Every other window op stays silent, permanently.** The rest of that
  family moves, resizes, raises and iconifies the window, and a remote host
  does not get to rearrange the user's desktop — reporting a size and acting
  on one are different powers that happen to share a final byte. These are
  the first `CSI t` this terminal has ever answered, and the cheapest way to
  grow that into something worse is a later `t` handler written beside them,
  so the silence has its own assertion and its own sabotage entry.

All three are switchable: OSC 4, 10 and 11 together as `colour-query`, and
the two window reports as `window-size`.

**What remains unanswered is unanswered because nobody needs it**, which is a
better reason than a rule. Asked to rank the rest, qtty replied that OSC 12,
OSC 7, DA3 and the title stack cost it nothing today — it has no title
emitter at all, and it lets the terminal colour its own cursor. That is the
shape a request should take here: the sequence, what the silence costs, and
what answering buys.

**The tier-1 switches were deferred once, at the asker's request, and that
round trip is the clearest argument in this exchange for asking before
building.** qtty first replied that switching bracketed paste, the mouse modes
or focus reporting off would not change its behaviour at all, because it did
not ask the terminal about them — it inferred them from whether it got raw
mode. Building then would have made this terminal honest for a client that was
not listening. qtty closed that gap, came back with the requirement above,
*and* ranked them: `sgr-mouse` first, because a client that believes it has
SGR mouse and does not gets no input at all. The work was then worth doing and
took a two-line move instead of a patch.

**Two practices came out of the exchange and are worth keeping.**
- **Any figure either side quotes about the other needs a timestamp and the
  other binary's mtime — checked again *after* the run, not only before.**
  qtty twice measured this terminal while it was being rebuilt underneath, and
  the second time nearly filed a parser bug that did not exist — its tool
  reported our background colour unanswered while a capture minutes later
  showed it arriving, because we had gained OSC 11 in between. Neither reading
  was wrong about what it saw; both were about a binary that no longer
  existed. The after-check is qtty's addition and is the half that catches it:
  what fooled it was a rebuild landing *during* a run rather than before one.
- **A specific wrong cause is worse than a vague one.** qtty attributed a
  five-per-cent aspect error to its own compositing function, then corrected
  itself: the function references cell metrics nowhere, and the cause is its
  internal cell size differing from the one this terminal reports. The
  measurement was right and the mechanism was not — `evidence.md`'s rule, sent
  back the other way, and the reason the correction was worth a message rather
  than a quiet edit.

**`vterm-patch/0006` is what made both reachable**, and it is the third patch
of the same shape: libvterm answers a question the layer above it knows
better, and answers it with the one reply that stops a client asking again.
`0002` did it for setting an unmodelled DEC mode, `0004` for querying one, and
`0006` for the two questions about *which terminal this is*. All three hand the
sequence to the fallback in its own shape and answer as before only when the
fallback declines. Worth sending upstream together.

**Tier 4 — images. [DECIDED: support all three]**

**Sixel**, the **kitty graphics protocol** and **iTerm2's inline images**
all put pixels in a character grid, and no two agree on transport,
encoding or placement. They agree only on the *result* — an image anchored
at a cell, occupying a rectangle of cells — so that result is what
`bssh_image_store` holds, and each protocol's parser is reduced to
producing one of them.

| Protocol | Transport | Payload | Status |
|---|---|---|---|
| **kitty** | APC `_G…` | base64 PNG (`f=100`) or raw RGB/RGBA (`f=24/32`), chunked with `m=1` | **Done** — transmit, display, delete, chunked |
| **iTerm2** | OSC 1337 | base64 PNG/JPEG | **Done** |
| **sixel** | DCS `q` | a 1974 pixel encoding with its own palette handling | **Done** — decoded by `libsixel` |

Decoding is never ours: `QImage` reads PNG and JPEG and
`QByteArray::fromBase64` does the transport, so kitty and iTerm2 needed no
image decoder written here at all, and sixel — whose payload is not a
container Qt knows — is decoded by **libsixel**, which is exactly the work
not worth repeating.

**The cell footprint is the remote's claim, and it is bounded now.** Both
kitty's `c`/`r` and iTerm2's `width`/`height` say how many CELLS an image
occupies, and both were taken with nothing but a "greater than zero" check.
The renderer forms `cols * cell_width` into an `int` for its target
rectangle, so `c=300000000` at a 9-pixel cell is signed overflow — undefined
behaviour reached from bytes anything upstream can send, a file somebody
`cat`s included — and the row count is worse than equal, since it also
drives the cursor advance written back to the remote. Capped at 10000 cells
in either direction, far above any real terminal and far below where the
arithmetic downstream can wrap. **Bounded rather than rejected**, because an
oversized footprint is not by itself malformed: an image may legitimately
hang off the right of a small window.
That crop is real now too. `draw_images` culled on ROWS only, so an image
placed near the right edge was drawn stretched across everything the widget
would accept rather than cropped at the grid the way a terminal does. Most
of the overrun was invisible — a `QPainter` on a widget is clipped to the
widget, and a child scrollbar paints over its own strip afterwards — so what
it cost was fidelity and rasterising work repeated on every repaint, not a
visible mess. Worth knowing which of those was true: the audit that found it
first reported it as painting over the scrollbar, and that part did not
survive checking.

Two parts of libsixel's contract are established by experiment rather than
by its header, and **both fail silently when got wrong**, so they are
written down here as well as at the call site: the **DCS introducer must be
included** (given only the body after the `q` it returns success and a 1×1
image), and the **palette is RGB triplets** despite the header comment
saying ARGB (read as four-byte entries the colours come out wrong but
plausible).

Two things images must do that are easy to miss, and both are tested:
they **follow the text when the screen scrolls** (an image pinned to its
original row while output moves past it looks like the terminal has come
apart), and they are **dropped when the grid shrinks past them or the memory
budget is reached**, because holding every image a long session ever drew is
how a phone runs out of memory. This paragraph used to end "there is no image
scrollback", and the **[DONE]** block below — which gives images a scrollback
and a 32 MB budget — contradicted it directly a few lines later.

**Ctrl+click, and a prompt, for hyperlinks.** The URL comes from the remote,
so a plain tap on a line of output must not be able to launch a browser at
an address someone else chose — every terminal supporting OSC 8 asks for a
modifier, and BeerSSH additionally shows the address before opening it. The
widget does not open anything itself: what to do with a remote's URL is the
window's decision, not a terminal component's.

**[DONE]** Image scrollback, its memory budget, and `z < 0`. A placement
scrolled off the top keeps going into negative rows — row −3 means three lines
above the live screen, where the text it belongs to now is — so scrolling back
brings the picture back with its output instead of finding a hole where one
used to be. What bounds it is **32 MB of decoded image per session**, not a
count: one screenshot outweighs a thousand sparklines. Over budget, the images
furthest back go first, and anything still on the live screen is never dropped
however big it is — a budget that can blank the picture being looked at has
misunderstood its job. `z < 0` now renders in its own pass before the cells,
which is what the comment beside the single over-the-text loop had claimed all
along.

**Where this lives in the code:** libvterm handles the sequences it knows
and hands everything else to `vterm_screen_set_unrecognised_fallbacks`, its
documented extension hook. Modes libvterm does not model (kitty flags,
modifyOtherKeys level) are tracked in `bssh_emulator` beside it.

**The hook turned out to have holes, and this is where vendoring earned its
keep.** Three sequences never reach it at all: `on_csi()` rejects any private
leader other than `?` and `>` before dispatch, which drops kitty's pop
(`CSI < u`) and set (`CSI = … u`); and `CSI ? 4 m`, xterm's XTQMODKEYS
query, is swallowed by a DECSGR case for a printer-era sequence no DEC
terminal implemented. And `set_dec_mode()` drops any DEC private mode
libvterm does not model with a debug log, which is why synchronized output
needed a patch before it could be implemented at all. All three are
*unreachable*, not merely unhandled, so no amount of application code
could have implemented them.

The fix lives in **`vterm-patch/`**, applied to the submodule at build
time by the top-level Makefile — **not** committed into the submodule
itself, because a submodule pinned to a commit that exists only on this
machine is one nobody else can clone. An upstream update that conflicts
fails loudly at the patch step rather than silently dropping the fix, and
`make veryclean` hands the submodule back pristine. All five patches are
worth sending upstream.

**`vterm-patch/0005` is the fourth kind: a reset that clears state without
telling anyone.** `vterm_state_reset()` cleared `mode.alt_screen` and
`mouse_flags` by direct assignment, bypassing `vterm_state_set_termprop()`
entirely — so the screen layer was never told, and neither was anything
above it. Reachable from bytes any remote sends, RIS or DECSTR:
the screen layer kept pointing at the alternate buffer while the state
said primary, and this terminal kept a mouse mode libvterm no longer had,
routing every click to a remote that would drop it — clicks dead, wheel
swallowed, no context menu, no selection, until the session reconnected.
Both are fixed by announcing rather than assigning, and `set_termprop`
already does the whole job for these two properties including the buffer
swap, so the reset came out **more** correct as well as louder.
The third part of that patch is a new property, and it is the one that
needed a design rather than a fix. A terminal built on libvterm has modes
libvterm does not model — the kitty keyboard flags and xterm's
`modifyOtherKeys` are ours, and **both specifications say RIS clears
them** — and there was no way to learn that a reset had happened at all.
So those survived everything: not RIS, not DECSTR, not leaving the
alternate screen, not a new shell. After a kitty-aware program died
without popping its flags, **`Ctrl+C` encoded as `CSI 99;5u` for the life
of the terminal**: no SIGINT delivered, and the escape deposited on the
shell's command line instead. `VTERM_PROP_RESET` is announced last, so a
listener sees a terminal that is already reset; it stores nothing, and it
is listed explicitly in `set_termprop`'s switch so that adding it cannot
quietly make that switch non-exhaustive.
It paid for itself immediately in our own code: `reset_modes()` used to
feed `CSI ? 1049 l` before its reset to work around the altscreen half,
and to clear the mouse, kitty and `modifyOtherKeys` shadows by hand.
Both are gone. A reconnect and a remote's own RIS now take the same path.

The third is not an extension hole but an ordering bug, and it is the one
that shows why vendoring keeps earning its keep after the feature work is
done: `scrollrect()` flushes merged damage *after* moving its own buffer,
so a consumer that both merges damage and implements `moverect` is handed
a rectangle whose coordinates predate the scroll and whose contents no
longer do. Nothing in the extension hook could have worked around it —
the damage arrives already wrong. See §9.5, which is where the merging
that exposed it is explained.

**Not everything libvterm does is a defect to patch, either.** Its keyboard
encoder emits `CSI u` unconditionally for the ambiguous control keys
(`Ctrl+I`, `Ctrl+M`, `Ctrl+[`) — correct for an editor that always speaks
the modern protocol, wrong for a terminal talking to whatever is on the
other end. A remote that has *not* enabled a keyboard protocol must receive
exactly what a VT100 would have sent. That is a policy decision this
project owns, so it lives in `bssh_emulator`, not in a patch.

---

## 8. Multi-session UI: tabs everywhere, splits on large screens

**[DECIDED]** Tabs are the primary model everywhere. **Split panes are enabled
only when the window is wide enough** (tablet, foldable unfolded, and most
desktop windows); phones get tabs and gracefully collapse splits.

This is the right call for a phone-first terminal: an 80-column shell needs
horizontal room, and two 40-column panes on a phone are worse than useless.

**The rule is about the window, not the platform.** A desktop window dragged
narrow deserves exactly the same treatment as a folded foldable, and gets it for
free if the breakpoints are driven by measured width. Resisting the temptation to
write "if Android" here is what makes the collapse path (§8.2) exercised daily on
desktop instead of only on a device nobody tests.

### 8.1 Adaptive breakpoints
Android **window size classes** supply the thresholds; apply them on every
platform, in **logical (density-independent) pixels**, measuring the *window*,
not the screen — multi-window, freeform and tiling WMs all matter:

| Width | Class | Behaviour |
|---|---|---|
| < 600dp | Compact (phone portrait, narrow desktop window) | **Tabs only.** Split UI hidden/disabled. |
| 600–840dp | Medium (tablet portrait, unfolded foldable, phone landscape, half-screen desktop window) | **Up to 2 panes** (H or V split). |
| ≥ 840dp | Expanded (tablet landscape, maximized desktop window, external display) | **Up to 4 panes**, one level of nesting. |

**Qt's logical pixels ARE dp, and converting them again cost three panes.**
`resizeEvent` divided `width()` by `logicalDpiX()/160` to reach the thresholds
above, on the reasoning that 600 and 840dp must mean one physical width on a
phone and on a 4K monitor. That converts twice: Qt folds display density into
`devicePixelRatio` and reports logical pixels above it, which on Android is
precisely the density bucket the table is written in. At 96 dpi the extra
division inflated every desktop width by **1.67x**, so a 504-pixel window
counted as 840dp and was given **four panes of about thirty columns each** —
the arrangement the paragraph above calls worse than useless. Nothing measured
it for as long as it stood, because every test that cared passed a width
straight to `bssh_layout_manager` and only the window did the conversion. What
found it was a drop onto a 360-pixel window that tiled instead of declining.

**[DECIDED]** Cap at 4 panes with a single nesting level for v1 — no arbitrary
recursive tmux-style tree. It covers the realistic cases and avoids a whole class
of layout, focus, and drag-target bugs. Revisit for v2 if there's demand.

Also react to **fold state** (hinge/posture) where available, and to whatever
each platform calls a size change — `onConfigurationChanged` on Android,
`resizeEvent`/`QScreen` changes on desktop. Desktop resizes are **continuous and
frequent** where Android's are discrete, so debounce: recompute the layout class
on a short timer, not on every pixel, or a window drag becomes a storm of
`window-change` messages to every session.

### 8.2 Collapse & restore (the part that must not lose work)

**[BUILT] The terminal declares a minimum, so a window cannot be all chrome.**
It declared none, Qt gave it zero, and the window inherited a floor made
entirely of furniture: rendered at its own claimed minimum of 214x77 it held a
menu bar, a tab strip and a status line with no grid between them. That is not
a harmless number — §8.3 records what Qt does when content is taller than the
space it is given, which is to squeeze rows past their minimums until they draw
on top of each other, and a window manager honouring 77px is asking for it.

**Twenty columns by four rows**, and both are arguments rather than taste.
Twenty columns is about the narrowest that holds a prompt and a short command
without wrapping mid-word; four rows shows the prompt, what was typed, and two
lines of what came back. It constrains nothing real: four panes side by side
only happen above this section's 840dp class, and four twenty-column grids fit
inside that comfortably.

The hard case: the user has a 2-pane split on an unfolded foldable, then folds it
— or rotates to portrait, drags the app into a narrow split-window, or (on
desktop) tiles a 4-pane window into a half-screen column.

**Rule: never destroy a session because a view went away.** On shrinking below the
split threshold:

1. Each pane's `bssh_session` stays in the `bssh_session_registry`, untouched.
2. Panes convert to **tabs**, preserving order; the previously focused pane
   becomes the active tab.
3. The layout intent is **remembered per tab-group**, so re-expanding (unfold,
   rotate back) restores the same split arrangement automatically.
4. Each affected session gets a `window-change` for its new geometry.

This is exactly why §3's session/view separation matters — implemented that way,
collapse/restore is bookkeeping, not surgery.

**Rotation itself is checked on an emulator, and checked from the far end.** A
session was opened, a unique marker left in its scrollback, and the emulator
rotated to landscape and back. The process is the same one
throughout, the scrollback is intact, and the grid re-measures 48x41 → 93x17 →
48x41. The part worth having is the last: the sizes were read from the
**server's** pty, not from the app — `17 93` while landscape, `41 48` after
rotating back — so the remote genuinely received a `window-change` in each
direction rather than the view merely redrawing itself at a new size. Step 4
above, and §7's resize, hold on the platform where the activity is torn down
and rebuilt underneath them. Nothing on a desktop can show this: there is no
activity recreation to survive.

**Steps 2 and 3 are still unverified on a device** — panes converting to tabs
on rotate-to-portrait, and the remembered intent restoring the split on
rotate-back. Both are unit-tested against `bssh_layout_manager` and neither has
been driven on hardware. The attempt was abandoned mid-test rather than
completed, so nothing here should be read as evidence either way.

**The 1886 ms frame that same run logged is not this section's**, and that is
measured rather than assumed. The device reported `Skipped 92 frames` and a
1886 ms frame across the orientation change, which is not obviously the
"bookkeeping, not surgery" this section claims — but the figure contains
Android destroying and recreating the surface, and Qt rebuilding its whole
window, neither of which is ours. `make bench`'s `crossing_a_size_class`
measures only the part that is: the class changing, the panes being destroyed
and rebuilt, each new view resolving a font and remeasuring its cell.

**Collapse 2→1 is 2.8–5.5 ms and expand 1→2 is 4.2–9.0 ms**, with a single
view costing 0.6–1.2 ms of that — best-of-5, on a machine under a load average
of 22–47, so the spread is the machine and the floor is the work. Three orders
of magnitude below the frame. A phone is slower than this x86-64 box, but not
by 300x, so the frame belongs to the surface teardown and not to the pane
rebuild. Which also means the "destroy and rebuild rather than diff" decision
in `bssh_pane_container` is not the thing to revisit if that frame ever needs
shortening.

The benchmark asserts the pane count in both directions as well as timing it:
a measurement that cannot tell "faster" from "stopped collapsing" would report
the same number for a layout manager that had quietly stopped working.

### 8.3 Tabs and focus

**[BUILT] A tab tears out on DISTANCE, not on direction.** The gesture measured
how far below the bar the pointer had gone and nothing else, so a tab could be
pulled out downward and not upward, sideways, or off either end. Three of the
four directions did nothing at all — and nothing at all is indistinguishable
from a feature that does not exist, which is how it was reported: tabs cannot
be dragged freely. Konsole and every browser detach on distance.

The threshold stays generous, for the reason it was generous before: a tab bar
is thin and a pointer reordering tabs crosses its edge constantly, so tearing
out on "left the widget" would make reordering nearly impossible. Sideways is
measured from the bar's full **width** rather than from the last tab, because
dragging along a half-empty strip is still reordering — measured from the tabs,
a tab dragged to the end of a two-tab bar would tear itself out mid-reorder.

- **Tab bar:** horizontally scrollable; label from **OSC title** with fallback to
  the profile name; a small **status dot** per tab (connected / reconnecting /
  disconnected / bell-rang-while-away — and *away* means **no pane**, not
  *unfocused*; see below). Long-press (touch) or right-click
  (desktop) for close/duplicate/move-to-split; desktop also gets a middle-click
  close and drag-to-reorder. The long-press is **built** (see §6.4 — Qt does not
  provide the gesture, so it is `bssh_long_press`), and the tab bar is the last
  of the three surfaces to get it; all three now share one gesture and one set
  of touch metrics, so a menu added later cannot quietly keep pointer-height
  rows. Note that a long press on a tab **switches to it first** — QTabBar acts
  on the press and the gesture only completes on the release — which right-click
  does not do. It costs nothing today: the one verb that would care,
  *Show beside current*, is already disabled wherever the long-press is the
  input, because touch means COMPACT means one pane (§8.2).
- **"While away" means no pane on screen, not merely unfocused**, and it was
  the second of those until a split made the difference visible. The flag was
  driven by which session the *window* had made current, which is the same
  thing only while there is one pane. With two, a bell in the pane beside the
  one being typed in raised a you-missed-this dot on a pane the user was
  looking straight at — and the pane that lost focus was marked away while
  still visible. The pane container owns the answer now, because "which
  sessions are on screen" is the question it exists to answer: a session with a
  pane is on screen, one without is a tab. The four dot states, the rule that
  **dead beats an unheard bell**, and the light/dark contrast floor for the
  colours were all already built and tested.
  **A pane is necessary and not sufficient: the window has to be up too.**
  Answering from the layout alone left every pane "on screen" while the whole
  app was hidden — the tray on desktop, the task moved to the back on Android
  (§5.5) — so the bell that rang while the user was in another program, the one
  they most want to know about, raised nothing at all. `isVisible()` answers
  for the widget and every ancestor, which is exactly the question, and coming
  back counts as having seen it.
  **[OPEN]** Whether a window that is *visible but not focused* — another app
  in front on a desktop — should count as away as well. Arguably yes by the
  same reading; equally, a dot that appears whenever you glance at a browser is
  one people learn to ignore, which costs the state its whole value. Left as
  the narrow rule (gone, not merely behind) until someone decides.
- **The OSC title chain was built, tested and correct** — OSC title, then the
  profile name, then `user@host:port`, never blank. One edge now pinned that
  was not: a remote *clearing* its title with an empty `OSC 0`, which tmux and
  a bare shell both do on exit, has to fall back down the chain rather than
  leave a nameless tab. That works because the fallback is evaluated on every
  read; caching the resolved title would strand the empty one.
- **All three verbs are built now, and *duplicate* was the missing one.** The
  menu carried close and move-to-split and nothing said the third was absent,
  because it was assembled inside the function that `exec()`s it — so its
  contents were unreachable to a test, which is the same reason
  `build_terminal_menu()` was separated out in §6.4. `build_tab_menu()` is that
  split, and the verbs, their order, their access keys and which of them are
  available are now pinned. *Move-to-split* is spelled **Show beside current**
  on screen, and this section keeps the design name; they are the same verb.
  Order is Duplicate, Show beside current, separator, Close — duplicate first
  because a second shell on the same host is the everyday action and a split is
  not, and the separator because Close is the only one here that cannot be
  undone. **Duplicate opens a new session on the same profile** through the
  front door's own path, so a host needing a password is asked for one; copying
  the session would mean copying a connection, which SSH cannot do.
- **Middle-click close and drag-to-reorder are built, and the reorder moves
  the registry rather than the bar.** Tab index and registry index are one
  thing here: every tab action — close, the context menu, switching — turns an
  index into a session with `at()`, and `on_registry_changed` rebuilds the bar
  from the registry. So a bar allowed to rearrange itself privately would point
  Close at whichever session used to be in that slot, and then have its drag
  silently undone at the next rebuild. `bssh_session_registry::move()` is where
  a drag lands, and the bar redraws from the result. Reordering is **desktop
  only**, as this section says: the bar scrolls horizontally under a thumb, and
  one that both scrolls and rearranges under the same drag picks between them
  by accident. Middle-click needs an event filter — `tabCloseRequested` belongs
  to QTabBar's own close button and fires for nothing else — and acts on
  release, since a press that drags away is a cancelled click everywhere else
  and a close is the click that cannot be taken back.
- **A session is not a dead end.** On the touch front door the terminal is not
  the root — the host list is (§8.5) — so backing out of a session goes **up**
  to the list rather than offering to quit. Opening a host used to be a one-way
  door, and Android has no other route back: that made the second session
  unreachable, and with it this whole section — the tab bar, its menu, and every
  split. The app could show four panes and a phone could never ask for two.
- **Focus indicator:** the focused pane needs an unmistakable visual — a colored
  border or dimmed inactive panes. Non-negotiable for safety (§6.3), and *more*
  important on desktop, not less: four panes fit on a monitor, and a pointer
  moving over an unfocused pane invites the assumption that it's the live one.
  **[DECIDED]** desktop offers focus-follows-mouse, **off by default** — which
  is what this line asked for before it was built. Off because it surprises
  anyone expecting click-to-focus, and the cost of that surprise is typing into
  the wrong shell, which is the same hazard this bullet calls non-negotiable
  about the border.
  Implemented by **asking for focus** on pointer-enter rather than by telling
  the pane container directly, so hover goes down the identical path a click
  does: the border, the status line, what copy and paste act on, and §7.1's
  focus reporting all follow from `focusInEvent`, and none of them learns a
  second way of being focused. Offered only where there is a pointer to hover —
  on touch it would be a setting with nothing to act on, which is the same
  dishonesty as offering to hide a window from screenshots where nothing can.
  **The join is pinned between TILED PANES, which is where it was asked for
  and where nothing tested it.** The stored value had a persistence test and
  the view had a setter; neither said the two were connected, and the setting
  reaches a view only through `apply_device_settings()`, so a pane built after
  the choice was made would read its own constructor default. It does not --
  the settings are re-applied on every `panes_rebuilt` -- but that was an
  unasserted property of code written for a different reason, which is the
  "stored, read and applied to nothing" shape §6.2 records for the key row and
  §10.1 for the override badge. Both halves are sabotaged now.
  It remains **off by default**, so it does nothing until it is turned on in
  Settings; that default is the [DECIDED] above rather than an oversight.
- **Tap-to-focus** (or click-to-focus) on a pane; keyboard shortcuts for
  pane/tab navigation (§6.1).
  **This was half-built in the way that is worse than not built at all.** Two
  ideas of "focused" existed and only one of them moved: Qt gives a clicked
  pane the keyboard on its own, since the view takes `StrongFocus`, while the
  pane container's own idea — which drives the focus border, the status line,
  and what copy and paste act on — was moved by the keyboard chords and by
  nothing else. So clicking the other pane sent your typing there and left
  every visible indication pointing at the pane you had left. That is precisely
  the disagreement the bullet above calls non-negotiable, and it is worse than
  a missing feature because the border actively asserts the wrong answer. The
  view now emits on `focusInEvent`, which the container turns into
  `focus_session()`: one hook for a click, a tap, Tab, and its own `setFocus`.
  Testing it needed a finding of its own, recorded because it will catch the
  next person: **Qt applies focus-on-click for spontaneous events only**, so a
  synthesised `QMouseEvent` moves no focus at all and a test written the
  obvious way fails whether or not the app works. Verified against a bare
  `StrongFocus` widget in a scratch program before concluding it was the
  synthetic event rather than this widget.
- **Dividers:** draggable, double-click/double-tap to equalize. Hit target
  ≥48dp effective on touch; desktop can be tighter but must show a resize
  cursor. Snap divider positions to **whole character cells** so panes never
  render a half column.
  **Checked one claim at a time, and two of the four were not there.** Dragging
  is QSplitter's, and the resize cursor is `QSplitterHandle`'s own with nothing
  overriding it. Snapping to whole cells was built. **Equalize was not** — Qt
  raises no signal for a double click on a handle, so the gesture reached the
  handle and stopped; it is an event filter now, and one handler serves both
  inputs because Qt synthesises a double-click from a double tap. Equalizing
  re-snaps to cells afterwards, or it would undo the rule the drag handler
  exists to enforce.
  **And the touch target was 8dp, under a comment claiming it honoured the
  48.** A splitter's grab area *is* its handle width — Qt has no separate hit
  region — so the number is the target, and 8 is six times too small for a
  thumb. It follows the input now, desktop staying narrow as this bullet
  allows. The comment is the part worth remembering: it asserted compliance,
  which is what stopped anyone checking. A claim in a comment is not evidence
  either.
- **Per-pane geometry:** each pane computes its own cols×rows from its own size and
  its session's font — panes may legitimately differ.
- **The grid size in the status line is asked of the view, not remembered.** The
  window cached it once while the pane was still being laid out, got a
  one-column answer, and the later resize arrived through a connection that was
  not yet in place — so it reported "1x41 cells" on a screen plainly showing 48
  columns, for the whole life of the session. Two places computing one thing.
  What the *remote* is told was never affected, and there are now live tests
  (§12.3) that ask a real shell `stty size` for both the opening size and a
  window-change on a live shell, at sizes that are not the pty's 80x24 fallback
  so an arriving size cannot be confused with a default.
- **Every touch surface is checked against a phone, by a test rather than by
  eye.** The same bug kept turning up one screen at a time and always by looking
  at a device: the status line pushed the terminal off the right edge, the
  password prompt demanded 508px on a 360px screen, the secret backend's name
  was clipped mid-sentence. So the question is now asked of all of them at once,
  in both directions, because they fail differently:
  **Too wide** puts content past the right edge. It found two more immediately —
  the host and preset editors, at 392 and 382px — and both for the same reason:
  they embed the font picker, whose one-line preview sample is unbreakable and
  demanded exactly 360px, so the picker alone consumed a phone and any label
  column beside it overflowed. Wrapping the sample fixed both.
  **Too tall** is worse: Qt squeezes rows past their minimums until they draw on
  top of each other, so a phone showed "Name" over "TERM" and a spin box across
  a checkbox. The host editor wanted 821px of a 640px screen and the preset
  editor 1063. Their fields now scroll and their buttons stay pinned to the
  bottom, where a thumb is, rather than becoming something to scroll down to.
  The height is asserted as one question — does the minimum fit — and not as
  "does it fit *or* is there a scroll area". The first version asked the second
  and passed a deliberately broken preset editor, because the orphaned scroll
  area was still a child of the page and `findChildren` cannot tell the
  difference. A working scroll area drops the page's own minimum on its own.
  **And a page can pass this check while the text on it is unreadable.** A
  `QLabel` with `setWordWrap(true)` knows its height only once it knows its
  width, but its size policy does not advertise the dependency — so a layout
  asking "how tall are you?" is told ONE LINE, believes it, and the rest of
  the sentence is drawn over the next row. Two notes on the device settings
  page were losing their tails that way: the key-row note stopped at
  "Override it if you have a keyboard" with *Appearance* printed through its
  remaining two lines. The minimum-size question above cannot see it, because
  the label is exactly what is lying about its minimum — the page passed
  while the words were cut in half. Found by rendering the screen and looking
  at it, which is what `make shots` is for.
  `bssh_wrapped_note()` sets `heightForWidth` on the policy, and every
  wrapped note in the app is built through it — one function rather than
  fourteen call sites, so a note added later cannot quietly miss it.
  **Fixing it made a second defect visible, which is the point.** With the
  labels reporting honestly, the device settings page asks for **752px of a
  640px screen**: it was always that tall and had simply never said so. It is
  the only touch surface that never got the scroll area the host and preset
  editors were given here, and it has grown since — the key-row row, the
  theme, the terminal menu, Caps Lock, the contrast floor, the secret
  backend, and §6.1's six stolen chords. It scrolls now, with *Back* pinned
  outside the scroll area, because a way back that scrolls off the bottom is
  one the user has to go looking for.
- **`make shots` renders every screen to a PNG so somebody can look at them.**
  Not a test: it asserts almost nothing and cannot fail a build. It exists
  because *looking* is what kept working — every layout defect in this project
  was found by rendering a screen and looking at it, and several had passed
  every test that existed at the time. The tests came after the picture, so
  the picture is worth a command rather than an afternoon.
  **The terminal is in it too**, which is the surface the app is *for* and the
  one nothing rendered: the sixteen colours, inverse/underline/bold/faint, box
  drawing, wide glyphs, the disambiguation set, in several schemes with the §9.3
  contrast floor on and off. It loads the bundled set through `BSSH_FONT_DIR`
  and **prints which family actually drew the picture**, because the candidate
  paths are relative to the *binary* and the test binary sits in `build-test/`
  — so it looked one level above the checkout, found nothing, and drew the
  terminal in whatever monospace the machine had. CJK came out as tofu in a
  picture of a renderer that handles it. A shot of a renderer is uninterpretable
  without knowing what rendered it.
  **Its caption named a font it had not drawn with.** The family is printed
  because a shot of a renderer cannot be interpreted without knowing what
  rendered it -- and the line re-derived it with `family_for(QString())`,
  which answers with the *fallback chain's* first entry, while a view
  resolves an empty id through `default_id()`. So every terminal picture
  said **DejaVu Sans Mono** and was drawn in **JetBrains Mono**: the
  safeguard against rendering in the wrong font was itself naming the wrong
  one. It asks a view now (`bssh_terminal_view::font_family()`), which is
  the only answer that cannot drift from what painted the pixels.
  **And it loaded the app's fonts too late for most of the pictures.** The
  bundled set was loaded inside the terminal pass, which runs last, so every
  widget shot before it was drawn with the app's fonts absent -- in whatever
  monospace the machine happened to have. That is the same defect this
  paragraph already records fixing for the terminal, fixed *there only*. The
  worst of it was the font picker: it showed **DejaVu Sans Mono** where the
  app would have chosen Iosevka Term on touch or JetBrains Mono on the
  desktop, so the one picture of the font-choosing UI was the one taken with
  no fonts to choose from. The load happens once now, before any surface is
  drawn, and the count is printed where it cannot be missed.
  It renders each surface twice over — the touch set at 360×640, the desktop set
  at 800×600, the front door at its own minimum, and **every page of the front
  door**, since the touch preset editor sat on its Presets page for as long as
  it did because nobody had looked past the first screen. Alongside each it
  prints the minimum size that surface demands, which is the measurement most of
  those bugs showed up in, and marks any that does not fit.
  Offscreen, so it needs no display. The output goes through a file rather than
  a pipe: a pipeline reports its *last* command's status, so `binary | grep`
  would have reported grep's and swallowed a crash into a clean-looking run.
  **It forces the arrangement rather than inheriting it,** and the first version
  did not. Fourteen widgets decide their shape from `is_touch_primary()`, which
  is false on the desktop build that renders these — so every "touch-" picture
  was the *desktop* arrangement squeezed into 360px. The licences view looked
  broken in a way it is not: side by side at that width its text arrives eight
  characters wide, which is exactly the failure its own code already avoids by
  stacking on a phone. Checked against a device before changing anything, and
  the device was fine. A picture that shows something the app never does is
  worse than no picture, in a tool whose whole argument is that you can trust
  it. `bssh_platform::force_touch_primary()` is the seam — never called by the
  app, in the same spirit as `bssh_ssh_session::set_factory` — and the layout
  tests use it too, since they had been holding desktop arrangements to a
  phone's width and calling it a touch test.
- **Touch targets are a list, and a widget left off it is short.** The preset
  editor applies §6.2's 48dp floor by naming every control, and Import and
  Export were not named -- so they stayed pointer-height on a page built for
  a thumb while the eleven controls around them grew. Invisible in the
  source, obvious in the render.
- **A surface is shot in a state the app can actually reach.** The SSH
  config importer was rendered bare, so it said *These hosts were found*
  over a blank box -- a state `run()` explicitly refuses to produce, since
  an empty parse is reported with a message and the dialog never opens. It
  is fed a parsed config now, which is also the only way the picture shows
  the ticklist the surface exists for, with its key and jump-host
  annotations.
- **The category pages are driven through the TREE, not by setting the
  stack's index.** Setting the index leaves the tree highlighting whatever
  it highlighted before, so all eight page shots showed *Session* selected
  beside a pane full of something else -- a state the app cannot produce.
  It is the same *picture that shows something the app never does* this
  section already had to fix once, when the touch shots turned out to be
  the desktop arrangement squeezed into 360px. Walking the tree also names
  them after the categories, so a reader knows which page they are looking
  at without counting.
- **A note printed above the control it explains.** The device settings page
  puts each explanation under the row it belongs to — every one of them, so
  the eye learns the rhythm — except the status line's, whose `addRow` was
  called one line too early. The result reads as a stray sentence hanging off
  the scrollbar's note: *"Everything adds the terminal's size in cells"*
  above a row that has not yet offered *Everything*. No test can see this,
  because every widget is present, sized correctly and inside the page; only
  the order is wrong, and order is what a picture shows.
- **Two form layouts start two label columns.** `QFormLayout` sizes its label
  column from its own widest label, and nothing tells a second one about the
  first — so the font picker, embedded below the preset editor's form,
  started a column of its own. The rows above put their fields at **x=72**
  and the picker put its at **x=50**, which reads as the page losing its
  alignment half way down. Measured off the PNG by detecting the widget
  frames, after eyeballing it produced a first answer that was wrong; a
  22px offset is exactly the size that looks like carelessness and argues
  about whether it is really there.
  `bssh_font_picker::align_labels()` widens **both** sides to the larger of
  the two, so no caller has to know which is wider, and a picker used alone
  — the desktop *Window* page — is untouched because nothing calls it.
- **The desktop front door gets the same treatment, and it found worse.** Its
  minimums were fine at 800x600 — the small window §8.2 already contemplates when
  it talks about tiling one into a half-screen column — so nothing a size check
  could see was wrong. What was wrong needed looking at:
  **It embedded the TOUCH preset editor.** The call read
  `new bssh_preset_editor(m_store, this)`, with the `QWidget*` landing in what
  was then a `bool touch` and converting to `true`. So the desktop dialog showed
  the thumb-sized editor stacked in one column, with a "Back to hosts" button
  inside a dialog whose navigation is a category tree down the left-hand side.
  It compiled and it ran. The parameter is an enum now, so a pointer is a build
  error rather than a silent wrong answer, and a test pins each arrangement by
  the one control that only makes sense in one of them.
  **Its four destination fields were four columns.** Each row was its own box
  layout, so every field began wherever its own label happened to end, and "Host
  Name (or IP address)" is twice the width of "User name". They are one form
  layout now — which also makes each label the field's buddy, the thing the
  hand-written accessible names had been compensating for. §8.5's whole argument
  for this dialog is that it borrows an interface people already have in their
  fingers, and PuTTY aligns them.
- **What the status line says is the user's to decide, including nothing.**
  It was unconditional, and it opened with the QPA platform name --
  `desktop/xcb`, a fact a developer wants and a user has no use for -- on a row
  of screen taken from the terminal. Konsole has no status line at all and shows
  the size transiently while resizing. Three states in `app.ini`: **session**
  (where you are connected and how it is going, the default), **full** (adds the
  grid size and the platform, for whoever is diagnosing a build), and **off**.
  Read once in `apply_device_settings` and kept, because `update_status()` runs
  on every state change, every resize and every keystroke that scrolls -- opening
  an INI file to answer a question whose answer changes twice a year would be a
  file read in the paint path.
- **The status line never decides how wide the window is.** A `QLabel` asks for
  room for its whole text, and a failure reason is a sentence — the jump-host
  messages (§10.5) run well past a phone's width. Left to itself the label pushed
  the layout past the screen and everything in it, including the terminal, off
  the right-hand edge: **a connection error blanked the app.** Found on a device.
  It is given no say in the width (`QSizePolicy::Ignored`) and its text is
  elided to whatever room it ends up with, re-elided on resize, with the full
  text kept as the tooltip and the accessible name so nothing is actually lost.
  Elided rather than wrapped: a status line that grows to three lines takes the
  space from the terminal, which is where the user was looking.

**The scrollbar is Konsole's, and it is a real one — [DECIDED].** There was
none at all: 5000 lines of scrollback (§10.2) and no way to see where you were
in them or to drag to a point, only the wheel and Shift+PageUp. It is a
`QScrollBar` rather than something painted, because a scrollbar is the one part
of a terminal a user expects to behave exactly like every other scrollbar on
their desktop — the wheel over it, the page-jump click, the theme's own arrows.

It lives **inside `bssh_terminal_view`**, not beside it, because the view owns
the grid arithmetic: the columns it can show are the ones left after the bar has
taken its width, and the code that maps a click to a cell has to start where the
grid starts. A wrapper widget would have split that arithmetic across two
classes; instead the view grew `grid_origin_x()` and `grid_width()`, which the
column count, the painter, the point-to-cell mapping and `size_for_grid()` (§8.6)
all ask.

Four states in `app.ini`, Konsole's three plus this app's usual one:
**automatic** is the right-hand side where there is a pointer to drag it with and
nothing where there is not — a thumb scrolls the terminal itself (§8.5), and a
bar it would have to hit is a strip of screen a phone cannot spare.

**It is set as the default a view is BUILT with**, and the panes are rebuilt once
when the setting actually changes — never poked into each live view. Poking it in
place cannot be done safely from `apply_device_settings`: that runs on
`panes_rebuilt`, changing a view's width re-measures the grid, and a re-measure
can cross the size class that decides how many panes fit (§8.2), which rebuilds
the panes, which calls it again. **That loop ran for five minutes and died on a
stack overflow.** Rebuilding only on a real change terminates, because the second
pass finds the default already right.

**A view nobody is showing must not tell its session how big it is.** A
window-change message is a claim about what the user can see, and Qt lays out a
widget in an unshown window with whatever the splitter had before it had a real
size. The measured symptom: a session told it was **1x2**, which made the remote
reflow every line it had to one column — so a tab torn into a new window had its
screen in the scrollback, a character per line. `recompute_grid()` computes either
way, so anything asking the view what it holds still gets an answer; what is
withheld is the message to the far end, until the window is shown and the resize
that follows says it properly. The floor of one column and one row is right — a
terminal is never zero columns — but it answers "how small can a grid be", not
"does this widget have one yet", and §8.4 records the same shape once already in
a status line that reported "1x41 cells" on a screen plainly showing more.

**The window title is a format, and it follows the tab — [DECIDED].** It was set
once in the constructor and updated only from the remote's OSC title, guarded on
that session being the current one. Two defects with one cause: a window showed
plain "BeerSSH" until a remote spoke up — several windows gave a task switcher
identical rows — and switching tabs kept the old tab's name until the new one
happened to set a title of its own. The title was a side effect of an event
rather than a function of what is on screen, so it is recomputed wherever that
changes.

The format is `%t - BeerSSH` by default, with `%t` the session's own name (the
OSC title, or the saved name until there is one), `%n` the saved name, `%u`/`%h`/
`%p` the destination and `%a` the app. A format rather than a list of canned
choices, which is where Konsole, xterm and tmux all arrived: everybody wants a
different one of the same four facts, and a list long enough to cover them is
longer than the format it replaces. An unknown placeholder is left as written
rather than swallowed — a title that silently loses characters cannot be debugged
from outside the program — and an empty format means the default, because a
window with no name is a blank row in the task switcher.

### 8.4 Implementation notes
- A **`bssh_layout_manager`** owns the tab-group → pane-tree mapping and is the
  single place that decides tabs-vs-splits from the current window size class. UI
  widgets ask it; they don't decide independently. It takes a width in logical
  pixels and knows nothing about which platform supplied it — which is what makes
  it unit-testable by feeding it widths (§12.3).
- Represent layout as a small serializable structure (split direction, ratio,
  session IDs) so it can be persisted and restored across app restarts. The
  format is **shared across platforms** — profiles synced or copied between a
  phone and a desktop must not trip over a platform-specific layout field.
- **A restore reopens every remembered host, and connects none of them.**
  **[DECIDED]** This section used to leave it open on the grounds that
  reopening them all would be "a queue of password prompts", waiting on key
  authentication (M6). That arrived, and the decision taken is stronger than
  the one it was waiting for: **a restore raises no prompt of any kind** --
  not a password, not a key passphrase, not a trust-on-first-use question.
  The reason is what a restore *is*. It happens before the user has asked
  for anything: they launched the app, and that is all. Startup is the worst
  moment to put a host-key warning on screen, because the one habit this app
  must never teach is clicking through a warning to reach the thing you
  actually wanted -- and on a phone, connecting several hosts unasked wakes
  the radio for sessions nobody has looked at.
  So the arrangement comes back and the connections wait. The saved split
  arrangement is restored too, bounded by what the window can show now
  (§8.2): reopened narrower, or on a smaller screen, it is tabs.
  **A disconnected tab must not be a dead end**, which is the defect this
  would otherwise have shipped: nothing in the app could connect a session
  that was not already connecting. *Connect* is the tab menu's first verb
  (§8.3), present only while the session is not live -- absent rather than
  disabled on a live one, since "connect" on something connected is not a
  state anyone should have to reason about. That absence is tested, because
  it is the half that rots quietly: the entry appearing is visible the first
  time anybody looks, while one lingering on a live session is not.
- **A tab, a split pane and a window are three states, and a terminal moves
  between all three by dragging.** **[DECIDED]**, from use: "tabs, splits and
  windows are supposed to be different things and movable between those
  states by drag and drop".
  The states had not been distinct. The tab bar was built from the window's
  session list outright, so a session tiled into a pane was listed in the bar
  directly above itself, and splitting a window in two produced two tabs for
  two terminals both already on screen. §8.3 had said otherwise since M8 --
  *"a session with a pane is on screen, one without is a tab"* -- and nothing
  had ever read it that way. The bar is now the window's list **minus what is
  on screen**, computed from `visible_panes()` rather than from the intent, so
  a window narrowed past its size class puts the panes it can no longer show
  back into the bar. That is §8.2's "panes convert to tabs, preserving order"
  expressed once instead of by a second list somebody has to keep in step.
  **The gesture that was missing is picking up a PANE.** A tab could always be
  pulled clear of the bar; a pane could not be picked up at all, so a terminal
  that had been tiled was stuck there -- and a window with two panes and
  nothing else has no bar to drag from either. **Alt and the left button** is
  the gesture: it costs no screen space, it is what every Linux window manager
  already means by "move this", and Alt+click is not something a terminal
  program expects, so the meta prefix stays on the keyboard where it belongs.
  A plain drag still selects text, which is the half that would have made the
  app unusable if it were wrong.
  It goes through the same code a dragged tab does, deliberately: **what a
  drop means must not depend on which state the terminal was picked up
  from.** Onto a tab bar it is a tab, onto panes it is a tile, onto nothing at
  all it is a window.
  **The un-split has two front doors**, and the second is not decoration: with
  two panes and nothing else there is no tab bar to aim at, so the gesture
  alone would leave a split with no way out. *Show as Tab* sits beside *Split
  Beside Current* on the terminal menu for the same reason that one is there
  -- a one-session window has no tab bar either. The **last** pane will not
  become a tab: a window with no pane shows nothing, which is `tear_out`'s "a
  window will not empty itself" one level in.
  The press starts nothing; movement past Qt's drag threshold does. Pressing a
  modifier and clicking is something people do by accident, and a window that
  rearranges itself on one is worse than a window that cannot be rearranged.
- **Multiple top-level windows are a desktop-only affordance** and **[DECIDED]**
  for v1: they are in. A window is just another view onto the registry — the
  registry stays single and process-wide, and a session can be shown in exactly
  one pane at a time. Do not let a second window imply a second registry.
  **That was written before anything could honour it.** The window *owned* its
  registry (`new bssh_session_registry(this)`), so a second window would have
  brought a second set of sessions with it, and a session opened in one would
  have been invisible to the other. It is `bssh_session_registry::shared()` now
  and the window is handed it, so the property this bullet asserts is a test
  rather than an intention.
- **Windows pretend to be separate processes, because one day they may be.**
  The intent is a transparent client/server split later: a session server
  holding the connections, and windows as clients that could be separate
  processes or even separate machines. So a window asks the registry and
  **never reaches into another window** — no sibling pointers, no shared
  widgets, no "the other window's current tab". Today every such call is a
  pointer dereference; the discipline is what lets it become a message without
  the callers changing.
  This is also why a torn-out tab becomes a new **window** and not a new
  **process**. A live SSH session cannot move between processes — libssh's
  state, the channel and the crypto context are all in this one — so a new
  process would mean dropping the connection and reconnecting, losing the
  scrollback and whatever was running. Once the server is real, the same drag
  becomes a client attaching to a session that never moved.
- **A window shows its OWN tabs, not the registry's.** The tab bar was built
  from every session in the process, which was indistinguishable from correct
  while there could be only one window and wrong the moment there could be two:
  both would have shown every tab, and closing one anywhere would have closed
  it everywhere. Each window keeps the sessions it displays, in its own order —
  tab order is a fact about a window rather than about the process, so dragging
  a tab to reorder moves it in that list and not in the registry, where it
  would have rearranged the other window's tabs as a side effect.
- **Tearing a tab out makes a window, and the session does not move.** It stays
  in the registry, connected, with its scrollback and whatever is running; only
  the view showing it changes. That is the whole reason a torn-out tab is not a
  new *process* — a live SSH session cannot cross one — and it is why the same
  operation becomes a message rather than a migration once the client/server
  split arrives.
  The gesture is a drag taken **40dp clear of the bar**, not "left the widget":
  a tab bar is thin, a pointer reordering tabs crosses its lower edge
  constantly, and tearing out on that would make reordering nearly impossible
  without losing the tab.
  **And it can land on another window instead of on the desktop.** The same
  drag now carries a payload, so a tab pulled clear of its bar can be dropped
  onto a window that already exists: **onto the tab bar it becomes a tab,
  onto the panes it becomes a tile** (§8.3's "Show beside current", reached
  by pointing at it). Tearing out is what happens when **nobody accepted the
  drop** — dropping on the desktop and dropping on nothing are one event to
  Qt, which is exactly the fallback this needs, so the gesture keeps its old
  meaning where there is nowhere to drop and gains a second where there is.
  **Qt's own drag-and-drop carries it, and that is the design rather than a
  convenience.** This bullet's rule is that a window never reaches into
  another window, and a drag is the one gesture most likely to break it: the
  obvious implementation finds the window under the pointer and calls
  something on it. Instead the source offers a payload and the *target*
  decides, resolving the session out of the shared registry in its own
  `dropEvent`. No sibling pointer is taken, and the same gesture becomes a
  message unchanged once the client/server split arrives.
  **What travels is a registry index, not a pointer**, resolved at drop time
  rather than trusted — a session closed while the pointer was still down
  names nothing, and is declined rather than landing on whichever session
  moved into that slot. A payload of any other kind is refused outright: the
  window accepts `application/x-bssh-session` and nothing else, so dropping
  text or a file on the terminal does nothing at all. That is a **separate
  feature and is deliberately not built**; §6.4 describes what the terminal
  does with the clipboard and says nothing about drops, and inventing an
  answer here would be guessing at intent.
  Offered only where there is a pointer, on the same answer that gates
  reordering: a drag nothing can start needs no drop target.
  **A torn-out window showed the FRONT DOOR, and had since the feature
  landed.** `adopt_session()` is how both a torn-out tab and a dropped one
  arrive, and only `open_profile()` switched the stack to the terminal page —
  two places computing one thing, with the other three callers not doing it.
  So the window a tab was torn out *for* came up showing the host list, with
  the session loaded, connected and hidden behind it, and the tab bar hidden
  with it. The page switch lives in `show_session()` now, which is the one
  function that means "show this session". Found by a drop test asking
  whether the tab bar it meant to aim at was actually on screen — the
  assertion was incidental to what it was testing, which is how the
  visible-half defects in this project keep getting caught.
  **A window will not empty itself.** Tearing out the last tab is declined
  rather than performed: it would leave a window with nothing in it and — on
  desktop, where a terminal is what the app opens with — no front door showing
  either. Two windows, one of them useless, is a worse answer than a gesture
  that does nothing.
- **Every window wires every session, so every per-session handler needs a
  guard — and one did not.** `wire_session()` hangs off the registry's
  `session_added` and the registry is process-wide, so a session created
  anywhere is wired by every window there is. That is the right arrangement
  and is what lets an adopted tab work at all: each handler guards on
  `m_shown` or on `m_current`, and a second listener that returns
  immediately costs nothing.
  `shortcut_pressed` was connected straight to `handle_shortcut`, which
  guards on nothing and acts on `m_terminal_view`, `m_current` and
  `m_panes` — all of them *this* window's. So **with two windows open, a
  paste typed in one was also pasted into whatever the other happened to be
  showing**: the clipboard onto a host the user was not aiming at, and a
  pasted line ending in a newline runs there. The new-tab and new-window
  chords opened one of each per window, and page-up scrolled both.
  **The comment above that connection named the hazard and did not close
  it** — "handle_shortcut acts on m_terminal_view and has no idea which
  window asked" was written while fixing the torn-out case, which was the
  opposite failure: the new window not wired at all. Wiring the new window
  did nothing about the chord reaching every *other* one, and a hazard
  written down next to the code that still has it reads exactly like a
  hazard that was dealt with. Worth the entry for that alone.
  Guarded on `m_current` rather than `m_shown`, because a chord belongs to
  the pane it was typed into: a background tab in this window is not where
  the keys went either.
  It needs only a second window to reach — no tear-out, no split, no drag —
  so the test is two windows and one Ctrl+Shift+V, and it fails on the
  bystander's transport rather than on the emulator, which `take_output()`
  would have drained to empty.
- **The status line is one sentence built from two sources, so an error
  belongs to whichever session set it last.** `update_status()` composes
  the current session's target and state and then appends `m_error` after
  a dash. `on_failed()` set that from any session the window had opened,
  guarding on nothing — so a background tab dropping printed
  `prod.example disconnected -- connection reset` against the host on
  screen, naming the one that was fine and describing the one that was
  not. Measured, not reasoned: that string is what the test read back.
  Its two siblings in the same function already guarded — `shell_exited`
  returns unless the session is current, `state_changed` acts only when it
  is — and this one **could not have**, because it is a slot taking the
  reason and not the session. The connection is the only place that knows
  which session spoke, which is why the guard has to live there and why
  the pointer-to-member form is what made the omission invisible.
  **Nothing cleaned it up.** The reconnect message that would have
  replaced the text is guarded the same way and returns early for a
  session that is not current, so the wrong line stayed until something
  unrelated set another. For the *current* session the same failure is
  superseded within the same turn of the loop by "reconnecting in 2s",
  which is why the test asks the control for a clause rather than for the
  failure's own words.
  The fix costs a background tab nothing: §8.3's per-tab dot is where a
  session nobody is looking at reports itself, and
  `bssh_tab_status::state_for()` already answers `DISCONNECTED`.
- **None of the eight signals `prepare_profile()` connects had a test, and
  the reason is reachability.** They are the credential and trust prompts
  — `failed`, `host_key_unknown`, `host_key_changed`, `host_key_revoked`,
  `needs_key_passphrase`, `needs_password` — plus `shell_exited` and
  `state_changed`. `prepare_profile()` is private and `open_profile()`
  protected, so the only road a test can take to any of them is the
  **launch profile** the constructor accepts; a session put in with
  `registry()->create()` and `adopt_session()` gets none of them. That is
  why the bug above survived, and it is worth knowing before writing the
  next test in this area: `bssh_main_window window(named("host"))` is the
  arrangement that has these signals, and `adopt_session` is the one that
  does not.
  It also meant the two roads gave a session **different** wiring, which
  was left open above and is now closed. Those connections took the
  creating window as their context object and `adopt_session()` did not
  remake them, so a session moved into a second window went on asking the
  first: the password dialog opened behind the window the user was
  looking at, and the session sat in `AUTHENTICATING` until they found
  it. Once that window had closed there was no prompt at all — Qt drops a
  connection when its context object dies, so `needs_password` was
  emitted into nothing and a **changed host key** raised a warning that
  never appeared. The test asked the honest form of the question, caught
  the modal from inside `exec()`'s own event loop and read which window
  owned it: *the prompt opened on the window the session LEFT*.
  All eight now live in `wire_session()`, which both roads call. The
  guard is the part worth understanding: `wire_session()` runs off the
  process-wide `session_added`, so every window wires every session, and
  an unguarded prompt would raise **one dialog per window** — the paste
  fault above, in a worse costume. Each prompt therefore tests
  `m_shown.contains(session)` when the signal ARRIVES rather than when it
  is connected, which is precisely what lets the answer follow the
  session as it moves between windows. `failed`, `shell_exited` and
  `state_changed` keep the stricter `m_current` guard, because they write
  the status line and that is about the focused session rather than a
  merely shown one.
- **The registry was made process-wide and the profile store was not, so
  two windows silently ate each other's hosts.** This section says "a
  window is just another view onto the registry — the registry stays
  single and process-wide... Do not let a second window imply a second
  registry", and that rule was applied to the thing windows *read* and
  never to the thing they *write*. `bssh_main_window` constructed its own
  `bssh_profile_store(QString(), this)`.
  The store loads hosts into a `QList` once, at construction, and
  `write_profiles()` calls `QSettings::clear()` and writes that list back
  whole. Two stores over one `hosts.ini` are therefore two full-file
  writers with no reader between them. Measured with two windows and one
  host saved through each:

      first.profile_store() != second.profile_store()
      beta == alpha                    -- both minted the same id
      "the first window's host is gone; the file holds: beta"

  **The id collision is the half that reaches credentials.** A new id is
  `highest + 1` over the store's own stale list, and secrets are keyed
  `profile:<id>:password` (§11) — so the host that survives inherits the
  stored password of the host that was erased. A credential reachable
  from a host its owner never gave it to.
  Fixed with `bssh_profile_store::shared()`, mirroring
  `bssh_session_registry::shared()`; the path-taking constructor stays for
  tests. Sharing the object also completes the view wiring described at
  §7's store entry, which until now could only reach views inside one
  window.
  **The generalisation is the part worth carrying.** When one object is
  made process-wide because two views must agree, ask what else those
  views *write* through. Half a rule is not half a fix: the read path
  looked correct, was tested, and was the reason nobody looked at the
  write path — and the write path is where a loss can happen at all.
- Rendering N panes means N terminal widgets repainting — keep the glyph cache
  **shared** across panes (§9.5) and throttle repaints to the display refresh
  rather than to data arrival, or a chatty `yes`-style output in one pane will
  starve the others.

**[BUILT] A menu bar on desktop, and a "+" on the tab strip.** Until this,
every way to open a tab, a window or a split was a right-click or a chord —
both things somebody has to be *told*, and the complaint that produced this
said exactly that: no visible way to add windows, tabs or tiles. A desktop
application says what it can do without being asked; that is what a menu bar
is for.

**File, Edit, View, Settings, Help**, which is Konsole's set and the 1995
Windows Interface Guidelines' shape — the same authority §6.4's context menu
already follows. mIRC and PuTTY differ in their middles and not in their ends,
because File-first and Help-last is the part everyone agreed on and the part a
hand learns. Items stay **visible and disabled** rather than vanishing, and
carry access keys, for the reasons that section already gives.

**The shortcut text comes from the router, not from the menu.** Chords are
rebindable (§6.1), so a menu printing `Ctrl+Shift+T` beside an item bound to
something else would be the most confidently wrong thing on the screen.
`refresh_menu_bar()` re-derives both the labels and the enabled states, and
runs on the three events that change what a window can do: which pane has
focus, what the registry holds, and whether there is a selection. It runs
**after** those settle — the first version ran at the top of
`on_pane_focus_changed()` and read the previous session.

**The window stays a plain `QWidget`.** What §8.3 rejected was QMainWindow's
menubar/toolbar/statusbar/dock scaffolding, which has to be fought on a phone;
a `QMenuBar` added to this layout on the platform that wants one is not that,
and both platforms still run the same window class.

**The "+" sits at the end of the tab strip**, where every browser and terminal
since the mid-2000s has put it, and appears and disappears with the strip — a
lone "+" above a single-session window would be chrome with nothing to sit
beside. Both it and the menu item exist deliberately: a menu teaches the chord,
a button is quicker once you know it.

**It broke `--geometry`, and that is the useful part.** The window computes its
size from a **named list** of the chrome above the terminal, the menu bar was
not on it, and `--geometry 120x40` would have asked for a window one menu bar
too short. The geometry test caught it because it asserts the resulting grid
both ways round instead of trusting the arithmetic. Left as a list rather than
converted to a walk: a walk would have to distinguish widgets sitting *beside*
the terminal from those above it, and getting that wrong is a silent wrong
answer instead of a loud one.

**Nothing had ever rendered the assembled window.** Every desktop shot is one
widget on its own, so the menu bar, tab strip, terminal and status line had
never been seen together — the arrangement a user actually meets, and the only
place their spacing can be judged. `desktop-window` shoots it with **two**
sessions, since one hides the tab strip and a picture of a deliberately-absent
tab bar says nothing about it.

### 8.5 The front door — **[DECIDED]**

**Desktop borrows PuTTY's interface. Touch gets a mobile one. Same model
underneath.**

This is the one place the two platforms deliberately do *not* share a
layout, and the reason is not screen size — it is that both idioms are
already in people's fingers, and neither survives being moved to the other
platform.

**Desktop: PuTTY's configuration window.** Category tree down the left,
the chosen category's settings on the right, the destination at the top,
and a saved-sessions list with Load / Save / Delete. Anyone arriving from
PuTTY — which is most of the Windows-side audience for an SSH client —
should be able to use it without reading anything. Double-clicking a saved
session opens it; saving under an existing name updates that entry rather
than making a second one. Categories beyond *Session* are listed and say
which milestone fills them in, because a pane that looks finished and does
nothing is worse than one that admits it is empty.

**The local shell is the first row of the host list**, and always there (§2).
Synthetic rather than stored: there is nothing to save, and a row that could be
edited or deleted would let somebody remove the only session that always works
— so it carries no context menu at all, since Edit would have nothing to edit
and Delete would offer to remove it from a store it was never in. First in the
list for the reason a browser puts a new tab at the near end rather than sorted
among the bookmarks: it is the entry that needs no setting up, and it is what a
first-run screen can offer before anything is configured. Present on touch as
well, even though a phone does not *open* with one — an Android sandbox has
little for a shell to do, but "little" is not "nothing", and being unable to
reach it is a different thing from it not being the default.

**[BUILT] Every category sets something, and every category is named after
what it sets.** Reported as a confusing dialog, and the two specific
complaints were these.

*Terminal*, *Connection* and *SSH* were **signposts** -- a paragraph and a
button reading "Open Presets" -- because what they name lives in a preset
rather than in a session. That is a true fact about the model and a bad
answer to a category tree: PuTTY has Terminal and Connection and both hold
settings, so borrowing PuTTY's shape and then leading nowhere reads as
unfinished rather than as principled.

Terminal and Connection hold their settings now, as the **same preset editor
showing a subset of its fields** -- one implementation of what TERM means, one
save, one propagation. Every field is still *built* in every view, because a
field that existed only on one page would be one the other page silently
discarded on save; only what reaches the form differs. The preset list stays
visible on both, which is what tells a reader they are editing something
**shared** rather than this one session.

*SSH* is gone. It could not be given settings because it had none to give:
the key is per host and lives on *Session*, keepalive and compression are per
preset and live on *Connection*. Its paragraph moved beside the key field it
explains, which is where it was pointing anyway.

*Window* is **Appearance**, because the page has always been headed Appearance
and holds the font, its size and the colours. PuTTY nests Appearance under
Window, which needs a sub-tree to be worth doing; one accurate word beats two
that argue.

Import and Export are hidden outside the *Presets* category: they move whole
preset files between machines, and on a page about four terminal settings they
are two more buttons between a reader and what they came for.

**A field left out of a form is not thereby invisible.** Each is built with the
editor as its parent, and a parented widget with no layout is drawn at its
default geometry -- the top-left corner. The first version left the unshown
fields out of the form and painted the key-row combo over the preset list's
first row. Nothing could have asserted it: every widget was present, sized and
inside the page. Found by rendering the page and looking at it.

**The tree and the stack are joined by magic indices**, so removing a page
shifted the two below it. A test clicks every category, asserts a distinct
page arrives, and asserts each page has a control on it -- which is what stops
a category quietly becoming a signpost again.

**Touch: a full-bleed host list**, and its editor is a **page, not a
dialog**. **The rows are 64dp and, until they were looked at, said nothing
about being rows** -- no rule between them, no selection, no pressed state
-- so the first screen an Android user meets read as two paragraphs of text
rather than as a list to tap. Every size check passed throughout, because
64dp of nothing is still 64dp; it took rendering the screen. They carry a
separator drawn from `QPalette::Mid` now, rebuilt on `PaletteChange` for
the reason §6.2 gives about the tab dots -- a fixed grey that reads as a
hairline on the light theme is invisible on the dark one -- over the list's
own Base background, since making it transparent cost the distinction
between the heading band and the content under it. A target elides in the
**middle**: the ends of `user@host:port` are what identify a row, and
eliding at the end drops the port, which is exactly what tells two rows
apart. The first Android build used a `QInputDialog` to add a host, and
driving it on a device showed exactly what this section warns about: a
small floating box with one cramped field, mid-screen, on a device held in
one hand. It worked, which was the problem — the desktop idiom had
survived into the mobile front door because nobody had looked at it. One list, 64dp rows carrying a name and
its `user@host`, one tap to connect, a floating action button to add. No
category tree: a tree of setting categories beside a form of small fields
is a desktop idiom that survives neither a thumb nor a 5-inch screen.
Kinetic scrolling via `QScroller` — dragging a desktop scrollbar with a
thumb is the single most obvious way a Widgets app announces it was not
written for touch. **Still Widgets, still no QML** (§2): the touch
affordances here are a stylesheet, a row height and a gesture recogniser.

**What each device OPENS WITH is a second capability question**, asked the same
way: `opens_a_local_shell(touch_primary)`. The desktop opens a local shell
(§2) — a terminal that opens showing a connection dialog cannot be used as a
terminal — and touch opens its configuration, because an Android app sandbox
has no userland worth driving a shell from.
**The configuration must therefore always be reachable from a terminal**, and
on desktop that is now the *only* way to reach it. Touch has Back (§8.3); a
pointer gets *Hosts and Settings* on the terminal's own context menu (§6.4),
behind its own separator. That menu is the one surface both platforms already
share, so one entry serves both. Without it the desktop would have exactly the
one-way door that made the tab bar unreachable on Android — the same defect
arriving on the other platform by the other road.
**Leaving the front door must not take the terminal with it.** Cancel was
wired straight to `close()`, so opening the settings from a working shell and
pressing Cancel closed the window and every live session in it — and §5.3 is
clear that a reconnect does not bring a shell back, so that is destroyed work
rather than an inconvenience. Reported from use, and invisible here because
the wiring is *correct for the screen it was written for*: on a desktop launch
with no target named, the front door is the first thing shown, and declining
it there has always meant "do not start". What changed underneath is that the
same screen became reachable from a live session, and nobody revisited the
Cancel. So it quits only when there is nothing behind it, and otherwise goes
back to the terminal.
**The touch side never had this bug**, which is the tell: its Back goes to
the host list because it was written as navigation, while the desktop's was
written as an answer to a startup question. Two idioms, one of which quietly
acquired a second meaning.
**And the same page-shaped front door lets a window exist with no terminal
in it at all** — the converse complaint, from the same structure. A desktop
window is supposed to *be* a terminal; the configuration living inside its
stack is what allows a window that is only settings.

**[BUILT] So on desktop the configuration is a window of its own**, and that
settles both directions at once. Closing it cannot reach the terminal, and a
main window is a terminal by construction — neither failure is expressible any
more, rather than each being guarded against. It is parented to the main window
with `Qt::Window`, so it stays associated with it and is destroyed with it
while getting its own frame, its own task-bar entry and its own close button.
Touch keeps its page and its Back, which was never the shape with the problem.

The Cancel rule above survives intact for the startup case: with nothing behind
it, declining the front door still means "do not start".

**The constructor still puts nothing on screen.** It records the intent and
`showEvent()` does the showing — the same rule that moved
`open_default_session()` and `import_launch_document()` out of the constructor,
for the same reason: building a widget tree is not running a program, and the
fifteen windows the suite constructs have no business opening anything.

**The frame's close button means what Cancel means**, and that is a third
control the page version never had. As a page there was no X: the only way out
was the button that told the window about it. A window hiding itself without
saying so leaves the flag claiming the front door is up and — with no session
behind it — an app running with no terminal and no way back to one, which is
the complaint that made this a window, arriving through the door the fix
opened.

**Two things broke on the way, and both were caught by tests written for the
page-shaped version of the same bug.** The window used to open on the front
door *by accident* — it was the stack's first page, so a window with nothing
else in it showed one without anybody saying so; stated explicitly now. And
bringing the terminal forward stopped working, because a page swap does
nothing to a separate window sitting over it — which is the tear-out bug this
section already records, arriving again in a new shape. That is what a
structural change buys: the old mistakes become impossible and their
neighbours become available.

**Opened by `main()`, never by the constructor.** Doing it in the constructor
forked a real shell for every `bssh_main_window` anything built — the fifteen
the suite constructs, and the one `--self-test` makes — and since a live
session asks the platform to stay resident, `quitOnLastWindowClosed` went false
and the smoke test never returned. A window is a widget tree, not a running
program; `import_launch_document()` is called from `main()` for the same reason
and said so first.

**Which front door appears is a capability question, not an OS question** —
`front_door_for(touch_primary)`, the same rule as the on-screen key row
(§6.2). A convertible in tablet mode gets the list; a phone docked with a
keyboard and a monitor does not. Only the chosen one is constructed: two
live front doors over one store would be two things to keep in step for no
benefit.

**Both read and write the same `bssh_profile_store`** (§10.6), so a host
saved on one is a host listed on the other, and the preset system (§10)
attaches to profiles rather than to either UI.

**Both must offer the same VERBS, whatever shape they take.** The touch
list had only two — tap to connect, button to add — for several
milestones, so a host was write-once on the only front door Android has:
one imported from an SSH config could never be corrected, and a mistyped
one could never be removed. Edit and Delete now live behind press-and-hold
on a row, which keeps the everyday action (tap to connect) the cheapest
one and puts the rarer pair behind the gesture the platform already
teaches for "act on this item" — rather than onto per-row buttons that
would compete with the tap on every row. The lesson generalises past this
screen: a front door that renders every field but cannot *do* half of what
the other one does satisfies §14's rule only on paper.

### 8.6 The command line — **[DECIDED]**

**BeerSSH takes xterm's and konsole's arguments, because it is now a terminal
emulator as well as an SSH client.** The desktop opens a local shell (§2), and
somebody typing `beerssh -e htop` is not experimenting: they are using the
vocabulary every terminal since 1984 has answered to. An SSH client that
ignored it would be one people have to write a wrapper script around.

| option | from | what it does |
|---|---|---|
| `-e`, `--execute CMD…` | xterm, konsole | Run a command instead of a shell. **Everything after `-e` is the command**, so it comes last. |
| `--hold` | xterm's `-hold`, konsole | Keep the pane when what it was running ends. |
| `--workdir DIR` | konsole | Start the local shell there. Local sessions only. |
| `-T`, `--title` | xterm | The name the session carries until the remote sets one with OSC. |
| `--geometry COLSxROWS[+X+Y]` | xterm | The terminal size **in cells**; the position, where given, in pixels. |
| `--load NAME` | PuTTY's `-load` | Open a saved session by name. |
| `--self-test` | ours | §12.4's headless smoke test. |
| `-h/--help`, `-v/--version` | everybody | |
| `[user@]host[:port]` | ssh | The destination. Was already accepted; now parsed in one place (`bssh_profile_t::from_target`, beside the `target()` that writes one). |
| `preset.ini` | §10.5 | A preset to import — what a file manager passes to a registered handler. |

**One parser, and there were two.** `main()` had a hand-rolled loop that took
`--self-test` and the first bare argument as a destination, while
`platform_desktop::launch_document()` separately re-scanned `argv` for a
`*.ini`. They agreed only by coincidence and could not report an error about
each other's arguments — and they disagreed in one visible way: the document
scan required the file to **exist**, so a mistyped preset path fell through to
the other loop and was dialled as a hostname. `bssh_parse_command_line` is the
only reader now, it decides a preset by its suffix alone, and whether the file
is there is the importer's question, which has a message for the answer.

**`-e` means two things, and that is the protocols' doing rather than ours.**
Locally the words are `exec`ed as given, with no shell between them — xterm's
behaviour, and why `-e ls *.c` needs no quoting and a filename with a space in
it survives. Over SSH there is no argv on the wire: an exec request carries one
string, which the far end hands to the account's shell, so the words are joined
with spaces exactly as `ssh host ls -l` does. The PTY is requested either way,
which is what `ssh -t` does and what `htop`, `vim` and `less` — the programs
people actually run this way — all need.

**`--geometry` is in cells, and Qt made that a fight worth recording.** Qt's XCB
plugin **consumes `--geometry`, `--title` and `--name` out of `argv` before the
program sees them**; the Wayland and offscreen plugins leave them alone.
Measured, not assumed: `beerssh --title Logs` set a title on one desktop and
vanished on another, and `--geometry 80x24` meant twenty-four *rows* under one
plugin and twenty-four *pixels* under the other. A flag whose meaning depends
on the display server is worse than one that does not exist. So the command
line is read from `argv` **before `QApplication` is constructed**, and Qt is
handed nothing but the program's own name.

The cost is stated rather than hidden: **Qt's toolkit options are no longer
accepted on the command line.** `-platform`, `-style` and the rest all have
environment variables, and `QT_QPA_PLATFORM` is what `make test` and
`tool/check-deb.sh` already use. `bssh_command_line()` is where the parsed
result lives afterwards, because `QCoreApplication::arguments()` now holds only
`argv[0]` and the desktop platform still has to answer `launch_document()`.

**A shell that ends closes its pane, unless `--hold`.** Nothing did this before:
the pane stayed with "session ended" written under it, which is xterm's `-hold`
applied to everybody whether they asked for it or not — so `beerssh -e ls`
would have printed its output into a window nobody asked to keep. The rule
lives in `bssh_session_registry` rather than in a window, because it is a fact
about a session rather than about a view of one: a tab torn into another window
(§8.4), and a session no view is showing at all, have to end the same way. Only
an **ending** closes. A dropped connection is §5.3's business and stays for the
reconnect; a failure stays so its reason can be read.

**And the last pane of a window whose session ended takes the window with it**,
on a device that opens with a shell — `opens_a_local_shell()` again (§8.5),
not an OS check. A terminal whose program has finished is done, and falling
back to the PuTTY-shaped front door would mean `beerssh -e ls` printed its
output and then offered a connection dialog. A tab closed **by hand** still
goes up to the front door: the user is still driving the app, which is a
different thing from the program being over.

**Errors go to stderr and exit non-zero, before a window exists.** An unknown
saved session, a `--geometry` that is not a grid, two destinations, a
`--workdir` given with a destination — all of them are a line on the console
and exit 2, because a console program's errors belong on the console and a
dialog over a window that should not have opened is not an improvement.
`--workdir` with a destination is **refused rather than ignored**: SSH has no
way to ask a server to start somewhere, and an option that silently does
nothing is how people stop trusting a program's arguments.

**A small `--geometry` was clamped, by a defect this uncovered.** The window's
pages — terminal, session dialog, host list, both editors — share one
`QStackedWidget`, and a stack asks *every* page how small it may be and takes
the largest answer. So the window inherited the front door's minimum while
showing a terminal: measured at **520x460**, about 57 columns by 21 rows.
Anything smaller came out bigger than it was asked for, and a desktop user
could not drag a terminal window small either — the same bug wearing the other
hat, and the one nobody had reported because nobody connects a resize limit to
a configuration page they cannot see.

**Fixed where it belongs, in §8.5's page stack rather than in the geometry
arithmetic**: a page that is not on screen has its size policy set to
`Ignored`, which is exactly the "this widget's hint does not count" that Qt's
own `qSmartMinSize` reads, and gets its policy back when it is shown again. The
policies are remembered per page rather than assumed, or a page with a policy
of its own would silently acquire the default the first time it was hidden. A
window showing a terminal is now bounded by the terminal, and `--geometry 40x12`
is honoured — which is what the test asserts, well under the old floor in both
directions so that a regression fails rather than passing by a pixel.

The consequence, and it is the right way round: opening the configuration on a
window too small for it grows the window, because that page's minimum is real
while it is the one on screen.

**The manual page is generated, by the program, from the same table.**
`beerssh --man` writes roff built from `bssh_command_line_options()`, and
`make man` — which a plain `make` already runs — puts it in `beerssh.1`. It is
**not committed**: a page kept as a file is a second copy of the option list,
and a manual page is exactly where nobody notices the drift, because it
documents a flag that no longer exists and omits the one you wanted and neither
is an error. `fmake` does this, from the same argument and with a
`--completion` on the same footing; harmonising with a sibling beat introducing
a `help2man` build-dependency here, which would have been a new pattern for one
project to set on its own. A test walks the table against the page **in both
directions**, since either alone is satisfiable by generating nothing.

The one limit worth stating: a **cross-built** package cannot generate its own
page, because the rule runs the binary it just built. Native builds are what
§12.2 describes and what the packaging does today; a cross build would need a
host build first. Better than a committed copy that is silently wrong on every
architecture at once.

**And the printing options had to stop needing a screen.** `--help` was
answered *after* `QApplication` was constructed, so with `DISPLAY` unset it
died with "could not connect to display" and exit 141 rather than printing
anything — measured, not supposed. That is `--help` over SSH, `--help` in a
container, and `--man` in the package build that generates the page: three
places a terminal emulator has no business needing a display. They are answered
before Qt exists now, and `tool/check-deb.sh` runs `--help` with no display
against the installed package rather than trusting that it works.

**Considered and not built**, each for a reason rather than by omission:

- **`-fn`/`-bg`/`-fg`-style appearance overrides.** A per-invocation font or
  colour scheme would have to sit *above* a preset in §10.1's resolver — a
  fourth layer, and a design change rather than an argument.
- **`--tab`/`--window`** (konsole's, gnome-terminal's). Both mean "give this to
  the instance already running", and there is no such mechanism here yet;
  §8.4's client/server split is where that question belongs.
- **A remote `--workdir`.** See above: the protocol has no way to ask.

---

## 9. Fonts: a curated, terminal-suitable set

**[DECIDED]** Bundle a **curated FOSS set**. No user font import in v1 — a font
that isn't truly monospace or lacks box-drawing glyphs produces a broken-looking
terminal and generates support burden. Shipping a vetted set means the grid always
renders correctly.

### 9.1 What makes a font suitable for a terminal (hard requirements)
Any candidate must pass **all** of:

1. **True monospace** — every glyph shares one advance width. Verify
   programmatically, don't trust the family name.
2. **Box-drawing** `U+2500–257F` and **block elements** `U+2580–259F` — required by
   `ncurses`/TUI apps (`htop`, `mc`, `vim` splits). Missing these is the single
   most common "why does my terminal look shattered" cause.
3. **Legibility disambiguation** — visually distinct `0/O`, `1/l/I`, `5/S`, `2/Z`,
   and clear brackets/braces.
4. **Weights** — at least regular + bold (SGR bold is common); italic desirable for
   SGR 3.
5. **Latin-1 + common symbols**, plus arrows and geometric shapes used by prompts.
6. **A redistributable licence** compatible with app bundling, Play distribution
   *and* desktop redistribution (Flathub, distro packaging) — OFL 1.1,
   Apache-2.0, MIT, or equivalent. Distro packagers scrutinise bundled fonts
   more closely than Play does, so treat their standard as the bar.

Desirable but not required: braille patterns `U+2800–28FF` (used by some TUI
charts), powerline glyphs, ligature support (must be **off by default** — ligatures
in a character-cell grid confuse column alignment for many users).

### 9.2 Proposed curated set (all redistributable, verify licences at build time)

| Font | Licence | Why it's in the set |
|---|---|---|
| **JetBrains Mono** | OFL 1.1 | **Default.** Tall x-height, excellent legibility at small sizes — ideal for phones. Ligatures present; ship them **disabled by default**. |
| **DejaVu Sans Mono** | DejaVu / Bitstream Vera (permissive) | The box-drawing and broad-Unicode workhorse. Also the **fallback** in the chain (§9.3). |
| **Iosevka** (narrow/term variant) | OFL 1.1 | **Condensed** — fits noticeably more columns on a phone screen. Genuinely valuable on mobile; strong candidate for the phone default. |
| **Hack** | MIT-derived (Hack Open Font Licence) | Classic, workmanlike terminal font with deliberate disambiguation. |
| **Fira Mono / Fira Code** | OFL 1.1 | Popular; Code variant for users who want ligatures. |
| **Source Code Pro** | OFL 1.1 | Widely liked, very clean. |
| **IBM Plex Mono** | OFL 1.1 | Distinctive, good weight range. |
| **Cascadia Mono / Code** | OFL 1.1 | Modern; `Code PL` variant carries powerline glyphs. |
| **Inconsolata** | OFL 1.1 | Long-standing favourite, compact. |
| **Noto Sans Mono** | OFL 1.1 | Fallback-tier: enormous Unicode coverage including CJK. |
| **GNU Unifont** | OFL 1.1 (dual-licensed) | Last-resort fallback so *nothing* renders as tofu. Ugly but complete. |

**[DECIDED]** Trimmed to exactly the priority list above: **JetBrains Mono,
Iosevka Term, DejaVu Sans Mono, Hack, Fira Code, Cascadia Code PL** selectable,
**Noto Sans Mono + GNU Unifont** fallback-only and never shown in the picker.
Source Code Pro, IBM Plex Mono and Inconsolata are dropped — not for any fault,
but because a picker of eight near-equivalent faces is a picker nobody reaches
the bottom of, and each one is a licence to track and megabytes to ship.

The set lives in one table in `bssh_font_catalog` and is fetched by
`tool/fetch-fonts.sh`, which pins a version *and* a SHA-256 per family. The
fonts are deliberately **not committed**: 54 MB that never diffs usefully, and
a pinned checksum gives the same reproducibility a committed copy would.
Specific choices worth recording: **Iosevka Term**, not plain Iosevka, because
the Term variant is the one metrically intended for a character grid; and
**Cascadia Code PL**, because the PL variant is where §9.2's powerline coverage
comes from without bundling Nerd Font patched derivatives.

**The *default* may differ by form factor; the *set* may not.** A condensed face
that wins on a 5-inch screen is not the obvious choice on a 27-inch monitor, so
picking a different out-of-the-box default per platform is reasonable — but it
must be a member of the same shared set (§9.4), so that a profile carrying an
explicit font choice resolves identically everywhere.

**Explicitly excluded:** proprietary/non-redistributable fonts (Menlo, SF Mono,
Consolas, Operator Mono), and any proportional font regardless of popularity.
**Nerd Font patched variants are out of scope for v1** — they multiply APK size and
the licensing of patched derivatives needs case-by-case review. Powerline coverage
comes from Cascadia Code PL instead. The size argument doesn't apply to desktop,
but the licensing one does, and adding them on one platform only would break the
shared-set rule above.

### 9.3 Fallback chain and wide characters
Define an explicit per-glyph fallback order; the renderer walks it until a glyph
is found:

`selected font → DejaVu Sans Mono (box drawing) → Noto Sans Mono (broad Unicode / CJK) → GNU Unifont (last resort)`

- **Never** fall back to a proportional system font — it breaks the grid.
- **Box-drawing glyphs advance by exactly one cell, and that is now checked
  rather than looked at.** The cell is measured from `W` (§9.5), so a face
  whose `U+2500` advanced by even a fraction less would leave a hairline at
  every cell boundary and a TUI's borders would come apart. It matters most for
  the *fallback*: a DejaVu that disagreed with the selected face would break
  lines only for the faces that lack the glyph, which is the hardest kind of
  fault to see and the easiest to blame on the wrong thing. A test walks every
  bundled face at every size from 8 to 20pt over `─ │ ┌ ┼ ┴ █ ▒`; all eight
  faces and the fallback are exact.
  **It is checked because the blame was misplaced once already.** This project
  carried a note saying box drawing "does not meet in JetBrains Mono", from a
  screenshot scaled up with smooth interpolation — which turns the antialiased
  edge above a one-pixel stroke into what reads as a gap. Measured instead: the
  stroke is a continuous run in all four terminal screenshots, dark and light;
  the advances are exact for every face and size; and a whole-run `drawText`,
  a per-cell draw and a clipped per-cell draw all produce an unbroken line at
  the shipped 9px cell. There was no defect. The case that *does* break lines is
  fractional scaling, and §9.5's device-pixel snapping already answers it.
- **Double-width cells:** implement `wcwidth`-equivalent logic so CJK and many
  emoji occupy exactly two cells, matching what the remote program assumed when it
  laid out the screen. Getting this wrong misaligns every TUI. Note that emoji
  width is genuinely inconsistent across terminals; pick a rule, document it, and
  make it a setting if it causes complaints.
- **Combining marks** should compose into the base cell, not consume a new one.
- **The far half of a wide glyph is one representation, built in one place.**
  A continuation cell is *no character, width 0*, and the renderer skips it on
  the width — everything else it draws. The screen and the scrollback each used
  to build one for themselves and they disagreed: the screen synthesised it,
  while the scrollback converted whatever libvterm had left in that column,
  which is its own marker, `chars[0] == (uint32_t)-1`. That is width 1 (so not
  skipped), not blank (so not treated as empty) and not valid Unicode (so the
  fallback chain above was asked which family covers it) — a box drawn over the
  right half of every CJK character in the history, while the live screen showed
  it correctly. Both paths call `continuation_of()` now, so they cannot drift
  again, and converting a stray marker fails safe rather than yielding
  U+FFFFFFFF. The lesson generalises past this bug: **two paths that build the
  same cell are two chances to spell it differently**, and the one that is
  wrong is whichever the user is not looking at.

**[DECIDED] A scheme owns the background and the colour of ordinary text. It
does not own the sixteen.** This reverses what this section used to say, and
the reason is that the old rule answered the wrong question.

The list used to carry Solarized, Gruvbox, Nord and Tango, reproduced exactly,
with a long argument below about faithfulness. They were faithful, and that
was the problem: **each one redefines what red is.** A program emitting
`\e[31m` means "the user's red", and it is entitled to assume red is red —
`ls` colouring an archive, a compiler underlining an error, an ANSI art file
from 1994. A palette that reassigns those makes every program's colour choices
mean something the program did not choose. Faithfully reproducing somebody's
redefinition is still a redefinition.

So there is **one palette, classic ANSI**, the VGA text-mode values every
console and every DOS program was written against: normal 0–7 dimmed, bright
8–15 saturated, yellow-as-brown included, which is what makes bold mean
something. Every scheme shares it, and a test asserts they do.

What a user actually wants to change is the colour of **ordinary text**, which
is the one colour the remote never sets. Hence the set:

| id | background | ordinary text | for |
|---|---|---|---|
| `default` | `#000000` | `#ffffff` | the default |
| `green-on-black` | `#000000` | `#00ff00` | the green phosphor |
| `yellow-on-black` | `#000000` | `#ffff00` | the amber phosphor |
| `black-on-white` | `#ffffff` | `#000000` | e-paper |
| `custom` | from settings | from settings | the escape hatch |

Three phosphors and an inverse. The inverse is named for **who it is for**
rather than called "light mode": an e-paper panel holds a light background for
free and pays for a dark one, which is the reverse of every other display, so
it is what that hardware wants rather than a taste.

Backgrounds are **pure** black or pure white. `#1c1c1c` was what this shipped
with, and a near-black is the worst of both — it costs an OLED its
switched-off pixels and buys no readability, because the contrast that matters
is against the text.

Anyone who wants Solarized's numbers can still have them through the custom
scheme. They are simply no longer offered as though picking one were an
ordinary thing to do. The id `default` is kept rather than renamed to what it
now is, so every stored profile already saying `default` keeps working, and
the removed ids pass through §10.1's resolver unchanged and draw in the
default — a profile pinned to `solarized-dark` degrades rather than breaking.

**Contrast is still a property of the reader.** The floor below was written to
reconcile faithfulness with legibility, and it survives the change intact for
a better reason than the one it was built for: **classic ANSI blue is worse
than xterm's was.** `#0000aa` on `#000000` measures below what `#0000ee` on
`#1c1c1c` did. The standard is not going to change and must not be edited, so
the renderer is where the problem gets solved — which is what this section
already said, now with a palette that cannot be quietly improved instead.

So **the palette is reproduced exactly, including its unreadable entries**, and
the floor is applied when **drawing**. `bssh_raise_contrast()` lifts a colour only
as far as it takes to clear the floor against whatever is actually behind the
glyph, preserving hue and saturation so a lifted red is still recognisably red,
and moving away from the background because that is the only direction that gains
anything. A colour already above the floor is returned untouched, so the lift is
a floor rather than a filter over everything: rendered side by side, blue and
black-on-background become readable while green, yellow, magenta and cyan do not
move at all.

It is a device setting (§6.2), **on by default**, because several shipped schemes
are genuinely hard to read as published — xterm's blue `0x0000ee` measures
**1.81:1** against the default background, the most notorious readability problem
in terminal history and the reason GNOME Terminal, iTerm2 and Windows Terminal
all ship a brightened one. Being a draw-time rule rather than a palette edit, it
also covers any scheme added later without anybody editing it, and it can be
turned off by someone who wants exactly what the author published.

The floor is **3:1**, not 4.5:1, and the difference is arithmetic rather than
leniency: blue contributes 0.0722 of luminance, so against a near-black
background no recognisably saturated red or blue can reach 4.5 at all — pure
`#ff0000` tops out at 4.26. A floor nothing can satisfy is not a standard, it is
a way of getting the rule switched off.

**Only text is lifted, and the order says so.** The floor is applied *after*
inverse video and selection have swapped foreground and background, to whichever
colour ends up drawing the glyph. Applied before the swaps, a lifted colour lands
*behind* an inverse cell — repainting a block in a colour the scheme does not
contain, for no readability gain, because what needs to be legible on that block
is the text, which is the other colour entirely. Contrast is symmetric, so the
mistake passes every ratio check and is only visible in the painted pixels; the
test asserts on those.

Anything that SHOWS a scheme has to show it through the same floor, or the one
control whose job is judging contrast becomes the one place reporting it wrongly.
The font picker's preview and its sixteen swatches go through
`bssh_raise_contrast()` too — otherwise it paints a nearly invisible blue block
beside a terminal drawing a perfectly readable blue prompt, while claiming in its
own comment that what is on screen is what the terminal will look like. Two
schemes whose blues both get lifted really will look alike in use, so a strip
that says so is being accurate rather than losing information.

**[BUILT] A text-brightness slider, for reading at night.** The floor answers
"can this be read"; it has nothing to say about "is this a lamp in a dark
room", which is a different complaint with a different answer. A terminal that
is comfortable at a desk at noon is genuinely painful at 2am, and where the
line falls depends on the room and the panel — so it is a slider (10–100%) and
not a light/dark toggle.

It **dims what draws and never the background**, so pure black stays pure
black and only the light on it comes down. Dimming the background too would
grey the screen out, which is the opposite of what a dark room wants.

**Order: the floor first, then the dim, and the dim may take text below the
floor.** Dimming first would simply be undone, since the floor lifts until it
clears. Dimming after means the user can go below the readable minimum — which
is correct, because the floor is a *default* and this is somebody explicitly
asking for less light. A slider the default could veto would stop working
exactly where somebody reached for it.

**Blocks dim too, and that was a bug first.** The dim was applied to the glyph
colour only, so an inverse cell — a solid rectangle drawn in what was the
foreground, and the brightest object on the screen — stayed at full strength
while everything around it came down. The night setting missed the one thing
most likely to have prompted it. Found by measuring the rendered pixels, not
by looking: at 50% the text went 255→127 and the block stayed 255. The test
asserts on painted pixels for that reason, since that is the only place the
difference between a colour we computed and a colour that reached the screen
shows up.

It is **device-wide**, not per host: it answers "what is the room like", which
is a property of where you are sitting rather than of what you connected to.
The stored value is bounded on the way *out* as well as in — a hand-edited
config saying `0` would render an invisible terminal, and the first thing
anybody does about an invisible terminal is not open the settings.

**[BUILT] The custom scheme has an editor, and it is the only way in.** The
shipped schemes deliberately cannot repaint the sixteen; this is the escape
hatch for somebody who wants to anyway, and its being a separate dialog is the
design rather than an implementation detail. Changing what red means has
consequences for every program on every host, so it should cost a deliberate
trip — a choice offered in a dropdown beside the font is one people make by
accident.

**Shaped after PuTTY's Colours panel**, which is where this problem was already
solved: every colour visible at once, **named rather than numbered**, and the
system picker for the actual choosing. PuTTY spells them out as a list with RGB
spinners; a grid says the same in a quarter of the height and reads as a
palette, which is what it is. Normal and bright are two ROWS rather than two
blocks, because they are pairs — bright red belongs beside red, not eight
squares away.

**The palette is device-wide; which scheme a host uses stays with the host.**
So `custom` means "this device's custom palette", and the two questions stay in
the two places that already answer them — the same split the brightness slider
uses.

Three details that are each a decision:

- **`Classic ANSI` resets the sixteen only.** Background, text and cursor are
  what somebody came here to change, and discarding those alongside the palette
  would make the button too dangerous to press.
- **Cancel is the default button**, so a stray Return does not accept a palette
  that was still being looked at.
- **Every swatch carries an accessible name.** A grid of coloured squares is
  unusable to a screen reader otherwise, and "which red is this" is precisely
  the question that cannot be answered by looking (§14).

The catalog does **not** read the setting. `term/` holds the palette and
`model/` holds the store, so the value is pushed in by whoever owns the
settings — a colour catalog reaching into a settings store is a dependency
running the wrong way. That also makes the update point explicit, which
matters: a view already showing `custom` has the id it wants and only its
*contents* changed, so it needs a re-read rather than a re-set, and
`set_colour_scheme()` early-returns on an unchanged id.

**[REJECTED]** Shipping a "readable" variant of each scheme alongside the
faithful one. It doubles the picker, and an improved *Solarized Dark* is either
mislabelled or an argument with its author; the same result is available from one
setting that works on everything.

### 9.4 Packaging and size
Bundling ~10 families × multiple weights is **tens of megabytes** — too much for
the base download on Android. Desktop has no such pressure, which is a licence to
diverge on *delivery* but never on *which fonts exist*.

- **Android:** ship **2–3 families in the base APK** (default + condensed +
  DejaVu fallback), and deliver the rest via **Play Asset Delivery** (on-demand
  asset packs), downloaded when the user picks them in settings. The AAB format
  already required (§2) makes this straightforward.
- **Desktop:** ship the full curated set in the package — tens of megabytes is
  unremarkable for a desktop install, and it keeps the font picker free of
  download states on the platform where they'd be pure friction.
- **The picker's contents must not depend on the platform.** A font the user
  selected on their phone has to resolve when the same profile opens on their
  desktop, so families are identified by a stable internal ID, and an
  unavailable font falls back down the §9.3 chain with a visible note rather
  than silently substituting.
- **[OPEN]** Whether desktop additionally offers system-installed monospace
  fonts. Tempting, and it's the one thing desktop users will ask for — but every
  such font is unvetted against §9.1, so if it's offered it must run the same
  suitability check and be clearly marked as unverified. Not in v1.
- Subset aggressively where safe: fallback fonts can be subset to the ranges they
  actually serve. **Do not** subset a selectable font so far that box-drawing
  breaks — that defeats the point. Subset only for the Android package; ship the
  unsubsetted originals on desktop.
- Unifont is small in its `.hex`/bitmap form; consider that rather than the full
  TTF.
- Keep each font's licence file in the bundle and surface an **Open Source
  Licences** screen in settings (also good Play hygiene, and required in spirit
  by the OFL/Apache terms on every platform). **[BUILT]** Reached from the
  Licences category in the desktop dialog and from the Hosts screen on Android;
  it shows each bundled font's full licence text verbatim (read from the bundle,
  and on Android straight out of `assets:/fonts/licences`), the colour schemes'
  attributions (§10.2), and the libraries — including the sentence libssh needs
  rather than a name, since its LGPL is what makes the desktop build link it
  dynamically (§4).

**The Android base set carries Unifont, and did not.** §9.3's chain ends at
Unifont for a reason — it is what makes "no terminal output is ever
unreadable" true — and the Android base set left it out on the stated grounds
that Noto Sans Mono covered CJK. It does not: Noto Sans **Mono** is Latin,
Greek and Cyrillic, and CJK lives in the separate Noto Sans CJK families that
run to tens of megabytes. Checked with `fc-query`, then on a device, where a
Japanese `ls` came back as a row of boxes — precisely the failure that
justification said could not be allowed. Leaving it out silently truncated the
chain on the one platform where the set is reduced. 4.9 MB covers the whole
BMP, and Japanese, Chinese and Korean are not scripts a phone will not meet.

### 9.5 Rendering pipeline
- **Shared glyph atlas/cache across all panes** (§8.4) keyed by (family, size,
  weight, glyph) — critical when 4 panes render at once. **Measured before
  building one**, and the atlas turned out not to be the problem: drawing cell
  by cell cost **94 ms for a full-screen repaint (11 fps)**, and it was the
  per-cell `setFont`/`drawText` pair, not glyph rasterisation, which Qt already
  caches. Drawing **runs** of same-looking cells brought that to 45 ms, and
  removing a per-cell QString allocation in the fallback-chain cache took it to
  **24 ms (42 fps)**; the four-pane case went from 42 ms to **5.9 ms**. A run
  only lands on the grid because each glyph's advance is pinned to exactly one
  cell with absolute letter spacing — inside one `drawText` Qt would otherwise
  advance by the font's own fractional metrics and drift away from the columns.
  A real atlas remains available if profiling ever points at rasterisation, but
  it would have been the wrong fix for the actual cost.
  **Re-measured, and the atlas is still not the fix — because it already
  exists.** `make bench` now measures *painting* as well as throughput, which it
  could not before: the figures above came from ad-hoc timings that left nothing
  behind to re-run, so the question they settled could not be re-asked. It can
  be now.
  Rasterisation costs **1.9–2.7 ms once** and nothing on any frame after it,
  against a steady-state repaint of **2.3 ms for a 200×50 pane (430 fps)** and
  **3.5 ms for four panes**. And the sharing §9.5 asks for by name is already
  happening: paint four panes in a size nothing has rasterised, one after the
  other, and the first costs **1.74–1.88 ms** while the other three cost
  **1.0–1.6 ms** — the first pays for the glyphs and the rest get them. Qt's
  font-engine cache is keyed by exactly the tuple this section proposes,
  (family, size, weight, glyph), and is shared process-wide. **An atlas of our
  own would be a second cache in front of a working one**, for at most ~2 ms
  recovered once per font change.
  One measurement had to be thrown away first: forcing a cold cache by changing
  the point size every frame reported the cold case as *faster* than the warm
  one, because a different point size is a different cell size — a bigger font
  simply drew fewer cells into the same pixels. It was measuring the grid, not
  the cache. First-frame-versus-rest keeps the work identical and answers the
  question the other way round.
  **The millisecond figures above have since failed to be comparable twice**,
  and the benchmark now prints the ratios instead. At load 81 they read fifteen
  times worse than recorded here — indistinguishable, from the output alone,
  from a serious regression. At load 6 on twelve cores they read two to three
  times worse and varied twofold between consecutive runs of the same binary.
  Neither was a regression: no line of the paint path has changed since these
  were taken. So `make bench` reports **first pane ÷ the mean of the rest**
  (measured 1.75–1.78×, and asserted above 1.1) and **cold frame ÷ warm**
  (1.37–2.74×, asserted above 1.05). Those are the claims this section actually
  makes, and dividing the machine out is what makes them re-checkable on
  somebody else's. §12.3 reaches the same conclusion for throughput by counting
  repaints rather than timing them.
  The sharing ratio cannot be demonstrated by breaking it, and the reason is
  the confound above: giving each pane its own point size defeats the cache and
  changes the work in the same move, so the ratio stays above 1 for the wrong
  reason. It is a regression guard, not a claim a deliberate break can prove.
  **Where the time actually goes, if painting ever needs work:** not here — the
  emulator's scroll path, which is where the next round went.
- **The grid is a ring, and the scrollback takes the mirror's own row.** Two
  changes to the same cost, both found by ablation rather than by reading:
  removing scrollback conversion was worth +44% and removing damage conversion
  +97%, which said the remaining cost was per-cell conversion done *twice* per
  scrolled line, not the row moves.
  **The ring.** Scrolling the whole screen up by one memmoved every row — ten
  thousand cells on a 200×50 grid, per scrolled line — which is why throughput
  tracked the grid's *area*. A row offset makes it one addition. Only when the
  move spans every column and reaches the top or bottom: a scroll inside DECSTBM
  margins leaves rows outside the region that must not move, and those still go
  row by row. The exposed rows keep their contents until libvterm's damage
  fills them, exactly as the memmove left them. Verified firing by counting —
  every scroll takes the fast path, none fall through.
  **The scrollback.** `sb_pushline` arrives *before* `moverect` — checked, not
  assumed — so the row leaving the top is still logical row 0 and the mirror
  already holds it converted. Converting libvterm's cells a second time was the
  larger half of the remaining cost. It also removed a duplicate: the wide-glyph
  continuation was synthesised there "exactly as the screen synthesises it",
  two places computing one thing, which is how an undrawable character once
  reached the scrollback while the screen showed the glyph correctly.
  **Reading the mirror means reading something only as current as the last
  damage flush, and that was a bug** — found while reviewing the vterm patches
  rather than by anything failing. Damage merges per ROW, so a row stays
  pending until a different row is touched; `sb_pushline` runs from the state
  layer's `premove`, which is *before* the screen layer's `scrollrect` and
  therefore before the flush that `vterm-patch/0003` moved. A row written and
  then scrolled away before anything else happened was still pending, and the
  mirror had never been told: measured with `HELLO` then `CSI S`, the scrollback
  line came back **empty**, not merely stale. Ordinary output cannot reach it,
  since writing has moved down the screen long before the scroll — which is why
  it survived every benchmark and every existing test. Fixed by asking for the
  flush at the top of the callback, where the buffer has not moved yet and the
  pending rectangle still describes it. **The same ordering argument as 0003,
  one callback earlier**, and the fix belongs here rather than in a fifth patch:
  upstream hands us correct cells in `cells[]`, and reading the mirror instead
  is our optimisation, so its cost is ours to pay.
  **Measured A/B, interleaved**, because the machine was under load 44 and
  single runs swung 8×: **200×50 3.15 → 3.97 MB/s (+25%)**, **80×24 8.87 →
  10.6 (+18%)**.
- **Damage conversion is what remains, and it is not waste — [DECIDED] leave
  it.** Ablating inside the per-cell path put it at roughly `vterm_screen_get_cell`
  19%, the conversion 12%, and the store the rest, with **no single dominant
  cost** to remove. Two things were tried against that and both were dropped:
  **Writing through a row pointer** instead of `set_cell()` per column, to skip
  the bounds check, the ring arithmetic and QList's per-write detach check.
  Measured neutral — best of five, interleaved, 5.04 vs 5.14 MB/s. The
  ablation's apparent 27% was the memory write itself, which no restructuring
  removes. Reverted rather than kept: a longer function that inlines a helper
  for no measured gain is churn.
  **Converting fewer cells.** 20,000 lines of ~54 characters through a 200×50
  grid convert **3.98 million cells**, and 19,811 of 20,000 damage rects are
  full width — the same 3.99M/2.82M split the scrollback trimming already
  exploits. But it is not the same opportunity: the scroll blanks the row and
  the write fills part of it, the two damages merge, and the row genuinely did
  change across its full width. Those blanks have to be written. Skipping them
  would need to know they are blank, which needs the `get_cell` that is being
  avoided.
  Further gains here would need a vendored libvterm patch exposing bulk cell
  access, which is a change to a dependency for a cost that is now spread thinly
  across three unavoidable steps. Not worth it at this size.
- **Throughput, measured separately from painting** — by `make bench`
  (`test/emulator_bench.cpp`, §12.3), which is where every MB/s figure below
  comes from and how to re-derive it. They are timings on a particular machine,
  so treat them as ratios and as a record of which change moved what, not as
  numbers to compare against a fresh checkout on other hardware. Scrolling output ran at
  **0.2 MB/s** while a full-screen TUI repaint — more bytes, no scrolling —
  managed 21 MB/s, and the gap tracked the grid size rather than the output. A
  scroll was handled by re-reading the whole destination out of libvterm and
  converting every cell, so one scrolled line on a 200×50 grid cost ten
  thousand cell reads. The mirror moves its own rows now (a memmove each) and
  lets libvterm's damage fill in what the scroll exposed: **0.2 → 3.3 MB/s**,
  and 9.6 MB/s on an 80×24 grid.
- **Damage merged per row, not per cell.** libvterm's default is
  `VTERM_DAMAGE_CELL` — one damage callback per printed cell — so the mirror
  was told about a screen's worth of output one character at a time:
  **1,188,841 callbacks** for 20,000 lines of build output, and **19,900,000**
  for 2,000 full-screen repaints. `VTERM_DAMAGE_ROW` coalesces each row into
  one rectangle: **20,001** and **100,000** callbacks respectively, for
  identical work. That is a count rather than a timing, so it is the same on
  any machine; the timings that follow are not, and were taken interleaved on
  a loaded desktop. A full-screen repaint went **28 → 38 MB/s**, 80×24
  scrolling **10.4 → 13.0**, 200×50 scrolling 3.5 → 4.1, and coloured
  scrolling not at all — the merge removes per-callback overhead, not the
  per-cell conversion that dominates when every cell carries an SGR change.
  Merging makes damage **deferred**, which costs two things: `feed()` must
  flush, or the tail of every write sits invisible until the next one (a shell
  prompt is a write with no newline followed by silence), and libvterm's own
  `scrollrect()` flushed *after* moving its buffer, handing back a pre-scroll
  rectangle pointing at post-scroll cells. That second one is upstream's bug
  rather than ours and is fixed in `vterm-patch/` (§7.1) — it is invisible
  under the default, because per-cell damage is delivered synchronously and
  nothing is ever pending when a scroll begins.
  The count is what CI guards (§12.3), measured at the emulator's own
  `damaged()` signal: **31,977 repaint requests for 500 lines of output on the
  default, 2,001 merged**. Note the shape of what is left — four per line, not
  one. Once damage is merged, most of what remains is **cursor movement**, which
  is the next thing to look at if this ever needs to go further.
- **The scrollback stored a grid row per line, not a line.** libvterm hands over
  a full-width row every time one scrolls off, so a 58-character line of build
  output on a 200-column grid was converted and kept as 142 cells of nothing:
  **2.82 of 3.99 million converted cells were trailing blanks**, and the cost
  was set by the *window width* rather than by the output — the same log cost
  more to scroll past a wider window. Dropping the tail took the default
  5000-line buffer from **22.9 MB to 6.8 MB** at 200×50, and the useful part is
  that 6.8 MB is now what it costs at 80×24 **as well**: scrollback is priced by
  content instead of by how wide the window happens to be, which is the property
  that matters on a phone. Throughput gained about a tenth (3.9 → 4.5 MB/s at
  200×50), which is the smaller half of the result. "Blank" means *draws as
  nothing* — a space with a background colour is a status bar and is kept, and
  libvterm reports a wide glyph's continuation half as a character rather than a
  blank, so a trim cannot cut a CJK character in two.
- Snap the cell grid to integer pixel metrics to avoid blurry text and
  accumulating rounding error across 200 columns.
- Respect **display density / device pixel ratio** on every platform and expose a
  font-size control; recompute cols×rows and fire `window-change` on change (§7).
  Desktop adds two cases Android doesn't have: **fractional scaling** (125%,
  150% — where integer cell snapping matters most) and **dragging a window
  between monitors of different DPI**, which must re-lay-out the grid rather
  than scale a stale one. Test both.
  **The second was not merely untested — nothing was listening.** No file
  under `src/` referred to `QScreen` at all, so a window dragged to a
  differently-scaled monitor never recomputed anything: the cell is snapped
  to whole *device* pixels, which makes it a function of the device pixel
  ratio, and Qt simply rescaled what was already drawn. That is the "scale a
  stale one" this bullet names, and it costs twice — box-drawing stops
  meeting at the cell edge, exactly as it did at 150% before snapping, and
  the remote goes on laying out for the column count the other monitor gave.
  `QWindow::screenChanged` is the watch, armed on first show because
  `windowHandle()` is null before that. `QEvent::DevicePixelRatioChange`
  would be tidier and arrived in Qt 6.6, above the 6.4 the CI runner carries
  (§12.4). The layout width is recomputed with it, since a move between
  monitors can change a window's width in dp without any resize.
  **What a test can hold is the wiring, and that is deliberate.** The
  re-layout needs two screens of different scaling and the offscreen
  platform has one, so the test asserts the window is *listening* — through
  `disconnect()`'s return value, the way §8.3 pins the tab bar's menu
  connection — and that a re-measure on an unchanged screen moves nothing,
  since this fires on an event the window does not control and one that
  resized every session each time would be sending window-change for no
  reason. The behaviour on real hardware remains unverified.
- **[OPEN]** Whether to honour the system font-scale accessibility setting.
  Leaning **no** by default for the terminal grid (it would silently change
  column count), but do honour it for app chrome. Offer it as an explicit
  setting.
  **What is measured, so the decision starts from facts rather than from this
  paragraph.** *Nothing in this tree reads the system font scale* — no
  reference to `fontScale` anywhere in `src/`. The grid builds its own `QFont`
  and sets an explicit point size (`bssh_font_catalog::make_font`); app chrome
  inherits the application font, which nothing here overrides — the only
  `setFont` calls are two page titles.
  **So whether the leaning is already the behaviour depends on a fact about
  Qt that has not been measured here**, and it is worth naming rather than
  assuming, because the two possibilities give opposite answers. If Qt applies
  Android's `fontScale` by adjusting the *default application font*, chrome
  scales and the grid does not — and the leaning is already true, for free. If
  Qt folds it into logical DPI instead, both scale together, and the leaning
  is not merely unimplemented but **unimplementable without new code**, since
  nothing in the tree distinguishes the two paths.
  **Run, on a Galaxy Note 9 (Android 10, API 29, 420dpi), and the answer is
  NEITHER.** The system font size was moved from 0.8 to 1.3 and the app was
  restarted: of the app's own pixels, **0.14%** differed between the two
  renders — noise from an animated preview strip, with every label, field and
  row identical in size and position. Qt does not apply Android's `fontScale`
  at all, to chrome or to the grid.
  So the leaning's first half is already satisfied — the grid does not move,
  which is what §9.5 wants — and its second half is **not implemented and
  needs new code**. It is implementable, and the mechanism is the one measured
  above: setting the application font from `fontScale` would move chrome and
  leave the grid alone, because the grid sets its own point size explicitly in
  `bssh_font_catalog::make_font`. What remains is the decision, not the
  question.

### 9.6 CI font validation
**Built** (`tool/check-fonts.py`, `make check-fonts`, run in CI): for every
bundled font it asserts uniform advance width, presence of the required
Unicode blocks (§9.1), the required weights, and a recognised licence file,
and it **fails the build** where a font doesn't qualify. This is how "only
fonts suitable for a terminal" stays true as the set evolves rather than being
a one-time intention.

---

## 10. Host presets and profiles

**[DECIDED]** Presets are **reusable named templates** attachable to many hosts —
not per-host settings duplicated everywhere, and not just shipped defaults.

### 10.1 The layering model
Three layers, highest precedence wins:

```
App defaults  ←  Preset ("work-bastion")  ←  Host profile overrides
   (lowest)                                       (highest)
```

- A **host profile** references **one preset** and may override any individual
  field.
- Editing a preset **propagates live** to every host using it — that's the whole
  point. The UI must make this obvious ("used by 7 hosts") before saving.
- Overridden fields show an explicit badge with a one-tap/one-click "revert to
  preset value", so users can always see why a host differs.
- **Reading an override and writing one are the same question, so they are one
  function** (`bssh_appearance_overrides` / `bssh_font_override_badge`). Both ask
  "does this differ from what the host *inherits*", and both must exclude the
  host's current override when working that out — comparing against what it
  resolves to *with* the override applied compares the value against itself and
  can never see agreement. Splitting them is not a tidiness question: the desktop
  dialog had it wrong on the write side alone, so choosing the value the preset
  already gives recorded an override pinning it **while the badge two lines away
  said "Following"**. The screen and the store disagreed about the same host, and
  the host quietly stopped following the preset it was displayed as following.
- **An override is cleared by agreeing, not only by reverting.** Insert-only
  leaves a host that has been talked back into matching its preset still carrying
  an override, which stops following the next time the preset changes. The revert
  button is the explicit route; picking the inherited value by hand has to mean
  the same thing, or the two controls disagree.

### 10.2 What lives in a preset (shareable, non-secret)
Terminal type/`TERM` string, colour scheme, font family + size (§9), on-screen key
row layout + keymap (§6.2), on-connect commands (e.g. auto-attach `tmux`),
encoding, scrollback size, cursor style/blink, bell behaviour, keepalive interval,
compression, algorithm preferences (ciphers/KEX/MACs), and default split/tab
behaviour for that host type.

#### The cursor — **[DECIDED]**
Three rules, all asked for directly and all konsole's convention before that.

- **It does not blink by default.** A blinking cursor is movement on screen that
  nothing caused, twice a second, for as long as the app is open; on a phone it
  is also a wakeup twice a second per pane, which §5.1 cares about for its own
  reasons. Anybody who wants it can have it, and nobody should have to turn it
  off. The default lives on `bssh_preset_t`, and every built-in inherits it —
  they are default-constructed presets with a few fields changed. Pinned there
  rather than on the view, because an absent key already reads as false when the
  view applies a settings object, so a test on the view would pass whatever the
  default said.
- **Only the focused terminal blinks.** Four panes each animating a cursor is
  four things moving on a screen where one of them is the one being typed into:
  the movement means *here*, and it means nothing when everything says it. The
  pane still remembers it was asked to blink, or focus arriving would leave it
  steady for ever — those are two separate facts and the sabotage set breaks
  each of them separately.
- **An unfocused terminal draws a hollow block**, whatever cursor shape it would
  otherwise use. Whatever shape, because the outline is doing a different job
  from the shape: the shape says what kind of cursor the host asked for, the
  outline says *your keystrokes are not going here*, and an unfilled bar or
  underline is a one-pixel line with a one-pixel hole in it. This is §8.3's
  focus-border question — the one it calls non-negotiable — asked of the one
  mark that is on screen even with a **single** pane, where that border is
  deliberately not drawn. It reads `hasFocus()` for that reason rather than the
  border's own flag.
  Asserted on the pixels, since the whole property is what somebody sees: a
  boolean saying "hollow" passes with the painter drawing a block from it. The
  first draft of that test compared two grabs of the *same* top-level view and
  found them identical — Qt hands a shown top-level with StrongFocus its focus
  immediately, so both images were of a focused pane and would have stayed
  identical with the hollow cursor deleted. It needs something else in the
  window that can hold focus.

#### Colour schemes — **[DECIDED]**
§10.2 gave a preset a `colour_scheme` field and never said what one is. It is
the sixteen ANSI colours plus a default foreground, background and cursor —
curated, compiled in, addressed by a stable id, identical on both platforms, on
the same pattern §9.2 uses for fonts. Six to start with, each recording its
licence because these are other people's designs:

| Scheme | Licence | Why it is in the set |
|---|---|---|
| **Default** | BeerSSH | The xterm-ish palette this renderer already used. Deliberately the default, so the day schemes landed nobody's terminal changed colour unasked. |
| **Solarized Dark / Light** | MIT (Ethan Schoonover) | The most widely recognised pair; one palette, two backgrounds. |
| **Gruvbox Dark** | MIT (Pavel Pertsev) | Warm, high-contrast, very widely used. |
| **Nord** | MIT (Arctic Ice Studio) | Low-contrast cool palette; the most requested modern scheme. |
| **Tango Dark** | Public domain (Tango Desktop Project) | What GNOME Terminal shipped for years — the one a Linux user recognises without knowing its name. |

**A scheme owns the low sixteen and nothing else.** The 6×6×6 cube and the grey
ramp above them are exact formulas every terminal computes identically: a program
asking for colour 137 has asked for a particular RGB value, not for the user's
taste, so recolouring those would make this terminal disagree with every other
one about what 256-colour output looks like. That is a bug wearing a feature's
clothes, and there is a test asserting it does not happen.

**Presets are platform-neutral by construction.** A preset describes the *host*,
not the device looking at it, so it holds no platform-conditional fields — the
key row layout it carries is simply ignored where no key row is shown (§6.2). A
preset exported from a phone must import unchanged on a desktop and vice versa;
if a field can't satisfy that, it belongs in local settings, not in a preset.

### 10.3 What must NOT live in a preset
**Hostname, port, username, passwords, private keys, key references, known-host
entries.** These stay on the host profile and in the secret store (§11).

This separation is a **security requirement**, not tidiness: it's what makes a
preset safe to export and share without leaking credentials or revealing
infrastructure. Enforce it in the type system — a preset should be structurally
incapable of holding a secret, so an export path can never accidentally serialise
one.

**Built, and now asserted on the artifact that actually travels.** The type has
no field a secret could occupy, which is the guarantee — but the test read
`to_json()`, an intermediate nobody sends. What lands in `~/Downloads` and gets
attached to a chat window is the INI, and nothing had ever looked inside one.
`write_ini()` is built on `to_json()` so the property does transfer, but
transitively: a field added straight to the writer would not have been caught
from the other end. The exported file's own bytes are checked now.

**And the hostile direction is covered from both entry points.** A shared
preset claiming `is_builtin` would take a read-only status and an id it was not
given, and the shipped set is what every other preset is cloned from. That was
tested against `from_json()` and not against `read_ini()` — the reader that
actually opens a file somebody sent. Three layers turned out to be guarding it:
`from_json()` hard-sets both fields, `read_ini()` sets them again afterwards,
and a filter inside `read_ini()`'s loop skipped `id` and `builtin`. **The field
is `is_builtin`, so that third one matched nothing** — and it was dead
regardless, since either of the others is sufficient. It is gone: a guard that
cannot fire still reads as protection, which is worse than no guard, because it
invites the next reader to weaken the one doing the work. Removing each
remaining layer in turn shows both are real.

### 10.4 Built-in starter presets
Ship a handful of **read-only** presets that users clone rather than edit:
*Default Linux*, *Raspberry Pi / SBC*, *OpenWrt / BusyBox router* (limited `TERM`,
no colour assumptions), *BSD*, *Network switch / serial-ish* (conservative
sequences, no UTF-8 box drawing), *tmux-first* (auto-attach on connect).

Each sets sensible `TERM`, colours, and key-row defaults for that target. These
double as documentation of what a preset can do.

### 10.5 Import / export
- **Export works on both platforms. Import works on both platforms**, by two
  different routes, because the two platforms hand a file over differently:
  - **Desktop:** the file picker inside the app, or a path on the command line
    (which is what a file manager passes to a registered handler).
  - **Android:** being **given** the file — share a preset to BeerSSH, and the
    launch intent's URI is read and imported. Verified on a device with a real
    SAF-granted `content://` URI: opened, parsed, imported, and name-collided
    into "BSD (imported)" rather than overwriting.
  - **[OPEN]** The in-app picker still cannot *select* a file on Android
    (`ACTION_OPEN_DOCUMENT` lists preset files greyed out, and a `.txt` behaves
    the same as an `.ini`, so it is not about filters). It is left in place
    because it costs nothing and may be an emulator-image quirk; the hand-over
    route above is the one that works and is the more Android-shaped gesture
    regardless.
  - Deliberately **not** claimed: `ACTION_VIEW` on arbitrary `content://`
    documents. Matching those needs a filter on `application/octet-stream` with
    no path pattern, which would put BeerSSH in the "open with" list of every
    unknown binary on the device — a rude thing to do, and a lie, since the only
    file this app can open is a preset.
- **An imported name is made unique by the store, not by the importer**
  (`unique_preset_name` / `unique_profile_name`). Three call sites each spelled
  the rule themselves — append " (imported)" once, without re-checking — and once
  is not enough: importing the same preset three times produced three rows all
  called "BSD (imported)", told apart only by a slug nobody is shown, while §10.4
  refuses to delete a preset a host still follows. It sits beside `unique_slug`
  because it is the same job one layer up: that one keeps the storage key
  distinct, this one keeps what the user *reads* distinct. Note the first import
  of a starter preset already collides, so this is the ordinary path rather than
  an edge case.
- Presets export/import as **INI** — the same format they are stored in (§10.6),
  so exporting one is copying a section out and importing is pasting it back,
  with a `schema` key for forward compatibility. Human-readable, diffable and
  shareable were the reasons the format had to be one of those; being *the same*
  format as the store is what stops export from being a second serialiser that
  drifts from the first.
- **OpenSSH `~/.ssh/config` import — built** (`bssh_ssh_config`). An explicit
  import and never a live read: following the file at connect time would mean
  BeerSSH's behaviour changed when a different tool's configuration did, with
  nothing on screen to explain why. Hosts are copied in once and are this app's
  to edit afterwards. Reached from the saved-sessions buttons on desktop and the
  overflow menu on touch; the file is chosen, parsed, and offered as a ticklist,
  because a real config accumulates dead machines and aliases for git forges that
  are not shells, and importing all of it is the rude version of the feature.
  Three pieces of OpenSSH's semantics are load-bearing and a parser that skips
  them produces entries that look right and connect elsewhere: **the first value
  for a keyword wins, not the last**; **patterns are not hosts** (`Host *`
  configures the concrete aliases it matches but is not itself a machine, and a
  matching `!negation` vetoes its whole block); and **`Match` cannot be evaluated
  statically**, so its contents are skipped rather than attributed to the `Host`
  above it. `Include` is not followed — reaching into files nobody pointed at is
  the live-read behaviour this feature exists to avoid.
  **`IdentityFile` is honoured**: it becomes the profile's `identity_file` and
  reaches libssh as `SSH_OPTIONS_ADD_IDENTITY` (§11). Verified against libssh
  rather than assumed — the option **prepends**, so the named key is offered
  first and the default identities still follow behind it, which is what
  OpenSSH's `IdentityFile` means in the absence of `IdentitiesOnly`. The tilde is
  expanded here rather than left to libssh, which stores the string as given
  (`ssh_options_get` hands back `~/.ssh/id` unchanged) and expands only when
  loading the key: a key that silently fails to load looks exactly like a server
  refusing it. Only the **path** is stored, never key material — which is what
  lets it sit on the profile at all under §10.3.
  **Editable on both front doors** (§8.5, §14): "Private key file" beside the
  destination in the PuTTY-shaped dialog, with a Browse button starting in
  `~/.ssh` and no name filter, since a private key has no extension and
  filtering by one would hide every file worth choosing; "Key file" on the touch
  form, typed, with no Browse — nothing in the app can get a key onto a phone
  yet, so a picker there would promise a workflow that does not exist, and what
  the field is really for on touch is reading and correcting what an import put
  there. Blank means the default identities on both, and both say so in the
  placeholder rather than leaving an empty box looking unfinished. It is filed
  with the destination rather than under PuTTY's Connection → SSH → Auth because
  that page is still a placeholder, and a field nobody can find may as well not
  exist.
- **`ProxyJump`** — **[DECIDED]** carried onto the profile and honoured, one hop.
  What `-J` does, and what makes it different from logging into the bastion and
  typing `ssh` again: the SSH protocol to the **target** runs end-to-end inside a
  plain TCP forward the bastion carries and cannot read. The target's host key is
  checked here, the target's credentials are offered here, and no shell is ever
  opened on the bastion. Hopping by hand gives the middle machine your session;
  this does not.
  **The shape libssh forces** (`bssh_jump_tunnel`): a session to the bastion, a
  `direct-tcpip` channel from it to `target:port`, and a **socketpair** between
  that channel and the target session — because libssh takes a file descriptor as
  a session's transport and not a channel. The pump between the two runs on **its
  own thread**: the target's `ssh_connect()` blocks through its handshake and
  every byte of that handshake travels through this tunnel, so pumping it from the
  same thread would deadlock on the first packet. `SSH_OPTIONS_HOST` stays the
  **target's** name, which is what its key is checked against and what a
  known-hosts entry is filed under.
  **The bastion is checked, never prompted about.** It is verified against the
  same known-hosts file as everything else, and an unknown or changed key is a
  refusal with a reason ("ssh to it once so its key is recorded") rather than a
  second trust-on-first-use dialog. A prompt about a machine the user did not ask
  to visit, while the connection they *did* ask for waits behind it, is a prompt
  nobody can answer sensibly, and the alternative — teaching people to click
  through key warnings — is worse than the inconvenience. For the same reason it
  authenticates with keys and the agent only: a bastion password would be a
  second credential flow that a reconnect could not supply anyway (§11 keeps no
  password past first use).
  **One hop.** OpenSSH allows a comma-separated chain and the same mechanism
  nests, but each hop needs its own host-key decision and its own credentials. A
  chain is refused rather than truncated — taking only the first hop of `a,b`
  would route through a machine the user named as a *step*, which either fails
  confusingly or, worse, works by a path nobody chose. The import reports a chain
  per host for that reason; a single jump host it simply brings across.
  **Editable on both front doors** (§8.5, §14): "Connect via" beside the key in
  the desktop dialog and "Via" on the touch form, both spelled the way OpenSSH
  spells it, since that is the string the user already has. Blank is a direct
  connection and both placeholders say so.
  **The tunnel's own socket gets §5.3's options too.** A session reached through
  a bastion has *two* sockets, and the one the app never sees is the one carrying
  everything. Left at the system defaults it probes after two hours, so a link
  that goes silent — a phone losing signal, not a server refusing — was noticed
  on the target's socket and never on the bastion's, and the session stayed
  "connected" indefinitely. Measured: with the options, 66s; without them, still
  believed good after 209s, at which point the test gave up.
  **libssh does not own a descriptor given to it with `SSH_OPTIONS_FD`.** Measured
  against 0.11.2: a socket handed over is still open after a full handshake,
  `ssh_disconnect` and `ssh_free`. Believing otherwise leaked one descriptor per
  connection *attempt*, and a host behind a bastion on a flaky link retries for
  as long as it takes.
  **The pump shuts its local end down however it exits.** Three of the ways out
  are the bastion dying, and leaving the socket pair open after them left the
  target session holding a descriptor that was open and permanently silent —
  never readable, never an error, so never a drop. Shutting it down turns each
  into the ordinary drop §5.3 already handles.
  **Checked against a real server** (§12.3): `tool/check-ssh.sh` uses the one
  throwaway sshd as *both* bastion and target — it carries a channel to itself,
  and a second independent SSH session runs inside it. A bastion that is not
  listening must stop the connection, which is what keeps the check from passing
  on a build that ignores the jump host and simply dials the target. A third
  phase kills the server under a live tunnelled session — its *children*, since
  sshd forks per connection and killing the listener alone leaves every
  established session running, which is how the first version of that phase
  "proved" a session survives a death that never happened. `check-keepalive.sh`
  adds the silent case, black-holing the port under a tunnelled session. And a
  descriptor count across four tunnels catches the leak above.
- **[OPEN]** Host groups/folders with inherited settings. Presets cover most of the
  need; add folders only if organising many hosts proves awkward.

### 10.6 Storage — **[DECIDED]** plain INI files
- **INI files**, one per kind, in the app's config directory. Same format and
  same layout on both platforms:

  | file | holds |
  |---|---|
  | `~/.ssh/beerssh_hosts` | `[host-3]` — name, host, user, port, key file, jump host, which preset it follows, and any per-field overrides as ordinary keys |
  | `presets.ini` | `[servers]` — the preset's fields, section named by slug |
  | `layout.ini` | `[layout]` — which hosts were open, and how |
  | `app.ini` | this device's own: `[appearance]` theme, scrollbar, status line, window-title format, contrast floor; `[input]` key row, terminal menu, Caps-as-Ctrl, focus-follows-mouse; `[chords]` one line per rebindable chord (§6.1); `[security]` hide-from-screenshots |

  **Everything in `app.ini` is written as WORDS**, not as the enum's number:
  `scrollbar=left` says what it does where Konsole's own `scrollbar=2` needs a
  source file to decode, and these files exist to be opened in an editor. A value
  nobody recognises reads as the default rather than refusing to start, which is
  what a hand-edited file with a typo in it deserves.

  **The host list lives in `~/.ssh`, and only the host list.** It is SSH
  material — a name, a host, a user, a port, a key path, which is what
  `ssh_config`'s `Host` blocks hold — and someone backing up `~/.ssh` expects to
  take it with them. Presets, layout and device settings stay in the config
  directory: a font size, a colour scheme and which tabs were open are terminal
  furniture with no business in `~/.ssh`. The encrypted secret store stays out
  too (§11).
  Named `beerssh_hosts`, **not** `beerssh_config`, which was the obvious choice
  and the wrong one: a file called `*_config` beside OpenSSH's `config` promises
  `ssh_config`'s grammar, and this is INI. `beerssh_hosts` mirrors `known_hosts`
  and promises only what it delivers.
  **Ordinary desktops only.** `bssh_platform::openssh_directory()` answers empty
  where `~/.ssh` is not a real place, and everything falls back to the config
  directory. This is a platform question rather than a path one: Android *has* a
  writable home — the app's own sandbox — so probing whether `~/.ssh` can be
  created succeeds there and produces a directory no ssh client will ever read.
  An existing `hosts.ini` is **renamed** on first run, not copied: two files both
  claiming to be the host list is the drift this tree keeps getting caught by.

  This replaced an earlier SQLite decision, and the reasoning is worth keeping:
  the data is tens of rows, there is no query that is not a full scan and no
  concurrent writer, so what the database was actually costing was the ability
  to **read, diff, copy between machines, or fix by hand** the configuration of
  a terminal — for a tool whose users live in text files. Migrations become
  "read the key if it is there, default it if it is not", which is what a config
  file has always done. Known-hosts is unaffected: it was never in the database,
  it is libssh's own file (§11).
- **Presets are addressed by slug, hosts by number.** `preset=servers` survives
  being edited by hand; `preset_id=4` survives nothing. Numeric ids still exist
  in memory, where a dialog needs a stable handle, and are persisted so they
  keep meaning the same thing across a restart.
- **A key that is not one of the host's own is an override** of its preset
  (§10.1). That is what makes an override something a person can add by typing
  it, rather than something requiring a nested syntax.
- Secrets **never** in these files — platform-wrapped blobs only (§11). A host
  carries `has_password`, a boolean saying a credential exists in the platform
  store, never the credential. That split is what makes the config safe to copy
  to another machine, where the flag is true, the keyring is empty, and the app
  simply asks.
- **Locations come from `QStandardPaths`**, never a hardcoded path:
  `AppConfigLocation`, which is `~/.config/beerssh` on Linux and the app-private
  directory on Android without either being special-cased.
- **These are normal user files** — visible, backed up, and *meant* to be
  opened. Assume they will be inspected and hand-edited: that is an argument for
  readable keys and for keeping the secret/non-secret split (§10.3) obvious,
  not for obfuscating anything.
- `tool/migrate-db-to-ini.py` converts a database written by an earlier build.
  A script rather than app code, because the app no longer links Qt SQL at all
  and carrying a database driver forever to read one file once is a poor trade.

---

## 11. Security & credential storage

All of it goes behind one interface — `bssh_secret_store` (`store`, `load`,
`erase`, `is_hardware_backed`) — with one implementation per platform, per §3's
core principle 3. Callers get an opaque ID back and never see a key mechanism.

**A discarded `ssh_options_set` could have skipped the jump host without
saying so.** Found by asking the rigorous form of the question rather than
stopping at the first instance: *every* libssh call in
`ssh_session_libssh.cpp` whose result is thrown away. There were eleven,
and one of them decides what the connection IS.

`SSH_OPTIONS_FD` hands libssh the bastion tunnel's descriptor. Its failure
mode is not a failed connection, which is what makes it worth this entry:

- `SSH_OPTIONS_HOST` is deliberately the **target's** name, so the host
  key is checked against the right machine (see the comment there);
- with no descriptor set, `ssh_connect` opens its own socket **straight to
  the target**;
- the host key verifies, authentication proceeds, the session is
  indistinguishable from a correct one;
- the tunnel stays open beside it, pumping nothing.

So a user who configured ProxyJump — often a policy and audit boundary
rather than merely a route — gets a session that never went through it,
and nothing anywhere says so. The known-hosts paths are the same shape:
unset, libssh falls back to its own defaults, so the app verifies against
a different store than the one it believes it is using.

The options that decide *what the connection is* — the descriptor, host,
port, user, and both known-hosts paths — now go through a checked helper
that fails loudly. **Compression and the identity file deliberately do
not.** Losing either degrades the session without changing which machine
it reaches or what its key is checked against, and a connection refused
over a compression flag is the worse outcome. That line is drawn in the
code rather than left to whoever edits next.

The tell was an asymmetry again, and a stark one:
`bssh_jump_tunnel::write_to_channel` handles `SSH_ERROR`, `SSH_AGAIN`,
zero and `EINTR` meticulously, ten files from a `do_send` that discards
everything `ssh_channel_write` returns. **A tree that already contains the
careful version of a thing is the cheapest place to notice the careless
one** — the same search `working-practice.md` names for decisions, applied
to error handling.

Still open and deliberately not bundled here: `do_send` and `do_resize`
discard their results too. Losing keystrokes silently and letting the
remote keep a stale window size are real, and they are a different fault
from a bypassed bastion; putting them in one commit would file both under
whichever subject the message chose.

**And libssh has six authentication results, of which exactly one means
the credential was wrong.** `do_authenticate_password` tested for
`SSH_AUTH_SUCCESS` and called everything else `"Authentication failed"`.
The same collapse as the known-hosts one above, one layer down the stack,
and found by looking for it there.

The tell was an asymmetry rather than a reading of the enum: `PARTIAL`
falls through to the next prompt in **both** key paths — which is correct
— while the password path called it a failure. The same value handled
sensibly in one place and as a failure in another is a question worth
asking, and the key paths were right only by accident of testing for
`SUCCESS`.

What it cost: a server with `AuthenticationMethods
password,keyboard-interactive` returns `SSH_AUTH_PARTIAL` for a password
that was **right** — it was taken, and another factor is wanted. The user
was told authentication failed, so they retyped a correct password
indefinitely. That is the worst instruction the program can give, because
it sends someone to change the one thing that was working.

`SSH_AUTH_ERROR` was the same misdiagnosis: a transport or protocol fault
reported as a rejected credential, sending the user to fix what was not
broken.

Three things are now deliberate rather than incidental. A partial
acceptance says plainly that the server took the credential and wants a
method this client cannot answer yet — no blame on what was typed. An
error reports libssh's own message. And an error in the **key** paths now
fails rather than prompting: asking for a credential after a fault asks
somebody to answer for something that never reached a verdict, and their
answer cannot help.

Multi-factor itself is neither built nor promised — this document has
never mentioned it — and this does not build it. Reporting accurately is
not the same as supporting it, and saying "cannot answer yet" is the
honest form of that.

The test worth copying is the property one: of the six results, only
`SSH_AUTH_DENIED` may map to "wrong credential". That is the claim the
bug violated, and it is what a seventh value added carelessly would trip
over. Sabotage: restoring the old mapping fails exactly four tests, all
of them naming this rule, and nothing else in the suite.

**libssh has six known-hosts verdicts and this handled two, so "I could
not check" was shown to the user as "this host is new".** `verify_host_key`
tested for `SSH_KNOWN_HOSTS_OK`, then for `CHANGED`, and sent everything
else to the first-contact prompt — the one whose button reads *Accept and
connect*. Two of the remaining four do not belong there, and libssh's own
header says why:

| verdict | libssh's description | was |
|---|---|---|
| `UNKNOWN`, `NOT_FOUND` | not recorded | first contact ✓ |
| `CHANGED` | *"you HAVE to warn the user about a possible attack"* | warning ✓ |
| `OTHER` | *"a key of a type while we had an other type recorded. It is a possible attack"* | **first contact** |
| `ERROR` | *"there had been an error checking the host"* | **first contact** |

`ERROR` is the one that matters. An unreadable or malformed known-hosts
file means the check **did not run**, and the user was told the host was
not recorded and invited to trust it. *"I could not tell"* and *"it is not
there"* are different answers, and rendering them identically is
`evidence.md`'s vacuous pass at its most expensive: a trust decision taken
on the strength of a verification that never happened. It now fails the
connection with its own message — there is nothing to decide when nothing
was verified, and every other branch holds the connection open precisely
because there is.

`OTHER` routes to the existing warning, which needed no new dialog: its
words already fit — *the key for this host is NOT the one recorded*.

**Why it survived: the file it lives in cannot be unit-tested.** Every
line of `ssh_session_libssh.cpp` needs a real server, so `make check-ssh`
is the only thing that has ever reached it — a gate somebody must
remember to run, against a host they must have. So the most consequential
rule in the program had no test. The decision is now
`bssh_host_key_verdict()`, a pure mapping that can be asked without a
socket, which is the same move `bssh_tab_status::state_for` makes for the
tab bar and with rather more at stake. **A rule worth this much should not
live where only a gated check can see it**, and that is the general form
worth carrying: when a decision cannot be tested where it sits, the fault
is usually where it sits.

Two details of the mapping earn their place. It takes an `int` rather
than `enum ssh_known_hosts_e`, because that header promises no libssh type
appears in it so a libssh2 backend could replace the `.cpp` without
touching its includers. And it switches on that `int` rather than casting
to the enum, because casting a value libssh never defined in order to
switch on it is undefined behaviour and coping with exactly that value is
the function's purpose — the cost being that `-Wswitch` can no longer warn
about an unhandled enumerator, which the default clause covers by
refusing rather than trusting. A libssh that grows a seventh verdict
cannot make it silently mean *trusted*.

Checked by sabotage: restoring the old mapping fails exactly the two
tests naming those two verdicts, and nothing else in the suite.

**And the credential erase had a second road into the same bad state,
which the first fix did not close.** `offer_to_remember` wrote the secret
and *then* read the row back to set its flag, bailing if the row had
gone. A session outlives the deletion of its host (§8.4) and keeps the old
id, so the prompt still offers to remember — and storing first meant a
credential written under an id with no row behind it, followed by a
polite refusal to record the flag. An orphan nothing will ever erase:
exactly what deleting a host was made to prevent, by another road.

Two things about the shape, because both are easy to repeat:

- **Closing one route to a bad state is not closing the state.** The
  earlier work asked "what happens when a host is deleted?" and answered
  it. The question that covers this is "how can a credential come to
  exist with no row behind it?", which has more than one answer. A fix
  believed complete because it addressed the case that prompted it is the
  same fault this document records for the store's own multi-window
  entry.
- **The guard existed and was in the wrong place.** The row check was
  already written, three lines too late. Not a missing check — a correct
  one positioned so it could not prevent the thing it describes, which is
  harder to see than an absent one because a reader asking "is this
  validated?" finds that it is.

The ordering the original comment protected — the flag written only after
the secret lands — is kept. That comment was right about what it covered
and silent about the row's existence, and it read as covering more than
it said.

**Appending to `known_hosts` could corrupt the entry already there.**
`bssh_append_hashed_known_host` opened the file in `Append` and wrote its
line. Two faults, and they compound:

- **Nothing checked that the file ended in a newline.** If it did not — a
  hand-edited file is the ordinary way there — the new entry was joined
  onto the last existing one, corrupting the *previous* host's entry as
  well as failing to record this one. **One bad append costs two trust
  decisions**, in a file this app deliberately shares with OpenSSH (§11).
- **`write(...) > 0` accepted a partial write**, so a short write
  recorded a truncated entry as a trust decision made — and left the file
  without a trailing newline, arranging the corruption of the next
  append.

The sabotage names the damage better than the description does: with the
blind append restored, the failing assertion is *"the entry that was
already there must survive intact"*. Not the new entry — the one that was
already correct.

The rule is `bssh_append_line(path, line)` now, taking a path rather than
a session so it can be tested without a server. That is the third time
this section has had to pull a decision out of code only `make check-ssh`
can reach, and it is the same move as `bssh_host_key_verdict`. It asks
the last byte rather than reasoning about who wrote the file: this app
always terminates what it appends and so does OpenSSH, and the case that
matters is precisely the file neither of them wrote last.

`flush()` and `close()`'s error are checked too — the out-of-band failure
channel one more layer along.

**The credential store destroyed itself before writing its
replacement.** `flush()` opened the real file with
`QIODevice::Truncate`, so the existing store was gone before the new
bytes were written. An interrupted or failed write left a truncated
blob — and `unlock()` rejects a short blob, so what is lost is not the
secret being saved but **every credential in the file**.

The reporting made it worse. A truncated store fails to unlock, and the
UI says *"either it is wrong, or the store has been modified"* — sending
somebody to look for an attacker, or to doubt their own passphrase, when
this file truncated itself. **A wrong diagnosis is worse than a bare
failure**, and this is the third instance tonight of the same shape:
"could not check" shown as "new host" (§11), five auth results shown as
"wrong password" (§11), and now self-inflicted loss shown as tampering.

`QSaveFile` now writes a temporary beside it and renames over the target
on `commit()`, so a failure at any point leaves the old store exactly as
it was. `setDirectWriteFallback(true)` keeps the one case the old code
handled — a writable file in a directory that is not — because refusing
it would trade one failure for another.

**The sabotage found a regression the fix would otherwise have
shipped.** Removing `setPermissions` leaves `ReadGroup` and `ReadOther`
set: a naive temp-and-rename makes the credential file world-readable,
and *nothing else in the tree fails*, because the secrets stay encrypted
so no test of their content notices. The test therefore asserts the mode
rather than trusting that `setPermissions` survives a rename. The
general form is worth carrying: **when replacing a write with a
different mechanism, the properties to assert are the ones the old
mechanism gave you for free.**

**The same unchecked write was left in the sibling file, and it defeated
the orphan sweep.** `bssh_app_settings::store()` discarded `QSettings`
failures exactly as the profile store's three writers did — both files
were grepped when that was fixed, and only one of them was.

A cosmetic device setting failing to persist merely reverts at the next
start. The orphaned-secret list is where the silence is **durable**: it
records a credential the locked store could not erase, so a write that
vanishes means the sweep never comes back for it and the password
outlives its host for good. The cleanup was defeated by the exact silence
it was built to close.

`store()` reports now, `set_orphaned_secrets` and
`bssh_note_orphaned_secret` propagate, and `remove()` records it when a
credential can be neither erased nor scheduled — the row goes as asked,
and the store says the password outlived it.

**The test was worse than the bug, and sabotage is what showed it.** Its
first version skipped where it should have failed: the skip read *"if the
write succeeded, this account can write read-only directories"* — and
`store()` **returning** success is precisely what the fault looks like.
With the status check removed the test skipped and the suite reported
zero failures.

The rule that fixes it is one this document already holds in two other
forms — the receipt over the exit code, the remote ref over the push
output: **condition on the artifact, not on the claim.** It reads
`app.ini`'s bytes now, and directly rather than through `QSettings`,
which serves back values it failed to write.

And the general form, which is the part worth carrying: **a skip is a
branch where a test declines to check, so its condition deserves the same
scepticism as an assertion — more, because a wrong skip is invisible in a
green run.**

**A save that never reached the disk was reported as a save.** `QSettings`
swallows a failed write: `sync()` returns `void` and `status()` reports the
failure to whoever asks. Nothing asked, in any of the three writers. So an
unwritable config directory — a read-only home, permissions somebody
changed, a full disk — left `save()` returning an id and emitting
`changed()` exactly as success does, and the host was gone at the next
start with nothing said. This tree's own Makefile documents that failure
mode costing an afternoon, in the suite rather than the app.

Three halves, and detection alone would not have been a fix:

- **The store detects it.** All three writers check `status()` and set
  `last_error()`.
- **Memory cannot outlive the disk.** A failed write restores the
  pre-write snapshot. **Restored, not reloaded** — `load()` was the first
  attempt and it did not work, because QSettings shares one cached
  `QConfFile` per path, so a failed write leaves its values in that cache
  and a fresh `QSettings` on the same file reads them back rather than the
  disk. Measured, after a test that expected reloading to undo the change
  and got the row back. This matters more since the store became
  process-wide (§8.4): a phantom row used to die with its window and would
  now be shown as saved in every one.
- **The user is told.** Both front doors report it, because a host lost
  silently on one surface and not the other is §8.5's asymmetry again.

**`last_error()` was in the earlier "never called" sweep and was dismissed
as a harmless unused getter.** It was the reporting half of this: set in
exactly one place, read only by a test's failure message, and never
reachable for the failure it existed to describe. The generalisation worth
keeping is that **an accessor nobody calls is sometimes a missing caller
rather than dead code, and the way to tell is to ask what it would have
reported** — the question that found `erase()` and was not asked of this.

The test refuses rather than skips where the condition can be made, and
skips with its reason where it cannot: running as root, which the CI
container does, a read-only directory is still writable. It asserts both
directions, so a store that dropped everything on any write error would
not pass.

**`erase` had no caller in the tree, and deleting a host left its password
behind.** The separation this section is built on — the row holds a *flag*
saying a credential exists, the credential lives in the store under a key
derived from the row's id — was only ever wired in one direction. `save`
and `load` had callers; nothing joined deletion to `erase`, so a password
for a host the user deliberately removed stayed in their keyring, or in
the passphrase-derived file this section calls safe to copy between
machines, indefinitely. Half an interface unused is how it hid: nobody
looks for the caller of a function that exists.

Two things measured before the fix, because both look worse than they are
and a later reader should not have to re-derive either:

- **Ids are reused and it is not an inheritance.** `save()` mints
  `highest + 1` over the rows that remain, so a deleted host's key is one
  a future host will be given. It cannot read it:
  `stored_password_for()` returns before touching the store unless that
  row's own `has_stored_password` is set, and a new row's is not. The
  fault is an orphaned secret, not a credential handed to the wrong host.
- **A session cannot die inside a modal prompt**, so the host-key
  handlers' raw `session` pointer is sound where the password path's
  `QPointer` is necessary. Qt processes `DeferredDelete` only when
  control returns to the loop `deleteLater()` was called from, and
  entering a dialog's nested loop is not that. The password path differs
  because it is an *asynchronous* platform callback (Android's
  BiometricPrompt), which does resume on the outer loop.

**The locked-store half is answered too: write the key down, sweep it at
the next unlock.** libsecret erases whether or not this app has unlocked
anything; the passphrase-derived file cannot, because it re-encrypts the
whole blob, so `erase` returns false while locked.

The three candidates, and why this one:

- *Prompt for the passphrase on delete.* Rejected. The user asked to
  remove a host, not to prove who they are — it turns a deletion into an
  authentication, and a cancel still leaves the orphan.
- *Erase without unlocking.* Impossible for that backend by construction:
  one encrypted blob, so removing a key means re-encrypting all of it.
- *Record the key and sweep it later.* Taken. `bssh_note_orphaned_secret`
  writes it to `app.ini` and `bssh_sweep_orphaned_secrets` clears what it
  can whenever the store is unlocked.

**The sweep point arrives on its own, which is what makes it cost
nothing.** The store is unlocked to release a remembered password — a
thing users do routinely and never for this — so nothing is ever asked of
them and an orphan does not outlive their next connection to a host they
saved a password for.

Two properties worth stating because they are what keep it safe: the list
holds key NAMES and no secrets, and lives in `app.ini` rather than in the
secret store precisely because the secret store is the thing that cannot
be read while locked; and the sweep keeps every entry whose erase fails
and drops only those that succeed, so running it against a still-locked
store is a no-op rather than a silent loss.

- **Private keys / passwords** stored encrypted; never plaintext in app files,
  `SharedPreferences`, or the INI config files (§10.6).
  - **Android [DECIDED, built]:** secrets are wrapped with a key generated
    inside the **Android Keystore** — not derived from anything the user types,
    not extractable, and on hardware that offers one held in a TEE or secure
    element. The store keeps the same shape as the desktop fallback's: one
    encrypted blob holding a JSON object rather than a file of encrypted rows,
    since a per-record layout leaks how many credentials exist and which
    profile each belongs to through the file layout alone.
    **Chosen at runtime**, like the desktop pair and for the same reason: a
    phone whose Keystore refuses to generate a key — it happens, and it happens
    after an OS update — falls back to the passphrase file rather than silently
    failing to save anything.
    **`isHardwareBacked()` is asked of the key, not guessed from the device.**
    An emulator and a phone with a secure element both have an "Android
    Keystore", and only one of them means what a user would take that to mean,
    so the name reports which: "Android Keystore (hardware-backed)" or
    "(software-backed on this device)". Measured on an emulator, which
    correctly says software.
    **The second Java file in the project**, for the same reason as the first:
    a platform limitation, not a design choice. The Keystore's whole point is
    that the key never leaves it, so there is no handle to hand to OpenSSL and
    no C API that reaches it — the encryption has to happen on that side of the
    fence. It holds no logic worth testing: it wraps bytes and unwraps them.
    **No user authentication on the key**, and that is now a decision rather
    than a gap. §11's [OPEN] item asked what happens on a reconnect at 3am
    with nobody holding the phone, and the answer taken 2026-08-27 is that
    **it depends on the host, so it is a per-host flag defaulting to off** —
    right for the box running tmux and wrong for the production database, and
    they are not the same host. Requiring it on the Keystore key would still
    make that decision by accident, so the key stays unauthenticated and the
    gate sits where the choice can be expressed.
    One fact settled how much the strict reading would cost, and it was
    measured rather than assumed: **a key-authenticated reconnect never reads
    the store at all** — `stored_password_for()` returns before touching it
    unless `has_stored_password` is set — so no shape of this can stop a
    key-authenticated session reconnecting. Only a remembered password is
    ever gated.
    **The Android half is built, and the option is now reachable.**
    `BsshBiometric.java` raises `android.hardware.biometrics.BiometricPrompt`
    and answers through a registered native method — the first callback *into*
    native code in this project; every other JNI call here is one-way. The
    answer crosses the fence as an integer **token** rather than a pointer, so
    a prompt still on screen when the app tears down resolves to a token
    nobody is waiting for rather than to freed memory, and it is queued onto
    the Qt thread because it arrives on Android's UI thread.
    **API 29 and above, absent below it rather than approximated.**
    `BiometricPrompt` arrived in 28, but `BiometricManager` — the only way to
    ask whether anything is *enrolled* — came in 29. On 28 the available
    answers are "the device has a reader" and "somebody registered a finger",
    and only the second is the question: a prompt offered where nothing is
    enrolled can only be cancelled, and the user who turned the setting on has
    made their own password unreachable.
    **The control was the missing half, and it was missing in the direction
    this section keeps recording.** `require_biometric` was persisted by the
    store and read by the window, and *nothing anywhere wrote it* — the
    inverse of §10.1's badge: not a setting applied to nothing, but a setting
    nothing could apply. The host editor offers it now, and only where
    `has_biometric()`, which is §11's *absent, not faked* and the same rule
    that keeps the screenshot checkbox off the desktop page. A profile edited
    where the control is absent keeps its value rather than being quietly
    disarmed — a host synced from a phone with a reader, opened on a desktop
    without one, must not come back with the lock removed.
    **An APK carrying 6.9 MB of nothing, and no idea why — recorded because
    nothing in the pipeline would have noticed.** Three consecutive builds
    produced a 46.5 MB APK where a cold build gives 39.6 MB. The files are
    identical: same 97 entries, same compressed sizes, one 256-byte
    difference in this app's own library. The whole 7 MB is a **single dead
    gap** immediately before `libc++_shared.so`, measured by walking the
    entry offsets; every other gap in the archive is ordinary 4–16 KB
    alignment.
    **The cause is not established and three hypotheses are dead.** It is not
    the fonts (the entry lists are identical). It is not warm-versus-cold as
    such: after removing the build tree, a rebuild with no change and a
    rebuild that deliberately grew this app's library by 4 KB both stayed
    compact. What the bloated builds had in common is only that they ran
    against a build tree **inherited from an earlier session**, and removing
    that tree destroyed the state — so it cannot now be reproduced or
    diagnosed. Recorded as a symptom with its measurement rather than
    attributed, since the environmental story is the comfortable one and
    §12.3 already records what a comfortable explanation costs.
    **The artifact is kept, at the copyright holder's direction**, as
    `~/fixtures/beerssh-dead-space-46.5M.apk` — 46,499,926 bytes, sha256
    `08360537107f5c922582f5322047a068680766e94df704ed2776a7cfd31d6e5c`,
    carrying one 6,942,406-byte gap before `lib/arm64-v8a/libc++_shared.so`.
    Outside the repository deliberately: it is a 46 MB build artifact and
    belongs nowhere near git, and outside session scratch because that is
    documented as disposable.
    It is kept because **a check of this kind has to be shown to reject
    something, and this hole cannot be manufactured after the fact** — a
    hand-made gap is one somebody designed, and the point is to reject one
    nobody designed. The state that produced it is gone, so if the file goes
    it cannot be regenerated. It is a structurally valid APK that is merely
    bloated, not a corrupt one, which is what makes it the right fixture:
    `zipfile.testzip()` passes on it.
    **State the denominator when quoting the overhead**, since a threshold
    will be written against one: dividing by the FILE gives 0.23% healthy
    against 15.1% bloated, and dividing by the summed ENTRIES gives 0.23%
    against 17.8%. Two orders apart either way, so the threshold is
    undemanding — but the two numbers are not the same number and the first
    report of this quoted one while the record now carries both.

    **The generalisable half needs no diagnosis.** The Android packaging step
    verifies the ABI and the signature and does not verify that the artifact
    is *compact*, so megabytes of dead space ship silently: the size is
    printed and never compared to anything. That check belongs in the shared
    `tool/android.mk`, which four projects copy, so it is a cross-project
    signal rather than a change to make here — and it is the same argument
    the signature check itself was built on, that an artifact is verified
    rather than trusted because the build reported success.

    **PROVEN ON A DEVICE, 2026-08-30.** A Galaxy Note 9 running Android 10,
    API 29 — which is exactly the minimum this code targets, so it pins the
    lower bound rather than a comfortable middle. The host editor offers the
    checkbox, and that one fact exercises the whole chain: the native method
    registered against `se/vibes/beerssh/BsshBiometric`, the JNI call into
    `available()` returned, `BiometricManager.canAuthenticate()` answered
    `BIOMETRIC_SUCCESS`, and `platform_has_biometric()` therefore returned
    true. Any break in that chain shows as an absent control, which is the
    same thing a device with no reader shows — so the *presence* of the
    checkbox is the assertion.
    It also settled a question a `dumpsys` probe could not: whether anything
    is enrolled. Two probes for that returned empty and were recorded as
    inconclusive rather than negative; the app's own check answered it, which
    is the right instrument asking the right party.
    **Still unproven: the prompt itself.** Raising it needs a saved password
    on a real host, and the callback's trip back to the Qt thread is what
    would be exercised. The capability half is now evidence rather than
    intention; the release half is not.

    **What is proven and what is not.** The APK builds, the class is in the
    dex and `USE_BIOMETRIC` is in the packaged manifest — all three checked
    with `aapt2` and by searching *every* dex, after a first probe that read
    one of five and a manifest probe that could not read UTF-16 AXML at all.
    Both were caught by running a known-present control rather than by
    trusting a zero. **That the prompt appears, and that its callback reaches
    the Qt thread, is not verified**: it needs a device, and nothing off one
    can exercise it. Two properties are held
    by tests rather than intent: the gate is on the capability **and** the
    flag, so a profile carrying the flag to a machine that cannot ask falls
    through to the typed prompt instead of locking its own password away; and
    `authenticate()` on a platform that cannot ask calls back **false**, so a
    caller who forgets the check withholds a password rather than releasing
    one. An error here has a direction, and that is the one it must fall in.
    A refused or cancelled prompt takes the same road as no stored password
    at all: it asks for one to be typed. Biometric protects the STORED
    password, not the account, and somebody who knows it can always type it.
    **Verified on a device**, since none of this can be exercised off one: a
    password stored with "Remember" survives a full process restart and is
    submitted without prompting; the plaintext does not appear in the file; and
    a single flipped byte — what a store restored from another phone's backup
    looks like — is rejected by the GCM tag, so the app asks again instead of
    handing a caller garbage.
  - **Desktop [DECIDED, built]:** the platform secret service —
    Secret Service / `libsecret` (GNOME Keyring, KWallet) on Linux, Keychain on
    macOS, DPAPI/Credential Manager on Windows. **Fallback where no such service
    is running** (a headless or minimal session — common enough on Linux to plan
    for, not an edge case): a passphrase-derived key (Argon2id) unlocking a local
    encrypted store, prompted for at app start. **Both are built**, and the
    choice between them is made at **runtime, not at build time** — "is a
    keyring running" is a property of the session, and the same binary meets
    a desktop login and a headless one. libsecret is asked whether a service
    actually answers rather than assumed from its presence; the file store
    (PBKDF2-HMAC-SHA256 + AES-256-GCM, both OpenSSL's) is what a container,
    a minimal window manager or a broken session bus falls back to.
- **Be honest in the UI about which one is in use.** Keystore-backed and
  passphrase-backed are not equivalent security, and a desktop user with the
  fallback deserves to know their keys are only as safe as the passphrase and
  the machine. Surface it in settings; do not average the two into a reassuring
  "encrypted".
  **The password prompt is its own class** (`bssh_password_prompt`) for one
  reason: it was a dialog assembled inline, and it did not fit a phone. The
  whole string "Password for nabbe@10.0.2.2:2299:" sat in a form layout's
  *label* column, so the dialog's width grew with the length of the
  destination — 508px demanded on a 360px screen — and what ran off the
  right-hand edge was the checkbox and the buttons. A password prompt whose OK
  button is off-screen is not a cosmetic problem, and nothing could have caught
  it while there was nothing to construct. The destination now goes on its own
  wrapped line, and a test asks the prompt to fit 360dp with every button
  inside it.
  **Its heights were never looked at, only its width.** The field, the
  checkbox and both buttons stayed pointer-sized on the one dialog a user
  cannot avoid -- there is no reaching a password-authenticated host
  without it -- so a 20dp checkbox sat beside a password field, which is a
  target you miss into somewhere that matters. They carry sec 6.2's 48dp
  floor now, gated on the input device rather than the OS, the same
  question the key row and the front door ask. Found by rendering it:
  fixing a surface's width and calling the surface done is the shape of
  this one.
  **Built**, and it was the last piece: `backend_name()` and
  `is_hardware_backed()` existed from the start and nothing showed them. The one
  place the name appeared was the unlock prompt — which the keyring backend
  never raises — so the users who most needed to know were the ones who could
  not find out. "This device" now names the backend verbatim and says what it
  means, chosen from what the store REPORTS rather than from which class it is:
  `needs_passphrase()` is the honest dividing line, since a store the user
  unlocks by typing something is only as strong as that something. The test
  asserts the name appears unsoftened, and fails if it is replaced with
  "Encrypted".
- Support key formats: OpenSSH, PEM; passphrase-protected keys; optional
  **biometric unlock** — `BiometricPrompt` on Android, the platform equivalent
  where one exists on desktop, and simply absent (not faked) where it doesn't.
- **The `POST_NOTIFICATIONS` runtime request — [DECIDED]**, and it was missing.
  The manifest had declared it since the service was written, and on Android 13+
  a declaration is not a grant: the permission is denied until the user is asked,
  and nothing asked. The service still runs — Android does not kill it — so this
  failed in the quietest way available: the ongoing notification §5.2 says the
  user must be able to see simply never appeared, and Play checks for the flow.
  Asked at the moment the first session goes live rather than at launch, because
  a permission dialog before the user has done anything is one they dismiss on
  reflex. Done in C++ through the Activity's own methods — Qt has no typed
  permission for notifications — rather than by adding a second Java file, which
  §5.2 spends a paragraph avoiding. **Untested on a device:** the emulator here
  is API 30, where the permission does not exist; what was verified there is that
  the JNI plumbing makes both calls without throwing and that the version guard
  makes it a clean no-op.
- **[OPEN]** Desktop `ssh-agent` support (`SSH_AUTH_SOCK`). Not v1, but it is the
  single most-requested desktop feature for a client like this, and it fits the
  model cleanly: the agent is just another `bssh_secret_store` implementation
  that never yields the key material at all.
- **Host key verification**: known-hosts store with trust-on-first-use prompt and
  clear warning on key change (MITM protection). Do not silently accept.
  **Built.** An unknown or changed key **holds the connection open** rather than
  failing it — session up, nothing torn down, nothing written — until a person
  answers. Failing instead (which is what it did first) meant the only way to
  reach a new host was to hand-edit the store. The two prompts are deliberately
  not the same dialog: first contact asks, a *changed* key warns, says plainly
  that this is both what a rebuilt server and an interception attack look like,
  and defaults to Cancel so leaning on Enter trusts nothing.
- **The user's `~/.ssh/known_hosts` is read, and an accepted key is appended
  to it, hashed.** That is the settled answer; it arrived in two steps, and
  the bold summary used to stop after the first, which left a reader
  scanning this list with the superseded rule. The original rule
  refused both, on the grounds that two tools sharing one file is how a warning
  gets silently pre-accepted. Half of that was wrong. It is the same person
  deciding about the same hosts, and not looking at their file costs twice: a
  host they have already verified is put to them again — the prompt fatigue that
  teaches people to stop reading prompts — and, worse, **a changed host key reads
  as a first contact**, because with no previous key on file there is nothing to
  notice a change against. Refusing to look does not make this safer than
  OpenSSH; it makes it quieter about the one thing worth shouting about.
  So their file goes in libssh's *global* slot, which is consulted during
  verification and never written to. Its hashed-hostname support is real —
  checked against a file OpenSSH itself wrote.
  **Accepting writes there too — hashed, and appended.** One store, because the
  same person is deciding about the same hosts: a decision only half their tools
  know about is one they have to make twice, and a key that later *changes* is
  only noticed by whichever tool recorded the first one.
  The reason this was refused before was that **libssh writes entries unhashed**,
  and Debian and Ubuntu ship `HashKnownHosts yes` — 12 of 12 entries hashed on
  the machine this was written on — precisely so a leaked copy cannot be read as
  a list of the machines someone connects to. That is a fact about libssh, not a
  reason to keep two stores: the host field is hashed here instead
  (`|1|salt|HMAC-SHA1`, via the OpenSSL already linked), and the result is
  verified the only way that means anything — **`ssh(1)` itself accepts it with
  `StrictHostKeyChecking=yes`**, a mode that prompts for nothing and so can only
  succeed on a match.
  **Append only.** Somebody else's file is not this app's to rewrite, reorder or
  prune; it adds the one fact it was told, which is how OpenSSH writes too and
  what makes two writers tolerable. And it **falls back** to this app's own store
  whenever `~/.ssh` is out of reach — Android has none, a sandboxed desktop build
  may not be allowed near it — because a trust decision the user made must never
  be lost over where it could not be filed.
  **`@revoked` is honoured, and this app has to do it itself.** libssh reports a
  revoked key as *unknown*, which would put a friendly "trust this new host?"
  prompt in front of a key somebody went out of their way to revoke — and
  accepting would write it back as trusted. Reading OpenSSH's file while
  disagreeing about what a line *means* is worse than not reading it. The marker
  is matched against the server's own key, with the host field compared plain
  **and** hashed, since hashed is what these files actually contain and a
  revocation nobody can match is a revocation that does nothing. It is checked
  **before** libssh's verdict and outranks all of them including OK: a key can be
  recorded as trusted and later revoked, and the revocation is the newer
  statement. The UI is told, not asked — there is no accept button, unlike the
  changed-key dialog.
- **Which files are consulted, and in which slot.** The user's
  `~/.ssh/known_hosts` is the *primary* (`SSH_OPTIONS_KNOWNHOSTS`) — read and
  appended to — and `/etc/ssh/ssh_known_hosts` stays in libssh's *global* slot,
  which is what that slot means: how an administrator hands host keys to every
  account on a managed machine. An earlier version of this put the user's file in
  the global slot and thereby displaced the system one, so a host the admin had
  vouched for came back as unknown. One function decides the primary, because the
  file libssh is pointed at and the file the accept path writes to must agree —
  otherwise a host is recorded in one place, looked for in the other, and asked
  about again on the next connection.
- Other files in `~/.ssh` need no sharing decision: the private keys are already
  offered (`ssh_userauth_publickey_auto` tries the agent then the default
  identities), `authorized_keys` is the server's side of the question, and
  `known_hosts.old` is `ssh-keygen -R`'s backup rather than a source of truth.
  `~/.ssh/config` is the one genuinely open case, and it is §10.5's — imported
  rather than followed.
- **Preset export must be provably secret-free** — see §10.3. Export is more
  exposed on desktop, where the file lands in `~/Downloads` and gets attached to
  a chat window, so the §10.3 type-level guarantee earns its keep there.
- No secrets in logs; scrub on crash reporting.
  **Checked, and it holds by a wide margin**: the only logging anywhere in
  `src/` is two lines in `main.cpp` reporting how many font faces were found.
  There is no debug channel a credential could reach because there is
  effectively no debug channel. Worth re-checking whenever one is added, since
  the property currently rests on absence rather than on scrubbing.
- Consider an app-lock (PIN/biometric/passphrase) for opening the app, and an
  **idle auto-lock** — more useful on desktop, where an unattended unlocked
  workstation is the realistic threat.
- Hide terminal content from the OS screenshot/thumbnail path: `FLAG_SECURE` on
  Android (as a setting). No portable desktop equivalent exists — don't promise
  one in the UI.
  **Built, and the OS-level effect is UNTESTED ON A DEVICE** — the same
  honest half-state as the `POST_NOTIFICATIONS` flow above. `FLAG_SECURE` goes
  on the Activity's window through JNI, off by default because it restricts the
  user as well (they cannot screenshot their own terminal either), stored in
  `app.ini` under `security/`. What is verified here is the plumbing: the
  setting persists, and the window applies it on every settings change. What
  cannot be verified without a device is the thing that matters — whether the
  recents thumbnail actually stops showing somebody's shell.
  **The capability is asked, not assumed, and that is the load-bearing part.**
  This section says there is no portable desktop equivalent and not to promise
  one in the UI, so `can_hide_from_screenshots()` answers *no* on desktop and
  the checkbox is **absent from the page entirely** rather than present and
  ineffective. A security control that reports success while doing nothing is
  worse than a missing one: it is the difference between a user knowing their
  terminal is in the screenshot and believing it is not. A test asserts the
  page has no such control on desktop, because "not offered" is a property of
  the assembled page and nothing else can see it.

---

## 12. Build, packaging, CI

### 12.1 Builds
- **[BUILT] A missing submodule is fetched, not complained about**, and the
  fetch happens while the Makefile is being READ. `VTERM_SRCS` is a wildcard and
  a wildcard expands at parse time, so a checkout done from a recipe comes too
  late for the run that needed it — the source list is still empty and make goes
  on to build an archive with no members. An ordinary build pays nothing: the
  test for a missing submodule is itself a `$(wildcard)` and spawns no process.
  Targets that cannot need libvterm are excluded, because a style check has no
  business reaching the network.
  What it replaced could not work. The guard hung off `$(VTERM_SRCS)`, so with
  the submodule absent that list was empty, the rule had no targets and never
  fired; and with it present the list held only files that already existed, so
  it could not fire then either. The same empty list also stripped the archive
  rule's prerequisites, and a rule with none fires whenever its target is
  missing — hence `ar: build-vterm/libvterm.a: No such file or directory`, which
  names neither the submodule nor the fix.
- **The fonts are a prerequisite of the PACKAGE though not of `make install`.**
  Install is deliberately tolerant of a checkout that never ran `make fonts`
  (§9.4 falls back to system families); `debian/beerssh-fonts.install` names
  `usr/share/beerssh` outright and is not tolerant at all. Without them the
  build does not produce fewer packages, it produces none — and it fails at
  `dh_install`, after the whole app has compiled.
- **One qmake project (`beerssh.pro`), two scopes.** The desktop build is a plain
  host `qmake6` against the system Qt, libssh and OpenSSL. The Android build runs
  the Qt-for-Android kit's qmake and hands off to `androiddeployqt`. No second
  project file, no CMakeLists kept alive alongside it — **two build systems drift,
  and nothing notices until one of them is broken.**
- **Everything that isn't the Qt app is a hand-written Makefile**, and the
  top-level `Makefile` is the single entry point that wraps qmake for the app.
  qmake earns its place only because moc/uic/rcc don't fit a pattern rule; that
  is not a reason to let it own the whole tree.
- Gradle wraps the Android build into an **AAB** via `androiddeployqt`
  (`make android`).
- **Play Asset Delivery** packs configured for the optional font families (§9.4);
  the desktop package just installs them.
- Keep the platform-conditional surface in the build files as small as it is in
  the code: one `android { ... } else { ... }` scope selecting
  `platform_android.cpp` vs `platform_desktop.cpp` and the dependency source, not
  conditionals sprinkled through the file. A second scope appearing elsewhere in
  `beerssh.pro` is the same smell as an `#ifdef` in the sources.

### 12.2 Desktop packaging — **[DECIDED]** tarball, `make install`, and a
**`.deb`**; Flatpak and the rest deferred
All of them want the app ID from §0.1.

**Neither of these checks runs in CI, and CI stopped working on 2026-08-14.**
Measured over the last 100 runs with `gh`: **19 successes and 81 failures**,
the most recent green being `2026-08-14T02:52`. Many of the failures are
runs that never started — annotated *"The job was not started because recent
account payments have failed or your spending limit needs to be increased"* —
and it splits on repository visibility rather than on anything in the code,
so it is an account-level billing state that no tree can fix.
**The date is the fact that matters, not the colour.** Everything merged here
since 2026-08-14 has been verified by the local gates and by nothing else.
**In THIS repository's window the two populations do not overlap**, which is
narrower than the rule it is tempting to write. No success completed in under
**326 s** (median 372); **57 of the 81 failures finished in under 10 s**
(median 5) — a 300-second gap with nothing in it. That makes duration a
usable *triage* here: the remaining ~24 failures, the ones that ran long
enough to have done something, are the only rows in this history carrying
information about the code.
**It is not a general tell, and the sibling that disproved it is the reason
this paragraph is worded this way.** `netcfgd`'s runs overlap completely —
blocked at 1m36s, executing at 40s — same tool, same account, same billing
block, opposite structure. So the useful question was never whether duration
separates the two; it is that **you cannot learn whether it separates them
for a given repository without doing the full listing the proxy was meant to
save you.** The saving is on the second read, never the first.
This entry claimed the general form before it claimed the narrow one. That is
the third instance in three days of a measurement's scope widening past what
was measured, and the second in this paragraph.
**This entry said "every run" before it said 19.** That claim came from
reading three rows and generalising to a hundred — the scope error
`evidence.md` records, committed here by the same hand that had just
discussed it, in a paragraph whose own subject was a list view hiding a
distinction. Corrected from the other end by a sibling checking its own
repository rather than accepting a relay.
**The mechanism is worth more than the instance**, and two sessions hitting
it on one fact within a day is what shows it is structural: `gh run list`
defaults to a short page, the rows look uniform, and *"recent"* slides into
*"all"* with no moment at which anybody decides it should. The fix is
`-L 100` and counting, not care.
(The under-10s count differs by one between two independent measurements of
the same window — 57 here, 58 there. A boundary case at the threshold; it
moves nothing and is left unresolved rather than tidied into agreement.)
**`make check-deb` passes as of `3a6ffce`**, run locally because that is the
only place it can run: three packages into a clean `debian:trixie`, the app
starting and loading 22 font faces, the manual page installed, `--help`
working with no display, the desktop entry validating, the icon it names
present, and a clean uninstall. Worth knowing because it is exactly the step
two sibling projects fail at — build and lint pass there and *installing the
built package* does not — so this is the one to re-run first when CI
unblocks, and a first green run will not be the same as a first correct one.

**Both artifacts are checked by installing them, not by building them.**
`make check-deb` puts the packages into a clean `debian:trixie` container, lets
apt resolve the `Depends` the binary itself generated, runs the app, validates
the desktop entry, confirms the icon it names is installed, and removes it
again. `make check-apk` installs the APK on an attached device, launches it,
and asserts the process is *still* alive a moment later, that a window belongs
to it, and that it found its bundled fonts. A package that builds says nothing:
the failure that matters is one that installs on the developer's own machine —
where every library is present for other reasons — and dies at first launch on
anybody else's. Both of the Android bugs this project has had (the
networkinformation plugin shipped without its JAR, the foreground service
breaking its lifecycle contract) were exactly that shape.

**`make android` names the artifact and says who signed it.** Gradle leaves it at
`android-build/build/outputs/apk/debug/android-build-debug.apk`, which tells
nobody which app, version or ABI they are holding; it comes out as
`beerssh-<version>-<abi>.apk`. Debug-signed by default — installs anywhere,
which is what an evaluation build wants — and **said out loud**, because a
debug-signed APK mistaken for a release is a bad afternoon: Play rejects it and
it can never replace a properly signed install. `ANDROID_KEYSTORE` and
`ANDROID_KEY_ALIAS` produce a real signed release.
That signing had to be *verified* rather than announced. The first version
passed the flags to Qt's generated Makefile as a variable it does not have, so
they were dropped in silence and the target printed "release build, signed
with…" over a debug-signed APK — the one packaging mistake that looks
identical to success. The flags now go to `androiddeployqt` directly, and the
target reads the certificate back with `apksigner` and fails if a keystore was
given but the Android debug key came out.

**And the `<abi>` in that name is read back too, for the same reason.** It came
from a Makefile variable defaulting to `arm64-v8a` on its own, while
`beerssh.pro` took the real ABI from the Qt kit — so building against the
x86_64 kit produced `beerssh-0.1.0-arm64-v8a.apk` containing `lib/x86_64`, an
artifact that installs on nothing it claims to target and whose name is the
only thing anyone will read. Two places computing one thing. The Makefile now
reads the ABI from the kit's own `qdevice.pri`, which is where qmake gets it;
an explicit `ANDROID_ABI` that contradicts the kit is refused rather than
built; and the finished package is unzipped and its `lib/` compared against
the name it was given.
**With the kit's directory name as a fallback**, because making an unreadable
`qdevice.pri` a hard error would have broken a build that used to work: CI
installs its Qt with an action rather than from this machine, and a layout that
differed there would have stopped the Android job dead — a regression
introduced *by the fix*, in the job most likely to notice it last. It is a
fallback and not a second opinion: consulted only when the authoritative file
says nothing, with the contradiction check still applied to whatever comes out.
A path that is not a kit at all still refuses, and names both sources it tried.

**Built:** `make install` now lays down what a Linux desktop actually needs —
the binary, `se.vibes.beerssh.desktop`, the AppStream `.metainfo.xml`, a
hicolor scalable icon, and the bundled fonts with their licence files under
`$PREFIX/share/beerssh/fonts`. That last one was not optional: the app looks
for its fonts there, so before this an installed copy silently fell back to
system families and the licences screen had nothing to show — the binary alone
was never an installation. `make uninstall` removes exactly what it put there.
Distro packaging and any self-contained format both build on this rather than
replacing it, which is why it came first.

**`make deb` builds two packages**, and the split is the point: `beerssh` is
251 KB and `beerssh-fonts` is 9.7 MB, because a terminal client that costs
54 MB to install is a terminal client people do not install. The fonts are
genuinely optional — without them the app falls back to system monospace
families (§9.4) — so they are `Recommends`, arch-independent, and packaged the
way Debian ships fonts anyway. Dependencies come from `dpkg-shlibdeps` reading
the binary rather than from a hand-kept list that drifts, and an empty result
is a **fatal** build error: a package with no `Depends` installs happily onto a
machine with no Qt and fails at first launch rather than at install time.

Flatpak, `.dmg` and Windows installers are **deferred**. Nothing about the
above blocks them later.

**The icon is a placeholder.** §0.1 leaves the branding direction open, and
that is a decision to make rather than to default into; a `.desktop` entry
without an icon is defective, so there is a plain one meant to be replaced.
- **Linux:** an upstream tarball, `make install`, and `make deb`. A
  self-contained format (Flatpak would be the best fit, and would also solve
  the Qt-version-skew problem across distros) is deferred rather than dropped.
  Ship a `.desktop` file, an AppStream `.metainfo.xml`, and hicolor icons —
  without them the app is invisible to software centres.
- **macOS/Windows:** `.dmg` / installer, best-effort per §1, unsigned until
  someone actually wants them (code signing on both is a cost, not a checkbox).

### 12.3 Testing
The desktop build is what makes this project testable at all — use it.
- **Unit tests run on the host**, no device and no server: the emulator fed
  captured byte streams (via a fake `bssh_ssh_session`, §4), the input router fed
  synthetic key events (§6.3), the layout manager fed window widths (§8.4). These
  are the three components most likely to regress and all three are pure logic.
- **Golden-file emulator tests**: byte stream in, expected screen grid out.
  Cheap to write, and the only practical defence against a parser fix breaking
  three other sequences.
- **One check runs against a real server** (`make check-ssh`, §12.4's sibling).
  The fake is what makes the suite runnable with no server and no device, and it
  leaves exactly one layer unexercised: the libssh binding. A fake proves the
  emulator handles bytes; it cannot prove an option reached libssh, or that
  libssh did what the option's name suggests. That gap was not theoretical —
  `IdentityFile` was wired to `SSH_OPTIONS_ADD_IDENTITY`, shipped, and given a
  widget on both front doors before anything checked that a named key
  authenticates.
  It proves both halves: with the key it connects **and the shell answers**, and
  *without* it the server refuses — the negative is what stops the test passing
  against a server that would let anyone in. The tilde form is checked against
  the server too rather than by comparing strings, since carrying a `~` across
  from an OpenSSH config is the field's whole purpose (§10.5).
  It also covers **§11's trust-on-first-use against a server whose key can
  really change** — the failure §11 names is a warning that ends up silently
  pre-accepted, and a fake can be *told* to report a changed key while proving
  nothing about whether the binding notices. An unknown host is announced and the
  connection **held**, not failed and emphatically not accepted; accepting once
  is remembered so the second attempt asks nothing; and the accepted key lands in
  BeerSSH's own store with `~/.ssh/known_hosts` left untouched. Then the server is
  restarted **on the same port with a new host key** — a different port would be a
  different known-hosts entry and merely unknown — and the changed key is reported
  as *changed* and refused. Sabotaging the `SSH_KNOWN_HOSTS_CHANGED` branch fails
  exactly that test.
  The run is two phases for that reason, and each trust test clears the store
  itself rather than relying on running before the authentication tests, which
  accept the key as a side effect: a test that passes only in one order fails
  later for a reason nobody can see.
  Everything is thrown away: its own host key, user key, `HOME`, `XDG_DATA_HOME`
  and an sshd on a spare loopback port taking that one key and no password. Nobody's real
  `~/.ssh` is read or written. Without the server the tests **skip** rather than
  fail, so running the binary by hand cannot report a false failure.
- **Breaking the code on purpose is automated now — `tool/sabotage.py`.**
  This section has asked since it was written for every test to be checked by
  making it fail, and by hand that is three commands and an editor window per
  assertion, which is why it got skipped exactly when there was least time. A
  spec (`tool/sabotage/*.toml`) names a file, a string to break, and **the
  test that must notice**; each edit gets its own suite run and a verdict of
  `caught`, `MISSED`, `WRONG TEST` or `NO BUILD`. `NO BUILD` is a verdict
  rather than a pass because a suite that did not compile prints no PASS and
  no FAIL lines, which counts identically to green if you only look at
  failures — the vacuous pass this project keeps finding, in the tool built
  to find it.
  **Its first run found one, and it was §8.4's.**
  `a_window_shows_only_its_own_tabs` could not fail: the tab bar's count
  comes from the shown list and only the *label* came from the sabotaged
  lookup, so asserting counts tested nothing — and one window holding one
  session cannot tell the shown list from the registry anyway, because both
  hold the same thing in the same order. Two windows each holding a
  different session is the smallest case that can, and the test builds it
  now.
  **Three defects in the tool itself, and the worst of them is a build
  rule.** `shutil.copy2` preserves the mtime, so a restored file lands
  *behind* the object compiled from the sabotaged text; make judges that
  object current, and the next `make test` runs the sabotage against a tree
  whose `git status` is clean. That is §12.1's "never conclude a test passes
  from a binary the build step did not rebuild", reached through a timestamp
  rather than a missing dependency, and it cost an hour of hunting a
  test-contamination bug that did not exist. The others: the deadline killed
  `make` and left the test binaries it spawned reparented to init — measured,
  PPID 1 — which is the orphan that fills a disk invisibly, so the suite runs
  in its own session and is killed by group; and a spec had gone stale and
  matched nothing, which aborts the batch, so every `find` string is checked
  to match exactly once before a run.
  **That last check was a claim rather than a check until 2026-08-14.** The
  runner tested only that the string was *present* and then replaced the
  first of however many matched, so a spec matching twice broke a line it
  did not mean and every verdict after it described an edit nobody had asked
  for — a report on a test that was never put under pressure, delivered as
  confidently as a real one. It is the runner's own job now, over every edit
  before the baseline rather than at the moment each is applied: a stale
  string otherwise surfaces at edit five, five suite runs in.
- **A check can be vacuous in more than one way, and this one managed two in a
  day.** §6.2's touch floor is enforced by a walk over a list of widget TYPES,
  and the test that guards it went wrong twice.
  First it kept its own copy of that list, which drifted: the copy omitted
  `QAbstractSpinBox`, so a spin box was sized by the walk and checked by nobody.
  Replacing the copy with a call to the same predicate looked like the obvious
  fix and was worse — it made the check **unfalsifiable**, because deleting a
  type then removes the requirement and the check in one edit, which is the one
  failure it exists to report. The sabotage runner said so: dropping sliders
  from the list was reported as caught by nothing.
  Asking "does it take keyboard focus" instead is wrong in **both** directions,
  and is worth writing down because it is such a tidy-looking idea. A `QLabel`
  with selectable text takes focus and is read rather than tapped, so the
  settings page failed on a paragraph; and the key row's buttons take **no**
  focus deliberately — tapping a key must not steal focus from the terminal —
  so the app's most important tap targets were skipped entirely.
  What it does now: the test keeps its own list of interactive types for the
  **requirement**, uses the code's list only for the nesting **exemption** —
  where an incomplete list produces noise rather than silence — and asserts that
  it examined something at all. The two uses are not symmetric, and that is the
  general lesson: a list is safe where being incomplete is loud.
- **Rendering the viewport shows a third of the truth**, on three surfaces so
  far. The settings page hid the brightness slider below the fold, the host
  editor hid the font picker, and the key row hid five keys in thirteen — where
  `minimal` and `default` rendered *identically* at phone width while being
  eight keys and thirteen. All three shoot the scroll area's CONTENT now, at its
  natural size, which is the only view that contains what is being judged.
- **The shots directory kept pictures of screens that had been removed** — an
  SSH category, a *Window* page renamed to *Appearance*, two deleted Solarized
  terminals — so anyone browsing it would have concluded all three still
  existed. That is this section's own rule turned against it. Last run's output
  goes before each run, with the directory checked non-empty, relative and free
  of parent escapes, because the file names are not knowable when the set of
  surfaces is the thing that changes.
- **Panes, the key row and the menus have pictures now**, and each was a surface
  where an assertion counts things a picture judges. Two panes side by side with
  the focused border between them; all five key-row layouts at full width; and
  the terminal menu in both arrangements, which is how §6.4's claim that touch
  changes *the metrics and nothing else* became checkable by a person rather
  than by a row count.
  The split shot found something on its way in: whether a drop tiles or falls
  back to a tab is decided by the size class, the size class arrives in a
  resizeEvent, and Qt does not deliver one to a hidden widget — so a window
  resized before being shown tiles nothing. That is a property of the code and
  not only of the test.
- **`make check-keepalive` is the one check that needs the network to lie.**
  Everything else can be proved with a server that answers or refuses; §5.3's
  socket options exist for the case where packets simply stop arriving and
  nothing says so, and simulating that needs them dropped rather than refused —
  `iptables`, `NET_ADMIN`, and therefore a container. It builds inside the image
  rather than mounting the host's binary, so it does not quietly depend on the
  developer's Qt matching the image's. Slow by construction: the budget it
  verifies is a minute, so the run is over one.
- Device/emulator tests cover what only Android can break: IME and hardware-key
  delivery (§6.1), the foreground service surviving screen-off (§5.1), activity
  recreation on rotation.
- **A crashing test does not fork a debugger**, and that is an incident rather
  than a preference. QtTest's crash handler runs
  `gdb --batch -ex "thread apply all bt" --pid <self>` on a fatal signal. In a
  tree whose method is to break the code on purpose and watch a test fail,
  crashes are routine — and each one left gdb attached with the binary stopped
  in state `t`, neither ever exiting. Because **a tracer becomes the reaping
  parent**, the dead test then showed as a zombie nothing could clear, and a
  stopped process ignores `SIGTERM`, so the obvious `pkill` cleaned up nothing.
  Five accumulated in a single session holding **15 GB of RSS between them** —
  on a machine whose load and swap pressure were meanwhile being blamed on
  another build, including by `make bench`'s own too-loaded guard. The
  measurements that guard was protecting were being distorted by the test
  runner that produced them.
  `make test` sets `QTEST_DISABLE_STACK_DUMP` and `QTEST_DISABLE_CORE_DUMP`.
  The signal and the test that raised it are still reported, which is enough to
  know where to look; `make test BSSH_TEST_STACK_DUMP=1` asks for the backtrace
  when a crash is worth one.
- **A runaway test is bounded by the suite itself, not by whoever ran it**, and
  that is an incident too. Three copies of the test binary were found orphaned
  to init in state `R`, seven to ten hours old, with stdout and stderr
  redirected into log files that had since been deleted. An unlinked file that
  is still open belongs to no directory, so `du` could see none of it and
  deleting the logs freed nothing — the space returns only when the last
  descriptor closes. Between them they held **693 GB** and took a 905 GB
  filesystem to 100% full, at which point the suite began reporting
  `write_ini() returned FALSE` and settings that would not persist: a full disk
  wearing the costume of a regression, which is the same disguise the `TMPDIR`
  floor exists to strip off.
  **None of the three was started by `make test`** — they were run directly, so
  a `timeout` wrapped around the Makefile recipe would have caught none of them.
  The bound lives in `test/main.cpp` for exactly that reason: a wrapper only
  guards the way somebody did not run it.
  Two limits, because the failure has two shapes. `BSSH_TEST_TIMEOUT` (seconds
  per test class, default 300) catches a test that spins in silence;
  `BSSH_TEST_MAX_OUTPUT` (MB, default 256) catches one that spins printing, and
  catches it far sooner — 530 GB of that 693 was a single process writing about
  15 MB/s for nine hours. The cap reads the *size* of stdout and stderr rather
  than counting what is written, so it costs nothing per line and still sees a
  test that writes to the descriptor behind libc's back; it applies only when
  they are regular files, since a terminal has no size to blow. Either limit is
  off with `0`, for running under a debugger where a process sitting still is
  the point.
  A tripped watchdog exits **124**, the code `timeout(1)` uses, and **names the
  class it died in** — which is precisely what nobody could recover from 693 GB
  of deleted log. It calls `_exit` rather than `abort`, because `abort` raises
  `SIGABRT` straight into the gdb-forking handler disabled above; a watchdog
  that reproduced one incident while preventing the other would be no bargain.
- **Benchmarks are built by `make test` and run by `make bench`.** Every
  throughput figure in §9.5 was measured with a scratch harness that was then
  thrown away, so each one cited a program nobody could re-run — a claim
  wearing the clothes of evidence. `test/emulator_bench.cpp` is that harness,
  kept. It is **not a CI gate and must not become one**: the numbers move by
  more than a real change does depending on what else the machine is compiling,
  so a threshold would either sit so loose it catches nothing or fail builds
  for reasons that are not the build's fault. Compare a run against another run
  of the same binary.
  **And it refuses to measure a machine that cannot answer**, which "load
  average is part of the result" printed above the numbers did not achieve. The
  target reported a full-screen repaint at 35 ms against §9.5's 2.3 ms —
  fifteen times worse, and nothing in the output separated a real regression
  from a box at load 81 on twelve cores with 23 GB in swap, which is what it
  was. A printed number outlives the run that produced it, so above half the
  core count `make bench` stops rather than answering wrongly. `BSSH_BENCH_ANYWAY=1`
  overrides it, and is the right thing for an interleaved A/B where both sides
  see the same contention and only the ratio is claimed.
  Each case also asserts on the resulting screen, because a
  benchmark that only measures cannot tell "faster" from "stopped doing the
  work". The measurement that *is* machine-independent is a **count**, and that
  is what guards the throughput in CI:
  `emulator_test::damage_is_reported_per_row_not_per_cell` asserts on repaint
  requests per line of output (§9.5) rather than on a clock, so it reads the
  same on a fast desktop, a loaded runner and a phone and cannot flake. The
  ceiling is set between the two measured figures rather than on either, because
  pinning an exact count would turn a libvterm update into a failing test for no
  reason.

- **Qt 6.12's Android accessibility bridge aborts beerssh on this phone,
  and the condition is NOT isolated.** Stated that way deliberately: the
  trigger has been mis-stated twice already, and a wrong one here sends the
  next reader to test the wrong thing.
  What reproduces, on demand: about nine cycles of `am start` to another
  app, 0.6s, `am start` back, 0.6s, opening the overflow menu every other
  cycle. That run gave two tombstones six seconds apart, the second on the
  process that had just restarted.
  Every abort is identical -- `signal 6 (SIGABRT)`, Android's JNI checker
  reporting `JNI DETECTED ERROR IN APPLICATION: JNI CallVoidMethodV called
  with pending exception`, through `QtNativeAccessibility.populateNode`
  under `QtAccessibilityDelegate.notifyValueChanged`. Measured 2026-08-26 on
  SM-N960F, Android 10, arm64-v8a, Qt 6.12.0, debug-signed.
  **Four hypotheses, each falsified with five clean trials**: the first
  launch after an install; backgrounding via KEYCODE_HOME; opening the
  overflow menu and dismissing it with BACK; and rapid text input alone
  (three commands per launch, through `adb input text`).
  **And one occurrence the reproducing procedure does not explain.** The
  fourth crash happened while typing into the terminal with no app
  switching at all -- so focus churn is not necessary; and since typing on
  its own does not reproduce, it is not sufficient either. Both
  characterisations tried so far are approximations of something not yet
  named. Four crashes happened by accident, all while working quickly; the
  fifteen scripted trials that isolated one step at a time, with generous
  sleeps, produced nothing. Whatever it is, it is lost by slowing down.
  The delegate is live because `com.jamworks.bxactions` is enabled -- a
  Bixby-button remapper, not a screen reader -- so this is an ordinary
  configuration and not an assistive-technology corner.
  **hydra aborts in the same subsystem on the same phone, deterministically
  on every menu open**, and the pair is worth more than either half. Its
  trace runs QRhi -> QOpenGL -> `QAndroidPlatformOpenGLWindow::eglSurface()`
  and dies on a deadlock protector; ours has no OpenGL in it at all, this
  front door being plain widgets on a raster surface. One lock-ordering
  fault reaching two victims by two routes, or two bugs in one bridge --
  nothing measured distinguishes those, and neither project asserts one.
  No frame of ours appears in any trace. That is not the same as the fault
  not being ours: the bridge is asking sec 6.4's accessible interface for
  values, and a pending exception could start there.
  **The obvious mitigation does not work, and that is a relief rather than
  a loss.** hydra built with `QAccessible::setActive(false)` called before
  any widget, confirmed by its own log that `isActive()` was 0, and ran its
  deterministic procedure three times: aborted all three, identical message.
  Qt's Java `QtAccessibilityDelegate` is installed by the Activity whatever
  the C++ flag says, so `runInObjectContext()` still runs and still takes
  the lock. So there is no application-side lever here -- and no trade to
  put to anybody either, since the option that looked like it cost screen
  reader support turns out to buy nothing.
  Untestable on the host. Offscreen Qt never instantiates the Android
  accessibility delegate, so sec 6.4's accessibility tests exercise our side
  of that interface and nothing that calls into it. They pass, and their
  passing says nothing about this.

- **The stray `<` at the last column of a wrapped row: solved, and it was
  never ours.** It is **mksh's own horizontal-scroll marker**. The device's
  shell pads the line out to the last column and writes a literal `<` there
  to say the line extends past what is shown; beerssh draws exactly what it
  was sent. Nothing in the emulator, the mirror or the renderer was at
  fault, and the two tests that went in hunting it (76538ec, 43f0e2c) were
  measuring a terminal that was already behaving.
  **The proof is the byte stream, taken from the device's own shell.**
  `adb shell` allocates a pty when its own stdin is one, so the shell was
  driven under a local pty at `stty rows 46 cols 58` and its output
  captured. Typing nothing but printable characters -- `abcdefghij` seven
  times, no escape sequence anywhere in the input -- comes back as:

      abcdefghijabcdefghijabcdefghijabcdefghijabc
      \r fghij...abc          <  \b x19

  a carriage return, the visible window of the line, padding spaces, the
  `<` in the last cell, then nineteen backspaces to put the cursor back.
  That is the whole phenomenon, and it needs no mouse report, no wrap bug
  and no repaint theory to produce it.
  **The road there is worth keeping, because most of it was wrong.** Four
  hypotheses were eliminated by measurement before the right question was
  asked, and each cost something:
  - *The emulator's wrapping and scrolling* --
    `emulator_test::a_line_longer_than_the_grid_wraps_without_debris` and
    `wrapped_lines_scrolling_off_leave_no_debris`, which pass.
  - *A redraw of a full row.*
    `emulator_test::a_wrapped_line_redrawn_shorter_leaves_no_debris` fills a
    row exactly to its last column -- where autowrap is left PENDING rather
    than the cursor moving -- and redraws it shorter four ways. All four
    clear the last column.
  - *The device's libvterm being unpatched.* Proved by byte identity: the
    shipped `build-vterm-android/arm64-v8a/state.o` is byte-for-byte
    identical to a fresh build from the patched sources and 424 bytes
    different from one built from blobs checked equal to
    `git show HEAD:src/state.c`. **The grep trap next to it nearly became a
    fact in this document:** `0xfc` matches branch-target symbol offsets
    like `<on_csi+0xfc>` in an *unpatched* object, so grepping for the
    constant is vacuous; as an immediate operand it appears zero times
    there and once in the patched build. Neither constant alone
    discriminates -- `#0xfe` is an immediate in both -- what does is the
    (mask, compare) pair at the instruction that loads `leader[0]`.
  - *A width disagreement between the terminal and the shell.* The obvious
    one, and false: `stty size` inside the session answers **46 58** and the
    grid measured off the prompt's own glyph pitch is 58. A speculative fix
    for that non-defect was written, could not be pinned by any test that
    isolated it, and was reverted rather than kept.
  **And the renderer was exonerated by measurement rather than by reading**,
  which was worth having independently of the answer: with the character on
  screen, typing one more key makes the shell echo, which fires `damaged()`
  and repaints the whole grid from the mirror -- and the character survives.
  So it was content, not a repaint artifact. Re-deriving the paint path by
  inspection had been listed as one of the three things that might be wrong;
  it was not.
- **A reset reaches the state we keep beside libvterm's now — `vterm-patch/
  0005`, and it is the fourth patch that vendoring paid for.** The mechanism
  and the design are in §7.1; what belongs here is what it cost to find and
  how it is held down.
  It was found by an audit asking, of every path that writes to the remote,
  what gates it — the same question that had just turned up the reconnect
  carry-over. The answer for the kitty flags and `modifyOtherKeys` was
  "nothing clears these, ever", which is a worse answer than the one the
  audit was looking for.
  **The proof the patch is right is a rebuild from pristine**, not a reading
  of the diff: the submodule at its gitlink commit, all five patches applied
  in order, compared byte-for-byte against the working tree. Run again after
  the sabotage pass, which edits the vendored source on purpose, and it still
  reproduced — which is what says the runner's journal really did hand the
  submodule back.
  **Two of the four sabotage edits are in the vendored source**, deliberately:
  a spec that broke only our side would report the consumer covered and leave
  the thing it consumes unguarded. `tool/sabotage/vterm-reset.toml`.
  And one test-authoring trap worth keeping, because it produced a plausible
  green: `"\x1bc"` is **one** character, not `ESC` then `c`, since a hex
  escape swallows every hex digit that follows it and `c` is one. It compiles
  with a warning nobody reads and feeds a stray `0xBC`. The octal form
  `"\033c"` takes three digits at most and is what the test uses.

- **The accessibility interface converted between three spaces and got each
  one wrong — fixed, and the fix is that it no longer converts.**
  `characterRect()` used the bare `BSSH_GRID_INSET_PX` rather than
  `grid_origin_x()` — the cursor bug over again, in a second copy, and the
  copy existed because `grid_origin_x()` was private and the file could not
  ask. It ignored the scroll offset, so every rectangle was wrong by that
  many rows while scrolled back and history rows plainly on screen reported
  nothing. And it treated a character index as a cell column, so a line with
  wide glyphs walked one cell further left per preceding glyph.
  `offsetAtPoint()` had all three inverted; `cursorPosition()` and
  `selection()` reported cell columns as character offsets.
  **`cell_at()` and a new `cell_rect()` are public on the view now**, and the
  interface asks rather than computing: one implementation, or two answers
  that drift. The character-versus-cell half cannot be delegated, since only
  this interface knows what it dropped, so `flat_t` records the cell column
  of each character — **per code unit, and only for rows that contain a wide
  glyph**, an empty list meaning "column equals index", so ordinary text
  costs nothing.
  **The round-trip test found its own blind spot.** Asking that the middle of
  a character's rectangle map back to that character is the only form that
  cannot pass by agreeing with a second copy of the arithmetic — but the
  first version probed **column 0**, and `cell_at` clamps a negative column
  to zero, so a rectangle shifted left by an entire scrollbar still mapped
  back correctly. The sabotage runner reported it MISSED. It probes five
  columns in now, with a left-hand scrollbar, which is the configuration
  nobody looks at and the one where the error is a whole bar wide rather
  than a pixel.
- **"Absolute" lines stopped being absolute when the scrollback evicted —
  fixed by giving the ring a base.** `line_for_row()` counted from the start
  of the current buffer, and `push()` drops the front at the cap, so once the
  buffer was full every new line renumbered every old one while the stored
  selection anchor did not move. With a build streaming into a full buffer, a
  selection made in the history walked down the content one line per line of
  output, and Copy yielded text the user never highlighted.
  **It was invisible for the reason that matters here: the painting agreed
  with the anchor.** The highlight moved *with* the error, so the screen
  looked right and only the copied text disagreed — which is why the test
  asserts on `selection_text()` and not on a coordinate, and why a screenshot
  could never have caught it.
  `bssh_scrollback::base()` is the count of lines the ring has dropped, so
  `base() + index` names a line that does not change. It advances wherever
  the front is dropped — `push()` at the cap, `set_max_lines()` when
  shrinking, and `clear()` by the whole count, because a cleared line is gone
  rather than renumbered and anything still holding its name must fail to
  find it rather than find a different line under it. `pop()` takes from the
  back and leaves the base alone.
  Everything that crosses between a buffer index and a line's name goes
  through it now, including the accessibility interface, whose flat rows had
  coincided with absolute lines only because the base had always been zero.

  **What the hunt did turn up is a real defect, and it is not this one.**
  Reproducing the long line the first time used mouse reports: with a mode
  left on by a program that exited without disabling it, every tap writes
  `CSI < 0 ; c ; r M` onto the shell's command line. That is not what draws
  the `<` -- plain typing does -- but it is beerssh writing bytes to a
  remote that never asked, and on a phone a tap is how a pane gets focus.
  **sec 6.4 says "on touch, keep local selection winning" and the pointer
  path never asks `is_touch_primary()`.** The document and the code
  contradict each other; flagged, not resolved.
### 12.4 CI
- **The receipt binds HEAD-at-archive; the hook compares HEAD-at-push, and
  between them sits a thirteen-minute window.** `check-ci` resolves HEAD
  once, beside the `git archive` that consumes it — 5e9b540's fix, and right
  — and the pre-push hook refuses anything that is not the commit the receipt
  names. Both halves are correct and the failure is in neither: a commit
  landing from another session while the container builds makes the receipt
  stale, and the push is refused for a tree that would have passed.
  Measured 2026-08-26/27, alongside a spread pass working in the same trees:
  **six container runs, well over an hour, spent on receipts that went stale
  between the archive and the push.** Every refusal was the gate working.
  **A message protocol was tried first and it does not hold.** "Ask the other
  session for a hold before the run, and release it afterwards" cost one round
  trip and worked several times — and then the hold was broken **by the
  session enforcing it**, in good faith, running a four-tree spread from a
  different project while beerssh was mid-gate. That is the decisive case
  rather than an unlucky one: **the writer who must remember is by
  construction the one whose attention is elsewhere.** A hold is remembered by
  whoever is thinking about beerssh; a spread is run by whoever is thinking
  about the fragment. Relaying it harder cannot reach that, and neither can
  enumerating the peers — an earlier near-miss involved a third session nobody
  had thought to name.
  **What works is a marker resident in the tree.** `.git/write-hold` carries
  the reason as its text; `sync.py` skips any project that has one, names it
  HELD, and drops it from the compared count, so a held tree is out of the
  arithmetic as well as out of the writing. In `.git`, so it is machine-local
  and never cloned. **It expires**, and the expiry is by AGE rather than by
  the commit it names: a gate that passes is followed by a *push*, not a
  commit, so HEAD does not move and a commit-named marker on a dead session
  is exactly as immortal as a bare one — the commit name is kept only as an
  early clear, since a tree that has committed past the gated commit is
  certainly no longer gating it. An expired hold is reported and then ignored,
  never dropped quietly; and "cannot say" does not override a hold, so if the
  directory is not a repository the hold stands.
  **It closes the spread road, not every road.** Nothing blocks an ordinary
  commit, so a session working directly in the tree can still move HEAD under
  a run. Whether that earns a `pre-commit` hook is open and is a question for
  a deliberate pass, not for whichever session is currently paying the
  thirteen minutes: it would block every other writer for the length of a
  gate, and the party proposing it is the one who benefits.
  **Keep the two apart while deciding.** The receipt that refuses is the
  safety property; closing the window is only a convenience. Every one of the
  six refusals cost container minutes and none of them risked a bad push.
  Not a thing to fix in the gate. A receipt that tolerated a moved HEAD would
  be a receipt for a tree nobody built, which is the whole failure 9bc3395 and
  5e9b540 exist to prevent.
- **Build both platforms on every change** — Linux host build + all Android ABIs.
  A change that builds on one and breaks the other is the failure mode this whole
  document is arranged to prevent, and only CI catches it reliably.
- Run the host unit tests (§12.3) and the **font validation script** (§9.6).
- Run the Android x86_64 emulator for smoke tests.
- **The runner's toolchain is older than the developer's, and that is the whole
  point of it.** `ubuntu-latest` is Ubuntu 24.04: **Qt 6.4.2** and **libssh
  0.10.6**, against 6.8.2 and 0.11.2 on the machine this was written on. Two
  build-breaking uses of newer APIs got in unnoticed because they compile
  perfectly here — `Qt::ColorScheme` (6.5+) in the platform interface, and
  `ssh_channel_get_exit_state` (libssh 0.11+) in the shell-exit path. Neither is
  reachable by any amount of local testing.
  The fix for the first is the rule the SSH layer already follows: **no library
  type in an interface**. `bssh_appearance_t` is this project's own, so a Qt
  version bump cannot pin every caller; the Qt calls that do exist are behind
  `QT_VERSION_CHECK`, and the detection chain still answers without them —
  which on Linux it was already doing better than Qt anyway (§6.2). The second is
  a version guard falling back to `ssh_channel_get_exit_status`, which returns
  the only field that path actually used.
  **`make check-ci` reproduces the runner** in its own image, and is what found
  all of this plus an indentation regression the local gate had not been re-run
  against. It reads the package list **out of the workflow** rather than
  repeating it: the failure it exists to catch is a dependency the runner image
  happens to carry and the workflow never names, and a second copy of the list
  would be a second thing to be wrong. If the workflow changes shape the parser
  refuses to run rather than installing an empty list — a check that quietly
  stopped looking reads exactly like a passing one, which is the same failure
  `check-indent.sh` guards against with its file-count floor. Not a CI step: CI
  already *is* the runner.
- **`check-ci` sent the working tree rather than HEAD, and the "only what is
  committed" promise held nowhere — not just for the submodule.** Found on
  2026-08-10, scope corrected and fixed on 2026-08-13.
  As first recorded, this was a submodule problem: the main tree was tarred from
  `git ls-files` *deliberately*, so the container got what a push would give the
  runner, while the submodule could not come that way — `git ls-files` prints
  `vterm` as a bare path — so the script took a plain `tar -cf vterm.tar vterm`
  of the directory as it sat. A green run therefore said "HEAD plus whatever is
  in the tree builds". It surfaced on a run that passed on `ubuntu:24.04` while
  carrying 38 uncommitted lines in `vterm/src/screen.c` and `src/state.c` from a
  parallel session.
  **The main tree had the identical hole, and the comment three lines above the
  code denied it.** `git ls-files` names tracked *paths*; the `tar` that follows
  reads those paths off the disk, so every uncommitted edit to a tracked file
  rode in exactly as the submodule's did. Measured rather than argued: a marker
  line appended to a tracked file and left uncommitted arrived in the tar, and
  did not arrive in `git archive HEAD` of the same file. Worth recording as its
  own lesson — the original diagnosis was right about the symptom and wrong
  about the scope, because it trusted a comment instead of the line under it.
  Both halves are `git archive` now: `HEAD` for the main tree, and `vterm`'s own
  `HEAD` for the submodule when the workflow asks for submodules. Archive reads
  the tree object, so it cannot emit uncommitted content, and it omits
  submodules on its own — which retires the positional `--no-recursion` the old
  line depended on being kept in front of `-T`.
  **The patch step was the third thing this hid.** Because the submodule arrived
  already patched, the container stamped `build-vterm/.patched` to skip the
  patch rule — so the one rule that breaks when upstream moves under
  `vterm-patch/` was never exercised by the reproduction that exists to catch
  exactly that class of divergence. The submodule now arrives unpatched, as the
  runner's checkout leaves it, and `make` applies the patches in the container
  the way it does on the runner. `git` joins the container's package list for
  it; `git apply` needs no repository, which is what makes this work on an
  extracted tree.
- **Pixel assertions get a tolerance, not equality.** Antialiasing bleeds
  differently between Qt versions — 3/255 on 6.4 against 0 on 6.8 — so a test
  demanding an exact background colour in the margin fails on the older one while
  testing nothing extra. A glyph genuinely drawn there is ~180/255 away, so the
  tolerance is nowhere near what the test is looking for.
- **Every `PKGCONFIG` line has a package named in the workflow, even when
  something else would drag it in.** `libsecret-1-dev` was satisfied for months
  only because the runner image happened to carry it, and `libssl-dev` still
  arrives as a dependency of `libssh-dev`. Both are declared now: a build that
  depends on a package nobody named fails one image update later, with a
  pkg-config error that names the library and not the fix — and the failure lands
  on whoever pushed next rather than on whoever added the dependency. There is
  deliberately no tool checking this, because the case that bit is a package
  present on the runner and absent from the file, which nothing run locally can
  see.
- **A `timeout` around a container runner bounds the runner, not the
  container.** Measured 2026-08-31: `timeout 600 sg docker -c "make check-ci"`
  killed make at ten minutes and `docker ps` showed the container Up for
  another ten, still compiling, reparented and unsupervised. The trap in
  `check-ci.sh` had already reasoned about exactly this moment — its comment
  says an EXIT-only trap leaks the staging tree "when a long docker run gets
  cut short" — and its remedy was half the size of the problem, because a
  trap cleans up what the *shell* owns and a container is a child of the
  docker daemon, which hears nothing when the shell is signalled.
  **This is worse than a wasted run.** The receipt that gates a push is
  written after the container exits under make's supervision, so the orphan
  finishes nothing that counts and then competes with the next attempt for
  the same cores — a bound that stops the supervisor while the work carries
  on turns a slow success into an invisible one, and the ten minutes it
  "saved" were spent twice. All three container scripts — `check-ci.sh`,
  `check-deb.sh`, `check-keepalive.sh` — now pass `--name` and kill it from
  the trap. **The name is what makes cleaning up possible at all**: docker's
  generated names are random, so an orphan from a previous run and the
  container the current run is waiting on are indistinguishable in
  `docker ps`, and there is no safe by-hand recipe to write down.
- **That fix did not work, and proving it is what found the real defect.**
  The `--name` half was verified immediately — the container went from
  `reverent_mendeleev` to `bssh-check-ci-17219` — and the kill half was
  assumed. It could not fire: **a shell trap cannot preempt a foreground
  command**, it runs only once that command returns, and `docker run` *is*
  the foreground command. Measured on a two-line fixture with a working
  control: a script sleeping in the foreground ignores SIGTERM until the
  sleep ends and its trap never runs, while the same script backgrounding
  the sleep and calling `wait` fires its trap at once. Then measured on the
  real thing, which is the control that matters — the committed shape,
  interrupted, leaves `script: STILL RUNNING` and `container: STILL UP`;
  backgrounding the run and waiting gives `script: exited` and
  `container: GONE`. So the kill only ever ran on the normal exit path,
  where the container has already gone and killing it is a no-op.
  **A cleanup in a trap is not a cleanup until something proves the trap
  can run**, and "the trap contains the right command" is not that proof.
- **What it still does not buy, measured rather than assumed.** An outer
  `timeout N sg docker -c "make check-ci"` signals `sg`, and nothing
  downstream hears it: a `timeout 75` was observed alive at 2m45s with the
  script and the container both running underneath it. So the original
  incident is only half what it looked like — the wrapper did not merely
  fail to stop the container, it did not reliably stop the *supervisor*
  either. Signal the script itself, or kill the container by name. The name
  is `bssh-check-ci-$$`, so it carries the pid of the shell to signal: the
  container names its own supervisor, which is the whole answer to "which
  process do I send this to".
- **That fix shipped a defect of its own on the first attempt, and `sh -n`
  could not see it.** In `check-deb.sh` the name was assigned beside the
  staging tree — inside the `else` branch — while `docker run --name` sits
  below the conditional. The documented fast loop,
  `make deb && DEB_DIR=build/deb ./tool/check-deb.sh`, takes the other
  branch, so `set -u` would have aborted it at the `docker run`. Both
  versions parse, because an unset variable is a runtime error and not a
  syntax one; a control built as the shape nearly committed dies at line 110
  with rc=2 where the fixed script exits 0. **A cleanup added for one path
  is not a cleanup until the other paths still run**, and the branch that
  skips the work is the one that never gets exercised while testing the fix.
- **Run the gate as a background job the harness owns, not under an outer
  `timeout`.** This is the local remedy for the orphan above and it is the
  one that actually holds: a backgrounded job is supervised for its whole
  life and announces its own exit, so nothing needs to guess a duration, and
  there is no wrapper whose death can strand the work. The two bounds that
  were tried first both failed in the same direction — an outer `timeout`
  around `sg docker -c` never reached the script, and a `timeout` that *did*
  land would kill the supervisor while the container carried on. **A bound
  chosen by predicting how long the work takes is the wrong instrument
  anyway**: the gate's honest range here is roughly eight to thirteen
  minutes depending on what else is compiling, so any number tight enough to
  catch a hang is loose enough to kill a healthy run, and a run killed at
  twelve minutes costs the whole twelve.
- **`wait "$!"` still fails the gate, and this had to be checked rather than
  assumed, because the change sits on the path that decides pass/fail.**
  Under `set -e` a failing backgrounded child gives the script the child's
  status and the line after `wait` is never reached — measured at rc=3, with
  a positive control confirming a succeeding child does reach it. The
  receipt write is after that point, so a failing container aborts before
  anything is recorded. The failure mode being guarded against is the worst
  one available: a gate that passes unconditionally looks exactly like a
  gate that passes.
- **Three probes were wrong in one session and nothing they measured was.**
  Recorded because the tally is the useful part rather than any one of them.
  A `pgrep -f 'sh tool/check-ci.sh'` matched **the shell running that very
  command line** — my own — so the kill landed on the wrong process and the
  output truncated in a way that read as the test failing. A fixture using
  `'EOF'` quoting never expanded its path, so its marker file was never
  created and the column reporting "trap ran" was meaningless in both rows.
  An `awk` line-width check died with a collation error and the `echo` after
  it printed "widths checked" regardless. Each was caught the same way: ask
  what a case *known* to be good should produce, and check the probe says
  it. The container's name is the durable version of that lesson —
  `bssh-check-ci-$$` carries the pid of the shell to signal, so the thing
  being cleaned up names its own supervisor and no process-tree hunt, and no
  `pgrep` pattern, is needed to find it.
- Keep native dependency build scripts reproducible and pinned to versions.
- Signing config for Play (upload key + Play App Signing).

---

## 13. Google Play compliance checklist

Android-only. Desktop has no equivalent gate — which makes it tempting to let the
desktop build ship things the Android one can't. Don't: the store rules that also
happen to be good practice (licence attribution, no unnecessary permissions,
honest data handling) apply to both packages.
- Target **API 35** (2026 requirement for new apps); 64-bit ABI present.
- **Foreground-service type declared** in manifest **and** justified in Play
  Console (see §5.1). Prepare the written use-case description.
- **`POST_NOTIFICATIONS`** runtime permission flow (Android 13+).
- Data safety form: declare what's stored (credentials stay on-device).
- **Open-source licence attribution screen** for bundled fonts and libraries (§9.4).
- Justify any sensitive permissions; avoid `QUERY_ALL_PACKAGES` and unneeded
  permissions. **Note:** left alone, `androiddeployqt` injects
  `READ/WRITE_EXTERNAL_STORAGE` from Qt6Core's own dependency manifest — an
  SSH client asking for the user's photos. The `%%INSERT_PERMISSIONS` marker
  is therefore deliberately absent from our manifest, so the permission list
  is owned rather than inherited. Verified in the built APK. Gate `REQUEST_IGNORE_BATTERY_OPTIMIZATIONS` behind explicit user
  action or drop it.
- Store listing per §0.1: title `BeerSSH — SSH Terminal`, package
  `se.vibes.beerssh`, JuiceSSH-successor framing in the description body.
- Content rating: no alcohol/mature content in the app; expect *Everyone*.
- Large-screen quality: Play surfaces tablet/foldable readiness — the §8 adaptive
  layout work directly helps store presentation.
- Privacy policy URL.
- **The versionCode is derived, not written.** It was hardcoded to 1 in
  `beerssh.pro`, which is enough for exactly one upload: Play accepts a bundle
  only if its versionCode strictly exceeds the last one it took, so every
  update after the first would have been refused — and being updatable is most
  of the point of being on a store. The Makefile computes it from `VERSION` as
  `major*10000 + minor*100 + patch` (0.1.0 is 100, 1.2.3 is 10203) and passes
  it to qmake, which keeps the fallback of 1 so a build opened straight in Qt
  Creator still produces something installable. Computed in `make` because
  qmake has no arithmetic — `num_add` exists and `multiply` does not, and
  assembling the digits as a padded string gives `"00100"`, which is not an
  integer a manifest should carry. `ANDROID_VERSION_CODE=` overrides it for the
  case the scheme cannot serve: re-uploading the same version after Play has
  already consumed a code, which is what a rejected release leaves you needing.
- **`make android-aab` is the Play artifact and is treated like one.** It was a bare
  recursion into Gradle, leaving its output under a generated name that says
  nothing about which app or version it holds — while `make android`, the artifact
  that matters *less* here, had naming and a signing check. The bundle now
  comes out as `beerssh-<version>.aab`, reports the versionCode it was built
  with rather than leaving that to be discovered from the Console after the
  upload, and **refuses to produce a debug-signed bundle at all**, since Play
  will not take one.

---

## 14. Milestones / phased plan

**Every milestone lands on both platforms before the next one starts.** Not
"desktop first, Android later" as phases — per-milestone, desktop is where a
feature is written and Android is where it's finished. A milestone with a green
desktop build and an untried Android build is not done, and the debt from
deferring that compounds fastest in exactly the two riskiest areas (input,
background).

1. **M0 – Skeleton & toolchain.** *(Android half done: the app
   cross-compiles for arm64 and x86_64, packages, installs, runs, and
   connects over SSH on an emulator. `make android` checks for the Qt kit,
   NDK, SDK, a JDK and the cross-built dependencies by name, because each
   of those fails unhelpfully when missing.)*
   Qt Widgets app named per §0.1
   (`se.vibes.beerssh`) building **both** ways from one qmake project: host build
   on Linux, Qt-for-Android + NDK build for arm64 (libssh + OpenSSL cross-compiled
   when M1 needs them, not before). Blank terminal widget renders on both. **Both
   builds in CI from day one** (§12.4) — retrofitting the Android build later is
   how it rots.
2. **M1 – SSH connect + raw shell.** `bssh_ssh_session` (libssh impl), password
   auth, open PTY channel, pipe bytes to a naive text view. Prove end-to-end I/O.
3. **M2 – Terminal emulator.** Integrate libvterm (or custom parser); colors,
   scrollback, resize/`window-change`, UTF-8. Real shell usability. Golden-file
   tests (§12.3) start here, not later.
4. **M3 – Keyboard.** Physical/Bluetooth key handling + `bssh_input_router` +
   on-screen key row (Ctrl/Alt/Esc/arrows). Get the tables right on desktop, then
   test on a real Android hardware keyboard *and* on Wayland + X11.
5. **M4 – Platform layer & background.** **Done on both platforms.**
   `bssh_platform` split; Android foreground service + JNI bridge +
   notification, verified keeping a backgrounded session alive on an
   emulator; desktop tray/stay-resident behaviour and the
   close-with-sessions prompt (§5.5).
6. **M5 – Reconnection & network awareness.** Connection FSM, keepalive, network-
   change reconnect (Android callbacks + desktop `QNetworkInformation`, including
   suspend/resume), backoff, status UI. **Built:** `bssh_conn_manager`
   per session, exponential backoff capped and jittered, network-change
   retry, user-disconnect suppression, and an attempt limit so a dead host
   stops waking the radio. **Keepalive is built** (§4): a per-host
   SSH_MSG_IGNORE interval for NAT plus TCP keepalive and `TCP_USER_TIMEOUT`
   for dead-peer detection, applied both when settings resolve and when the
   transport is built — they arrive in that order, and doing only the first
   leaves every first connection unprotected. The reconnect status is in the
   UI. **The timings are now verified against a real dropped link**
   (`make check-keepalive`), which was the last thing open here. A closed
   socket was never the hard case — a peer that goes away cleanly sends a FIN
   and is noticed at once — so the check black-holes the port with `iptables`
   inside a container and makes the packets genuinely vanish. Detected in
   **62s**, against a budget of "about a minute"; the same link with the socket
   options removed was **still believed connected after 240s**, which is what
   says the options rather than the kernel's defaults are doing the work. Both
   halves behave the same: 62s with a shell being typed into (where
   `TCP_USER_TIMEOUT` bounds unacknowledged data) and 62s completely idle
   (where the keepalive probes are the only thing on the wire).
   **The reconnection machine is verified on the same link.** The backoff
   schedule is a pure function and unit-tested as one; what that cannot show is
   that the FSM *engages* on a genuine drop — the failure being a session that
   goes quiet and stays quiet — or that what comes back is usable. The check
   drops the link, waits for the session to notice and schedule a retry, and only
   **then** restores it: restoring earlier would prove nothing about detection,
   and never restoring would only prove the backoff can count. The session
   reconnects. Neutering `attempt_reconnect()` fails exactly that assertion,
   which is what says the test is measuring the machine and not the weather.
   Key authentication is what makes it seamless, exactly as §5.3 claims: the
   password is consumed at first connect and deliberately not kept (§11), so a
   password-authenticated session could only reconnect as far as asking again.
   **And the shell that comes back gets a clean terminal, which it did not.**
   The emulator survives a reconnect on purpose so the user keeps their
   history — and that meant every MODE the old shell's programs set survived
   with it, into a shell that set none of them and cannot know to turn them
   off. Mouse reporting, so every click writes a report onto the new command
   line; bracketed paste, so a paste arrives wrapped in markers the shell
   prints as text; application cursor keys, so the arrows send bytes it does
   not understand; and the alternate screen, so a link that dropped inside an
   editor came back with the shell drawing into the editor's buffer and the
   history hidden behind it. `reset_modes()` is a **soft** reset —
   `vterm_screen_reset(vts, 0)` — because the hard branch homes the cursor and
   erases the screen, which is the history the reconnect exists to keep.
   Two things libvterm will not do here, both found by breaking them: it sets
   `state->mode.alt_screen` directly without going through
   `VTERM_PROP_ALTSCREEN`, so the alternate buffer has to be left through
   libvterm's own path first or the state says primary while the screen layer
   still points at the alternate buffer; and the shadows kept beside it to
   answer DECRQM — the mouse mode, `modifyOtherKeys`, the kitty stack — are
   ours to clear.
   **The sabotage runner earned its keep here.** The first spec had four
   edits: the three inside the reset were caught, and removing the CALL SITE
   was MISSED — because the test asked the emulator directly, and a reset
   nothing invokes is indistinguishable from a working one. A session-level
   test holds that ground now.
7. **M6 – Security & profiles.** `bssh_secret_store` with both backends, key
   auth, host-key verification, host profiles, biometric unlock where available.
   **Built, except biometric unlock:** public-key authentication is tried before any password
   (agent, then the default identities), so a key-authenticated session holds
   no credential at all and can reconnect on its own; passphrase-protected
   keys are offered an unlock prompt before any password, since for a
   key-only server a password prompt is not a fallback but a prompt that
   cannot succeed; and the trust-on-first-use prompt is built (§11). Still
   open: biometric unlock. The store IS wired
   in: a password can be remembered per host (off by default), the profile
   row carries a non-secret `has_password` flag so the app knows whether
   asking for the store's passphrase is worth it, and a remembered password
   is replayed on reconnect instead of being held in memory all session.
8. **M7 – Sessions & tabs.** `bssh_session_registry` + session/view separation,
   tab bar, OSC titles (and desktop window title), per-tab status, layout
   persistence. *(Do this before splits — the registry is the foundation.)*
   **Built:** registry, `bssh_session` owning transport + emulator +
   router, a tab bar rebuilt from the registry, and titles falling back
   OSC → profile name → target. Layout persistence and per-tab status dots
   landed too — `save_layout`/`restore_layout` and `bssh_tab_status::dot()` —
   and this line went on saying they were open long after.
9. **M8 – Adaptive splits.** `bssh_layout_manager`, size classes, 2/4-pane
   splits, focus indication, collapse-to-tabs on shrink, per-pane
   `window-change`. Desktop window-drag resizing exercises this hardest — use it.
   **Built, and reachable since the wiring landed.** Size classes, pane
   limits, the collapse/restore rule with the layout intent remembered,
   per-pane geometry, the focus border, pane navigation chords, and dividers
   snapping to whole cells — all of it existed and was tested for several
   milestones while **nothing in the app ever constructed a
   `bssh_pane_container` or a `bssh_layout_manager`**, so the window showed
   exactly one view and every one of those features was unreachable. The
   window now holds a pane container, splits are made from the tab context
   menu ("Show beside current", §8.3), the navigation chords reach
   `focus_adjacent`, and a resize feeds the width in dp to the layout
   manager so folding or tiling collapses the splits to tabs. The arrangement
   is persisted (profile ids, pane count, active tab), and **every
   remembered host now reopens** -- as a tab that is NOT connected.
10. **M9 – Fonts.** Curated set, fallback chain, wide-char handling, shared glyph
    cache, asset-pack delivery (Android) / full set (desktop), CI validation
    script, fractional-scaling and multi-monitor DPI checks.
    **Mostly built:** the set is decided and pinned (§9.2), `bssh_font_catalog`
    resolves a stable id through §9.3's chain per glyph, fonts are built with
    font merging **off** so nothing proportional can be substituted in, real
    bold/italic faces are loaded rather than synthesised, ligatures are
    disabled by default, the font directory is a `bssh_platform` question,
    and §9.6's validator runs in CI and fails the build on an unsuitable
    font. **On both platforms:** the base set (JetBrains Mono, DejaVu, Hack,
    Noto — 4.1 MB, 13 faces) ships inside the APK and is verified loading
    from `assets:/fonts` on a device.
    **The shared glyph atlas is settled and declined, not open**, and this
    line said otherwise for long enough to nearly buy it twice. §9.5 asked
    the question with a benchmark and answered it: Qt's font-engine cache is
    already keyed by exactly the tuple §9.5 proposed — family, size, weight,
    glyph — and is shared process-wide, so the first pane to paint a size
    pays for the glyphs (1.74–1.88 ms) and the rest get them (1.0–1.6 ms).
    **An atlas of our own would be a second cache in front of a working
    one.** The justification printed here was wrong as well as stale: it said
    the renderer caches family lookups and QFonts "not rasterised glyphs",
    which is true of *our* caches and false of the one that matters.
    **This is §16's failure in the milestone list rather than the register**,
    and it is why a summary must point at where a thing was measured instead
    of repeating the reasoning: a copied justification cannot be corrected
    when the measurement that replaces it lands somewhere else.
    Still open: Play asset-pack delivery and subsetting
    for the rest
    (Iosevka alone is 42 MB, so the condensed face cannot be the compact
    default until it ships), and the fractional-scaling and multi-monitor
    DPI checks — **the fractional-scaling half is done**: the cell now snaps
    to whole *device* pixels rather than whole logical ones, because at 150%
    a 9-pixel cell is 13.5 device pixels and box-drawing characters stop
    meeting at the cell edge. TUI borders came out seamed at every other
    column; solid at 100%, visibly broken at 150%, found by rendering both
    and looking. Multi-monitor DPI is **wired now** — the window watches
    `QWindow::screenChanged` and re-measures every pane's cell, where
    before nothing under `src/` referred to `QScreen` and no amount of
    dragging could have reached any code. Still unverified on real
    hardware, which needs two monitors of different scaling. The **font picker is built** and is one widget on both
    platforms (§9.4): Window → Appearance in the PuTTY-shaped dialog, the
    host page on Android, with a live preview, unavailable families listed
    and marked rather than hidden, and the choice stored as a per-host
    override only where it differs from what the host already resolves to.
11. **M10 – Preset system.** Preset/profile layering, built-in starter presets,
    JSON import/export, propagation UI. **Core built:** `bssh_preset_t` (with
    no field a secret could occupy), the three-layer resolver with
    override detection and revert, JSON round-trip, and the six starter
    presets, the preset table with the starter set seeded once, hosts
    following a preset with sparse overrides, and `hosts_using()` — the
    number behind "used by 7 hosts". Shipped presets are structurally
    read-only: saving over one is refused, and a preset still followed by a
    host cannot be deleted. **The editing UI is built**: presets are listed
    with the shipped ones marked, the fields are editable, and "used by N
    hosts" is on screen while they are being typed into as well as in a
    confirmation before the write. A shipped preset offers "Save as new"
    instead of Save rather than a Save that silently fails, and saving
    re-resolves the *open* sessions that follow the preset — propagation
    that only reached the next connection would look like the edit had not
    worked. On Android the same editor is reached from the Hosts screen
    and laid out for touch — one column, a full-width list, 48dp controls
    and a way back — because §14's both-platforms rule is not satisfied by
    a desktop layout that technically renders on a phone. **Override badges
    and revert are built** (§10.1): a host's Appearance page says whether it
    is following its preset or overriding it — naming the preset and what
    that preset would give — and offers "Use the preset's font", which drops
    the override rather than copying the preset's current value, so the host
    goes back to *following* rather than merely agreeing for now. That claim
    was **half true for several milestones** and is worth recording as the
    shape of drift §14's rule exists to catch: the badge, the revert and the
    three-layer resolve behind them were built on the PuTTY-shaped dialog
    only. The touch page resolved against the app defaults alone — correct
    while it could only *add* a host, wrong the moment it could open one that
    follows a preset, and invisible because the touch front door had **no way
    to edit an existing host at all**. Both editors now call one
    `bssh_font_override_badge()`, so the answer cannot depend on which front
    door you opened. Preset import/export through the UI landed as well
    (§10.5) — the buttons are on the preset editor and the format is INI, the
    same one the store uses — and this line went on calling it open.
    **The settings now act.** `term` reaches the PTY request **and** the
    emulator's XTGETTCAP mirror, `compression` the handshake,
    `keepalive_seconds` the transport, `scrollback_lines` the emulator **and
    the view's scrollbar**, `cursor_style`/`cursor_blink` the renderer,
    `bell_enabled` the tab marker, and `on_connect_command` runs on every
    connect — including reconnects, which is the one that matters, since
    §5.3's answer to "a reconnect is a new shell" is attaching to tmux.
    **Every preset field now reaches everything that uses it**, and the two
    emphasised above are why that word had to change. This line used to end
    "reaches *something* that uses it", and both of those fields had a second
    consumer that nothing was wiring: `term` went to the channel while the
    emulator answered XTGETTCAP with its default, so the terminal named
    itself one thing in the environment and another when asked; and
    `scrollback_lines` trimmed the ring without telling the view, so the
    scrollbar advertised history that had been dropped.
    The wording encoded the defect. "Reaches something" is satisfied by the
    first consumer anybody thinks of, and so is the test named for the same
    claim — `every_resolved_setting_reaches_something_that_uses_it` checked
    the transport and not the emulator, and passed. A field is wired when
    every consumer has it, and counting them is the only form of the question
    that can fail. **`colour_scheme` is built** (§10.2):
    six curated schemes, picked alongside the font so the preview shows what
    the terminal will actually look like, with the sixteen swatches drawn
    because a scheme's name tells a user nothing.
12. **M11 – Settings, polish, accessibility.**
    **Accessibility started, measured rather than assumed.** Querying
    `QAccessible` for what a screen reader would actually be told found the
    controls in three states: the touch host editor announced everything
    correctly *by accident* (`QFormLayout::addRow` makes the label a buddy for
    you), the desktop Session page left **4 of 13** controls anonymous because
    its rows are a label and a field side by side in a box layout, and the touch
    host list's **2 of 2** controls — every control on the phone's first screen —
    were read out as "vertical ellipsis" and "plus", which is what they look like
    and not what they do. All named now.
    **The terminal itself was the real gap**, and naming would not have fixed it:
    a widget that paints characters appeared as role `Client` with no name, no
    value and no text interface, so the entire content of the application was
    invisible. `bssh_terminal_accessible` gives it role `Terminal`, its screen as
    the value, and a `QAccessibleTextInterface` with line navigation and the real
    cursor position. Rows are joined with newlines rather than padded to the grid
    width, because Qt's boundary navigation works on `\n` and a reader that
    announces eighty spaces before every line break is one nobody will use.
    Read-only and it says so: the cursor belongs to the remote program, so
    `setCursorPosition` does nothing rather than lie. Change notification is
    guarded by `QAccessible::isActive()`, so the flatten cost stays off the
    `damaged()` path that §9.5 spent two commits making cheap.
    **Scrollback and selection are exposed too**, and they had to arrive
    together: a selection starting in the history had no offset to start *at*
    while only the visible screen was published, and a range reported against the
    wrong coordinates is worse than none. History and screen are one stream,
    because to somebody reading that is what they are — the boundary between them
    is a scroll position, not a document break — and a build log a sighted user
    can scroll back through was otherwise simply gone. Only the visible rows get
    a `characterRect`: a line that has scrolled off is not on screen, and
    inventing coordinates for it would send a magnifier somewhere blank.
    The text is **cached against a revision counter** the view bumps when it
    repaints. A full 5000-line buffer is ~257,000 characters, and a reader asks
    these questions constantly, so rebuilding per query would put the cost of the
    whole history on every keystroke. Building it once per change cost **16.7 ms**
    until the per-character `QString::fromUcs4` — a whole QString allocated and
    destroyed per cell — was replaced by appending code units directly: **1.4 ms**,
    which is what makes exposing the history affordable at all.
13. **M12 – Release readiness.** Play compliance checklist, data-safety, FGS
    justification, licence screen, signing, closed test — plus the desktop
    packaging decided in §12.2, its `.desktop`/AppStream metadata, and a first
    tagged desktop release.

**Sequencing notes:** M7 must precede M8 (registry before splits). M9 should land
before M10, since presets reference fonts. Keep extra buffer on **M3 (keyboard)**
and **M4/M5 (background)** — highest risk, and the two milestones where the
platforms differ most, so budget for doing each of them properly twice. M8's
collapse/restore is cheap *if* M7's session/view separation was done properly and
expensive if it wasn't.

---

## 15. Key risks & mitigations
| Risk | Impact | Mitigation |
|---|---|---|
| Qt Android IME/hardware-key quirks | High (core UX) | Prototype input early (M1/M3) on real devices; handle both key + IME paths. |
| FGS type rejected by Play review | High (ship blocker) | Prefer `connectedDevice`; prepare `specialUse` justification; verify live policy. |
| Session state trapped in widgets | High (breaks §8 + background) | Enforce `bssh_session_registry` separation from M7; review any state added to a view. |
| **Platform drift — desktop races ahead, Android bit-rots** | High (silently becomes a desktop app with a broken Android build) | Both builds in CI from M0 (§12.4); no milestone closes on one platform; platform code confined to `bssh_platform` (§3). |
| **Platform-specific behaviour leaking into the core** | Medium/High (every fix needs doing twice) | `#ifdef` outside the platform layer is a review failure; the router, emulator and layout manager take no platform input. |
| Desktop secret store absent (headless/minimal session) | Medium | Passphrase-derived fallback (§11), and say plainly in the UI which backend is active. |
| APK bloat from bundled fonts | Medium | 2–3 fonts in base, rest via Play Asset Delivery; subset fallbacks (§9.4). |
| APK bloat from Qt's own deployment | Medium | `androiddeployqt` scanning for itself bundles the virtual keyboard plugin and with it QtQuick, QtQml and Lottie — a QML runtime in an app §2 forbids from using QML. `ANDROID_DEPLOYMENT_DEPENDENCIES` lists what ships instead: 29.8 MB → 18.6 MB, verified running. |
| Wide-char / CJK cell misalignment | Medium | Implement `wcwidth` rules early; test with CJK + emoji + `htop`. |
| Multi-pane render perf (4 panes, chatty output) | Medium | Shared glyph cache; repaint throttled to refresh rate, not to data. Desktop hides this (fast CPU, big cache) — profile on a phone. |
| libssh LGPL linking obligations | Medium (legal) | Dynamic linking, or switch to BSD-licensed libssh2; decide before ship. |
| Preset propagation surprises users | Medium | "Used by N hosts" warning; per-field override badges with revert. |
| OEM battery killers stop the service | Medium | Battery-opt whitelist prompt; document; robust reconnect. |
| No SSH session resume on reconnect | Medium | Document; tmux/screen via preset on-connect command; Mosh in v2. |
| `qtermwidget` doesn't fit Android/no PTY | Medium | Use libvterm/custom emulator over the SSH channel, not a PTY — and don't take it as a desktop-only shortcut (§7). |
| Native dep cross-compile pain | Medium | Pin versions; scripted reproducible NDK builds; start at M0. |
| Play target-SDK churn | Low/Med | Re-check API-level + FGS rules at build time. |

---

## 16. Open decisions to resolve during implementation

This list is a **register, not a backlog**: an entry leaves the open half by
being decided somewhere in this document, and moves to the closed half with a
pointer at where. It had gone stale in the way a register does — eight of its
entries were settled elsewhere here while this section went on calling them
open, which is worse than an out-of-date list, because a reader who trusts it
relitigates a decision that has already been made and built.

**Still open:**

- libssh (LGPL, dynamic link) vs libssh2 (BSD) — licensing may decide it, and it
  is the Android build that decides it (§4).
- Foreground-service type: `connectedDevice` (built and shipped in the manifest)
  vs `specialUse` (prepared fallback). Open on Play review's opinion, not on
  ours — §5.1.
- Inhibit sleep on desktop while a session is connected? (Leaning no; per-session
  toggle instead — §5.5.)
- Offer system-installed monospace fonts on desktop? (Not v1 — §9.4.)
- Desktop `ssh-agent` support (§11) — v2, but the secret-store interface should
  already accommodate a backend that never releases key material.
- Which font is the **phone** default (JetBrains Mono vs condensed Iosevka).
  The shortlist itself is settled; this half is not, and cannot be until
  Iosevka's 42 MB ships by asset pack (§9.4, M9).
- Honour system font-scale for the terminal grid? (Leaning no; setting instead.)
- Host groups/folders in addition to presets? (§10.5.)
- Bundle an optional custom soft-keyboard? (Lean no for v1 — §6.2.)
- Persist scrollback across restarts?
- Icon and branding direction (§0.1), which the placeholder in §12.2 is waiting
  on.

**Decided, and kept here so they are not reopened:**

| Decision | Where |
|---|---|
| Crypto backend — OpenSSL | §4 |
| Min SDK — API 26 | §2 |
| Desktop secret-store backend and its no-keyring fallback — both built, chosen at runtime | §11 |
| Desktop packaging — tarball, `make install` and a `.deb`; Flatpak deferred | §12.2 |
| Multiple top-level windows on desktop — in, one process-wide registry | §8.4 |
| Focus-follows-mouse on desktop — offered, off by default | §8.3 |
| Font shortlist — six selectable, two fallback-only | §9.2 |
| `~/.ssh/config` import — built, an explicit import and never a live read | §10.5 |

---

## 17. Notes for the implementer
- Keep the **SSH + terminal core portable and PTY-free**; the SSH channel is the
  byte stream. This holds on desktop too, where a local PTY is available and
  therefore tempting.
- **Sessions live in the `bssh_session_registry`, never in a widget.** This single
  rule underpins background survival, rotation, split↔tab collapse, and closing a
  desktop window without killing anything. Treat any session state found inside a
  view as a bug.
- **Platform differences live behind `bssh_platform`, one file per platform.** An
  `#ifdef Q_OS_ANDROID` in domain or UI code means the interface is missing a
  method. Qt Widgets only — a touch problem gets fixed by subclassing a widget,
  never by reaching for QML for one screen.
- **On Android, write only the JVM code the platform genuinely forces** (§5.2):
  a manifest-registered `Service`, its notification, and two JNI calls. No
  networking, protocol, session, or UI logic in Java/Kotlin — including glue that
  would merely be shorter there.
- Put **all key→bytes translation in one `bssh_input_router`** driven by emulator
  modes, and make it focus-aware once panes exist. The same keystroke must
  produce the same bytes on every platform.
- **`bssh_layout_manager` is the only component that decides tabs-vs-splits** from
  the window size class. Widgets ask; they don't decide. It takes a width, not a
  platform.
- **Presets must be structurally incapable of holding secrets** — enforce in types,
  not just in the export function.
- **Font validation runs in CI and fails the build** — that's what keeps "only
  terminal-suitable fonts" true over time. **Both platforms build in CI** for the
  same reason: a rule that isn't mechanically checked stops being true.
- **Verify current Google Play foreground-service policy and target-SDK
  requirement at build time** — these change and are ship-blockers if wrong.
- Test on **real devices**: at least one Bluetooth keyboard, one foldable or tablet
  (for §8 collapse/restore), and one aggressive-battery-management OEM. On
  desktop, test Wayland *and* X11, and a fractional-scaling display.
- **Follow §18 for style.** It is the same style as the `fuzzypickles` tree next
  door, deliberately, so moving between them costs nothing.
- **Reach for a library first, vendor it second, reimplement last.** Use what
  exists; where it is only 90% right, vendor it and fix the wall in the vendored
  copy; write it from scratch only when neither works. Vendoring is the escape
  hatch that makes committing to a library safe. It is why key→bytes translation
  calls libvterm's own `vterm_keyboard_*` rather than a hand-written escape table
  — libvterm was already vendored for the emulator, so the table would have been
  a second implementation of something the tree already carried.

### `make shots` finds layout defects; measuring the pixels settles them

Every layout defect here was found by rendering a screen, and several had passed
every test that existed. But **eyeballing a rendered screen has produced a
confident wrong answer four times**, so the render finds the candidate and a
measurement decides it:

- Box drawing "did not meet" — it did. The gap was smooth interpolation on a
  zoomed screenshot. Point-sample, or measure.
- The settings page was 386px wide "because a combo item was too long" — wrong
  twice; it was `addRow(QString(), w)` reserving the label column for nothing.
- The font picker "aligned at x=68" — it was 72 against 66, and stayed 6px out
  after the first fix, because equal label widths do not settle horizontal
  *spacing*.

Read the pixels out of the image rather than looking at it. A screenshot is
evidence of what rendered; an opinion about a screenshot is not.

**And the fourth was found by an audit rather than by a render.** The cursor
was drawn *after* `painter.resetTransform()`, in raw widget coordinates,
while every cell above it is drawn under a `translate()` past the grid inset
and past a left-hand scrollbar. So the block sat one pixel left and up of the
cell it marks — and a whole scrollbar's width away with the bar on the left,
which is the configuration nobody looks at. Measured before it was moved: the
cell painted at x 1..9 and the block at 0..9.
The blink's partial `update()` — the only non-full repaint in the class —
carried the same un-inset rectangle, so a band of the cursor cell was never
repainted while it blinked. It moved with the draw; a repaint region that
disagrees with what it repaints is the same bug wearing the other hat.
The punch-out went with them. It used `m_font` rather than the font the CELL
was drawn with, so it had neither §9.3's per-glyph family nor `cell_font`'s
letter spacing: a different shape and a different width from the glyph it is
cutting out of the block, and the notdef box for anything outside the chosen
family, since `make_font` sets `NoFontMerging`. **That half is not pinned by
a pixel assertion** — the test proves the block is punched rather than
covered, and which family answers is the fallback chain's question rather
than a measurable one.
Worth keeping as method: this was three defects in eleven lines that every
existing test walked past, and none of them came from rendering a screen.
They came from asking, of each primitive in the paint path, *what coordinate
space is this in* — a question a screenshot cannot be asked.

---

## 18. Code style and repo conventions — **[DECIDED]**

Same rules as the `fuzzypickles` tree. **The long form is `code-style.md` at this
repo's root** — worked examples, the tooling, and the reasoning; it takes
precedence over this section for detail, but must not contradict the three rules
below.

### 18.1 The three rules

1. **`snake_case`, not `camelCase`,** for identifiers this project defines.
2. **Tabs for indentation, spaces for alignment.**
3. **Lowercase filenames,** unless a tool demands otherwise.

### 18.2 Naming

`snake_case` for functions, variables, struct/type names and fields — **including
in the Qt C++ code, which is all of it here.** Qt's own API is `camelCase` and you
call it exactly as it is (`setParent`, `addWidget`, `sizeHint`); names *you*
introduce stay `snake_case`. Don't let the surrounding toolkit's convention pull
your own names camelCase.

- Types this project owns carry a **`bssh_` prefix**: `bssh_session_registry`,
  `bssh_input_router`, `bssh_ssh_session`. Plain structs/typedefs take a `_t`
  suffix (`bssh_preset_t`, `bssh_cell_t`); classes don't need it.
- C++ member variables keep an `m_` prefix but are otherwise snake_case:
  `m_session_registry`, `m_scrollback_lines`.
- Qt signals and slots are ordinary members: `bytes_received`, `on_resize`, not
  `bytesReceived`.
- Prefer a plain descriptive name over a decorated one — the SSH transport
  interface is `bssh_ssh_session`, not `ISshSession` or `AbstractSshBackend`.
  The Hungarian-ish `I`-for-interface prefix is not used anywhere in this project.
- The names in §3's diagram are the real ones. Use them rather than inventing
  synonyms, and update §3 if one genuinely needs renaming.

### 18.3 Indentation and alignment

Indent structural nesting with **tab** characters, one tab per level. When lining
up continuation or aligned tokens *within* a line — parameters under an open
paren, a block comment's `*` column, an aligned trailing comment — use **spaces**
after the indent tabs.

The point of the split: alignment is expressed relative to the shared leading
tabs, so it survives at any tab width.

`switch` bodies get a full tab too — a `case`/`default` body is one level deeper
than its brace nesting, since C and C++ labels open no brace.

**Do not run `clang-format`.** With no config it falls back to LLVM defaults,
which are spaces, and silently converts the whole tree; a config doesn't fix it
either, because `AlignAfterOpenBracket` can't express the continuation alignment
above and would reflow nearly every multi-line signature. The mechanical check
that *is* wanted lives in `tool/` (`tabify.py` + `check-indent.sh`, carried over
from `fuzzypickles`) and runs as `make style`, in CI alongside both builds.

### 18.4 Filenames

Lowercase `snake_case` for everything this project names itself — sources,
headers, docs: `terminal_view.cpp`, `input_router.cpp`, `platform_android.cpp`,
not `TerminalView.cpp`. The exception is a name a tool won't accept lowercased:
`Makefile`, `AndroidManifest.xml`, and similar. qmake imposes no such constraint,
so `beerssh.pro` is simply lowercase.

### 18.5 Commit conventions

Kernel format, and the detail lives in the global guidelines rather than being
restated here; `code-style.md` carries what the hook itself does.

- **`subsystem: summary`, imperative, no trailing period, 75 columns hard.**
  The subsystem is the tree's own unit — `src/ui`, `src/model`, `src/term`,
  `src/ssh`, `src/platform`, `src/input`, `tests`, `tools`, `build`,
  `packaging`, `android`, `docs`, `project.md` — and a change genuinely
  spanning the tree omits it. That is what makes the log filterable:
  `git log --grep '^src/term:'` answers "what has happened to the emulator"
  without a path list, and goes on answering after files move.
- **The body explains why, wrapped at 75 columns.** The diff already says
  what. A design document can be deleted or drift; the log cannot, so a
  message that carries the reasoning and the alternative rejected is doing
  its job, and length is not the constraint.
- **The whole history was reformatted to this on 2026-08-06**, and three
  indentation/debug fixups were squashed into their parents — 173 commits
  became 170, and every hash changed. Worth knowing before concluding that
  the discipline was there from the start, or that an old hash quoted
  somewhere else is a typo. It was force-pushed, so a clone taken before it
  holds the dead history and cannot fast-forward. The old head was `2ceaa25`
  and the rewritten one `3ca7d82`; `refs/backup/master-pre-reformat` still
  holds the old head locally, which is the only way to translate between the
  two, and is safe to delete once nothing needs to.
- **Rebase onto the remote; never merge into it.** This history has zero merge
  commits and is meant to keep it that way. Two sessions run in this tree and
  both push, so a divergence is ordinary rather than a surprise: fetch and
  read `git rev-list --left-right --count origin/master...master` before
  assuming a push fast-forwards, and rebase when it does not.
- **No docs-only commits.** Documentation changes ride along with the code
  commit they describe. Two judgement calls earn their own: reconciling
  `code-style.md` against its source, which is documentation-only by
  construction, and editing this document ahead of the code it specifies.
- **No AI-attribution trailers** in commit messages — no `Co-Authored-By:` for a
  tool, no generated-by footers. Messages end at their real content.
- `tool/hooks/commit-msg`, installed by `make hooks`, enforces the two
  mechanical halves: no generator attribution, and a subject within 75
  columns. Everything else above is a review item.
