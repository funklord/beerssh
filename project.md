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
- Local terminal / on-device shell — this is an SSH client, not Termux and not
  a desktop terminal emulator. Tempting on desktop, where a local PTY is right
  there; still out of scope for v1, and §3's no-PTY principle exists partly to
  stop it creeping in.
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
| Min SDK | **[OPEN]** API 26 (Android 8) suggested | Below 26, foreground-service + Keystore APIs differ; 26 keeps code simple. Android-only concern. |
| Target SDK | **[DECIDED]** API **35 (Android 15)** | Required for new Google Play apps in 2026; new apps must target API 35. |
| ABIs | arm64-v8a (primary), armeabi-v7a, x86_64 (emulator) | Google Play requires 64-bit; ship arm64 + optionally armv7. Desktop: x86_64 + aarch64. |
| Package format | **AAB** (Android App Bundle); desktop per §12.2 | AAB required by Play, and enables per-ABI + asset-pack splits (see §9.4). |
| Code style | **[DECIDED]** see **§18** | `snake_case`, tabs for indent, lowercase filenames — same rules as `fuzzypickles`. |

---

## 3. High-level architecture

Identifiers below are the real names to use in code — `snake_case` with a
`bssh_` prefix, per §18, on both platforms.

```
┌──────────────────────────────────────────────────────────────────────────┐
│  Qt / C++ layer (portable core — identical on Android and desktop)       │
│                                                                          │
│  ┌─ UI ──────────────────────────────┐   ┌─ Domain ────────────────────┐ │
│  │ bssh_tab_bar                      │   │ bssh_session_registry       │ │
│  │ bssh_layout_manager (tabs⇄splits) │◀─▶│  owns bssh_session[]        │ │
│  │   └ bssh_pane_view[]              │   │  (survives view teardown)   │ │
│  │       └ bssh_terminal_view        │◀─▶│   └ bssh_emulator (VT)      │ │
│  │ bssh_input_router (focused pane)  │──▶│      screen + scrollback    │ │
│  │ bssh_key_row (touch only)         │   │   └ bssh_ssh_session        │ │
│  │ bssh_font_manager                 │   │ bssh_conn_manager (FSM)     │ │
│  │ bssh_settings_ui                  │   │ bssh_preset_store           │ │
│  │                                   │   │ bssh_profile_store          │ │
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

**Core principle 1:** keep SSH + terminal emulation in **portable C++** with **no
local PTY dependency**. The SSH channel *is* the byte stream that feeds the
emulator; there is no `/dev/pts` on Android for an unprivileged app. This is why
desktop widgets like `qtermwidget` (Konsole-derived, PTY-oriented) are a poor
drop-in and a lighter VT emulator is preferred (see §7). Note that the desktop
build *could* open a PTY — it must not, because the moment the emulator can be
fed from two kinds of source the Android path stops being the tested one.

**Core principle 2 — sessions are not views.** A `bssh_session` (SSH connection +
emulator state + scrollback) lives in the **`bssh_session_registry`**, independent
of any widget. `bssh_pane_view`/`bssh_terminal_view` **attach to** a session and
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
and it is one of the reasons this table gave for choosing libssh. It is
now **[OPEN]** again, with three routes: send our own periodic global
request through the vendored-and-patched route (§7.1 already establishes
the precedent), switch this one component to libssh2 — which the
`bssh_ssh_session` interface exists precisely to make possible (§4) — or
rely on TCP keepalive plus the network-change trigger, which is slower but
not nothing. **Do not treat the transport choice as settled on the
strength of a row that was wrong.**

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

**Crypto backend: [OPEN]** — default to **OpenSSL** (best-tested with libssh).
mbedTLS/wolfSSL are lighter but add integration risk. Decide at build setup.
Desktop uses the system OpenSSL; only Android needs a cross-compiled copy
pinned in-tree.

---

## 5. Background running & reconnection (a core requirement)

Requirement chosen: **full reconnect / session persistence**, not just a brief
keep-alive.

The *behaviour* is identical on both platforms — sessions outlive the window
they were opened in, and reconnect on their own. Only the mechanism differs:
Android needs a foreground service to be allowed to keep running (§5.1–5.2),
desktop needs a decision about not quitting when the last window closes (§5.5).
`bssh_conn_manager` (§5.3) is shared and knows about neither.

### 5.1 Foreground service (mandatory on modern Android)
A backgrounded app cannot reliably hold a socket open; Android will kill it.
Solution: a **foreground service** with an ongoing notification, started when the
first session connects and stopped when the last disconnects.

**Foreground-service TYPE — [OPEN], important for Play review:**
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
- **Pane navigation shortcuts** once splits exist (§8): e.g. `Ctrl+Alt+Arrow` to
  move focus between panes, `Ctrl+Shift+T` new tab. These must be **intercepted
  before** the byte translation and be **user-remappable**, because any chord you
  steal is a chord the remote shell can no longer receive. Keep the stolen set
  minimal and document it.

### 6.2 On-screen keyboard strategy (touch platforms)
Android soft keyboards don't expose Ctrl/Esc/etc. Plan a **custom on-screen key
row** (a strip of buttons above the soft keyboard) providing: `Esc Tab Ctrl Alt
↑↓←→ Fn - / | ~` and a Ctrl/Alt "sticky modifier" toggle. This is what ConnectBot
/ Termux do and is expected by users.

- Render it as a Qt Widget docked above the input area; feed synthetic key events
  into `bssh_input_router`.
- Provide a settings screen to customize the row. The row layout is a
  **preset-owned setting** (§10.2), so different server types can get different
  key rows.
- **Hidden by default on desktop, but not `#ifdef`-ed out.** Gate it on
  "touch-primary input", not on the OS — a convertible laptop in tablet mode and
  an Android device with a Bluetooth keyboard are the two cases an OS check gets
  wrong in opposite directions. Make it a three-state setting (auto / always /
  never); auto follows the primary input device.
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
| Select text | Click-drag (**done**); double-click for word (**done**), triple-click for line | Long-press then drag handles |
| Copy | `Ctrl+Shift+C` (never bare `Ctrl+C` — that's `SIGINT`) | Selection toolbar |
| Paste | `Ctrl+Shift+V`, middle-click on X11/Wayland | Selection toolbar / key row |
| Scroll | Wheel, `Shift+PgUp/PgDn` | Kinetic touch scroll (`QScroller`) |

- **Paste always goes through the router** so bracketed-paste mode applies. A
  paste that bypasses it will silently break `vim` and any shell with bracketed
  paste enabled.
- **Mouse reporting** (SGR 1006 et al.) matters mainly on desktop, where TUIs
  expect clicks to reach the remote app. When the remote enables it, clicks go
  to the remote — with a modifier (conventionally `Shift`) to override and select
  locally. On touch, keep local selection winning. **Done**, encoding delegated
  to libvterm, which already tracks which mouse modes the remote enabled.
- **Selection is anchored to absolute lines**, not to view rows: it spans the
  fold into scrollback, and scrolling must not drag the highlight across the
  text. It renders by **inverting** the cell rather than tinting it — a fixed
  highlight colour cannot stay legible against 256 possible backgrounds.
- **Multi-line paste is a footgun**: a paste containing newlines executes
  commands. Warn on multi-line paste by default (a setting), on both platforms.

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
| **Focus reporting** (1004) | Tells the remote when the terminal gains/loses focus, so editors can reload changed files | libvterm parses it; needs the window events wired |

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

**Tier 3 — worth having, not yet built.**

| Extension | What it is | Why it is not built yet |
|---|---|---|
| **Synchronized output** (DEC 2026) | Remote brackets a screen update so the terminal paints it in one frame instead of mid-redraw; removes tearing in full-screen apps | Cheap and worth doing — needs the repaint coalescing in §8.4 to land with it |
| **OSC 8 hyperlinks** | Marks a run of cells as a clickable URL | Needs a per-cell link ID in `bssh_screen` and a click target in the view |
| **OSC 52 clipboard** | Remote sets the local clipboard | **Security review first** — a remote host silently writing the user's clipboard is a real hazard, and reading it is worse. Off by default, if offered at all |
| **XTGETTCAP / DECRQM** | Lets a remote query what the terminal supports rather than guessing from `TERM` | Small; pairs naturally with the tier-2 work |

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
apart), and they are **dropped when scrolled off or when the grid shrinks
past them** — there is no image scrollback, and holding every image a long
session ever drew is how a phone runs out of memory.

