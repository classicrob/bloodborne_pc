#!/bin/bash
# Interleave two existing pinned build manifests; no build, checkout, copying or deletion.
exec python3 "$(dirname -- "$0")/replay_ab.py" "$@"
