#!/usr/bin/env python3
"""Extract git SHA for build-time injection into firmware."""
import subprocess
import sys

try:
    sha = subprocess.check_output(
        ['git', 'rev-parse', '--short=7', 'HEAD'],
        stderr=subprocess.DEVNULL
    ).decode().strip()
    print(f'-D GIT_SHA=\\"{sha}\\"')
except Exception:
    # Git not available or not a git repo
    print('-D GIT_SHA=\\"unknown\\"')
    sys.exit(0)  # Don't fail build
