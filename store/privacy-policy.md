# BeerSSH privacy policy -- DRAFT for the holder to review and publish

Every app on Google Play needs a privacy policy URL, including one that
collects nothing (the play-store skill). This is its source, written from
what the code does as of 2026-11-02; each statement names where it can be
checked, so a later change to the code can be checked against it. It is a
statement in the holder's name and is published by the holder.

---

**BeerSSH collects no personal data. The developer receives nothing from the
app: no analytics, no crash reports, no advertising identifiers.**

## What BeerSSH connects to

- **The hosts you add or open.** BeerSSH makes an SSH connection to the
  address you give it, and to the jump host you name for it, if any. What
  passes over that connection is between you and that server.
- **Font downloads, only when you ask.** A few terminal fonts are offered for
  download rather than bundled. Tapping download fetches them from github.com
  or unifoundry.com (`tool/font-sources.txt`). Those servers see your IP
  address, as any web request does.

Nothing else. The app has no server of its own and contacts none.

## What stays on your device

- **Your hosts and settings**: host names, user names, ports and per-host
  settings, in the app's private storage.
- **Saved passwords**, only if you choose to save one: kept in the Android
  Keystore, and released only after a fingerprint check if you ask for that.
- **Private keys**: BeerSSH reads the key files you point it at and does not
  copy them elsewhere.
- **Host keys** of the servers you have connected to, to warn you if one
  changes.
- **Kept scrollback**, only for a host where you turn it on: the terminal's
  history, stored unencrypted in the app's private storage so it survives a
  restart. The setting says so where you turn it on.

Uninstalling BeerSSH removes all of it.

## Permissions

- **Internet and network state**: to connect to your hosts and to notice when
  the network changes, so a session can reconnect.
- **Foreground service**: to keep your sessions running while the app is in
  the background.
- **Notifications**: to show that sessions are running.
- **Biometric**: only to check your fingerprint before using a saved password,
  if you ask for that.

## Children

BeerSSH is a tool for administering servers and is not directed at children.

## Contact

(the holder's address)

## Changes

A change to this policy is published here, with its date.