**[OPEN]** Image scrollback, and a memory budget for it. Also `z < 0`
placement is stored but the renderer draws everything over the text.

**Where this lives in the code:** libvterm handles the sequences it knows
and hands everything else to `vterm_screen_set_unrecognised_fallbacks`, its
documented extension hook. Modes libvterm does not model (kitty flags,
modifyOtherKeys level) are tracked in `bssh_emulator` beside it.

**The hook turned out to have holes, and this is where vendoring earned its
keep.** Two sequences never reach it at all: `on_csi()` rejects any private
leader other than `?` and `>` before dispatch, which drops kitty's pop
(`CSI < u`) and set (`CSI = … u`); and `CSI ? 4 m`, xterm's XTQMODKEYS
query, is swallowed by a DECSGR case for a printer-era sequence no DEC
terminal implemented. Both are *unreachable*, not merely unhandled, so no
amount of application code could have implemented them.

The fix lives in **`vterm-patches/`**, applied to the submodule at build
time by the top-level Makefile — **not** committed into the submodule
itself, because a submodule pinned to a commit that exists only on this
machine is one nobody else can clone. An upstream update that conflicts
fails loudly at the patch step rather than silently dropping the fix, and
`make veryclean` hands the submodule back pristine. Both patches are worth
sending upstream.

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

