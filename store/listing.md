# Google Play listing -- DRAFT for the holder to review and publish

The words below are a statement made in the holder's name, so they are a
draft until the holder publishes them (the play-store skill). Limits are
Play's, read 2026-10-22: name 30 characters, short description 80, full
description 4000. The counts beside each are measured with `wc -m` on the
text alone, not estimated.

## App name (22 of 30)

BeerSSH — SSH Terminal

## Short description (80 of 80)

SSH terminal that keeps sessions running in the background, with a real keyboard

## Full description (2017 of 4000)

BeerSSH is an SSH terminal for people who run real work on remote machines
from a phone.

Sessions keep running when you switch apps or the screen turns off, in the
foreground service Android requires for it. A session that logged in with a
key reconnects by itself when the network comes back, because it holds no
password it would have to ask you for.

A real keyboard for a shell:
• A key row with Ctrl, Alt, Esc, Tab, the arrows and function keys, above your
  usual keyboard -- or BeerSSH's own keyboard, with Ctrl and Alt held by one
  finger while another types.
• Physical and Bluetooth keyboards work fully, including Ctrl, Alt and the
  function keys.

A terminal that keeps up with a desktop one:
• Correct xterm emulation: colours, mouse, the alternate screen for vim, htop
  and mc, UTF-8 and wide characters.
• Text rewraps when the screen changes size instead of being cut off, and the
  scrollback rewraps with it.
• The kitty keyboard protocol, inline images, OSC 8 links (it asks before
  opening an address the host chose) and synchronised output.

Many sessions at once:
• Tabs, and splits on a tablet, a foldable or a connected display; on a phone
  held upright, the panes become one scrolling column.
• A drawer that lists the host's tmux and screen sessions and switches between
  them in one tap.

Your keys and passwords stay on the phone:
• Public-key login is tried first, from your own key files.
• A saved password is kept in the Android Keystore and can be set to need your
  fingerprint before it is used.
• Host keys are trusted the first time and checked every time after, with a
  plain warning when one changes.

Settings that stay out of the way: one font, size and set of colours for every
host, a curated set of terminal fonts, and a host's own settings only where
that host needs them.

BeerSSH collects nothing about you. It connects to the hosts you add, and to
nothing else unless you ask it to download a font.

Not included: SFTP file browsing, port forwarding and Mosh.

## Support e-mail

(the holder's to give -- see the run's questions)

## Category and tags

Category: Tools. Tags: SSH, terminal, sysadmin.
