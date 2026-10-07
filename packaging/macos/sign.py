#!/usr/bin/env python3
"""Ad-hoc sign and strictly verify a Darwin build output before it can execute."""
import argparse
from pathlib import Path
import subprocess
import sys


def sign(path):
    path = Path(path).resolve(strict=True)
    subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', '--timestamp=none', str(path)],
                   check=True)
    subprocess.run(['/usr/bin/codesign', '--verify', '--strict', str(path)], check=True)
    print(f'Darwin ad-hoc signature verified: {path.name}', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('outputs', nargs='+', type=Path)
    args = parser.parse_args()
    if sys.platform != 'darwin':
        parser.error('signing requires macOS')
    for output in args.outputs:
        sign(output)
