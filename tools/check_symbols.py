#!/usr/bin/env python3
"""Offline verifier for app/src/main/cpp/SAMPMOBILE/Game/Engine/Symbols.def.

Checks every declared symbol against both arm64-v8a and armeabi-v7a libGTASA.so
exports via llvm-nm, detects duplicate names/symbols, and optionally audits the
codebase for remaining legacy string-based symbol lookups or raw Abs(0x...) hex.

Usage:
  python tools/check_symbols.py
  python tools/check_symbols.py --audit
  python tools/check_symbols.py --strict-audit
"""

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SYMBOLS_DEF = os.path.join(REPO, "app", "src", "main", "cpp", "SAMPMOBILE", "Game", "Engine", "Symbols.def")
SO_ARM64 = os.path.join(REPO, "app", "src", "main", "jniLibs", "arm64-v8a", "libGTASA.so")
SO_ARM32 = os.path.join(REPO, "app", "src", "main", "jniLibs", "armeabi-v7a", "libGTASA.so")
SRC_ROOT = os.path.join(REPO, "app", "src", "main", "cpp", "SAMPMOBILE")


def find_nm(explicit_nm: str | None) -> str:
    if explicit_nm:
        return explicit_nm
    for cand in ("llvm-nm", "llvm-nm.exe", "nm"):
        found = shutil.which(cand)
        if found:
            return found
    local_app = os.environ.get("LOCALAPPDATA", "")
    if local_app:
        pattern = os.path.join(
            local_app, "Android", "Sdk", "ndk", "*", "toolchains", "llvm", "prebuilt", "*", "bin", "llvm-nm*"
        )
        matches = sorted(glob.glob(pattern))
        if matches:
            return matches[-1]
    raise RuntimeError("llvm-nm not found; pass --nm <path>")


def load_exports(nm_bin: str, so_path: str) -> dict[str, str]:
    out = subprocess.check_output([nm_bin, "-D", "--defined-only", so_path], text=True, errors="ignore")
    exports: dict[str, str] = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            exports[parts[2]] = parts[1]
    return exports


def parse_symbols_def(path: str):
    pat_data = re.compile(r'^\s*SYM_DATA\(\s*(\w+)\s*,\s*"([^"]+)"\s*,\s*([^,]+?)\s*,\s*(Required|Optional)\s*\)')
    pat_fn = re.compile(r'^\s*SYM_FN\(\s*(\w+)\s*,\s*"([^"]+)"\s*,\s*([^,]+?)\s*,\s*(Required|Optional)\s*\)')
    pat_abi = re.compile(
        r'^\s*SYM_FN_ABI\(\s*(\w+)\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*([^,]+?)\s*,\s*(Required|Optional)\s*\)'
    )
    entries = []
    with open(path, "r", encoding="utf-8") as fh:
        for lineno, line in enumerate(fh, 1):
            m = pat_data.match(line)
            if m:
                entries.append(("DATA", m.group(1), m.group(2), m.group(2), m.group(4), lineno))
                continue
            m = pat_fn.match(line)
            if m:
                entries.append(("FN", m.group(1), m.group(2), m.group(2), m.group(4), lineno))
                continue
            m = pat_abi.match(line)
            if m:
                entries.append(("FN_ABI", m.group(1), m.group(2), m.group(3), m.group(5), lineno))
                continue
    return entries


