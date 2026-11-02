# Play Console declarations -- DRAFT for the holder to review and submit

Every answer here is a statement in the holder's name, made in the Console
by the holder (the play-store skill). Drafted 2026-11-03 from what the code
does; where an answer rests on code, the place is named so a later change
can be checked against it. Section numbers are project.md's.

## Foreground service (sec 5.1, sec 13)

The manifest declares `foregroundServiceType="connectedDevice"` with
`CHANGE_NETWORK_STATE` qualifying it -- the type as built, accepted by
Android 14 on an emulator (sec 5.1). Play asks, per type, for a description,
the user impact if the task is deferred or interrupted, a video, and a use
case from its list (Play Console Help 13392821, read 2026-09-30).

**Description.** BeerSSH keeps the user's SSH sessions -- interactive shells
on remote machines they have connected to -- open while the app is not on
screen. The service runs only while at least one session is connected, shows
a notification saying so for as long as it runs, and stops when the last
session ends.

**User impact if deferred or interrupted.** The SSH connection to the remote
machine is dropped. Whatever the user was running in that shell -- an editor,
a long command, a database console -- loses its terminal, and work not saved
on the remote side is lost. A connection cannot be deferred and resumed
later: SSH has no way to pick an interrupted session back up.

**Use case.** The preset list's examples for connected devices are external
hardware (wearable, headset, car); a remote server reached over the network
is the "network connection" in Android's own definition of the type
(developer.android.com, *Foreground service types*). If the list offers
nothing closer, choose the nearest and say so in the description.

**Video (the holder's to record).** Open BeerSSH, connect to a host, run a
command that takes a while (`ping -c 30 example.com`), press Home, open
another app for half a minute, return: the command has kept running and the
notification was shown throughout.

**If Play refuses `connectedDevice`** -- the fallback sec 5.1 prepared:
`specialUse` with the subtype property *"Interactive remote shell (SSH) kept
connected while the user switches apps"*, the same description and impact
above, or `remoteMessaging`, which ConnectBot declares. Either is a manifest
change and an Android 14 run on `bssh_api34` before resubmitting.

## Data safety

- **Does the app collect or share any of the required user data types?**
  No. The developer receives nothing: no analytics, no crash reporting
  (the hang watchdog writes to the device's own log), no advertising ID.
- **Is all data encrypted in transit?** Yes -- every connection the app
  makes is SSH, or HTTPS for an optional font download
  (`tool/font-sources.txt`: github.com, unifoundry.com).
- **Can users request that data be deleted?** Nothing is held off the
  device. On the device, uninstalling removes everything, and deleting a
  host erases its saved password -- at once, or, where the password store
  is locked, the next time it is unlocked (sec 11; `remove()` and
  `bssh_sweep_orphaned_secrets()` in profile_store.cpp).
- Data that stays on the device and never reaches the developer -- hosts,
  user names, saved passwords (Android Keystore), host keys, kept
  scrollback where the user turns it on -- is not "collected" in Play's
  sense, which counts only what leaves the device.

## Content rating questionnaire

Category: **Utility, productivity, communication, or other.** No violence,
sexual content, profanity, controlled substances, gambling or horror. The
name's "beer" is a brand word, not content (sec 0.1): the app shows no
alcohol. The app does not let users interact with or share content with
other users of the app; it connects the user to machines they administer.
Expected rating: *Everyone* (sec 13).

## Target audience

18 and over is the honest audience for a tool that administers servers; it
is not directed at children (privacy-policy.md says so).

## App access -- the holder's to provide

On Android the app opens on its host list: there is no local shell (sec 1),
so a reviewer can see the settings and the keyboard but nothing of a
session without a host to connect to. Play asks for instructions to reach
every feature. Provide a test host -- an address, a user and a password, on
a machine set up for the purpose and holding nothing -- and say: "Tap +,
enter the address, user and port, tap Add host, then tap the host and enter
the password."

## Ads

No ads.
