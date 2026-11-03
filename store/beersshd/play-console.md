# beersshd Play Console declarations -- DRAFT for the holder to review and submit

Every answer here is a statement in the holder's name, made in the Console
by the holder (the play-store skill). Drafted 2026-11-03 from the code and
from project.md sec 19.7, which has the reasoning, the policy text quoted
and the precedents; the rhetoric below follows its five points.

## Foreground service: connectedDevice

The manifest declares `foregroundServiceType="connectedDevice"` with
`CHANGE_NETWORK_STATE` qualifying it, in the `:server` process (sec 19.6).

**Description.** beersshd is an SFTP server for the user's own computers on
the same Wi-Fi. While the user has it switched on it keeps a listening
socket on the phone's Wi-Fi address, so a computer can transfer files to
and from the phone -- most often a backup that the computer runs on its own
schedule. It is started by the user's Start, or, only if the user turned on
"Serve again after the phone restarts", at boot; a notification is shown the
whole time it runs, with Stop in it and in the app, and it ends when the
user stops it.

**User impact if deferred or interrupted.** The computer cannot reach the
phone, so that run of its backup does not happen, and nothing tells the user
until they need the files. A transfer in progress is cut off. Neither can
be deferred: the computer chooses when to connect, not the phone.

**Use case.** Data transfer to a device over a network connection -- the
words of Android's definition of the type and of Play's 2027 policy text
(sec 19.7). Not `dataSync`: Play keeps that for user-initiated transfers and
names the scheduled backup as what it is not for.

**Video (the holder's to record).** On the phone: Start in beersshd, then
Home and screen off. On a computer on the same Wi-Fi: run a backup against
the phone (`rsync` over `sshfs`, or `restic -r sftp:...`); it completes.
Then Stop in the notification: the service and the notification end.

## All files access (MANAGE_EXTERNAL_STORAGE)

**Core functionality.** A backup and restore app, by Play's own category:
the computer's backup tool reads the phone's shared storage over SFTP.

**Why the Storage Access Framework will not do** (sec 19.7, option C
costed). On Android 11 and later the folder picker cannot grant the root of
shared storage or the Download folder at all, so a backup through it would
leave out the folder where a phone's received files land, and would need a
separate grant for every other folder. The server's handlers work on file
paths; the grant has to be to the storage, not to picked documents.

## Battery optimisation (REQUEST_IGNORE_BATTERY_OPTIMIZATIONS)

Asked only from the app's "Allow running in the background" button, which
is shown only while the app is still optimised (sec 5.4). The reason is the
one above: a server that a vendor's battery manager pauses cannot be
reached, and the backup that needed it does not run.

## Data safety

- **Does the app collect or share any of the required user data types?**
  No. Nothing reaches the developer, and the app makes no outbound
  connections.
- **Is all data encrypted in transit?** Yes: every connection is SSH.
- **Can users request that data be deleted?** Nothing is held off the
  device; uninstalling removes the keys, password hash and log.

## Content rating questionnaire

Category: **Utility, productivity, communication, or other.** No content of
any rated kind; it moves the user's own files between their own devices.
Expected rating: *Everyone*.

## Target audience

18 and over, as for BeerSSH: a tool for administering one's own devices.

## App access -- the holder's to provide

A reviewer needs a computer on the same Wi-Fi to see it work. Give the
instructions: "Grant All files access when asked, tap Start, set a
password, and from a computer on the same network run
`sftp -P 8022 <phone address>` with that password."

## Ads

No ads.