### 8.3 Tabs and focus
- **Tab bar:** horizontally scrollable; label from **OSC title** with fallback to
  the profile name; a small **status dot** per tab (connected / reconnecting /
  disconnected / bell-rang-while-away). Long-press (touch) or right-click
  (desktop) for close/duplicate/move-to-split; desktop also gets a middle-click
  close and drag-to-reorder.
- **Focus indicator:** the focused pane needs an unmistakable visual — a colored
  border or dimmed inactive panes. Non-negotiable for safety (§6.3), and *more*
  important on desktop, not less: four panes fit on a monitor, and a pointer
  moving over an unfocused pane invites the assumption that it's the live one.
  **[OPEN]** whether desktop should offer focus-follows-mouse; if it does, it is
  off by default.
- **Tap-to-focus** (or click-to-focus) on a pane; keyboard shortcuts for
  pane/tab navigation (§6.1).
- **Dividers:** draggable, double-click/double-tap to equalize. Hit target
  ≥48dp effective on touch; desktop can be tighter but must show a resize
  cursor. Snap divider positions to **whole character cells** so panes never
  render a half column.
- **Per-pane geometry:** each pane computes its own cols×rows from its own size and
  its session's font — panes may legitimately differ.

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

**Touch: a full-bleed host list.** One list, 64dp rows carrying a name and
its `user@host`, one tap to connect, a floating action button to add. No
category tree: a tree of setting categories beside a form of small fields
is a desktop idiom that survives neither a thumb nor a 5-inch screen.
Kinetic scrolling via `QScroller` — dragging a desktop scrollbar with a
thumb is the single most obvious way a Widgets app announces it was not
written for touch. **Still Widgets, still no QML** (§2): the touch
affordances here are a stylesheet, a row height and a gesture recogniser.

