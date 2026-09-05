#!/usr/bin/env python3
"""Recover the working-tree version of a file from this session's transcript.

A blanket `git checkout -- .` (run to normalise CRLF line endings) also reverted
the unstaged edits to docs/AMARIAN_PROTOCOL.md. Unstaged changes are not git
objects, so git cannot restore them — but the transcript records every Read and
Edit the assistant performed, and a Read result carries the whole file with
`cat -n` style line numbers. This extracts the candidates so the newest complete
copy can be identified and written back.

Usage: recover_from_transcript.py <transcript.jsonl> <needle> <outdir>
"""

import json
import pathlib
import sys


def iter_text_blocks(path: pathlib.Path):
    """Yield (line_no, role, kind, text) for every text-bearing block."""
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for lineno, raw in enumerate(handle, start=1):
            raw = raw.strip()
            if not raw:
                continue
            try:
                record = json.loads(raw)
            except json.JSONDecodeError:
                continue

            message = record.get("message")
            if not isinstance(message, dict):
                continue
            role = message.get("role", "?")
            content = message.get("content")
            if isinstance(content, str):
                yield lineno, role, "text", content
                continue
            if not isinstance(content, list):
                continue

            for block in content:
                if not isinstance(block, dict):
                    continue
                kind = block.get("type")
                if kind == "text":
                    yield lineno, role, "text", block.get("text", "")
                elif kind == "tool_result":
                    payload = block.get("content")
                    if isinstance(payload, str):
                        yield lineno, role, "tool_result", payload
                    elif isinstance(payload, list):
                        for part in payload:
                            if isinstance(part, dict) and part.get("type") == "text":
                                yield lineno, role, "tool_result", part.get("text", "")
                elif kind == "tool_use":
                    yield lineno, role, "tool_use", json.dumps(block.get("input", {}))


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    transcript = pathlib.Path(sys.argv[1])
    needle = sys.argv[2]
    outdir = pathlib.Path(sys.argv[3])
    outdir.mkdir(parents=True, exist_ok=True)

    found = 0
    for lineno, role, kind, text in iter_text_blocks(transcript):
        if needle not in text:
            continue
        found += 1
        target = outdir / f"{lineno:06d}_{role}_{kind}_{found:02d}.txt"
        target.write_text(text, encoding="utf-8")
        print(f"{target.name}  {len(text):>8} chars  {len(text.splitlines()):>5} lines")

    print(f"--- {found} block(s) containing {needle!r}")
    return 0 if found else 1


if __name__ == "__main__":
    raise SystemExit(main())
