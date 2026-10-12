#!/usr/bin/env python3
"""Checks the `flags |= ... GpuModified` spelling: only the allowed functions use it.

TextureCache::MarkGpuWritten is meant to be the one way to make an image GPU-authoritative after
a GPU write recorded outside the binding paths: it also tracks the image's pages for guest CPU
writes. An image marked GpuModified without tracking is trusted by write-backs
(SynchronizeMemoryFromImage) while guest CPU writes to its memory go unnoticed; that is the
candidate mechanism of the PREVIEW-20 float-descriptor stops (the PREVIEW-21f A/B decides).
The binding paths go through UpdateImage, which tracks first; the runtime's copies propagate the
source's flag inside texture-cache merges, whose new images are tracked when registered.
A guard for that spelling only, not a proof of every state transition (another spelling, or an
assignment of a whole flags value, is not seen).
Exit 0: no matching setter outside the allowed functions. Exit 1: names each one.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent / 'gpu' / 'shadps4' / 'video_core'
ALLOWED = {
    ('texture_cache.cpp', 'FindTexture'),
    ('texture_cache.cpp', 'FindRenderTarget'),
    ('texture_cache.cpp', 'FindDepthTarget'),
    ('texture_cache.cpp', 'MarkGpuWritten'),
    ('vk_runtime.cpp', 'CopyImage'),
    ('vk_runtime.cpp', 'CopyImageWithBuffer'),
    ('vk_runtime.cpp', 'CopyMip'),
}
SETTER = re.compile(r'flags\s*\|=\s*[^;]*GpuModified')
FUNCTION = re.compile(r'^[A-Za-z][^;(]*?(?:::)?(\w+)\s*\([^;]*$')


def main():
    bad = []
    found = set()
    for path in sorted(ROOT.rglob('*')):
        if path.suffix not in ('.cpp', '.h'):
            continue
        function = None
        for number, line in enumerate(path.read_text(errors='replace').splitlines(), 1):
            match = FUNCTION.match(line)
            if match:
                function = match.group(1)
            if SETTER.search(line):
                key = (path.name, function)
                found.add(key)
                if key not in ALLOWED:
                    bad.append(f'{path.relative_to(ROOT)}:{number} in {function}: {line.strip()}')
    for line in bad:
        print('GpuModified set outside TextureCache::MarkGpuWritten and the binding paths:', line)
    missing = {f for f in ALLOWED if f[1] == 'MarkGpuWritten'} - found
    for name in missing:
        print('MarkGpuWritten no longer sets GpuModified:', name)
    print('gpu-modified-setters:', 'FAIL' if bad or missing else 'PASS')
    return 1 if bad or missing else 0


if __name__ == '__main__':
    sys.exit(main())