**Which one appears is a capability question, not an OS question** —
`front_door_for(touch_primary)`, the same rule as the on-screen key row
(§6.2). A convertible in tablet mode gets the list; a phone docked with a
keyboard and a monitor does not. Only the chosen one is constructed: two
live front doors over one store would be two things to keep in step for no
benefit.

**Both read and write the same `bssh_profile_store`** (§10.6), so a host
saved on one is a host listed on the other, and the preset system (§10)
attaches to profiles rather than to either UI.

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
- **Multiple top-level windows are a desktop-only affordance** and **[OPEN]** for
  v1. If added, a window is just another view onto the registry — the registry
  stays single and process-wide, and a session can be shown in exactly one pane
  at a time. Do not let a second window imply a second registry.
- Rendering N panes means N terminal widgets repainting — keep the glyph cache
  **shared** across panes (§9.5) and throttle repaints to the display refresh
  rather than to data arrival, or a chatty `yes`-style output in one pane will
  starve the others.

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

**[OPEN]** Trim this list at build time — 6–8 user-selectable families plus 2
fallback-only families is plenty. Prioritise: JetBrains Mono, Iosevka, DejaVu,
Hack, Fira, Cascadia as selectable; Noto Sans Mono + Unifont as fallback-only
(never shown in the picker).

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
- **Double-width cells:** implement `wcwidth`-equivalent logic so CJK and many
  emoji occupy exactly two cells, matching what the remote program assumed when it
  laid out the screen. Getting this wrong misaligns every TUI. Note that emoji
  width is genuinely inconsistent across terminals; pick a rule, document it, and
  make it a setting if it causes complaints.
- **Combining marks** should compose into the base cell, not consume a new one.

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
  by the OFL/Apache terms on every platform).

### 9.5 Rendering pipeline
- **Shared glyph atlas/cache across all panes** (§8.4) keyed by (family, size,
  weight, glyph) — critical when 4 panes render at once.
- Snap the cell grid to integer pixel metrics to avoid blurry text and
  accumulating rounding error across 200 columns.
- Respect **display density / device pixel ratio** on every platform and expose a
  font-size control; recompute cols×rows and fire `window-change` on change (§7).
  Desktop adds two cases Android doesn't have: **fractional scaling** (125%,
  150% — where integer cell snapping matters most) and **dragging a window
  between monitors of different DPI**, which must re-lay-out the grid rather
  than scale a stale one. Test both.
- **[OPEN]** Whether to honour the system font-scale accessibility setting. Leaning
  **no** by default for the terminal grid (it would silently change column count),
  but do honour it for app chrome. Offer it as an explicit setting.

### 9.6 CI font validation
Add a build-time script that, for every bundled font, asserts: uniform advance
width, presence of required Unicode blocks (§9.1), required weights, and a
recognised licence file. **Fail the build** if a font doesn't qualify. This is how
"only fonts suitable for a terminal" stays true as the set evolves rather than
being a one-time intention.

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

### 10.2 What lives in a preset (shareable, non-secret)
Terminal type/`TERM` string, colour scheme, font family + size (§9), on-screen key
row layout + keymap (§6.2), on-connect commands (e.g. auto-attach `tmux`),
encoding, scrollback size, cursor style/blink, bell behaviour, keepalive interval,
compression, algorithm preferences (ciphers/KEX/MACs), and default split/tab
behaviour for that host type.

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

### 10.4 Built-in starter presets
Ship a handful of **read-only** presets that users clone rather than edit:
*Default Linux*, *Raspberry Pi / SBC*, *OpenWrt / BusyBox router* (limited `TERM`,
no colour assumptions), *BSD*, *Network switch / serial-ish* (conservative
sequences, no UTF-8 box drawing), *tmux-first* (auto-attach on connect).

Each sets sensible `TERM`, colours, and key-row defaults for that target. These
double as documentation of what a preset can do.

### 10.5 Import / export
- Presets export/import as **JSON** (human-readable, diffable, shareable) with a
  schema version field for forward compatibility.
