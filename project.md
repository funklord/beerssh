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
- Unbounded tmux-style split trees. Panes nest as they are dropped (§8.1,
  since 2026-09-25), but the count stays capped by width.
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

**What each route costs -- measured 2026-09-30, for the holder to decide.**
Asked for as a write-up rather than a change. Versions measured: libssh 0.11.5
(this tree's, static on Android from `tool/build-deps-android.sh`, the system's
shared library on desktop) and libssh2 1.11.1 (Debian's, headers and library on
this machine).

- **Algorithms: parity, and this table's older rows undersold libssh2.** Asked
  of each library, not recalled: libssh2 lists curve25519 key exchange, ed25519
  and certificate host keys, chacha20-poly1305 and AES-GCM, the ETM MACs and
  strict key exchange (`libssh2_session_supported_algs`); libssh's library
  carries the same names (read from its strings, with `aes256-ctr` as the
  control both must contain). **Neither** has post-quantum key exchange
  (`sntrup761x25519`, `mlkem768x25519`); servers still offer curve25519.
- **Every capability this tree uses has a libssh2 counterpart** (it calls 39
  libssh functions and sets 9 options). Checked in `libssh2.h` for the ones
  that could have been missing -- agent (`libssh2_agent_*`), key files
  (`libssh2_userauth_publickey_fromfile_ex`), known hosts
  (`libssh2_knownhost_checkp`), SHA-256 fingerprints
  (`LIBSSH2_HOSTKEY_HASH_SHA256`), direct-tcpip for the jump host
  (`libssh2_channel_direct_tcpip_ex`); the rest are ordinary channel calls
  (pty, window change, exit status, windows). Two do more work by hand: `ssh_userauth_publickey_auto` (agent, then the default keys) becomes a
  loop over the agent's identities and then over key files; and libssh2 is
  non-blocking throughout, where the worker here is blocking-and-polled.
- **Keepalive gives neither library an edge -- measured and read 2026-09-30.**
  §4's table credited libssh2 with a keepalive libssh lacks, and it was read as
  a real round trip. `libssh2_keepalive_send` returns 0 or a socket error, by
  its own header, and nothing about whether a reply came -- so like
  `ssh_send_ignore` it proves only that bytes left. libssh 0.11 does export
  `ssh_send_keepalive` (declared in `server.h`), and against a throwaway
  loopback sshd it sent `keepalive@openssh.com` with want_reply and waited for
  the answer (2.5 ms, then 0.2 ms; the server logged it) -- but with the
  server's session process stopped it **blocked the caller for 5.0 s, the
  session timeout, and then returned `SSH_OK`**, still connected. Its result
  cannot tell a live peer from a dead one either, and it would block the
  worker `do_send` was rebuilt never to block. In both, dead-peer detection is
  TCP's, as §4 already says; `ssh_send_ignore` plus TCP keepalive stays.
- **Size is not a factor**: `ssh_`-named symbols are about 145 KB of the
  Android library's ~4.8 MB of sized symbols (a name-based estimate).
- **What switching costs**: `ssh_session_libssh.cpp`/`.h` and
  `jump_tunnel.cpp`/`.h`, about 2,160 lines, rewritten against a different
  threading model, plus `tool/build-deps-android.sh`, `debian/control`, the CI
  image, and the live-SSH tests re-proved. The `bssh_ssh_session` interface
  keeps it out of everything else, as §4 intended, and the fake session the
  emulator tests use is unaffected.

**So the choice is about the licence and nothing else**, and there are three
routes, not two:

1. **libssh, static (today).** LGPL-2.1 §6 then asks that a user be able to
   relink the app against a changed libssh -- in practice the app's object
   files, or its source, made available.
2. **libssh, as a shared library inside the APK.** `BUILD_SHARED_LIBS=ON`, and
   `libssh.so` ships beside the app's own library; the build change is small.
   It is the arrangement LGPL is written around, though whether a library a
   user cannot swap without re-signing the APK satisfies §6 is debated, and not
   for this document to settle.
3. **libssh2 (BSD-3).** No relinking obligation, at the cost above.

How much 1 and 2 cost depends on the terms BeerSSH itself ships under, which
is the copyright holder's to settle, not this section's. The technical
position is only that nothing but the licence favours switching.

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

**The service must not outlive the UI DYING, only the UI going to the
background — [FIXED 2026-09-04].** It returned `START_STICKY`, and the
comment defending that argued itself out of its own conclusion: it conceded
that the SSH sessions do not come back with the service, "so this is about
the service's own lifecycle". A service whose only job is to keep a process
alive for sessions that no longer exist has no lifecycle worth preserving.

What sticky actually bought, measured on the device: the Qt main loop thread
aborted (`SIGABRT`, abort message *"Failed to acquire deadlock protector for
`QAndroidPlatformOpenGLWindow::eglSurface()`"*), Android recreated the
service in a **fresh process with no Qt in it**, and that process then held
the app's identity. The launcher could no longer bring BeerSSH up — a splash
screen, then `Not drawing due to not visible` — and there was no crash
dialog, because from Android's point of view the app was running. It stayed
that way for twenty-six minutes until an `am force-stop`. The notification
said *Connected* throughout, because `s_status` is a static that a new
process initialises to that word.

**From the user's side this is indistinguishable from "the app will not
start"**, which is how it was reported, and it is the worse failure of the
two on this page: a session that dies is an inconvenience, an app that
cannot be started again is uninstall-and-reinstall territory for anyone who
does not know about force-stop.

Two changes. `START_NOT_STICKY`, so a process that dies stays dead and the
next launch gets a clean one. And a guard on a **null intent**, which is how
a system-restarted service is told it was Android's idea rather than the
app's: it honours the `startForeground` contract above — that applies to
this start like any other — and then stops itself. The guard is the belt to
the other's braces, since an OEM or a redelivery could restart a service
that is no longer sticky.

**Verified by compiling it, which is more than this file used to get.**
`android/src/se/vibes/beerssh/BsshService.java` is built only by the Android
build, so no routine gate here sees it — the same hole §11 records for
`secret_store_keystore.cpp`. It compiles off-device against the real
`android.jar` and Qt's own `QtActivity`:

    javac -cp $ANDROID_SDK_ROOT/platforms/android-36/android.jar:\
              $QT_ANDROID_ROOT/jar/Qt6Android.jar:\
              build-android-arm64-v8a/android-build/build/intermediates/\
              javac/debug/compileDebugJavaWithJavac/classes \
          android/src/se/vibes/beerssh/BsshService.java

and the check was shown capable of failing before it was believed: the same
command on a copy with `START_NOT_STICKY` misspelled exits 1 naming the
symbol.

**~~That makes a `make check-java` gate cheap and obvious, and it does not
exist yet.~~ It exists** (2026-09-04). It compiles all three files under
`android/src` with no NDK and no device: `android.jar` from the newest
installed SDK platform for the platform classes, `Qt6Android.jar` for the
bindings, and Qt's own `src/android/java/src` on the sourcepath for
`QtActivity`, which the service names — so it works from a clean checkout
rather than only from a tree that has already built. `-implicit:none` keeps
Qt's sources out of the output.

**It refuses rather than skipping** when the SDK or the kit is absent, and
that is the one design decision in it. A gate that needs the thing it
checks for is weakest on the machines that lack it, and one that skips
quietly there is a green light nobody earned; whoever has no SDK does not
run the target. Three controls, all run: a deliberate compile error exits 2
naming the symbol, a `QT_ANDROID_ROOT` that is not a kit refuses, and an
`ANDROID_SDK_ROOT` with no platforms refuses. An UNSET kit is auto-detected
by `tool/android.mk` and correctly still passes — worth recording, because
it was written up as a failed control before the reason was read.

`secret_store_keystore.cpp` is still the other file nothing compiles, and
this does not reach it: it is C++ needing QJniObject, which Qt ships only
for Android.

**What is NOT verified is the behaviour.** Reproducing it needs the Qt main
loop to abort again, and no attempt was made to provoke it a second time. So
the fix rests on the mechanism being read correctly from the log, not on
having watched the app recover.

**~~The EGL deadlock abort is its own open question and is not addressed
here.~~ Its mechanism is established (2026-09-04), and it is upstream
Qt's rather than ours.** `QAndroidPlatformOpenGLWindow::eglSurface()`
guards surface creation with `AndroidDeadlockProtector` — a single
process-wide `QBasicAtomicInt`, acquired by one non-blocking
`testAndSetAcquire`, with no timeout and no per-window scope
(`qjnihelpers.cpp:446`). Inside the guard Qt calls `createSurface()` and
then waits on a condition until the **Android UI thread** delivers the
Surface back. Every other holder of that same flag — the IME's
`runOnQtThread()` at 23 call sites, accessibility's `runInObjectContext()`,
native drag, permissions — takes it ON the Android UI thread and holds it
across a blocking call INTO the Qt thread. So it is a genuine A-waits-B
cycle, and Qt turns a hang into a crash deliberately.

Three facts decide what we can do about it, and none of them is ours to
fix:

- **The `qFatal` is itself an upstream regression.** Until qtbase
  `d885a69` (2025-08-21) the same failed acquisition returned silently.
  Every other user of the flag still returns gracefully; this call site
  alone aborts.
- **We are already past the "fixed" versions.** The caller-side fixes for
  QTBUG-140674 and QTBUG-141579 are present in the 6.12.0 tree we build
  against — verified by reading `qandroidinputcontext.cpp:81` and
  `androidjniaccessibility.cpp:82`, not by trusting a changelog — and it
  crashed anyway. That is QTBUG-144207 exactly: filed against 6.10.2, P1,
  still open, reported across several vendors, not reproducible on demand.
- **Our own widget churn cannot cause it directly.** `src/` contains no
  `winId()`, no `WA_NativeWindow`, no `QOpenGLWidget` and no
  `createWindowContainer`, so the terminal views share the top-level's
  backing store and a pane rebuild creates no Surface at all.

**But there is a traceable aggravating route, and it is ours.**
`rebuild()` → `focus_session()` → `setFocus()` → `focusInEvent` →
`request_input_panel()` → `inputMethod()->show()`. Each IME show is a burst
of Android-UI-thread traffic holding the flag. Under the code before
§8.4's resize fix, a keyboard opening rebuilt the panes on every animation
frame, so that burst fired per frame while repaints needed the same flag —
the maximal version of the race. The fix makes it strictly less likely and
cannot close it.

**The next step is one line of logcat, and it is cheap.**
`AndroidDeadlockProtector::acquire()` emits, immediately before the fatal,
a warning reading *"...while already locked by '&lt;HOLDER&gt;'"* — which names
which of the four had it. That string has exactly one producer in the whole
installed Qt tree, so any occurrence of it is this failure and nothing
else. It was not captured for the one instance seen here, because only the
crash buffer was saved and the warning goes to the main one. Capture
`adb logcat -v threadtime` to a file BEFORE provoking it.

**The holder is named in a sibling's confirmed instance, and it is
accessibility.** hydra reports one from 2026-08-26 — a Galaxy Note 9 with
`com.jamworks.bxactions` running and `accessibility_enabled=1`, any menu tap
killing the app within a second, reproducibly — carrying the line this
entry says to go and capture:

    Abort message: 'Failed to acquire deadlock protector for
    QAndroidPlatformOpenGLWindow::eglSurface().
    ... while already locked by 'QtAndroidAccessibility::runInObjectContext()'.

That is their measurement, on their device, in their app. **It does not
establish what held the flag in OUR abort**, which was a different app on a
different phone during IME animation, and the four candidates are still
four. What it does establish is that this holder reaches the fatal branch
in practice rather than in principle, which is more than the source alone
said.

**The consequence for this tree is specific, because we asked for it.**
`bssh_terminal_accessible` calls `QAccessible::installFactory`
(`terminal_accessible.cpp:65`), so `runInObjectContext()` is live in this
process whenever any accessibility service is running — which is the
condition hydra's instance needed. §14's M11 exposed the terminal, its
scrollback and its selection to screen readers on purpose, because a widget
that paints characters was otherwise invisible to them.

So the population is not "users with an unusual gesture app". **It is users
who need the screen described to them**, and for them the failure is not a
degraded terminal but an app that dies on a menu tap. That is the shape
hydra's own record already names for a related crash — narrowing a
condition can make a finding worse, because TalkBack is an accessibility
service — and it arrives here through the accessibility work rather than
in spite of it.

**One negative run, recorded with its limits because a bare "no crash"
would be worth nothing.** hydra turned TalkBack on against their own app —
three services registered, a page loaded, a `-v threadtime` capture with
zero `deadlock protector`, `Abort message` or `FATAL` lines. They decline
to call it a refutation and so does this entry: different device (Fold 3,
not the Note 9 of the confirmed instance), different Qt, and **no menu was
ever opened** before the handset disconnected — while the instance that
aborted was a menu tap. So the run does not touch the case. It is here so
nobody repeats it believing it settled something.

**And the sibling comparison cuts the other way, which is worth knowing
before quoting "same population".** hydra has no accessibility code at all
— not one `setAccessibleName` anywhere in its `src/`. A screen reader gets
whatever Qt Widgets supplies by default, so its icon-only toolbar buttons
are unlabelled and much of that browser is unnavigable before a menu is
ever reached. The population is the same; the failures are not. This tree
made its terminal navigable deliberately, which is precisely why the abort
reaches the users M11 was written for.

**Not established, and recorded as such:** whether our instance had this
holder; whether the accessibility bridge failing to get a surface explains
the stale `uiautomator` reads hydra spent an hour on (they record that as
open too — same bridge, same surface, one path returning quietly and one
fatal); and whether anything in this app's own accessibility code changes
the odds. Nothing here is a defect in `bssh_terminal_accessible`: the flag,
the wait and the `qFatal` are all Qt's, and an app that installs an
accessibility factory is doing the thing Qt asks of it.

**What it changes about what to do next.** Capturing the holder line stops
being a nice-to-have: if ours is also `runInObjectContext()`, then this
tree's exposure is proportional to how many of its users run a screen
reader, and that is a different argument for chasing an upstream bug than
"it crashed once". `adb logcat -v threadtime` to a file, before provoking
it, and with TalkBack deliberately on as one arm.



**A SECOND unstartable app, measured 2026-09-07, and the process was
never a service restart.** Reported as *"Beerssh cannot be started on my
phone now, it hangs at the logo screen"*, on the Note 9 carrying the
2026-09-07 00:39 build. The framework's own usage-event log is what
settles the shape, and it is worth knowing that this record exists,
because logcat had already rolled and this had not:

    time="2026-09-07 00:39:52" ACTIVITY_RESUMED   QtActivity
    time="2026-09-07 00:39:59" FOREGROUND_SERVICE_START  BsshService
    time="2026-09-07 08:37:11" ACTIVITY_PAUSED
    time="2026-09-07 08:37:12" ACTIVITY_RESUMED
    time="2026-09-07 08:40:02" ACTIVITY_PAUSED
    time="2026-09-07 08:40:04" ACTIVITY_STOPPED
    -- nothing at all until an am force-stop at 15:09 --

    adb bugreport, then grep the usage events out of dumpstate.txt

So the app RAN, for eight hours, with a live session and the foreground
service. What failed is everything after `ACTIVITY_STOPPED`: not one
launch between 08:40 and the force-stop produced an `ACTIVITY_RESUMED`,
which is what a splash screen that never becomes a window looks like from
the framework's side.

The process was pid 12035, **started 2026-09-06 00:52** -- so it predated
the 00:39 install by a day, survived it, and served the activity
afterwards, meaning the user spent that morning running the previous
day's code. It was still alive at 15:09 with 30 threads, all sleeping,
and `am_finish_activity ... proc died without state saved` on task 324
when it was killed, which is what proves it owned the activity rather
than merely sharing the package's name.

**What that rules out.** It is not the START_STICKY resurrection above:
the process was the original one, with Qt in it, not a service recreated
in a fresh process -- that fix landed 2026-09-04 and is not implicated.
It is also not the four ANRs on 2026-09-05 that a first reading of this
attributed to beerssh; **the traces name `se.vibes.bbq_predictor:fetch`
in all of them** and contain the string "beerssh" zero times. No
tombstone was written for beerssh at any point; the newest on the device
is from 2026-08-27.

**What is established, and what is not.** Established: a beerssh process
can outlive its own window, hold the app's identity together with the
foreground-service notification, and leave the launcher unable to bring
the app up until somebody force-stops it -- the same USER-VISIBLE failure
the entry above records, by a different route. Not established: why the
window stopped coming back at 08:40. There is no crash, no ANR and no
tombstone to point at, and the log that would have carried the
`AndroidDeadlockProtector` holder line had been cleared by the session
investigating -- which is the mistake to avoid next time. **Capture
`adb logcat -v threadtime` to a file BEFORE clearing anything**, as the
EGL entry above already says, and the usage-event dump is the record that
survives when logcat has not.

**One CANDIDATE mechanism, named as a candidate and with the experiment
that would settle it, because an unexplained failure keeps somebody
looking and a comfortable story stops them.** The process staying alive
at 08:40 was CORRECT: a session was live and the foreground service
exists precisely so the UI can go to the background without dropping it
(sec 5.1). What failed is the return trip. The activity record still
existed -- killing the process named task 324 -- so Android had somewhere
to deliver a resume and the app did not take it, which is the shape of a
Qt-thread/Android-UI-thread deadlock rather than of a dead process. The
EGL entry above establishes that cycle exists in this Qt and that ONE of
its call sites turns it into a `qFatal` while every other returns
silently; a silent return is a hang, and a hang here looks exactly like
this. **This is a candidate and not a finding** -- nothing was captured
that names a holder, and the four flag-holders remain four. What would
settle it: `adb logcat -v threadtime` to a file left running, and, at the
moment the launcher fails, `adb shell kill -3 <pid>` to dump the Java
thread stacks into `/data/anr`, which `adb bugreport` can then retrieve
even unrooted. Neither was available this time, because the process had
already been force-stopped.

**The cure is `adb shell am force-stop se.vibes.beerssh`**, and it worked
here. Nothing in the app can do it for the user, which is what makes this
worth its own entry rather than a line in the one above: two distinct
mechanisms now produce an app that cannot be started, and neither is
recoverable from inside the app.

**REPORTED AGAIN 2026-09-17, on closing the LAST terminal. Reproduced on
the device the same day -- the STATE, not yet the transition.** "Close
the last terminal, the app will not start, just hangs, and the
notification still reports those terminals." Same user-visible failure as
the two incidents above. The clean path reaches the service-stop:
`registry->close()` -> `on_registry_changed()` -> `live_count()==0` ->
`set_session_count(0)` and `set_keep_alive(false)`; and the window-close
branch in `session_closing` is gated behind
`opens_a_local_shell(is_touch_primary())`, false on Android. So the
service-stop IS issued on a clean last-close, which the device confirms.

**What the device showed, and it corrects the theory this entry first
carried.** On plugging in, a beerssh process was ALREADY hung from an
earlier close: alive (not dead), foreground, holding the app token, and
launching it drew only the SPLASH -- the "just hangs". The service record
was the finding: `isForeground=true` with a STACK of `act=...STOP` intents
QUEUED and unprocessed. So `stop()` had run and been delivered, more than
once -- the notification kept its old count not because the stop was never
issued but because `onStartCommand` never ran to process it. And
`onStartCommand` runs on the MAIN (UI) thread. The main thread is stuck.
That is the whole mechanism: a hung main thread cannot process the STOP,
so the foreground service stays up, and a foreground service is exactly
what keeps Android from reaping the hung process -- so it lingers holding
the app identity, the notification frozen, and every relaunch lands on the
stuck process. `am force-stop` clears it (service 0, process gone, next
launch clean), which is why that is the only cure. The earlier "the
process died before the service-stop" was wrong: it does not die, it
hangs, and the service it cannot stop is what makes the hang permanent.

**The ACCESSIBILITY narrowing was wrong, measured.** This entry first
reasoned the deadlock flag-holder must be accessibility, since last-close
rebuilds with an empty pane set and fires no IME show. But the device had
NO accessibility service enabled (`enabled_accessibility_services` null,
`accessibility_enabled` 0) when it hung. So accessibility is not required,
and the sibling instance that named it (hydra's) was a different route.
Do not send the next person to turn TalkBack on.

**It is a rare race, which is why a fix must not be guessed.** 26
scripted open/close cycles -- plain, and with background+relaunch between
them to force surface recreation -- did NOT reproduce the hang; every one
took the clean path (service to 0, front door). The one captured instance
was the pre-existing hung process. The likely trigger is the
`eglSurface()` deadlock on SURFACE CREATION (foreground-resume, fold,
rotate) rather than on the pane rebuild, which creates no Surface -- and a
fold on this device is a hardware hinge no script can drive. Two live
observations point the same way: a backgrounded beerssh spins
`makeCurrent(): no EGLSurface, likely Surface destroyed by Android`
roughly every 14s (the render thread stays hot while not visible, which is
the precondition the deadlock needs), and the one system-level ANR near
the hang was the camera timing out `Waited 10001ms for
FocusEvent(hasFocus=false)` -- consistent with a hung beerssh window not
releasing input focus, though the trace is `/data/anr` at mode 600 and
unreadable without `adb bugreport`.

**What is still owed before a fix.** The exact Qt thread stack -- the
main thread's block and the flag holder -- was not captured: the hung
process could not be `kill -3`'d as shell, and no fresh hang was
provoked. Get it by `adb bugreport` while a hung instance exists, or by a
physical fold/unfold with a session live and `logcat -v threadtime`
running. The fix direction the evidence already supports is that a
foreground service must not be able to keep a HUNG process alive -- but
stopping the service earlier does not help, because the device showed the
STOP already issued and unprocessable on the stuck main thread. So the
lever is more likely to stop the backgrounded render activity (no repaint
while not visible, so no surface race on resume) or to let the service
self-terminate when its process stops drawing. Both are upstream-adjacent
and want the stack first. Do not commit a fix before it -- this family has
cost two incidents, and this entry has already corrected itself once.

**The fix that the evidence supports is a hang watchdog, and it is
DESIGNED here 2026-09-18 rather than built, because its one risky failure
cannot be validated without the deadlock.** A fold session on the device
2026-09-17 did not reproduce the hang in half an hour of folding,
backgrounding and resuming a LOCAL shell -- so it is rare, and probably
wants a live SSH session's keepalive/reconnect churn during the surface
swap, which a local shell has none of. Working it from the evidence
instead:

- **The only lever is killing the stuck process.** The service cannot be
  stopped from within -- `onStartCommand` runs on the Android UI thread,
  which the deadlock has blocked, which is why the STOP intents queued
  and never ran. Nothing on that thread executes. So the process must be
  terminated from ANOTHER thread, and `_exit()` from a watchdog thread
  does it: the foreground service dies with the process, START_NOT_STICKY
  keeps Android from recreating it, and the next launch is a clean
  process. It turns "permanently unstartable until force-stop" into
  "self-heals on relaunch", and a `kill(getpid(), SIGQUIT)` just before
  the exit finally captures the holder line the manual repro could not.

- **The gate is the whole risk, and its two failures are not
  symmetric.** The watchdog must fire only when the app is FOREGROUND and
  frozen, never when it is merely backgrounded -- Qt suspends its event
  loop on background, so a stale heartbeat there is normal, not a hang. A
  gate too STRICT misses the hang: no worse than today. A gate too LOOSE
  kills a live backgrounded session: a regression worse than the rare
  bug. So the gate must be the Android lifecycle's own resumed/paused
  state (an `ActivityLifecycleCallbacks` flag read by the watchdog), not
  Qt's `applicationState`, because the deadlock happens DURING the resume
  and the timing of Qt's state signal relative to it is exactly what
  cannot be checked without reproducing it.

- **What stays unvalidated even when built: that `_exit()` cleanly
  removes the service and the next launch is clean.** That is the
  recovery, and only a real deadlock proves it. The firing DECISION --
  `should_terminate(resumed, elapsed, threshold)` -- is a pure function
  and is testable; a conservative threshold (30s, well past Android's own
  10s ANR) makes a false fire require a 30-second foreground freeze, which
  is itself a severe bug the SSH I/O on worker threads should never
  produce.

**So the decision, and it is the holder's:** ship a process-killing
watchdog whose recovery is unvalidated until it first fires (tuned to
never fire in normal use, and it captures the stack when it does), or
roll it out DIAGNOSTIC-first -- the same detector dumping the stack via
SIGQUIT WITHOUT the `_exit`, so the next real occurrence proves the gate
timing and captures the holder line at zero regression risk, and the kill
is added once. The second is the cautious order this family's history
argues for; the first fixes it a release sooner. Not built either way
until that is chosen, because a watchdog that kills the process is the
one addition where being wrong is worse than the bug.

**Chosen and built diagnostic-first, 2026-09-18.** The holder said "stop
here, work the fix from the evidence" and answered the kill-now versus
diagnostic-first question with "continue"; the session took the cautious
order. `1ab34c4` adds `src/hang_watchdog.cpp`: a 1s main-thread heartbeat,
a detached watcher thread, and `SIGQUIT` (ART's stack dump to logcat and
`/data/anr`) once the app was foreground and the beat has been silent 30s.
It was installed on the Fold 2026-09-18 and has not yet fired on a real
hang. **The `_exit()` half is still owed**, held until one real
occurrence shows the gate fires and the dump names the holder; read it
with `adb bugreport` or `logcat -v threadtime`.

**The gate is the lifecycle's now, and the first one was wrong in the
dangerous direction -- [FIXED 2026-09-24].** `1ab34c4` read "foreground"
from `QGuiApplication::applicationState()` inside the heartbeat, where the
bullet above says it must not. Reading Qt 6.12's own source settled what
that costs rather than leaving it a matter of design:

- **On pause, Qt stops delivering timers before it posts the state.**
  `updateApplicationState` calls `QAndroidEventDispatcher::goingToStop(true)`
  and only then `handleApplicationStateChanged`; from that moment
  `processEvents` runs with `X11ExcludeTimers`. So the heartbeat stopped
  with "foreground" as its last sample, and **every trip to the background
  longer than 30s met the gate** -- a stack dump now, and a killed live
  session the day the `_exit()` half landed. The one failure the design
  called worse than the bug.
- **On resume, Qt's state arrives late.** `QtActivityBase.onResume()`
  reports it after `super.onResume()`, and the Qt thread applies it
  asynchronously, after the window work where the deadlock is suspected --
  so a thread wedged there never records the resume, and the hang this
  exists for would have read as "backgrounded" and never fired.

`bssh_platform::watch_foreground` now carries Android's
`ActivityLifecycleCallbacks` (`BsshLifecycle.java`) into
`bssh_hang_state` directly, on Android's UI thread: `onActivityResumed`
fires from `super.onResume()`, before any of Qt's resume work, and
`onActivityPaused` from `onPause()`. **Resume also restarts the clock**,
or a healthy resume after ten minutes away would be charged for the ten
minutes and dumped before its first beat. Tested as a state object
(`the_hang_gate_follows_the_lifecycle_not_the_last_beat`) with three
sabotage edits in `tool/sabotage/hang-gate.toml`.

**Not proven on a device -- no phone was attached.** The wiring is
Android-only and no desktop test reaches it. What to look for once one
is: background the app for a minute and bring it back, and `adb logcat
-d` must show no `Signal Catcher` stack dump from `se.vibes.beerssh`. The
build installed on the Fold since 2026-09-18 carries the old gate, so
its log from that period should show exactly such dumps -- which is the
cheap confirmation of the reading above. The `_exit()` half stays held
until a real hang has fired this gate.

**Two observations above were weakened the same evening (2026-09-17).**
The ~14s backgrounded `makeCurrent(): no EGLSurface` spin matches no
beerssh timer and is Qt's own surface retry, so "stop the backgrounded
render activity" is not a lever beerssh obviously holds. And the
camera's `Waited 10001ms for FocusEvent` ANR is a recurring Samsung
camera fault: it also fired 2026-09-15 and at 20:23 on 09-17 with no
beerssh in the stack, so it is not evidence of a hung beerssh window.

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
**What remains open is narrower and is validation rather than design.** ~~The
type has never been adjudicated by a system that enforces it.~~ **Adjudicated
on Android 14, 2026-09-30:** an API 34 emulator image (`bssh_api34`), the app at
its targetSdk 35. Opening the local shell started the service foreground --
`isForeground=true`, `types=00000010`, which is
`FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE` -- with its notification, and the
system logged `Background started FGS: Allowed ... targetSdkVersion:35`, no
exception. Ninety seconds on the home screen later it was still foreground and
the process alive; `exit` in the shell removed both service and notification.
So Android itself accepts `connectedDevice` as declared, with
`CHANGE_NETWORK_STATE` qualifying it. **Play's review of the justification is
what remains**, and no local test answers it.
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

**Costed against the current texts, 2026-09-30, for the holder to decide.**
Fetched, not recalled:

- **Android's definitions** (developer.android.com, *Foreground service
  types*): `connectedDevice` is "Interactions with external devices that
  require a Bluetooth, NFC, IR, USB, or network connection", and qualifies on a
  declared `CHANGE_NETWORK_STATE`, which this manifest already has.
  `specialUse` "Covers any valid foreground service use cases that aren't
  covered by the other foreground service types" and wants a `<property>` in
  the manifest naming the use. `remoteMessaging` is "Transfer text messages from
  one device to another". None of the three has a time limit.
- **Only `dataSync` and `mediaProcessing` time out** (*Foreground service
  timeouts*): 6 hours in 24 for apps targeting Android 15, then `onTimeout`,
  and a crash if the service does not stop. That confirms avoiding `dataSync`.
- **Play's declaration** (Play Console Help 13392821): per type, a description,
  the user impact if the task is deferred or interrupted, a video of the user
  triggering it, and a use case picked from a preset list. Its example list for
  `connectedDevice` is "wearable, baby monitor, headset, car" -- external
  hardware; a server reached over the internet is not among them, which is the
  "slight stretch" above made concrete. `specialUse` is allowed where "your use
  case meets the other characteristics required for foreground service usage".
- **Precedent:** ConnectBot, an SSH client on Play, declares
  `remoteMessaging` on its main branch (manifest fetched today). Whether that
  build has been through Play review was not established.

So the three live routes:

1. **`connectedDevice`, as built.** No time limit, no manifest change; the
   risk is a reviewer holding it to the preset list's external-hardware
   examples.
2. **`specialUse`**, with a subtype property ("interactive remote shell kept
   connected while the user switches apps"). The most accurate description, at
   the cost of a free-text case reviewed individually. A manifest property and
   a permission change.
3. **`remoteMessaging`**, ConnectBot's choice. A terminal session is text to and
   from another device, which fits the definition's words better than the
   "continuity of messaging" framing Play's list may assume. Same size of
   change as 2.

The adjudication this section names has since been run for the type as built
(above, 2026-09-30): Android 14 accepts `connectedDevice`. A switch to
`specialUse` or `remoteMessaging` would want the same run on `bssh_api34`,
which is now one emulator start away. Play's opinion stays unknowable before
submission.

**Plan (as first written):** prototype with `connectedDevice` (no time limit, lowest friction), and
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
- **The one networking exception: HTTPS for the font download (§9.4),
  decided by the holder 2026-09-28.** Qt on Android has no TLS backend -- it
  dlopens a libssl the platform does not offer apps, and the kit ships none
  -- so every `QNetworkAccessManager` request to an https address fails with
  "TLS initialization failed". Found on the emulator after the download had
  passed every desktop test. `BsshHttp.java` does a HEAD and a ranged GET
  with `HttpURLConnection`, which brings the system's certificate store and
  redirects, and nothing else; `platform/range_source_android.cpp` runs it on
  a worker thread and hands the bytes back as a `bssh_range_source`, so which
  ranges to ask for and every check on what comes back stay in C++, under the
  suite. Rejected: bbq-predictor's route -- shared OpenSSL in the APK, native
  libraries extracted, Qt's TLS plugin loaded by hand -- for every native
  library on disk twice; and an HTTPS client of our own over the static
  OpenSSL libssh already links, for hand-written TLS verification. SSH is
  unaffected: libssh does its own crypto.
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
  **It never actually backed off. [FIXED]** `on_session_state_changed()`
  reset the attempt count whenever the session was `is_live()`, and
  `is_live()` is "not DISCONNECTED and not FAILED" — so it is true of
  `RESOLVING`, `CONNECTING` and `AUTHENTICATING`, which are exactly the
  states a reconnect ATTEMPT passes through on its way to failing. Every
  attempt reset the counter that was measuring it. The delay never left its
  first step, `BSSH_MAX_ATTEMPTS` was unreachable so `gave_up()` could never
  fire, and against a host that is simply not there the app dialled once a
  second for as long as it was left running — on a phone, the radio kept
  awake all night. That is the storm §5.3 exists to prevent rather than a
  variation on it.
  **The existing tests could not see it, and both are good tests.**
  `backoff_grows_and_then_stops_growing()` measures `backoff_delay_ms`, a
  pure function, and proves the CURVE is right; it has no way to observe
  whether the manager ever walks up it. The storm test is about EIGHT
  sessions flapping together and staggering apart. One session against a
  dead host is a third thing neither asks about.
  **The obvious fix would have been wrong in the opposite direction.**
  Narrowing the reset to `CONNECTED` alone drops the in-flight states
  through to `schedule_reconnect()` — dialling again while a connection
  attempt is still being made. So an attempt in flight is now explicitly
  neither: not a success to reset on, not a failure to schedule against.
  **Three fixtures were needed to reach it**, and the first two failed the
  way the defect itself does. With no transport the session reports
  DISCONNECTED throughout, `is_live()` is never true and the reset is never
  reached; driving `attempt_reconnect()` by hand leaves the single-shot
  timer armed, so the next `schedule_reconnect()` silently early-returns and
  nothing is scheduled. It waits for the manager's own timer now.
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
- **Inhibit sleep? DECIDED 2026-09-26, and BUILT** as a per-host toggle, off
  by default -- see §16. A long-running remote build is the argument for it;
  silently keeping a laptop awake because a shell is open is the argument
  against a default.
- **Desktop notifications** (via `QSystemTrayIcon`, or the platform's native
  service) carry the same events the Android notification does: reconnecting,
  reconnect failed, bell rang in an unfocused pane.
  **[BUILT 2026-09-20], and what was there before was the whole feature
  except the part that runs.** `bssh_platform::notify()` was declared with
  those three events written beside it as a comment, implemented on desktop
  (tray `showMessage`) and on Android (the notification channel), and
  **called by nothing, ever**. Both halves a reader would check were
  present and correct; the only thing missing was an invocation.
  **Reading the interface could not have found it, and that is the point.**
  The declarations are exactly the part that is not missing, so an
  interface review passes a half-wired implementation. What finds it is
  asking, of each method in turn, who calls it -- which is how §11's
  `erase()` was found leaving credentials in the keyring, and this is the
  second instance in this tree of the same lens. **The next sweep after an
  empty one should be the lens the last FIND suggests, not the one that
  just reported nothing.**
  The consequence was specific rather than cosmetic. Every window handler
  for these signals opens `if (session != m_current) return` and writes
  into the status line, so the focused session was covered and everything
  else had **no signal at all** -- a session reconnecting behind another
  tab, or in a window the user is not looking at, retried and gave up in
  silence. So the notification's guard is the **inverse** of the status
  line's, and the two together cover the sessions exactly once.
  Wired in `bssh_session_registry`, not in a window, for the reason the
  shell-ended handler beside it already gives: it is a fact about a
  session rather than about a view of one. A window cannot even ask the
  question -- each knows only whether IT shows the session, and §8.4 has
  them holding no pointer to each other on purpose, so with two windows
  open the answer needs an agreement none of them can reach. The registry
  owns the `bssh_conn_manager`s and is process-wide, so it fires once.
  **Giving up notifies whether or not the pane is visible**, unlike the
  other two: it is terminal, and a pane that has quietly stopped retrying
  looks exactly like one still trying.
  `notify()` is now a public non-virtual that emits `notified()` and
  delegates to a protected `platform_notify()` -- the split
  `has_biometric()`/`platform_has_biometric()` already uses, so it is this
  class's own convention rather than a new one, and it makes the call
  observable to a test without a test-only hook. Guarded by
  `tool/sabotage/notify-events.toml`, four edits.
  **One of the four was MISSED on the first run, and the TEST was what
  was wrong** -- the second miss of the day and the opposite cause to the
  first. §6.2's had been a sabotage that could not break anything; this
  was a sabotage that broke the code correctly while the test could not
  see it. The test raised `bell_while_away` itself, so deleting the only
  thing that legitimately raises it changed nothing it observed: a test
  that calls the helper it is verifying cannot see a wrong caller. It
  feeds a BEL byte now, which exercises the two conditions the session
  evaluates rather than assuming their result, and asserts the other
  direction too -- a bell rung while the session IS on screen must be
  remembered and not announced.
  **From outside the two misses are identical** -- a green suite under a
  broken thing -- and only running them says which one you have. That is
  the argument for the harness rather than for care: it removes the
  moment where being careful is a choice.

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
  **General key remapping is BUILT 2026-09-18**, key-to-key: a user says
  "when I press this, send that instead" for a fixed set of slots
  (`BSSH_KEY_REMAP_SLOTS`), stored as portable key-sequence pairs and
  applied in `handle_key` after the stolen chords -- so a chord this app
  keeps still wins -- and before the key's own translation. The target is
  translated through `emulator_key_for` like any other key, so it covers
  the remaps a person keeps (an F-key to a navigation key, a stray key to
  Escape) without a second, text-sending path; a target that is a
  character has no emulator key and is left alone. The UI is a fixed set of
  from/to `QKeySequenceEdit` rows in the shortcuts settings, chosen over an
  add/remove list because it needs no dynamic table to make fit a phone --
  that shape is the implementer's call, since sec 6.1 says a user should be
  able to remap keys but not how the control looks.
  **CapsLock is deliberately not a remap source**: it is intercepted
  earlier for Caps-as-Ctrl, and its OS lock state is beyond this window's
  reach anyway (see that setting), so the classic CapsLock->Escape belongs
  in the OS layout, not here. Guarded by
  `tool/sabotage/key-remap.toml`.
- **Pane navigation shortcuts** once splits exist (§8): e.g. `Ctrl+Alt+Arrow` to
  move focus between panes, `Ctrl+Shift+T` new tab. These must be **intercepted
  before** the byte translation and be **user-remappable**, because any chord you
  steal is a chord the remote shell can no longer receive. Keep the stolen set
  minimal and document it.
  **The set is written down exactly once, and its SIZE is not written
  down here.** It was eight when this paragraph was written, nine when
  Find landed and twelve since zoom (§9.5); the count belongs to
  `bssh_input_router::default_chords()`, where it can be recounted, and a
  number in this sentence would be a claim that goes stale every time the
  feature it describes is extended -- which is what happened twice before
  the sentence was rewritten.
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
  **And for three days it had exactly one route, which on a phone is
  none.** Found 2026-09-21 by asking what opens the find bar: a grep for
  `open_find()` returns the chord handler and nothing else, and a grep
  for *Find* across `main_window.cpp` returns no menu entry at all. The
  bar itself is built on the terminal page whatever the device, wired,
  and covered by six sabotage edits -- so every test that drives it
  directly passed throughout, because the defect is not in the search.
  A phone has no chord to press, so the feature sec 6.1 calls *"the thing
  a long scrollback is FOR"* could not be reached on the platform sec 8.7
  exists to complain about. It is on the terminal menu now, which is the
  surface both platforms have, and in the desktop menu bar's **Edit**
  menu, where every application has kept Find since menus were invented
  -- the discoverable half of a chord that worked only for somebody who
  already knew it.
  **The menu had room because it had just been grouped**: 520px became
  584 of a 640dp screen, and what is left is 56px, less than one more
  row. The next verb trips `the_touch_terminal_menu_fits_a_phone` rather
  than a screenshot, which is the whole reason that gate exists.

  **Find in the scrollback is BUILT 2026-09-18, and it is the ninth stolen
  chord.** Ctrl+Shift+F (konsole's) opens a bar at the bottom of the terminal
  page, below the input selector; typing searches the focused pane's
  scrollback and screen incrementally, Enter and the up/down buttons walk the
  matches, Escape closes it. The search itself lives in the VIEW -- it owns
  the scrollback, the screen and the coordinate space that spans them
  (`find_text`/`find_step`/`clear_find`), and searches the emulator's width
  rather than the widget's so an unshown or half-laid-out pane still finds
  what is stored. It is case-insensitive, it starts on the match nearest the
  live screen (the most recent time the text appeared), and it scrolls a
  match above the viewport into view a third of the way down for context.
  Matches are highlighted amber -- the current one bright, the rest muted,
  applied AFTER the brightness slider so a night setting cannot dim the one
  thing a search exists to make findable. The bar (`bssh_find_bar`) is a
  plain control that emits the query and the step and reads back a count; the
  window routes it to whichever pane has focus, the same split the input
  selector uses. A wrapped line is searched as one LOGICAL line: a row is
  joined with the continuation rows below it (the fact `line_is_continuation`
  and selection_text already rejoin on), so a word split across the wrap is
  found, and a match is a start-to-end RANGE like a selection rather than a
  single-row span, so its highlight covers both rows. Guarded by
  `tool/sabotage/scrollback-search.toml`, six edits seen to fail:
  case-sensitivity, a step that ignores direction, a match not scrolled to, a
  wrapped line searched a row at a time, the chord opening nothing, and the
  bar searching for nothing. The per-cell highlight is a paint change tested
  by reading, like the reverse-video and selection swaps it sits beside.
  **The right-click menu was the second copy, and it was there all along.**
  Its Copy and Paste entries wrote their chords out as the literals
  `"Ctrl+Shift+C"` and `"Ctrl+Shift+V"`, while every other menu in the window
  asks `binding_for()`. So rebinding copy left the context menu advertising
  the chord that had just been replaced — the app telling the user, in
  writing, to press a key that no longer does anything. Exactly the drift
  this section says a list written anywhere else would be free to do, sitting
  in the one place nobody had looked.
  **The menu already had a test, and the test could not see it.** It asserts
  the item TEXT and the order — and the text was never wrong. What was wrong
  was an attribute nothing asked about, which is the general shape: a menu
  test that reads labels passes over anything the label does not carry. The
  new one rebinds copy and asks the menu what chord it now shows, and leaves
  paste unbound so a failure cannot be "both were blanked".
  **Worth recording about the test rather than the bug:** the first version
  set the binding and then built the window, and failed against a correct
  fix. `bssh_main_window`'s constructor loads the chords from app settings
  and calls `set_bindings` itself, discarding anything set before it. The fix
  was right and the test was wrong, which is the reverse of the usual
  suspicion and the reason to read a failure before believing it.
  **The same two menus also answered "can you paste?" two ways.** The bar
  enabled Paste whenever a session existed; the right-click menu also
  required something on the clipboard. Same verb, same instant, greyed in
  one place and offered in the other. The bar could not have asked the
  better question: it re-derives on session and binding changes, and Copy
  additionally on `selection_changed`, but **nothing watched the
  clipboard** — so an answer depending on it would have been whatever was
  true when the last session appeared. The missing subscription is the
  bug and the weaker rule is what it left behind. Both verbs now have one
  answer each, `can_copy()` and `can_paste()`, and the constructor
  subscribes to `QClipboard::dataChanged` the way it already listens for a
  selection.
  **The first version of that test passed against the bug**, and only
  sabotage found it out. It built a window with no session — and with
  none, "is there a session" and "is there a session and something to
  paste" are both false, so the two menus agreed for the wrong reason. It
  is the failure this document keeps recording, committed while writing
  the test for it: **the case that distinguishes two rules is the one
  where they disagree, and a fixture that cannot reach it will report
  agreement whatever the code says.** It adopts a session now, and each
  half is sabotaged separately — the old rule gives context-disabled
  against bar-enabled, and removing the subscription gives the reverse,
  which is what shows the subscription is load-bearing rather than
  decorative.

### 6.2 On-screen keyboard strategy (touch platforms)
Android soft keyboards don't expose Ctrl/Esc/etc. **Two answers ship, and a
device-level setting chooses between them** (`input/keyboard`, §10.6); the
second was an open question in this section for a long time and is settled
below.

**The key row** — a strip of buttons above the system keyboard providing
`Esc Tab Ctrl Alt ↑↓←→ Fn - / | ~` and sticky modifiers. This is what
ConnectBot and Termux do and is what users coming from either expect. It is
the default, and it is right wherever a system keyboard appears: that keyboard
keeps its prediction, its languages and its layout, none of which a terminal
should be reimplementing.

**The built-in keyboard** — one this app draws itself, replacing both. It
exists because the row has a floor under it: where no system keyboard comes
up, a strip of modifiers has nothing to modify.

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
  terminal at all.
  **A user can compose their own now** (2026-09-20), which closes the half
  this line carried open and answers the second of the four things §6.3
  says are one question. It is the colour scheme's mechanism rather than a
  new one: a reserved `custom` entry compiled into the catalogue, written
  by a push from whoever owns the settings, never by the catalogue reaching
  into them. The entry is born as a copy of **Default**, so selecting it
  before editing anything gives a working row instead of a blank strip.
  The row is written **as words** — `esc tab ctrl alt left down up right`
  — and that is the decision rather than a shortcut: the spec is what
  `app.ini` stores, which §10.6 wants hand-editable, so an editor
  producing anything else would be a second representation to keep in step
  with the first. Composing is a trip to a dialog (Settings → Keyboard →
  *Custom key row...*) with a live preview, for the colour editor's reason:
  a choice that takes a deliberate trip is one somebody made, where one
  offered in a dropdown on the way past is one they made by accident.
  **The floor moved out of the test and into the code, and that is the
  part worth keeping.** "Escape and a modifier" was an assertion walking
  the curated layouts, which is the right place for it while every row is
  compiled in. Once a user can add one, a rule that lives in the test can
  only report the breach after it has happened — and it reports it
  against the five rows that are correct, because the assertion walks
  `all()`. `bssh_key_row_usable()` is the predicate now: `set_custom()`
  refuses at the door and the contract test calls the same function, which
  is §6.3's *"they become the contract every layout must satisfy rather
  than tests about the two boards we happen to have"* arriving a milestone
  early and by a route nobody planned.
  A refusal leaves the last good row alone, which is not fastidiousness:
  the row is the only way to send Escape on a device with no system
  keyboard, so an emptied one cannot be used to reach the settings that
  would fix it.
  Guarded by `tool/sabotage/custom-key-row.toml`, six edits seen to fail:
  an unusable row accepted, a refused row written before it is judged, the
  reserved id renamed, a chord not surviving the write-back, the dialog
  naming a key the parser cannot read, and the stored row never reaching
  the catalogue. The last of those is the call site rather than the
  object, which M5 is the reason for: its three edits inside the reset
  were caught and removing the CALL SITE was missed, because a push
  nothing invokes is indistinguishable from a working one.
  **One of the six was MISSED on the first run, and the sabotage was what
  was wrong.** It read `layout.keys.clear()` immediately before
  `layout.keys = keys`, which is the assignment and nothing else — an
  edit that cannot break the code, so the green suite proved nothing
  about it. That is the positive control that cannot fail, one layer out:
  the harness exists to ask whether the test would notice, and it can
  only answer for the edit actually written. Rewritten to write the row
  before judging it, it is caught. Worth recording because reading the
  edit did not find it and running it did, on a spec whose other five
  were right.
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
  **It still has to scroll on the 320dp cover screen, whatever the layout**
  (2026-09-10): the narrowest layout is 402px of keys against 320, and at
  `BSSH_TOUCH_TARGET_DP` 48 about six keys fit, so the strip is always
  dragged there. Raised with the holder as a design question and not
  answered; scrolling remains the design until it is.
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
  **The portal answered when that was written and does not answer now, so
  the three-tier chain currently fails on the holder's own desktop.**
  Re-measured 2026-09-03: `XDG_CURRENT_DESKTOP=TDE`, `QT_QPA_PLATFORMTHEME`
  unset, and `org.freedesktop.portal.Desktop` **is not provided by any
  `.service` file** — `gdbus` returns `ServiceUnknown`. The only portal names
  present are `org.freedesktop.impl.portal.desktop.lxqt` (activatable) and
  `impl.portal.Secret`, and an *impl* is not the frontend: something looking
  only for "a portal" on this box finds one and still gets no answer. So tier
  1 returns Unknown, tier 2 cannot answer, Qt hands back its default light
  palette, and tier 3 reads that palette and says **light** — correctly, from
  a palette nothing themed. **Three tiers, none broken, all wrong**, and
  nothing in the chain reports a difficulty; it looks exactly like a
  correctly-detected light desktop.
  **Tier 4 is the fix and is settled workspace-wide** (`harmonization.md`
  `038d7e9`, signalled by hydra): read `~/.trinity/share/config/kdeglobals`,
  then `~/.kde/share/config/kdeglobals`, and compare Rec.709 luminance of
  `windowBackground` against `windowForeground` — deliberately the same
  comparison tier 3 makes, pointed at the source that has the answer.
  Measured here: `windowBackground=0,42,78` is luminance 35.7 and
  `windowForeground=220,220,220` is 220, so background-darker gives *dark*,
  which is the truth. **Do not read the scheme name**: this machine's
  `colorScheme=DarkBlue.kcsrc` contains "Dark" by luck, and a name is not a
  predicate. **Abstain when the file will not parse**, rather than guessing.
  Not implemented here yet — the measurement is recorded so the next session
  does not re-take it, and hydra has tiers 1-3 plus this one, so its function
  is the thing to copy rather than a fourth implementation to write.
  **beerssh's existing final tier already obeys the safety half of that rule**
  without saying so: it does not abstain, it answers *light*, which is the
  direction that survives being wrong. In a terminal a wrong dark guess is
  pale text on pale — unreadable rather than merely plain — which is why this
  tree is the one where the whole question bites hardest.
  Both change sources are watched — Qt's signal and the portal's
  `SettingChanged` — because either may be the one that notices, and a theme that
  is only right at startup is one a user calls broken.
  **But touching the session bus to do it leaked a daemon per run, and it
  is fixed.** `QDBusConnection::sessionBus()` does not fail when
  `DBUS_SESSION_BUS_ADDRESS` is unset: libdbus AUTOLAUNCHES a
  `dbus-daemon --session` and connects to that. In a headless run — a test
  under the offscreen platform, an agent's shell, a build container — nothing
  reaps it, so each invocation that asked whether the desktop was dark left a
  daemon orphaned to init with a stale `/tmp/dbus-*` socket. Reported
  2026-09-18 by the copyright holder and independently by the guidelines
  session: **1031 of them across this workspace's Qt projects, ~2.2 GB, the
  oldest six days old**, every readable one carrying `PWD` of a project source
  directory — 43 were beerssh's. The fix is `session_bus_configured()`, which
  reads the env rather than calling `sessionBus()`, guarding both the portal
  read (`portal_appearance`) and the `SettingChanged` connect
  (`watch_colour_scheme`): with no bus configured the tier abstains, which is
  what it did already, only now without forking one to find out. It is the
  running-code.md "a daemon that outlives the shell — ask what kills it" case,
  and every project that consults the portal has the same leak to close (a
  cross-project signal, since the guidelines session is aware). **Not asserted
  in the suite**: the autolaunch needs an X display to succeed, `--self-test`
  does not reach the portal tier, and the platform is a singleton built once,
  so a test cannot re-enter the constructor to watch it happen — verified
  instead by reading the code path and by the cleared backlog staying cleared
  for beerssh's own runs once the guard was in.
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
  already made. **[DECIDED 2026-09-29 by the holder, and BUILT] The default
  alone follows the app.** A host that never chose a scheme is white on black
  while the app is dark and black on white while it is light, and flips live
  when the app does; one that chose, even `default` by name, keeps it. It
  follows the app's APPLIED appearance, not the desktop's, so a light desktop
  with the app set to Dark gives a dark terminal. No new scheme: the existing
  `black-on-white` is the light one the rules allow (§10.2). The theme sets
  the catalog's default before the palette and emits `appearance_applied`,
  which each view hears -- the palette-change event was tried first and did
  not reach a view that was not shown. Guarded by
  `tool/sabotage/scheme-follows-app.toml`, five edits.
- Keeping it buildable on desktop is also what makes it *testable* — a synthetic
  key row press is the cheapest way to exercise the router's sticky-modifier
  logic in a unit test.
- **A bundled soft keyboard was the open question here, and it is answered:
  yes. [BUILT]** This entry used to read "probably **no** for v1; rely on the key
  row + external keyboards", and that reasoning had a floor underneath it nobody
  had stood on. The key row is a strip of Ctrl, Esc, Tab and arrows placed *above
  the system keyboard*. Where no system keyboard appears, it is a strip of
  modifiers with nothing to modify, and the terminal cannot be typed into at all
  — reported from a device, which is the only place it can be seen. "Rely on the
  key row + external keyboards" is a complete answer only for people who have an
  external keyboard.
  **Both halves ship, and which one shows is a device-level setting** (§10.6,
  `input/keyboard`): *this device's keyboard* keeps its prediction, its languages
  and its layout with the key row above it, and is still the default and what
  every existing install has; *BeerSSH's own* replaces both. **Two states, not
  the three every other setting on that page carries** — and that is a real
  difference rather than an oversight. `AUTO` means "ask the device", and this is
  the one question the device will not answer: Android reports no capability for
  whether an IME will actually appear. An `AUTO` here would be a guess wearing a
  detection's clothes, and the case it would have to guess is exactly the one
  that strands the user — guess *system* where none arrives and there is no way
  to type the setting back.
  **Three groups, one side button.** Letters, numbers-and-symbols, and the
  terminal keys with F1–F12. The button sits in a full-height column on the right
  and names the group it goes *to*, because a button labelled with where you
  already are is one nobody presses. Cycling rather than a chooser: with three
  groups the furthest is two taps, and a menu over a keyboard that exists because
  the screen is already small is the wrong shape.
  **Every group has the same number of rows, and it is checked.** Not tidiness —
  the keyboard docks under the terminal, so a group one row taller would shrink
  the terminal when the side button is pressed: a resize on the wire (§6.3) and a
  reflow of whatever the remote is drawing, on every tap of the button whose only
  job is to change which keys are shown. Every row is also exactly 20 half-columns
  wide, so keys line up between rows; a thumb aims at where the key was on the row
  before. **That check caught a real fault on its first run** — the symbols row
  added to 22 — and the fix was not the obvious one: dropping a character would
  have left the comma with nowhere on the keyboard to be, while the semicolon was
  already duplicated on the letters home row.
  **The 48dp floor is a floor on height, and that is what makes a keyboard
  possible at all.** Ten keys at 48dp *wide* is 480 of a 360dp phone; nobody ships
  that, and Android's own keys are around 34. `check_fits` measures
  `control->height()`, so the rule and the reality already agreed. What the
  keyboard sets explicitly is a small minimum *width*, because a button left to
  its own size hint asks for its label's width — twenty of those is the defect
  this section already records the key row shipping with, where `Ctrl` drew as
  `Ctr` and the row read as cramped rather than as twice too wide. The terminal
  group's labels are `Hom`, `PgU`, `Bksp` for the same reason: a keyboard cannot
  scroll sideways out of an elided label the way the row can.
  **Shift is a layout change, not a wire modifier.** The letter keys relabel
  themselves to capitals while it is on, because the character itself differs —
  a key still reading `q` while about to send `Q` is the one thing on screen that
  could have said so, saying the wrong thing. This found a latent defect in
  `send_text`: it passed `MOD_SHIFT` through to libvterm, asking for a shifted
  version of an already-shifted character, which is the rule `handle_key` states
  thirty lines above itself. Before a keyboard of our own had a Shift key, the
  only route there was a sticky Shift armed for the mouse override, so the defect
  existed with almost no way to meet it.
- **Nothing ever asked for the system keyboard, which is why there was none.
  [FIXED]** The setting above defaults to the system keyboard, and that mode
  did not work — the finding that prompted this whole section's rewrite was
  "there is no keyboard on Android", and this is the mechanism. Qt raises the
  on-screen panel from `QLineEdit`, `QTextEdit` and the spin boxes, which call
  the private `QWidgetPrivate::handleSoftwareInputPanel()` themselves. **A
  custom `QWidget` gets nothing.** `bssh_terminal_view` set
  `WA_InputMethodEnabled` and implemented `inputMethodEvent`, so it would have
  understood a panel perfectly — the attribute says a panel *would be
  understood*, not that one should be shown, and no call to
  `QGuiApplication::inputMethod()->show()` existed anywhere in the tree.
  Asked for on focus **and** on a tap that selected nothing. Both are needed
  and neither is redundant: focusIn covers arriving by Tab, by the pane
  container's own `setFocus` and by the first touch, while a tap covers coming
  back after the user has dismissed the panel — which does not move focus, so
  focusIn alone would never fire again. `QLineEdit` asks on release for that
  same reason.
  **The suite cannot assert this and says so rather than implying otherwise.**
  It runs offscreen, where there is no input panel to raise and
  `inputMethod()->show()` is a no-op, so nothing here demonstrates a keyboard
  appearing; that needs a device. What the suite does hold is the gate around
  it — a view declines the panel when the built-in keyboard is chosen.
  **A claimed second defect did not survive its own test, and is recorded
  because the near miss is the lesson.** The reasoning was that applying the
  choice only when the *setting* changes would leave a view opened afterwards
  carrying its constructor's attribute — choose the built-in keyboard at the
  front door, where there is no view, then open a session. A test was written
  for exactly that order and **passed with the fix removed**, and still passed
  once strengthened to assert both directions. So the fault could not be
  produced. The test was deleted rather than kept, and the one-line call kept
  as belt-and-braces with a comment saying it is not a fix for anything
  observed. A test that cannot fail is worse than no test, because it reads as
  cover; this one had gone green in two full suite runs before sabotage was
  pointed at it.

- **The caret was answerable and never announced. [FIXED]** The accessible
  text interface implements `cursorPosition()`, so a screen reader asking
  where the caret is gets the right answer — and `QAccessibleTextCursorEvent`
  appeared nowhere in the tree, so nothing ever said it had moved. libvterm's
  `movecursor` callback routes through `notify_damaged()` like everything
  else, so a caret move reached a client as "the text changed" and never as
  "the caret moved". For a terminal that is the worse half to leave out: the
  caret is where your own typing appears, and following it is how a reader
  follows you typing.
  **Where it was hiding is the finding.** `note_selection_changed()`, the
  function immediately below the one that needed this, already carried a
  comment describing the defect exactly — "selectionCount() and selection()
  answer correctly whenever they are asked… nothing ever announced a change,
  so an assistive client learned of one only if it happened to re-query".
  Whoever fixed that half found the class, wrote down why it mattered, and
  left the identical gap one function above. The lens generalises: **for any
  accessible interface, list what it can ANSWER against what the program
  ANNOUNCES and look at the difference.**
  Announced on change only. Output that leaves the cursor where it was is the
  common case — a scrolling log ends on the same line — and an event per
  damage would be a reader announcing a move that did not happen.
  **The suite cannot observe any of this, and says so rather than implying
  otherwise.** Both emitters are guarded on `QAccessible::isActive()`,
  deliberately, since flattening the screen for an event nobody receives
  would put that cost on every damage; and under the offscreen platform
  `setActive(true)` does not take — measured again in the run that landed
  this, where the test called it and `isActive()` was still false. So
  `a_selection_change_is_announced_not_only_answerable` SKIPS, and the caret
  assertions were added to it rather than to a new test that would also skip.
  **No sabotage entry, and that is a decision rather than an omission**: the
  runner decides by whether a named test fails, and a test that skips cannot
  — an entry here would report MISSED and read as the fix being wrong instead
  of the platform being unable to see it. Same call as the `F_GETFL` guard
  and the keepalive loop.
  **A whole category of accessibility defect is invisible to every gate in
  this workspace for that reason**, and reading the two lists against each
  other is the only thing that finds it.

- **A modifier button must not outlive the session it describes. [FIXED]**
  Three instances of one mistake, all found by reading the new keyboard code
  rather than by using it. `refresh_modifier_buttons()` opened with
  `if (!m_router) return;`, so `set_router(nullptr)` — which is what a
  closing session does — left the buttons showing whatever they had. Lock
  Ctrl, close the session, and it still reads locked over a window with no
  terminal. The row is reachable in that state because **its visibility
  depends on the mode and the platform and never on whether a session
  exists** (`should_be_visible`), so it stays on screen with nothing behind
  it.
  **The sharper half is the press.** The button is CHECKABLE, so Qt toggles
  it before the handler runs — and a handler that returned early on a null
  router left it drawn as pressed with nothing able to put it back. At the
  front door that is permanent until a session arrives.
  The built-in keyboard had both, plus a third of the same shape:
  `refresh_letter_labels()` also returned early, so it could stay drawn in
  capitals after the session that shifted it had gone.
  The rule in all three: **no router means no session, which means nothing
  is armed**, so the buttons are drawn OFF rather than left as they were —
  and the refresh happens OUTSIDE the guard, because by then Qt has already
  toggled the button.
  What makes this worth recording is where it was hiding.
  `refresh_modifier_buttons()` already carried a comment warning about
  exactly this — "a Ctrl button still looking pressed afterwards is a button
  lying about the state of the terminal" — and the guard one line above it
  created the same lie one step further out. **The comment was right about
  the hazard and blind to the case beside it**, which is the same shape as
  §6.2's own note that a rule written under one configuration never
  announces the dependency it was written under.
  Both halves are sabotaged separately and fail at different assertions, so
  the test cannot be passing for one reason and covering both by accident.

- **Modifiers are three-state, the way a Treo's were. [BUILT]** `OFF → armed for
  the next key → LOCKED → OFF`, one press each; the press count *is* the state,
  so there is nothing extra to discover. A one-shot alone cannot express what a
  terminal asks for constantly: Ctrl-C is one key and arming serves it, but
  walking a buffer with Ctrl-N or paging with Ctrl-B is the same modifier over and
  over, and re-arming before every keystroke is an interaction no physical
  keyboard demands. Locking from *armed* rather than from *off* is deliberate —
  one tap for the common case, a second only when you already know you are
  sending more; the other order makes the rare case free and the common one
  dangerous, since a lock nobody noticed turns every following letter into a
  control byte. Armed and locked must not *look* alike either, since they differ
  by exactly what a user needs to know before the next letter.
  **Caps-as-Ctrl (§6.1) deliberately does not use this cycle.** It toggles, and
  the reason is recorded at that key: the second press is a correction, and
  answering a correction with a lock is the worst available reading of it. That
  came out of a test failure, and the test was right — the decision was already
  written down.


**Shift was `toUpper()`, and that left 21 characters unreachable —
[FIXED 2026-09-04].** Reported from a device as *"the full keyb isn't
full"*, which was exact:

    ~ ! @ # $ % ^ & * ( ) _ + { } | : " < > ?

No pipe, no `~`, no `$`, no `:`. In a terminal that is not a gap in the
keyboard, it is most of a shell.

**The cause is a model, not a typo, and the code said so in its own
comment**: *"A digit or a bracket has no capital to become, and
`QChar::toUpper` would leave it alone anyway."* True of `toUpper` and
false of a keyboard. Shift does not uppercase anything — it picks **the
other legend printed on the keycap**, and which character that is was
settled by a layout standard rather than by arithmetic. So `bssh_key_t`
carries a second value and the full keyboard's number row and punctuation
are given their ANSI twins from a table. Letters keep `toUpper`, because
for letters that genuinely IS the rule.

**The compact keyboard hid it.** Its symbols live on the `123` page as
separate keys, so Shift doing nothing there was invisible; the full board
has a real number row, which is what exposed it. A defect present in both
was only reachable through the surface built second.

**The label and the character come from one function**
(`bssh_shifted_char`), because a keycap that disagrees with its output is
worse than one that never relabels: the user learns the wrong thing and
keeps using it. And every key that CHANGES under Shift now relabels, where
before only letters did — which is precisely why nobody noticed the digits
were inert.

**Held by a test that walks the catalogue rather than pressing keys**, and
its first version was wrong in this document's favourite way: it keyed a
map by the plain character, so the numpad's `0` overwrote the number row's
`0` and it reported `)` unreachable. The numpad `0` is correctly twinless
— a real one is Insert. Two distinct keys carry the same character and
only one is shifted, so the question is *does SOME key produce this*,
which is also what a user means by "can I type it".

**And the fallback was silent — [FIXED 2026-09-04].** Choosing the
full-size keyboard on a folded phone did nothing visible, and was reported
as *"the choice doesn't work on a small screen"*, which from where the
user stood is what it was. The setting page now says so when the two
differ, asking `bssh_keyboard_style_for()` rather than repeating the
threshold, and re-asking on resize so that unfolding does not leave a
stale *"too narrow"* beside a keyboard that is no longer falling back.
**A setting that is quietly overruled is worse than one that is refused,
because there is nothing to argue with.**


**A configuration key and a hide key, and one tap to undo the hide —
[BUILT 2026-09-04]**, asked for by the copyright holder after using the
full keyboard on a device.

**They live in the SIDE COLUMN, not on a page**, and that is the only real
decision in it. Hide has to be reachable from wherever you are: a key on
the letters page cannot be reached from the numpad, so somebody who
cycled over to type a digit would have to cycle back to put the keyboard
away. The side column is the only part of the widget that does not change
when the page button is pressed. It also leaves the row arithmetic alone —
both keyboards have rows that must sum exactly to their own width, and the
full-size one is written in quarter keys where one wrong span puts every
key after it in the wrong place, so adding two keys to a row is the
expensive way to do this and buys nothing.

**The keyboard does not hide itself.** The window owns visibility, and
`apply_device_settings()` recomputes it on every pane rebuild and every
session event — so a widget that called `hide()` on itself would be
overruled moments later, which from the user's side is a key that does not
work. The window holds `m_keyboard_hidden_by_user` instead, and the
setting's own visibility rule is `built_in && !hidden_by_user`. Not
persisted: it is about this session's screen rather than a preference, and
a keyboard that stayed hidden across a restart would look like the setting
had changed itself.

**The way back is a tap in the terminal, and it travels through three
objects.** `bssh_terminal_view` reports the gesture FIRST and
unconditionally — before the `WA_InputMethodEnabled` check that decides
whether the SYSTEM panel should answer — because that attribute is cleared
precisely when the built-in keyboard is chosen, so a report gated on it
would be silent in the one mode that needs it. `bssh_pane_container`
forwards it rather than the window connecting to views directly, because
views are destroyed and rebuilt whenever the panes change and the window
would be wiring itself to objects that stop existing.

**Every part of that is a connection, which is why both tests drive the
WINDOW and not the widget.** A test of `bssh_soft_keyboard` alone passes
with none of it wired — the same shape as the `set_group` defect earlier
the same day, where correct-looking code had never been exercised.
`the_hide_key_is_remembered_and_a_tap_undoes_it` clicks the real key,
forces a pane rebuild and requires the keyboard to still be away, then
brings it back with a real `QTest::mouseClick` on the view rather than by
emitting anything — the path under test IS the plumbing.
`the_keyboard_config_key_opens_the_settings_page` requires the page stack
to land on the SAME `bssh_app_settings_view` the front door's menu opens,
and checks it was not already showing, or the assertion would pass with
the button doing nothing.

**Sabotage, two edits, both the version this would have been if written
carelessly:** dropping the remembered flag (`setVisible(built_in)`), and
moving the tap report below the `WA_InputMethodEnabled` check. Neither
looks wrong on the page and both make the feature useless.


**Would a system-wide Android keyboard help Google eavesdrop? — answered
2026-09-06, not a plan.** The holder asked: "If this keyboard were to be
made into a proper Android keyboard applet. Would it make it easier for
Google to eavesdrop on text etc?" The answer given was "no, not
materially": an IME's text goes from the keyboard's process to the target
app's `InputConnection` over Binder, and Google's position does not depend
on the keyboard, since they ship the OS. **The in-app keyboard is itself a
privacy feature today** — terminal input, commands and passwords never
pass through Gboard. The real costs of becoming a system IME are the blast
radius (it would see every app's text) and a long-lived service other apps
bind to; if one is ever built, "never log, never persist, never network"
is a hard constraint. Caveat recorded at the time: the `system_server`
claim was not checked against AOSP, and Samsung's additions were not
examined.

**A second built-in keyboard, in a real keyboard's shape — [BUILT
2026-09-04].** The compact one above is a phone keyboard: three groups of
four rows, thumb-sized, letters where a phone user expects them. The
holder asked for the other thing as well — every key a real board has, at
the width it has there — and the two are different enough that neither can
be the other with a setting.

**Quarter-key columns, because halves cannot express a keyboard.** The
compact one counts half-keys on a 20-wide grid. ANSI needs 1.25u, 1.5u,
1.75u, 2.25u, 2.75u and 6.25u, so the full board counts QUARTERS and is 60
wide — 15u, the width of a real main block. `columns` is therefore a
property of the GROUP rather than a constant for the tree. A single grid
could not hold both without one of them rounding, and a Shift a quarter-key
wrong is one a touch-typist's hand notices at once.

**Two pages, split where a real board's clusters are.** Page one is the
main block: function row, digits, the three letter rows, bottom row — 78
keys. Page two is what sits to the right of it on a real keyboard: the
navigation cluster, the arrows and the numeric keypad. Both pages are the
same 60 columns, so a keypad key is exactly as wide as a letter; giving
page two a narrower grid would have made its keys BIGGER than the letters',
which is the one thing a keyboard shaped like a keyboard must not do.

**Keys a terminal cannot send hold their space and draw nothing —
[DECIDED 2026-09-04]**, by the copyright holder, asked because the other
answer was defensible: a real keyboard has those keys, and "as many keys as
a real keyboard" could reasonably have meant drawing them inert. Super,
Menu, PrtSc, ScrLk, Pause, NumLock and Caps Lock have no encoding in
libvterm and no meaning on the wire. The geometry has to keep their places
or every key after them shifts; drawing a button that does nothing is the
*absent, not faked* rule §11 applies to capabilities, arriving in a
keyboard. `bssh_key_t::SPACER` is that, and the widget's switch over kinds
is exhaustive so a new kind fails to compile rather than drawing a dead
button.

**It does not fit a phone, and that is arithmetic rather than a defect.**
15u plus the page button needs about **770dp** to give every key §6.2's
48dp touch target. Measured: a 360dp phone gives each 1u key about **21dp**
and Qt elides `Home`, `PgUp`, `Left` and `F10`–`F12` to an ellipsis; a Fold's
cover screen at 317dp gives about 18dp; its inner screen at roughly 680dp
gives about 42dp, which is usable. Rendered at 800dp it reads as a
keyboard, every label intact. **So this is a large-screen keyboard**, and
**a narrow screen falls back to the compact one — [DECIDED 2026-09-04]**, the
holder's words being "fall back to compact on narrow screens". `a9152c8`
carries the reasoning: the threshold is §8.1's MEDIUM boundary, 600dp,
reused rather than a second number; it is a deliberate compromise against
the honest 770dp, which would exclude the Fold's inner screen (~680dp, 41dp
keys) that this keyboard was asked for; it never rewrites the stored
setting, so unfolding gives the full board back; and it only narrows — a
compact choice on a tablet is respected. The same request asked to "move
the leftmost groups of keys such as numpad into a second page"; it was read
as the clusters outside the main block (the numpad is on the right), and
the holder did not object.

**The default was wrong and is now the platform's.** `keyboard()` returned
SYSTEM everywhere. On a phone that hands a new user a keyboard with no Esc,
no Ctrl, no arrows and no function keys — the four things a terminal is
mostly made of — and leaves them to find a setting to fix it. It is
BUILT_IN on a touch device and SYSTEM on a desktop, where there is a real
keyboard already. A stored value always wins, so nobody's choice changes.

**And the consumer had to be asked the right question.** The window decided
whether to show a built-in keyboard with `keyboard() == KEYBOARD_BUILT_IN`.
With a second built-in value that test is false for the full board, so the
system panel would have been declined and nothing drawn in its place — a
phone with no keyboard at all. It asks `!= KEYBOARD_SYSTEM` now. §10
records the same shape for preset fields: a setting that reached the first
consumer anybody thought of and not the second.

**Two things the pictures caught that no assertion did**, which is §17's
argument for `make shots` and the reason the keyboard now has shots at all
— it had none, a surface the user's thumbs live on with nothing rendering
it. The settings combo's third item wanted 373px of a phone's 346 and
pushed the whole form into sideways scrolling. And the elision above is
only visible in a render; the catalog's shape test says every row adds up,
and every row did.

**The row arithmetic is generated, not typed.** Page two was written with
its widths spelled out and one row came to 52 instead of 60 — caught by the
catalog's shape test, which now walks both styles and each group's own
width. It is built from named geometry (`nav`, `gap`, `pad`, `trailing`,
summing to the row by construction) since. Worth recording how the hand
check missed it: page one was re-derived from ANSI dimensions and passed,
and page two is exactly the part that was not re-derived. **A check over
part of the population reads exactly like a check over all of it.**



**Cfg and Hide are icons, drawn rather than typed — [DONE 2026-09-05].**
Asked for by the copyright holder. The two are painted from primitives
in `bssh_soft_keyboard`: a cog for settings, a chevron coming down to a
bar for hide, which is what every platform's dismiss control draws.

**The settings icon was three sliders first, and the reasoning for them
was backwards — [FIXED 2026-09-06].** The holder said they would have
preferred a wrench or a cog. Sliders had been chosen because a gear's
teeth are the first thing to turn to mush at 36dp: an argument about
RENDERING, offered against an argument about MEANING. Meaning wins. A
cog is what Android's own Settings, GNOME, iOS and essentially every
toolbar use, so it needs no learning, while sliders read as a filter or
an equaliser to most people — and an icon that has to be explained is
the wrong icon. The wrench was declined in the same breath: it reads as
tools, repair or developer options, and Chrome moved off it years ago.

**The rendering problem is real and is answered by drawing FEWER teeth
than a gear has, and by drawing them square.** Eight, not the twenty a
real gear carries.

**Three attempts, and each was wrong on a device rather than in a
test.** The first drew a stroked ring with six radial lines out of it
and read as a ship's helm: thin spokes STICKING OUT of a circle, where
a gear's teeth are part of the rim. The second filled the outline --
one path out to each tooth, along it, back to the rim -- with the
centre punched out by an odd-even subpath, and the roots following the
root CIRCLE rather than a straight chord, which at six teeth is long
enough to see and flattens the body.

**The third fixed the thing the first two had wrong and nobody had
named.** Reported as not quite clear, with the reporter unable to say
why. Blown up, the teeth were WEDGES: each walked from the rim at one
angle out to the tip at another, so its sides slanted and it came to a
point, and eight of those would have been a spikier starfish rather
than a clearer cog. **A gear tooth has parallel flanks** -- both
corners of the base at the same angles as the two corners of the tip,
so the side runs straight out along a radius and the top is flat.

Shorter teeth with it: rim at 0.33 and tip at 0.42, about a quarter
again rather than half, since a tall tooth on a small body is the other
half of the starfish. The hole went from 0.115 to 0.15.

**Worth keeping for the next icon: "add more detail" was the reported
remedy and would have made it worse.** What was missing was not
ornament but a shape rule, and the way to find it was to blow the
rendered pixels up rather than to reason about the drawing code.

**The test asserted the icon was not null, and a QIcon built from a
fully transparent pixmap is not null.** It counts ink now, rejects a
solid block as well as a blank one, and asserts the cog has a HOLE —
which is what separates it from a blob at this size, and the one thing
an inverted fill rule destroys. Made to fail: `Qt::WindingFill` fills
the hole and the assertion reads alpha 255 against 0, exit 2.

**A character would have been three lines and a gamble.** A gear is
U+2699 and a chevron U+25BE, the label font is the system's, and a
device whose fallback lacks the glyph draws a tofu box — which under
§11's absent-not-faked rule is worse than the word it replaced. Qt's
standard pixmaps have no settings icon either. Painted primitives cannot
be missing, scale with the button, and take `QPalette::ButtonText`, so
they follow the light and dark schemes without a second asset.

**They carry their colour rather than taking it when drawn**, an icon
being pixels, so `changeEvent` repaints them on a palette change — the
failure `bssh_key_cap` avoids with alpha and this one cannot.

**And the word did not disappear with the label.** A button whose text
becomes a picture loses what a screen reader was reading, so "Settings"
and "Hide the keyboard" moved to the accessible name and the tooltip.
`the_keyboard_side_icons_are_still_named` asserts the name, the icon and
the *absence* of the old text — a name on a blank button would satisfy
the first check while leaving nothing to press.

**åäö, and the layout question underneath it — [BUILT 2026-09-04].** The
holder asked for the Swedish vowels and named the better answer in the
same breath: US International with AltGr, annotated on the keyboard,
because it is unfamiliar to many. So `bssh_key_t` carries four levels
rather than two, the right Alt is AltGr — which is what it is on every
board that has one, no new key and no change to the row's arithmetic —
and `MOD_ALTGR` is tracked by the router and masked out of everything it
sends, because AltGr is a *layout* selector and no terminal has ever
received one.

**One deliberate divergence from us(intl): the quote keys stay live.**
That variant makes `'` and `"` DEAD at levels 1 and 2, so `'` then `a`
gives `á`. In a shell that is hostile — they are quoting characters typed
constantly, and each would need a trailing space. Levels 1 and 2 come
from `us(basic)` and only levels 3 and 4 from `intl`. That is what makes
it usable in a terminal and is why AltGr rather than dead keys was the
right answer to the holder's question.




**The `&` key drew a blank cap — [FIXED 2026-09-04].** Qt reads `&` in a
button's text as a mnemonic marker and eats it, so the ampersand on the
compact symbol page was an empty key. It sent the right character, it
was the right size, it was in the right cell, and every test about the
catalogue passed — because the catalogue was right. What was wrong was
the *drawing*, and only a photograph of the phone showed it.

`bssh_key_cap` already draws its own text when there is a hint, and
`QPainter::drawText` does no mnemonic interpretation, so the fix is to
take that path for an ampersand too. The two alternatives are worse:
escaping to `&&` makes `text()` lie, and `text()` is what a screen
reader reads; and drawing *every* cap here would lose the style's
eliding, which the full board's narrow keys already depend on.

Pinned through the predicate rather than through pixels —
`bssh_key_cap::needs_own_paint` — because pixels are what `make shots`
is for, and a unit test that rasterises a button is a test of Qt. What
the test holds is the decision, plus the fact that the keyboard really
does carry such a cap, or it would be a rule about nothing.

**The compact keyboard could not type `+ = [ \\ ] { }` — [FIXED
2026-09-04].** Seven printable ASCII characters unreachable at any level
of any group, on the board a phone gets, in a program that is a shell.
No `${VAR}`, no `[ -f x ]`, no escaping, no line continuation. Nothing
said so: every group was the right shape, every row added up, every key
sent what its cap showed. **A keyboard is not a set of correct keys, it
is a claim that you can type** — and only asking what it *cannot* reach
can check that.

**What paid for them was duplication the page already had.** `?`, `!`
and `*` were each one Shift away on that same page — Shift+`/`,
Shift+`1`, Shift+`8` — so it was spending three of its seventeen keys on
characters it already carried. Nine of the seventeen were, in fact;
these three are the ones whose Shift lives on the same page rather than
another one, which is what makes them the cheapest to give up.

**The replacements needed no new machinery**, which is the split paying
off a second time. They come in the pairs a real keyboard uses: `[` and
`]` are AD11 and AD12, `=` is AE12, and the backslash replacing the bare
pipe is BKSL. The layout table already gives each of them `{`, `}`, `+`
and `|` on Shift — so four keys bought seven characters and the palette
did not have to learn anything about them.

`both_keyboards_reach_every_printable_character` is the guard. Capitals
are excluded because they are not a level: `bssh_shifted_char` falls
back to `toUpper`, so a letter key covers its capital without storing
one, and a layout in a caseless script gets the right answer by not
having one to store.

**It was found by running the sabotages, and by the one that failed.**
The QWERTZ edit — the compact board keeping its own copy of the letters
and drifting — was MISSED, because `both_keyboards_read_one_layout`
compares only characters both boards carry, so a letter going *missing*
is an absence rather than a disagreement and it cannot see one. Chasing
that blind spot is what prompted asking which characters each board can
reach at all, which is how the seven turned up. **A sabotage that is not
caught is worth more than one that is**: the four that were caught
confirmed what was already believed, and the one that was not led
somewhere nobody was looking.

**The compact keyboard had no AltGr key — [FIXED 2026-09-04], and it is
the worst thing in this section.** Every letter on that board carries a
third legend and now draws it in the corner of the cap. The board had
Shift, Ctrl and Alt and nothing else — so on a **phone**, which is the
device that gets the compact keyboard and the device the accented
characters were asked for, not one of the 78 hints could be reached. The
full-size board had AltGr from the start, on its right Alt, and the
full-size board needs 600dp. The level therefore worked everywhere
except where it was wanted.

**Every test passed while it was broken**, and each was true: the
catalogue reaches the characters (`altgr_reaches_the_us_international_level`),
the router sends them, the cap draws them
(`the_altgr_legend_is_shown_and_then_stands_aside`), both boards agree
about them (`both_keyboards_read_one_layout`). Not one asked whether the
board a user is actually looking at has a key that gets there. It is
§14's most repeated defect — a correct function with nothing wired to it
— arriving a level up: a correct *keyboard* with no way in.

**It was found by a photograph of the phone.** Not by a sweep, not by an
assertion, and not by the desktop renders, which show the same missing
key and had been looked at twice. What made it visible was the device
screenshot taken to confirm an unrelated alignment fix, with a shell
open and the keyboard up, where the question "can I type ä on this"
answers itself.

`a_keyboard_can_reach_every_level_it_shows` is the guard, written as a
relationship rather than as "the compact board has an AltGr key": every
level any key on a style *advertises* must be one some key on that same
style *offers*. It holds for a style added later and for a layout that
puts its third level somewhere else. The row pays for it with a column
off Alt and one off Tab, which puts them at the width of a letter key
rather than below it; Ctrl, Esc and Enter keep theirs, being what a
terminal reaches for most.

**Slots and legends, split — [BUILT 2026-09-04].** A keyboard is a
GEOMETRY and a LAYOUT, and this file had them fused: `full_literal_run
("qwertyuiop")` declared in one call both *ten keys of this width here*
and *these ten characters*. A second layout could not have been added
without forking the geometry it was welded to, and the first thing anyone
wants after AltGr is their own letters.

Positions are named the way XKB names them — `AE01` leftmost on the digit
row, `AD01` on QWERTY, `AC01` on home, `AB01` on the bottom, plus `TLDE`
and `BKSL` — because that is the vocabulary every layout in the world is
already written in. Geometry says `slot_run("AD", 1, 12, BSSH_FULL_UNIT)`;
one table of 47 rows says what those positions carry. A Swedish layout is
a second such table and *no change to any geometry at all*.

**The table was generated from `/usr/share/X11/xkb/symbols/us`** — levels
1 and 2 from variant `basic`, 3 and 4 from `intl` — rather than
transcribed, because a table of accented characters is exactly the kind
of thing that is plausible and wrong when recalled and cannot be caught
by re-reading it. Deriving it also **checked the two hand-written tables
it replaced**: it reproduces all 38 rows of the old AltGr table and all
21 of the old shifted-twin switch, exactly. Two transcriptions agreeing
is worth something only when the second did not come from the first.

**The refactor carries a proof, and the proof caught two invented
values.** The whole composed catalogue — every group, row, key, span and
level, 238 lines — was dumped before and after and diffed. It came out
identical, but not on the first attempt: the generator had dropped level
4 wherever it equalled level 2, which silently took `^` off Shift+AltGr+6;
and it had kept level 4 on `g h v b`, where intl merely repeats the
capital that `toUpper` already gives. Reading the diff of the *source*
would have shown neither. Both are the same underlying phenomenon — intl
repeating basic on the keys it deadened — and the rule that expresses it
(`if l3 == l1 and l4 == l2`) reproduces the old tables exactly, where two
plausible rules did not.

**What the split buys, asserted:**
`both_keyboards_read_one_layout` compares every character the compact and
full boards share, at all four levels. They used to be two copies of the
alphabet with nothing holding them together — a letter corrected on one
would have stayed wrong on the other, and no test could have seen it,
because each was right about itself. It counts what it compared, since a
loop that agrees about nothing passes just as loudly.
`every_slot_the_geometry_names_exists` catches a mistyped position:
`slot_key()` is handed a string and cannot fail at build time, so an
unknown slot draws a key that sends nothing and wears the id it could not
find, and this is what reads it.

**The compact symbol pages are deliberately NOT slots.** They are a
palette — `-/:,()$&@"` arranged for a thumb, not a keyboard's positions —
so they name characters, and one lookup by level-1 character gets them
the same second and third legends from the same one table. A character
that sits at no slot's level 1, such as the `?` on that page, has none,
which is what it had before.


**A keypad key sends its digit and nothing else — [FIXED 2026-09-04].**
It did not. Every key on the numeric keypad was built with the helper
that consults the layout, and the layout is keyed by character, so the
keypad inherited the *digit row's* legends: Shift on its `4` sent `$`,
AltGr on its `5` sent `€`, Shift on its `.` sent `>`. A real keypad with
NumLock on sends its digit, and KP7 is KP7 in every language — which is
also why the keypad gets no slot.

**The one correct key in the group was an accident.** The double-width
`0` had been built with a different helper because of its width, so it
alone behaved, and the group disagreed with itself for as long as nobody
compared its members. `keypad_key()` is that helper for all fifteen now.

**It was found by a test written for something else**, which is worth
recording as a method rather than as luck. `both_keyboards_read_one_layout`
hashes keys by the character they send, so the keypad's `0` and the digit
row's `0` collided and it failed on `) != nothing`. The first reading was
that the test was wrong — it was, and scoping it to the main block was
right — but the collision it tripped over was a real fault underneath.
Reading a failure before deciding the test is at fault is what separated
the two, and the composed-catalogue dump showed the fix touching exactly
13 keys and nothing else.

**The annotation shows one prominent legend, always the one that will be
sent — [DECIDED 2026-09-04].** The holder asked whether to show only the
active character while a modifier is held, or to show every legend and
move the emphasis. The answer is a middle one and it is a *paint*, not a
string: `QToolButton` renders one font in one colour and neither it nor
`QAbstractButton` renders markup, so two legends of different weight in
one cap needs `bssh_key_cap::paintEvent`. The cap reads `q` at full
weight with `ä` small and dimmed in the corner a printed keycap puts it;
arm AltGr and it reads `ä` at full weight with no hint at all.

Showing both always, with the emphasis moving, reads better on paper and
worse on a phone: hunting for a character means scanning forty caps, and
forty caps showing two glyphs each is twice the reading at the one moment
the user is already looking for something. Collapsing to the active
character alone — the holder's first option — loses the annotation
exactly where it was cheapest to keep, on the idle keyboard nobody is
reading under pressure.



**The first paint made the letters wander, and said in a comment that it
did not — [FIXED 2026-09-04].** It raised the primary legend by half the
hint's height, with a comment claiming that was what kept a hinted cap
and an unhinted one in line. It was the exact opposite: only the hinted
caps moved, so `g`, `h`, `v` and `b` — the four letters us(intl) leaves
without a third level — stood visibly lower than the rest of their rows.
The primary is centred in the whole cap now, which is where the style
puts it on a cap with no hint.

Worth keeping for how it was found rather than for the fix. **No
assertion could see it**: every key is present, correctly sized, in the
right cell, sending the right character, and the catalogue is identical
either way. It was the render on the phone that showed it — and once it
was known, it is plainly visible in the desktop shot too, which had been
looked at and passed. A picture answers a question a number cannot, and
a picture only answers the question you bring to it.

**Only the AltGr legend is hinted, and not the Shift one.** A cap reads
`1` with `¡` in the corner and no `!`, which looks like an omission and
is the rule doing its job: the hint is for a level the user cannot
discover by a gesture they already know. Everyone who has used a phone
knows Shift relabels the keyboard, and pressing it does — `1` becomes
`!` on every cap. Nobody arrives knowing that AltGr exists, let alone
that it is the right Alt. Hinting both would put three glyphs on a 36dp
key to teach one thing that needs teaching and one that does not.

**The dim is alpha and not `bssh_dim()`**, which §9.3 defines as scaling
toward black. That is right for a terminal glyph on a background this
program owns and knows is dark; a keycap is drawn on the *desktop's*
button colour, which may be either, so scaling toward black would dim the
hint on a dark theme and SHARPEN it on a light one. Alpha blends toward
whatever is actually behind. It is the workspace's rule about a colour
meeting a ground the program did not choose, arriving one widget down.

**And the hint is said as well as drawn.** `setAccessibleDescription`
carries it, because the cap's `text()` stays the single character it
sends — a braille display reading `q ä` would be wrong twice over, since
it is one key sending one thing — and without the description a screen
reader user would have no way to discover the third level at all. An
annotation that is invisible to the people who most need things named is
an accessibility regression dressed as a feature.


#### A layout engine — **[BUILT 2026-09-04]**

**The three open decisions were answered by the copyright holder the same
day: the setting is GLOBAL, Swedish ships first, and the layouts come
from somewhere the legwork is already done.** That last one settles the
curation question in a better way than naming a set would have: the
corpus is **xkeyboard-config**, which every Linux desktop uses and which
carries about 100 layouts, so what ships is a matter of running the tool
again rather than of transcription.

`tool/gen-layout.py` is that tool. It resolves a layout through
`setxkbmap | xkbcomp`, takes characters from `keysymdef.h`'s own `U+`
comments, and dead-key bases from the system Compose file. **Three
sourced files and nothing transcribed** -- which matters because the
hand-written table it replaced was wrong.

**Regenerating the shipped US table was the proof, and it failed twice
before it passed.** First the tool lost `«` and `»`: xkb spells that
keysym `guillemotleft`, and `keysymdef.h` carries the old spelling as a
bare alias whose comment says *deprecated alias for* and gives no
codepoint, so resolving name-to-comment silently dropped it. Resolving
name-to-value-to-codepoint fixes aliases generally. Then it wanted to
emit **combining** marks from flattened dead keys -- `dead_horn` gives
U+031B -- which stack onto the glyph to the left instead of typing
anything, so it refuses anything with a non-zero combining class.

**Then it found eleven slots the shipped table had silently dropped**,
including `ç`, `Ç` and `¿`. The cause was a hand-written keysym table
with holes in it, where an unknown name became an empty string. **And
the corroboration recorded above was worthless**: this document said the
generated table "reproduces all 38 rows of the old AltGr table exactly",
and it did -- because the same author wrote both, with the same blind
spot. Two witnesses, one hand.

**Dead keys are flattened, not composed, and the Compose file decides to
what.** Swedish has no live tilde and no live backtick at all: `AE12` is
`dead_acute`/`dead_grave`, `AD12` is `dead_diaeresis`/`dead_circumflex`,
and AltGr+`AD12` is `dead_tilde`. Each becomes what the system pairs it
with space -- `'`, `` ` ``, `"`, `^`, `~` -- which is **one keystroke
where a physical Swedish keyboard needs two**, and is somebody else's
decision about what a dead key stands for rather than ours. Composing
properly remains worth having as fidelity; it is not needed for access.

**The contract earned its keep immediately, twice, and the second time
corrected this document.** `both_keyboards_reach_every_printable_character`
runs over *every* layout in the catalogue, and on the first non-US layout
it failed twice:

- **The compact board could not type `*`, `;` or `^`.** Those came from
  the US layout's Shift levels -- `;` is `AC10`, which Swedish makes `ö`,
  and `^`/`*` were Shift+6 and Shift+8, which Swedish makes `&` and `(`.
  The number row was `slot_run("AE", 1, 10)` and should never have been:
  **a phone's number page is a palette, not a layout**, so it is a
  `literal_row` now and keeps the US pairs in every language. The colon
  key became a semicolon key carrying the colon on Shift, the way a real
  `AC10` does.
- **The full board could not type `|`.** This corrects the sentence
  written a few hours earlier saying the ISO key was fidelity rather
  than capability, because the symbol page covers what a layout cannot
  reach. **That is true of the compact keyboard and false of the full
  one, which has no symbol page** -- its two pages are the main block and
  the keypad, so it is purely the layout and inherits every gap the
  layout has. `LSGT` ships in the Swedish table now, and
  `full_main_group()` spends a column of the left Shift on it: 1.25u and
  1u, which is what an ISO keyboard does with that space, and the row
  still comes to 60.

**The claim that a layout needs no geometry change is therefore dead in
both directions** -- it needed correcting once for ANSI-vs-ISO and again
for which keyboard is asking. What survives is narrower and true: the
*compact* keyboard's geometry is layout-independent because its palette
carries the ASCII; the *full* keyboard's is not, and asks the layout
whether it is ISO.



**The compact keyboard is 22 half-columns, not 20 — [DECIDED 2026-09-04]**
by the copyright holder, choosing all three pages over the letters page
alone. It was eleven keys or nothing: **selecting Swedish made Swedish
worse.** `å` is `AD11` and `ä` is `AC11`, the rows stopped at `AD10` and
`AC10`, and only `ö` survived because it sits on the row. Under US
International all three are reachable through AltGr — so the US layout
was a better Swedish keyboard than the Swedish one, on the device it was
asked for.

**The ASCII contract passed throughout, and was right to.** Those letters
are not ASCII, and what that test promises is a keyboard that can type a
shell. `a_small_screen_loses_no_letter` is the promise it does not make:
every LETTER the full board can send, the compact one must send too.
Stated against the full keyboard rather than a list, so it holds for a
layout nobody has added yet — the full board carries every slot, so what
it sends is what the layout has. Swedish loses only AltGr curiosities
from `TLDE`, `AE12` and `BKSL` — `√ § ´ · ± … ‰` — and no shell wants any
of them.

All three pages went to 22 so that keys do not move under a thumb when
the page button is pressed, which is what §6.2 designed the shared
bottom row for. **Alt and Tab got back the column AltGr had cost them**,
and the extra width paid for real things rather than padding: the digit
row runs `1234567890-` in `AE01`–`AE11` order, the US letters page gains
`[` and `'` as direct keys, and the terminal page finally has **all four
arrows on one row** — they had been split across two because ten was the
width, and a cursor cluster with two arrows on the row above is not one
a thumb can use without looking.

**A keycap shrinks its label rather than eliding it — [FIXED
2026-09-04].** The narrower keys cost one label: `Hom` drew as an
ellipsis at §8.1's narrowest phone, where `Ins`, `Del`, `End`, `PgU` and
`PgD` all still fit. The style's answer to a label too wide for its key
is an ellipsis, which on a keyboard is the worst of both — the key still
takes the room and no longer says what it does.

`bssh_key_cap::fitted_font` walks the font down in 6 per cent steps to a
floor of 70 per cent and the cap draws at whatever fits. **Below the
floor it elides after all**, deliberately: a three-letter label smaller
than the AltGr hint beside it reads as a mistake rather than as a
smaller label, and an ellipsis is bad where four-point type is worse.
That is why the full keyboard still elides at 360dp — 1u is 24dp there,
no font in the range fits `PgUp`, and §6.2 already says that board wants
600dp and falls back to the compact one below it. At 800dp every label
is whole.

It also decides the paint path by MEASUREMENT rather than by asking the
text: whether a label fits is a property of the key's width, which
changes with the screen, so `paintEvent` measures and `needs_own_paint`
answers only the two questions that are properties of the string.

**Two of the four sabotages were missed on the first run, and both
misses were findings.** Dropping the ISO key was expected to fail the
shape test and did not -- because that test walked only the *current*
layout, so a Swedish shift row four columns short went straight past it
while the US row it was looking at was unchanged. It walks the catalogue
now. And replacing the settings page's `addRow` with a `hide` left the
suite green, because the test found the control by object name and
`findChild` finds a widget that was built and never put on the page. It
asserts the control is not hidden and has a parent now. **A sabotage
that is not caught is worth more than one that is** -- the two that were
caught confirmed what was already believed.


**Twenty-seven layouts ship, and the corpus sorted them rather than
anybody curating — [BUILT 2026-09-05].** The holder asked for a fuller
set once Swedish worked. Because the corpus supplies the layouts and the
contract admits them, this was a measurement rather than a curation:
generate, register, and read which pass
`both_keyboards_reach_every_printable_character` and
`a_small_screen_loses_no_letter` unchanged.

**Thirteen passed immediately** and shipped with the corpus's own names
from `rules/base.lst`, so nobody here decides what a layout is called:
US International, Swedish, Norwegian, Danish, Finnish, English (UK),
Dutch, Spanish, Portuguese, German (Switzerland), Polish, Latvian and
Romanian.

**Thirteen more were refused for one reason, and it was a fact about
this keyboard rather than about those languages.** German lost `ss`,
French lost `e-acute e-grave a-grave`, and Czech, Slovak, Hungarian and
Lithuanian lost most of their accented set. Confirmed by reading the
tables rather than inferred: eszett is **AE11** and the French three are
**AE02, AE07, AE10** — all on the DIGIT row. The compact keyboard's
digit row is the ASCII palette, not a slot run, so a letter placed there
had nowhere to appear.

**That palette is not an accident and could not simply be reverted.**
The digit row *was* a slot run, and that is what left the Swedish
compact board unable to type `*`, `;` or `^`. So the two failures are
the same knob at its two ends:

| digit row | guarantees | loses |
|---|---|---|
| ASCII palette | every printable ASCII, in every layout | letters a layout puts on the digit row |
| slot run | the layout's own digit-row letters | ASCII, wherever the layout's Shift level differs |

Neither is right for AZERTY, which needs both.

**The fourth compact page closed it, and all fourteen remaining Latin
candidates now ship — [BUILT 2026-09-06].** A fourth page carrying the
slots the other three omit, which costs a tap and no ASCII. Fourteen
were generated and registered and **not one was refused**: German,
French, Czech, Slovak, Hungarian, Lithuanian, Italian, Belgian,
Slovenian, Croatian, Estonian, Icelandic, Turkish and Maltese.

**The page is defined by the GAP rather than by a row**, which is what
makes it hold for a layout nobody has added yet. Naming "the AE row"
would have covered German and French and missed Slovak, which puts
a-diaeresis on **AD12** — the letters page runs AD01-AD11. That is not
hypothetical: the first version enumerated the digit row, TLDE, BKSL,
LSGT and AB08-AB10, read as complete, and `a_small_screen_loses_no_letter`
refused Slovak on the one slot it had missed. The enumeration is written
out rather than derived, because a key does not carry its slot id once
built, so **the contract test is the guard that the list is complete —
and it has already done that job once.**

**It is conditional, and that is the half a reachability test cannot
see.** Under most layouts every omitted slot holds punctuation the
palette already carries, so a fourth page would be a tap every user pays
and nobody spends. The page is admitted by a set difference against the
pages already built — is there a LETTER the compact keyboard could not
otherwise send — rather than by naming the layouts that get one.
`a_small_screen_loses_no_letter` would go on passing if every layout
grew a page it did not need, so
`a_fourth_page_appears_only_where_a_letter_needs_it` counts the pages
instead: three for US International and for Swedish, four for German.
Swedish is the sharper control, having letters the palette lacks that
the letters page already reaches at AD11 and AC11.

**The button carries the letters the page adds rather than a fixed
label**, because the page exists for different characters in every
layout that has one, so `abc` and `123` have no sibling that is true
everywhere. German shows eszett.

**The French button reads `a-grave e-grave` and not `a-grave
c-cedilla`, and that is the rule working rather than a defect.** AZERTY
keeps four letters on the digit row -- a-grave, c-cedilla, e-grave and
e-acute -- and only three of them are new. US International puts
c-cedilla at AltGr+comma; the palette rows carry US International's
levels by construction, so that they stay ASCII whatever layout is
chosen; and the comma is on the numbers page. **C-cedilla is therefore
already typeable in every layout**, and a page offering it again would
be adding nothing.

That was read off a phone as a suspected defect and understood only
after the tables were checked. The check that settled it is one grep:
c-cedilla appears in the French table at AE09 alone, so its
reachability had to come from somewhere that is not the French table.

**And the bug found while chasing it explained none of it.** The list
of added characters was built from `characters_of(alone).cbegin()` and
`characters_of(alone).cend()` -- two calls, two temporaries, iterators
from different containers, undefined behaviour. It is fixed because it
is real, not because it caused the symptom: the label that prompted the
look was correct both before and after. **A wrong theory that finds a
genuine defect is still a wrong theory**, and the comment that first
recorded this claimed the fix explained the label, which it never did.

**Two existing contracts caught this change and both were right.**
Building the page with two rows failed
`every_keyboard_group_is_the_same_shape`, which is not cosmetic:
switching to a shorter page resizes the terminal and reflows the
remote's output. The page is built to the shape the others have rather
than to the content it happens to carry, and its middle row's keys are
simply wider. **Made to fail on purpose afterwards** — admitting the
page unconditionally turns the counting test red with exit 2.

**Three non-Latin candidates needed a toggle, and now ship —
[BUILT 2026-09-06].** Russian, Greek and Ukrainian put Cyrillic or Greek
on the whole letter block, and the ASCII palette carries punctuation
only, so choosing one of them cost `a`-`z` entirely, on both keyboards.
A terminal without the Latin alphabet is not a terminal, so they were
refused until there was something to switch with.

**Latin mode swaps the WHOLE layout, not just the letter rows.** A
language toggle on a real keyboard changes the keyboard, and doing it
here by pointing `g_layout` at US International for the length of the
build means every builder is reused exactly as written — no second code
path for the letters, the digit row, the fourth page or the full board,
and so no second path to go stale.

**It defaults ON, which is the half worth arguing.** Starting in the
layout the user chose and making them find a toggle reads as more
respectful and is wrong here: the first thing anybody types into a
terminal is a command, and a command is Latin. So a non-Latin layout
opens in US International and its own script is one tap away. A layout
change resets the mode, since carrying it across would mean choosing
Greek and getting whichever script the last layout was left showing.

**The button is in the side column and only exists where it does
something.** `bssh_layout_has_latin()` reads the plain level of the AD,
AC and AB slots — the letter block, because a stray Latin character on
the digit row is not an alphabet — and is false for exactly these three.
A Latin layout has no second script to reach, and an always-present
control that does nothing on 27 of 30 layouts is worse than an absent
one, which is §11's absent-not-faked rule applied to a button. It sits
beside settings and hide rather than on a key because it belongs to the
KEYBOARD and not to a page, so the script can be changed from the
numbers page without going back to the letters first.

**The label is the id in capitals, and a script sample was tried
first.** Three letters of the layout's own AD row is idiomatic for
Cyrillic — YTsU is to a Russian keyboard what QWERTY is to this one —
and wrong for Greek, whose AD row opens on a semicolon and a final
sigma. `RU` is short, fits the column, and is the same string the
setting uses. It names what pressing it will GIVE you, the way the page
button names the page it moves to.

**Proved by the contract that already existed.** Disabling the
substitution makes `both_keyboards_reach_every_printable_character`
report *layout ru, style 0 cannot type: abcdefghijklmnopqrstuvwxyz* —
the exact failure the feature exists to prevent, from a test written
long before it. On a device: Russian opens in Latin, the button reads
`RU`, pressing it draws YTsUKEN and reads `abc`, and typing sends
Cyrillic to the wire.

**The setting is global**, at `[input] keyboard_layout`, defaulting to
`us-intl`, with a control on the settings page listed from the catalogue
so adding a layout needs no edit to that page. An unknown id is
**refused** rather than silently replaced, because a settings file
written by a newer version should not quietly become a keyboard whose
letters are somewhere else.


The slot split above was the prerequisite: geometry names XKB positions and
one table says what they carry, so a second layout is a second table. This
is what has to be true beyond that, and it was **measured rather than
assumed** — the same way the US table was built.

**Settled already, three of them by precedent rather than by a new
decision.**

- **Curated, compiled in, addressed by a stable id, identical on both
  platforms** — the pattern §9.2 set for fonts, §10.2 for colour schemes
  and §6.2 for key rows. A layout is the same kind of object and gets the
  same treatment. Whether a user can supply their own is one question for
  all four, and **two of them have now answered it the same way**: the
  colour scheme first (a reserved `custom` entry, written by a push from
  the settings layer) and the key row after it, deliberately copying that
  mechanism rather than inventing a second. ~~What is still open is the
  other two — a user-supplied font and a user-supplied keyboard layout.~~
  **The keyboard layout has answered it the same way since** (re-derived
  2026-09-29: `keyboard_custom_layout` in `app.ini`, one reserved entry);
  a user-supplied font FILE is the one still open, listed in §16. Desktop
  offers installed families (§9.4), which is not the same question. A
  layout differs from a row in one way that matters: a row is refused
  unless it carries Escape and a modifier, and the layout's equivalent is
  `both_keyboards_reach_every_printable_character` — a heavier contract,
  and the reason that one is not simply the row's code with a different
  table.

  **[BUILT 2026-09-21, approved from the proposal below, with one
  correction the build found.] How a user-supplied keyboard LAYOUT is
  expressed.** The other
  three of §6.3's four are values a user picks or composes; a layout is
  the one with no representation to fill in, because today it is code:
  `bssh_keyboard_layout_t` carries `{id, display_name}` and the layout
  itself is a `bssh_layout_t` table the key builders read through
  `g_layout`. There is nothing for a user to write into.

  **The shape is already decided and the code says so**, which is what
  makes this a transcription rather than a design: a layout is ~47 rows
  of `{slot, {level 1, 2, 3, 4}}`, the slot being an XKB name — `AE01`
  the leftmost digit key, `AD01` of the QWERTY row, `AC01` the home row,
  `AB01` the bottom, `TLDE` left of `AE01`, `BKSL` above Enter — and the
  levels being plain, Shift, AltGr and Shift+AltGr in UTF-8. The file's
  own comment states the consequence: *"A Swedish layout is then a second
  table of these same 47 rows and no change to any geometry at all — and
  it can be transcribed from that layout's own xkb definition rather than
  recalled."* So the user's job is transcription from a source that
  already exists, not invention.

  **Proposed form**, in `app.ini` under its own section, because §10.6
  wants that file hand-editable and 47 rows on one line is not:

      [keyboard_custom_layout]
      name = Swedish
      AE01 = 1|!|©|¡
      AE02 = 2|"|@|
      AD01 = q|||
      AC10 = ö|Ö||

  One key per slot, four levels separated by `|`, empty where the slot
  has none — the same `""` the C table uses. A literal pipe is `\|`,
  which is the one escape and is needed because `|` is a real legend on
  `BKSL`. **Level 2 stays empty on letters**, as in every shipped table:
  Shift on a letter is a case mapping and is computed, not a second
  legend, and a user writing `Q` there would be stating something the
  code already knows.

  **Acceptance runs the contract, and refuses.** §6.3 already says the
  catalogue's checks *"become the contract every layout must satisfy
  rather than tests about the two boards we happen to have"*, and the key
  row has since shown what that means in practice: the rule moves out of
  the test and into a predicate the setter calls, because a rule that
  lives only in the test reports the breach after it has happened and
  reports it against the layouts that are correct.
  `both_keyboards_reach_every_printable_character` and
  `a_keyboard_can_reach_every_level_it_shows` are the two to promote.

  **Latin is the one thing a custom layout can get catastrophically
  wrong**, and it is already handled for the shipped ones:
  `layout_has_latin()` computes it, and a layout without `a`-`z` is
  refused the Latin toggle rather than leaving a terminal with no Latin
  alphabet. A custom layout goes through the same function — not a
  declared flag, because a flag is a claim and this is a fact about the
  table.

  **The mechanism is the one already built twice**: a reserved `custom`
  entry in the catalogue, born as a copy of US International so selecting
  it before editing gives a working board, written by a push from
  whoever owns the settings, never by `keyboard/` reading them.

  **The proposal specified ONE escape and needed two, which the positive
  control found and nothing else would have.** `\\|` for a pipe was
  right and incomplete: a level that IS a backslash, written raw, sits
  against the separator that follows it and the parser reads the pair as
  an escaped pipe. So the format as designed could not round-trip US
  International — the layout every transcription starts from — and the
  symptom was a keyboard that could not type `\\`.
  The cause was structural rather than a slip: there was **no writer**.
  The parser existed alone and the test joined the four levels by hand,
  which is exactly how the escaping got forgotten.
  `bssh_layout_slot_spec()` owns it now, beside the parser the way the
  key row pairs its two, so the round trip is a property of the code
  rather than of whoever calls it.
  **All three of the built answers are now asserted at their controls**,
  not only this one. The key row's chooser and the colour scheme's were
  reasoned about rather than checked -- both iterate `all()`, so both
  must list it -- and that reasoning was equally true of `notify()`, the
  palette's own push and the selection handles, each correct one layer
  down and reaching nobody. Both hold; the sweep found nothing, which is
  worth recording because the colour scheme is the feature the other two
  were copied FROM, so a hole in the pattern would have been there
  first. Neither combo had an `objectName` until now, which is why one
  hop was assertable and two were not.

  **And the last hop is asserted on the CONTROL, not the catalogue.**
  `bssh_keyboard_layouts()` can be perfectly right while the chooser was
  built from something else, and every assertion about the catalogue
  would still pass -- which is this tree's most repeated defect, the one
  `notify()`, the palette push and the selection handles all are. The
  test asks the `QComboBox` whether it offers the layout, and a sabotage
  drops `custom` from the combo while leaving the catalogue correct: five
  edits catch nothing and that one goes red.

  **Only a case that must be ACCEPTED could find this.** Every negative
  assertion — a three-slot layout refused, not offered in the chooser,
  the keyboard left intact — passes against a function that refuses
  everything. The control transcribes all 47 rows of a shipped table
  back through the text form and requires them accepted.

  **What this does NOT settle, and is still open.** Whether a 188-legend table is something a person will ever type
  into `app.ini`, or whether the useful version is an import of an
  existing xkb file — which is where the transcription actually comes
  from, and would make the hand-editable form a debugging aid rather than
  the interface. That is a question about who this is for, and it is the
  holder's.

- **The soft keyboard only.** §6.1 already decided this without naming it:
  physical keys arrive as `QKeyEvent` with the OS layout *already applied*,
  and dead keys arrive through `inputMethodEvent`. A Swedish USB keyboard
  already produces `ä`. Applying a layout on top of that translates twice.
  This is forced by the architecture, not a preference.
- **The catalogue validates its members.** Key rows are already checked to
  carry Escape and a modifier; `both_keyboards_reach_every_printable_character`
  and `a_keyboard_can_reach_every_level_it_shows` are the same idea, and
  they become the **contract every layout must satisfy** rather than tests
  about the two boards we happen to have.

**What the measurement found, and the first item invalidates a claim this
document made yesterday.**

- **A layout cannot be read from one xkb file.** `se(basic)` defines
  **zero** keys — it is `include "latin(type2)"` plus `include "se(se)"`.
  Resolving it needs the include chain walked, which in practice means
  `setxkbmap -layout se -print | xkbcomp -xkb`. That resolved to 227 keys
  and all 47 of our slots. **So the generator is a build-time developer
  tool, never a runtime dependency** — which is exactly what "compiled in"
  already implies, and it means the US table's ad-hoc generator has to
  become a checked-in tool. It currently exists nowhere, which is a fact
  recorded without its method.
- **An ISO layout needs a 48th slot, `LSGT`.** Swedish puts `< > | ¦` on
  the key between left Shift and `AB01`, which ANSI does not have. The
  commit that landed the split said a second layout would need "no change
  to any geometry at all" — **that is wrong, and this is the correction.**
  It is a fidelity question rather than a capability one: `<` and `>` are
  Shift+`,` and Shift+`.` on our own symbol page and `|` is Shift+`\`, so
  nothing becomes untypeable without `LSGT`. An ISO geometry is worth
  having and is not on the critical path.
- **Dead keys are the real problem, and Swedish is the proof.** On `se`,
  `AE12` is `dead_acute`/`dead_grave` and `AD12` is `dead_diaeresis`/
  `dead_circumflex`/`dead_tilde`. So `~`, `` ` ``, `^` and `´` have **no
  live key at all** — and a shell without `~` is one that cannot name a
  home directory. Thirteen distinct dead keys appear in that one layout.
- **Five levels exist.** `de` uses a fifth. We carry four, and capping
  there is a recorded limit rather than an oversight.

**The structural answer to the dead-key problem, which the split already
gives us for free.** A layout owns the **alphanumeric slots** and nothing
else. The symbol pages are *ours*: a palette arranged for a thumb, named
by character rather than by slot, and deliberately ASCII. So a Swedish
user gets a Swedish letter block and an unchanged symbol page that still
has `~`, `` ` ``, `|`, `\`, `[`, `]` and `=` on it. **The layout can be as
exotic as it likes and the terminal stays typeable**, and the reachability
test is what enforces that rather than anybody remembering it.

That also says what dead keys are *for* here: fidelity, not access. A Swede
types `~` as two keystrokes on their own laptop, so composing is the
authentic behaviour and not a hardship — but it must not be the only route,
and with the ASCII palette it is not.

**The order of work.**

1. **The table and its generator.** `bssh_layout_t` — id, display name,
   slot table — and a catalogue beside the colour scheme and key row ones.
   `tool/gen-layout.py` runs the xkb pipeline and emits the table, so
   adding a layout is a command rather than a transcription. The US table
   is regenerated by it and must come out byte-identical, which is the
   proof that the tool reproduces what is already shipped.
2. **Selection and wiring.** A setting, a control on the settings page,
   and `bssh_soft_keyboard` asking the catalogue. The reachability
   invariant runs over *every* layout in the catalogue, so a layout that
   would make a shell untypeable cannot be added.
3. **Dead keys.** A compose step: a dead key arms, the next key composes,
   space yields the bare character. Needs a compose table — the subset the
   shipped layouts actually reference, not all of Unicode.
4. **ISO geometry.** `LSGT` and the ISO Enter, as a geometry variant a
   layout can ask for. Fidelity, and last.

**Answered 2026-09-04 by the copyright holder, and recorded here because
the reasoning outlives the answer:**

- **Which layouts ship** -- answered by sourcing them: xkeyboard-config
  has done the legwork, so the set grows by running the tool rather than
  by curation. Swedish first, a fuller set to follow.
- **Global or preset-owned.** Colour scheme and key row are preset-owned
  (§10.2) because they suit a *host*; a keyboard layout suits a *person*,
  and a different alphabet per server would be strange. The recommendation
  is a global setting beside `[input] keyboard=`, but the precedent points
  the other way and that is worth deciding rather than assuming.
- **Whether a user may supply one**, which is the key row's open question
  and the fonts' v2 question, and should be answered for all of them at
  once rather than three times.

**Auto-repeat — [DONE 2026-09-07].** Reported from the phone: holding
Backspace deleted one character, and the arrows were a tap per column,
which makes correcting a mistyped path on a phone worse than retyping the
line. Every key that *sends* something now repeats after 500ms at 20/s;
modifiers, the Fn toggle and the side column (settings, script, page,
hide) do not.

The rule is a physical keyboard's, deliberately, and the alternative was
measured against it rather than dismissed: a phone's own keyboard repeats
Backspace and the arrows and never a letter, which is right for prose
where a stray `aa` is a typo somebody has to spot. It is wrong here. The
thing being typed into is a shell, the user came from a keyboard where
everything repeats, and a rule that repeats some keys and not others is
one nobody can predict at the moment they need it. What makes it safe is
the **delay**, not the exclusion list: a deliberate tap on a phone is well
under 200ms, so 500ms cannot be reached by anyone who meant to press once.
X11's own 660ms is safer still and buys nothing on a key somebody is
already holding down. The interval is Qt's 50ms rather than X11's 40ms,
because each repeat is a round trip through the router into the emulator
and out to the remote.

One helper, `bssh_set_key_repeat()` in `keyboard/key_cap.h`, because the
key row builds plain `QToolButton`s and the built-in board builds
`bssh_key_cap`s: two boards, one rule, and the way it would drift is a key
added to one of them without the call.

**What the end-to-end test cannot reach, recorded rather than left
silent.** The key row grabs a `QScroller` gesture on its scroll area's
viewport, and a scroller holds a press for a quarter of a second before
deciding it is not a drag and replaying it underneath -- so a real thumb
reaches the button late, and whether the repeat survives that is a
question about the scroller. It was written and the instrument could not
answer it: pressing the viewport at the button's mapped position produced
zero bytes, and the control settles which side that is -- a plain TAP
through the viewport produced zero as well, on a row that works under a
finger. `QTest`'s synthetic press does not survive `QScroller`'s filter
under the offscreen plugin. The press that IS asserted goes straight to
the button, which proves the repeat and deliberately does not prove the
reach; the built-in keyboard's grid has no scroller and is unaffected,
and `Bksp` and `Del` live there.

The test is a **partition**, not a list of keys — a keycap repeats if and
only if it is not checkable, and nothing outside the key grid repeats at
all — so a kind added to `bssh_key_t` without a repeat call lands in the
wrong half and fails, where a list could not fail about a key it does not
name. Both halves were seen to fail through their own assertion:
`Bksp` on the letters group with the call removed, and
`side button "keyboard-hide" repeats` with it wrongly added. The
property check says the flag is set on the right buttons; a separate
end-to-end test holds the left arrow for 900ms through the row and the
router and asserts the emulator got more than one `ESC [ D`, because Qt's
auto-repeat is a timer on a *mouse press* and `click()` never starts it —
the first version of that test used `click()`, passed against a row with
no repeat set at all, and proved nothing.

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
| Select text | Click-drag, double-click for word, triple-click for line — **all done** | Long-press then drag handles — **built 2026-09-21** |
| Copy | `Ctrl+Shift+C` (never bare `Ctrl+C` — that's `SIGINT`) | Selection toolbar |
| Paste | `Ctrl+Shift+V`, middle-click on X11/Wayland | Selection toolbar / key row |
| Scroll | Wheel, `Shift+PgUp/PgDn` | Drag, under §8.7's scroll mode — **built**, not `QScroller` |
| Drop a file | Pastes its path, shell-quoted — **built** | No system drag source in practice |

- **Paste always goes through the router** so bracketed-paste mode applies. A
  paste that bypasses it will silently break `vim` and any shell with bracketed
  paste enabled.
- **Long press then drag handles — [BUILT 2026-09-21].** The table asked for
  this from the start and nothing implemented it: a long press opened the
  menu and selected nothing, so Copy there was live only if a selection
  already existed, and §8.7's SELECT mode was the only way to make one.
  The press now takes the WORD under the finger first, using the same word
  characters the double-click uses -- one idea of "word", because two is
  two answers, and a path is what anybody reaches for in a terminal.
  **Two handles, and the target is deliberately bigger than the mark.**
  `selection_handle_rect` answers `BSSH_TOUCH_TARGET_DP` because that is
  what a finger needs; the circle drawn is a sixth of it, because 48dp of
  ink over a terminal hides several cells of the text it exists to help
  select. They hang below the first and last cell rather than over them,
  for the same reason.
  Dragging one re-anchors to the OTHER end and then normalises, so a
  handle dragged past its partner keeps working instead of collapsing the
  selection the moment the two cross.
  **The hit-test has to run before §8.7's input mode, and that was a real
  bug the test caught.** In the default scroll mode `mousePressEvent`
  takes the scroll branch and returns before any selection code runs, so
  the handles were unreachable exactly where they are for -- while
  drawing correctly and hit-testing correctly. Nothing called them.
  Reading found nothing wrong because nothing was wrong; only pressing
  one did. A handle drag is neither scrolling nor a new selection, so
  neither mode should see it.
  Guarded by `tool/sabotage/selection-handles.toml`, five edits.
  **Two were MISSED first time and both were the test, in two different
  ways.** It called `select_word_at()` directly, so deleting the
  selection from the long press changed nothing it could see -- the
  third time in one session that a test has been written against the
  helper rather than the caller, after the lesson had been written down
  twice. It drives a real press-and-hold now, which also surfaced a
  precondition the direct call hid: the gesture is OFF unless something
  enables it, because a pointer keeps press-and-hold meaning "start a
  selection and think about it", and only a window turns it on for
  touch. **A test that skips the real path skips the switches on it**,
  and a switch left off is how a built feature fails to reach anybody.
  The other miss was a fixture that could not reach its hazard: the
  sabotage breaks the START handle's anchoring and the test only ever
  dragged the END, so one of two paths was exercised while the name said
  *handles*. Both are dragged now, and each drag asserts in both
  directions -- the end that moved did, and the end that should not
  have, did not.
- **~~A finger could not reach the scrollback until 2026-09-21.~~ That was
  wrong, and it was committed and pushed before anybody checked it.**
  Touch drag-to-scroll has existed since `c123e12` -- the input selector
  commit -- in `mousePressEvent` and `mouseMoveEvent` under §8.7's input
  mode, with a movement threshold, content-following-the-finger
  direction, the offset measured from the press so a slow drag cannot
  accumulate drift, and a tap told apart from a drag. `BSSH_INPUT_SCROLL`
  is the **default** mode, and there is a whole `bssh_input_selector` for
  switching it.
  **The reasoning that produced the error is the part worth keeping**,
  because every observation in it was true. §6.4's table said
  `QScroller`; the view grabbed no gesture; `QScroller` was grabbed on
  the host list and the host editor but not here; and the wheel handler
  said *"Touch (QScroller, §6.4) will drive the same
  `set_scroll_offset`"*, in the future tense. From four true statements
  the conclusion "touch scrolling is missing" does not follow, and the
  step never taken was **asking whether the thing was built some other
  way**. §8.7 governs exactly this and was never opened.
  A `QScroller` path was therefore added, tested, sabotage-guarded four
  ways, gated and pushed as `1b633d2`, and then removed again: it
  duplicated a working implementation, and by grabbing the touch gesture
  it would take the drag from both the existing scroll AND from select
  mode on a device. **A redundant path that can break selection is worse
  than no path.**
  **The wheel handler's comment is the stale one and stays stale until
  somebody decides.** It promises `QScroller` in the future tense while
  touch scrolling was implemented as a drag in the input-mode system
  instead. A promise in a comment reads exactly like a description of
  what the code does -- that is what made it persuasive -- and whether
  the promise or the implementation is the intended design is §8.7's
  question, not one to settle while deleting code.

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
  **The audit found the READ side and the write side had the same hazard.**
  That third lie — "a blocking read on a master reads as a hang" — was
  guarded by making the master `O_NONBLOCK`. But only a READ notifier was
  ever installed, so nothing said when there was room to WRITE, and `send()`
  answered `EAGAIN` with `usleep(1000)` in a loop with no bound, on the UI
  thread. The file forbids exactly this a few hundred lines up, in the
  comment on `collect()`: *"Never an unbounded waitpid. This runs on the UI
  thread ... would otherwise hang the window, or the entire test suite."*
  The rule was stated, obeyed where it was written, and broken in the
  neighbouring function.
  **The first probe said there was no bug**, and stopping there would have
  been wrong. Writing 1 MB to a pty whose slave is unread in CANONICAL mode
  completes in 114 spins, because the line discipline discards on overflow.
  In RAW mode — what every full-screen program sets — 15,360 bytes land and
  the loop is still spinning 3,500ms later. **The configuration that hides it
  is a bare shell and the configuration that hangs is a TUI**, which is the
  wrong way round for anything found by trying it once.
  It is a deadlock rather than a slow write: spinning on the UI thread means
  output is not drained either, so a child echoing back fills the master's
  read buffer, stops reading its own stdin, and the pty never empties.
  Confirmed by sabotage under an external `timeout -s KILL 40` — the old loop
  is killed at 40s where the fix passes in under one. **Run once, outside the
  suite**: putting a known-unbounded loop into `make check` is the thing
  `running-code.md` exists to prevent, so the suite gets the positive test and
  the negative evidence is taken separately.
  The fix queues what will not fit and installs a write notifier, disabled
  unless something is pending — an always-on one fires continuously while the
  pty has room. Nothing is dropped, which was the old comment's stated
  concern and the reason it was spinning in the first place.
  **It has a test and deliberately NO sabotage entry**, which is the same
  call as the `F_GETFL` guard above and for a sharper reason: the sabotage
  for this one HANGS. Restoring the spin loop makes the suite wait on a
  child that is not reading, and `tool/sabotage.py` runs `make test` per
  edit — so the entry would burn the runner's whole `RUN_DEADLINE_S` (1800s)
  and then be killed, which `run_suite()` reports as *did not run at all*
  rather than as caught by the test that names it. Half an hour to produce a
  verdict that says nothing. The negative evidence was taken once instead, by
  hand and outside the suite, under `timeout -s KILL 40`: killed at 40s where
  the fix passes in under one. Written down rather than left as a silent
  absence, so the next reader of `tool/sabotage/` does not take the gap for
  an oversight.

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

**Dropping the history was the REMOTE's to decide, and now it is not.**
`ED 3` -- what `clear` sends -- has been handled since the scrollback
existed, and the handler's own comment says what somebody is relying on
when they run it: *"before pasting a password"*. Nothing local could ask.
So a user who wanted their own history gone had to hope the far end
cooperated, and **a disconnected session cannot cooperate at all** --
which is the state a restore leaves every session in.

`Clear Scrollback` is on the terminal menu and in the desktop Edit menu,
where konsole keeps it. No confirmation: the screen is untouched, the
scrollback is not a document, and an "are you sure" on a privacy action
is a prompt that makes people hesitate at the moment they least want to.

**One implementation with two callers, and the second sabotage is why.**
`bssh_emulator::clear_scrollback()` clears AND announces;
`on_sb_clear` now calls it rather than repeating both halves. An edit
that removes the announcement leaves a clear that works perfectly and a
view still scrolled into lines that no longer exist -- the defect that
handler's comment already describes, which a second copy would have
reintroduced silently. Both edits seen to fail.

**And it left a selection pointing at lines that were gone --
[FIXED 2026-09-22].** The same verb, two taps away on a phone, met a
hazard the scrollback itself had already named and nobody had connected
to the selection. `bssh_scrollback::clear` advances the base rather than
renumbering, and its comment says why: *"a cleared line is gone rather
than renumbered, so anything still holding a name for one must fail to
find it rather than find a different line under the same name"*. A
selection is two absolute line numbers -- exactly such a name -- and
nothing in the view was listening.

So after dropping the history the view still reported a selection. Copy
stayed offered, and it answered: the evicted lines resolve to empty
cells, the trailing-space trim takes what is left, and what reaches the
clipboard is newlines. A copy that silently yields blanks is worse than
one that is greyed out, and this one is reachable from the menu entry
the paragraphs above added.

**Only when the WHOLE selection has gone, and that half is guarded
separately.** A ring evicting its oldest line under a selection that
reaches down into the live screen has taken part of it, and the part
that survives still copies what it says. Dropping the selection there
would lose it because output arrived, which is the commonest thing a
terminal does -- so the existing
`a_selection_stays_on_its_text_when_the_scrollback_evicts` is the other
sabotage's target, and it fails when the guard goes. Both seen to fail
naming their own test.

**The find matches had it too, and that is the lens rather than a second
instance — [FIXED 2026-09-22].** A `find_match_t` is a pair of absolute
line numbers, so the feature on the same menu failed the same way, and
worse to look at: a selection that has gone quietly stops highlighting,
while the bar's match count is a NUMBER. It went on saying two matches
with neither of them anywhere, Next walked lines that were gone, and the
view scrolled to where they used to be.

**Dropped on the START line, where the selection is all-or-nothing, and
the difference is not an inconsistency.** A selection is a range the
user drew, and half of it is still half of what they drew. A match is an
occurrence of a string, and an occurrence whose beginning has gone is
not a shorter occurrence — it is not one.

**And the count is PULLED, which is a second seam and a second
sabotage.** `update_find_status` runs when the query changes or a step
is taken; nothing else asks. So a view that drops its matches perfectly
leaves the old number on screen until the next keystroke. The view
emits, the pane container relays, the window pulls — three places the
signal can stop, so the wiring test asserts on the TEXT the bar shows
rather than on the view's count.

**And the pictures in it — [FIXED 2026-09-22].** The third instance of
the same lens, one layer down. An image scrolled off the top is placed
at a NEGATIVE row precisely so that scrolling back brings it with its
output (§7.1), which makes it part of the history as much as the text
around it. `clear_scrollback` took the text and left the picture decoded
in memory: unreachable by any route, since with the history gone there
is nothing to scroll back to, and still charged to the 32 MB budget.

**It is the privacy half of the verb as much as the memory half.** §6.4
records why somebody runs it -- *"before pasting a password"* -- and a
screenshot or a plot that was on screen a moment ago is exactly the kind
of thing that motivates the request. Dropping the text and keeping the
picture is the wrong half.

**Guarded in both directions, because the errors are not the same size.**
Keeping a dead picture costs memory quietly; dropping a live one deletes
what the user is reading, which is the thing `enforce_budget` already
refuses to do. The test places one image above the fold and one on the
live screen and asserts which survives, and
`tool/sabotage/history-pictures.toml` breaks it each way.

**And the OTHER verb on that menu had the same shape — [FIXED
2026-09-22].** Reset Terminal exists for "the modes a dead program left
behind", and §6.4 names the classic cause: a binary landing on the
terminal. A binary can open a chunked kitty transmission -- `m=1` means
another chunk follows -- and die. The only things that ever emptied that
buffer were the final chunk and the over-budget abandon, and a program
killed mid-send produces neither, so up to 32 MB sat held for a picture
that was never coming and the next transmission was prepended to it.

**The comment already said a reset emptied it, and nothing did.** The
soft reset goes through libvterm and that buffer is ours, so none of
`vterm_screen_reset`'s work reached it. That is a claim in a header
rather than in this document, which makes it the cheaper kind to check
and the easier kind to leave standing: a test asked it and the answer
was no. The whole struct is replaced rather than the bytes cleared,
because `abandoned` and the pending controls are the same mid-stream
state and a reset that cleared only the bytes would leave the terminal
refusing the next image -- which the test's control catches.

**The lens: state that names something the scrollback can take away.**
It came from the selection fix rather than from a category list, which
is `working-practice.md`'s rule about deriving the next lens from the
last bug. Each instance was found by asking the same question of the
same signal, and the shape is worth keeping: anything holding an
absolute line number outside the buffers that own them.

**And the population was enumerated rather than sampled, which is what
lets this be closed.** The view declares three `qint64` line members --
`m_last_click_line`, `m_selection_anchor_line` and
`m_selection_focus_line` -- and the last two were the selection. The
list holders are `m_find_matches` and the image store's `m_placements`,
both now dropped with the lines they name.
`m_last_click_line` is compared only inside a double-click interval, so
a history that went between two clicks cannot mislead it. The
accessibility layer holds offsets into the flattened stream and is
already correct for a different reason: `note_content_changed()` bumps
the revision the cache is keyed on and announces the text update, which
is the fix §9.5 recorded when the trim case found it.

**And the sweep that found all this had the wrong SCOPE for a day,
which is worth more than the finds.** It walked `src/` -- and this tree
keeps its on-screen keyboards in `keyboard/`, a sibling directory since
§12.2b. Widened, one earlier candidate turns out to have a caller all
along (`steal_char_chord`, called from the key row), and a separate
sweep for signals with no listener went from three findings to one:
`sticky_modifiers_changed` and `locked_modifiers_changed` are consumed
by both on-screen keyboards, exactly as the router's comment says they
are. **A narrow scope manufactures absences**, and the one it
manufactured here would have been read as "the sticky modifier
machinery is wired to nothing", which is the opposite of true. Nothing
published depended on it -- every finding acted on survived the wider
run -- but that is luck rather than method. The scope is `src/` and
`keyboard/`, and the next person to point this lens at the tree should
check what other top-level directories hold code.

**And the menus' enabled conditions were swept against their handlers
and came back clean apart from Show as Tab.** Twenty `setEnabled` sites
in `main_window.cpp`; the three Split conditions agree in substance
across the menu bar, the terminal menu and the tab menu, and the tab
menu's extra clause is right for its different verb. Recorded because
an empty result with no method behind it licenses nothing.

**And the reset path was swept the same way and came back clean, which
is worth recording with its method.** The emulator's other accumulators
-- `apc_buffer`, `osc_buffer` and `selection_buffer` -- all clear on
their sequence's INITIAL fragment, so a half-arrived one cannot prefix
the next. The transmission buffer was the only member of that family
without such a clear, because it is the one that stitches across several
complete payloads, which is exactly why it was the one that broke.
Synchronised output has carried a 150 ms release since it was written,
for the same dead-program case. So the reset verb has one fix rather
than four, and the next reader has the list.

**And the unwired-method sweep was narrower than I said it was.** It
was published as "612 declarations inspected ... now exhausted", and
its pattern required a declaration ending in `;` -- so every INLINE
accessor, the `int attempt() const { return m_attempt; }` shape, was
outside it. There are **120** of those in `src/` and `keyboard/`, never
looked at. Re-run over them: 36 have no caller in `src/`, and reading
them they are overwhelmingly test seams (`panes`, `shown_sessions`,
`scroll_offset`, `colour_for_test`) or read-backs beside a setter that
does have one. Eight have no caller ANYWHERE, and none is a missing
feature -- `soft_keyboard`'s `group` and `group_button`,
`locked_modifiers` where `effective_modifiers` is what callers want,
`used_key_auth` and `disconnect_was_requested`, `working_directory`,
`memory_budget`, `being_dragged`. Listed rather than acted on, because
an unread accessor is not a finding on its own.

**What is worth carrying is the correction, not the list.** "Exhausted"
was a claim about the tree and the instrument could only support a claim
about one shape of declaration. That is the second time in two days this
sweep's SCOPE has been the thing at fault rather than what it measured
-- first the directory, now the syntax -- and both times the wrong half
was the confident sentence rather than the finding.

**The limit of that enumeration, stated so nobody reads it as wider than
it is:** it was a grep for `qint64` members named for a line or a row in
`src/ui` and `src/term` headers, plus the two containers above. A class
holding a line in an `int`, or under another name, is not in it.

**And the shrink sabotage's anchor was the cost of all this, which is
its own small finding.** `scrollback-shrink.toml` quoted the whole
`scrollback_changed` lambda as its anchor, so both fixes above broke it
-- twice in one day, in a spec about neither. It anchors on the connect
now and rebinds to a signal that never fires here, which is what "the
view never hears it" means and does not move when the body grows.

**And the finding came from a sweep rather than from use, which is worth
the sentence.** Asking which methods the header declares and nothing in
`src/` calls, `bssh_terminal_view::clear_selection` came back with the
tests as its only callers -- the same shape as every other find in this
section. The first run of that sweep reported nothing at all, because
its threshold counted a declaration and a definition as two hits when a
definition matches twice; an uncalled method scores three, so the check
could not have fired. It found this the moment the threshold could.

**And the modes a dead program left behind are a user's to clear too.**
`reset_modes()` -- sec 5.3's soft reset, kept soft because the hard
branch erases the screen a reconnect exists to preserve -- had one
caller, the reconnect. Nothing local could ask for it. The classic need
is older than this program: `cat` of a binary leaves mouse reporting on,
the alternate screen up or the keypad in application mode, and the cure
is `reset`, which has to be **typed into the shell that is wedged** --
and a session that has dropped has no shell to type it into at all.

`Reset Terminal` sits beside `Clear Scrollback` now, in a `Terminal`
group on the context menu and flat in the desktop Edit menu. The two
menus differing in shape is deliberate: a menu BAR has no 640dp ceiling,
konsole keeps both of these in Edit, and this menu at 13 top-level rows
is 584px of a cover screen, so a fourteenth would not fit. One row that
opens two is what does.

**One implementation again**: the window calls the same `reset_modes()`
the reconnect does, so a mode the reconnect learns to clear is one this
clears too, rather than a second list to keep in step.

**The runner counts skips now, because of what that disagreement
showed.** A skipped test asserts nothing and reports no failure, so a
sabotage that turns a test INTO a skip printed `MISSED: the suite stayed
green` -- the same words as a sabotage nothing noticed, and the worse of
the two outcomes, since the test is still listed in the suite claiming
to cover what it no longer touches. The baseline line now reads
`N passing, M skipped`, and an edit that raises the skip count says so.

**Proved with a control rather than by reading it.** A scratch spec put
a `QSKIP` at the top of a real test; the runner reported
*"the suite stayed green and 1 more test(s) SKIPPED than at baseline"*,
where before it would have said only the first half. The control was
never committed, and the tree came back clean.

**And the two ways of counting that run disagreed by one, which is
recorded rather than explained.** The suite's own `Totals:` lines sum to
621 passing for this tree and the sabotage runner's baseline reported
620, with no failure either time -- so one test SKIPPED in the runner's
baseline that passed minutes earlier. It does not weaken these verdicts,
because each edit was caught by a test that actively FAILED, which is
evidence independent of any count. It is written down because a test
that sometimes skips is a gate that is sometimes not there, and the
first step in finding one is noticing that two counts of the same tree
did not agree.

**The menu ran out of screen, and the arithmetic says it had to.**
Measured 2026-09-21 by an assertion written for the purpose: flat, this
menu wanted **776px of a 640dp cover screen** with 17 entries. Working it
back -- fourteen verbs at the 48dp floor above, three separators and the
frame -- it had about **16px of headroom before zoom arrived, and a row
costs 48**. So no fourth top-level verb could ever have fitted, and the
three zoom rows were simply the ones that walked into it.

**Grouping is what fits, and the gate is new.** *Text Size* holds the
three zoom verbs and *Pane* the four arrangement ones, which takes the
menu to 12 entries and **520px**. `the_touch_terminal_menu_fits_a_phone`
now asserts it, and names the height and the entry count when it fails,
because the number is the useful half for whoever trips it next. This
surface was not in `every_touch_surface_fits_a_phone` -- it is not a page
-- so nothing had ever measured it.

**The same lens, pointed at the surfaces around it, found nothing —
and what it found is worth the sentence anyway.** Two were swept on
2026-09-21 after the menu:

- **The input selector fits**, and is guarded now. It is the one
  surface a phone user cannot navigate away from (§8.7 keeps it on
  screen the whole time a terminal is), it has no desktop form to be
  noticed on, and it was not in `every_touch_surface_fits_a_phone` --
  the same two properties that left the key row unmeasured until it
  asked for 652px of a 360px screen. Its entry asserts the five state
  buttons exist *before* measuring, because a bar that built none
  would fit beautifully and mean nothing.
- **The tab menu is not a touch surface at all**, which is why it has
  no guard rather than an oversight. `m_tab_order` is the shown
  sessions minus the visible panes, and §8.7's column holds every
  session -- so nothing is ever left to be a tab. Measured by trying:
  three sessions in a shown 900x600 window under forced touch produced
  no tab at index 0, and the code path says why. A phone-fit assertion
  there would have been measuring a state touch cannot reach, so the
  one written for it was deleted rather than repaired.

**And choosing an entry is assertable now, which it was not all day.**
Every test of this menu asserted that an entry EXISTS; none could assert
that choosing it does anything, because the dispatch sat after
`QMenu::exec()` -- a call that runs its own event loop and returns when
somebody clicks, so a headless suite could never reach past it. That
limit was recorded three times today while the day's whole hunt was for
entries that reach nothing: the guard could see only whether they were
drawn.

`act_on_terminal_menu()` is the seam -- the remedy `evidence.md` names,
moving the composition out of the UI shell into code the suite can call.
It adds no behaviour; `show_terminal_menu` builds, execs and hands the
chosen action to it.

**Six more verbs followed, and one of them caught the FIXTURE rather
than the code.** Select All, Copy, Paste, Find, Split and Show as Tab
are asserted to act now. Copy first returned `"s"` -- one character --
because the window in that fixture was never shown, so its pane laid out
at nothing, reported a degenerate grid, and the container resized the
emulator to match. The verb was right and the terminal was one column
wide. The fixture shows and sizes the window now, and asserts
`cols() > 20` **before** selecting a ten-character word, so the same
mistake fails saying "the grid is N columns wide" rather than quietly
measuring a one-column terminal.

**Open link is deliberately excluded**, and this is the record of why:
choosing it hands a URL to `QDesktopServices`, so the test would launch
a browser, and the confirmation in front of it wants an answer no
headless suite can give.

**What it bought, measured**: gutting the Close, Clear Scrollback and
Reset handlers -- each leaving its entry present and enabled -- is now
three caught sabotages against a 624-passing baseline. **This morning
all three would have left the suite green.** The test also asserts a
CROSS effect, that clearing the history leaves mouse reporting alone, so
a verb that quietly did its neighbour's job would fail too.

**A QMenu fails more mildly than a form and it still fails.** Qt gives an
oversized menu scroll arrows rather than squeezing rows until they
overlap, which is what `check_fits` describes; what it costs is entries
sitting behind a gesture nobody looks for.

**Grouping the PANE verbs was a judgement about somebody else's menu**
and is recorded as one: §8.7 put *Move Pane* on this menu precisely so
touch could reach it, and a submenu is one more tap where a removal
would be no route at all. The alternative -- leaving them flat and
dropping something else -- takes the same 48px from a different verb,
and that is a choice worth making deliberately rather than by whoever
next needs a row.

**And grouping introduced a bug that the same pass had to fix.**
`apply_menu_layout` walked one menu's actions; a submenu is a menu of
its own, so the moment anything was grouped its contents went back to
pointer-height rows with their chords still on -- the exact failure that
function's own comment describes, arriving by the one route it did not
cover. It recurses now, and the metrics test builds a submenu of its own
to prove it rather than asserting on the terminal menu's groups.

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

  **And the legacy tier was not legacy, which was measured on 2026-09-21
  rather than suspected.** `send_char` states the rule -- *"A remote that
  has not enabled a keyboard protocol must get exactly what a VT100 would
  have sent, or it sees a sequence it cannot parse"* -- and enforced it
  only for the `@` to `_` run, which is the set that HAS a control byte.
  Everything else fell through to libvterm, whose
  `vterm_keyboard_unichar` emits `CSI u` for any non-letter carrying a
  modifier whether or not anybody asked. So **Ctrl+comma reached a plain
  shell as `ESC [ 44 ; 5 u`** and landed on its command line as text.
  The test was written to fail first and did, printing those bytes; the
  fix drops the Ctrl for a character with no control byte, which is what
  a VT100 did with a key it had no code for. Alt keeps its `ESC` prefix,
  since Alt alone never took the `CSI u` road and was never broken.

  **Three assertions rather than one, because the boundary is the
  interesting part**: `Ctrl+,` and `Ctrl+1` arrive as themselves, `Ctrl+[`
  is still `ESC` -- it has a control byte, so the rule does not reach it
  -- and `Alt+,` is still `ESC ,`. Guarded in
  `tool/sabotage/keys-to-the-wire.toml`, which is where the key-to-wire
  chain's other three edits live.

  **What is deliberately NOT done**: xterm additionally maps `Ctrl+2` to
  NUL and `Ctrl+3` through `Ctrl+8` to `ESC`, `FS`, `GS`, `RS`, `US` and
  `DEL`. That is a keyboard's historical wiring rather than this rule,
  and adding it is a decision rather than a fix.

  **How it survived**: every test of this area asserts a protocol that
  was enabled first, because that is the half that is interesting to
  write. The fallback is what a shell gets when nothing was negotiated,
  and nothing asked it anything until a chord change made somebody look.

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

**Which could not be said at all until 2026-09-22.** The field was
there, stored, validated on load and applied to the session -- and no
editor had a control for it, alone among every field a preset carries.
The only way to give a host a reduced terminal was to hand-edit
`app.ini`, so the sentence above described a capability the program did
not offer.

**Found by enumerating the preset's fields against the editor's
controls**, which is the same question §8.7 asked of its dissolved
surfaces turned on a data structure: not "what does this editor show"
but "what does this record hold that nothing shows". Twelve fields,
eleven with a control, one without.

**A button to a dialog, which is the colour editor's shape and its
argument.** Fourteen checkboxes are a page of their own, and this is the
rarer choice of the two -- `term_features.h` calls it "not a preference
knob: it is a test instrument", so it belongs a deliberate trip away
rather than beside the cursor style. The button carries the state
("All on", "2 off"), because a button that says only *Terminal
features...* answers nothing for somebody scanning the page.

**The list is generated from the feature table**, on the argument the
manual page and the completion are generated under -- and here the stale
copy would be worse than either, because it would be a checkbox that
silently governs nothing. So the test asserts the POPULATION against
`FEATURE_COUNT` rather than checking that a few boxes are present, and
the sabotage that shortens the loop by one is caught by that assertion
alone.

**The same enumeration was run on the other two records and came back
clean, which is worth its method rather than a sentence saying somebody
looked.** The host profile holds thirteen fields; four have no control
in the host editor and all four are right to have none --
`has_stored_password` is the secret store's flag rather than a field
(the remember-me offer is on the password prompt), and `local`, `shell`
and `working_directory` describe a session the command line launches,
which §8.5 settled deliberately: the local shell is "synthetic rather
than stored ... there is nothing to save". The device settings hold
twenty-four setters; twenty-three have a control and the twenty-fourth,
`custom_keyboard_layout`, is file-only on purpose -- its own comment
says a ~47-row layout "gets a section of its own where each line is a
slot a person can find and change", which is §10.6's argument that
`app.ini` is meant to be opened.

**And into `make shots`, which is where the two things no sweep asked
about turned up.** The button read *All on* with no ellipsis, beside an
*Import...* and an *Export...* on the same page -- a status label that
happens to be clickable rather than one that says it opens something.
And the dialog's own minimum was **274x197**, rendered with the
explanatory note clipped through its top and two checkboxes below it:
the note reports its height correctly (that is what `bssh_wrapped_note`
is for) and the dialog was allowed to be shorter than the sum, because a
scroll area's minimum is tiny by design. A floor of three rows fixes it
-- three rather than fourteen, since the list is MEANT to scroll on a
phone and a minimum demanding every row would not fit the screen it
opens on.

**And the instrument had a picture of its own that said nothing.**
`touch-key-row-custom.png` was BYTE-IDENTICAL to
`touch-key-row-default.png`, because the catalogue's custom row is born
as a copy of Default and the shot took it as found: two of the seventy-
one images saying the same thing, and the one named *custom* saying
nothing about what a custom row is. A reader browsing the directory
would conclude the custom row IS the default. It is shot with a row
written the way a person writes one now -- `esc tab ctrl alt gap left
down up right` -- which is also the only picture in which `gap` can be
seen holding its space, since that token is reachable nowhere else. The
catalogue is put back afterwards, like the modifier cycling in the same
loop.

**And the sharpest of them: `desktop-window-split.png` had never shown
a split.** The fixture dropped a third session to create one -- and by
the time it runs the window is ALREADY split, because the drag shot
fifty lines above calls `split_beside_current()` and asserts two panes.
An 800px window is MEDIUM, which §8.1 caps at two, so the drop found no
room, took `drop_session`'s documented fallback, and `show_session()`
replaced the intent with the dropped session alone. **The split the
picture is named for was destroyed by the line that was supposed to
create it**, and the saved image was one terminal filling the window,
under a name promising two, for as long as the shot has existed.

**Measured rather than reasoned about**, after two wrong guesses about
stale geometry: at the drop the layout held width=800, max=2, intent=2,
and afterwards intent=1 with four tabs. The numbers are in the fixture's
comment so the next reader does not re-derive them. The picture now
shows the divider, the focused pane's blue border and the two grids --
which is what the comment beside it always claimed it showed.

**The fix is an assertion as much as a deletion.** A picture is evidence
only if something checked it was of the thing it is named for, and the
drag shot beside it already carried exactly that check. This one has it
now.

**And `shoot()` itself had been resizing every widget while it was
hidden**, which is the same fault the split fixture had worked around
locally. Qt delivers no resizeEvent to a hidden widget, so the geometry
was set silently and everything a widget recomputes on resize kept the
answer it had at construction. Measured on the main window: its status
line elides against the room it has when the text is set and re-elides
from the original on `resizeEvent` -- correct, and never reached, so
`desktop-window.png` read *"nabbe.testing..."* across an 800px window
with acres of room, and its one tab read *"build ...urope)"*. Shown
before it is resized now, and both read in full.

**The workaround was right and the place was wrong**, which is the
transferable half: the split fixture's own comment had diagnosed this
exactly -- *"Qt does not deliver [a resizeEvent] to a hidden widget"* --
and fixed it for one shot out of seventy-one, in the function that calls
`shoot()` rather than in `shoot()`.

**And a design question fell out of it, ANSWERED 2026-09-22 -- see §16,
which records that I answered it rather than the holder.**
Dropping a session onto a window that is already at its pane cap
collapses the split: both panes become tabs and the dropped session
becomes the only pane. Nobody is told, and §8.2's "the layout intent is
remembered per tab-group" does not survive it, because the intent is
what gets overwritten. The sessions are all still there -- §8.2's rule
holds -- but an arrangement the user made by hand is gone in one drag.
Recorded rather than changed.

**All seventy-one were checked for the same fault and two more pairs
came back identical, both honestly so.** `desktop-front-door.png` and
`desktop-page-session.png` coincide because the front door opens ON the
Session page, and `touch-key-row-default.png` matches
`touch-key-row-minimal.png` because the two rows agree for the first
seven keys, which is all a 320dp crop shows -- their `-full` pictures
differ. Recorded with the method (`md5sum` over the directory, pairs
read by hand) because an identical pair is exactly as easy to produce by
a shot that lies as by two things that genuinely look alike, and the
next person should not have to re-derive which is which.

**Neither was visible to a sweep, and that is the entry.** The fit check
asks whether a surface fits 320x640 and this one does; the accessibility
check asks whether controls are named and they are. What found both was
rendering the screen and looking at it, which is the same sentence
`wrapped_note.h` already carries about the defect it exists to prevent.

**The dialog went into both sweeps the day it was written**, which is
the habit §12.3's own record asks for: the phone-fit one, where it
caught the checkbox floor before anybody saw it on a device, and the
accessibility one, where fourteen checkboxes make it the surface with
the most controls in the list. Both carry a population assertion, so
both had their numbers moved with it -- a sweep whose count still says
six surfaces after a seventh is added is a sweep that has stopped
counting.

**And the compiler had been reporting a defect for as long as the token
existed.** `gap` is one of the words a custom key row may be written
with (§6.2's "the row is written as words"), and it did nothing at all:
the row's build switch had no case for `SPACER`, so the token parsed,
the editor's live preview drew a row without it, and the keys after it
closed up. A word in the vocabulary that is accepted and ignored is
worse than one refused.

`enumeration value 'SPACER' not handled in switch` had been riding along
in every build, in a wall of `-Wreorder` noise nobody reads. **The
warning was not a style complaint; it was the finding.** It is a fixed
width now, a key's, rather than a stretch -- the enum's own comment says
"the geometry has to hold the space or every key after it shifts", which
is a gap you can count rather than however much room is spare, and a
stretch would have shared the slack with the row's own trailing one.

**And `check-ci` counts them now, so it stays that way visibly.** The
container's build and test output is captured, printed as before, and
the `warning:` lines counted; the count is reported on every run and the
distinct warnings are listed when there are any. **Reported rather than
refused**, deliberately: making a warning fatal is a build-policy change
-- a new compiler with a new warning would then block every push -- and
that is the holder's to make. What this buys without asking anybody is
that the next one is visible in the gate's own summary rather than
inside a wall nobody reads, which is exactly how the `SPACER` defect
survived.

**The counter was seen to fire before it was trusted.** Run against the
33-warning log from before the pass it reports 15 for the build half and
0 for a clean one, and `set -e` survives both -- which needed `|| true`,
because `grep -c` prints `0` and EXITS 1 when nothing matched. That is
this workspace's own recorded trap, met in the code written to report
its absence.

**The wall is gone, 2026-09-22, and the build is warning-clean.** A full
`make test` -- the app and the test binary together -- now emits **zero**
warnings of any kind, where it emitted 33. The measurement below is kept
because it is what the pass was sized from.

**The proof was the compiler, not the tool, and the difference cost a
run.** The reorder tool checked what `evidence.md` asks a mechanical
change to check: the MULTISET of initialisers unchanged, and the one
initialiser that reads another member (`m_tabs(new
QTabBar(m_terminal_page))`) still behind the member it reads. Both held,
and the result was still wrong -- because the tool's target ORDER came
from its own reading of the header, and that reading had matched
`return m_reverse_video ? m_foreground : m_background;` inside an inline
accessor as a declaration of `m_foreground`. **A gate comparing an
artifact to its own generator**, exactly: the set was right, the target
was wrong, and nothing in the proof could tell. What caught it was gcc
still reporting `-Wreorder` afterwards. The witness disagreed and the
witness was right.

**So the acceptance test for this pass is the warning count rather than
the tool's own verdict**, which is the shape to copy the next time
something rewrites source in bulk here: keep the invariant, and find
something outside the tool that can still say no.

**The wall it hid in is worth a decision rather than a sweep, so here is
the measurement.** The container build emits 33 warnings: 30
`-Wreorder` across ten sites in four constructors, the one `-Wswitch`
above, and one `-Wunused-result`. The last is fixed here -- a test
discarded `qWaitForWindowExposed`, and an unexposed window grabs an
empty frame, so every measurement after it would have been about
nothing. The `-Wreorder` thirty were left at the time, as a judgement
rather than an oversight -- 109 initialisers across the four busiest
files in the tree, which is a deliberate pass rather than a drive-by --
and the pass was made later the same day, on the holder's repeated
instruction to carry on.

**What a pass would need is already established, so it need not be
re-derived.** Checked before deciding: exactly one initialiser in the
four reads another member -- `m_tabs(new QTabBar(m_terminal_page))` --
and it is correct today, because C++ initialises in DECLARATION order
whatever the list says, and `m_terminal_page` is declared 73 lines
earlier. So reordering each list to declaration order preserves it by
construction, and the proof a mechanical pass owes is that the set of
initialisers is unchanged and that this one pair keeps its order. The
other three constructors have no such dependency at all.

**And the first assertion passed for the layout's reason rather than the
fix's**, which the second sabotage caught: `wide > narrow` is true of a
ZERO-width placeholder, because a box layout charges its own spacing for
the extra item. It asks for a key's width now. That is the fourth time
today a MISSED verdict has found a true assertion about the wrong thing
-- the clamp that the setter had already satisfied, the `contains` that
matched a commented-out line, this, and the arrangement pair that
started it.

**And one more claim that outlived its subject, found beside the
others.** `layout_manager.h` described `max_panes()` as "a column shows
at most two (§8.7: one full-screen or two half-screen), regardless of
width" -- true of the column's first increment and of nothing since.
Eight lines above it, `BSSH_COLUMN_PANE_LIMIT`'s own comment says the
opposite and is right: "a sanity ceiling, not a design cap: it holds
every session". **The header contradicted itself, and the stale half is
the one a reader meets first** -- somebody following it would conclude
that Split must be refused at two panes, which is the opposite of what
a column is for. The phrase remains true of the DIVIDER's detent, which
is where `pane_snap.h` still says it, so the fix is to stop the
function's own comment claiming it about the CAP.

**And every preset field is written BACK, which is the same question
pointed the other way.** A control that never reaches `save()` loses
what somebody just typed, and this editor has lost host settings that
way before -- the comment at the top of `save()` records it. All
fifteen are either assigned there or carried in `m_current` by a control
of their own.

**And `to_spec()` has its consumer.** It had none: it writes the off
ones as `-name` and nothing when they are all on, which is exactly the
shape a checkbox dialog produces and exactly what a preset wants to
store. A function whose output shape fits one caller that does not exist
is a design waiting for the other half of itself.

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
- **DECSCUSR is honoured — BUILT 2026-09-18.** The design question this
  entry held open — whether a remote gets to override the preset cursor
  shape — is decided the way every terminal decides it: the preset (§10.2)
  is the DEFAULT and the remote overrides it at runtime, because that is how
  vim shows a bar in insert mode and a block in normal, and a terminal that
  ignores DECSCUSR is broken for its users. libvterm parses `CSI Ps SP q`
  and reports a shape and a blink through settermprop; `on_set_term_prop`
  maps the shape into the preset's own words ("block"/"underline"/"bar") and
  emits `cursor_shape_changed`/`cursor_blink_changed`, and the pane showing
  the session follows. It does not fight §10.2: the remote sets what is
  WANTED, and focus still gates whether the cursor actually blinks, so an
  unfocused pane stays asleep whatever the host asked for. The initial
  screen reset announces a shape too, but before any view is connected, so
  the preset's choice stands until the remote actually asks.
  **It is opt-out**, on the holder's call 2026-09-18: "Let programs change
  the cursor shape" in the appearance settings, on by default, and off
  keeps the preset's cursor whatever the host sends. The preference rides a
  process-wide flag on the view (`honour_remote_cursor_shape`, beside the
  scrollbar and ligature defaults) that `apply_device_settings` sets and the
  pane's DECSCUSR handler reads live, so turning it off takes hold on the
  next change rather than at the next rebuild. Guarded by
  `tool/sabotage/cursor-shape.toml`, six edits seen to fail: the emulator
  dropping the shape or the blink, the pane unwired from either, the
  override that cannot be turned off, and the preference that does not
  persist.
- **DECSCNM (reverse video) is honoured — BUILT 2026-09-18**, closing the
  other half of the entry above. `CSI ?5h/l` flips the whole screen's
  foreground and background; libvterm tracks the mode and the emulator
  forwards `reverse_video_changed`, and the view swaps every cell as it
  paints. The scheme colours are untouched, so an OSC 10/11 query still
  answers what the user configured -- what changes is only what is PAINTED,
  through `effective_background()`/`effective_foreground()`, which the base
  fill and the per-cell background compares read so a default cell matches
  the flipped base and is not repainted. The swap sits on top of the
  per-cell SGR inverse and before the selection swap, so a selection stays
  visible by re-inverting whatever it lands on. Guarded by
  `tool/sabotage/reverse-video.toml`, three edits seen to fail: the emulator
  dropping the change, the pane unwired from it, and the flip that swaps no
  colours. The per-cell `std::swap` in the paint loop is a mirror of the
  SGR-inverse swap right above it, covered by that reading rather than by a
  pixel test; what the tests pin is the state and the effective-colour flip
  the paint reads.

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
apart), and they are **dropped when the grid shrinks past them or their text
leaves the scrollback**, while the memory budget moves them to disk rather
than dropping them, because holding every image a long session ever drew *in
memory* is how a phone runs out of it. This paragraph used to end "there is no image
scrollback", and the **[DONE]** block below — which gives images a scrollback
and a 32 MB budget — contradicted it directly a few lines later.

**Ctrl+click, and a prompt, for hyperlinks.** The URL comes from the remote,
so a plain tap on a line of output must not be able to launch a browser at
an address someone else chose — every terminal supporting OSC 8 asks for a
modifier, and BeerSSH additionally shows the address before opening it. The
widget does not open anything itself: what to do with a remote's URL is the
window's decision, not a terminal component's.

**[BUILT 2026-09-21] A bare URL in plain text is clickable too, and that is
the case that was actually missing.** OSC 8 was the only thing Ctrl+click
could open, and OSC 8 is rare: almost nothing a person runs emits it, so
every address from `curl`, a git remote, a log line or an error message was
inert. `url_at()` reads the LOGICAL line -- the same rejoining `find_text`
does, because a URL worth clicking is long enough to wrap and the half on
the next row is the half with the path in it -- and hands the result to the
same `link_activated` signal, so it inherits the prompt above rather than
opening a second door beside it.

**http and https only, deliberately narrower than the marked path.** An
OSC 8 link is an explicit act by the program that drew it and may carry any
scheme; text that merely LOOKS like a link is not, so what is inferred from
it is held to less. A host printing `file:///etc/passwd` gets no offer to
open it.

**The whole difficulty is where a URL stops**, and the trim counts brackets
rather than stripping a character class. `(see https://example.com/page).`
ends at `page`; handing a browser `page).` is finding the address and
breaking it. But `https://en.wikipedia.org/wiki/Fo_(bar)` keeps its
parens, because the opener is inside -- and no character test separates
those two cases.

**Two of the five sabotages were MISSED first time and both were the
test.** One deleted the CALL from `mousePressEvent` and the suite stayed
green, because every assertion asked `url_at()` directly: a test that calls
the helper it is verifying cannot see a wrong caller. That is the same trap
the session bell hit hours earlier in this same session, written down at
the time, and repeated anyway -- **knowing the trap did not prevent it and
running the sabotage did**, which is the argument for the harness over
care. It drives a real Ctrl+click now, and asserts the negative too: a
click without the modifier emits nothing.
The other fed `https:// nothing` to prove a scheme with no host is refused.
The pattern needs a non-space after the slashes, so it never matched and
the assertion passed having exercised nothing -- **a fixture that does not
reach the hazard is indistinguishable from one that reaches it and finds
the code correct.** `https:///path` matches and has no host.
Guarded by `tool/sabotage/bare-urls.toml`, five edits each aimed at a
different way of getting the end wrong rather than at one way five times.

**[BUILT 2026-09-21] And a link is reachable without a control key, which
it had never been.** Every route in went through Ctrl+click -- the marked
kind and the inferred kind alike -- and a phone has no Ctrl. The
long-press menu offered Copy, Paste and Select All, so on Android the
entire hyperlink feature simply did not exist, on a project whose §14
rule is that a milestone lands on both platforms before the next one
starts. Adding bare-URL detection the same day had made that asymmetry
larger rather than smaller.

This is `working-practice`'s recorded shape rather than a new one: **an
override that only one input device can produce is not an override on the
other**, and the question is not who owns the gesture but what the user
can still reach. The Shift-on-a-phone lockout is the same sentence about
a different key.

`link_at()` answers for a position -- marked link first, then inferred --
and both front doors ask it, so they cannot answer differently. The
window's prompt was extracted into `open_remote_link()` for the same
reason: two ways in and one copy of the question, because a second copy is
a second place to forget it. The entry appears only where there IS a link,
rather than being always present and usually disabled, and its label names
the address elided to 48 characters -- "Open link" alone asks somebody to
authorise something they cannot see, and the whole reason it asks is that
the remote chose the URL.

**The refactor broke a sabotage spec that had shipped hours earlier**, and
`--check` caught it the same day. Routing both doors through `link_at`
deleted the line `bare-urls.toml` anchored on, so that edit could no
longer apply: a guard that had quietly stopped guarding, one commit after
being added. It is repaired by aiming inside `link_at` so the edit still
MEANS what it meant -- the marked path keeps working and only inferred
text goes quiet. **Repointing a stale anchor at the nearest line instead
would have been a different test wearing the old name**, which is worse
than a broken one, and is the reason the specs are held to the tree as
tightly as the code is.

**The fixture found something about the suite, too.** Every other
window-level test here asks a question that needs no geometry -- a cursor
style, a status dot, a tab title -- so none had noticed that a shown
`bssh_main_window`'s view still reports a 0x0 grid until the pane
container has had a resize. This is the first that asks where a cell IS,
and it waits for the layout. Recorded because the next geometry-dependent
window test will meet it as a confusing failure otherwise.

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

**[BUILT 2026-09-26] Past the budget a picture is spilled, not dropped.** The
holder's direction when the budget became a setting: bound what is RESIDENT,
not what is remembered, and load a scrolled-off picture back when it is
scrolled to. `enforce_budget` now writes the picture furthest back to a
`bssh_picture_spill` and drops only the decoded copy; `bssh_image_store::image()`
reads it back when the renderer asks, keeping it while it fits the budget
beside what is resident, least recently drawn let go first -- so a picture
scrolled to stays drawn while it is looked at. The "Picture history" setting
keeps its number and now means memory.

- **One file per picture, zlib at level 1, sealed with AES-256-GCM** under a
  key made per terminal and held only in memory. The picture's serial is the
  nonce and the additional data, so a file renamed onto another picture does
  not open. That is why this is on by default where kept scrollback (§10.5)
  had to be opt-in: a crash leaves files nobody holds the key to. PNG was
  rejected as several times slower on the thread that is scrolling.
- **Where** is `<cache>/pictures/terminal-XXXXXX`, made on the first spill --
  most terminals never draw a picture -- owner-only, and removed with the
  terminal. Each holds a `QLockFile` for its life; at start the app removes
  the directories whose lock is provably stale and leaves alone any with no
  lock yet, which is also what one looks like in the instant another
  instance is making it.
- **Bounded on disk too**: 256 MB of sealed pictures per terminal, the
  furthest back forgotten first. A fixed number rather than a second
  setting; plots compress well, so it is far more than 256 MB of pictures.
- **Where it cannot spill** -- no cache directory, a full disk -- the
  picture is dropped as it always was: the budget is a promise about memory.
- **A picture is forgotten when its text leaves the scrollback**
  (`forget_beyond`), which dropping had hidden: with the budget no longer
  throwing old pictures away, one whose text was gone would have sat on
  disk for the whole session, drawn by nothing.
- **And a picture in the history now moves with it, which it never did.**
  Writing the test for the item above found it: `scrolled()` moves what is
  inside the scrolling region, and a picture wholly above the screen is not,
  so it stopped at the row it left the screen from. Measured at row -1 after
  a hundred more lines -- scrolling back found every old picture stacked
  just above the screen, beside the wrong text. `push_top_line` now moves
  the history's pictures one line per push, except during a resize, whose
  pushes `shift_rows` already pays for.
- **Measured on the emulator 2026-09-28** (x86_64, API 33) with 1000x1300
  pictures of noise, the worst case for compression: 160 ms to spill one,
  49-55 ms to read one back, both on the GUI thread. Both came back beside
  their own text. Not yet measured on a phone.
- **[BUILT 2026-09-29] The spill is sealed on a worker**, the holder's pick
  after that measurement: it is the slow half and it ran while output was
  arriving. `bssh_picture_spill::prepare` hands back a job that compresses
  and seals -- a pure function of copies of the picture and the key -- and
  one worker thread shared by every terminal runs them in turn, so a burst
  of pictures queues rather than taking every core; `store` writes the
  result on the GUI thread, which is quick. The picture stays in memory
  and drawable until it lands, and counts as gone meanwhile, or the next
  scroll would spill a second picture for the same bytes. One that has
  scrolled back onto the screen by then stays and nothing is written.
- **[BUILT 2026-09-29] And read back on a worker**, the holder's next pick:
  49-55 ms was a dropped frame per picture reached. The first `image()` for
  a spilled picture starts the read on a worker of its own -- not the
  spill's, so a scroll back does not wait behind seals going the other way
  -- and returns null, which the renderer already skips; the picture's
  cells are blank for the frame or two it takes. When it lands the store
  calls back, the emulator turns that into `images_changed`, and the view
  repaints. A file that does not open or verify is remembered, so a damaged
  spill is read once rather than on every paint that asks for it.

Guarded by `tool/sabotage/picture-spill.toml`, ten edits.

**[FIXED 2026-09-28] Two faults in placing an image, found by that run.**
A `cat` of five large kitty images placed three: the owed cursor advance
past an image was fed to libvterm after the whole read, and when the next
image's APC had begun in the same read, the advance's ESC landed inside the
open string and ended it -- the rest of that chunk was lost and the image
decoded to nothing. The same lateness hid text: whatever followed an image
in one read was printed before the cursor moved, onto the image's rows. And
the advance was a CUD, which stops at the bottom margin, so an image placed
at the bottom -- where output puts nearly all of them -- kept the cursor in
its own rows whatever the timing. `feed()` now writes in pieces ending at
each string terminator (ST or BEL) and pays the advance between them, holds
a trailing ESC for the read that brings the other half of its ST, and
advances by IND, which scrolls. Every existing test fed one image per read
with nothing after it, which is the one arrangement none of this shows in.
Guarded by `tool/sabotage/image-in-the-read.toml`, five edits.

**And that "never drop what is on screen" rule is what let sixel animation
run the budget over.** Sixel carries no image id, so `place()` gives every
frame a fresh one and appends it — and a program that redraws in place, which
is what an animation is, never scrolls. So the entire run landed in the one
case `enforce_budget()` declines to touch: it drops only what has gone above
the fold, and every frame is on the live screen by construction. The store
grew one decoded frame at a time for as long as the animation played, past a
budget whose own comment says it exists so "a session left running all day
cannot quietly become the reason the app is killed" — which is precisely how
that happened.

**A covered frame is not an image anybody is reading**, which is the
distinction the refusal actually rests on. The renderer walks the list in
order and paints the later placement over it, so not one of its pixels can
reach the screen again. `place()` now drops every earlier placement the new
one paints entirely over — same z SIDE, since a below-text image is painted
in its own pass and so is not covered by one above the text however exactly
the cells line up. Sixty frames become one. A frame that covers only part of
an earlier image leaves it, because the rest is still visible, and that is
what the test checks alongside the count — otherwise it would pass just as
well against a store that simply kept the newest.

**Same shape as the night's others, in a subsystem with no config file in
it**: a bound that held on one path and not on the neighbouring one. Here the
two paths were not two readers of a value but two ways for an image to become
unreachable — scrolled away, which was handled, and drawn over, which was
not.

**There was a third way, and a test was pinning it shut.** A RESIZE pushes
lines into the scrollback exactly as a scroll does — but libvterm reports
that through `sb_pushline`, and only `moverect` reaches `on_moverect` and so
`images.scrolled()`. So on a reflow the text moved and the images did not.
Measured: shrinking a 10-row screen to 5 pushed five lines, `DDD` went from
row 5 to row 0, and the image stayed at row 0 — five rows from the output it
was drawn beside, sitting on top of whatever had taken its place.

`bssh_emulator::resize` now measures how far the buffer moved — the
scrollback count before and after — and shifts every placement by it.
Measured rather than derived, because it is libvterm that decides how much a
reflow pushes. `shift_rows` has no region where `scrolled()` has one, and
that difference is the point: a scrolling REGION must not move what lies
outside it, while a resize moves the whole buffer past the screen, so an
image already at row −3 belongs at −8 after five more lines go by. A grow
runs the same arithmetic backwards, since libvterm pops lines back off the
scrollback.

**`images_are_dropped_when_the_grid_shrinks` asserted the opposite, and its
stated reason was false.** *"The row it sits on no longer exists. Holding it
would leak."* The row does still exist — it MOVES. On that same fixture the
text at row 7 arrives at row 2, still on the live screen, so the old
behaviour dropped an image whose text was three rows further up. That is
word for word the failure the test beside it documents for the scrolling
path: *"a plot vanished as soon as one more line of output arrived, while
the text it was drawn beside was still there to scroll back to."*

It passed because the image genuinely was left behind, so dropping it was
the only way to avoid a placement stranded on the wrong row. **The test
pinned the workaround rather than the behaviour anybody wanted**, and the
name said so out loud — which is the tell worth keeping: a test named for a
mechanism rather than for a promise is describing what the code does, and
the reason in its comment is then a rationalisation rather than a
requirement. It now asserts against the TEXT, which is the thing an image is
supposed to stay beside.

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

**So ` M vterm` in `git status` after a build is normal, and must not be
tidied away.** The gitlink has not moved -- `git submodule status` shows
the pinned commit with no `+` -- and the dirty files inside it
(`include/vterm.h`, `src/screen.c`, `src/state.c`) are exactly the ones
`vterm-patch/` touches. fuzzypickles' session once took it for a stray
edit. `git submodule update --force` or a checkout inside the submodule
silently reverts all five patches; `make veryclean` is the one sanctioned
way back to pristine (checked by the guidelines session, 2026-08-27).

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
| ≥ 840dp | Expanded (tablet landscape, maximized desktop window, external display) | **Up to 4 panes**, nested as the user drops them. |

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

**[DECIDED, and REVISED 2026-09-25]** Cap at 4 panes. This first also held
splits to a single level for v1 -- no tmux-style tree -- to avoid a class of
layout, focus and drag-target bugs. The holder reversed that half when asking
for panes to be rearranged by where they are dropped: "there needs to be a
difference in where it is dropped, left right up down or middle, middle swaps
them". Up and down on a side-by-side pane is a split the other way, which a
single level cannot hold, and the holder chose nesting over two narrower
readings (only the edges along the row, or an edge that flips the whole
window). The cap on COUNT stays: it is what keeps a window from being carved
into panes too small for a shell, however they are nested.

**The shape is a tree the flat list still describes** (`bssh_pane_node` in
`layout_manager.h`). Its leaves in depth-first order ARE the intent, always,
so everything that reads the intent as a list -- the tab bar, focus order,
Move Pane, a column -- goes on working unchanged. `set_intent()` keeps the
shape around whatever list it is given: sessions no longer listed leave and
their split closes up, new ones arrive beside the rest, and a reordered list
moves sessions through the places the user built rather than flattening
them. `place()` is the drop: an edge along the target's own split adds one
more beside it, an edge across it splits the target itself, and the middle
swaps -- or, for a session that was not a pane, takes the target's place and
leaves the target a tab. A window narrower than the tree shows its first
panes in their places (`visible_tree()`), and the rest of the shape is
remembered for when there is room. A column ignores the tree and stacks the
same sessions in order.

**Two defects were in the way of any of this, and neither was the model.**
A drop was wired from the FOCUSED pane only, so a pane dropped on the other
one -- the ordinary gesture -- was accepted by that view and delivered to
nothing. And a pane dropped anywhere in its own window read as "put back
where it was". Drops now come from every pane through the container, the
session is read from the drop while its data exists, and the placement runs
a turn later, since placing rebuilds the panes and the view delivering the
drop is one of them. The zones are a quarter of the pane deep along each
edge, the nearer edge winning in a corner (`bssh_drop_edge_at`), and the one
under the pointer is lit while a drag is over it. Guarded by
`tool/sabotage/pane-drop-zones.toml`, seven edits.

**The shape is saved too — [BUILT 2026-09-25].** `layout.ini` gains a
`shape` key beside the host list, written as `shape_text()` writes it
with profile ids for leaves -- `V(3,H(7,9))` -- and read back by
`parse_shape()`, which resolves each id to a session of that host not
already placed (one host can be open twice) and drops a leaf for a host
that did not come back, the split closing around it. Text that does not
parse whole is nothing saved rather than half a shape. A layout written
before this has no `shape` and restores as it did, the first N side by
side. The save is tested by closing a real window, which reaches the
private `save_layout()` the way the app does; guarded by
`tool/sabotage/layout-shape.toml`, three edits.

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
  **[DECIDED 2026-09-29 by the holder, and BUILT] A window behind another
  program counts as away too.** The worry recorded here -- a dot on every
  glance at a browser -- weighed less once it was clear which dot this is:
  the *bell*-while-away state, raised only when a program rings, not on
  output. So the pane container asks `bssh_platform::is_application_active()`
  beside `isVisible()`, and asks again on `application_active_changed`.
  Application-wide on purpose: a BeerSSH dialog in front of the terminal,
  or a second BeerSSH window, is not "another program". The same flag gates
  §5.5's desktop notifications, so a bell in a window behind a browser now
  raises one, which is what that section's own list said ("a bell in an
  unfocused pane"). The suite forces the answer in front, since the
  offscreen platform never activates anything; `layout_test` flips it.
  Guarded by `tool/sabotage/away-behind.toml`, two edits; the platform's
  forwarding of the real state is not reachable offscreen and says so.
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
  paths are relative to the *binary* and the test binary sits in `build-tests/`
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
  above a row that has not yet offered *Everything*. **None of the checks in
  place could see it**: they ask whether every widget is present, sized
  correctly and inside the page, and all three were true. Only the order was
  wrong, and order is what a picture shows.
  **This entry used to say "no test can see this", and that was too strong.**
  `QFormLayout` exposes `rowCount()` and `itemAt(row, role)`, so asserting
  that each explanatory note follows the control it explains is ordinary.
  Nobody had written it, which is a different fact from nobody being able to.
  The distinction is worth the correction because the two decay in opposite
  ways: **a stale "untested" is harmless the moment somebody closes it, while
  a stale "untestable" goes on deterring the test that would have worked.**
  Found by a sweep of this document's own gap claims — nine of them, of which
  four were already closed in the same entry that stated the gap, two were
  device-dependent facts that will stay true, two were principles rather than
  confessions, and this one was the only live item. It was live because it
  was WRONG, not because it was stale, which suggests the sharper filter for
  a document like this one: **search for claims of impossibility, not claims
  of absence.**
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

**And the same lesson was available one function away, unlearned, where it
cost the phone its keyboard.** `resizeEvent` called `m_panes->rebuild()`
**unconditionally**. On Android the on-screen keyboard's appearance IS a
window resize, delivered repeatedly as it animates — so every frame of the
keyboard opening destroyed every pane view, including the one that had just
asked for the panel, and Qt answers a focus object that no longer accepts
input by calling `QtInputDelegate.hideSoftwareKeyboard()`. The keyboard
flashed up and vanished on every tap. On a phone that is the whole of
typing, and the copyright holder's report was "completely broken on mobile
— no way to use it".

Three things make it a defect rather than a trade-off, and the third is the
one worth carrying:

- **The width does not change when a keyboard opens** — only the height —
  so the rebuild could never have been the right answer to this event.
- **It was rebuilding every pane view per pixel of a desktop window drag**
  too, which nobody had noticed because on a desktop it only costs time.
- **The guard already existed and the caller walked past it.**
  `bssh_layout_manager::set_width()` emits `layout_changed` only when the
  size CLASS changes, `bssh_pane_container` already connects that to
  `rebuild`, and `set_width`'s own comment says a rebuild per pixel is
  precisely what it exists to prevent. `resizeEvent` called `set_width()`
  and then rebuilt anyway. §11 records the same shape in
  `offer_to_remember` — "not a missing check, a correct one positioned so
  it could not prevent the thing it describes" — and this is that a second
  time, in a second subsystem, three weeks later.

**Found on a device and diagnosed from the platform's own log, not from
the code.** `ImeTracker` and `InsetsController` name who asked and why:
a show request, then `onRequestHide at ORIGIN_CLIENT reason
HIDE_SOFT_INPUT_ON_ANIMATION_STATE_CHANGED`, with
`QtInputDelegate.lambda$hideSoftwareKeyboard$6` in the stack that set
`setRequestedVisibleTypes: visible=false`. That is the app hiding it, five
milliseconds after asking for it, and no amount of reading `request_input_panel()`
would have said so.

**Held by `a_resize_that_keeps_the_size_class_keeps_the_views`, which is a
desktop test for an Android defect** — the property is not Android's. It
holds a `QPointer` to a pane view, resizes the height only, then the width
within one class, and requires the view to survive both; then crosses into
a class that fits more panes and requires the view to be **destroyed**.
That last assertion is the control, and it earned its place twice in one
sitting. The first version of the test did not show the window: a hidden
widget is not laid out and delivers no `resizeEvent`, so all three
"survives" assertions held because nothing had happened at all, and only
the control caught it. The sabotage then reported **two** failures for one
cause, because a failing `QVERIFY2` returns from the test function and the
registry cleanup written at the bottom never ran — the leaked session
failed a middle-click test that counts the registry. The cleanup is a
destructor now. **A cleanup written at the end of a test is the cleanup
that does not run on the day it is needed.**


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
  **process**: nothing in this app moves a live session, so a new process
  would mean dropping the connection and reconnecting, losing the scrollback
  and whatever was running. Once the server is real, the same drag becomes a
  client attaching to a session that never moved.
  **This used to say a live SSH session *cannot* move between processes, and
  that was never tested.** It can — see the migration entry below, where an
  authenticated one does, and sshd notices nothing. What holds is the weaker
  claim: libssh's state, the channel and the crypto context are all in one
  process, and nothing here moves them.
- **Moving a live session between processes: the transport half is
  measured, the serialising half is not. [PROBED]** The bullet above says a
  live SSH session cannot cross a process boundary, and gives that as the
  reason a torn-out tab becomes a window rather than a process. That claim
  was never tested, and half of it is wrong.
  `make check-handoff` runs two probes against a real sshd. The first is
  plaintext: it connects, deliberately reads only **ten bytes** of the
  version banner so the rest is still in the kernel, passes the socket to
  another process by `SCM_RIGHTS` together with the ten bytes already
  drained, and the receiving process reassembles the banner, sends its own,
  and **gets KEXINIT back**. sshd sends that only after a valid banner
  exchange and drops anything malformed, so it is the *peer* certifying the
  connection stayed continuous — not the probe certifying itself.
  The second is the case that matters: an **authenticated libssh session**
  with cipher contexts, MAC keys, sequence numbers and an open channel. The
  parent authenticates and runs a command; the child runs another on the
  same session and it works. `fork()` carries the state by inheriting the
  heap, which is the point — it holds the serialising question still while
  the *discipline* question is settled. **fork is not the proposed
  mechanism**: it cannot move a session into an already-running process,
  which is the actual feature.
  So what is left for a real implementation is **laborious rather than
  unknown**: walking `ssh_session_struct` and `ssh_crypto_struct`. Cipher
  contexts do not need serialising at all — for every AEAD/CTR construction
  SSH uses they are reconstructible from key, IV and sequence number, which
  are plain bytes.
  **Two faults are demonstrated rather than argued, and both must keep
  failing** — a probe that can only pass proves nothing.
  `shutdown()` after handing the fd over kills the connection, where
  `close()` is refcounted and invisible to the peer; the encrypted form of
  the same mistake is the departing process calling `ssh_disconnect()`,
  which is why the parent deliberately **leaks** the session and says so —
  leaking is what looks like the bug there, and it is the correct move.
  Reading past the commit point fails in plaintext by stealing bytes, and
  fails **worse** encrypted: both sides advance the same receive sequence
  number, the MAC stops verifying, and it presents as `Socket error:
  disconnected` with nothing naming the cause.
  Two constraints follow and are now measured, not predicted.
  **Compression must be off** — zlib has `deflateCopy` and no serialisation
  at all, so a migratable session cannot carry a zlib stream;
  `bssh_ssh_session::set_compression()` already exists to say so. And the
  handoff **must not straddle a rekey**.
  Kept runnable rather than written up once, because the finding depends on
  libssh internals that move: this is 0.11.5, and a result about 0.11.5 is
  not a result about whatever Debian ships next. **[OPEN]** whether to build
  any of it, and where the emulator would live if so — in the UI, and only
  emulator state needs exporting; in a per-session process, and nothing does
  but the terminal never moves.
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
  new *process* — nothing here moves a live session — and it is why the same
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
- **A pane drag was raised from inside the view's own signal emission, and
  the thing it reaches destroys that view.** `mouseMoveEvent` emits
  `pane_drag_requested`; `start_session_drag` answers it with
  `QDrag::exec()`, a nested event loop that does not return until the
  pointer is released. Every outcome of the drop — another window taking
  the session, this one rearranging, or `tear_out()` when nobody wanted it
  — comes back through `rebuild()`, which `qDeleteAll()`s the views. So the
  view is deleted while its own handler is on the stack, inside its own
  emission, which Qt does not promise to survive.
  **It did not misbehave, and that is the interesting part.** Nothing after
  the emit touches `this`: the two remaining lines are `event->accept()`
  and `return`. The object survives by a property of two lines rather than
  by design, and the next edit to that function has no way of knowing they
  are load-bearing. Reported as a latent hazard rather than a bug, because
  no failure could be demonstrated — the same call as reverting the Split
  fix that fixed nothing.
  `Qt::QueuedConnection` closes it for one word: the emit returns at once
  and the drag begins from the event loop with the handler long finished.
  A `QPointer` guards the session, because deferring by a turn is exactly
  what lets one disappear first — the same guard, for the same reason, as
  the biometric prompt's.
  **The test asserts the DELIVERY, not the crash.** Invoke the view's
  signal, check the container's has not fired, run the event loop, check it
  has. A test for the crash would have to drive a real drag and then depend
  on undefined behaviour showing itself, which is evidence of nothing.
  `deleteLater()` was the other candidate and was not taken: it removes the
  whole hazard class, but a visible child widget then lingers a turn unless
  hidden and reparented first, trading a theoretical crash for a visible
  flicker.
- **The id collision has a second direction, and it DELETES a password.**
  Ids are still minted `highest + 1` over the profiles that currently
  exist, so removing the highest-numbered host frees its id for the next
  one — and secrets are keyed from the id. A deletion the **locked**
  passphrase store cannot carry out writes the key down to be swept later
  (§11's deletion rule), and `bssh_sweep_orphaned_secrets()` erased every
  key on that list unconditionally. So: delete a host that had a password
  while the store is locked, create a new host — which takes the freed id —
  save a password for it, then unlock. The sweep erases the key, and the
  key is now the new host's. Demonstrated before fixing: the new password
  reads back as `""`.
  The entry above is the same collision pointed the other way — a credential
  **reachable** from a host nobody gave it to. This one is the right host's
  credential **destroyed**, and it is the more visible of the two, because
  the user watches a password they just saved disappear.
  The sweep now skips any key a live profile owns, and **drops** it from the
  list rather than keeping it: the id has been re-issued, so whatever the
  deleted host left there is already overwritten by its new owner — keeping
  it would mean deleting that owner's password on every sweep, for ever.
  Both sweep tests run together, which is the pairing that matters: a live
  credential survives AND a genuine locked-store orphan is still swept. One
  alone is satisfiable by a sweep that does nothing.
  **The root cause was id REUSE, and it is closed now.** `save()` mints from
  a counter that only rises, kept in `hosts.ini` as a ROOT-LEVEL key —
  invisible to `load_profiles()`' loop, which walks groups — and raised past
  every id already present, so a file written before the counter existed, or
  one hand-edited to add a host, cannot collide with it. Preset ids keep the
  old rule deliberately: hosts reference presets by SLUG and no secret is
  keyed from a preset id, so reuse there costs nothing.
  **The sweep guard stays, and its test had to be rebuilt to keep meaning
  anything.** It used to construct the collision through reuse, which no
  longer happens — the honest options were to delete the test with the cause
  or to reach the same key another way. The orphan list is process-wide app
  settings while ids are minted per store DIRECTORY, so two stores both hand
  out id 1 and derive the same key; that is the route the test takes now, and
  it asserts the keys really do collide before asserting anything else.
  **The sabotage runner then caught a test of mine that could not fail**, on
  its first real use, which is the argument for the registry in one line. The
  persistence assertion created a third host BEFORE reopening, so the highest
  id was in the file and the migration line recomputed the same answer with
  or without the stored key — MISSED, the suite stayed green. The persisted
  counter only matters when the highest id is ABSENT, which is the deletion
  case the fixture had accidentally filled in. Split into two directories
  now: one for the in-memory counter, one that deletes the highest-numbered
  host, reopens, and asks whether the dead id comes back. All three edits
  caught afterwards.
  **And a restore is not a rebuild.** Running the suite straight after a
  sabotage run reported a failure in the fix that had just been verified
  three ways: the harness restores the source but does not rebuild, so the
  binary was still the sabotaged one and the message was the sabotage
  announcing itself. `restore()`'s `utime` stamp exists so `make` will
  rebuild — it cannot help a caller who skips `make`.
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




**A pane has a drag handle, and a drag says what it is carrying —
[BUILT 2026-09-05].** Four things, asked for together, all about the one
gesture that had no visible existence.

**The handle.** A pane has no chrome — panes are bare views in a
splitter — so the only place a grip can go is on the terminal, where it
occludes cells. Three constraints shaped it:

- It is drawn **only under the pointer**, because two pixel tests
  refused it otherwise: drawn always it put ink at x=590 y=9 in a
  run-of-text check and made a 13-pixel underline in a 21-pixel cell.
  The terminal's pixels are the terminal's, and those tests were right.
- It belongs to a **pane** rather than to a view. `set_pane_handle()` is
  off by default and `bssh_pane_container` turns it on, so a view built
  anywhere else — a preview, a test — has no grip. That is also what
  keeps it out of the pixel tests *honestly*, by those views not being
  panes, rather than by teaching the assertions to ignore a corner.
- A pane too narrow to spare the corner has **no** handle, rather than
  one hanging off its left edge. Found by a test that asked before the
  splitter had given the pane a width: the arithmetic returned x = -25
  in a view 0 wide, which draws nothing and would still have answered
  `contains()` for a press.

**Touch gets no grip, and it was given one first — [MEASURED
2026-09-07].** Touch never hovers, so the hover rule left a phone
without one, and the note that stood here said it lost nothing:
cross-window drag needs more than one window, which a phone has not got.

**That was right about reaching a pane and wrong about arranging one.**
The tab bar and the Detach item both take a pane OUT; neither reorders
two panes against each other, and a folding phone shows two panes
unfolded. So the grip was made to persist for touch wherever more than
one pane existed, with a test asserting it both ways.

**On the device the gesture it enables cannot complete, and the failure
is worse than the gap.** Dragging the grip calls `QDrag::exec()`;
Android makes the whole WINDOW the drag payload, and this device's
window manager claims it as drag-to-split-screen. The app shrinks to a
thumbnail over the wallpaper with a Cancel chip, the drop lands in the
system's app picker, beerssh is halved into multi-window, and
`dropEvent` never runs. Measured at two drag distances -- one crossing
the window, one staying well inside it -- and confirmed by the copyright
holder by hand: *when I grab it to move, it disappears quite easily.*

**So the grip is hover-only again**, which is §11's absent-not-faked
rule applied to a control rather than a capability: a handle that hands
the window to the platform is worse than no handle. `touch_gets_no_pane_grip`
pins the absence so it cannot come back unnoticed.

**Reordering moved to the menu instead, where nothing can intercept
it.** *Move Pane Left* and *Move Pane Right* are on the terminal's
context menu, enabled only where there is somewhere to go, and they
move the INTENT rather than the visible list -- the intent is what the
layout rebuilds from, so reordering the visible subset alone would be
undone by the next rotation. Focus follows the pane rather than the
position, since somebody who just moved a terminal is still working in
it and the alternative hands their next keystroke to a different host.
A move off either end does nothing rather than wrapping, because a wrap
makes "left" mean "far right" at the moment a user is looking at the
leftmost pane.

**Verified on hardware, both halves — [2026-09-07].** On a phone screen
and on the Fold folded, both items are greyed with one pane and there is
no grip. Unfolded, with two panes and focus on the LEFT one, *Move Pane
Left* is greyed and *Move Pane Right* is live -- the end-stop, on the
device rather than only in the test -- and choosing it swaps the panes
with the focus border following the session that moved. No drag, no
system picker, and the app stayed in the foreground throughout.

The neighbouring items move with the arrangement as they should: with
one pane *Split Beside Current* is live and *Show as Tab* greyed, and
with two the pair reverses, a third pane not fitting.

**Getting there cost three cables and four ports**, and the fault was
the Fold's cable rather than its socket or the hub -- worth recording
only because two false diagnoses were published on the way, one blaming
the hub and one blaming the phone's USB mode.

**The same lens was pointed at the TAB drag and came back negative,
which is the useful half.** Both gestures reach one `QDrag` --
`start_tab_drag()` calls `start_session_drag()`, the tree's only drag
site, with no platform branch anywhere near it -- so a tab drag was
predicted to be claimed by the window manager exactly as the grip was.
It is not: measured on the Fold, dragging a tab starts no system drag,
the app never leaves the foreground, and nothing moves. Confirmed by
hand.

**Nothing moves because the bar is deliberately not movable on touch,
and §8.3 already says why**: the tab bar scrolls horizontally on a
phone, and a bar that both scrolls and rearranges under one drag picks
between them by accident. `m_tabs->setMovable(pointer)`, eight hundred
lines from the grip, is that decision -- so this project had already met
the class of problem the grip walked into, and answered it by refusing
the gesture rather than shipping it half-working. That is the standard
the grip should have been held to, and the answer was in the same file.

**The asymmetry it left is closed — [BUILT 2026-09-07].** Panes could be
reordered on touch through the menu and tabs could not, the drag being
off by design and nothing replacing it. *Move Left* and *Move Right* are
on the tab's own menu now, above the separator so the destructive verb
stays alone below it, at the holder's instruction.

**The first version moved the wrong list, and only the device said so.**
It reordered the REGISTRY, and that is wrong twice. The registry holds
the sessions shown as panes, which are not in the bar, so a step past
one of those moves nothing a user can see: measured on a phone, the item
fired, the session went from registry index 0 to 1, and the bar did not
move. And the registry is shared between windows, so reordering it
rearranges every other window's tabs -- which the desktop drag handler
had said in a comment since long before, eight hundred lines from where
the menu was being written.

**So the drag's own logic became `move_tab()` and both callers use it.**
One implementation, two entry points; the menu having a second and worse
copy is exactly how this arrived.

**The test that separates them is the last TAB that is not the last
session.** With three sessions -- one on screen as a pane, two in the
bar -- the second tab is last in the bar and not last in the registry,
so a registry-based Move Right is offered and does nothing while a
bar-based one is correctly refused. It also asserts the registry's own
order is untouched, which is the two-window half.

**Verified on the device as far as the hardware allowed**: both items
present, *Move Left* greyed on the first tab and *Move Right* live with
a tab beside it. **Not verified: that the fixed version visibly swaps
two marked tabs.** Three attempts could not establish it because
selecting a tab stopped switching the shown session -- which worked on
the previous build twenty minutes earlier and wants looking at -- and
the phone left the USB bus mid-investigation.

**And the general lesson is the one this section keeps paying for.** The
grip's visibility was tested, asserted both ways, sabotaged and seen on
glass, and none of that touched whether the gesture it exists for can
finish. A correct function is not a working feature, and the device is
where the difference showed.

**What the pointer carries.** Qt's default is no pixmap at all, so a
session drag looked like a text drag, a file drag, or nothing. It now
carries a small painted terminal with the session's name beside it —
painted rather than loaded, since this program ships no icon theme and a
missing asset would leave the gesture looking like nothing again.

**What is left behind.** The pane a drag picked up is hatched with a
diagonal net and outlined in the accent, drawn last so nothing — not the
focus ring, not the grip — paints over it. A net rather than a tint,
because a tint over a dark scheme is a slightly different dark and reads
as nothing. Held through a `QPointer` around `exec()`, which runs a
nested loop: the view is destroyed when the session lands elsewhere, so
the pointer would be dangling by the time the drag returns.

**Dropping it back does nothing**, which is what cancelling should look
like — and it was not true before. The drop went through take-and-show,
and on a window with no room for another pane that means
`show_session()`, which replaces the intent with the dropped session
*alone*. So putting a pane back exactly where it came from collapsed the
split it belonged to: **a gesture whose undo is destructive, which is
worse than one that cannot be undone.**

Pictured in `desktop-pane-being-dragged.png`, because a net over the
right pane and a net over the wrong one are the same assertion and
different screens — and in `desktop-session-drag-cursor.png`, since
`QDrag::exec()` is a nested loop waiting on a real pointer and nothing
headless can drive it. **A pixmap nobody has looked at is the same kind
of claim as a keycap nobody has looked at**, so it is rendered on its
own rather than trusted.

**On X11 the session drag is the app's own, not the platform's — [FIXED
2026-09-25].** Reported by the holder: dragging a pane outside the window
"wants to create a named file or something on the desktop". A `QDrag` is
offered to every program under the pointer, and TDE's desktop accepts a
drop of ANY data -- it asks for a filename to save it under -- so the drop
was accepted, `exec()` returned an action rather than `IgnoreAction`, and
`tear_out()` never ran. No payload format fixes that, since that desktop
takes all of them; the only fix is for nothing outside the app to see a
drag. So on xcb `start_session_drag` runs its own: a small picture beside
the pointer while the app holds the mouse and keyboard, the zone under the
pointer lit on whichever of our windows it is over, and on release
`finish_session_drag()` decides -- a window's tab row makes a tab, a pane
places by zone, the rest of a window makes a pane beside, and anywhere
else tears out into a window of its own at the release point. Esc
cancels, and the loop also ends if the button is found up without a
release having arrived, so a grab lost to another program cannot leave it
running. Other platforms keep `QDrag`; Wayland does not report where the
pointer is outside our own surfaces, which this needs.
**Tested at the release, not the drag**: `finish_session_drag` is driven
at a point outside every window and at another window's pane edge
(`own-session-drag.toml`, three edits). The loop itself holds a real
pointer grab the offscreen suite does not have, so whether it feels right
on TDE is the holder's to try. It carries the same `session_drag_pixmap`
the platform drag did.


**Dropping a pane on the tab bar missed most of the tab bar — [FIXED
2026-09-05].** §8.3 says where a drop lands is what it means: the bar
makes a tab, the panes make a tile. Driven with a real pointer, the
un-split worked when the drop landed **on a tab** and did nothing at all
a few hundred pixels to the right, on the same row.

The hit test asked the tab bar WIDGET, and a `QTabBar` is only as wide
as its tabs — 123 pixels of a 1000-pixel window with one tab open.
Measured: a drop at window x=60 gave `on_the_bar=1` and un-split; the
identical drop at x=340 gave `on_the_bar=0`, so it counted as a drop on
the panes, where the drop-back guard correctly swallowed it. **Nobody
aiming at "the tab bar" is aiming at a widget**, so the test is on Y
alone and the whole row answers.

**Instrumenting was what settled it**, after two rounds of reasoning
from the screen produced two different wrong stories. One `qWarning` in
`dropEvent` gave the coordinates and the verdict, and the fix followed
in a line.

**And the test holds the DECISION rather than the drop.** A synthesised
`QDropEvent` never reaches `dropEvent()` — established the same way, by
watching other tests' drops appear in the instrumented log while this
one's did not, so Qt's drop delivery wants a real drag. The geometry is
therefore named — `point_is_on_tab_row()` — and asserted at three
points: on a tab, the same row well beyond the last tab, and below the
row. The middle one is the bug and the third is what stops the fix
swallowing every drop meant for the panes.

**Driven end to end with a real pointer, once xdotool was installed.**
Under Xvfb with a window manager, against two real local shells: press
the grip, watch the net and the carried terminal, and release. It found
one bug that no test had, and confirmed the three that had only been
asserted.

- **Dropped back inside the pane it came from: nothing changed.** Same
  two panes, same scrollback, focus intact.
- **Dropped on the empty desktop: torn out into its own window**, the
  second shell going with it.
- **Dragged from that window back into the first: it merged** — and left
  the emptied window on screen, titled "BeerSSH", status line reading
  *no session*, a grey rectangle doing nothing.

**A window that gives up its last session closes now.** The rule is not
new: `tear_out()` already refuses to empty a window because the result
is useless, and a drop into *another* window empties one just as
thoroughly with nothing enforcing it there. Never the last window, which
would quit the program on a gesture about rearranging it.

**`close()` rather than `deleteLater()`, and a test aborted to say so.**
A window need not be heap-owned — most of this suite builds them on the
stack — and `deleteLater()` on one of those had Qt trying to free stack
memory, SIGABRT, in a test about password prompts that had nothing to do
with dragging. `close()` lets each window keep its own policy: one made
by `tear_out()` carries `WA_DeleteOnClose` and goes, one that does not
simply hides.

**How far a test can follow the gesture, and where it stops.**
`the_grip_asks_for_a_drag_once_the_pointer_moves` presses the grip and
moves, and asserts the view asked for a drag — and asserts it did NOT
ask on the press alone, since a press that rearranged the window by
itself is exactly what the Alt gesture was careful to avoid and a handle
must not reintroduce. It runs against a **bare view**, not through a
window: through a window the request reaches `start_session_drag()` and
the test would hang in `exec()`, which is the same trap that made an
earlier cleanup look like a fault in the code under test. Everything
past the request needs a real pointer and is covered by the pictures.

**A torn-out window has no tab bar, and that is the other half of the
report — [OPEN, found 2026-09-05].** Rendered rather than reasoned:
`desktop-detach-source.png` and `desktop-detach-torn-out.png` are the
same window before and after, and the pair says what no assertion did.
The source keeps a tab and a terminal; **the new window has a menu bar
and a terminal and nothing else.**

That is §8.3 working as written -- a pane is not in the bar, and a
window showing its only session shows it as a pane -- so the bar has
nothing to list. It is also why detaching feels one-way. The session
leaves through a menu item, and the route back is the Alt+drag nobody
discovers, because there is no tab to take hold of.

**Not fixed here, because the fix is a design choice rather than a
defect.** Listing a lone pane in the bar would contradict §8.3's rule
that the bar holds what is *not* on screen; adding a "Move to Other
Window" item is a new piece of UI; and drawing a drag handle on a pane
is a third answer. Which one is the copyright holder's call. What is
established is the shape: **the inverse of Detach has no discoverable
route**, and the pictures are in `build-shots/` for whoever takes it.

**Detach takes the session you are LOOKING at — [FIXED 2026-09-05].**
Reported as "detach tab to window does nothing", together with "there is
no way to drag a pane out". They are the same fault seen twice.

The item read `m_tabs->currentIndex()` and was called "Detach Tab to a
Window", which is wrong twice over on a wide screen. A split shows
sessions as PANES, and a pane is not in the tab bar (§8.3) — so the item
either acted on a different session than the one with focus, or, with no
tab, on nothing at all. Both read as *it does nothing* from the other
side of the screen.

It is also **the only route a pane has out of a window that anybody
finds**. Dragging one out is Alt and a drag, which works and which
nobody discovers; a drag nobody accepts falls back to `tear_out()`, so
the gesture is complete and invisible. A menu item that does the same
thing is the discoverable half.

`the_detach_action_makes_a_window` covers both halves of the guard — one
session refuses, because a window will not empty itself, and more than
one does not — across a tabbed window and a split one.

### 8.5 The front door — **[DECIDED]**

**Android's Back leaves the PAGE, not the app — [FIXED 2026-09-06].**
Reported from a phone: open the settings from a session's keyboard,
press the system Back, and the whole app went to the background with
the settings page still showing underneath when it came forward again.
Qt delivers the system Back as `Qt::Key_Back`, and a window that does
not handle it lets Qt close the window — so the app left while the page
it should have left stayed exactly where it was. The only way out was
the page's own on-screen Back button.

**It must not be swallowed at the bottom of the stack.** Back at the
terminal or the host list is a request to leave the APP, and Android
backgrounds on an unhandled Back, so `go_back()` returns false there and
the event is left ignored. Eating it would strand a user pressing a key
that does nothing, which is the opposite failure and just as bad.

**And Back returns to the page it came FROM, which was a second fault in
the same place.** Every sub-page returned to the host list whatever
opened it — right from the front door and wrong from a session, since
reaching the settings from a live shell's keyboard and pressing Back put
the shell out of sight. Entry goes through `show_page()`, which records
the current page; `go_back()` returns to it and falls back to the host
list.

**The test asserts the EVENT, not only the page.** At the bottom of the
stack "the page did not change" is true whether Back was handled or
ignored, and those are opposite behaviours — so a page-only assertion
would have passed with the handler deleted. Made to fail: disabling the
handler reports *Back on the settings page was not handled* with the
accepted flag false, exit 2.


**Desktop borrows PuTTY's interface IN SPIRIT. Touch gets a mobile one.
Same model underneath.**

**"In spirit" is the copyright holder's own correction, 2026-09-11**, and
it matters because this sentence has been quoted to refuse things: on
2026-09-10 it was cited to rule out a category tree on touch and to keep
the settings page whole as the "This device" category of that tree.
*"I only meant to follow putty in spirit, not exactly. Its config menu is
far from perfect."* Borrow the recognisability, not the layout -- and
sec 8.7 supersedes the front-door half of this section.

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

### 8.7 Portrait mode, one config menu, one input selector — **[AGREED 2026-09-11, BUILT EXCEPT WHERE NOTED]**

Recorded from the copyright holder's own words rather than inferred, and
deliberately ahead of the code: this changes decisions sec 8.5 currently
states, so the document has to say the new thing before anything is
built against the old one.

**The heading said NOT YET BUILT until 2026-09-22**, by which point the
column, the scroll area, the five-state selector, the arrangement
setting and the scroll's own snap had all landed and said so in their
own paragraphs -- so the section disagreed with itself, and the half a
reader sees first was the stale one. What is still outstanding is named
where it belongs rather than summarised here, and is three things: the
`row` arrangement, which has no decision and no consumer; the kinetic
flick, which is the device pass; and the snap radius, which sec 8.7
asks to be configurable and has no settings plumbing.

**The verdict that prompted it: "The UI on Android sucks and makes no
sense."** Not a defect report -- a judgement about the structure, after
a day spent fixing details inside it.

**PuTTY was the spirit, not the letter.** sec 8.5 says the desktop
"borrows PuTTY's configuration window" and is marked DECIDED, and it has
been read strictly: on 2026-09-10 it was cited to refuse a category tree
on touch and to keep the whole settings page intact as the "This device"
category of that tree. The holder's correction: *"I only meant to follow
putty in spirit, not exactly. Its config menu is far from perfect."* A
sentence quoted to refuse things needs to say what it means.

**Exactly one config menu.** Today there are several roads: the host
list's overflow offers Import, Presets, This device and Licences, and
"This device" opens an index that offers Licences again. Presets and
import sit outside config entirely. Presets move INTO the config menu
and stay editable -- and remain reachable from the picker too, since
that is where a preset is chosen. Two roads to one page is a shape this
tree already uses.

**THREE INDEPENDENT DIMENSIONS, which is what the naming bought.** They
had been one boolean -- `is_touch_primary()`, 36 calls across `src/`, 12
of them in `main_window.cpp` -- answering questions that are not the
same question:

    idiom       mobile | desktop      front door, config shape, Back,
                                      whether there is an overflow menu
    arrangement tiled | column | row  how terminals occupy the window
    metrics     auto | on | off       target sizes, kinetic scrolling,
                                      row heights

**"Desktop" names the idiom and nothing else**, which is why the
arrangement values are geometric. `tiled` is today's `pane_container`,
`column` is the portrait scrollable one, `row` its landscape mirror --
**not built, and answered as not wanted** (re-derived 2026-09-29): the
holder's 2026-09-25 rule below gives the cover screen sideways one
terminal in the column, which is the case a `row` was for.

**The idiom picks the arrangement's default and then lets go.** Mobile
defaults to `column` and desktop to `tiled`, and either can be set to
anything -- which is the point rather than a concession: `tiled` has to
be runnable on a phone and `column` on a desktop, or neither can be
tested without two devices. Every layout question today was slowed by
exactly that.

**The split is scaffolding until a feature needs it, so it waits for
one. Classified 2026-09-17.** All 37 `is_touch_primary()` sites were read
and sorted: about five are IDIOM (`front_door_for`, `opens_a_local_shell`,
the native-vs-Qt file pickers), about thirty are METRICS (touch heights,
default font, long-press, splitter handle width, movable tabs, the
scrollbar-auto default), and NONE is ARRANGEMENT, because `column` and
`row` do not exist yet. Since all three dimensions equal
`is_touch_primary()` today, splitting the boolean now would rename 37
sites and change no behaviour and give nothing a consumer that sets the
dimensions apart -- a sweep with no request behind it. So the split lands
WITH the first feature that makes a dimension diverge (the portrait
`column`, or a touch-desktop metrics toggle), introducing only the
dimension that feature needs, not as a standalone refactor first.

**The form-stack bug had no siblings, and that is a measurement.** The
same classification was the lens the form_wrap fix earned -- a site that
keys off TOUCH where it should key off WIDTH is a live bug on the
unfolded Fold (touch and wide at once). Read against all 37: the metrics
sites correctly want touch on an unfolded Fold, and the layout-choosing
ones (settings page, preset editor) are already width-aware
(`WrapLongRows`) or are file-picker idiom rather than width. The two
`WrapAllRows` calls were the only instance of the shape, and they are
fixed. So this is not systemic; the next Fold layout bug needs a new lens.

**A touch desktop is the case that could not be expressed before.** It
wants the desktop idiom with touch metrics, and one boolean could only
offer both or neither. The two `WrapAllRows` calls added on 2026-09-10
were the live instance: they keyed off touch, so unfolding a Galaxy Z
Fold 3 -- touch on both screens -- produced the 320dp cover screen's
stacked form stretched across the inner screen's 1768px, while a desktop
window dragged to 400px kept two columns and starved exactly the fields
a phone is stacked to protect.

**The two figures, measured on the device 2026-09-11:** the cover screen
is 840x2289 and the inner 1768x2208, both at density 420, so **320dp and
674dp**. An earlier draft of this section said the unfolded screen was
840dp -- which is the COVER screen's pixel width wearing the inner
screen's name, and it was wrong in a way nothing downstream would have
re-derived. The size-class table in sec 8.2 had it right the whole time:
Medium is 600-840dp and names "unfolded foldable" among its cases, so
674dp lands there and gets two columns. **A sentence that contradicts a
table four thousand lines up in its own document is a sentence nobody
checked against it.**

**FIXED 2026-09-11, ahead of the rest of this section**, because it did
not need any of it: a form stacks when it is NARROW, and sec 8.2 already
says what narrow is. `ui/form_wrap.h` asks that -- COMPACT stacks,
anything wider does not -- and watches the page, so unfolding re-lays it
out rather than leaving the phone's answer on a screen that is no longer
a phone's. A threshold of its own would have been a second answer to a
question the tree had already settled for the pane count.

**Screenshotting this device needs the display named.** `adb exec-out
screencap -p` with two displays prints a warning -- "no display id was
specified ... this default is not guaranteed to be consistent across
captures" -- and then the PNG, so the file is a banner with an image
glued to the end of it and `file` calls it `data`. Pass
`-d <id>` from `dumpsys SurfaceFlinger --display-id`: the inner screen is
4630947232161729154 and the cover 4630947232161729155. On a device whose
whole point is having two screens, a capture that does not say which one
it is is not evidence.

**Verified on the hardware, across the hinge.** Installed on the Fold 3
2026-09-11 and opened at Add host: folded, labels above fields; unfolded,
labels beside them -- and `am start` reported "its current task has been
brought to the front", so it was the same activity re-laying-out rather
than a fresh one built at the new width. That is the half a headless test
cannot reach, and the half a constructor-time decision would have failed:
the page was BUILT at 320dp.

**The folded picture on its own proves nothing**, which is worth saying
because it is the one that looks like the result. The cover screen is
narrow AND touch, so the code this replaced drew it identically. Only the
unfolded shot separates the two versions, and it is the one that needed a
person to open the phone.

**Guarded, and the guard seen to fail.** `tool/sabotage/form-wrap.toml`
throws the width away and returns a constant -- which is what keying off
touch did on a Fold -- and the verdict is `caught` rather than WRONG
TEST against a 549-passing baseline. Only that direction is sabotaged:
a form that NEVER stacks is already held by
`check_free_form_fields_own_their_row`, which asserts the geometry at
320dp and is reached first, so a second entry would report WRONG TEST
and read as a hole that is not one. `narrow-screen.toml`'s two editor
entries anchor on the calls now rather than on the policy lines that
have gone -- two entries and not one edit in the shared helper, since an
edit there could not say which editor had stopped asking.

**Deriving one from the fields' own font metrics was tried first and
dropped, and the reason generalises.** It compared the room left beside
the label against forty characters of the page's own font -- forty
because the two strings the editors' comments record are 38 and 39
characters. On this machine's metrics that lands within a few pixels of
the 245px those comments measured, which is the trouble: the phone's
verdict was a few tens of pixels from flipping, and every term in it
moves with the face the machine draws in. sec 12.3d already records the
font picker's two-column minimum as 222px here against 285 to 302 in the
CI container, on identical source. **A layout the container and the
device can disagree about, while both are "measured", is worse than a
coarser rule they must agree on.** The arithmetic was never checked
against a running build -- it is recorded as an approach rejected, not
as a measurement.

**Exactly one config menu -- DONE 2026-09-11.** The overflow is `Import from SSH config` and
`Settings`; Presets is the first row of the index, above the five device
sections, because those describe this device and a preset describes every
host that resolves through it (sec 10.1). `presets_requested` and
`licences_requested` are gone from `bssh_host_list` rather than left as
signals nothing emits. Import STAYS on the host list: it adds hosts and
is not a setting, and this section only says Presets moves in.

**Guarded, and both guards seen to fail.** 550 passing with it, and
`tool/sabotage/one-config-menu.toml` breaks it the two ways it can
actually break -- a row added without the page it indexes, and a config
page given a second road as Licences had. Both report `caught` rather
than WRONG TEST, so the check that spoke is the one named.

**Still owed: the menu read on the device.** A QMenu's rows are actions
rather than widgets, so `bssh_apply_touch_heights` cannot reach them and
no headless assertion sees their height -- they came up 23dp on a Note 9
once, against sec 6.2's 48dp floor. `bssh_apply_touch_menu` exists for
exactly that and this change did not touch it, but the menu has one
fewer entry now and nobody has looked at it on a phone.

**The index maps rows to pages BY POSITION, in two separate loops, and
this change prepends a row to both.** That is the tab bar's off-by-one
shape below. The test clicks every row and asserts the page that arrives
is the one the row names; the other half asserts the menu and the index
share no entry, as an intersection rather than by pinning either list, so
it fails the moment a page acquires a second road. Against the old code
it fails naming `Licences`.

**The host list becomes a picker, not a front door.** It appears when a
terminal is being created, and when there is none: *"if it has no
terminal, one has to be picked for the window to make sense."* So the
first-run screen does not change; what changes is that a returning user
with sessions lands in a terminal. The picker also carries the route
into the config menu.

**What opens on an empty window is the IDIOM's answer, and the code
already holds it under the wrong name.** Desktop opens a local terminal;
mobile opens the picker.

**This is about what OPENS, never about what exists.** The local shell
stays the first row of the picker on every platform, which sec 8.5
already settles -- always there, and "present on touch as well, even
though a phone does not *open* with one". The holder's reason for
keeping it is that the population is not uniform: *"Some phones are
different so we do not remove the local terminal, but the picker is
important on mobile."* A rooted phone, or one with a real userland, has
something worth driving.

So the judgement is only that a default local shell is a poor FIRST
SCREEN on mobile, not that it is useless -- which is the same call
`opens_a_local_shell()` already makes, in sec 8.5's words: an
unprivileged app sandbox has no userland worth driving from one. It is
written

    bool bssh_main_window::opens_a_local_shell(bool touch_primary) {
        return !touch_primary;
    }

so it asks about the pointing device when it means the idiom, and a
touch desktop would be handed the phone's answer. The decision survives
the redesign intact; only its input changes.

**Portrait mode: a scrollable column of terminals.** One full-screen or
two half-screen, resized freely down to the bottom of the screen with
magnetic snapping to those two positions, snap radius about 5-10px and
configurable. The column's SCROLLING snaps too, so a terminal starts at
the top of the viewport rather than half off it. One terminal resizes at
a time, so a drag is one neighbour growing as another shrinks.

**A phone rarely shows more than two**, which is what keeps this cheap:
at most one `SIGWINCH` per drag-end rather than a stream of them, and no
general N-pane geometry to reason about.

**The column's first increment is BUILT 2026-09-18**, and the
`arrangement` dimension lands with it -- the feature this section said
would carry the three-way split rather than a standalone refactor. Only
`arrangement` (`tiled | column`) is introduced, on `bssh_layout_manager`,
which is TOLD it rather than inferring it: the manager stays as
platform-free as the width it already takes, and the idiom sets the
default (`is_touch_primary()` still the proxy, mobile -> column) and lets
go, so a desktop can be forced into a column and a phone into tiles for a
headless test. `row` is still absent, because it still has no decision
and no consumer.

What the increment delivers: a column caps at two at any width
(`max_panes()`, where the arrangement diverges from the width-keyed
`max_panes_for`), so a phone-width column holds the second terminal a
tile pushes to a tab; the pane container runs its splitter vertically and
stacks; and the divider is magnetic to the halfway line ("two
half-screen"), the math pure in `ui/pane_snap.h` so a headless test
asserts the detent while the gesture that drives it waits for a device.
Guarded by `tool/sabotage/column-mode.toml`, three edits, each seen to
fail naming its own test against a 556-passing baseline.

**The arrangement became a SETTING 2026-09-22, which is what this
section actually asked for.** The increment above gave the manager the
dimension and left `is_touch_primary()` choosing it, once, in the
window's constructor -- so "either can be set to anything" was true of
the manager and false of the program: on a desktop the column was
reachable only by a test fixture forcing the platform to lie, which is
the arrangement that cannot be demonstrated to anybody holding a phone.
`layout/arrangement` now carries `auto | tiled | column`, AUTO being the
idiom's old answer, read by a file-local `chosen_arrangement()` that the
constructor and `apply_device_settings` both consult, and offered on the
settings page under *Terminals* as Automatic / Side by side / Stacked.
The test asserts the SPLITTER's orientation rather than the setting,
because a setting agreeing with itself proves nothing and what a column
means is that the panes stack.

**Two paths that agree are one path as far as a startup test can see.**
Both sabotages for this were MISSED: the constructor sets the
arrangement and so does `apply_device_settings`, so a test that opens a
fresh window for every case passes with either of them broken. The case
that separates them opens one window, changes the setting under it and
asserts the splitter turned -- which is also the defect a user meets,
since a setting that takes effect at the next start and not in the
window you changed it in is the thing being complained about. It is why
`apply_device_settings` is public: its only callers are signals from the
settings surfaces, so the suite could otherwise reach it only by
building one of those pages and driving it.

**What this first increment did NOT do -- since largely closed.** It left
the intent and tab model unchanged, so the column showed only what the
existing split machinery put in the intent and tabs still converted. That
is the "holds every session" evolution, and it landed 2026-09-18 (the
BUILT note under the off-by-one below): the column now holds the whole
shown list and scrolls, so this paragraph's first gap is closed and the
scroll area exists. ~~What is still open is the scroll's FEEL and the two
column-level selector states -- `scroll terminals` and `move terminals`.~~
**Both states are built** (re-derived 2026-09-29): the input selector's
**Pan** scrolls the column and **Move** reorders a pane in it
(`BSSH_INPUT_COLUMN_SCROLL`, `BSSH_INPUT_MOVE`). The FEEL is what remains.
The snap radius is the user's since
2026-09-22 (see below). And the resize/snap FEEL owes the same device
pass every touch surface here does: the model is tested, the thumb is
not.

**The column's scrolling snaps too, BUILT 2026-09-22.** The section
asked for it in the same breath as the divider -- "so a terminal starts
at the top of the viewport rather than half off it" -- and it had been
grouped with the kinetic flick as part of the device pass. It is not the
same kind of thing. A flick needs a thumb because it is about velocity
and easing; where a released drag SETTLES is geometry, pure, and
assertable with no device at all, which is what `bssh_snap_column_divider`
already demonstrated for the divider one paragraph up. Grouping the two
is how a piece of testable work sat behind a hardware dependency it did
not have.

`bssh_settle_column_scroll` in `ui/pane_snap.h` takes the offset the
finger left, the pane tops and the end of the scroll range, and returns
where the viewport lands; the view emits on release only when the drag
actually moved, and the container gathers the tops and sets the bar. The
drag itself stays free -- a snap applied to every step would fight the
finger -- so it is the release that lands it.

**Nearest top, not a magnetic radius, and that is a reading rather than
a quotation.** The divider takes a radius because positions outside it
are legitimate: one terminal genuinely larger than the other. A scroll
has no such position to protect -- the thing the radius would leave
reachable is exactly "a terminal half off the top", which is what the
sentence above objects to. The end of the range is a candidate alongside
the tops, so a last pane shorter than the viewport settles showing its
bottom rather than being hauled back to a top the scrollbar cannot
reach. Written down for the same reason the full-screen reading below
is: a later session should not have to re-derive it from the gesture.
Guarded by two more edits in `tool/sabotage/scrollable-column.toml`,
both seen to fail naming their own test.

**The snap radius is configurable, BUILT 2026-09-22.** §8.7 asked for
it in the same sentence as the magnet -- "snap radius about 5-10px and
configurable" -- and it had been a constant since the column landed.
`layout/column_snap_radius` carries it, defaulting to
`BSSH_COLUMN_SNAP_RADIUS`, on a slider beside the arrangement rather
than a spin box, for the reason the brightness one is a slider: a thumb
cannot hit a spin box's arrows.

**Zero is the end somebody comes here for, which is why the range starts
below §8.7's five.** The reason to go looking for this setting is to
turn the magnet off; a range of 5 to 10 offers only degrees of a thing
you cannot decline. The other end is clamped at
`BSSH_MAX_COLUMN_SNAP_RADIUS`, because a magnet wider than the panes
holds the divider on the halfway line whatever the finger does, and that
reads as a divider that cannot be moved rather than as a number somebody
typed wrong.

**The clamp on the way IN was untested and its sabotage said so.** The
setter clamps too, so a value written through it is already in range and
the reader's clamp was never asked -- two assertions passing throughout
while the edit that removed it stayed green. The case the reader exists
for is somebody editing `app.ini`, so the fixture writes the file the
way they would. That is the MISSED verdict doing the job it was fixed
for this morning, on the same day.

**"Full-screen" is the one-terminal case, read from the geometry.** A
two-pane column cannot reach one-full-screen by dragging, because
`setChildrenCollapsible(false)` keeps each pane a whole row -- so the
magnetic detent within a two-pane column is the halfway line, and the
full-screen position is simply the arrangement with one terminal in it.
Recorded because "magnetic snapping to those two positions" reads as two
divider detents and is one detent plus a pane count; the reading is
chosen here rather than left for a later session to guess at from a
gesture.

**One global input selector, visible at all times, five states.** Global
rather than per terminal, and in every arrangement rather than portrait
only:

    scroll            body drag moves this terminal's scrollback
    select            body drag highlights
    click             the gesture is forwarded to the remote
    scroll terminals  body drag moves the column
    move terminals    body drag reorders the column

**SCROLL IS THE DEFAULT, and that is the decision rather than the
ordering.** The first four states were written as "normal (highlight),
mouse click, scroll terminals, drag and drop", and scrolling INSIDE a
terminal had no home at all -- the holder found the gap by asking "how
do we scroll inside a terminal?". The original phrase "scroll terminals
(in a larger window than shown)" reads both ways, and the first version
of this section preserved that ambiguity instead of resolving it, which
is how a sentence ends up quoted later as though it had decided
something.

It resolves by FREQUENCY. Scrollback is the commonest non-typing gesture
in a terminal; selection is occasional. A mode you must enter to scroll
taxes the constant action and guarantees a mode error every time someone
forgets. So the names follow the frequency rather than the desktop's
history: body drag scrolls, and selecting is the mode you ask for.

**On 2026-09-17 NO route scrolled a phone's scrollback -- since
delivered, see below.** That was the gap this had to close, and the
enumeration is kept because it is the shape of what was fixed. All three
ways in were closed at once:
QScroller is named in a comment in `terminal_view.cpp` and never grabbed,
so a body drag does not scroll; `SCROLLBAR_AUTO` hides the scrollbar on
touch (`main_window.cpp`); and the keyboard route is dead because the
soft keyboard's keys go through `send_key`/`send_text`, which -- unlike
`handle_key` for a hardware key -- do NOT walk `stolen_chords()`, so
Shift+PgUp on the on-screen board sends PageUp to the shell instead of
scrolling. So the on-screen keyboard reaches none of the app chords the
hardware path steals (scroll, and Ctrl+Shift+C/V), which is the "cannot
be two lists" principle `handle_key` states, broken by a second entry
point that steals nothing. Body-drag-scrolls closes the gap the design
way; the keyboard hole is a separate call -- make `send_key` steal chords
like `handle_key` (one input path, but the chord table is in Qt keys and
`send_key` is in emulator keys, so it needs the translation `handle_key`
already does), or leave the on-screen board a pure key-sender and let the
selector's `scroll` state carry it. Not decided here; recorded so the
selector work knows it is starting from zero, not from a keyboard
fallback that already works.

**Both routes are now open.** The keyboard hole was taken the first way
-- `send_key` walks `stolen_chords()` now (the `steal_chord` translation
into Qt keys), so Shift+PgUp on the on-screen board scrolls rather than
paging the shell. And the body-drag route landed with the input selector
below: `scroll` is the default, so a body drag moves the scrollback
without a mode change. The gap this paragraph opened is closed from both
ends.

**The states are not equally dangerous, and `click` is the one to arm
deliberately.** Scrolling when you meant to select costs a scroll back.
Forwarding a click when you meant to select can ACT -- hit a menu in
`mc`, move a cursor, trigger something that cannot be undone by
scrolling. The mode whose mistakes are destructive is the one that has
to be explicit and visibly armed; the mode whose mistakes are harmless
is the one that should be the default.

**And the conflict is smaller than it looks where it matters most.**
Full-screen programs like `mc` and `htop` run on the alternate screen,
which has no scrollback -- so in exactly the situation that wants
`click`, scrolling is mostly not competing for the gesture.

**This answers a defect already on the record rather than adding a
feature.** `working-practice.md` documents that every selection override
in the terminal is gated on Shift and *"a phone has no Shift to hold"*,
so inside `mc` or `htop` a phone has "no selection, no clipboard, no
context menu and no way back". A visible selector is the escape that was
missing.

**BUILT 2026-09-18: the three terminal-level states, and the bar.** The
selector lands as `bssh_input_selector`, one per window, and the window
pushes its state to every pane -- `scroll` (the default), `select` and
`click`, the three that route a gesture WITHIN a terminal. The routing is
in `bssh_terminal_view`: on touch the mode is exactly the override Shift
is on a desktop, so `scroll` and `select` keep a gesture local even where
the remote grabbed the mouse and `click` lets it through. A body drag in
`scroll` mode moves the scrollback -- the route sec 8.7 said "must
deliver" and that a phone did not have. Desktop is left untouched: it
keeps Shift, the wheel and drag-select, so the mode is read only where a
thumb is the pointer, and the bar is shown only there.

**Where the bar lives -- the open placement, now resolved.** This section
left "where the selector physically lives" open. Settled on the holder's
go-ahead 2026-09-18 to the bottom edge of the terminal page, beneath the
status line: always visible, thumb-reachable, and one `addWidget` to move
if it should go elsewhere. It is not in the key row, which scrolls, so it
cannot scroll off.

**`move terminals` is BUILT 2026-09-18.** The two
column-level states needed the scrollable column, which now exists.
`move terminals` (reorder the column) is built: a fourth selector state,
Move, offered only in the column arrangement -- there is nothing to reorder
in the tiles -- where a body drag moves the pane one place, up the screen
toward the top of the column. The gesture only ASKS, on release: the reorder
rebuilds the panes and would destroy the dragging view mid-gesture, so it is
queued through the container (which names the session) to the window's
existing `move_pane`, the same method the desktop's context menu uses. That
method now also moves m_shown, the column's order of record, so a reorder
survives the next session opening rather than being undone by
`sync_column_intent`. A single step per drag is deliberate for now -- the
continuous drag, and the snap, are the device pass, as the scroll's feel is.
Losing the column falls the mode back to Scroll so it is never stranded on a
hidden button. Guarded by `tool/sabotage/column-reorder.toml`, five edits
each seen to fail: the state offered without a column, a reorder that skips
m_shown, the gesture reversing direction, the container swallowing it, and
the window not wired to it.

**`scroll terminals` is BUILT 2026-09-18 too**, as the Pan state, so both
column states now exist. Where the overflow scrollbar and focus-scrolling
gave reachability, Pan gives the thumb-drag: in Pan mode a body drag scrolls
the column's viewport, content following the finger. It is simpler than the
reorder and needs no window -- the container owns the scroll area, so the
view reports the step since the last move (it holds no offset of its own,
the scroll being the container's) and the container turns it into a scroll,
live and unqueued because scrolling rebuilds nothing and the view stays put
under the finger. Pan is column-only beside Move, and falls back to Scroll
the same way when the column goes. The selector disambiguates it from the
scrollback scroll, which is the fight the earlier note worried about: the
mode says which the drag means. Guarded by `tool/sabotage/pan-scroll.toml`,
four edits seen to fail: the state offered without a column, the view
reversing the step, the container scrolling backwards, and the container
left unwired. The kinetic flick is still the device pass; a step-per-move
drag is the reachable version, as the reorder's single step is. The
snap-to-a-pane-top is BUILT 2026-09-22 and was never a device question
-- see below.

**Guarded by `tool/sabotage/input-selector.toml`**, five edits, each seen
to fail naming its own test: a scroll drag that moves nothing, select
losing its override, click losing its forward, the window not telling the
panes, and the bar opening on the wrong state.

**The design argument that was raised and settled.** The four states are
not one axis: two decide WHO receives a gesture (local highlight against
a click forwarded to the remote) and two decide WHAT a gesture
manipulates (the viewport, the layout). The case for splitting them was
that a user wanting to select text should not have to think about a
dimension that is not about selection.

Settled the other way, and the reason is the Shift defect again: that
failure was not too many modes but an override that was **invisible and
unproducible** on the device. A selector that is permanently on screen
and unmistakable is strictly better than a hidden modifier, whatever its
arity. Handles stay as well for now.

**And long press is not in conflict**, which was the other objection.
Android separates them by movement: a press that does not move is a long
press, a press that moves is a drag. So reordering by dragging can
coexist with long-press-for-menu. What stays genuinely ambiguous is a
drag INSIDE a terminal body -- select, scroll, or reorder -- which is
precisely what the selector disambiguates.

**The tab bar's off-by-one is structural, and portrait mode dissolves
it.** Reported as "the current tabs are broken and seem to have an off
by one problem". The cause, in `refresh_tabs()`:

    m_tab_order.clear();
    for (bssh_session *session : m_shown)
        if (!panes.contains(session)) m_tab_order.append(session);

The bar holds only sessions that are NOT currently visible as panes. A
phone fits exactly one pane, so two sessions give one tab and three give
two, and the session on screen has no tab at all. That is sec 8.2's
"panes convert to tabs, preserving order" working as written --
coherent on a desktop showing several panes, nonsense on a phone showing
one. If the portrait column holds every session there is nothing left to
convert, and the off-by-one goes with the concept rather than being
fixed.

**BUILT 2026-09-18: the column now holds every session, and the
off-by-one is gone with it.** `max_panes()` in a column is a sanity
ceiling rather than the tiled cap (`BSSH_COLUMN_PANE_LIMIT`), so
`visible_panes()` truncates nothing; `bssh_main_window::sync_column_intent`
keeps the layout intent equal to the shown list wherever that list
changes, so every session the window holds is a pane and the tab bar has
nothing to build. A column taller than the screen scrolls: its splitter
lives in a `QScrollArea` (`pane_container`), moved there only in the
column arrangement so a tiled window is untouched, and focusing a pane
past the bottom scrolls it into view. Guarded by
`tool/sabotage/scrollable-column.toml` and the model half of
`column-mode.toml`.

**What this costs, and it is the honest half.** Dissolving the tabs
dissolves the touch tab-REORDER with them -- there are no tabs to move on
a phone now -- and its replacement, reordering the column itself (the
selector's `move terminals` state), is now BUILT (see above), so touch
reordering is back by that route; tab-reorder survives on the desktop,
where tabs do, and `a_tab_can_be_moved_along_the_bar` moved there to prove
it. The scroll now has its thumb-drag as well: the Pan state (see above)
scrolls the column's viewport, the fight with the scrollback scroll settled
by the selector saying which the drag means. What is still the device pass
is the FEEL -- no kinetic flick -- and the snap radius. The
snap-to-a-pane-top landed 2026-09-22 and is not part of that: it is the
same kind of pure geometry the divider detent is, and a device adds
nothing to asserting it.

**And it cost two more verbs than that paragraph noticed, which is the
part worth keeping.** Close and Duplicate lived on the tab bar and its
menu (§8.3) and nowhere else: every route that closes ONE session went
through the bar's own close button, a middle-click on a tab, that
menu's Close, or File > Close Tab. Dissolving the bar on touch took the
first three, and the menu bar is built only for the desktop front door,
so **a phone could open sessions and never close one.**

**Measured 2026-09-21 rather than reasoned about**: the assertion was
written first and failed, reporting that the terminal menu offers no way
to close and that touch has no tab bar or menu bar to offer one either.
The registry closes perfectly throughout, which is exactly what kept
this invisible -- every test that closes a session does it through the
registry, and the defect is that nothing a thumb can reach calls it.

**The reorder is the tell.** This section DID notice the reorder going
and replaced it with the column's Move state, in the paragraph directly
above. Two verbs on the same menu, lost by the same edit, went
unmentioned -- so the check that matters when a surface is dissolved is
not "what did this surface do" but **"what could ONLY be reached here"**,
enumerated rather than recalled.

**A third verb went the same way and is the sharpest of the three.**
The tab menu offers **Connect** for a session that is not live, and its
comment says exactly why: *"a restore reopens every remembered host as a
tab and connects none of them, so without this a restored tab would be a
dead end."* Dissolving the tab bar recreated that dead end on the
platform the entry was written for -- a phone restores, shows every
session in the column, and had no way to connect any of them. Asserted
red before it was built, with the fixture checking its session really
was not live, because a live one is deliberately offered no Connect and
the test would otherwise be demanding a verb that should be absent.

**Both are on the terminal menu now, in a `Session` group** with New Tab
and New Window, Close behind a separator because it is the one that
cannot be undone. Grouped rather than flat for the measured reason Text
Size and Pane already carry: four more rows do not fit a 640dp cover
screen and one submenu row does -- the menu went 584px back to 520.
**What is asserted is the ROUTE**: that the entries exist and are
enabled. That choosing one closes the session is not pinned, because the
dispatch happens after `QMenu::exec` and a headless suite cannot reach
it -- the same limit every other entry on this menu has, recorded here
rather than left for somebody to assume otherwise.

**A FOURTH went the same way, found 2026-09-22 by re-running that
enumeration rather than by anybody meeting it.** *Detach to a Window* is
on the menu bar's View menu, and its own comment states the case:
*"It is also the only route a PANE has out of a window. Dragging one out
is Alt and a drag, which works and which nobody discovers, so a menu
item that does it is the discoverable half of the same gesture."* A
phone has neither half -- no menu bar, and no Alt to hold -- so a
session could be put into a column and never taken out of it. It is in
the `Pane` group now, beside Split, where the menu bar has always kept
it: they are the two halves of one question about where a session
lives.

**And it is refused where `tear_out` would refuse**, rather than offered
and silently declining, which is §8.4's "a window will not empty
itself" applied at the surface instead of one call in.

**Show as Tab was the same mistake pointing the other way.** In a column
`show_as_tab` returns early -- §8.7's column holds every session and has
no bar to reach a dropped one by -- and the entry was enabled anyway, so
on a phone it looked available and did nothing when chosen. An entry
that declines silently is worse than one that is greyed out, which is
the argument the paragraph above makes about `tear_out`; it is greyed
out in a column now.

**The zoom was the mirror image, on the surface a DESKTOP user looks
at — [FIXED 2026-09-22].** Text Size is on the terminal menu and the
chords work, so every test and every phone saw a feature that was
present twice. The menu bar had nothing, which is the argument its own
Find entry was added under: *the chord still works, so this costs
nobody who already knew and it costs everybody who did not.* Flat in
View rather than in a group, for the reason Clear Scrollback and Reset
Terminal are flat in Edit -- a bar has no 640dp ceiling to respect, and
the group on the context menu exists because that one does.

**So the enumeration runs in both directions, and saying so is the
point.** §8.7 asked what could only be reached on the surface it
dissolved, and that question found four verbs. Asked the other way --
what can only be reached on a surface a POINTER has -- it finds the
zoom. Neither question is the other's complement by accident: a verb
lands on the surface whoever added it was using.

**The fit gate had stopped being able to see this class of growth, and
that is the more general finding.** `the_touch_terminal_menu_fits_a_phone`
measured the top-level menu -- and verbs have been pushed into SUBMENUS
precisely because the top level was full, four times now. So the one
measurement guarding the menu was blind to exactly the place the next
verb goes. It walks the groups too now, and asserts how many menus it
measured, because a walk that stops finding groups reports success
exactly as loudly as one that measured them all.

**Everything currently built stays until it is replaced**, on the
holder's instruction: *"We keep all the features we have for now and try
to test our way forward."* Open, and cheap to settle when they come up:
whether tabs survive on desktop and landscape, and whether the terminal's
overflow menu goes once the picker carries the config route. (Where the
selector lives was the third, and is settled above: the bottom edge.)

**Which arrangement applies in which device state -- [BUILT 2026-09-25]
from the holder's answers.** 2026-09-11, 08:26: *"Now that we have many
disparate modes, we should probably configure which mode applies in which
state. Like, in landscape mode we probably only want one terminal on
screen but scrolling might be useful, in portrait mode 1-2 and scrolling,
in folded out mode, act more like desktop but optimized for touch."*
08:40: *"Desktop might have touch too, so let's try to make the modes the
same with minor differences for mobile and desktop, switchable for
testing. Let's give them names, 1. Desktop mode, 2. Vertical scrollable.
They can each also have tabs on top/bottom/left/right."* The two names
became `tiled` and `column` in `layout/arrangement`.

Three questions stayed open until 2026-09-25, when the holder answered:
the unfolded Fold is side by side with touch sizing, not a third
arrangement; the 1-2 count follows orientation, so rotating a window
re-flows it; and the cover screen sideways (~700x390) takes the landscape
rule, one terminal.

**The rule, as built** (`bssh_layout_manager::choose_arrangement`):
AUTOMATIC on a touch device reads the device's SCREEN, never the window --
opening the on-screen keyboard shrinks the window's height, and a rule
keyed on the window would flip the arrangement every time it opened. A
screen 600dp or more in BOTH directions is side by side; anything smaller
is a column showing one terminal held sideways and two held upright, the
rest scrolled to and nothing closed. A platform with no device screen to
follow (`bssh_platform::screen_size()` invalid -- a desktop) keeps the
older answer, touch stacks and a pointer tiles. A hand-chosen column on a
device still counts by how it is held. "One on screen" is the column's
TOTAL height set to the viewport times panes over the count, not a
minimum per pane, so the divider between two stacked panes stays
draggable. The window re-asks when the screen changes, not on every
resize. Guarded by `tool/sabotage/arrangement-by-screen.toml`, four edits.

**Found on the way**: the input selector's Move state was set once at
construction, so a window that became a column at runtime -- through the
setting, and now by being folded -- offered no way to reorder it.
`apply_arrangement()` sets it with the arrangement.

**The tab bar's position -- [BUILT 2026-09-25].** Top, bottom, left or
right, chosen per arrangement (the holder's pick over one global
setting), default top as it always was. Only side by side has a bar to
place -- a column shows every session as a pane -- so the setting is
`layout/tabs_tiled`, offered on the settings page as "Tabs, side by
side", and keyed per arrangement for the day a column has tabs. Left and
right put the bar in the panes' own row, drawn facing them, and a drop
aimed anywhere down that side is a drop on the bar: the hit test runs
across the strip whichever way it lies. Guarded by
`tool/sabotage/tab-position.toml`, three edits.

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

**`--load=?` lists the saved sessions, BUILT 2026-09-22.** The option
took a session name and there was no way to find out what it would
accept short of opening the app -- which is the state `--term-features=?`
already exists to avoid, and the reason the generated completion had to
stay silent about `--load`. Names only, one per line, sorted
case-insensitively, on stdout with a zero status: the completion reads
it as well as a person, so a header would be a candidate that is not a
session name.

**Answered before Qt is up, like the other listings, and that needed one
ordering fix.** The store's default directory is
`QStandardPaths::AppConfigLocation`, which reads `QCoreApplication`'s
application name -- a static that answers with no instance, but answers
"beerssh" only once it has been told. So the identity is set before the
listing is built; without it the listing would read a different
directory from the one the app uses and confidently print nothing.

**The listing lives in `profile_store.cpp` rather than `main.cpp`, where
it started, because `main.cpp` is the one file the test build leaves
out** -- a listing nothing can call is a listing nothing can check. It
takes an optional store, like the orphan sweep beside it, so the test
hands over its own.

**And two sabotages went MISSED against the completion's own test, which
is what fixed it.** The test asserted that every value-taking option has
a case ARM; every value shape falls back to a silent arm, so deleting
the whole classification -- or just the session branch -- left an arm
behind and the suite green. What it asserts now is the RELATIONSHIP
between the table's `value_name` and what the arm DOES: a `command`
completes with `compgen -c`, a `directory` with `compgen -d`, a
`session` by asking the program. All four are caught.

**And the flag list is picked out of the script by its contents rather
than by position.** It was the first `compgen -W` in the file until
`--load` learned to ask the program, whose word list is a command
substitution -- and the assertion then read that as the options and
reported every flag missing. A position is not an identity.

**And the completion is generated too, BUILT 2026-09-22.** The
paragraph above cited fmake's `--man` and its `--completion` as being on
the same footing, adopted the first and left the second -- so `beerssh`
had no shell completion at all, and a user typing `beerssh --ge` got
nothing. `--completion` writes a bash script built from the same table,
`make` produces `beerssh.bash` beside `beerssh.1`, and `make install`
puts it where bash-completion loads it on demand:
`share/bash-completion/completions/beerssh`, named for the command and
carrying no suffix, because that filename is the lookup.

**The argument for generating it is the page's, and sharper.** A page
that omits a flag is merely unhelpful; a completion that omits one
refuses to finish what somebody is typing, and they conclude the flag
does not exist. So the same both-directions test the page gets: every
option in the offered word list, and every word in that list an option
the parser takes.

**What it completes is keyed on the table's `value_name`, not on the
option's name.** A `directory` is a directory on anybody's machine and a
`command` is on their PATH, so an option added later that takes either
is completed without anybody editing the generator. Everything else --
a session name in this user's host store, a term-features spec, a title
-- gets a case arm that deliberately offers NOTHING, because falling
through to the option list would propose `beerssh -e --help` as a thing
to run. `-o default` carries the positional argument, which is a host or
a preset path and only the second of those is completable.

**Bash only, and that is a decision rather than a first instalment.**
zsh and fish want different shapes rather than the same script under
another name, and a generator emitting three dialects nobody here runs
is three things to be wrong. The table is what matters; another shell is
another function over it when somebody wants one.

**A MISSED verdict found the weak assertion again, the third time
today.** The sabotage that COMMENTS OUT the `complete -F` line left the
suite green: the test asked whether the script *contained* that text,
and a commented line contains it. It reads whole lines now. A true
condition that is not the one you mean passes for the same reason a
correct one does, and only breaking the thing it claims to guard tells
them apart.

**`check-deb` sources the installed file and asks bash**, rather than
grepping it: `complete -p beerssh` answers whether anything actually
registered, which a grep for the word cannot do about a line that is
commented out. And the build runs `bash -n` on the generated file and
removes it when that fails, because a completion that does not parse is
sourced by every new shell of everybody who installed it, and the
failure lands in their prompt rather than anywhere near a build.

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
| **Iosevka** (narrow/term variant) | OFL 1.1 | **Condensed** — fits noticeably more columns on a phone screen. Genuinely valuable on mobile; offered on the phone as a download, not the default (§16). |
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
explicit font choice resolves identically everywhere. **As built, it does not
differ** (the holder, 2026-09-29, §16): JetBrains Mono everywhere, because a
default that changed when a font was downloaded changed hosts nobody touched.

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
| `default` | `#000000` | `#ffffff` | the default while the app is dark |
| `green-on-black` | `#000000` | `#00ff00` | the green phosphor |
| `yellow-on-black` | `#000000` | `#ffff00` | the amber phosphor |
| `black-on-white` | `#ffffff` | `#000000` | e-paper, and the default while the app is light |
| `custom` | from settings | from settings | the escape hatch |

Three phosphors and an inverse. The inverse was named for **who it is for**,
"(e-paper)", rather than called "light mode": an e-paper panel holds a light
background for free and pays for a dark one, the reverse of every other
display. **Since 2026-09-29 it is also the light default** (§6.2), and the
label is plain "Black on white": under the two rules here -- a pure ground, one
shared palette -- it is the only light scheme there can be, so a second one for
comfort on an LCD was declined rather than drawn. The id `default` stays white
on black, because a profile can store it by name and that profile chose it.

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

**The push from the settings into the catalogue had no test until
2026-09-20, and this feature is the one the others are copied from.** The
editor was covered and the store was covered; the single line in
`apply_device_settings` joining them was not. Remove it and every custom
palette silently falls back to the black-and-white the entry is born
with — while the picker still says *Custom*, the id still resolves, and
nothing goes red. §10.1's "stored, read and applied to nothing", sitting
in the exemplar rather than in a copy of it.

It was found while building §6.2's user-composed key row, which copies
this mechanism deliberately: imitating something is a reason to read it
closely, and reading it closely is what found the hole. Worth keeping as
method, because nothing else had prompted anybody to look — the feature
works, and a call site that is never exercised looks exactly like one
that is. `the_custom_palette_reaches_the_catalogue_at_startup` asserts a
colour the entry is NOT born holding, since asserting on black or white
would pass with the push removed; `tool/sabotage/custom-palette-push.toml`
holds both halves, the missing push and the entry written under another
id, each seen to fail.

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

**[BUILT 2026-09-29] A font of the user's own, the reserved "custom" entry**
the colour scheme, key row and keyboard layout already have -- the last of
the four catalogs to get one, taken by the holder from §16. "Use a font
file..." under the picker, on both platforms, takes a `.ttf`, `.otf` or `.ttc`
(on Android through the same wildcard picker the preset import uses),
copies it into app storage as `fonts/custom/custom-font` -- one fixed name,
so replacing it deletes a named file -- registers it, and lists it as
"Your font: <family>". It loads after the set at every start, from the copy,
so the original may move or go. One at a time, as the others are.
- **Monospace by measurement, not by its flag.** §9.1 excludes proportional
  faces because a grid cannot hold one, and a font's fixed-pitch bit is its
  author's claim: `i W l m . 0 _ M` must share one advance within 2 %, and a
  font that fails is unregistered and refused with the reason. So is a file
  that is not a font, or one over 64 MB. A refusal leaves the previous
  custom font in place, in the catalog and on disk.
- **The id is `custom`**, and a profile naming it on a machine with none
  resolves as any absent font does: "Your font (none imported here)" in the
  picker, drawn by the chain meanwhile.
- Its licence is the user's; it is their file, and it is not listed on the
  licences screen, which lists what BeerSSH ships.
- Guarded by `tool/sabotage/custom-font.toml`, five edits. The import dialog
  itself is not driven by a test; the import behind it is.
Bundling ~10 families × multiple weights is **tens of megabytes** — too much for
the base download on Android. Desktop has no such pressure, which is a licence to
diverge on *delivery* but never on *which fonts exist*.

- **Android:** ship the base families in the APK, and fetch the rest **from
  their own pinned upstream releases** when the user picks one -- decided by
  the holder 2026-09-26, over the Play Asset Delivery this bullet first
  planned. That needed Google's `asset-delivery` library under the Play Core
  SDK terms rather than an open licence, reached only Play Store installs
  (never an `adb` or F-Droid-style one), and needed bundletool to test.
  **BUILT.** `tool/font-sources.txt` holds the pins, read by
  `tool/fetch-fonts.sh` for a build and embedded in the app (`resources.qrc`),
  so the two cannot disagree; its `file|` lines pin every output on its own,
  and the script checks each extracted file against them, so a build is also
  a check of the pins a phone relies on. `bssh_font_download` takes a family
  one file at a time: the archive's length, its last 64 KB for the zip
  directory, then each wanted member by HTTP range (`term/zip_index`, stored
  and deflated, no zip64) -- Iosevka's release is 165 MB for the four files
  wanted, and fetching it by range took 2.8 s in a live run. Every file is
  checked against its pin and kept only when the whole family has verified,
  into app storage, loaded after the bundle at every start; the licence comes
  with it, and the licences screen reads it there. The picker offers
  "Download <font>" under the note for a font of the set that is missing,
  never for a fallback-only one. Guarded by `tool/sabotage/font-download.toml`,
  six edits; `BSSH_TEST_LIVE_FONTS=1` runs a real family over the network.
  **It could not work on Android as first built** -- Qt there has no TLS, so
  the download failed with "TLS initialization failed"; the Android source
  is `BsshHttp` now, the §5.2 exception. **Seen working on the emulator
  2026-09-28** (x86_64, API 33): the picker offered "Download Iosevka Term",
  and four font files and the licence were in app storage within about 20
  seconds, the preview redrawn in it. Not yet on the phone, which dropped off
  `adb` mid-install every time that day.
- **Desktop:** ship the full curated set in the package — tens of megabytes is
  unremarkable for a desktop install, and it keeps the font picker free of
  download states on the platform where they'd be pure friction.
- **The picker's contents must not depend on the platform.** A font the user
  selected on their phone has to resolve when the same profile opens on their
  desktop, so families are identified by a stable internal ID, and an
  unavailable font falls back down the §9.3 chain with a visible note rather
  than silently substituting.
- ~~Whether desktop additionally offers system-installed monospace fonts.~~
  **Decided and built 2026-09-26** -- offered in a group of their own after the
  set, marked as the computer's rather than the set's. No suitability check
  runs on them beyond Qt's fixed-pitch flag; see §16.
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
- **BUILT 2026-09-18, the lean taken as written.** The interface follows
  the system font-scale accessibility setting; the terminal grid does not,
  because it would silently change the column count; and it is an explicit
  setting, on by default. `bssh_platform::system_font_scale()` reads it --
  Android from `Configuration.fontScale` through JNI, exactly as
  `android_night_mode` reads `uiMode`, and the desktop returns 1.0 because
  Qt applies the desktop's own font size already. `main()` scales the
  application font by it, before any widget, when
  `honour_system_font_scale()` is on; the grid is untouched because
  `make_font` sets an explicit point size, which
  `the_terminal_grid_ignores_the_application_font` now pins as a
  regression. Guarded by `tool/sabotage/interface-scale.toml`.
  **The Android value itself is unverified on hardware** -- the desktop
  half, the setting, the scaling arithmetic and the grid's insulation are
  all tested headlessly, but that a real device reports the fontScale the
  user set is the one part only the phone can show, like every other
  Android reading here.
  **What was measured before building, and it still holds.** *Nothing in this
  tree read the system font scale* before this — no reference to `fontScale`
  anywhere in `src/`, until the Android backend added one. The grid builds its own `QFont`
  and sets an explicit point size (`bssh_font_catalog::make_font`); app chrome
  inherits the application font, which — until this feature — nothing here
  overrode beyond two page titles. `main()` now sets it once, scaled, which
  is what moves the chrome and leaves the grid alone.
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
  So the leaning's first half was already satisfied — the grid does not move,
  which is what §9.5 wants — and its second half was the new code the BUILT
  note above describes: `main()` sets the application font from the platform's
  reported scale, which moves the chrome and leaves the grid alone, because
  the grid sets its own point size explicitly in `bssh_font_catalog::make_font`.
  That measurement is what made the mechanism safe to write without a device
  in hand.

**Zoom is BUILT 2026-09-21, and it is a DELTA rather than a second size.**
A terminal that cannot make its own text bigger for the next two minutes
is one that sends a user to a settings page, a preset field and a decision
about every future connection -- and nothing in this document had asked
for it, which is why it is recorded here with the shape rather than only
the fact.

**Where the number lives decided the design.** A pane is destroyed and
rebuilt by every layout change (`bssh_pane_container::rebuild`
`qDeleteAll`s its views), so a zoom kept on the widget is lost the first
time a window narrows enough to collapse a split -- which is exactly when
somebody wants bigger text. It lives on `bssh_session`, beside the
resolved settings and for the reason those live there, and the view is
told at build time and on `font_zoom_changed`.

**And it is a delta on top of the resolved size, not a replacement for
it.** Storing the zoomed size instead would make the next resolve either
lose the zoom or silently overwrite what the host's preset says -- a
preset edit arriving while a pane is zoomed has to move the base under
the zoom, and that only works if the two are separate numbers.
`set_font_id()` therefore keeps the zoom rather than clearing it, and
re-clamps it against the new base.

**Bounded by the range §10.5 already clamps a preset's font size to**
(6 to 32 points), in both places that hold the number, and clamped as a
ZOOM rather than as a size. Bounding only the sum lets the stored steps
walk past the limit while the drawn size stands still, so the first step
back does nothing -- which reads, from the keyboard, as the zoom having
broken.

**Three ways in, and the third is the one that is not about desktops.**
Ctrl+= , Ctrl+- and Ctrl+0 are the twelfth, eleventh and tenth stolen
chords (§6.1); Ctrl+wheel is tested **before** the remote's claim on the
wheel, because text too small to read is most often text inside `less` or
`vim` and those are precisely the programs that have turned mouse
reporting on. The terminal context menu carries all three as well, and
that is for the phone: a device with no keyboard and no wheel had no
route to this at all, and §8.7's menu is the surface both platforms
already have.

**Guarded by `tool/sabotage/zoom.toml`, nine edits, all nine seen to
fail** against a 615-passing baseline: the font built from the base
rather than the zoomed size, a resolve throwing the zoom away, the clamp
removed, the clamped value not written back, a rebuilt pane starting at
the resolved size, Ctrl+wheel falling through to the remote, the
container not naming the session a wheel zoom happened to, the chord
swallowed, and the menu losing the entries. Six of the nine are wiring
rather than arithmetic, which is where this feature could most easily
have been present and inert.

**What is deliberately NOT done.** The zoom is not persisted -- not into
the profile, not into the preset. What a HOST should look like is a
preset field with an editor and an override badge behind it (§10.1), and
writing a zoom into either would answer a question the user did not ask.
**Pinch-to-zoom is BUILT 2026-09-22**, which this section called "the
obvious next increment on touch: the menu makes the feature reachable
there, a gesture would make it natural". It emits the same
`zoom_requested` the Ctrl+wheel and the Text Size menu emit, so every
question already settled about zooming stays settled -- the session owns
the number, the clamp lives in `rebuild_font`, and the value is not
persisted. **This adds an input route and decides nothing else.**

**The zoom persists per host -- [DECIDED and BUILT 2026-09-26].** The
holder chose "remember per host" over "forget on close". Not into the
preset or its editor, which is what the paragraph above declined: a
host's `font_zoom` in `hosts.ini` is the zoom it was last left at, in
the same steps the gestures count, and a new session of the host starts
from it (`bssh_session`'s constructor). The window writes a session's
zoom back to the host 800 ms after the last step, reading the host fresh
from the store so a stale copy cannot undo an edit made since -- a pinch
is a stream of steps, and each write is the whole hosts file. A local
shell or a command-line target has no host to keep it in.

**Accumulating the incremental factor rather than reading the gesture's
total is what makes it testable, and that is not a compromise.** Reading
the total needs to know where each pinch BEGAN, and a gesture's state is
set by Qt's recogniser through private friends: no test can produce a
`GestureStarted`, so the reset branch would have been the one thing in
this feature nothing could reach. Accumulating has no beginning to
detect -- a new pinch continues from whatever fraction of a step was
left, at most one step's worth, which is what somebody pinching twice in
the same direction expects anyway.

**Steps of 20% rather than a continuous scale**, because the zoom is an
integer point-size offset: the grid is whole cells and there is no half
a point to show, so a continuous factor would be rounded to these same
steps one layer further from the thing that owns them.

**What no test here can reach is the `grabGesture()` call itself.** Qt
recognises a pinch from real touch points and offers no way to ask a
widget which gestures it grabbed, so the suite drives the handler
through a real `QGestureEvent` -- which proves `event()` routes it --
and cannot prove the gesture would arrive on a device. The same limit
the drop tests record for Qt's own drag target, and it is why
`tool/sabotage/pinch-zoom.toml` breaks the ARITHMETIC rather than the
grab: an edit nothing can catch is not a sabotage, it is a claim. The
gesture itself owes the device pass every touch surface here does.

**The floor did not reach the cursor, and one scheme in five could
show it.** Found 2026-09-21 by asking which drawn colours go through
`bssh_raise_contrast`: the cell foreground does, after both inverse
swaps and before the dim, and the pane grip does unconditionally
because it is chrome. The cursor did not. It took the brightness dim
and nothing else.

**The setting's own words are what make this a defect rather than a
choice** -- it lifts *"a drawn colour when the scheme's own value falls
under it"*, it is ON by default, and the colour editor offers exactly
three colours to pick: Background, Text and Cursor. So a person can
choose a cursor that vanishes into their own background, have their
TEXT rescued on the same screen, and watch the cursor stay gone.

**Unreachable with a shipped scheme, which is why it lasted.** All five
set `cursor` equal to `foreground`, so their cursor clears the floor
because their text does -- the five cases anybody would have tested are
the five that cannot fail. Only `custom`, the scheme a person makes by
hand, reaches it. A remote cannot: OSC 10, 11 and 4 answer queries here
and refuse to set, so this is the user's own choice and nobody else's.

**Lifted against `effective_background()`, before the dim.** The
reference is the reverse-video-aware background, because DECSCNM flips
what the cursor is actually drawn over; and the order is the one the
text path twenty lines above already argues for, since the floor lifts
until it clears and a dim applied first is simply undone.

**The obvious companion edit was written, measured and reverted.**
Raising the block's punched-out glyph against the cursor looks
necessary and is a no-op in every reachable case: the glyph is drawn in
the very colour the block was lifted AGAINST, so it clears the floor by
construction whenever the setting is on, and when it is off faithful
colours are what was asked for. The reasoning is now a comment at that
site rather than code, because a change nothing can observe is one the
next reader has to re-derive.

**Guarded by a pixel test.** `a_cursor_under_the_floor_is_lifted_like_text_is`
pushes a custom scheme whose cursor is `#101010` on black, asserts the
fixture really is under the floor before measuring anything, then
samples what was actually painted -- and asserts the faithful value is
drawn with the setting OFF, so a view that always lifted would fail.
The edit that removes the lift was seen to fail against a 618-passing
baseline.

**The substitution note was written for nobody, for as long as it has
existed.** `bssh_font_catalog::substitution_note()` composes a whole
sentence for a person -- *"X is not installed on this device yet.
Showing Y until it is."* -- and `bssh_terminal_view::font_substitution_note()`
hands it on. That function's header names its consumer: *"The window
shows it; the view only knows it."* **No window did.** Nothing under
`src/` referenced either function.

**So a host whose font is not on this device rendered in a substitute
with no explanation**, and the explanation existed the whole time. §9.4
already lists unavailable families in the picker and marks them, which
is the answer at CHOOSING time; this is the answer at looking-at-it
time, and it was the half nobody could reach.

**The same sweep, run again, found one more and cleared four classes.**
`bssh_app_settings::default_keyboard()` decides what an installation
that has never been asked gets, and it differs by device -- a phone
opens with the built-in board, a desktop with the system one. Its header
says it is public *"because the settings page shows which one that is,
and a second copy of the rule would be a second thing to be wrong"*, and
the page never called it: the rule was public for a consumer nobody had
written, and a user was never told what "unset" would give them. The
page says so now, as a wrapped note rather than a longer combo item --
sec 12.3l records that combo's width as load-bearing at 320dp -- and it
reads the NAME back out of the combo, since two lists of the same three
names is the second copy the comment is about.

**And asserting it on both devices is what makes it a test.** A note
naming a constant is right on a desktop; the sabotage that writes
`KEYBOARD_SYSTEM` in place of the call is caught only because the same
assertion runs under forced touch.

**Swept and clean**: the public APIs of `bssh_session`,
`bssh_pane_container`, `bssh_emulator` and `bssh_session_registry`, and
five other comments that name a consumer -- `back_requested` on three
touch pages, `can_hide_from_screenshots`, `hosts_using` before a preset
delete. Every one of those consumers exists. The instrument cannot see
intra-file use, so a method called only from its own `.cpp` reads as
unreferenced; that is what made `mark_seen`, `equalize_panes` and
`scroll_column_by` look unwired when they are not.

**Found by a mechanical sweep, and the first version of that sweep
lied.** Walking the view's public methods for ones referenced nowhere
outside their own class first reported `set_reverse_video` as unwired --
and `pane_container` connects it, as a pointer-to-member, which a
`name(`-shaped pattern cannot match. The corrected sweep matches
`::name` too, which is *Four ways to manufacture an absence* paid for
again: the pattern, not the tree.

**It is on the status line, recomputed only when the font id moves.**
Answering costs a `QFontDatabase::families()` scan and `update_status()`
runs on every state change, every resize and every keystroke that
scrolls -- the same paint-path argument that keeps the status-line
setting out of that function. Comparing one string is what makes asking
in ONE place affordable, and one place cannot have a call site somebody
forgot: there are three places a view can change.

**Two edits, both seen to fail** against 622 passing: the line that
shows the note, and the id comparison pinned so the note is computed
once and never refreshed. The second is the subtler of the two -- it
leaves a session that changes font wearing the note it had, including
an empty one forever, which is the silent case again by a quieter road.

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


#### The brightness slider could be set to unreadable — **[FIXED 2026-09-04]**

Reported as "the text colours are dark, impossible to read", and it was
a setting rather than a rendering fault: this machine's config held
`text_brightness=10`, which is the lowest the slider offered.

§9.3 puts a **3:1 floor** under anything this program draws, and the dim
is applied *after* that floor deliberately -- a slider the floor could
veto is a slider that stops working where somebody reached for it. What
nobody decided was **how far down it goes**. Measured against pure
black, which is what a dark scheme's background is and what the dim is
always applied against, since it is never applied to a background:

| brightness | colour | contrast |
|---|---|---|
| 100% | `#ffffff` | 21.00:1 |
| 70% | `#b2b2b2` | 9.90:1 |
| 50% | `#7f7f7f` | 5.24:1 |
| **40%** | `#666666` | **3.66:1** |
| 30% | `#4c4c4c` | 2.45:1 |
| 10% | `#191919` | **1.19:1** |

So the slider reached a quarter of this program's own minimum, and 10
was the value a real config held. `BSSH_TEXT_BRIGHTNESS_MIN` is 40 --
the lowest step that still clears 3:1 -- and it bounds the slider's
range, the setter, and the getter.

**Clamping on READ is the half that matters.** A config already holding
10 comes back as 40, so the terminal is readable on the next launch
without anybody having to find the setting that made it unreadable --
which is the same argument the surrounding comment already made for
refusing a hand-edited 0, finishing its own sentence.

`the_brightness_slider_cannot_reach_unreadable` walks every value the
slider can produce and asserts each clears the floor, **and asserts the
step below the minimum does not** -- or the floor would be higher than
it needs to be and the slider would have lost range for nothing. A bound
wants both halves checked.

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

**`term_features` is refused rather than coerced, and that is the opposite of
every other field.** `from_json()` *clamps* the numeric ones, because a
scrollback of two billion has an obvious nearest legal value. A misspelled
feature name has none — `sixl` is not nearer to `sixel` than to anything else —
so falling back to the defaults hands somebody a preset whose file says one
thing and whose terminal does another, with nothing said. That is the hazard
`from_spec()`'s own comment describes ("report a pass for a configuration
nobody tested") and the one the command line already refuses for the identical
string, `exit 2` at parse time, "so a misspelling is a refused command line".
The preset path did not: `bssh_session_t` reads it at `session.cpp:86` and
`:180` with the error argument named `ignored`, and cannot do better — a
session being built has nobody to report to. **So it is checked where the file
enters.** In `read_ini()`, which is the single reader for both import doors —
the editor's file dialog and `import_launch_document()`, the Android
"open with" path — and not in either door, where it would have been written
twice and covered whichever was found first. `read_ini()` gained an optional
`error` out-parameter for it: the existing refusal says "does not look like a
preset file", which for this case points whoever was handed the file at the
wrong end of a working one. It stays empty when the file genuinely is not a
preset, since there is nothing to add. **The residue is a hand-edited
`presets.ini`**, which no longer goes through `read_ini()` and still falls back
silently; that is the same trust the tree extends to the rest of its own
config, and a typo there is the typist's.

**A host OVERRIDE reached the emulator without meeting any of these
bounds.** `from_json()` clamps scrollback, font size and keepalive, and the
comment saying why names sec 10.5: a preset is a file people send each other,
so the spinbox that used to enforce the ranges is not on the path. A per-field
override in `hosts.ini` is the same number in the same kind of file — and
`resolve()` inserted it verbatim, having checked only that the key still
exists. Its comment shows the STALE-key case was thought about and the
out-of-range one was not. `load_profiles()`' own comment uses
`scrollback_lines=99999` as its example of a hand-typed override, which is
below the bound but was never being measured against it; `999999999` reached
the emulator as written, which is the memory bound sec 10.2 added the clamp for
in the first place.

**The other end of that range was worse, and the clamp had nothing to do with
it. [FIXED]** `BSSH_PRESET_MIN_SCROLLBACK` is 0 and the preset editor's spin
box starts there, so "0 lines" is a value the UI invites. `bssh_session`
applied it behind `if (lines > 0)`, so the emulator kept its 5000-line default
while the page read 0 — and the app went on holding history the user had just
asked it not to keep. For a terminal that is a **privacy** setting rather than
a preference: somebody who turns scrollback off is asking for output not to be
retained, and a key or a password echoed into a session is the reason.
**ABSENT and ZERO are different, and `QJsonValue::toInt()` reports both as 0.**
The guard was defending the default against an absent key, which is the only
thing it could distinguish that way — and the case it protected against cannot
occur, because every caller passes a RESOLVED object and `to_json()` always
inserts this key. So it discarded the reachable case to guard an unreachable
one. Asking whether the key is *there* separates them properly. The same
mistake, in three different costumes, was fixed in `version-check` and in
`check-apk` on the same day.

**And a second defect was hiding behind the first, unreachable until it was
fixed.** `bssh_scrollback::push()` returned early at capacity nought without
touching `m_base`. But `base()` is documented as "how many lines this ring has
dropped off the front", and at capacity nought every pushed line is dropped the
instant it arrives — so the absolute numbering froze, and `base() + count()`
stopped being the number of lines that have gone past. That difference is
exactly what the emulator measures a resize against to keep images with their
text (§7.1), so with scrollback off, images would have stopped following the
text they were drawn beside. Nothing could reach it while the session was
discarding a scrollback size of 0.
**The fixture was wrong before the code was right**, which is worth recording
because it is the shape this document keeps returning to: the first version fed
three lines to a 24-row screen, nothing scrolled, nothing was ever pushed, and
`base()` correctly did not move. A test that cannot reach the distinguishing
case reports agreement whatever the code says. It feeds sixty lines now, and
the non-zero size is asserted BEFORE the zero one so it cannot pass by the
setting never being applied at all.

`resolve()` now returns `from_json(resolved).to_json()` — the round trip
through the struct, rather than a second list of `qBound` calls beside the
first. Two spellings of one schema drift, and this drift would show up as a
bound that holds for a preset and not for a host, which is the bug over again.
The round trip is lossless for every key the defaults carry, which the test
asserts directly rather than trusting.

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
  - **The in-app picker imports on Android too -- measured 2026-09-29.** It
    was recorded here as unable to *select* a file: on the Android 11
    emulator image `ACTION_OPEN_DOCUMENT` listed preset files greyed out,
    whatever the filter. On the API 33 image with Qt 6.12 the same intent
    (`typ=*/*`, `OPENABLE`, DocumentsUI's `acceptMimes=[*/*]`) lists a
    `picker-test.ini` from Downloads as choosable, and choosing it imported
    "Picker test" with its TERM. Nothing in the code changed; Android 11 was
    not run again, so whether the image or the Qt of the time was at fault
    is not known, and the entry says so rather than guessing. The hand-over
    route above works as well.
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
- **DECIDED 2026-09-26, and BUILT:** host folders, for organising only --
  settings still come from presets alone, so there is one inheritance
  mechanism rather than two. See §16.

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

#### The preset editor dropped a failed write without a word — **[FIXED 2026-09-24]**

Found 2026-09-03: `on_save`, `on_clone`, `on_delete` and `on_import` in
`src/ui/preset_editor.cpp` returned in silence when the store refused,
and so did the desktop preset import in `main_window.cpp`, two lines
above its "Preset imported" message -- so a button that did nothing
looked exactly like one that worked. The holder approved the fix that
evening ("go ahead with the preset editor fix"); it was drafted, then
displaced by the keyboard work, and landed 2026-09-24 on being asked a
second time.

**Both halves had to change.** The store gave no reason for most
refusals -- an empty name, a shipped preset, a preset hosts still follow
-- and did not clear `last_error()` on entry, so a caller had nothing
true to show. `save_preset()` and `remove_preset()` now clear it and give
every refusal a reason, as `remove()` already did; the editor's four
buttons and the desktop import show "Not saved" / "Not cloned" / "Not
deleted" / "Not imported" with that reason, the shape the host editor's
"Not saved" already has.

**And a rename could split the two files.** `hosts.ini` names a preset
by its slug, so a rename rewrites it -- and that rewrite's status was
discarded, so a failed host rewrite still wrote `presets.ini` under the
new section; the opposite order, `presets.ini` failing after the hosts
had moved, left them naming a section in neither file. Both now fail the
save and put the files back in agreement.

**Tested through a write that fails for root too**: a directory where
the `.ini` goes. A read-only directory is the usual way, and the
check-ci container runs as root, which writes one anyway -- so a
permission-based test would skip in the one place every push is gated.
`an_unwritable_preset_file_is_reported_and_undone` covers both rename
orders; `a_save_or_delete_the_store_refused_is_said` clicks the real
Save and Delete buttons and reads the warning they raise. Six sabotage
edits in `tool/sabotage/preset-refusals.toml`. Not covered by a test:
Clone, which takes Save's path but is not clicked by the fixture, and
the two imports, which are reached through a file dialog or a handed-in
document the fixture does not drive.

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

~~Still open and deliberately not bundled here: `do_send` and `do_resize`
discard their results too.~~ **Both fixed since, and this line was stale
(re-derived 2026-09-29 by reading them):** `do_resize` fails the session
when `ssh_channel_change_pty_size` does not return `SSH_OK`, and `do_send`
queues and writes at most the channel window, so input is neither lost nor
able to block the worker. Fixed apart from the bastion, as this line asked:
`749934e` for both results, `d020720` for the window.

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

**And both writes reported failure to a caller that read neither.** The
third act of the same story, and the one the two above could not reach,
because they were about the *order* of the writes rather than about
their *answers*. `offer_to_remember` asked the secret store to store the
password and returned quietly when it said no; then set the row's flag
and never looked at what `save()` returned at all. A user who ticked
"Remember" was told nothing either way, and found out at the next
connection — asked for a password they believed they had saved, with
nothing on screen connecting the two moments.

**The flag half is the expensive one, and it is a leak rather than an
inconvenience.** A secret whose flag never landed is read by nothing:
`stored_password_for()` returns early without the flag, `remove()`
erases a credential only for a row that claims to have one, and
`bssh_sweep_orphaned_secrets()` skips any key a live profile owns. So it
sits in the keyring, unused, unerasable, and it survives the deletion of
the host it belongs to — the very state §11 twice made deletion prevent,
arriving a third time by a third road. It is erased again now when the
flag cannot be written, and the one case where even the erase fails is
told to the user rather than swallowed.

**Where the fix went matters more than what it does.** The ordering, the
row check and the rollback are `bssh_remember_profile_password()` in
`profile_store.cpp`, beside the sweep and the orphan note that already
reason about both stores. What is left in the window is the half that IS
a window's job: turning a result into a sentence. The reason is the one
this document keeps arriving at from the other direction — the previous
version was untestable *because of where it sat*, a private method of a
widget behind a modal prompt, and three of these assertions cannot be
written against a window at all. §11's host-key entry above says the same
thing in the same words: when a decision cannot be tested where it sits,
the fault is usually where it sits.

**The lens was the previous day's fixes, asked from the other end.** Four
commits made `unlock()`, `store()` and `erase()` return false honestly
where they had been reporting success. The question that follows is not
where else that shape occurs — that sweep was finished — but **who reads
the false they now return**. One caller did not, and it was the only
caller any of them had, so the four fixes were invisible from where a
user stands: a store that correctly refused to write reached a widget
that discarded the refusal. A correct function is not a working feature.

Held by three tests and two sabotage edits. The failure is forced the way
§12.3's delete test forces it — a read-only directory for the hosts file,
with the credential store in the app's own config directory and still
writable, which is exactly the split that makes the secret land and the
flag not. The skip is conditioned on whether the flag reached the FILE
rather than on what the call returned, because an account that can write
a read-only directory (root in a container, which is what `check-ci` is)
would otherwise skip for the very defect the test exists to catch.

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

**The command line mangled every IPv6 address, and the config importer
had validated the same syntax correctly all along.** `from_target` split
`[user@]host[:port]` on the LAST colon, so `2001:db8::1` became host
`2001:db8:` on port 1 and `::1` became host `:` on port 1 — not a failure,
a connection to somewhere nobody asked for. It also read the port with
`toUShort()` and no `ok` flag, so anything unparseable or out of range
became **0**: worse than the 22 it overwrote, and a failure whose message
says nothing about the port.

`ssh_config.cpp` had `to_port` rejecting non-numeric, zero and
out-of-range with a fallback, for the same `host:port` syntax. **The
careless path was the one reading what somebody had just typed; the
careful one was reading a file** — the wrong way round if either had to be
careless.

Bracketed literals are handled first now, which is how OpenSSH writes an
address carrying a port and the form `known_hosts` already stores. An
unbracketed address with several colons is a host with no port, which is
OpenSSH's rule. Both paths call one `bssh_port_from_string`, and
`to_port` is a forwarder.

**And a FOURTH copy was still reading the same field with a cast.**
`load_profiles()` had `static_cast<quint16>(file.value("port", 22).toUInt())`
— so `port=65538` hand-typed into `hosts.ini` arrives as **2**, and `port=ssh`
as **0**, on a row whose editor still shows what was typed. That is the worst
version of this bug rather than the mildest: the connect bar at least fails
visibly, while here the file and the connection disagree and only the file is
readable. `is_valid()` cannot catch it either — it asks only whether a host is
named. It calls `bssh_port_from_string` now, like the other three.

**Third time tonight for one shape**: a rule with two implementations
where only one is careful — preset ranges in a spinbox but not the
importer, port validation in the importer but not the command line, image
bounds in the placement but not the decode. Each time the fix was to move
the rule to the data and have every caller use it, which is this
document's own "two places computing one thing" arriving from three
directions.

**The preset editor validated its numbers and the importer did not.**
§10.5 makes a preset a file people send each other, so `from_json` reads
numbers from wherever that file came from — and the ranges the app
believes in lived as file-static constants inside `preset_editor.cpp` and
`font_picker.cpp`. They were properties of a **spinbox**, not of the
field, so the only thing enforcing them was somebody typing.

`scrollback_lines` is the one that matters: §7 caps the ring because *"a
terminal left open on a chatty log is the normal case"*, and an imported
preset could raise that cap past any memory the device has. `font_size`
took 0 and `keepalive_seconds` took negatives by the same route.

**What makes this worth recording is why it was invisible.** `from_json`
already hardened against a hostile preset — it forces `id = 0` and
`is_builtin = false` so a shared file cannot impersonate a shipped one —
**and there was already a test for that**
(`an_imported_preset_cannot_claim_to_be_builtin`). So the code visibly
considered the adversary and had the test to prove it. A partial threat
model with a passing test reads as a complete one, which is the same
property that let bracketed paste survive above: every individual claim
audits clean.

The bounds live in `preset.h` beside the fields now and the editors read
them from there, so there is one definition rather than two copies of a
rule drifting from the data it describes. Clamped rather than refused,
matching what a spinbox does to an out-of-range number: rejecting a whole
preset over one field would discard the fifteen that were fine.

The test asserts against the named constants rather than literals —
hard-coding 200000 would add a third copy and pass while the editor and
the importer disagreed — and it re-asserts the identity hardening, since
extending a defence is exactly when the half already there deserves an
assertion rather than an assumption.

**A paste could close its own bracket, and the command after it was
typed rather than pasted.** §6.4 leans on bracketed paste to stop a pasted
newline executing, and §7.1 lists it as Done. It is implemented, and it
was escapable by its own payload: the marker that ends a paste travels in
the same byte stream as the data it wraps, so pasted text containing
`ESC [ 2 0 1 ~` ended the paste early. Demonstrated rather than argued —
the bytes that went to the remote were

    ESC[200~ safe ESC[201~ rm -rf ~ ESC[201~

so the far end saw the paste finish after `safe` and read the rest as
**typed input**. Somebody copies a line from a web page and the shell
takes the tail as something they entered.

**Two protections existed and the gap was between them.** Bracketed paste
is escapable by its content; the multiline confirmation reports "2 lines"
while the reader believes the bracketing covers them. Neither is wrong on
its own terms, which is why it survived — §7.1 could say Done and be
telling the truth about what it implemented.

Control characters are dropped from pasted text now: C0 except tab, DEL,
and C1 as well. **Dropped rather than escaped, because there is nothing
to escape to** — the remote's parser defines those bytes and a paste is
text, not a command channel. Tab survives because it is text people
paste. This is xterm's default (`disallowedPasteControls`) and what kitty
does by stripping the terminator, so it is the established answer rather
than an invented one.

The test asserts three directions, so it cannot pass against a paste that
dropped everything: no second end marker, the text still arrives —
including the command, delivered as ordinary characters *inside* the
bracket, which is exactly where a shell will not run it — and a tab
survives.

**Both image decoders trusted remote dimensions, and one of them read
8.59 GB out of bounds.** The kitty raw-format guard multiplied two `int`s
taken from the remote's own `s=` and `v=` controls:

    s=46341 v=46341 f=32
      bytes_per_line          =       185,364
      bytes_per_line * height = 8,589,953,124
      as a 32-bit int         =        18,532   <- what the guard demanded
      what QImage then read   = 8,589,953,124   <- 8.59 GB

So the check asked for 18 KB of payload and handed `QImage` a
46341×46341 image over it. `46341` is not arbitrary: it is the smallest
square whose 32-bit product wraps small enough for an ordinary payload to
satisfy, which is what makes it reachable rather than theoretical. Best
case a crash; worst case heap contents decoded into an image and drawn on
screen.

**The sixel path had the same overflow a hundred lines below**, in
`pixels[y * width + x]`, plus two more: no `isNull()` check after an
allocation that can fail, and no bound on the decoded dimensions at all —
`BSSH_MAX_IMAGE_CELLS` caps the *placement*, not the decode, so it never
reached this. One ceiling closes all three.

**Both ceilings are argued rather than picked.** 64-bit arithmetic cannot
wrap for any `int` pair, so the kitty comparison now means what it says —
and §7.1's payload cap then bounds the image for free, since an image the
guard accepts must fit a payload that already has one. The sixel ceiling
is 8 Mi pixels: 25 MB at three bytes each, inside `bssh_image_store`'s
32 MB session budget, and still larger than a 4K screen's 8,294,400.
Above it an image would be evicted on arrival, so decoding it was work
with no possible outcome.

**The sabotage is the evidence, and it does not fail — it crashes.**
Restoring the 32-bit arithmetic makes that test segfault rather than
report, which is the honest shape of the defect. It also attached
`gdb --batch` through the suite's crash handler, and a stopped-traced
process ignores `timeout`'s SIGTERM, so a debugger sat resident for an
hour with two zombies beside it — exactly the incident
`running-code.md` records. Cleaned up and verified empty. Worth knowing
before anyone re-runs that sabotage.

**Four more buffers had the same fault, and a switched-off protocol still
paid for it.** Generalising the title's cap found that this terminal
accumulates five remote-controlled strings and bounded exactly one:

| buffer | protocol | was bounded |
|---|---|---|
| `selection_buffer` | OSC 52 | yes, 64 KB |
| `dcs_buffer` | XTGETTCAP | no |
| `dcs_buffer` | sixel | no |
| `apc_buffer` | kitty graphics | no |
| `osc_buffer` | OSC 8, 4, 10/11, 1337 | no |

**And every image protocol tested `features.enabled(...)` AFTER buffering
the whole payload.** So a host could spend this terminal's memory on a
protocol the user had switched off — or, on Android, one the build cannot
even decode, since it compiles without libsixel. The switch stopped the
behaviour and not the resource, which is worth saying because "the
feature is off" reads like protection.

All four go through one `bssh_accumulate()` now, and sixel returns before
accumulating anything when it is disabled. Oversized payloads are dropped
**whole**, which is the selection's rule and right for the same reason:
half a base64 image or half a hyperlink is not a smaller version of the
thing but an invalid one. §8.3's title stays the deliberate exception,
because a truncated label is still a label.

**What the test does and does not pin.** The buffer is not observable
from outside, so the test asserts behaviour: with sixel off, a
quarter-megabyte payload leaves no image and the terminal still renders
text afterwards. Sabotage removing the early return fails it — but it
fails because the protocol then RENDERS, so what is pinned is the switch
rather than the memory. Said here because a green suite should not be
read as covering the bound.

**A remote could grow the title buffer without limit, and the reasoning
against it was already written down in the same file.** OSC titles arrive
as fragments and need no terminator to keep arriving, so a host that opens
`ESC ] 0 ;` and never closes it appended forever. Measured by sabotage:
262,144 characters from one sequence in a unit test, and that is only what
the test chose to send. The string reaches the tab label, the window title
and, on Android, the notification.

`on_selection_set` caps OSC 52 at 64 KB six hundred lines below, and its
comment states this case exactly — *"libvterm hands a long string over in
pieces as its own buffer fills, so accumulating them without a bound of
our own would put the cap back where it started: the remote decides how
much memory this costs."* The title was the one remote-controlled buffer
with no cap, beside a scrollback with a hard line limit (§7) and an image
store with a memory budget (§7.2), both bounded for this reason.

**The lens worth keeping is the sharp form of it**: not *"is this buffer
bounded?"* but *"this file bounds its other remote-controlled buffer and
says why — does that reasoning apply here?"* It applied verbatim.

**It applied once more, to the last unbounded thing in the file, and that
one is not a buffer of bytes.** `CSI > flags u` pushes a kitty keyboard
mode, and the push had no cap — five bytes on the wire per entry, cleared
only by a reset, with the remote choosing how many. What makes it the
interesting case is its neighbours: the POP is carefully guarded so the
stack never empties, and the SET writes in place. Two of the three
operations were thought about. The one that grows was not, which is the
half nobody checks when the others are careful.
`BSSH_MAX_KITTY_STACK` caps it at 64 — generous, because a program nests
these to save and restore its mode around a subprocess and anything past
a handful is not a program keeping track of itself. **The eviction takes
the entry ABOVE the base, never the base**, because the pop refuses to
remove the last one: a program popping its way back down has to land on
the zero it started from rather than on whatever eviction left behind.
`kitty_stack_depth()` exists so the bound can be asserted at all — the
same reason `scrollback().count()` and `images().count()` do. A bound
nothing can observe is a bound nothing can test, and the sabotage reports
the depth it reached (5001) rather than merely that it was too big.

**And the hyperlink cap was only half a cap, which its own commit message
had already said.** `e7c7fc3` bounded ONE uri at 4096 characters and
argued, about the bound it replaced, that "65535 times that is not a
limit". The same arithmetic applies to what it left: 65535 ids × 4096
QChars is about **536 MB**, held for the life of the session, because
`links` was never cleared — not by the reset that clears every other
piece of state this file owns. Reducing a worst case from half a terabyte
to half a gigabyte is worth doing and is not the same as bounding it.
`BSSH_MAX_LINK_BYTES` is 8 MB, placed beside the image store's 32 MB
rather than dwarfing it; an ordinary session is nowhere near, since
`ls --hyperlink` produces file:// uris of a hundred characters or so.
**Emptied wholesale rather than evicted one at a time**, because a QHash
has no order to evict by and the existing comment already accepts the
cost: a wrapped id "can only ever mislabel a link that has already
scrolled out of the bounded scrollback". What the clear buys is a ceiling
this program chose. The test asserts the most recent link still resolves,
so a terminal that simply stored nothing would not pass.
**Two of my own test's failures were the FIXTURE, not the code**, and the
second is worth writing down. `"\x1bc"` is not ESC followed by c: a hex
escape consumes every hex digit it can reach, and `c` is one, so that
literal is the single value 0x1bc. The test fed garbage and reported the
reset as clearing nothing — a specific, plausible wrong number rather
than a crash. The RIS test at the top of the same file uses `"\033c"`,
octal, capped at three digits, for exactly this reason: **when a
neighbouring test spells the same thing differently, the difference is
usually load-bearing.**

**The cap bounded the memory and not the LAYOUT, which is the other half
of the same string.** 1024 characters is a modest buffer and, in a
`QTabBar` with `setExpanding(false)` and no elide mode, a tab **11,472
pixels wide** — measured. Every tab after it sits past the right-hand
edge: present, but reachable only through the scroll buttons, which is
not where anybody looks for the session they were just in. No hostility
is needed either, unlike the unbounded case — a shell putting the full
command line or a deep path in its title gets there on an ordinary
afternoon.

Three widgets in this tree set an elide mode deliberately, and
`elide_status()` does the arithmetic by hand for the status label. The
tab bar, the one label a REMOTE writes, was the one that set none.
`Qt::ElideMiddle`, for the reason `bssh_host_list` already gives about
the same kind of string: the two ends identify a session and the middle
of a long path is the part nobody needs — and several tabs on one host
differ in their leaf directory, so eliding at the end throws away the
character that tells them apart. The test asserts structurally, that one
tab cannot be wider than the bar it lives in, rather than pinning a pixel
count to a style and a font.

**A bound is not a bound on everything**: capping a string's length says
what it costs in memory and nothing about what it does to a layout that
sizes to its content.

**The same lens found OSC 8, where the comment stated the wrong
conclusion out loud.** `begin_link` wraps its ids at 65535 and says so —
*"Ids wrap rather than grow without bound: a session left running on
output full of links would otherwise allocate for ever."* That bounds the
COUNT. Each entry is a URI straight from the remote, `links` is never
cleared, and the only thing limiting the size of one was the OSC payload
cap — so the real bound was 65535 × that, which is not a bound. Two
different quantities, one of them measured and the other assumed to
follow.

**Dropped whole rather than shortened**, at 4096 characters. That is the
call `on_selection_set` makes for the clipboard and the opposite of the
title's, and this file already wrote down the rule that decides which:
half a title is still a label and nothing executes it, while half a URI
is a DIFFERENT address — and this one is handed to a browser. The refusal
also CLOSES any link already open. Returning with `open_link_id` standing
would have attributed everything printed afterwards to the previous URI,
which turns a refusal into a wrong destination — the single outcome the
whole rule exists to prevent, reintroduced by the guard against it.

**And the confirmation dialog had the tab bar's bug, in the one place it
matters most.** The prompt asking whether to open a remote's address put
the URI in verbatim. At 2083 characters — which the OSC 8 convention asks
emulators to *support* — that is a dialog **14,211 pixels wide** on an
800-pixel screen: a box whose entire purpose is letting somebody read
where they are about to go, too wide to show it. No hostility needed; a
signed download link runs to hundreds of characters on an ordinary day.
Shown elided, opened whole — a shortened address is a different one, so
only the display changes. `bssh_uri_for_prompt` is pure and lives in the
header so it can be tested without a modal dialog, which is the shape the
libssh rules were extracted into for the same reason. Elided in the
MIDDLE: the scheme and host say who is being talked to and the tail says
what is being fetched, so eliding at the end hides the tail and eliding
at the start hides the HOST. The limit is measured rather than picked —
485px at 64 characters, 700 at 96, 807 at 112, 1130 at 160 — and 96 is
the widest that still fits with margin.

Truncated where the selection is dropped whole, deliberately: half a
clipboard is a different string and pasting it is worse than pasting
nothing, while half a title is still a label and nothing executes it. The
divergence is in the comment so it does not read as an inconsistency
somebody should tidy.

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

- **A failed write left the store disagreeing with what it had just told the
  caller. [FIXED]** `store()` and `erase()` changed `m_secrets` and *then*
  flushed, returning what the flush said. So when the write failed the caller
  was told it failed and the process behaved for the rest of its life as
  though it had not.
  **The erase is the direction that costs.** A user asks to forget a
  password, is told it could not be done, and it is gone from memory anyway
  — the app cannot use it, and the next successful write for any *other* id
  persists the erasure that was reported as failed. The store either holds a
  secret or it does not, and the answer has to be the one it gave. Both
  directions roll back now.
  **Same fault as the profile store's delete, one store over**, found by
  asking the question that fix suggests: who else changes state before a
  write that can fail? There it was the credential erased ahead of
  `write_profiles()`; here it was memory mutated ahead of `flush()`. Fixing
  one did not fix the other, and nothing connected them.
  **And `unlock()` itself, which gates the other two.** On first use it set
  `m_unlocked` and *then* returned `flush()`, so a failed write answered
  false while `is_unlocked()` answered true — the store denying it was open
  and claiming to be open at once. That is the worst place in the file for
  it: anything asking `is_unlocked()` before prompting skips the passphrase,
  believing the store is already open, and `load()` then returns empty,
  which reads as "no secret saved" rather than "the store never opened".
  **Four instances of one shape in a day**, each found by asking the last
  fix's question — who else changes state before a write that can fail? The
  profile store's credential ahead of `write_profiles()`, this file's
  `store()` and `erase()` ahead of `flush()`, the Android keystore's pair,
  and `unlock()` a few lines above them. Nothing connected any of them, and
  fixing one never fixed the next.
  **The Android backend had it too, and no gate here can even compile that
  file.** `secret_store_keystore.cpp` carries the identical
  mutate-then-flush in both `store()` and `erase()`, fixed the same way. It
  is listed only inside `beerssh.pro`'s `android {}` block, so the desktop
  build, the test build and `check-ci` all skip it — and it includes
  `QJniObject`, which exists only on Android, so it cannot be compiled here
  at all. A stub of the JNI surface was tried and abandoned: a stub that
  drifts from the real API would pass code that does not build on Android,
  and false assurance is worse than none.
  **So that fix is verified by symmetry and inspection only** — the added
  lines are token-identical to the file backend's, which compiles and is
  tested, and use only types already in that translation unit. Worth knowing
  as its own fact rather than as a footnote: **a source file in this tree is
  compiled by no routine gate**, so any change to it — this one included —
  is unchecked until somebody builds for Android.
  **Forcing the failure is the part worth copying.** The test puts a
  DIRECTORY where the file goes. Deleting the file does not work — the path
  helper calls `mkpath` on every call — and making the directory read-only
  does not either, because the container `check-ci` gates in runs as **root**
  and root ignores the mode. That cost a test earlier in this project's life,
  so the reasoning is in the test rather than left to be rediscovered.

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
    **~~The cause is not established and three hypotheses are dead.~~
    Established 2026-09-04: it is `com.android.zipflinger`, filling the hole
    left by the PREVIOUS `libbeerssh_arm64-v8a.so` during an incremental
    repackage.** Three hypotheses were indeed dead — not the fonts, not
    warm-versus-cold as such — and the entry that survived them, that the
    state "cannot now be reproduced or diagnosed", was wrong twice over: it
    reproduced on the next build, and it is diagnosable in an hour.

    `ZipArchive.fillFreeLocation()` in `zipflinger-9.2.1.jar` writes free
    space as a chain of nameless local file headers in 65,475-byte chunks,
    with a literal DOS timestamp of 1981-01-01 01:01:02 — which is the
    `21 08 21 02` in every stub. AGP's packaging task opens whatever APK is
    already on disk, so a rebuild deletes the old library entry and re-adds
    the new one at the tail, leaving its corpse behind as stubs. The hole is
    slightly SMALLER than the current library, so first-fit cannot reuse it
    and the cycle repeats every build where the library grows.

    **The control is what makes this a diagnosis rather than a story**, and
    it is the part to copy: writing a fresh archive through the same jar
    produces **zero** stubs, while reopening one, deleting entries and
    re-adding produces stubs whose first thirty bytes are byte-identical to
    ours. Neither `zipalign` nor `apksigner` is involved — this project runs
    neither, and three build stages of the APK are byte-identical, so it
    leaves Gradle already containing them.

    **And the earlier measurement here was low.** It is **111 stubs,
    6,954,823 bytes, 14.91%** of the file in SIX regions, not one; the
    single 6.9 MB gap is the largest. The 4,096 bytes before the central
    directory that a naive gap walk also reports are not dead space at all
    but the APK Signing Block. Reported as one region because the first
    script stopped at the first gap it found — a check over part of a
    population reading exactly like a check over all of it, which is the
    same fault this document records for the keyboard's row arithmetic
    (§6.2), on the same day.

    **Recoverable, measured end to end:** `zipalign -f -P 16 4` followed by
    a re-sign gives 39,707,611 bytes, `apksigner verify` passes, and all ten
    `.so` stay 16 KB aligned. The order matters — `zipalign` alone destroys
    the signature, since it drops the signing block — and `-p 16` is not
    `-P 16`: the lower-case form takes no argument and silently gives 4 KB
    alignment, which would break 16 KB-page devices. **Not yet done here**,
    because it belongs in the shared `tool/android.mk` that four projects
    copy, which makes it a signal rather than a local edit
    (`harmonization.md`).

    **~~Open: whether deleting the output APK before packaging is enough.~~
    Measured 2026-09-04, and it is: 46,639,876 → 39,692,128 bytes, ZERO
    stub regions, 6,947,748 saved, and all ten `.so` still on 16384
    boundaries.** One `rm` of
    `android-build/build/outputs/apk/debug/android-build-debug.apk` before
    the build, which AGP then re-runs because its output is missing. So
    prevention works and the `zipalign` post-process is the fallback rather
    than the remedy.

    **The alignment check that says so was wrong the first time**, and it
    is worth the sentence because the error would have raised a false
    alarm about 16 KB-page devices. Python's `ZipInfo.extra` is the
    CENTRAL DIRECTORY's extra field; zipalign's padding lives in the LOCAL
    header's, and the two differ. Computing the data offset from the
    central copy reported all ten libraries misaligned in both the new
    APK and the old fixture — which, since the fixture was independently
    measured as aligned, is what said the instrument was at fault rather
    than the artifact. Read `nlen`/`elen` from the local header at
    `header_offset` instead.

    **Scope beyond this tree is NOT established, and the first report of
    it said otherwise.** The signal to `claude-guidelines` originally read
    "four projects copy the fragment so all four are almost certainly
    paying it" — a scope claim reasoned from a proxy rather than from any
    APK. fuzzypickles measured theirs within the hour: zero stubs, 129
    named local headers, a walk reaching 99.9% of the file. Their own
    caveat is the reason it stays open rather than closing the other way:
    they cannot say whether that build was incremental, and a zero from a
    build that never reopened an existing output is exactly what the
    fresh-write control predicts.
    **Later the same day (2026-09-04): hydra confirmed stubs in its APK**,
    so the measured scope is two of four projects, recorded in
    `claude-guidelines`' `project.md`. Deleting the previous APK first was
    reproduced on a second and third build here, 39,692,128 and then
    39,708,512 bytes, each with zero stubs. **This tree's `make android`
    still does not delete it** -- the measurement used a scratch script --
    so the prevention is known and not wired in.
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
- **Desktop `ssh-agent` support (`SSH_AUTH_SOCK`) -- USED, and was for some
  time before this line said so.** `ssh_userauth_publickey_auto` offers the
  agent's keys before the default identities; measured 2026-09-26 by reading
  `ssh_session_libssh.cpp`. Adding keys TO the agent was declined for now by the
  holder the same day (§16). What this paragraph imagined -- the agent as a
  `bssh_secret_store` that never yields key material -- is a different thing
  and nobody has asked for it.
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


#### ~~Tearing out the FOCUSED session hangs~~ — **[WITHDRAWN 2026-09-05, the diagnosis was wrong]**

**It does not hang, and this entry was published saying it did.** The
correction is kept in full rather than deleted, because the way the
false diagnosis was reached is worth more than the fact it was false.

What was written: that `tear_out()` does not return when the session is
`m_current`, on the evidence that changing the Detach item to
`tear_out(m_current)` took its test from 200ms to QtTest's 300-second
timeout, and that reverting the one line put it back to green. **Both
observations were true and the conclusion did not follow.**

**A backtrace settled it in one look**, and should have been the first
move rather than the last:

    #8  QDialog::exec()
    #9  bssh_main_window::closeEvent(QCloseEvent*)
    #21 layout_test::the_detach_action_makes_a_window()

The hang was in the TEST'S OWN CLEANUP. `closeEvent()` asks before
closing a window with a live session in it, and a modal dialog offscreen
waits for an answer that never comes. Reverting the line did fix it --
by changing which session was torn out, and so whether the window being
closed still had one. **A revert that cures a symptom is not a diagnosis
of it**, and this is the whole of what went wrong: a one-line change and
a clean revert felt like a controlled experiment, and the variable it
actually moved was three steps away from the one being named.

**Then the fixed cleanup crashed, and that was the test too.** With the
dialog avoided it segfaulted in `QMetaObject::cast` -- deleting windows
while walking a snapshot of `topLevelWidgets()`, which holds raw
pointers and includes top-level children a window owns, so the next cast
read freed memory. Collected first, then deleted, and 522 tests pass
with `tear_out(m_current)` in place.

**So two "faults in the code under test" in a row were faults in the
test**, and the published one carried a mechanism -- an ordering hazard
in `take_session()` -- that was invented to explain a measurement whose
cause had not been established. That is the failure `evidence.md` names:
a specific wrong cause is more actionable than a vague one, and sends
the next person somewhere with the authority of a diagnosis. The entry
said in as many words that the mechanism was "not established"; it
should not have named one at all.

**What is true, and shipped:** the Detach item now takes the session you
are looking at rather than whichever tab happens to be current. See
§8.4.

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
- The optional font families are **downloaded on the phone** from their pinned
  releases (§9.4), not packed; the desktop package just installs them.
- Keep the platform-conditional surface in the build files as small as it is in
  the code: one `android { ... } else { ... }` scope selecting
  `platform_android.cpp` vs `platform_desktop.cpp` and the dependency source, not
  conditionals sprinkled through the file. A second scope appearing elsewhere in
  `beerssh.pro` is the same smell as an `#ifdef` in the sources.


**`make clean` must always cause a rebuild — [FIXED 2026-09-04].** It did
not: `make clean && make` built a man page and nothing else. Reported by
the copyright holder, who also named the principle — *that is what clean
is for* — which is why the fix is not the one missing line.

The mechanism is worth keeping because it is invisible from either half.
The binary at the repo root is a **copy** of `$(BUILD_DIR)/$(TARGET)`,
and its prerequisites are the *sources* rather than the thing it was
copied from. So removing the objects left the copy in place, newer than
every source, and make correctly concluded there was nothing to do. The
man page rebuilt only because `clean` *had* removed it — from the stale
binary.

**The tell was inside the file.** `veryclean` carried `rm -f $(TARGET)`
and `clean` did not, so the two disagreed about whether the binary is a
build artifact. Neither is wrong on its own page; only reading them
together shows it.

So the fix is structural rather than a remembered line: `ARTIFACTS =
$(TARGET) $(MAN_PAGE)` is named once, `all` builds it and `clean` removes
it, and `veryclean` gets it by depending on `clean`. Adding a product to
the default build now makes clean remove it without anybody remembering
to. Verified by reproducing the fault first — a `beerssh` newer than the
sources with no man page beside it produces exactly the reported
one-line build — and then watching a real `g++ -c` come back after the
change.

**`make deb` no longer runs the test suite — [FIXED 2026-09-04].** It
did, and it broke there. `debian/rules` is the minimal `%: dh $@`, and
**`dh_auto_test` runs `make test` whenever a test target exists** — so
packaging built and ran all 516 cases, most of them Qt widget tests that
want a display. That fails in a build chroot for a reason unrelated to
packaging, and it fails on a developer machine too the moment anything
else is building, because two `make test` runs share `build-tests`.

`override_dh_auto_test:` is the whole fix, and nothing is lost by it:
`make test` and `make check-ci` gate this project, they run before every
push, and check-ci builds in the runner's own image — a stricter
environment than `dpkg-buildpackage` provides. Running the suite again
during packaging is a second copy of a check that already passed, which
takes the package down with it when it collides. The override records
how to get it back — an offscreen platform in a `dh_auto_test` override
— so the decision is reversible rather than merely absent.

**Proven with a positive control**, since a step that is absent and a
step that never appears look identical: with the override commented out
`dh binary --no-act` lists `dh_auto_test`, and with it in place the step
is gone. Checking only the after state would have shown nothing either
way.

**A cold `make android` compiles one file at a time unless the outer make
has `-j`** (2026-08-19). The recipe recurses as `$(MAKE) -C
$(ANDROID_BUILD_DIR) apk_install_target` with no `-j` of its own, so it
inherits whatever the caller gave: the first device build hit a 40-minute
`timeout` (exit 124, no compiler error) after ~70 objects built serially
on twelve cores, and `make -j12 android` completed. Incremental rebuilds
are ~20s and do not notice.

**A killed compile can leave an object make believes** (2026-09-09). An
out-of-memory kill left a truncated `emulator_test.o` with a fresh mtime;
make took it as current and the link failed far from the cause. After any
killed build, delete the objects newer than the last good link, or
rebuild clean.

**`.gitignore`'s `fonts/` is unanchored, so it also ignores
`android/assets/fonts/`** (2026-08-20). That happens to be wanted, but by
accident: `android/assets/terminfo/` has its own rule and the fonts
directory rides on the root one. The consequence is that `android/assets`
is an unignored directory whose every child is ignored, which `git
status` reports as nothing at all while `git clean -xdn` lists 9 MB --
so a cleanup report of "untracked-and-unignored: none" said nothing
about it.

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
  **Its menu category is System, beside the other terminals** (2026-09-24).
  It said `Network;RemoteAccess;TerminalEmulator;`, which put it alone
  under Internet in TDE's menu while Konsole, xterm, uxterm, QTerminal and
  cool-retro-term all sat in System -- measured with `tde-menu
  --print-menu-id` against a menu cache built in scratch, before and
  after, so the live one was never rebuilt. Keeping `Network` beside
  `System` would list it twice, and `desktop-file-validate` was already
  hinting that `TerminalEmulator` belongs with `System`.
  **The metainfo shipped unvalidated until 2026-09-21, and did not
  validate.** `check-deb.sh` ran `desktop-file-validate` on the desktop
  entry from the day it existed and never asked `appstreamcli` anything
  about the file beside it, so `appstreamcli validate` had two complaints
  nobody had seen: a missing developer and a missing homepage. §17's rule
  that a rule which is not mechanically checked stops being true, applied
  to the one metadata file that decides whether a software centre shows
  the app correctly.
  The developer is added — factual, and `harmonization.md` already asks
  the holder be named on the surfaces a reader looks at, of which a
  software centre is one.
  **The homepage is deliberately still absent.** The repository is
  private, measured with `gh repo view` rather than assumed, so a
  homepage would send a user to a 404: absent is the correct value and
  the warning is right to raise it. It goes in when the repository goes
  public.
  **The gate asserts the expected issues as a SET rather than ignoring
  them.** `appstreamcli` exits 3 for any issue at all, so the status
  cannot discriminate the known warning from a new one; the check fails
  on any error and on any warning that is not the homepage. An ignore
  list would let the next complaint in silently, which is how a gate gets
  switched off by instalments.
  **And that check shipped VACUOUS for one commit, which is the sharper
  lesson.** `appstreamcli` is not in `debian:trixie`. The first version
  ran it with a trailing `|| true` and then grepped the output for `E:`
  and `W:` lines, so the command failed, the log held a shell error
  rather than findings, both greps matched nothing, and the check printed
  that the metadata validates. **A gate over an empty list, reporting
  success exactly as loudly as a real pass** -- written into `b5547c6`,
  whose own message quotes the neighbouring rule about checks that are
  not mechanically run. The tool is installed in the image now, its
  absence is a hard failure rather than `|| true`, and an empty log is
  refused: a guard whose failure is not the caller's failure is not a
  guard.

  **[BUILT 2026-09-21] `lintian` runs over the installed packages**, which
  it never had. It is the standard check for this format and it is the
  tool that WOULD have caught the metainfo shipping invalid -- its first
  honest run reported `appstream-metadata-validation-failed` alongside two
  things nobody had seen.
  The expected tags are named as a POPULATION with the reason for each,
  and a tag outside it fails the build. `duplicate-font-file` is
  deliberate: §9.2 pins a curated set precisely so the app does not depend
  on a distribution's font versions, which is what pinning is for.
  **`debug-file-with-no-debug-symbols` is a live finding and is NOT
  settled.** `dh_auto_build` runs the Makefile, which wraps qmake, so
  Debian's build flags never reach `QMAKE_CXXFLAGS_RELEASE`; nothing emits
  `-g`, and the `-dbgsym` package therefore ships a debug file with
  nothing in it -- a package that claims to carry symbols and does not.
  Both fixes cost something and neither is recorded anywhere: `-g`
  globally also inflates a from-source `make install`, and scoping it to
  the packaging build needs a hook the Makefile does not have. It is
  tolerated in the expected set so the gate can run at all, named there
  with this reasoning, and that line is what somebody deletes when the
  question is answered. **Tolerating it in a named set with a
  justification is not the same as an ignore list**, which is how a gate
  gets switched off by instalments.

  **[FIXED 2026-09-21] Four gates read "0 failed" as success, which a run
  that verified nothing also satisfies.** `check-ssh.sh` and
  `check-keepalive.sh` judged their phases with
  `grep -q "^Totals: .* 0 failed"`. QtTest prints
  `Totals: 0 passed, 0 failed, 1 skipped` for a run where every case
  skipped, and that matches — so each gate could pass having executed
  nothing. `evidence.md` on a tally and a status being halves of one
  result, in four shipped gates at once.
  It is reachable rather than theoretical: `check-ssh.sh` runs
  `ssh ... || true` and, if that fails, silently never exports
  `BSSH_TEST_OPENSSH_KNOWN_HOSTS`, whereupon the OpenSSH-interop cases
  `QSKIP`. There is no `else` and no message, so the gate cannot tell
  "interop verified" from "interop never attempted".
  **The obvious fix was wrong and the baseline is what caught it.**
  Forbidding skips looks right — a skip is how the interop check vanishes
  — and skips here are STRUCTURAL: each phase runs a filtered subset, so
  the other phases' cases skip. Measured on a real run against a live
  sshd: 18 passed with 5 skipped, then 8 with 15, then 3 with 20.
  Requiring zero skipped would have reddened a working gate on its first
  run for something that is not a defect.
  What went in requires a positive PASS count instead, which closes the
  hole while leaving the structural skips alone, and was checked against
  all three shapes — a real run, an all-skipped run, and a genuine
  failure — before the files were touched.

  **A trap worth knowing before editing `check-deb.sh`:** its whole body
  is one single-quoted argument, so a comment containing an apostrophe
  ends the quote sixty lines early. The first version of this block did
  exactly that and `bash -n` caught it; the note is now in the file where
  the next person will meet it.
- **macOS/Windows:** `.dmg` / installer, best-effort per §1, unsigned until
  someone actually wants them (code signing on both is a cost, not a checkbox).

### 12.2b The keyboard lives in `keyboard/` — **[DONE 2026-09-06]**

Asked for by the copyright holder, who expects to want these routines in
other software. The move is the preparation; taking them out is a later
and separate piece of work.

**At the repo root beside `vterm/`, not under `src/`**, which is the
holder's own correction after the first attempt put it in
`src/keyboard/`. The root is where this tree already keeps a separable
library: `vterm/` is a submodule there, built by its own rules and
included through its own path. A keyboard that is expected to leave
should sit in the shape it will leave in, so that becoming a submodule
is a change to `.gitmodules` rather than a second move.

It costs two lines elsewhere. `INCLUDEPATH` gains the repo root in both
`.pro` files so `keyboard/...` still resolves, and the `Makefile`'s
`SOURCES` and `HEADERS` wildcards name `keyboard/` explicitly — those
are the qmake build's PREREQUISITES, so missing a directory does not
fail, it quietly stops triggering rebuilds.

`key_cap`, `key_row`, `key_row_layout`, `soft_keyboard` and
`soft_keyboard_layout` — ten files — moved with `git mv` so history
follows them. Nothing else changed: the bulk edit was proved by
comparing each file against `git show HEAD:` and asserting that every
differing line is an `#include`. Sixteen files, twenty-six changed
lines, no violations.

**What still binds it to beerssh, which is the whole remaining job:**

| dependency | why it is in the way |
|---|---|
| `term/emulator.h` | the key codes and modifier masks. A shared keyboard needs its own key vocabulary, or that enum moves with it |
| `input/input_router.h` | where keys are sent. The natural seam: an abstract sink rather than this program's router |
| `platform/platform.h` | `key_row` only |
| `ui/touch_metrics.h` | a genuine shared UI concern — 11 non-keyboard files use it, so it stays where it is |

**The move's real hazard was not the build.** Five sabotage specs name
source files by path, and they still said `src/ui/`. A sabotage that no
longer applies is indistinguishable from a check that cannot fail, and
nothing in a build or a test run would have said so. Sixteen path
references were updated across the specs and `tool/gen-layout.py`.

**Which raised the question of how many were ALREADY stale, and the
answer was fifteen of 167 — [FIXED 2026-09-06].** Measured by parsing
every spec and counting its `find` anchor in the file it names. The same
fifteen failed at `6faac65`, before the move, so the move caused none of
them. Every edit matches exactly once -- and `make style` recounts that now rather than this sentence carrying a number, which is why the figure is gone from it.

**They had rotted in four distinct ways, and only one was a simple
drift.** Seven anchors still described live code that had moved — a slot
run reaching AD11 rather than AD10, a palette row that grew a hyphen
when the grid went to 22 columns, `sticky_modifiers` becoming
`effective_modifiers`, a row calculation gaining `history_base()`. Four
had been reorganised and needed re-deriving: tabs build into
`m_tab_order` now rather than being read out of `m_shown` by index, and
`set_width()` is no longer the last line of its function. Two named a
construct that no longer exists — "a row one key short" pointed at a
hardcoded `literal_row("asdfghjkl;")` that the slot/legend split
replaced, and it shortens the AC slot run instead.

**And two were retired rather than repaired, which is the case worth
knowing.** Both `reconnect` entries broke hand-rolled workarounds that
`vterm-patch/0005` DELETED on purpose: one fed `CSI ? 1049 l` because
`vterm_state_reset()` set `mode.alt_screen` by assignment and never told
the screen layer, the other cleared the mouse shadow by hand because
nothing announced the reset. One call does all of it now, and an entry
breaking that call was already in the same spec. Writing replacements
would have duplicated it, so they are gone with the reason recorded in
the spec.

**Two `replace` halves were wrong as well, which an anchor check cannot
see.** The number-row sabotage substituted a ten-key slot run into a
22-column grid, so it would have been caught by the shape test rather
than the reachability test it names — a sabotage passing for the wrong
reason, which is worth no more than one that misses.

**Proved by RUNNING them, 49 edits across the twelve repaired specs
from a 532-passing baseline: 46 caught, 3 missed.** Matching an anchor
turned out to prove very little, which is the finding.

**One miss was an anchor that had never broken.** "The emulator never
hears the resolved TERM" pointed at the `set_term_name()` call in
`set_settings()`, and deleting it changes nothing a test can see:
settings resolve before anyone presses connect, so there is usually no
transport and that whole block is skipped -- which is why the apply path
grew a second copy, and that copy is the fix. **A sabotage can rot
without its find string ever failing to match**, and no anchor check can
see it. Re-aimed at the apply path and re-run: caught.

**Its anchor needed a leading NEWLINE, because the runner matches
substrings rather than lines.** The one-tab call is contained in the
two-tab one, so the first re-aim matched both sites and the uniqueness
check reported a bland "2" rather than naming the wrong site.

**The other two misses were gaps in the TESTS, and both are closed —
[FIXED 2026-09-06].** `scroll`'s "the block cursor covers its character"
and `windows`' "the last tab may tear out" applied cleanly and the suite
stayed green.

**Neither spec was wrong about the code; both were wrong about the test
that covered it.** `expect` is a claim about coverage and nothing checks
it, so a spec can name a test that exists, runs and passes while the
sabotage it describes goes unnoticed — and the verdict then reads MISSED,
which looks like a hole in the tests rather than a mis-aimed spec.

`the_last_tab_does_not_tear_out` asks `tear_out()` DIRECTLY, which is the
whole point. The old `expect` named
`the_detach_action_makes_a_window`, which drives the menu ITEM — and the
menu is disabled for a single session, so it never reaches the guard.
Every other caller tears out with two sessions, where the guard does not
fire. One line stopping a window from emptying itself, covered by
nothing.

`the_block_cursor_does_not_cover_its_character` asserts a RELATIONSHIP
between two renders rather than pixels: a block over a letter must not
look identical to a block over an empty cell, and with the punch-out
removed both are the same filled rectangle. That needs no knowledge of
the grid inset, the palette, or which pixels the glyph occupies. It also
states the two preconditions the block path needs — the view focused, the
style block — because an unfocused view draws an outline and a bar cannot
cover anything, and either would make it pass while measuring nothing.
**The first run failed on that precondition rather than on the
assertion**, which is the check refusing to measure the wrong thing.

Both specs re-aimed and re-run: all 14 edits in `scroll` and `windows`
caught, the two former misses among them.

**And then the other 42 specs, which had never been run: 112 edits,
110 caught, 2 missed — [MEASURED 2026-09-06].** Every spec in the tree
has now been executed at least once. The two misses are a THIRD kind,
distinct from the mis-aimed anchor and the mis-aimed `expect` above, and
neither is a gap in the tests.

**The covering test exists, is correct, and does not RUN here.**
`a_sweep_does_not_erase_a_password_a_live_host_now_owns` skips with
"this store cannot be locked, so it has no orphans to sweep" — this
machine's secret store needs no passphrase, so there are no orphans and
the scenario is unreachable.
`a_send_to_a_remote_that_is_not_reading_still_lets_go` is in
`live_ssh_test` and needs a real server. A MISSED verdict for either
means the test did not run, not that nothing covers the code.

**The sweep one is FIXED — the test injects a lockable store now.**
`bssh_secret_store::set_instance()` is the same seam
`bssh_ssh_session::set_factory()` already is, wired into both platform
selectors so there is one definition rather than a copy each. The double
is in-memory rather than the real passphrase store: that store's whole
point is a slow key derivation, so injecting it cost about ten seconds
per test and tripped the suite's per-class timeout and its output cap.
The code under test is the SWEEP, not anybody's cryptography, so the
double has the one property the sweep needs -- a store that locks, and
refuses to write or erase while locked. Both tests run and pass now,
and re-running the spec catches all three edits including the one that
was missed.

**It was the uncomfortable half**, and it is `evidence.md`'s
*a gate that needs the thing it checks for is weakest where it matters*:
the test can only run on a machine whose store LOCKS, which is exactly
the machine where an orphan sweep can erase a live host's key. A green
run on a keyring-backed desktop says nothing about it. Recorded in the
spec rather than papered over, because the fix is a lockable store in
the fixture and that is its own piece of work.

**So the runner has three failure modes and only one is mechanical.** An
anchor that no longer matches is detectable by a script. An `expect`
naming a test that does not cover the defect, and an `expect` naming a
test that does not run, are both invisible until the spec is executed —
and both report as MISSED, which reads like a hole in the code's tests
and is not.

**Proved by running a spec end to end rather than by matching strings.**
`soft-keyboard.toml`, from a 532-passing baseline: all four caught by
the tests they name, including both entries repaired here — the keyboard
visibility anchor at 3 failures and the re-aimed AC row at 32. The tree
came back clean and the journal empty.

### 12.2e `make check-ssh` was dead for thirteen days — **[FIXED 2026-09-07]**

Found while trying to run `ssh-send-window.toml`, which is the one spec
whose covering test needs a real server. The target could not run at all:

    check-ssh: ./build-test/beerssh_test is not built. Run: make test
    make: *** [Makefile:684: check-ssh] Error 1

`make test` had of course already been run. `TEST_BUILD_DIR` is
`build-tests`; the script looked for `build-test`, so it sent everybody
round the same circle.

**The cause is a rename that reached outside what it owned.** `1080cee`
(2026-08-25, *"build: singular directory names -- test, vterm-patch"*)
rewrote `tests` to `test` and caught `build-tests` in two scripts, while
the Makefile that CREATES that directory kept the plural. It was measured
with `git log --follow -S`, after a first attempt blamed the `tool/`
rename of the same day — `e360f38` moved the file with **zero** content
changes, and `-S` on the new path alone reports the rename because that
is where the string first appears under that name.

**Nothing noticed because nothing runs it.** `check-ssh` and
`check-keepalive` are in neither `make test` nor `check-ci`. The rename
commit's message cites `make style` and `make test` both passing, which
is true and says nothing about a check that runs under neither —
`evidence.md`'s *a check that runs only in CI is a check you have stopped
running*, with "by hand" in place of CI.

**`check-keepalive` had it three times**, inside its container, where the
build also produces `build-tests/`. Its phases wait 120 half-seconds for
a session that can never come up and then report *"session never came
up"* — a true sentence about the wrong thing.

**Fixed so the path cannot disagree again.** `make check-ssh` passes
`BSSH_TEST_BIN=$(TEST_BIN)`, and the script no longer carries a default:
it refuses and names the target rather than guessing a second copy of a
value the Makefile owns. `check-keepalive.sh` names the binary once with
a guard that fails at the right place if the directory moves. Three stale
prose references — in the Makefile, `test/ui_shots.cpp` and this document
— said `build-test/` too and now do not.

**Both halves are verified by running them.** `make check-ssh` exits 0
— three phases, 29 passes, 0 failures. `check-keepalive` exits 0 too,
and the entry above said it was unverified for about an hour before that
run happened; the sentence is rewritten rather than appended to, because
a reader who finds both believes the more cautious one.

What the keepalive run establishes, and it is more than the path fix.
Three tests ran that `make test` and `make check-ssh` can BOTH only skip,
because each needs packets to vanish rather than be refused:

    a_black_holed_link_is_noticed                      PASS   67s
    a_dropped_session_reconnects_when_the_link_returns PASS   68s
    a_black_holed_jump_host_is_noticed                 PASS   66s

Those timings sit against this script's own header, which recorded 62s
with the socket options and still-connected after 240s without them — so
the budget §5.3 exists to hold is still being held, measured rather than
assumed, on Qt 6.4.2 in the container rather than the desktop's 6.8.2.

**And the guard added with the path fix stayed silent**, which is the
part that could not be settled by reading. It refuses if
`build-tests/beerssh_test` is missing after the container's own `make
test`, and the fix rested on that being where the build puts it. It is.

**Every gate was run on 2026-09-22 to ask the question this entry is
about, rather than waiting to trip over the next one.** §12.2e's lesson
is that a gate stops silently: the suite stays green, and the thing that
would report the gap is the thing not running. So each was run and its
result read, on this machine, with what it needs:

    make check (style + test)   passes
    make check-ci               passes, ubuntu:24.04
    make check-deb              passes, debian:trixie
    make check-ssh              passes, against /usr/sbin/sshd
    make check-keepalive        passes, 66s, in the container
    make check-fonts            passes, 8 families
    make check-handoff          passes, both fault modes detectable
    check-apk / check-java      need the Android SDK and NDK; not here

**And the live SSH phases were enumerated against the gates that run
them**, because a phase no gate selects is a test nothing executes --
the same shape as a sabotage naming a test that cannot fail.
`live_ssh_test` names six: `drop`, `reconnect`, `rekeyed`, `trust`,
`tunnel_black_hole`, `tunnel_drop`. `check-ssh` runs `trust`,
`tunnel_drop` and `rekeyed`; `check-keepalive` runs the other three. The
two sets are equal -- no orphan either way.

**The skip counts are the thing to read carefully, and they are
honest.** `check-ssh` reports "3 passed, 20 skipped" for its last phase
and `check-keepalive` "8 passed, 15 skipped", which looks alarming and
is the harness working: each phase runs the whole binary and the tests
belonging to other phases say so rather than pretending. What would be
worth worrying about is a phase nobody runs, which is what the
enumeration above exists to answer.

### 12.2f The live-only sabotage is provable by the runner — **[DONE 2026-09-07]**

`sabotage.py` took `make test` as the suite and nothing else, so
`ssh-send-window.toml` — whose test QSKIPs without a server — reported
MISSED for ever, and its own comment told the reader to reproduce it by
hand. A guard covered by a test the guard-checking tool cannot run is a
guard checked by nobody.

`BSSH_SABOTAGE_SUITE` names the command instead, and the runner prints
which one it used, because a verdict means something different under a
different suite and a report that does not say which cannot be re-taken:

    BSSH_SABOTAGE_SUITE="make check-ssh" tool/sabotage.py \
            tool/sabotage/ssh-send-window.toml

**Run 2026-09-07: baseline 29 passing, and the edit CAUGHT with exactly
one failure** — `a_send_to_a_remote_that_is_not_reading_still_lets_go`
and nothing else. Tree byte-identical afterwards, journal empty. So the
MISSED this spec used to report was the runner saying the covering test
did not run, exactly as its comment claimed, and that claim is now
checked rather than asserted.

**The trap the option carries, stated in the source.** The command must
BUILD as well as run, or the edit is never compiled in and every verdict
describes the previous binary — a green suite over code that was never
sabotaged, which is the vacuous pass arriving through the tool built to
prevent it. `make test` and `make check-ssh` both build; a bare
`./tool/check-ssh.sh` does not.

`--check` still names this spec among the four whose `expect` can QSKIP,
which is correct: under the DEFAULT suite it still cannot run, and the
line is about what a reader of a MISSED verdict should suspect.

### 12.2g The full sweep: 165 edits, and the one miss was the harness — **[2026-09-07]**

Every spec run end to end except `ssh-send-window`, which was proved
under `make check-ssh` the same day. **54 specs, 165 edits, 2h42m from a
541-passing baseline: 164 caught, 1 MISSED — and re-aimed, 165 of 165.**
Tree byte-identical afterwards, journal empty.

**The estimate given beforehand was 8 to 16 hours and was wrong by a
factor of three.** It came from timing a suite run while a container was
building: 159s idle and 343s after touching one source file, quoted
without saying they were contended. The real cost is about 55s an edit.
A number that decides whether to commit a machine overnight is worth
re-measuring on a quiet machine before it is passed to anybody, and this
one was passed to the copyright holder and to a peer before it was.

**The single MISSED was a sabotage that could not fail, not a missing
test** — and it is worth reading closely, because it reported as the
opposite. `a pane moved off the end wraps around instead of stopping`
replaced the range guard

    if (to < 0 || to >= intent.size()) return;

with a SIZE check, `if (intent.size() < 2) return;`, which leaves `to`
out of range rather than wrapping. With two panes the edit then reached
`QList::move(0, -1)` and `move(1, 2)`. `QList` asserts both indices and
the suite builds with `QT_NO_DEBUG`, so the asserts are compiled out and
the result is UNDEFINED rather than wrapped — it happened to leave the
list unchanged, the test's `QCOMPARE` against `before` passed, and the
runner called it a hole in the tests.

`a_pane_can_be_moved_along_the_arrangement` covers the no-wrap case
exactly and its fixture reaches it. Re-aimed at modulo arithmetic — what
a hurried fix actually reaches for, since it makes the operation total —
and re-run: **caught, with exactly one failure, that test and nothing
else.**

**So: a sabotage has to produce the DEFECT its name claims, not merely
break the guard that prevents it.** Breaking the guard is the easy half
and it is what a first draft writes. §12.3 already records two `replace`
halves that were wrong, and `--check` cannot see this one either, by
construction: the anchor matched exactly once and the `expect` named a
real test. Only running it says anything, which is the whole argument for
running them.

**What 164 confirmations are worth, stated because a large green number
invites the wrong reading.** They do not say the code is correct. Each
says one named test really does defend one named behaviour — that the
test would go red if somebody removed the guard. That is a property of
the SUITE rather than of the program, and it is the property this tree
keeps finding it did not have.

### 12.3a Backspace across a wrapped line is the SHELL's job — **[2026-09-07]**

Reported from the phone: holding Backspace deleted back to the start of
the wrapped row and stopped, *"unless the terminal is weird"*. It is not
weird, and it is not ours — but the answer was only worth having because
it was measured rather than reasoned about.

**What a shell emits, measured with a plain pty at 40 columns**, 60
characters typed and 55 deleted, no beerssh involved:

    bash    53 BS, 3 CR, 2 cursor-up      redraws across the wrap
    dash   110 BS,  no CR, no cursor-up   never moves up

A line editor tracks the wrap and moves the cursor; a shell in the
kernel's canonical mode emits nothing but `\b \b`, and a backspace at
column 0 cannot climb to the row above on ANY terminal, xterm included.
The characters are still deleted from the buffer — only the display stops
reflecting it.

**The first measurement was wrong and nearly settled it the other way.**
It sent 30 deletions, and with a 10-character prompt 60 characters at 40
columns puts exactly 30 on the second row: it measured up to the wrap
boundary and never across it, reporting zero cursor-ups for bash. 55
deletions is what showed bash redrawing. A fixture that stops exactly at
the boundary it is testing looks like a result.

**The half that IS ours is asserted**:
`pty_session_test::backspace_crosses_a_wrapped_line` drives a real bash
through `bssh_emulator` at 40 columns and counts what is left on screen —
five or fewer of sixty after deleting 55. It passes, so the emulator
follows bash's redraw.

**Seen to fail, with a control that is a different shell rather than a
broken emulator**: the same test pointed at dash fails on the ERASE
assertion — reaching the typing stage first, so it fails through the
check under test rather than being caught upstream by the prompt wait.
That is what makes the pass evidence, and it pins the finding: the
difference is the shell's, and no change here can make dash behave.

**The control was re-run after the test was rewritten**, because a
control taken against an instrument since replaced has stopped being
evidence about it.

**Seven instrument bugs in one small test, and the code under test never
changed.** Worth listing, because each would have been "fixed" by
weakening the assertion, and because they share one shape: an unstated
dependency on the environment the test happened to run in.

The seventh is the one worth reading twice. A control rebuilt with plain
`make` -- which does NOT build the test binary, `test:` being its own
target -- ran a STALE binary and passed, "disproving" a correct
diagnosis. That retraction was published before it was checked. **The
first six made a correct thing look broken; that one made a wrong
conclusion look verified**, which is worse, because a false negative
ends an investigation instead of prolonging it. `build-and-commit.md`
names it exactly: never conclude a test passes or fails from a binary
the build step did not rebuild.

And the cause it nearly buried: **`TERM`**. Measured in the CI image
rather than inferred -- the same bash 5.2.21 with TERM unset emits 104
backspaces of `\b \b` and zero cursor-up, where this machine emits
`\b ESC[K` and two cursor-ups. Readline runs in both; with no terminfo
it has no cursor movement available. The test sets `TERM=xterm` now, and
with that line removed and TERM cleared it fails on the container's exact
assertion.

The other six:

- **A fixed `qWait`.** Passed here, failed in the container at *only 25
  of 60 characters arrived*. A sleep turns a slower machine into a false
  verdict about the code.
- **A missing precondition.** Replacing the sleeps with condition waits
  also removed the wait for the PROMPT, so the characters were typed
  before bash reached readline, the kernel's canonical mode echoed them,
  and the test reproduced the very failure it exists to rule out.
- **A marker a terminal shows twice.** Waiting for `echo bssh-up` matched
  the ECHO of the command as it was typed, which happens before the
  command runs — so the wait returned while the shell was still busy.
  The fix is shell quoting: `PS1="RE""ADY> "` types as `RE""ADY>` and
  draws as `READY>`, so the string waited for can only be the prompt.

The last one also closed a vacuous-pass route: a bash that came up
non-interactive prints no prompt, so the wait expires and the test fails
rather than measuring the line discipline and calling it a line editor.

### 12.3b The phone floor is 320dp, and three surfaces did not fit it — **[FIXED 2026-09-07]**

**`BSSH_PHONE_DP` was 360 and the device is 320.** A Galaxy Z Fold 3's
cover screen is 832x2268 at 420dpi — 320dp — and folded is how it is
carried. So every "fits a phone" assertion in the suite, and every
picture `make shots` renders, described a screen 40dp wider than one this
app is used on daily. Not a hypothetical population: `evidence.md`'s *a
set of tabulated extremes is not a worst case*, met on hardware.

At 320, three of seven surfaces overflowed. **Neither cause was the one
guessed at**, and both guesses were confident:

    host editor      320px of 306      widest child: bssh_font_picker 288
    preset editor    288px of 284      widest child: bssh_font_picker 288
    settings         313px of 306      widest child: a checkbox at 241

- **The font picker's colour strip was a fixed `16 * 18` pixmap**, and a
  QLabel demands its pixmap's width — so one decorative strip set the
  floor for the picker and for both editors embedding it, at 288 to the
  pixel. It draws to the width it is GIVEN now, eight columns of narrower
  cells rather than fewer colours, with `QSizePolicy::Ignored` so the
  pixmap cannot argue with the layout about a number it invented. 288 to
  222, and both editors fit.
- **Every shortcut's description IS its form row's label**, and
  QFormLayout does not wrap labels — so the settings page's floor was its
  longest description beside a QKeySequenceEdit. `WrapLongRows` rather
  than shorter text: the description is the only thing on the row saying
  what the chord does, and trimming it to reach a number is deforming the
  thing being measured.

**The first two guesses were the combo boxes, twice, and they were 166
and 134.** Only instrumenting the pages — every visible child by minimum
width, with its text and word-wrap flag — found either cause. Recorded
because the guesses were the plausible ones.

**And a latent defect fell out of it that has nothing to do with width.**
Adding a null guard to the picker's new `resizeEvent` prompted a look at
the constructor, where only `m_form` of seven pointer members was
initialised. The guard would have read uninitialised memory and passed a
garbage pointer into a dereference. All seven are initialised now.

### 12.3d The fix fitted here and not in the container — **[2026-09-07]**

The local suite was green over all 20 classes and `check-ci` refused the
push:

    host editor    317px of 306    picker asking 285
    preset editor  302px of 284    picker asking 302

**The same picker is 222 here and 285 to 302 there.** Its minimum is a
label column plus a combo whose items are FONT FAMILY NAMES, so it is a
function of the UI font's metrics, and the container draws in a different
face from this desktop.

So the first fix was not wrong, it was FRAGILE: 288 taken to 222 against a
306 budget, called fixed, when the quantity reduced varies by about a
third across machines. **A two-column form that only just fits is one
that stops fitting on somebody else's fonts** — and a margin is not a fix
when the thing measured is machine-dependent.

`WrapLongRows` on the picker's own form is the answer for the same reason
it was on the settings page, and a better one: it makes a row's minimum
`max(label, field)` rather than their sum, which REMOVES the font-metric
dependency instead of leaving it more headroom. At 320 with this
machine's fonts the picker still draws two columns, because the policy
wraps only what does not fit; the container gets the wrapped form.

**This is what the container gate is for, stated plainly because the
local suite agreed with the mistake.** sec 12.4 justifies check-ci by Qt
and libssh version skew; this is a third kind — the same Qt, the same
code, different installed fonts — and it is the kind no version check
would predict. The thing that disagreed was right.

### 12.3u A crashed suite was reported as a green one — **[FIXED 2026-09-22]**

`tool/sabotage.py` read only the PASS, SKIP and FAIL lines the suite
prints. A run that dies partway has already printed hundreds of PASS
lines and no FAIL line, so `ran` was true, `failing` was empty, and the
verdict was **MISSED: the suite stayed green** — the harness telling
you a guard does not exist while the guard was killing the process.

Found on §8.7's arrangement. Deleting `m_layout->set_arrangement(wanted)`
leaves `arrangement_changed` permanently true, so every settings pass
rebuilds the panes, re-enters `apply_device_settings` and runs the stack
out; `layout_test` took a SIGSEGV inside
`a_desktop_can_be_asked_for_the_column`, which is the exact test the
spec named, and the report said nothing had noticed.

**The direction of the error is what makes it expensive.** A MISSED
verdict sends the next reader to write a test that already exists, and
it arrives with a number and a name attached, which is the shape
`evidence.md` says nobody re-derives. The harness's own docstring
already warned about this class for a build failure — neither PASS nor
FAIL — and the guard it grew for that case, `passes + failing > 0`, is
defeated by a crash, because the passes are real.

**`make`'s exit status is the only place the crash is reported, so the
status is what had to be read.** `run_suite` returns it now, and the
verdict tells three things apart that all used to be "green": a test
failed (caught), nothing failed and the suite exited 0 (MISSED), and
nothing failed and the suite exited badly (**NO VERDICT**, which is
neither a covered case nor a gap but a run to go and look at). A
deadline kill gets the same verdict by a sentinel code rather than being
called a broken build, which is what it was reported as before.

The control is the case that found it: the crashing edit reported MISSED
before the change and NO VERDICT after it, same edit, same suite. The
spec then moved to an edit that breaks the feature without breaking the
process — holding `arrangement_changed` false, so the program runs and
does nothing — and that one is caught.

### 12.3t The settings page became an index and five pages — **[2026-09-10]**

It was the tallest surface in the app: **1929px at 320dp**, three screens
of a 640px phone.

**Measured before anything was designed**, because the obvious fix is the
wrong one:

    controls  912px   45%
    labels    532px   26%
    notes     510px   25%
    other      96px

Nearly half is the settings themselves, which cannot shrink without
removing features, and a quarter is the explanatory notes -- which
sec 12.3b says explicitly must not be trimmed to reach a number, since
that deforms the thing being measured. So shortening the prose could
never have been the answer, and the measurement is what says so rather
than taste.

**sec 8.5 rules out the desktop's answer by name**: no category tree,
"a desktop idiom that survives neither a thumb nor a 5-inch screen". What
it does not rule out is full-screen pages in sequence, which is what the
platform's own settings do -- and `show_page()`/`go_back()` already did
it for the host editor and the licences, so the navigation is not a new
concept, only a new user of one.

    Keyboard    483px  fits        Passwords   141px  fits
    Appearance  687px  scrolls     Shortcuts   633px  fits
    Terminal    215px  fits

**Four of the five need no scrolling at all.**

**The desktop is untouched, and that is structural rather than careful.**
The same view is the "This device" category inside sec 8.5's PuTTY tree
(`session_dialog`), so it builds `EVERYTHING` -- one level of navigation
is already above it and a second would be a tree inside a tree. Both
modes ship, which is what keeps the whole-page path honest: it is not a
legacy branch nobody exercises.

**Two measurement errors on the way, both mine.**

The first heights read `content->height()` after a resize, and three
sections came back at exactly 592px. They were the VIEWPORT: the scroll
area is `widgetResizable`, so anything shorter than the screen is
stretched back up to fill it and `height()` can never report less. Three
identical numbers is what made it obvious. `sizeHint()` is the honest
reading, and it also moved the whole page from 2038 to 1929.

The second was reading a sabotage's result through `head -3`, where the
warnings ate the window and the FAIL line never appeared -- so a caught
sabotage looked uncaught. *Never reduce a check's output before you know
it passed*, met in miniature while applying it.

**What guards the split is not the height.** A row whose section nobody
assigned would simply stop existing on touch, and every other test would
stay green because the desktop still builds the whole page. So the test
counts controls: the sections must carry exactly what `EVERYTHING` does.
Seen to fail by removing one row's guard -- *"the sections carry 32
controls and the whole page has 28"*, which is the unguarded row landing
on all five pages at once.

The height is asserted as a RELATIONSHIP -- every section at most half
the whole page -- rather than against a pixel bound, which would be a
constant to maintain and, worse, could be satisfied by shrinking the
content the entry above refuses to shrink.

**And the gallery renders the sections now**, not the whole page. `make
shots` was drawing a surface touch no longer displays, which is this
tree's own rule about pictures of things the app never does.

**The split was the holder's pick of three** (2026-09-10): "Nested
sub-pages". Rejected: "Collapsible notes", which saves the ~510px of
notes, about a quarter of the page, but makes the explanations
undiscoverable; and "Tighten spacing only", worth 100-200px against a
1929px page.

### 12.3s Cancelling a password prompt said "reconnecting" — **[FIXED 2026-09-10]**

Found in real use on the phone: cancelling the password dialog for
`funk@vibes.se` left the status line reading *"funk@vibes.se
disconnected -- reconnecting..."*.

**`note_user_disconnect()` had no caller anywhere in `src/`.** It
existed, it was correct, and the flag it sets was reachable only from a
test that called it directly -- `evidence.md`'s *an interface is only as
wired as its least-used method*, where a method with no caller hides
better than a signal with no listener because a complete-looking
interface looks done.

**And the suite was green about it.** `a_user_disconnect_is_not_retried`
calls `manager.note_user_disconnect()` itself and then asserts the
manager stays quiet: it proves the flag works when set and says nothing
about whether anything sets it. That is the case `evidence.md` states as
*a test that calls the helper it is verifying cannot see a wrong
caller*, found here in the wild rather than in the abstract.

**Two corrections to my own reading, both worth keeping.**

The symptom is not what it was first reported as. `bssh_session::
reconnect()` guards on the SESSION's own flag, so the attempts the
manager schedules all no-op: there is no second password prompt. What is
wrong is a status line claiming a reconnect that will never happen, and
a backoff timer cycling through its ten attempts for nothing. Saying it
would re-prompt was inference stated as fact.

The first reproduction PASSED and nearly closed the investigation. It
built a `bssh_session` with no transport, so `disconnect_from_host()`
changed no state, `state_changed` never fired, and
`on_session_state_changed` -- the code under test -- never ran. A
vacuous pass, written while hunting one. Only with a
`bssh_fake_ssh_session` injected does it fail, and that failure is the
defect on the production path.

**The fix wires the method that was already there.** The session gains a
`user_disconnected()` signal, emitted from `disconnect_from_host()`
BEFORE the transport is told -- the manager decides from the state
change that follows, so being told afterwards would be one scheduled
attempt too late -- and the manager connects it to
`note_user_disconnect()`.

**The control that matters is not the new test.**
`a_dropped_connection_is_still_retried` has to stay green: a fix that
simply stopped reconnecting would satisfy the new assertion and quietly
remove the feature sec 5.3 exists for. Both pass.

### 12.3r A generated file newer than its own header — **[FIXED 2026-09-10]**

Two `moc_*.cpp` in `build/` and two in `build-tests/` had been generated
back when their headers lived under `src/ui/` and `src/keyboard/`. They
sat there for six days and no build noticed.

**Timestamps are why, and the mechanism is worth keeping because nothing
about it looks wrong.** `6a03c7c`, "keyboard: move the keyboard to its
own directory", was a RENAME, so `keyboard/key_row.h` kept the mtime it
had at its old path -- 2026-08-07. The moc generated from that old path
is dated 2026-09-04. **The generated file is NEWER than its own header**,
so make compared the two and correctly concluded there was nothing to
do.

**It stays invisible until an object file goes missing.** The stale
`.cpp` is only compiled when its `.o` is absent, so the tree builds
happily for as long as nobody cleans. Here it surfaced because an
out-of-memory kill left a rebuild to redo the objects, and the error
then blames a missing header and names neither the move nor the fix:

    moc_key_row.cpp:9:10: fatal error: ../src/ui/key_row.h:
    No such file or directory

It cost three builds to diagnose, and the second instance only appeared
after the first was fixed -- one per rebuild, because each stale file
stops the build before the next is reached.

**`make check-build-tree` reads what the file INCLUDES**, because
timestamps provably cannot see this: make compares mtimes and never
content. One grep per generated source, 86 of them in this tree, no
false positives. It runs BEFORE the build as a prerequisite of `test`,
which is a detail that had to be corrected -- prerequisites are made
left to right, so listing it last let a full compile happen and then
reported the problem.

**Seen to fail**, by planting a `moc_planted.cpp` including a header
that never existed: both `make check-build-tree` and `make test` refuse,
and the message names the two files to remove.

**It reports how many files it read**, because a gate over an empty
list reports success as loudly as a real one, and the empty case is
reachable: a clone that has never been built has no moc anywhere.

**The count corrected a guess of mine within one run, which is the
argument for it.** The first container run was written up here as having
inspected nothing -- reasoned from check-ci building out of
`git archive HEAD` -- and that was asserted without being read. With the
count printed, the container says **31**: it builds the application
before running the suite, and that populates `build/` first. The
reasoning was sound and the fact was not, which is the difference a
number makes.

**What it does not cover**: a generated file whose header still exists
but whose CONTENT is stale. Nothing here can see that, and the honest
scope of this check is the header-moved case that produced it.

### 12.3q Shipping a terminfo for local shells — **[BUILT 2026-09-10]**

sec 12.3p left the question open and the copyright holder settled it:
ship one.

**The concern raised before building it, because it changes what the
change is worth.** Android's `/bin/sh` is mksh, whose line editor does
not link ncurses and does not read terminfo -- it emits `\b` and spaces
directly and takes its width from `TIOCGWINSZ`. So this is unlikely to
change the backspace behaviour that prompted it. What it does fix is
every terminfo-consuming program run inside that shell, which today
sees `TERM=xterm-256color` naming an entry the device cannot look up.
**Verified on hardware 2026-09-10, and the concern was right.**

The mechanism works. The database unpacks to
`files/terminfo/x/xterm-256color` at 4071 bytes, the same size as
shipped, and the shell's own `/proc/<pid>/environ` carries

    TERM=xterm-256color
    TERMINFO=/data/user/0/se.vibes.beerssh/files/terminfo

**It changes nothing about the local shell's line editing**, which is
what prompted it. Typing 46 characters at the prompt shows a WINDOW of
the input -- the first 13 scrolled off the left, a literal `<` at the
right edge -- rather than a line wrapped across two rows. That is mksh
scrolling one line, not a terminal failing to wrap, and it is what the
copyright holder's original report described as backspace "only
deleting until the start of that line": there is no second row, only a
viewport. Deleting all 46 leaves the line clean.

So the benefit is what was predicted and no more: every OTHER
terminfo-consuming program in that shell can now look up the TERM the
app sends. In a sandbox with almost no userland that is a small
population, and the entry says so rather than claiming the feature
fixed the symptom that motivated it.

**And `evidence.md` had already named this trap**: a stray character at
a terminal's edge that survived five sound hypotheses about the
emulator across two sessions, where the answer was in the bytes and the
shell had written a literal horizontal-scroll marker. The same `<`, the
same terminal, found again from the other direction.

**One instrument error worth keeping.** Reading the value with
`adb shell input text 'echo TI=[$TERMINFO]'` reports it EMPTY, and that
is not the app: adb hands the string to the device's own shell, which
expands `$TERMINFO` in ITS environment before `input` types the result.
The measurement was of the adb session, whose `TERM=screen` is this
desktop's value propagated over the wire. `/proc/<pid>/environ` of the
child is the reading that answers the question asked.

**`xterm-256color` and nothing else, which adds no claim.** It is
already what `BSSH_TERM` sends and what the default preset carries, so
shipping the matching entry lets the device look up a claim this app
makes everywhere else. A DIFFERENT entry would be a new statement about
what the terminal can do, and sec 7.1 owns that question.

**Generated, not committed.** `make terminfo` runs `infocmp` then `tic`,
which is the same trade `fonts/` makes: somebody else's data with
somebody else's terms, so the tree carries the command rather than the
file. A machine without ncurses skips it and the app behaves exactly as
it did before -- which is what an empty `terminfo_directory()` means.

**A REAL path, never a Qt virtual one**, and that is the whole reason
the Android side has code in it rather than one `return`.
`font_directory()` may answer `assets:/fonts` because only Qt reads it;
this is read by ncurses through `open(2)`, which knows nothing about
Qt's file engine. So the Android implementation unpacks the database out
of the APK once, skipping files already there at the same size -- which
still refreshes when an update ships a different one, where a bare
existence check would not.

**The desktop deliberately answers empty.** It has `/usr/share/terminfo`
with thousands of entries; pointing `TERMINFO` at one entry of ours
would shadow the real database for exactly one name and leave every
other name worse off.

**What is guarded and what is not**, because the two halves are not
equal. The child's environment is tested end to end -- a real shell,
`printf` reading `$TERMINFO` back -- with the empty case as the control,
and the sabotage sets the wrong variable name and is caught. The
FACTORY half, `create_local()` asking the platform, ~~cannot be tested on
a desktop at all~~ **is guarded since 2026-09-22.** The claim was that
the desktop platform returns empty by design, so a factory that asks
correctly and one that never asks produce the same answer, and that
"closing that needs a platform test double, which this tree does not
have".

**It needed one ANSWER forced rather than a whole double.**
`bssh_platform::force_terminfo_directory()` is the seam
`force_touch_primary()` has been since §6.2, and with it a test watches
the factory carry a directory the platform was told to give. **An
impossibility claim is worth re-reading whenever the fixture grows**,
and the differences behind one are usually smaller than the label -- a
lesson this section already carried and had not applied to itself.

**And a NULL forced value means "nothing is forcing this" while an EMPTY
one is the desktop's real answer**, so the hook tests `isNull`. That
distinction has no sabotage, and the reason is the original claim one
level in: swap it for `isEmpty` and the call falls through to the
platform, which on a DESKTOP also answers empty. Written, run, MISSED,
and recorded in `terminfo.toml` rather than shipped -- it is observable
only where the platform's answer is not empty, which is Android.

**Known gap, left deliberately (2026-09-10):** an asset that is shipped
but cannot be read makes `terminfo_directory()` answer empty, which is
indistinguishable from "nothing shipped". It is not reported because the
tree has no warning idiom for it, and inventing one is a convention
change rather than part of this.

**The holder's own answers on the original report, quoted because they
do not settle it.** Asked on 2026-09-08 for `echo $SHELL` on `vibes.se`,
the holder replied "it's bash" (08:30) and a minute later "it has no
value". At 15:21, after being asked for `ps -p $$ -o comm=` and
`echo "${TERM:-<unset>}"`, the reply was again "it has no value", and
the session never acknowledged it. The mksh explanation above is
measured on the phone's LOCAL shell; if the report was made against bash
on `vibes.se`, `backspace_crosses_a_wrapped_line` says bash redraws
across the wrap -- and an unset `TERM` on the remote (§12.3n measures
only that ours is sent) would be the next thing to check. `printenv
TERM` on `vibes.se` was still open when the session ended.

### 12.3p The stale glyph on the phone is not ours — **[2026-09-09]**

Found while testing key repeat on the device: eight characters typed at
a local shell, Backspace held until they were gone, and **one of them
left sitting in the last column** -- surviving the shell redrawing the
line and fresh output scrolling past below it.

It looks exactly like a rendering bug, and the first guess was the
renderer failing to repaint a damaged cell. **It is in the model.**
Reproduced locally by feeding the emulator the canonical erase pattern,
with the two cases that do not reach the last column as the control:

    prompt 7 chars, 20 columns, then `\b \b` per character

    fill=11   last char at col 17   erased "READY>"                z left=0
    fill=12   last char at col 18   erased "READY>"                z left=0
    fill=13   last char at col 19   erased "READY>             z"  z left=1

**And it is correct.** The mechanism comes from the vendored library
rather than from reasoning about ECMA-48: libvterm's own
`t/20state_wrapping.test` pins that after a glyph is written into the
80th column the cursor still reports `0,79` -- the wrap is PENDING, held
in `at_phantom`, and the cursor has not left the last column. Its
`on_control` ends with `updatecursor(state, &oldpos, 1)`, so BS is an
explicit cursor move and cancels the flag, which is upstream's own
"phantom cancelled by explicit cursor move" case. BS therefore lands on
N-2, the space rewrites N-2, and nothing in the sequence can reach N-1
again. **`\b \b` cannot erase the last column on any terminal that
follows this model, xterm included.**

Pinned as a test rather than fixed, because the obvious fix breaks
conformance and the artifact is convincing enough that somebody will try
it.

**What DOES vary is whether a shell uses that pattern.** sec 12.3's pty
test already records the same bash emitting `\b ESC[K` with `TERM=xterm`
and falling back to the canonical `\b \b` with TERM unset --
clear-to-end-of-line takes the last column with it. Android ships no
terminfo database, so the local shell there gets the fallback.

**So there is a real question, and it is the holder's**: whether a local
shell on Android should be given a terminfo the app ships. It is a
convenience feature rather than the app's purpose, the cost is shipping
and pointing `TERMINFO` at a database, and the benefit is a line editor
that behaves the way it does everywhere else. Recorded rather than
decided.

**And it is NOT the vibes.se report.** That one is a remote shell with
its own terminfo, reached over SSH with the preset's TERM -- which
sec 12.3n establishes we send correctly. The two share a shape and not a
cause, and `echo "${TERM:-<unset>}"` on that host is still what
separates them. Writing them up as one explanation would have closed a
question that is still open.

### 12.3o The untestable half was testable — **[FIXED 2026-09-09]**

`a_held_key_repeats` carried a long comment headed *WHAT THIS TEST
CANNOT REACH*, and it was wrong. That is the class `evidence.md` says to
sweep for in preference to gap language: **an impossibility claim that
was never true tells the next person not to write a test that would
work**, and this one had already sent the question to the copyright
holder's thumb as one of two things "still yours".

The claim was that a press delivered at the scroll area's viewport
produced zero bytes, tap included, so QTest's synthetic press must not
survive QScroller's filter and the fixture was measuring itself. It does
not reproduce. Sending a `QMouseEvent` to the viewport, in a fixture
doing nothing else:

    down_after_press = 0     down_during_hold = 1
    held 900ms       = 12 bytes, four left-arrows
    tap, instant     = 0 bytes
    tap, 80ms        = 3 bytes, exactly one

**Two things had to be got right at once, which is why the earlier
attempt read as a broken instrument.** The press must go to the
VIEWPORT, because an event filter runs on its receiver and a press sent
to the button is not filtered at all -- so the old test proved the
button repeats and said nothing about a finger reaching it. And the tap
must take HUMAN time: the scroller withholds a press while it decides
whether the finger is dragging, and that decision runs on a timer, so a
press and release with no turn of the event loop between them is
released before anything is decided and reaches no button. An
instantaneous tap is a gesture nobody can perform.

**Zero bytes is also what a dead fixture produces, and that is what
made the wrong reading reasonable.** What separates them is the held
case working while the instant tap does not: a fixture that had stopped
delivering would have zeroed both. The earlier run had no case that
could tell the two apart.

So the scroller does not swallow a held press -- it delays it and
replays it, and auto-repeat survives that. The test now presses where a
thumb presses, and asserts `isDown()` before measuring anything, so a
press that really were swallowed fails loudly instead of quietly
measuring nothing.

**The press was only half of it, and the half that was wrong shipped.**
The first version of this test sent the release to the viewport with
`Qt::NoButton` named as the button that changed. Both are wrong and each
alone looks like the other one failing, so the single-variable attempts
each read as the theory collapsing. Bytes arriving in the 300ms AFTER
the release:

    viewport, NoButton     18     runaway
    button,   NoButton     18     runaway
    viewport, LeftButton   18     runaway
    button,   LeftButton    0     stops

The release goes to the BUTTON because it holds the mouse grab once the
scroller replays the press, and it must name `LeftButton` as the button
that CHANGED because `QAbstractButton` ignores any other release. A
release the button never sees leaves the key down repeating for ever --
and the tap assertion then reads those repeats rather than a tap, which
is green often enough that it **passed a container run and was pushed**.

**The arm that catches it is the quiet after the release**, which the
pushed version did not have. That is `evidence.md`'s rule about a
control having to fail the way the thing it controls for fails: the tap
count could be right for the wrong reason, and only asserting that
nothing more arrives can tell the two apart.

**And the touch path was measured too, on the device, the same day.**
The desktop test exercises the mouse events Qt synthesises rather than a
`QTouchEvent` arriving from Android, so it was written up as the
remaining gap. `adb shell input swipe X Y X Y 1500` closes it: that is a
real MotionEvent through Android's input stack. On a Galaxy Z Fold 3
cover screen -- 840px at 420dpi, which is 320dp, the device this floor
is named for -- with `abcdefgh` typed at a local shell, one hold of the
key row strip's left arrow walked the cursor all eight cells to column
zero. The built-in keyboard answers the same way: eight characters, one
hold of Backspace, all eight gone.

**But the device could not have shown a runaway, and saying it closed
the question was too strong.** Both device cases are clamped at a
boundary -- the cursor stops at column zero, backspace stops at the
start of the line -- so a key that never stopped repeating would look
exactly like one that stopped on time. The device settles that repeat
STARTS under real touch, which was the open question; that it STOPS is
the desktop test's half, and that is the half that had been broken.

### 12.3n Half the TERM question is ours, and it is sound — **[2026-09-09]**

The report behind sec 12.3's wrap investigation left two one-line
questions for the copyright holder, and one of them is only half theirs:
if `TERM` is unset on `vibes.se` it explains the backspace behaviour, but
it would ALSO mean the preset's `xterm` never reached the remote, and
that half is our code and answerable here.

**Traced end to end, by reading every link rather than the two ends:**

    preset.term
      -> to_json()["term"]                       preset.cpp:28
      -> resolver copies every key it finds      settings_resolver.cpp:19-21
      -> session's m_settings["term"]
      -> m_transport->set_term(...)              session.cpp:66 and :170
      -> m_term                                  ssh_session_libssh.cpp:1146
      -> ssh_channel_request_pty_size(m_term)    ssh_session_libssh.cpp:1083

Nothing renames the key: the resolver iterates `preset.to_json()` and
copies whatever it holds, so a mismatch is not available by
construction. **Which matters because a mismatch here is silent** -- a
missing key yields an empty QString and the backend maps empty to
`BSSH_TERM`, so the remote would get the compiled-in default while the
preset said otherwise, and nothing anywhere would say so.

**Both call sites are needed and both are load-bearing.** Settings
resolve before a transport exists, so `set_settings()`' transport block
is skipped on a first connection and `:170` is the only one that runs --
which is the connection that matters.

**And the guard was half there.** `host-edits.toml` pinned the EMULATOR's
copy of the resolved TERM and nothing pinned the transport's. That is
exactly the shape `evidence.md` cites for this test by name: a test
called `every_x_reaches_something_that_uses_it` asserts exhaustiveness in
its name and enumerates whatever its author listed, and TERM's two
consumers -- the capability mirror and the channel -- were the original
incident, where the working one was the one checked.

Measured rather than assumed: deleting `:170` **is** caught by that test
today, so the enumeration is currently honest. It is now pinned by a
spec of its own, because "currently honest" is what the emulator entry's
own comment says a sabotage cannot rely on -- an anchor can go on
matching while the fix moves out from under it.

**So the answer to the holder is narrower than it was.** Whatever
`vibes.se` reports for `TERM`, it is not because this app failed to send
one. The remaining question is what the remote does with what it is
given.

### 12.3m A sixth sweep that found nothing: wrapped notes — **[2026-09-09]**

The lens after sec 12.3l, derived from `wrapped_note.h` existing at all:
**a helper written because a plain call gets something wrong, with a
caller that makes the plain call anyway.** One site in the tree reaches
`setWordWrap(true)` without going through it -- the licences view's
identity label, which carries the version and the copyright line
`harmonization.md` requires on that surface.

The comfortable story was ready: that label's text is 48 characters, the
render shows it ending flush with the edge at 320dp, and sec 12.3d has
already established that a widget's width is a function of the UI font
and that the container's are wider. A defect in waiting.

**The experiment that would have confirmed it refused it.** Squeezing the
view from 320dp to 160 so the copyright line must wrap to a third line
and a fourth:

    320    label 298 wide    h=34  need=34   ok
    260    label 238 wide    h=51  need=51   ok
    200    label 178 wide    h=51  need=51   ok
    160    label 138 wide    h=68  need=68   ok

The height tracks the need at every step. `wrapped_note` is scoped to a
note in a FORM -- QFormLayout asks how tall a label is, is told one line
because the size policy does not advertise the dependency, and prints
the next row through the rest of the sentence. This label is in a box
layout, which asks `heightForWidth` and honours it. The bypass is
correct, and the site now says so, because an unstated precondition and
an absent one look identical to the next sweep.

**And the wider measurement is worth keeping.** Every wrapped label on
all seven touch surfaces, at 320dp: 22 of them, none cut off.

    licences 1    colours 2    picker 2    prompt 1
    settings 11   hosts 2      presets 3

Twenty of the twenty-two sit at slack=0 -- height exactly equal to need,
which is the layout doing its job rather than a margin to worry about.

**No assertion was added for the six surfaces beyond the settings
page**, and that is sec 12.3i's rule rather than laziness: outside a
form the mechanism cannot bite, so six arms would be six that cannot
fail. The settings page keeps the one that can, and this entry records
that the others were swept, by what method, and what the code does
instead.

### 12.3l The same starved form on the narrowest page — **[FIXED 2026-09-08]**

sec 12.3j fixed the host editor's two-column form on a phone and left
the obvious question unasked: what else. The lens is the bug's own shape
-- **a QFormLayout on a touch surface with no row wrap policy** -- and it
is three greps:

    src/ui/host_editor.cpp        forms=1  wrap=1  touch-aware=1
    src/ui/preset_editor.cpp      forms=1  wrap=0  touch-aware=3   <--
    src/ui/session_dialog.cpp     forms=1  wrap=0  touch-aware=0
    src/ui/font_picker.cpp        forms=1  wrap=1  touch-aware=2
    src/ui/app_settings_view.cpp  forms=2  wrap=2  touch-aware=1

One candidate. `session_dialog.cpp` has no touch awareness at all and is
the desktop dialog, so a two-column form there is correct.

**Measured rather than predicted, because the first reading was that it
would be fine.** The preset editor's fields are mostly bounded -- spin
boxes, combos, checkboxes -- and its combos were already measured at
roughly double what they need in sec 12.3i. Instrumenting the form row
by row at 320dp said otherwise:

    label w=66  field x=72 w=212  needs= 23   "BSD"          Name
    label w=66  field x=72 w=212  needs= 34   "xterm"        TERM
    label w=66  field x=72 w=212  needs=230   tmux command   On connect

**212 against 230.** Three of its rows are free-form after all -- Name,
TERM and On connect -- and the last holds a shell command. An entirely
ordinary `tmux attach -t main || tmux new -s main` does not fit.

**And this is the narrowest page in the app**: its scroll area leaves
284px where the settings page and the host editor get 306, so the label
column's 72px costs proportionally more here than anywhere else. The
field a person is most likely to paste a long string into had the least
room of any in the tree.

The fix is the host editor's, guarded on the widget's own `m_touch`
rather than re-asking the platform, and the free-form assertion now
covers both editors: 5 fields on one, 3 on the other. The page grows 950
to 1111 tall, and the checkbox rows cost nothing -- Qt collapses an empty
label, so only the labelled rows stack.

**Two instances is what makes it a rule rather than a fix.** One page
with a starved form is a defect; two, found by asking the same question
of every form in the tree, says phone forms stack and the desktop's two
columns are the special case. The remaining two-column touch forms are
the font picker's and the settings page's, both `WrapLongRows`, both
holding fields that CAN state a width because their contents come from a
fixed table -- which is the distinction sec 12.3j drew and the reason
they are not the same case.

### 12.3k The phone width was written in four files under two names — **[FIXED 2026-09-08]**

The lens sec 12.3j suggests: **an instrument whose output is a function
of a constant nobody measured, checked against nothing.** Swept over
`test/` and `tool/` for numbers standing in for a value the code could
have asked for.

Most literal sizes in the suite are innocent -- a test picking a window
to work in is not claiming to represent a device. Five sites were the
other kind, all of them the string `360`:

    secret_store_test  no_setting_is_cut_off_at_a_phone_width   WEAKENED
    secret_store_test  const int phone = 360                    WEAKENED
    layout_test        two resize(360, 640) crossing a class    cosmetic
    platform_test      a_long_failure_does_not_widen_the_window cosmetic

**Two are genuinely weakened, and in the direction that hides
defects.** A word-wrapped label needs MORE height as the screen narrows,
so a test asserting that no setting is cut off "at a phone width" was
asking the easier question -- the gallery bug of sec 12.3j, in an
assertion instead of a picture. The other asserts the password prompt's
minimum fits, which 320 makes stricter, and its comment cited a section
that had moved.

**Measured at 320: nothing fails.** 543 passed, same as at 360. So there
is no defect behind this, and the entry is a record of a method rather
than of a bug -- the labels and the prompt are genuinely fine at the
device's width, which is worth knowing precisely because the old
assertion could not have told anyone.

**The structural cause is the part worth fixing.** sec 12.3b moved the
floor from 360 to 320 by hand, and reached two of six sites, because
every file carried its own copy -- and the two it did reach spelled it
differently from each other, `BSSH_PHONE_DP` against `BSSH_PHONE_W`, the
same 320 under two names.

`test/phone_metrics.h` holds it once now. **`touch_metrics.h` already
made this exact argument** about sec 6.2's touch floor, which had been a
static in seven files under four names: *the danger is not the
duplication -- it is that a rule written in seven places is one that can
be changed in six.* The phone width is that rule, and it has now been the
exception twice.

**The rename carried a proof rather than a reading.** Values asserted
identical before any substitution, then use counts compared across it:
15, 9, 14 and 14 before and after. A rename that moved every use and
invented none.

### 12.3j The gallery was drawn at a width nobody had measured — **[FIXED 2026-09-08]**

`make shots` sized every scrolling page at `BSSH_PHONE_W - 20`, and the
20 was a fudge for a scrollbar nobody had measured. It asks the scroll
area now:

    touch-host-editor-full.png     306 wide      was 300
    touch-preset-editor-full.png   284 wide      was 300
    touch-settings-full.png        306 wide      was 300

306 is the same number sec 12.3b's instrumentation reported from the
other side, walking the settings page's children -- two instruments, one
answer, which is what makes it a measurement rather than a second guess.

**Both directions of the error are here, and only one of them had been
noticed.** sec 12.3e records the narrow direction: at 300 the settings
page's Keyboard combo looked clipped, was reported as a device defect,
and is not one. The preset editor is the other direction and is worse.
Its viewport is 284, so the gallery drew it **sixteen pixels wider than
the device** -- and a control clipping between 284 and 300 there would
have been invisible in the one instrument that exists to show it. A
picture too narrow manufactures a defect and somebody checks it; a
picture too wide hides one and nobody does.

So the width is printed beside every picture now. A shot whose width is
in the log can be checked against the phone; one whose width is a
constant two files away is checked by nobody, which is how a six-pixel
fudge survived long enough to produce a wrong report.

**And the thing it had been hiding was not on the preset editor.** With
the pages laid out at their real widths, the host editor's ProxyJump
field shows `nabbe@bastion.internal.example.com:2222` in 218px of the
245 its text needs.

**sec 12.3i had already seen that and read it correctly**, which is why
this entry is underneath it rather than correcting it: a line edit
scrolls, no width fits text nobody chose, and asserting that it fits
would have been an assertion no layout can satisfy. **An assertion
nothing can satisfy is not a guard -- it is a finding waiting to be
waived, and the waiver is what switches a check off.**

What sec 12.3i did not ask is why the field was 218px wide. The form was
two columns on a phone: the label column took 88 of the 306 available,
and the fields got what was left. Nothing about that is forced. On touch
the host editor stacks now -- `QFormLayout::WrapAllRows` -- and every
field gets the whole row.

**WrapAllRows rather than the settings page's WrapLongRows, and the
difference is the same distinction one layer down.** WrapLongRows wraps
a row whose MINIMUM will not fit; a field willing to scroll its own text
has a small minimum, so it would never fire here. The settings page's
fields hold chords from a fixed table and can state what they need.
These cannot, so they are given the room unconditionally.

**Which gives the free-form fields an assertion that IS satisfiable**:
not that the field fits its text, but that its label sits above it and
the two share a left edge. That is a question the layout can answer, it
fails naming all five fields when the policy is reverted, and it has a
spec in `narrow-screen.toml` that says so.

**The bounded/free-form split is structural rather than a list of
names.** A spin box and a key-sequence editor each own a QLineEdit and
`findChildren` reaches it, so excluding those and combo boxes leaves
exactly the fields somebody types into. Applied to the settings page it
removed one control from the clipping arm -- the window-title format,
`%t - BeerSSH`, which was being measured and passed only because that
string is short.

**The clipping arm stays on the one page sec 12.3i scoped it to.** Its
argument still holds: the other surfaces' combos sit at roughly double
what they need, so an arm there could not fail, and a guard that cannot
fail on three of four pages reads as broader coverage than it has. The
extension was written, measured -- 4 controls on the host editor, 7 on
the preset editor, 15 on the settings page, nothing clipping -- and
taken back out. **A decision recorded yesterday is cheaper to read than
to re-derive**, and re-deriving it is what produced the measurement above
rather than a second opinion.

### 12.3i A fifth that found nothing: combos on the other surfaces — **[2026-09-08]**

§12.3e asserts that nothing on the settings page is drawn narrower than
its own text, and scopes that to one page. The obvious extension is
every touch surface, so it was MEASURED before an assertion was written
rather than after.

Combos only, deliberately: a `QLineEdit` scrolling long user text is
normal -- the host editor legitimately shows
`e@bastion.internal.example.com:2222` clipped from the left because the
cursor is at the end -- so asserting on line edits across every surface
would manufacture false positives. A truncated `QComboBox` is never
legitimate, because it cannot be scrolled and the setting simply reads
as the wrong word.

At 320dp, nothing clips:

    font picker     270 wide,  89 needed
    host editor     218 wide,  89 needed
    preset editor   212 wide,  89 needed;  116/55;  85/44

The narrowest is the preset editor's cursor-style combo at roughly double
what it needs, against the nine pixels the settings page had.

**The settings page is the outlier and the reason is structural**: its
combos sit in a form whose LABEL COLUMN is long -- shortcut descriptions
and setting names -- so the field gets what is left. The editors put
theirs in forms with short labels ("Font", "Colours"), so the field gets
most of the width. Same widget, same style, different neighbours.

So the assertion stays scoped to the page where it has been seen to
fire. **A guard that cannot fail on three of the four pages it covered
would read as broader coverage than it has**, which is the failure this
document keeps recording from the other direction.

### 12.3h A fourth sweep that found nothing: dangling session pointers — **[2026-09-08]**

Derived from §12.3g by inverting it. That defect is a value read before
it is valid; the mirror image is a value read after it stops being --
a raw `bssh_session *` held in a container while the session is
destroyed elsewhere. A multi-window terminal that hands sessions between
windows is where that would live.

**What would have matched:** a container of `bssh_session *` with no path
that removes an entry when the session closes. Six exist -- `m_shown`,
`m_tab_order`, `m_current` and `m_wired_sessions` in the window,
`m_intent` in the layout manager, `m_sessions` and `m_focused` in the
pane container.

**What the code does instead**, and it is a design rather than a set of
individual removals: `session_closing` clears the window's wired set and
calls `m_layout->forget()`, and the registry's `changed()` reaches
`on_registry_changed()`, which prunes `m_shown` against the registry by
`index_of` -- a pointer comparison, never a dereference -- while
`refresh_tab_bar()` REBUILDS `m_tab_order` from scratch each time rather
than editing it. `m_tab_order` has no removal path at all and needs
none, which is what made it look like a finding until the rebuild was
read.

That is §8.4's own rule showing up as a safety property: the tab bar is
rebuilt from the registry rather than edited alongside it, because two
lists kept in step by hand drift. Kept in step by hand they would also
dangle.

**Two places already use `QPointer` and both carry a comment naming the
bug that put it there** -- `session_registry.cpp`'s deferred emit and
`main_window.h`'s held session -- so the hazard is known where a raw
pointer genuinely had to survive a turn of the event loop.

So this family is swept and the next lens should be somewhere else.

### 12.3g A null guard is only as good as the value it reads — **[FIXED 2026-09-08]**

Found in `bssh_font_picker` by accident: a `resizeEvent` was added, it
needed a null guard because a resize can arrive while the constructor is
still building the widgets it touches, and only then did anyone look at
the constructor -- where **one of seven pointer members was initialised**.
The guard would have read uninitialised memory and passed a garbage
pointer into a dereference.

**Swept as a lens rather than left as an anecdote**, which is what found
the rest. Seven classes in `src/ui` and `keyboard/` have uninitialised
pointer members; three of them have an event override that READS one
before the constructor can have assigned it, and those three are the
defect:

    bssh_app_settings_view   17   resizeEvent -> a guard on
                                  !m_keyboard || !m_keyboard_fallback
    bssh_host_list            4   resizeEvent dereferences m_add,
                                  with no guard at all
    bssh_pane_container       1   showEvent -> update_on_screen() reads
                                  m_splitter

The settings page is the same defect as the picker's and predates it: a
guard written to make the resize path safe, reading memory that has not
been assigned. `bssh_host_list` is worse -- no guard, a straight
dereference to position the floating button.

**The first version of this entry stopped at three, and the reason it
gave was wrong.** It said the other four "override no event handlers, so
nothing reads them before assignment". True about event handlers and not
a characterisation of the set: **a `connect` made in a constructor is
the other way in**, and a signal can fire before the constructor
finishes. All four connect during construction.

Re-measured by the ordering that actually decides it -- is the first
`connect(` after the last `m_... = new`:

    host_editor       connect at 193, last assignment at 203   HAZARD
    session_dialog    connect at  25, last assignment at 121   HAZARD
    licences_view     connect at  81, last assignment at  68   safe
    preset_editor     connect at 261, last assignment at 246   safe

Both hazards are the same defect again. `session_dialog` connects
`bssh_profile_store::changed` to a lambda guarding on `m_sessions &&`
-- a guard on uninitialised memory, 96 lines before `m_sessions` is
assigned. `host_editor` connects the font picker's `font_chosen` and
`scheme_chosen` to `update_override_badge()` while `m_save` is still
ten lines from being assigned. Both are initialised now.

**`licences_view` and `preset_editor` stay untouched, and now for a
measured reason rather than the wrong one**: every member is assigned
before anything is connected, so nothing can reach one early. Editing
them for a defect that cannot occur is the sweep
`working-practice.md` warns about.

**The discriminator is not "overrides an event handler".** It is
"anything can reach a member before the constructor assigns it", and
event overrides are one instance of it. A narrower rule looked like a
characterisation because every case it had been checked against agreed
with it.

### 12.3f Today's guards gained sabotage, and three of them were wrong — **[2026-09-08]**

`--check` reported all 166 edits healthy while `src/ui/font_picker.cpp`
had **no spec at all**: the swatch-strip fix took three surfaces from
overflowing to fitting with nothing in the harness guarding it. That
check verifies the specs that EXIST; nothing asserts that new behaviour
acquired one. Six entries added — the key repeat on both boards, the
strip, the settings wrap policy, the chord editors — and all six caught.

**Getting the strip entry right took three attempts, each wrong in a
different way, and every one passed `--check`:**

- **Two entries, one guard with a spare.** The fix is two independent
  protections -- the strip draws to the width it is GIVEN, and the
  label's policy is `Ignored` so the pixmap cannot decide the form's
  width. Sabotaged separately each was MISSED, because a fixed pixmap is
  harmless while the policy ignores it and a `Preferred` policy is
  harmless while the pixmap follows the layout. Neither one's sabotage
  CAN fail.
- **One entry that was undone by the next line.** Replacing only the
  size-policy line left
  `m_swatches->setMinimumWidth(BSSH_SWATCH_COLUMNS)` immediately below
  it, overwriting the 288 just injected. It matched its anchor exactly
  once, compiled, ran, and did nothing: the picker measured 172px with
  the sabotage confirmed present in the built source. **A sabotage that
  lands and is then overwritten is indistinguishable from a test that
  cannot fail.**
- **Both adjacent lines in one `find`** -- caught, failing the two
  editors that embed the picker.

**None of the three is detectable by the anchor check**, which is the
limit §12.2d states and this demonstrates rather than asserts: each
matched exactly once and named a declared test. Together with the
pane-wrap entry that produced undefined behaviour instead of the defect
it named (§12.2g), that is four distinct ways a spec can be aimed at
something real and still be unable to fail.

**And twice today a conclusion was drawn from a build that did not
contain the change.** Once from `make` rather than `make test` -- plain
`make` does not build the test binary -- which "disproved" a correct
TERM diagnosis; once from an edit never confirmed to have survived to
the compiler, which produced a widget measurement from the wrong binary.
Both made a WRONG conclusion look verified, which is the expensive
direction: a false negative ends an investigation rather than prolonging
it. The habit that catches it is one line -- grep the marker out of the
source AFTER the build, not before.

### 12.3e Clipping has its own assertion, and one arm of it is unproved — **[2026-09-08]**

§12.3c records that a width assertion cannot see truncation: it asks
whether a MINIMUM fits, and a control willing to clip its own text has a
small one, so a clipping widget passes MORE easily than an honest one.
That was recorded and then left resting on a screenshot somebody happened
to look at. `no_control_is_narrower_than_the_text_it_shows` is the
assertion it should have had — every visible line edit and combo on the
settings page, laid out at 320dp, against the width its own text needs.

**The two arms are not equally proved, and the pass reads as though they
were.** The QLineEdit arm fires on demand: removing the chord editors'
minimum width names four of them, *"Shift+PgDown" in 67px of 80 needed*
among them. The QComboBox arm has never fired, and measuring says why
rather than leaving it to guesswork — with the combo fix disabled,
`BeerSSH's own, compact` is 161px wide needing 152. It FITS.

**Which corrects a claim made from the render.** The clipped
*"BeerSSH's own, compac"* in `make shots` was at that tool's page width
of `BSSH_PHONE_W - 20` = 300, six pixels narrower than the 306 a real
320dp screen leaves inside the scroll area. The chord editors clip at
306 and are a real device defect; the combo does not, and was reported
as one. **A picture rendered six pixels narrower than the device is
evidence about the picture.**

The fudge is gone -- sec 12.3j -- and the pictures now state the width
they were drawn at.

**`bssh_fit_items` stays, justified by a different measurement.** Nine
pixels of slack is not a margin worth trusting when the same font picker
measured 222px here and 285 to 302 in the CI container on identical
code: a form's width is a function of the UI font, so a margin that thin
is machine-dependent by construction. The fix is defensive and the
comment says so, rather than claiming a test proved it.

### 12.3c The gate went green while the page was unreadable — **[2026-09-07]**

Worth its own entry, because the fix above passed the assertion and was
still wrong, and only a PICTURE said so.

With `WrapLongRows` the settings page dropped 313 to 273 and the width
assertion passed. Rendered at 320 it read:

    trl+Shift+C          Ctrl+Shift+C, clipped from the left
    ft+PgDown            Shift+PgDown
    l+Alt+Right          Ctrl+Alt+Right
    BeerSSH's own, compac

The rows had not wrapped. The FIELDS had shrunk, and were truncating.

**The assertion asks whether a widget's MINIMUM fits, and a widget
willing to clip its own content has a small minimum — so a control that
truncates passes more easily than one that does not.** That is the gate
being satisfied by deforming what it measures, arrived at honestly and
one step at a time.

`make shots` caught it only because the renders had just been moved to
the same 320 floor as the assertions. At 360 every picture showed a page
that fitted. **A gallery rendered at the second-narrowest case cannot
find the defect it exists to find**, which is the argument for the two
numbers being one number.

The fix is fields stating what they actually need — the chord editors
sized to their sequence text, the combos to their longest item, both from
font metrics rather than a constant — and that is also what makes
`WrapLongRows` work: a field that admits it needs the room stops sharing
a row with its label, so Qt moves the label above rather than truncating
beside it. Verified in the render: "Open another session in a window"
wraps with a full-width editor beneath, "BeerSSH's own, compact" is whole,
and the desktop page still draws its two-column form untouched.

### 12.2d The anchor check is permanent now -- **[DONE 2026-09-07]**

The fifteen rotted anchors above were found by a script somebody had to
think of writing, on a day they happened to look. Nothing would have said
so otherwise: a sabotage whose anchor no longer matches is
indistinguishable from a check that cannot fail, and neither a build nor a
test run mentions it. The only standing alternative was to RUN the specs,
at one suite run per edit, which is exactly why nobody did.

`tool/sabotage.py --check` is that script made permanent, and `make style`
runs it -- so it runs on every push, because `check-ci.sh` runs `make
style` in the container against `git archive HEAD`. It reads and builds
nothing and takes about a second. Under `style` rather than `test` for the
same reason the style gate's own suite is there.

**What it establishes:** every spec parses and holds entries, every entry
has its four fields, every named file exists, every `find` matches exactly
once, and every `expect` names a test the suite declares. That last one is
new -- nothing before could catch an `expect` left behind by a renamed
test, and it is the same rot as an anchor with a different surface.

**What it does not**, stated in the tool at length because a cheap check's
green line gets quoted for the expensive one's guarantee: whether the edit
CHANGES anything a test can see (one anchor matched for months while the
call it deleted ran on a path no test reaches), whether the named test
covers the defect or even runs here, and whether the `replace` half is
right. Only running the spec answers those.

**The vendored tree nearly made it a gate that blocks every push.** Five
anchors live in text `vterm-patch/` ADDS, and `make style` runs before
`make` applies the patches -- so they are in `state.c` on a developer
machine and absent in the container, which starts from the submodule at
its gitlink commit. Measured: 1 and 0. Skipping them there would have been
the vacuous pass this check exists against, so an anchor absent from the
file must instead be supplied by exactly one contiguous run of added lines
in `<vendored>-patch/`. That is a real check in both states and strictly
stronger than reading the file, because a patch rewritten without updating
the spec goes to zero in both. The file wins where it has the text, or a
patched tree counts one line twice and reports a duplicate.

Runs are per contiguous block rather than one joined blob, which is the
only care the function needs: joining every added line would let an anchor
spanning two unrelated hunks match, and that is a false PASS.

**`Edit.check()` in run mode deliberately does NOT take that fallback**,
and the comment there says so, so nobody harmonises them later. `--check`
asks whether a spec is aimed at something; the runner is about to edit the
file on disk, and an anchor that exists only in a patch means the tree is
unpatched, where a sabotage would measure the wrong libvterm.

**Seen to fail before it was believed, on every arm.** A rotted anchor, an
anchor matching 180 times, a missing file, an `expect` naming no test, an
entry missing a field, a spec that will not parse, and an empty spec: each
exits 1 on its own. Two of them were run through `make style` end to end
so the GATE was watched going red rather than the tool -- `Error 1`, with
all 166 edits still counted, which is what says the run was not truncated.
The first attempt at that proved nothing and is worth recording: the
sabotage landed inside a `'''` block and broke the TOML, so the gate failed
through the parse arm while the anchor arm stayed inert -- `evidence.md`'s
*a control has to fail through the check you are testing*, met head on.

**Two further arms, added while their cells were EMPTY**, which is the
point rather than an admission that they found nothing. A `replace`
identical to its `find` applies cleanly and changes nothing, so its
verdict is MISSED for ever and reads as a hole in the code's tests; and
two entries breaking exactly the same `(file, find, replace)` spend two
suite runs to learn one fact, the second usually a copy somebody meant
to re-aim. There are none of either today. Asserting the partition now
is what makes one arriving later fail as a message to whoever added it,
rather than being absorbed by a condition that would have passed it.

**The no-op arm is EXACT equality and nothing cleverer, deliberately.**
`replace` CONTAINING `find` is the normal way to write a break -- six
entries prepend an `if(0)` or a guard and keep the original text -- so a
containment test would report six false findings on a clean corpus, and
a gate with false findings is one somebody switches off by instalments.
Measured before the arm was written, which is why it is not a
containment test.

**And the one thing that CAN be said in advance about the third failure
mode.** An `expect` naming a test that does not RUN reports MISSED, and
MISSED reads like a hole in the code's tests when it is a guard that did
not fire — the entry above calls that invisible until a spec is
executed. Half of it is not: whether a guard EXISTS is a property of the
source. `--check` now cross-references every `expect` against the test
methods whose body can `QSKIP`, and names the four that match:

    delete-both-halves     the credential is erased before the write that can fail
    remember-both-halves   a flag that could not be written leaves the secret behind
    settings-per-window    a settings page writes every field, not the one that changed
    ssh-send-window        do_send asks for more than the channel window

Four of 166. It cannot say a guard will fire and does not try; it says
one is there, which is enough to stop a MISSED from those four being
read as a finding without a second look.

**Measured on this machine, the practical count is ONE, not four**
(2026-09-08). Three of the four tests RUN and pass here -- their guards
are the read-only-directory and settings-page ones §12.2c already found
do not fire -- so a MISSED from `delete-both-halves`,
`remember-both-halves` or `settings-per-window` is a real hole rather
than a dormant guard, and should be chased rather than shrugged at. Only
`ssh-send-window` is genuinely ambiguous under `make test`, and that one
is provable under `make check-ssh` (§12.2f).

The warning stays as written, because the SOURCE can only say a guard
exists and that is a property of the tree rather than of this laptop.
What this adds is the reading: check whether the named test actually
skipped in the run that produced the MISSED, which the suite's own
output answers in one grep. **Reported and never failed
on**, because a guard that exists is not a guard that fires and refusing
would be a false finding on every run — the distinction between an
investigative sweep and a standing gate. The parser IS refused on: no
`QSKIP` anywhere under `test/` is not credible, so it exits 2 saying the
cross-reference is not being made rather than printing a reassuring
zero. Proved by breaking the match and watching it refuse.

**The control lives inside the tool and refuses.** The corpus is clean, so
a run over it reports nothing whether the checker works or not. Six
deliberately-wrong fixtures and one sound one are classified before any
real spec is read, and a wrong verdict prints *the control failed, so no
result below means anything* and exits 2. Proved by breaking the
classifier and by breaking the patch-set fallback: both refuse.

### 12.2c Three sweeps that found nothing, and what they were — **[2026-09-06]**

Recorded because an empty result is a measurement only when its method
is, and because these say where NOT to look next. All three lenses came
out of the day's real defects rather than off a list.

**Tests that do not RUN, from the sweep sabotage that stayed green.**
35 `QSKIP` sites; only one fires on this machine now —
`a_selection_change_is_announced_not_only_answerable`. Its guard was
re-measured rather than believed: a probe under Qt 6.12 shows
`QAccessible::setActive(true)` leaves `isActive()` false under BOTH the
offscreen platform and xcb under Xvfb, so the comment's claim still
holds and the skip is unavoidable without an AT-SPI bridge. The
accessibility sabotages are caught by other tests, so nothing hides
behind it. The read-only-directory, bundled-font and settings-page
guards do not fire here at all.

**Settings that reach nothing, from the TERM anchor that had rotted.**
All 16 `bssh_app_settings` getters have a caller outside the class.
`every_resolved_setting_reaches_something_that_uses_it` already covers
the per-host half; this is the app half, measured once by hand.

**A rule enforced at one door and not the other**, which is the shape of
both the Back key and the TERM call. Three actions in `main_window` are
guarded by their menu item's enabled state: `split_with` deliberately
does NOT repeat it -- asking for a pane that does not fit is REMEMBERED
rather than refused (sec 8.2) -- `show_as_tab` repeats it and cites
`tear_out`'s rule, and its sabotage is caught. `tear_out` was the one
that was guarded and untested, and that is fixed above.

**And one from hydra's lens, 2026-09-04: an environment variable gating
an Android diagnostic.** hydra found diagnostics gated on
`qEnvironmentVariableIsSet` never run on Android, because `am start`
cannot pass environment variables. beerssh has four environment-variable
reads and none gates a diagnostic: `SHELL` falls back to `/bin/sh`, which
exists on Android, and the `USER` default is on a command-line path a
phone never takes. Nothing found.

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
- **Copy put a newline in the middle of a wrapped command.** A line too long
  for the window is ONE logical line that ran past the right edge;
  `selection_text()` joined every ROW with `\n`, so copying it and pasting it
  into a shell ran the first half as a command and the remainder as another.
  Measured on a 70-column fixture: one `echo`, one newline in the middle of
  it. The same function already trims trailing padding, and says why —
  *"pasting the padding back into a shell is how a copied command arrives
  with a tail of spaces"* — so paste fidelity was the stated intent all
  along; this was the same hazard several orders of magnitude worse, sitting
  two lines above the comment describing the smaller one.
  **libvterm knew, and was being asked the older question.** It tracks
  continuation per row and offers it twice over:
  `vterm_state_get_lineinfo()` for rows still on the screen, and
  `sb_pushline4`'s extra argument for a row on its way into the scrollback.
  The callback table **named `sb_pushline4` and set it to `nullptr`**, with
  the opt-in call never made — so the bit existed, was reachable, and was
  declined. That is the shape worth remembering: not a missing capability but
  a declared one left switched off, which reads as a considered choice
  because the field is right there.
  **Two halves, two mechanisms, and the first test only reached one.** A row
  on the live screen is answered by libvterm's own state; once scrolled off,
  libvterm no longer holds it and the flag has to have been *stored* on the
  way past — so `bssh_scrollback` keeps a list parallel to its lines,
  maintained in every place the front or back is dropped. The screen test
  passes with the scrollback half entirely broken, which is why there is a
  second test that scrolls the line away first. Each half was sabotaged on
  its own: killing the screen half puts the newline back, and removing the
  `has_pushline4` opt-in leaves the command no longer contiguous.
  **Trailing blanks are not trimmed on a row that continues.** There the last
  column is where the text carries on, so a space at the end of it is content
  and trimming it would silently join two words.
- **`clear` did not clear the history, and the callback for it was the null
  beside the one above.** `CSI 3 J` — erase saved lines — is what `clear`
  sends as its terminfo `E3`, after homing the cursor and erasing the screen.
  `sb_clear` was declared in the same table as `sb_pushline4` and left
  `nullptr`, so the sequence arrived and nothing happened. Measured: sixteen
  scrollback lines before and sixteen after, for the bare sequence and for
  the whole of what `clear` sends. **Two nulls in one table, found by the
  same question**, which is the argument for reading a table of function
  pointers as a list of decisions rather than as configuration: every empty
  slot is a capability declined, and nothing distinguishes the ones declined
  on purpose from the ones nobody reached.
  Not cosmetic, either: running `clear` before pasting something private is
  exactly what people do with it.
  **`CSI 2 J` must NOT clear the history**, and the test says so — a fix that
  emptied the scrollback on any ED would take it away from every full-screen
  program that repaints. `bssh_scrollback::clear()` was already right for
  this: it advances the base by what it drops, so a selection anchored in the
  erased history fails to find its line rather than finding a different one
  under the same name. The change announces `scrollback_changed`, which the
  view already handles by re-clamping an offset measured against history that
  has gone.
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
  **It closed the spread road, not every road, and the rest is closed now.**
  The hold blocks a spread; nothing blocked an ordinary commit, so a session
  working directly in the tree could still move HEAD under a run. That was
  left to a deliberate pass rather than to whichever session was paying the
  thirteen minutes, since it blocks every other writer for the length of a
  gate and the party proposing it is the one who benefits.

  **Decided by the copyright holder 2026-09-01 and built.** `check-ci.sh`
  writes `.git/check-ci-running` -- gated commit, pid, start time -- and
  removes it in the trap it already had; `tool/hooks/pre-commit` refuses
  while that file names a live pid, printing what is gated and for how long.
  `--no-verify` is the escape, a dead pid makes it inert and clears it, and
  a garbled marker never blocks. All six behaviours are covered by a
  sabotage each, because a hook that has not been seen to refuse is not
  known to work.

  **What decided it was one more measurement rather than the argument.**
  2026-09-01, three runs in an evening with another session working here:
  the middle one earned a receipt for `204a3ac` that was stale before it
  could be used, because `663eeb9` landed while the container was still
  compiling. The run was not wrong, it was wasted -- and that is the fourth
  such hour.

  **It is machine-local, and that is a stated limit rather than a
  footnote.** `.git` is not shared, so this says nothing to a session on
  another computer, and this project is worked from more than one. Those
  writers are kept in step by fetching and pushing often, which is the only
  channel that crosses machines.
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
- **The pre-push hook does not run `check-ci` itself, and an opt-in to make
  it do so was declined** (2026-08-24/25). With three commits sitting
  unpushed for up to four days because nobody had run the gate, an opt-in
  `BSSH_PUSH_RUNS_CHECK=1` was proposed to run it inline. Its cost, stated
  at the time: it reintroduces the idle-socket failure the hook's own
  comment describes -- the remote closes a connection left idle for the
  length of a container build and the push dies of SIGPIPE -- for anyone
  who sets it, a foot-gun in a gate that has none. The holder: "leave it
  as is".
- **`tool/check-keepalive.sh` sends the whole in-container build to
  `/tmp/build.log`** and shows only its tail on failure, so the "applying
  N patch(es) to vendored libvterm" line that proves a reproduction from
  pristine never appears in its output the way it does in `check-ci`'s.
  Noted 2026-08-26, not changed.
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
   stops waking the radio.
   **The storm control above was the one requirement of §5.3 that was not
   built, and a test was named for it.** `note_network_changed` called
   `attempt_reconnect()` directly — no delay, no jitter, no attempt counted
   — and every manager in the registry hangs off the one platform
   `network_changed` signal. So a flap had all N sessions dialling in the
   same millisecond, by the single route §5.3 names: *"a network flap must
   not fire N simultaneous reconnects"*. The machinery to prevent it was
   already there and three lines away; the flap path went round it.
   **`jitter_separates_sessions_dropped_together` did not catch this, and
   its comment opens "the storm sec 5.3 warns about: one network flap drops
   eight sessions".** It measures `backoff_delay_ms`, a pure function, and
   proves the jitter separates eight draws — on the backoff path, which a
   flap does not take. A test can name the hazard exactly and still cover
   only the path that was already safe, because the name describes the
   scenario while the body describes the arithmetic. The new one asserts on
   the MANAGER: nothing dialled synchronously, a delay to be staggered by,
   and eight flapping sessions not returning in one instant.
   Routing the flap through `schedule_reconnect()` also counts the attempt
   and honours the cap, and the reset that was already there keeps it
   prompt — the delay becomes the base second rather than whatever the last
   outage had climbed to, so "try now" still means about a second instead
   of up to a minute. **Keepalive is built** (§4): a per-host
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
   Key authentication is what makes it seamless, exactly as §5.3 claims.
   ~~The password is consumed at first connect and deliberately not kept
   (§11), so a password-authenticated session could only reconnect as far as
   asking again.~~ **That is not what the code does, and the disagreement is
   recorded rather than resolved** — see *A password outlives the connect it
   was typed for* below.
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
   **Built.** ~~Except biometric unlock.~~ Public-key authentication is tried before any password
   (agent, then the default identities), so a key-authenticated session holds
   no credential at all and can reconnect on its own; passphrase-protected
   keys are offered an unlock prompt before any password, since for a
   key-only server a password prompt is not a fallback but a prompt that
   cannot succeed; and the trust-on-first-use prompt is built (§11).
   ~~Still open: biometric unlock.~~ **Built, and this line was stale.**
   Checked against the code on 2026-09-21 rather than read: the release
   point in `main_window.cpp` gates on `require_biometric` **and**
   `platform->has_biometric()`, calls `authenticate()`, hands the stored
   password over on success, and falls through to the typed prompt on a
   refusal or a cancel — with a `QPointer` because a system dialog
   outlives the session that raised it. §11 has recorded it as built
   since 2026-08-30, proven on a Galaxy Note 9 at API 29, while this
   summary went on calling it open.
   **What §11 still calls unproven stays unproven**, and the difference
   matters: the capability half is evidence, and that the prompt itself
   appears and its callback reaches the Qt thread needs a saved password
   on a real host. A summary that promoted that to "built" would be the
   same error pointing the other way.
   **The sweep that found it, and what it cost**: §14's claims of
   ABSENCE are the ones that send a reader at work already done, and
   there are three — this, Play asset-pack delivery, and the
   multi-monitor DPI check. Put to the code one at a time, one of the
   three was stale. The other two are true and need a Play account and a
   second monitor respectively, which is why they are still written down.
   The store IS wired
   in: a password can be remembered per host (off by default), the profile
   row carries a non-secret `has_password` flag so the app knows whether
   asking for the store's passphrase is worth it, and a remembered password
   is replayed on reconnect ~~instead of being held in memory all session~~ —
   and then held for the rest of it, by the same line that holds a typed one.
   See *A password outlives the connect it was typed for* below.

**A password outlives the connect it was typed for — [DECIDED 2026-09-26: it
does, for the session's life]**

**Measured 2026-09-21 by reading the code, not by inference.**
`bssh_session::supply_password` assigns `m_password` (`src/model/session.cpp`),
**nothing anywhere clears it** — there is no `m_password.clear()` in the tree —
and the `password_required` handler replays it. So a password-authenticated
session reconnects without asking, for as long as the session object lives, and
a *stored* password goes down the same road: the window hands it to
`supply_password` too, so after the first replay it is in process memory
exactly like a typed one. Two claims in this document said otherwise, M5's and
M10's, both struck above.

**The holder chose what the code does**: keep it until the session closes,
in memory only and never written to disk. §5.3's reconnect after a drop is
a core requirement, and a reconnect that stops to ask for a password at
3am is not one. The alternative offered -- discard after connecting, and
prompt again unless the password is in the keyring -- was declined for
that reason. The code's own comment already argued the narrower half:
the password is kept only once it has actually been needed.

**What stays open is small and is not the policy**: a `QString` is not
wiped on destruction and Qt may have copied it, so "memory only" means
"until the allocator reuses it". Whether that wants a zeroing container
is §11's to decide if it ever matters.
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
    disabled by default with an opt-in setting for the user who wants them
    (**BUILT 2026-09-18**: `appearance/font_ligatures`, off by default, fed
    to `make_font` through `bssh_terminal_view`'s device default the same
    way the scrollbar is, and effective on Qt 6.7+ where per-font feature
    control exists; guarded by `tool/sabotage/font-ligatures.toml`), the
    font directory is a `bssh_platform` question,
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
    Still open then: delivery and subsetting for the rest (since built as a
    download, §9.4; the phone default settled as JetBrains Mono, §16), and
    the fractional-scaling and multi-monitor
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
    **And guarded since 2026-09-20, which they were not before.** The naming
    was a measurement taken once by hand; nothing stopped a control being
    renamed back, and nothing looked at a control added later.
    `every_touch_surface_names_its_controls` walks the six touch surfaces
    the phone-fit sweep already builds and asks `QAccessible` what a reader
    would be told -- not `accessibleName()`, because a control with none can
    still be announced perfectly through a buddy or its own text, and
    asserting on the property would demand redundant names for controls
    that already work.
    **It found a real one on its first run**: the text-brightness slider
    announced nothing. Its form label says *Text brightness*, but `addRow`
    makes the CONTAINER the buddy, so a slider inside a box row inherits
    no name -- M11's own cause, on a control M11 never reached. It has a
    tooltip, which is not what a reader announces.
    **Two of the three findings were the instrument, and the split is the
    useful part.** A `QScrollBar` is a `QAbstractSlider` with no name to
    give, announced by role; a `QKeySequenceEdit` owns an internal line
    edit the reader never lands on, like a spin box or a combo. Both are
    excluded by TYPE rather than by forgiving the instance, because the
    rule is about what the class is -- a gate carrying a list of pardoned
    instances has been switched off by instalments. The probe
    over-reported and that is the direction worth having: a false positive
    prints something to go and read, a false negative prints nothing, and
    nothing is what a clean result looks like.
    **The count is asserted too.** A filter that stops matching turns the
    sweep into a gate over an empty list, which reports success exactly as
    loudly as a real pass -- so the number of controls inspected is checked
    as well as the number anonymous, and a sabotage proves that assertion
    is live rather than decorative.
    **And the first version asserted the wrong property, which the
    sabotage caught and reading never would have.** It checked that the
    accessible name was not EMPTY. Sabotaging the two names M11 added left
    the suite green: `m_add` carries `setText("+")` beside its accessible
    name, so removing the name makes `QAccessible` fall back to the text
    and answer `"+"` -- non-empty, and precisely the state M11 measured.
    **Emptiness was never the symptom.** What a reader announced was the
    GLYPH: "plus", "vertical ellipsis". So the assertion is now that the
    name says something a person could act on -- more than one character,
    and containing a letter or a digit -- because a glyph is an appearance
    and a control announcing its own appearance is the defect whatever the
    character is. A true condition that is not the one you mean passes for
    the same reason a correct one does, and only breaking the thing it
    claims to guard tells them apart.
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
| APK bloat from bundled fonts | Medium | 2–3 fonts in base, rest downloaded from pinned releases when chosen; subset fallbacks (§9.4). |
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

**Re-derived 2026-09-09, and it had gone stale again by one.** The method is
`evidence.md`'s — ask the source, not the sentence — so each entry was put to
the code rather than read: a grep for what would exist if it were settled, and
the answer taken from the tree. Ten of the eleven survived, one of those being
the holder's to decide on grounds this document does not hold.

The one that did not is the bundled soft keyboard, and §6.2 answers it in as
many words — *"a bundled soft keyboard was the open question here, and it is
answered"* — while this list went on calling it a lean no for v1. It has
`keyboard/`, ten files; a catalog with two styles; `KEYBOARD_SYSTEM` against
`KEYBOARD_BUILT_IN` as a setting a user picks; and 17 sabotage edits across
five specs. **The register contradicted the section it cited**, which is the
one disagreement a reader has no way to notice, because following the pointer
is the thing they do only when they already doubt the entry.

One in eleven, **34 days** after the last correction (`bf5b1a1`, 2026-08-06)
— so re-derive rather than read, and record the date when you do.

**Re-derived 2026-09-29**, the same way, over the whole document's "still
open" markers rather than this list alone. Four were stale, every one
outside this section: `do_send`/`do_resize` discarding results (fixed), a
user-supplied keyboard layout (built), the `row` arrangement (answered by
the cover-screen rule), and the column's two selector states (built as Pan
and Move). And the list itself was short by two open questions that lived
only in their sections -- the default scheme following the desktop, and a
user font file -- so it now carries them. **A register that only records
what reaches it cannot be complete**; the sweep has to go looking.

**Stale again on 2026-09-19, one day after the fact.** `8b4590b` settled the
system font-scale question — honour it for the interface, never for the
terminal grid, with an explicit setting — and §9.5 records it in those words.
This list still called it open. Found by the same method, putting the entry to
the code rather than reading it. What the interval says is worth keeping: it
is not the ten days since the last re-derivation that matter but the one day
since the decision. **A register goes stale the moment a decision lands, not
on a schedule**, so the re-derivation catches a backlog rather than a drift.

**Still open:**

- libssh (LGPL, dynamic link) vs libssh2 (BSD) — licensing may decide it, and it
  is the Android build that decides it (§4). **Costed 2026-09-30** (§4, *What
  each route costs*): algorithm parity, an equivalent for every call, about
  2,160 lines to rewrite -- so it is a licensing decision alone, with three
  routes, and the holder's.
- Foreground-service type: `connectedDevice` (built and shipped in the manifest)
  vs `specialUse` (prepared fallback). Open on Play review's opinion, not on
  ours — §5.1. **Costed 2026-09-30** against Android's and Play's current
  texts, with a third route found: `remoteMessaging`, which ConnectBot uses.
  **And Android 14 accepts `connectedDevice` as built** (API 34 emulator,
  2026-09-30), so what is open is Play's review alone.
- ~~Inhibit sleep on desktop while a session is connected?~~ **DECIDED
  2026-09-26 by the holder: a per-host toggle, off by default -- and BUILT.**
  "Keep the computer awake" beside "Keep scrollback" in the desktop dialog,
  offered only where the platform can do it (not on Android, where the
  foreground service already keeps the connection and the screen is a
  different question). While any session of such a host is live --
  connecting and reconnecting included, since a build waiting on a dropped
  link is still a build -- `bssh_platform::set_sleep_inhibited` asks logind
  for a `sleep:idle` block inhibitor on the system bus and
  `org.freedesktop.PowerManagement.Inhibit` on the session bus, both
  asynchronously, and lets both go when the last one closes. The platform is
  asked only when the answer changes, since the window asks on every
  registry change. Whether TDE's own power manager honours either was not
  checked. Guarded by `tool/sabotage/keep-awake.toml`, five edits.
- ~~Offer system-installed monospace fonts on desktop?~~ **DECIDED 2026-09-26
  by the holder: yes, in a group of their own -- and BUILT.** After the set,
  a separator and a disabled "Installed on this computer" heading, then every
  family Qt calls fixed-pitch that the set does not already name, sorted --
  the fallback-only families (Unifont, Noto Sans Mono) stay out by that rule,
  under their `system:` ids too. Desktop only. An installed font's id is
  `system:<family>`, and the catalog answers for it as for a set font:
  `is_available`, the fallback chain (the family first, the set behind it
  for glyphs it lacks), the substitution note, and `display_name` for the
  override badge. One that has since been uninstalled is kept and marked
  "(not installed)", drawn in the set's default meanwhile. Not vetted the
  way the set is (§9.2): fixed-pitch is Qt's word for it, not a promise
  every glyph fits the cell. Guarded by `tool/sabotage/installed-fonts.toml`,
  six edits.
- ~~Desktop `ssh-agent` support~~ -- **the question was stale, and is
  answered in two halves.** USING the agent is built: `ssh_userauth_publickey_auto`
  tries it before the default key files (`ssh_session_libssh.cpp`, and the jump
  tunnel likewise), which is what the connect dialog's note has said. MANAGING
  it -- adding a key to the running agent once its passphrase is typed -- the
  holder declined for now on 2026-09-26: `ssh-add` and the desktop keep that.
- ~~Which font is the **phone** default (JetBrains Mono vs condensed
  Iosevka)?~~ **DECIDED 2026-09-29 by the holder: JetBrains Mono on every
  form factor -- and BUILT.** The code already preferred condensed Iosevka on
  a touch-primary device whenever it was installed, and with the download
  built (§9.4) that turned into a trap: fetching Iosevka for one host
  changed the font of every host that had never chosen one. Rejected with
  it: fetching Iosevka unasked at first launch (~43 MB of network before the
  first terminal). `default_id()` lost its form-factor argument, and
  `a_downloaded_font_does_not_become_the_default` holds it where Iosevka is
  installed. Iosevka stays one pick and a download away.
- ~~Host groups/folders in addition to presets?~~ **DECIDED 2026-09-26 by the
  holder: folders for organising only -- and BUILT.** They group and collapse
  hosts in the list; settings still come only from presets, so there is one
  inheritance mechanism rather than two. A host's `folder` is a name in
  `hosts.ini`, set by an editable "Folder" field offering the folders that
  exist (beside the name on the desktop, which has no height to spare).
  `bssh_group_by_folder` orders both lists the same: unfiled hosts first,
  then folders by name with case aside, so "Work" and "work" are one. A
  folder is a heading row that folds and unfolds on a tap or click and opens
  nothing -- selectable, so a tap lands on it rather than on whichever host
  was current. Folded state lasts as long as the list. One level, no
  nesting. Guarded by `tool/sabotage/host-folders.toml`, seven edits.
- ~~Persist scrollback across restarts?~~ **DECIDED 2026-09-26 by the holder:
  yes, opt-in per host -- and BUILT.** Recommended against as a default
  and chosen as an option: scrollback is whatever was on screen, a password
  typed at the wrong prompt and secrets in output included, so it is off
  until a host asks. "Keep this host's scrollback after closing" in both
  editors (the desktop one says "stored unencrypted" in the label and the
  rest in its tooltip, because the dialog must fit 800x600).
  `bssh_kept_scrollback` writes one compressed file per host under
  `<config>/scrollback/`, owner-only, folder too; cells with their colours,
  not hyperlinks, whose ids mean nothing outside one session. The registry
  writes it when a session closes -- with the screen as it stood, which had
  not scrolled off yet and is the part most wanted, unless a program had
  the alternate screen up -- and on quit and on leaving the foreground,
  since Android may end a backgrounded app unasked. A new session of the
  host gets it back before any output, under a line reading "-- kept from
  <date> --". Turning the option off, or deleting the host, deletes the
  file. A file that does not decode whole restores nothing. Guarded by
  `tool/sabotage/kept-scrollback.toml`, nine edits; one of two permission
  calls was removed when each proved to hide the other.
- ~~**How long may a session keep a password it authenticated with?**~~
  **DECIDED 2026-09-26 by the holder: for the session's life**, in memory
  only, as the code already did -- see *A password outlives the connect it
  was typed for* in §14.
- Icon and branding direction (§0.1), which the placeholder in §12.2 is waiting
  on.
- ~~Should the **default** colour scheme alone follow a light desktop?~~
  **DECIDED 2026-09-29 by the holder: yes, to the existing `black-on-white` --
  and BUILT** (§6.2, §10.2).
- ~~A **user-supplied font file**~~ **DECIDED 2026-09-29 by the holder, and
  BUILT** (§9.4): the reserved entry the other three catalogs have.
- ~~**Is the image budget per SESSION or per PROCESS?**~~ **DECIDED
  2026-09-26 by the holder: per terminal, and the user's to set.** Asked why
  it is a hard limit at all -- it never removes a picture on screen, only the
  history kept after pictures scroll off -- the holder answered: "Sounds like
  we need the ability to dynamically load and unload these, but for now have
  a settable static amount per terminal." BUILT as "Picture history" on the
  Terminal settings page, 0-1024 MB, default the old 32, applied to every
  session from the moment it is created and re-applied when it changes --
  `set_memory_budget` has its caller. **The holder's direction, loading
  scrolled-off pictures back on demand instead of dropping them, is BUILT
  (2026-09-26, §7.1)**: past the budget a picture is sealed to disk and read
  back when scrolled to, so the setting bounds what is resident.
- ~~**Should a session's zoom PERSIST?**~~ **DECIDED 2026-09-26 by the
  holder: per host, and built** -- see §9.5. §9.5 recorded "not persisted" as a
  choice a phone might reasonably want reversed -- "a session that is always
  read outdoors is a host preference wearing a gesture's clothes". Pinch-to-
  zoom landed 2026-09-22 and deliberately left this half alone: it added an
  input route to the existing per-session number and decided nothing about
  its lifetime.
- ~~**Should a drop onto a FULL window collapse the split?**~~ **ANSWERED NO,
  2026-09-22**, and answered by me rather than by the holder, which is why
  this entry says so plainly. It was raised here and the holder's reply to it
  and to everything else was to carry on implementing; after the tenth such
  reply I took it rather than leave the register as the only thing happening.
  **The rule: a window showing SEVERAL panes keeps them and the drop arrives
  as a tab; a window showing ONE still hands the screen over.** Both halves
  matter -- the first because an arrangement made by hand should not go
  silently in one drag, the second because a drop that changes nothing
  visible reads as a drop that failed, which is what the fallback was written
  against. **Reversing it is deleting one branch in `drop_session`**, and the
  two sabotages in `arrangement.toml` will then fail in the direction that
  says which half went.
- ~~**Should `--load` be able to list what it accepts?**~~ **BUILT
  2026-09-22** on the holder's repeated instruction to carry on, being the
  one of the four that overwrites no recorded decision: it is additive, and
  `--term-features=?` had already set the shape. See §8.6.

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
| Bundled soft keyboard — built, `KEYBOARD_SYSTEM` or `KEYBOARD_BUILT_IN` | §6.2 |
| System font-scale — honoured for the interface, never for the grid | §9.5 |

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

### Driving the shared phones over adb, without landing in somebody else's app

The phones on this desk are shared -- with the holder, who may be holding
one, and with hydra's and bbq-predictor's sessions, which launch their own
apps on it. Collected from 2026-08-26 to 09-18, because every item cost
something:

- **Check the foreground app immediately before EVERY injected input**,
  not once per sequence. Blind `input tap` / `input text` went, over those
  weeks, into bbq-predictor, hydra's URL bar, a browser, the Camera (via
  the dock, after `BACK` had left beerssh for the home screen), the Clock
  while the holder held the phone -- and on 2026-09-03 into the holder's
  live Teams call, typing `echo KEYBOARD_STAYS_UP` and Enter, because
  hydra's session kept taking the foreground. Guard on
  `topResumedActivity`: `mCurrentFocus` goes null during Qt window
  transitions, so a guard on it refuses forever. When the foreground keeps
  moving, stop and ask whether to drive the phone or only watch. Coordinate
  with hydra's session before using the phone at all.
- **`BACK` at the terminal leaves the app**, so the next tap lands on the
  home screen; and when beerssh crashes mid-`input text`, the rest of the
  typing goes to the launcher.
- **A locked or dozing phone still reports beerssh as focused**, so any
  reading taken then is void -- it produced a false Back-key "bug". `svc
  power stayon usb` keeps it awake (approved by the holder); set it back
  with `svc power stayon false`. A white capture means the screen is off,
  a black one a lock screen, and `adb pull` of a fixed path can hand back
  a stale screenshot another session left there.
- **`dumpsys input_method`'s `mInputShown` read false while the keyboard
  was on screen**; do not use it as the witness.
- **Gestures**: `input swipe` arrives as a fling and never starts a drag;
  `input motionevent DOWN/MOVE/UP` does. Coordinates are per rotation --
  portrait coordinates on a landscape phone hit something else.
- **The Fold**: logical display 0 swaps to the inner screen when unfolded,
  and `input -d` wants the logical id (`screencap -d` is in §8.7).
  `cmd device_state print-state` gives 0 for CLOSED and 2 for HALF_OPENED.
  Unfolding re-enumerates USB and drops adb; the long outages were the
  phone falling back to charge-only USB on reconnect, and one bad cable.
  `lsusb` says whether the fault is below adb -- and a wrong `lsusb` probe
  once sent the holder swapping cables for nothing. Restarting the adb
  server drops hydra's tunnel.
- **Editing the app's settings on the device**: force-stop the app, then
  `run-as se.vibes.beerssh <cmd>` on `/data/user/0/...` rather than
  `/data/data/...`; `run-as ... sh -c` loses the working directory.
  toybox `sed` does not take the one-line `a` form and writes `\n`
  literally, which once mangled `app.ini`.
- **A tombstone is not ours until its header says so.** A "deadlock
  protector" tombstone was nearly claimed as beerssh's; its `>>>
  se.vibes.hydra <<<` line said otherwise. Read the package line first.
- **Credentials are never typed by a session.** The `vibes.se` profile on
  the phone has no key, so the local shell is the only credential-free
  test surface; twice a session stopped at the password prompt, and the
  holder's "go ahead and connect" was taken as a one-off each time. That
  is the sessions' own practice, not an instruction from the holder.
- **A shared device driven through screenshots cannot answer a question
  about specific grid cells** -- measure those in the emulator or a test.

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
- **Three messages cite the wrong section**, and the log cannot be
  corrected, so it is recorded here: `d0e1910` says §18 for a rule in
  §12.4, `713ce39` says §2 for a non-goal in §1, and `a9152c8` says §8.2
  for the 600dp MEDIUM boundary, which is §8.1's.

### 18.6 The gate's own suite runs before the gate's verdict

`tool/style_gate.py` is copied verbatim into every private project from
`~/.claude/tool/`, and `tool/test_style_gate.py` is its suite, copied the
same way. **Measured 2026-09-04 across eight trees — beerssh, hydra, situ,
fmake, netcfgd, apt-emerge, fuzzypickles and raidcfgd — every one carried
the suite and not one ran it.** The tests had travelled everywhere and were
executed only in the tree they came from, so a defect in the shared gate
would have been caught in one place out of nine.

`make style` is `style-tool style-source style-docs`, in that order. The
suite goes first because a gate's pass is worth reading only once the gate
is known to work: a checker that has stopped inspecting anything reports
success exactly as loudly as one that discriminates. It costs seconds
rather than minutes — 9 and 16 on two runs of this machine.

Under `style` rather than `test` on purpose, and it reaches further that
way: `tool/check-ci.sh` runs `make style` inside the container against a
tree exported from `git archive HEAD`, so the suite is proved on a clean
checkout as well as on a developer machine. `make test` never leaves this
machine.

**The wiring was verified by breaking the gate and watching the suite,
which took two attempts and the first one is the more useful.** Changing
`split_leading_ws`'s tab expansion from `col += width - (col % width)` to
`col += width` left all 106 tests green — which looked like a coverage
gap and was not, at first reading. The two expressions differ only when a
tab is preceded by something that leaves the column off a tab stop, and
every such line in leading whitespace is a space-before-tab, which is
rejected by an earlier rule. Probed directly, the broken function returns
column 11 where the correct one returns 8, and `check_text` produces
*identical verdicts* on all four crafted inputs. **A control has to be
able to fail the way the thing it controls for fails; that one could
not.** The second attempt — disabling the `space-indented line; use tabs`
branch — turned the suite red with exit 1, which is the control that
counts.

**What the first attempt did turn up is a real gap, and it belongs to the
shared tool rather than to this tree.** `split_leading_ws` is called only
from the conversion and column paths (`style_gate.py` lines 1143, 1599 and
1630), never from `check_text`, and the suite never names it. So the retab
converter's column arithmetic — precisely what `harmonization.md` says must
be proved independently of the tool that performed the retab — survives
being broken with the suite green. Signalled rather than fixed here: the
suite is spread from `~/.claude/tool/`, and a fix made in one copy is how
sixteen copies stop agreeing.
