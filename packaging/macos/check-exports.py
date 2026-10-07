#!/usr/bin/env python3
"""Reject a Mach-O GPU library whose executable cannot resolve its host imports."""
import argparse
from pathlib import Path
import re
import subprocess
import sys


def output(*command):
    return subprocess.check_output(command, text=True, stderr=subprocess.PIPE)


def exports(path):
    listing = output("dyld_info", "-arch", "x86_64", "-exports", str(path))
    return set(re.findall(r"^\s+0x[0-9A-Fa-f]+\s+(\S+)", listing, re.MULTILINE))


def dependencies(path):
    listing = output("otool", "-arch", "x86_64", "-L", str(path))
    return re.findall(r"^\s+(.+?) \(compatibility version", listing, re.MULTILINE)


def rpaths(path, executable):
    listing = output("otool", "-arch", "x86_64", "-l", str(path))
    paths = re.findall(r"cmd LC_RPATH\s+cmdsize \d+\s+path (.+?) \(offset", listing)
    return [p.replace("@loader_path", str(path.parent)).replace(
        "@executable_path", str(executable.parent)) for p in paths]


def check(library, executable):
    # Mach-O records dependency-bound undefined symbols separately from dynamic_lookup.
    listing = output("nm", "-arch", "x86_64", "-m", str(library))
    required = set(re.findall(
        r"\(undefined\) (?:weak )?external (\S+) \(dynamically looked up\)", listing))
    available = exports(executable)
    missing = required - available
    if missing:
        search = rpaths(executable, executable) + rpaths(library, executable)
        seen = {library, executable}
        pending = [executable, library]
        while pending and missing:
            image = pending.pop()
            image_search = rpaths(image, executable) + search
            for dependency in dependencies(image):
                expanded = dependency.replace("@loader_path", str(image.parent)).replace(
                    "@executable_path", str(executable.parent))
                candidates = ([Path(p) / expanded.removeprefix("@rpath/") for p in image_search]
                              if expanded.startswith("@rpath/") else [Path(expanded)])
                resolved = next((p.resolve() for p in candidates if p.is_file()), None)
                if resolved is None or resolved in seen:
                    continue
                seen.add(resolved)
                available |= exports(resolved)
                missing = required - available
                pending.append(resolved)
    if missing:
        print(f"Darwin GPU host imports unresolved by {executable}:", file=sys.stderr)
        for symbol in sorted(missing):
            print(f"  {symbol}", file=sys.stderr)
        return 1
    print(f"Darwin GPU host imports verified: {executable.name} ({len(required)} symbols)")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path)
    parser.add_argument("executable", type=Path)
    args = parser.parse_args()
    try:
        return check(args.library.resolve(), args.executable.resolve())
    except subprocess.CalledProcessError as error:
        print(f"Darwin export inspection failed: {' '.join(error.cmd)}", file=sys.stderr)
        print(error.stderr, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