- **[OPEN]** OpenSSH `~/.ssh/config` import — parse `Host` blocks (`HostName`,
  `User`, `Port`, `IdentityFile`, `ProxyJump`) into host profiles. Not chosen for
  v1, but design the profile schema so it maps cleanly onto those fields; it's a
  high-value v2 feature and retrofitting a mismatched schema is painful. Note it
  gets considerably more valuable now that there's a desktop build, where a real
  `~/.ssh/config` is usually sitting right there — but if it lands, it stays an
  explicit **import**, not a live read of the user's SSH config.
- **[OPEN]** Host groups/folders with inherited settings. Presets cover most of the
  need; add folders only if organising many hosts proves awkward.

### 10.6 Storage
- **SQLite** (via Qt SQL) for profiles, presets, layout state, and known-hosts.
  Same schema and same file format on both platforms.
- Secrets **never** in SQLite in plaintext — platform-wrapped blobs only (§11),
  referenced by ID. The database is therefore *not* portable between devices on
  its own; a secret ID is meaningless outside the machine that wrapped it.
- **Locations come from `QStandardPaths`**, never a hardcoded path:
  `AppDataLocation` for the database, `AppConfigLocation` for settings. That
  lands correctly in the app-private directory on Android and under
  `~/.local/share/beerssh` (XDG) on Linux without either being special-cased.
- **The desktop database is a normal user file** — visible, backed up by the
  user's backup tool, and openable in `sqlite3`. Assume it will be inspected and
  occasionally hand-edited; that's an argument for a schema that's readable and
  for keeping the secret/non-secret split (§10.3) obvious in the table layout,
  not for obfuscating it.
- Schema migrations from day one; users will accumulate profiles they care about.

---

## 11. Security & credential storage

All of it goes behind one interface — `bssh_secret_store` (`store`, `load`,
`erase`, `is_hardware_backed`) — with one implementation per platform, per §3's
core principle 3. Callers get an opaque ID back and never see a key mechanism.

- **Private keys / passwords** stored encrypted; never plaintext in app files,
  `SharedPreferences`, config files, or the SQLite database (§10.6).
  - **Android:** wrap secrets with the **Android Keystore** (hardware-backed
    where available).
  - **Desktop [DECIDED, partly built]:** the platform secret service —
    Secret Service / `libsecret` (GNOME Keyring, KWallet) on Linux, Keychain on
    macOS, DPAPI/Credential Manager on Windows. **Fallback where no such service
    is running** (a headless or minimal session — common enough on Linux to plan
    for, not an edge case): a passphrase-derived key (Argon2id) unlocking a local
    encrypted store, prompted for at app start. **The fallback is built**
    (PBKDF2-HMAC-SHA256 + AES-256-GCM, both OpenSSL's, behind
    `bssh_secret_store`); the Secret Service backend needs `libsecret-1-dev`
    and is not. The fallback was built first deliberately: a headless or
    minimal Linux session has no keyring, so it is needed regardless, and
    it is the case a keyring-only implementation would have left broken.
- **Be honest in the UI about which one is in use.** Keystore-backed and
  passphrase-backed are not equivalent security, and a desktop user with the
  fallback deserves to know their keys are only as safe as the passphrase and
  the machine. Surface it in settings; do not average the two into a reassuring
  "encrypted".
- Support key formats: OpenSSH, PEM; passphrase-protected keys; optional
  **biometric unlock** — `BiometricPrompt` on Android, the platform equivalent
  where one exists on desktop, and simply absent (not faked) where it doesn't.
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
  and defaults to Cancel so leaning on Enter trusts nothing. Keep
  BeerSSH's own known-hosts store — **do not** read or write the user's
  `~/.ssh/known_hosts` on desktop; sharing that file across two tools with
  different trust UIs is how a warning ends up silently pre-accepted.
- **Preset export must be provably secret-free** — see §10.3. Export is more
  exposed on desktop, where the file lands in `~/Downloads` and gets attached to
  a chat window, so the §10.3 type-level guarantee earns its keep there.
