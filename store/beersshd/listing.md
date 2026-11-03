# beersshd Google Play listing -- DRAFT for the holder to review and publish

beersshd is published apart from BeerSSH (project.md sec 19), so it has a
listing of its own. The words below are a statement made in the holder's
name, so they are a draft until the holder publishes them (the play-store
skill). Limits are Play's: name 30 characters, short description 80, full
description 4000; the counts beside each are measured with `wc -m` on the
text alone. Drafted 2026-11-03 from what the code does.

## App name (22 of 30)

beersshd — SFTP server

## Short description (78 of 80)

Back up and reach your phone's files over SFTP from any computer on your Wi-Fi

## Full description (1192 of 4000)

beersshd turns your phone into an SFTP server on your home Wi-Fi, so a
computer can back it up, or copy files to and from it, on its own schedule
-- with the tools it already has: rsync over sshfs, rclone, restic, a file
manager, or plain sftp and scp.

It is a server and nothing else. It serves your phone's shared storage --
Downloads, DCIM, Documents and the rest -- over SFTP, and refuses shells,
commands and forwarding outright.

Reachable when you need it:
• It keeps running with the screen off and the app closed, with a
  notification the whole time and Stop in it.
• It follows your Wi-Fi: if the phone leaves and comes back, it listens
  again on its own.
• Optionally, it starts again by itself after the phone restarts, so a
  night-time update does not cost the next backup.

Only who you allow:
• Public keys, pasted in as in an authorized_keys file.
• Optionally a password, stored only as a salted hash on the phone.
• Wi-Fi only: it never listens on mobile data.
• The host key is made on the phone and shown, to check on first connect.

beersshd collects nothing about you. It contacts no server; computers on
your Wi-Fi connect to it, and only with your key or password.

## Support e-mail

(the holder's to give, as for BeerSSH)

## Category and tags

Category: Tools. Tags: SFTP, backup, file transfer.
