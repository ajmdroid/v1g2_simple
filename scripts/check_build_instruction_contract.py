#!/usr/bin/env python3
"""Validate tracked build and deploy instructions stay aligned with the repo policy."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DOC_PATHS = [ROOT / "README.md", *sorted((ROOT / "docs").glob("*.md"))]
MANUAL_PATH = ROOT / "docs" / "MANUAL.md"
README_PATH = ROOT / "README.md"
DEPLOY_SCRIPT_PATH = ROOT / "interface" / "scripts" / "deploy.js"
BOGEYS_TOOL_PATH = ROOT / "tools" / "generate_bogeys_audio.sh"
AUTHORITATIVE_UPLOAD_POLICY = (
    "Authoritative filesystem upload path: `./build.sh --upload-fs` or `./build.sh --all`."
)


def main() -> int:
    errors: list[str] = []
    usage_pattern = re.compile(r"\./build\.sh\b[^\n]*\s-n(\s|$)")

    for path in DOC_PATHS:
        if not path.exists():
            continue
        text = path.read_text(encoding="utf-8")
        if usage_pattern.search(text):
            errors.append(f"{path.relative_to(ROOT)} mentions unsupported build.sh -n flag")

    if README_PATH.exists():
        readme_text = README_PATH.read_text(encoding="utf-8")
        if AUTHORITATIVE_UPLOAD_POLICY not in readme_text:
            errors.append("README.md missing authoritative filesystem upload policy")
        if "pio run -e waveshare-349 -t uploadfs" in readme_text:
            errors.append("README.md should not advertise raw uploadfs usage")

    if MANUAL_PATH.exists():
        manual_text = MANUAL_PATH.read_text(encoding="utf-8")
        if AUTHORITATIVE_UPLOAD_POLICY not in manual_text:
            errors.append("docs/MANUAL.md missing authoritative filesystem upload policy")
        if "**Alternative (manual steps):**" in manual_text:
            errors.append("docs/MANUAL.md still advertises alternative manual upload steps")
        if "pio run -e waveshare-349 -t uploadfs" in manual_text:
            errors.append("docs/MANUAL.md should not advertise raw uploadfs as a normal path")

    if DEPLOY_SCRIPT_PATH.exists():
        deploy_text = DEPLOY_SCRIPT_PATH.read_text(encoding="utf-8")
        if "./build.sh --upload-fs" not in deploy_text:
            errors.append("interface/scripts/deploy.js should point next steps at build.sh --upload-fs")
        if "uploadfs" in deploy_text:
            errors.append("interface/scripts/deploy.js should not recommend raw uploadfs commands")

    if BOGEYS_TOOL_PATH.exists():
        bogeys_text = BOGEYS_TOOL_PATH.read_text(encoding="utf-8")
        if 'cp "$OUTPUT_DIR/mulaw/bogeys.mul" "data/audio/' in bogeys_text or \
           'cp "$OUTPUT_DIR/mulaw/bogeys.mul" "../data/audio/' in bogeys_text:
            errors.append("tools/generate_bogeys_audio.sh should not copy directly into data/audio")

    if errors:
        print("[contract] build-docs mismatch:")
        for error in errors:
            print(f"  - {error}")
        return 1

    print("[contract] tracked build docs match supported build.sh usage")
    return 0


if __name__ == "__main__":
    sys.exit(main())