- No secrets in logs; scrub on crash reporting.
- Consider an app-lock (PIN/biometric/passphrase) for opening the app, and an
  **idle auto-lock** — more useful on desktop, where an unattended unlocked
  workstation is the realistic threat.
- Hide terminal content from the OS screenshot/thumbnail path: `FLAG_SECURE` on
  Android (as a setting). No portable desktop equivalent exists — don't promise
  one in the UI.

---

## 12. Build, packaging, CI

### 12.1 Builds
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

### 12.2 Desktop packaging — **[OPEN]** which formats to ship
Decide before the first release; all of them want the app ID from §0.1.
- **Linux:** an upstream tarball plus at least one self-contained format
  (Flatpak is the best fit — it also solves the Qt-version-skew problem across
  distros). Distro packaging is welcome but not something to block a release on.
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
- Device/emulator tests cover what only Android can break: IME and hardware-key
  delivery (§6.1), the foreground service surviving screen-off (§5.1), activity
  recreation on rotation.

### 12.4 CI
- **Build both platforms on every change** — Linux host build + all Android ABIs.
  A change that builds on one and breaks the other is the failure mode this whole
  document is arranged to prevent, and only CI catches it reliably.
- Run the host unit tests (§12.3) and the **font validation script** (§9.6).
- Run the Android x86_64 emulator for smoke tests.
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
  permissions. Gate `REQUEST_IGNORE_BATTERY_OPTIMIZATIONS` behind explicit user
  action or drop it.
- Store listing per §0.1: title `BeerSSH — SSH Terminal`, package
  `se.vibes.beerssh`, JuiceSSH-successor framing in the description body.
- Content rating: no alcohol/mature content in the app; expect *Everyone*.
- Large-screen quality: Play surfaces tablet/foldable readiness — the §8 adaptive
  layout work directly helps store presentation.
- Privacy policy URL.

---

## 14. Milestones / phased plan

**Every milestone lands on both platforms before the next one starts.** Not
"desktop first, Android later" as phases — per-milestone, desktop is where a
feature is written and Android is where it's finished. A milestone with a green
desktop build and an untried Android build is not done, and the debt from
deferring that compounds fastest in exactly the two riskiest areas (input,
background).

1. **M0 – Skeleton & toolchain.** Qt Widgets app named per §0.1
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
5. **M4 – Platform layer & background.** `bssh_platform` split; Android
   foreground service + JNI bridge + notification, keeping a session alive
   screen-off/backgrounded; desktop tray/stay-resident behaviour and the
   close-with-sessions prompt (§5.5).
6. **M5 – Reconnection & network awareness.** Connection FSM, keepalive, network-
   change reconnect (Android callbacks + desktop `QNetworkInformation`, including
   suspend/resume), backoff, status UI. **Partly built:** `bssh_conn_manager`
   per session, exponential backoff capped and jittered, network-change
   retry, user-disconnect suppression, and an attempt limit so a dead host
   stops waking the radio. Still open: SSH-level keepalive to detect a dead
   socket quickly, and the reconnect status in the UI.
7. **M6 – Security & profiles.** `bssh_secret_store` with both backends, key
   auth, host-key verification, host profiles, biometric unlock where available.
   **Started:** public-key authentication is tried before any password
   (agent, then the default identities), so a key-authenticated session holds
   no credential at all and can reconnect on its own; passphrase-protected
   keys are offered an unlock prompt before any password, since for a
   key-only server a password prompt is not a fallback but a prompt that
   cannot succeed; and the trust-on-first-use prompt is built (§11). Still
   open: the libsecret backend, and biometric unlock. The store IS wired
   in: a password can be remembered per host (off by default), the profile
   row carries a non-secret `has_password` flag so the app knows whether
   asking for the store's passphrase is worth it, and a remembered password
   is replayed on reconnect instead of being held in memory all session.
