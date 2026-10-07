#!/usr/bin/env python3
"""Install the pinned native macOS Python/Tk runtime for the launcher."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

ARCHIVE = 'cpython-3.14.8+20261003-aarch64-apple-darwin-install_only_stripped.tar.gz'
URL = 'https://github.com/astral-sh/python-build-standalone/releases/download/20261003/' + ARCHIVE
SHA256 = 'ed14e9042f36c0aa18383f5d9729bcbf17847e842666a793b5a5a5d8505c0ca1'


def verify(prefix):
    subprocess.run([str(prefix / 'bin/python3'), '-c',
        'import platform,tkinter; assert platform.machine()=="arm64"; '
        'print("Native Python/Tk verified:",platform.python_version(),tkinter.TkVersion)'],
        check=True, stdout=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix', type=Path, default=Path.home() / 'bbport-deps/python-arm64')
    parser.add_argument('--archive', type=Path, help='Use an already downloaded archive; its hash is checked.')
    args = parser.parse_args()
    if platform.system() != 'Darwin':
        parser.error('This runtime requires Apple Silicon macOS.')
    prefix = args.prefix.expanduser().resolve()
    receipt = prefix / 'bbport-runtime.json'
    identity = {'archive': ARCHIVE, 'sha256': SHA256, 'url': URL, 'architecture': 'arm64'}
    if prefix.exists():
        if not receipt.is_file() or json.loads(receipt.read_text()) != identity:
            parser.error(f'Existing runtime has no matching pinned receipt: {prefix}')
        verify(prefix)
        return
    prefix.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='python-stage-', dir=prefix.parent) as scratch:
        stage = Path(scratch)
        archive = args.archive
        if archive is None:
            archive = stage / ARCHIVE
            with urllib.request.urlopen(URL) as response, archive.open('wb') as output:
                shutil.copyfileobj(response, output)
        with archive.open('rb') as source:
            digest = hashlib.file_digest(source, 'sha256').hexdigest()
        if digest != SHA256:
            raise SystemExit(f'Python archive SHA-256 mismatch: {digest}; expected {SHA256}')
        with tarfile.open(archive) as distribution:
            distribution.extractall(stage, filter='data')
        runtime = stage / 'python'
        verify(runtime)
        (runtime / 'bbport-runtime.json').write_text(json.dumps(identity, indent=2) + '\n')
        runtime.rename(prefix)
    print(f'Installed {prefix}')


if __name__ == '__main__':
    main()
