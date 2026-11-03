# beersshd privacy policy -- DRAFT for the holder to review and publish

Every app on Google Play needs a privacy policy URL, including one that
collects nothing (the play-store skill). This is its source, written from
what the code does as of 2026-11-03; each statement names where it can be
checked. It is a statement in the holder's name and is published by the
holder.

---

**beersshd collects no personal data. The developer receives nothing from
the app: no analytics, no crash reports, no advertising identifiers.**

## What beersshd does with the network

- **It listens; it does not call out.** While you have it switched on, it
  accepts SFTP connections on your phone's Wi-Fi address and nowhere else
  -- never on mobile data (`listen_address()` in
  `src/sshd/sshd_platform_android.cpp`). It makes no connection of its own
  to anything.
- **Only to the keys and password you set.** A computer gets in with a
  public key you listed, or with the password if you set one; anything else
  is refused (`src/sshd/sshd.cpp`).

## What it can reach on your phone

With "All files access", which you turn on in Settings, a computer you let
in can read and change your phone's shared storage -- Downloads, DCIM,
Documents and the rest -- and nothing outside it: not other apps' private
data, and not beersshd's own keys (`src/sshd/root_path.cpp`). That access is
the app's purpose: it is what lets your computer back the phone up.

## What it keeps on your phone

- Its host key, the keys you listed, and your password as a salted PBKDF2
  hash -- never the password itself -- in its own private storage, which is
  left out of Android's cloud backup (`allowBackup="false"`).
- A log of connections, in the same place, shown in the app and cut back
  as it grows.

Nothing of this leaves the phone. Uninstalling removes all of it.

## Children

beersshd is a tool for administering your own devices and is not directed
at children.

## Contact

(the holder's address)