8. **M7 – Sessions & tabs.** `bssh_session_registry` + session/view separation,
   tab bar, OSC titles (and desktop window title), per-tab status, layout
   persistence. *(Do this before splits — the registry is the foundation.)*
   **Mostly built:** registry, `bssh_session` owning transport + emulator +
   router, a tab bar rebuilt from the registry, and titles falling back
   OSC → profile name → target. Layout persistence and per-tab status dots
   are still open.
9. **M8 – Adaptive splits.** `bssh_layout_manager`, size classes, 2/4-pane
   splits, focus indication, collapse-to-tabs on shrink, per-pane
   `window-change`. Desktop window-drag resizing exercises this hardest — use it.
   **Mostly built:** size classes, pane limits, the collapse/restore rule with
   the layout intent remembered, per-pane geometry, the focus border, pane
   navigation chords, and dividers snapping to whole cells. The arrangement
   is persisted (profile ids, pane count, active tab) and the front door
   reopens with the last-active host selected. **[OPEN]** Reopening every
   remembered host at once needs key authentication (M6) to be anything
   but a queue of password prompts.
10. **M9 – Fonts.** Curated set, fallback chain, wide-char handling, shared glyph
    cache, asset-pack delivery (Android) / full set (desktop), CI validation
    script, fractional-scaling and multi-monitor DPI checks.
11. **M10 – Preset system.** Preset/profile layering, built-in starter presets,
    JSON import/export, propagation UI.
12. **M11 – Settings, polish, accessibility.**
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
- Crypto backend: OpenSSL (default) vs mbedTLS/wolfSSL.
- libssh (LGPL, dynamic link) vs libssh2 (BSD) — licensing may decide it, and it
  is the Android build that decides it (§4).
- Foreground-service type: `connectedDevice` (start here) vs `specialUse` (fallback).
- Min SDK: 26 vs lower.
- Desktop secret-store backend and its no-keyring fallback (§11).
- Desktop packaging formats, and whether Flatpak is the primary Linux artifact
  (§12.2).
- Inhibit sleep on desktop while a session is connected? (Leaning no; per-session
  toggle instead — §5.5.)
- Multiple top-level windows on desktop? (§8.4.)
- Offer system-installed monospace fonts on desktop? (Not v1 — §9.4.)
- Desktop `ssh-agent` support (§11) — v2, but the secret-store interface should
  already accommodate a backend that never releases key material.
- Focus-follows-mouse on desktop? (Off by default if offered — §8.3.)
- Final font shortlist (6–8 selectable + 2 fallback-only) and which is the phone
  default (JetBrains Mono vs condensed Iosevka).
- Honour system font-scale for the terminal grid? (Leaning no; setting instead.)
- `~/.ssh/config` import — v2, but schema must be compatible now.
- Host groups/folders in addition to presets?
- Bundle an optional custom soft-keyboard? (Lean no for v1.)
- Persist scrollback across restarts?

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
  suffix (`bssh_pane_geometry_t`); classes don't need it.
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
that *is* wanted lives in `tools/` (`tabify.py` + `check-indent.sh`, carried over
from `fuzzypickles`) and runs as `make style`, in CI alongside both builds.

### 18.4 Filenames

Lowercase `snake_case` for everything this project names itself — sources,
headers, docs: `terminal_view.cpp`, `input_router.cpp`, `platform_android.cpp`,
not `TerminalView.cpp`. The exception is a name a tool won't accept lowercased:
`Makefile`, `AndroidManifest.xml`, and similar. qmake imposes no such constraint,
so `beerssh.pro` is simply lowercase.

### 18.5 Commit conventions

- **No docs-only commits.** Documentation changes ride along with the code commit
  they describe. (This document's own revisions are the exception, since there is
  no code yet.)
- **No AI-attribution trailers** in commit messages — no `Co-Authored-By:` for a
  tool, no generated-by footers. Messages end at their real content.
- Commit messages describe the change and why it was made, in the imperative.
