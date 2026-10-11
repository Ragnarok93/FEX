#!/usr/bin/env python3
"""Verify the *effective* target on real FEX compiler commands, not only CMakeCache."""
import argparse
import json
from pathlib import Path
import shlex
import sys

def verify(path: Path, expected_arch: str, expected_tune: str, expected_target: str) -> dict:
    entries = json.loads(path.read_text(encoding="utf-8"))
    selected = 0
    errors = []
    for entry in entries:
        source = entry.get("file", "").replace("\\", "/")
        # Check actual FEX translator and Windows ARM64EC files; ignore subprojects.
        if not ("/FEXCore/Source/" in source or source.startswith("FEXCore/Source/")
                or "/Source/Windows/" in source or source.startswith("Source/Windows/")):
            continue
        if not source.endswith((".c", ".cc", ".cpp", ".cxx")):
            continue
        args = entry.get("arguments") or shlex.split(entry["command"])
        selected += 1
        march = [a.split("=", 1)[1] for a in args if a.startswith("-march=")]
        mtune = [a.split("=", 1)[1] for a in args if a.startswith("-mtune=")]
        mcpu = [a for a in args if a.startswith("-mcpu=")]
        compiler = Path(args[0]).name
        problems = []
        if not march or march[-1] != expected_arch:
            problems.append(f"effective march={march[-1] if march else '<missing>'}")
        if not mtune or mtune[-1] != expected_tune:
            problems.append(f"effective mtune={mtune[-1] if mtune else '<missing>'}")
        if mcpu:
            problems.append(f"unwanted mcpu={mcpu}")
        if expected_target not in compiler and not any(expected_target in x for x in args if x.startswith("--target=")):
            problems.append(f"target compiler={compiler}")
        if problems:
            errors.append(source + ": " + ", ".join(problems))
    if selected < 10:
        raise ValueError(f"only {selected} translator/Windows compilation commands inspected")
    if errors:
        raise ValueError(f"{len(errors)} of {selected} compiler commands fail:\n" + "\n".join(errors[:25]))
    return {"checked_translation_units": selected, "effective_march": expected_arch,
            "effective_mtune": expected_tune, "compiler_target": expected_target, "status": "pass"}

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("compile_commands", type=Path)
    ap.add_argument("--arch", required=True)
    ap.add_argument("--tune", required=True)
    ap.add_argument("--target", required=True)
    args = ap.parse_args()
    try:
        report = verify(args.compile_commands, args.arch, args.tune, args.target)
    except (ValueError, KeyError, OSError) as ex:
        print(f"compiler flag verification FAILED: {ex}", file=sys.stderr)
        return 1
    print(json.dumps(report, sort_keys=True))
    return 0

if __name__ == "__main__":
    sys.exit(main())
