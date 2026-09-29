#!/usr/bin/env python3
"""Resolve a running cefbrowser's static root without selecting CEF children."""

from pathlib import Path
import posixpath
import sys


def static_argument(argv):
    values = []
    index = 1
    while index < len(argv):
        arg = argv[index]
        if arg in ("--staticPath", "--staticpath", "-s"):
            index += 1
            if index == len(argv) or not argv[index] or argv[index].startswith("-"):
                raise ValueError("static path option has no value")
            values.append(argv[index])
        elif arg.startswith(("--staticPath=", "--staticpath=")):
            values.append(arg.split("=", 1)[1])
        elif arg.startswith("-s") and len(arg) > 2:
            values.append(arg[2:])
        index += 1
    if any(not value for value in values) or len(set(values)) > 1:
        raise ValueError("empty or conflicting static path options")
    return values[0] if values else None


def detect(proc=Path("/proc")):
    processes = {}
    for entry in proc.iterdir():
        if not entry.name.isdecimal():
            continue
        try:
            if (entry / "comm").read_text().strip() != "cefbrowser":
                continue
        except FileNotFoundError:
            continue
        # Unreadable or disappearing known browser processes must fail closed.
        argv = (entry / "cmdline").read_bytes().decode(errors="surrogateescape").split("\0")
        if argv and argv[-1] == "":
            argv.pop()
        if not argv or not argv[0]:
            raise ValueError("empty cefbrowser command line")
        if any(arg == "--type" or arg.startswith("--type=") for arg in argv[1:]):
            continue
        status = (entry / "status").read_text()
        parent = next(int(line.split()[1]) for line in status.splitlines()
                      if line.startswith("PPid:"))
        processes[int(entry.name)] = (parent, argv, entry)
    roots = [pid for pid, (parent, _, _) in processes.items() if parent not in processes]
    adopted = [pid for pid in roots if processes[pid][0] == 1]
    candidates = adopted or roots
    if len(candidates) != 1:
        raise ValueError("no unique cefbrowser main process (without --type)")
    pid = candidates[0]
    _, argv, entry = processes[pid]
    value = static_argument(argv)
    if value is None:
        root = (entry / "exe").resolve(strict=True).parent
    else:
        root = Path(value)
        if not posixpath.isabs(value):
            root = (entry / "cwd").resolve(strict=True) / root
    return pid, root


def main():
    try:
        pid, root = detect()
        print(f"CEFBROWSER_MAIN_PID={pid}", file=sys.stderr)
        print(root)
        return 0
    except (OSError, ValueError, StopIteration) as error:
        print(f"STOP: cefbrowser static root detection failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