def audit_legacy_calls() -> list[tuple[str, int, str]]:
    pat_legacy = re.compile(
        r'(?:callFunction|CallFunction|GetSymbol|GetVmtFunction|Memory::hook)\s*(?:<[^>]*>)?\s*\(\s*"'
        r'|\bHOOK\s*\(\s*"'
        r'|\bAbs\s*(?:<[^>]*>)?\s*\([^)]*0x[0-9A-Fa-f]+'
    )
    hits = []
    for dp, _, files in os.walk(SRC_ROOT):
        rel_dir = os.path.relpath(dp, SRC_ROOT)
        parts = rel_dir.split(os.sep)
        if parts[0] in ("Deps", "Memory"):
            continue
        for f in sorted(files):
            if not f.endswith((".cpp", ".h")):
                continue
            full = os.path.join(dp, f)
            rel = os.path.relpath(full, SRC_ROOT)
            if rel.startswith(os.path.join("Game", "Engine")):
                continue
            in_block_comment = False
            with open(full, "r", encoding="utf-8", errors="ignore") as fh:
                for lineno, line in enumerate(fh, 1):
                    s = line.strip()
                    if in_block_comment:
                        if "*/" in s:
                            in_block_comment = False
                        continue
                    if s.startswith("/*"):
                        if "*/" not in s:
                            in_block_comment = True
                        continue
                    if s.startswith("//"):
                        continue
                    # Strip trailing // comment before matching
                    code = line.split("//", 1)[0]
                    if pat_legacy.search(code):
                        hits.append((rel, lineno, s))
    return hits


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--nm", help="Explicit path to llvm-nm")
    ap.add_argument("--audit", action="store_true", help="Report remaining legacy string lookups / raw Abs(0x...)")
    ap.add_argument("--strict-audit", action="store_true", help="Fail if any legacy lookup or raw Abs(0x...) remains")
    args = ap.parse_args()

    entries = parse_symbols_def(SYMBOLS_DEF)
    if not entries:
        print(f"ERROR: no symbols parsed from {SYMBOLS_DEF}")
        return 1

    errors = 0
    seen_ids: dict[str, int] = {}
    seen_m64: dict[str, str] = {}
    seen_m32: dict[str, str] = {}
    for kind, ident, m64, m32, policy, lineno in entries:
        if ident in seen_ids:
            print(f"ERROR: duplicate id '{ident}' at line {lineno} (first at line {seen_ids[ident]})")
            errors += 1
        seen_ids[ident] = lineno
        if m64 in seen_m64:
            print(f"ERROR: duplicate arm64 symbol '{m64}' ({ident} vs {seen_m64[m64]})")
            errors += 1
        seen_m64[m64] = ident
        if m32 in seen_m32:
            print(f"ERROR: duplicate arm32 symbol '{m32}' ({ident} vs {seen_m32[m32]})")
            errors += 1
        seen_m32[m32] = ident

    nm_bin = find_nm(args.nm)
    for abi, so_path, idx in (("arm64-v8a", SO_ARM64, 2), ("armeabi-v7a", SO_ARM32, 3)):
        if not os.path.exists(so_path):
            print(f"WARN: {abi} libGTASA.so not found at {so_path}; skipping {abi} export check")
            continue
        exports = load_exports(nm_bin, so_path)
        missing = 0
        type_mismatch = 0
        for kind, ident, m64, m32, policy, lineno in entries:
            sym = m64 if idx == 2 else m32
            sym_type = exports.get(sym)
            if not sym_type:
                print(f"MISSING [{abi}] line {lineno}: {ident} -> {sym} ({policy})")
                if policy == "Required":
                    missing += 1
            else:
                is_fn = sym_type in ("T", "W", "t", "w")
                if kind == "DATA" and is_fn:
                    print(f"TYPE MISMATCH [{abi}] line {lineno}: {ident} declared DATA but nm='{sym_type}'")
                    type_mismatch += 1
                elif kind in ("FN", "FN_ABI") and not is_fn:
                    print(f"TYPE MISMATCH [{abi}] line {lineno}: {ident} declared {kind} but nm='{sym_type}'")
                    type_mismatch += 1
        print(f"CHECK [{abi}]: {len(entries)} symbols checked against {len(exports)} exports -> missing={missing}, type_mismatch={type_mismatch}")
        errors += missing + type_mismatch

    if args.audit or args.strict_audit:
        hits = audit_legacy_calls()
        print(f"AUDIT: {len(hits)} remaining legacy string lookups / raw Abs(0x...)")
        for rel, ln, text in hits[:40]:
            print(f"  {rel}:{ln}: {text}")
        if len(hits) > 40:
            print(f"  ... ({len(hits) - 40} more)")
        if args.strict_audit and hits:
            errors += len(hits)

    return 0 if errors == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
