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
**`vterm-patches/0004`** hands the query to the fallback in its own shape
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

The fix lives in **`vterm-patches/`**, applied to the submodule at build
time by the top-level Makefile — **not** committed into the submodule
itself, because a submodule pinned to a commit that exists only on this
machine is one nobody else can clone. An upstream update that conflicts
fails loudly at the patch step rather than silently dropping the fix, and
`make veryclean` hands the submodule back pristine. All three patches are
worth sending upstream.

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

**Rotation itself is checked on a device, and checked from the far end.** A
session was opened on an emulator, a unique marker left in its scrollback, and
the device rotated to landscape and back. The process is the same one
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
  **[OPEN]** whether desktop should offer focus-follows-mouse; if it does, it is
  off by default.
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
  paths are relative to the *binary* and the test binary sits in `build-tests/`
  — so it looked one level above the checkout, found nothing, and drew the
  terminal in whatever monospace the machine had. CJK came out as tofu in a
  picture of a renderer that handles it. A shot of a renderer is uninterpretable
  without knowing what rendered it.
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

**Touch: a full-bleed host list**, and its editor is a **page, not a
dialog**. The first Android build used a `QInputDialog` to add a host, and
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

**Which one appears is a capability question, not an OS question** —
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

**[DECIDED]** Trimmed to exactly the priority list above: **JetBrains Mono,
Iosevka Term, DejaVu Sans Mono, Hack, Fira Code, Cascadia Code PL** selectable,
**Noto Sans Mono + GNU Unifont** fallback-only and never shown in the picker.
Source Code Pro, IBM Plex Mono and Inconsolata are dropped — not for any fault,
but because a picker of eight near-equivalent faces is a picker nobody reaches
the bottom of, and each one is a licence to track and megabytes to ship.

The set lives in one table in `bssh_font_catalog` and is fetched by
`tools/fetch-fonts.sh`, which pins a version *and* a SHA-256 per family. The
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

**Contrast is a property of the reader, not of the palette.** Two things were
wanted and they pull against each other: schemes faithful enough that a user can
check one against its source, and output legible enough to read. Editing the
numbers buys the second by destroying the first — and the argument that protects
Solarized from us protects xterm too.

So **every scheme is reproduced exactly, including its unreadable entries**, and
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
  (`tests/emulator_bench.cpp`, §12.3), which is where every MB/s figure below
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
  rather than ours and is fixed in `vterm-patches/` (§7.1) — it is invisible
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
  **Checked against a real server** (§12.3): `tools/check-ssh.sh` uses the one
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
- `tools/migrate-db-to-ini.py` converts a database written by an earlier build.
  A script rather than app code, because the app no longer links Qt SQL at all
  and carrying a database driver forever to read one file once is a poor trade.

---

## 11. Security & credential storage

All of it goes behind one interface — `bssh_secret_store` (`store`, `load`,
`erase`, `is_hardware_backed`) — with one implementation per platform, per §3's
core principle 3. Callers get an opaque ID back and never see a key mechanism.

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
    **No user authentication on the key.** Biometric release is §11's [OPEN]
    item and needs a decision about what happens on a reconnect at 3am with
    nobody holding the phone; requiring it here would make that decision by
    accident.
    **Verified on a device**, since none of this can be exercised off one: a
    password stored with "Remember" survives a full process restart and is
    submitted without prompting; the plaintext does not appear in the file; and
    a single flipped byte — what a store restored from another phone's backup
    looks like — is rejected by the GCM tag, so the app asks again instead of
    handing a caller garbage.
  - **Desktop [DECIDED, partly built]:** the platform secret service —
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
- **The user's `~/.ssh/known_hosts` is read, never written.** The original rule
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
  **[NOT BUILT.]** Nothing in the tree sets it, and there is no setting for it.
  Recorded rather than quietly implemented because it cannot be verified from
  here: the whole point is what the OS puts in the recents thumbnail and lets a
  screenshot capture, which only a device shows. Writing the JNI call and
  claiming the property on the strength of it compiling would be exactly the
  kind of unbacked assertion §12.3 exists to prevent. It is a small piece of
  work — one window flag and one entry on the *This device* page — and it wants
  a device to land against.

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

### 12.2 Desktop packaging — **[DECIDED]** tarball, `make install`, and a
**`.deb`**; Flatpak and the rest deferred
All of them want the app ID from §0.1.

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

**`make apk` names the artifact and says who signed it.** Gradle leaves it at
`android-build/build/outputs/apk/debug/android-build-debug.apk`, which tells
nobody which app, version or ABI they are holding; it comes out as
`beerssh-<version>-<abi>.apk`. Debug-signed by default — installs anywhere,
which is what an evaluation build wants — and **said out loud**, because a
debug-signed APK mistaken for a release is a bad afternoon: Play rejects it and
it can never replace a properly signed install. `BSSH_KEYSTORE` and
`BSSH_KEY_ALIAS` produce a real signed release.
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
- **Benchmarks are built by `make test` and run by `make bench`.** Every
  throughput figure in §9.5 was measured with a scratch harness that was then
  thrown away, so each one cited a program nobody could re-run — a claim
  wearing the clothes of evidence. `tests/emulator_bench.cpp` is that harness,
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

### 12.4 CI
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
   suspend/resume), backoff, status UI. **Partly built:** `bssh_conn_manager`
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
7. **M6 – Security & profiles.** `bssh_secret_store` with both backends, key
   auth, host-key verification, host profiles, biometric unlock where available.
   **Started:** public-key authentication is tried before any password
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
   **Mostly built:** registry, `bssh_session` owning transport + emulator +
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
   is persisted (profile ids, pane count, active tab) and the front door
   reopens with the last-active host selected. **[OPEN]** Reopening every
   remembered host at once needs key authentication (M6) to be anything
   but a queue of password prompts.
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
    from `assets:/fonts` on a device. Still open: the shared glyph atlas
    (§9.5 — the renderer caches per-glyph *family* lookups and QFonts, not
    rasterised glyphs — see below), Play asset-pack delivery and subsetting
    for the rest
    (Iosevka alone is 42 MB, so the condensed face cannot be the compact
    default until it ships), and the fractional-scaling and multi-monitor
    DPI checks — **the fractional-scaling half is done**: the cell now snaps
    to whole *device* pixels rather than whole logical ones, because at 150%
    a 9-pixel cell is 13.5 device pixels and box-drawing characters stop
    meeting at the cell edge. TUI borders came out seamed at every other
    column; solid at 100%, visibly broken at 150%, found by rendering both
    and looking. Multi-monitor DPI (dragging a window between screens of
    different scaling) is still untested. The **font picker is built** and is one widget on both
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
    **The settings now act.** `term` reaches the PTY request, `compression`
    the handshake, `keepalive_seconds` the transport, `scrollback_lines` the
    emulator, `cursor_style`/`cursor_blink` the renderer, `bell_enabled` the
    tab marker, and `on_connect_command` runs on every connect — including
    reconnects, which is the one that matters, since §5.3's answer to "a
    reconnect is a new shell" is attaching to tmux. Still inert:
    nothing — every preset field now reaches something that uses it. **`colour_scheme` is built** (§10.2):
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
