#!/usr/bin/env python3
"""Convert an old BeerSSH SQLite store into the INI config files.

BeerSSH kept hosts, presets and layout in a SQLite database until the
config moved to plain INI files (project.md sec 10.6). This converts one
into the other, once.

It is a script rather than code in the app for a deliberate reason: the app
no longer links Qt SQL at all, and carrying a whole database driver
forever so that it can read one file one time is a poor trade. Python's
sqlite3 is in the standard library, so this costs nothing and can be
deleted the day nobody has an old database left.

Non-destructive. The database is read and left exactly where it is; if the
INI files already exist the script refuses rather than merging, because
merging two sets of hosts silently is worse than doing nothing.

Usage:
  tools/migrate-db-to-ini.py [--dry-run] [database] [config-dir]

Defaults match where each version put its files:
  database    ~/.local/share/beerssh/beerssh.db
  config-dir  ~/.config/beerssh
"""

import json
import os
import re
import sqlite3
import sys


def slug_for(name):
    """Match bssh_profile_store's slug rule: the section name a host refers to."""
    slug = re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")
    return slug or "preset"


def ini_value(value):
    if isinstance(value, bool):
        return "true" if value else "false"
    return str(value)


def write_ini(path, sections, dry_run):
    """sections: list of (name, [(key, value), ...])"""
    lines = []
    for name, pairs in sections:
        lines.append("[%s]" % name)
        for key, value in pairs:
            lines.append("%s=%s" % (key, ini_value(value)))
        lines.append("")

    text = "\n".join(lines)

    if dry_run:
        print("--- %s ---" % path)
        print(text)
        return

    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)
    print("wrote %s" % path)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    dry_run = "--dry-run" in sys.argv

    home = os.path.expanduser("~")
    database = args[0] if args else os.path.join(
        home, ".local/share/beerssh/beerssh.db")
    config = args[1] if len(args) > 1 else os.path.join(home, ".config/beerssh")

    if not os.path.exists(database):
        sys.stderr.write("no database at %s -- nothing to migrate\n" % database)
        return 0

    hosts_ini = os.path.join(config, "hosts.ini")
    presets_ini = os.path.join(config, "presets.ini")

    if not dry_run:
        for existing in (hosts_ini, presets_ini):
            if os.path.exists(existing):
                sys.stderr.write(
                    "%s already exists.\n"
                    "Refusing to merge two sets of config -- move it aside "
                    "first if you want this database imported.\n" % existing)
                return 1
        os.makedirs(config, exist_ok=True)

    connection = sqlite3.connect(database)
    connection.row_factory = sqlite3.Row

    # --- presets, and the id -> slug map the hosts need ---------------------
    presets = []
    slugs = {}

    try:
        rows = connection.execute(
            "SELECT id, is_builtin, json FROM presets ORDER BY name COLLATE NOCASE"
        ).fetchall()
    except sqlite3.OperationalError:
        rows = []          # a database old enough to predate presets

    for row in rows:
        fields = json.loads(row["json"])
        name = fields.get("name", "preset")

        slug = slug_for(name)
        suffix = 1
        while slug in slugs.values():
            suffix += 1
            slug = "%s-%d" % (slug_for(name), suffix)
        slugs[row["id"]] = slug

        pairs = [(k, v) for k, v in sorted(fields.items()) if k != "schema"]
        pairs.append(("id", row["id"]))
        if row["is_builtin"]:
            pairs.append(("builtin", True))

        presets.append((slug, pairs))

    # --- hosts, with their overrides flattened into the section -------------
    hosts = []
    for row in connection.execute(
            "SELECT * FROM profiles ORDER BY name COLLATE NOCASE"):
        keys = row.keys()
        pairs = [
            ("name", row["name"]),
            ("host", row["host"]),
            ("user", row["user"] or ""),
            ("port", row["port"]),
        ]

        if "has_password" in keys and row["has_password"]:
            pairs.append(("has_password", True))

        if "preset_id" in keys and row["preset_id"] and row["preset_id"] in slugs:
            pairs.append(("preset", slugs[row["preset_id"]]))

        # Overrides become ordinary keys in the host's section -- which is
        # the whole point of the format: one place to look, editable.
        if "overrides" in keys and row["overrides"]:
            for key, value in sorted(json.loads(row["overrides"]).items()):
                pairs.append((key, value))

        hosts.append(("host-%d" % row["id"], pairs))

    connection.close()

    write_ini(hosts_ini, hosts, dry_run)
    write_ini(presets_ini, presets, dry_run)

    if not dry_run:
        print("\n%d host(s) and %d preset(s) migrated." % (len(hosts), len(presets)))
        print("The database was not touched: %s" % database)
        print("Delete it once you are satisfied the config is right.")

    return 0


if __name__ == "__main__":
    sys.exit(main())
