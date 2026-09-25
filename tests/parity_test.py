#!/usr/bin/env python3
"""Compare official stream_llama output with native WebSocket deltas."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--official-output", type=Path, required=True)
    parser.add_argument("--native-output", type=Path, required=True)
    return parser.parse_args()


def official_deltas(path: Path) -> list[str]:
    deltas: list[str] = []
    committed = ""
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.startswith("text="):
            continue
        current = line.removeprefix("text=").strip()
        if current.startswith(committed):
            delta = current[len(committed) :]
            if delta:
                deltas.append(delta)
                committed = current
    if not deltas:
        raise ValueError(f"no official text= lines found in {path}")
    return deltas


def native_deltas(path: Path) -> list[str]:
    deltas: list[str] = []
    saw_final = False
    for line in path.read_text(encoding="utf-8").splitlines():
        try:
            message = json.loads(line)
        except json.JSONDecodeError:
            continue
        if message.get("status") != "success":
            continue
        body = message.get("msg", {})
        delta = body.get("text", "")
        if delta:
            deltas.append(delta)
        saw_final = saw_final or body.get("reset") is True
    if not saw_final:
        raise ValueError(f"native output has no final reset=true message: {path}")
    return deltas


def main() -> int:
    arguments = parse_arguments()
    official = official_deltas(arguments.official_output)
    native = native_deltas(arguments.native_output)
    official_text = "".join(official)
    native_text = "".join(native)

    print(f"Official emissions: {len(official)}")
    print(f"Native emissions: {len(native)}")
    print(f"Official transcript: {official_text}")
    print(f"Native transcript: {native_text}")
    if official_text != native_text:
        print("Parity result: FAIL")
        return 1
    print("Parity result: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
