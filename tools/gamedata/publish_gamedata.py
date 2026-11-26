#!/usr/bin/env python3
"""
Packages local game data into downloadable chunks + a manifest for the client, optionally uploading straight to Cloudflare R2.

Usage:
    # Only generate output into dist/cdn/
    python tools/gamedata/publish_gamedata.py --game dist/gamedata/files

    # Generate, then upload to R2
    python tools/gamedata/publish_gamedata.py --game dist/gamedata/files --upload

    # Specify the game data and online package separately
    python tools/gamedata/publish_gamedata.py \\
        --game dist/gamedata/files \\
        --online dist/samp_online/files \\
        --version 2

Output structure (dist/cdn/):
    manifest.json
    chunks/<packageId>/0000.bin, 0001.bin, ...

Design notes:
- Zip first (stored, not compressed — just a container), then split into chunks. That way the client reassembles a standard zip,
  and the extraction logic never needs to know a custom format.
- 16 MB per chunk: balances concurrency against per-chunk retransmission cost. R2 supports Range, so resume works per chunk.
- Both chunks and the whole package get a SHA-256; the client verifies at both levels.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sys
import zipfile
from pathlib import Path

CHUNK_SIZE = 16 * 1024 * 1024  # 16MB

# Chunk size may be overridden via environment variable, for offline self-checks only (do not change for real-device releases).
_env_chunk = os.environ.get("SAMP_GAMEDATA_CHUNK_SIZE")
if _env_chunk:
    CHUNK_SIZE = int(_env_chunk)
MANIFEST_SCHEMA = 1


def human(n: int) -> str:
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if n < 1024:
            return f"{n:.1f} {unit}" if unit != "B" else f"{n} B"
        n /= 1024.0
    return f"{n:.1f} PB"


def sha256_file(path: Path, bufsize: int = 1024 * 1024) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            block = f.read(bufsize)
            if not block:
                break
            h.update(block)
    return h.hexdigest()


def make_zip(src_dir: Path, out_zip: Path) -> Path:
    """Zip a directory (stored, not compressed — game data is mostly already-compressed formats, so compressing gains nothing)."""
    if not src_dir.is_dir():
        raise SystemExit(f"Directory does not exist: {src_dir}")

    out_zip.parent.mkdir(parents=True, exist_ok=True)
    if out_zip.exists():
        out_zip.unlink()

    count = 0
    total = 0
    with zipfile.ZipFile(out_zip, "w", zipfile.ZIP_STORED, allowZip64=True) as zf:
        for root, _dirs, files in os.walk(src_dir):
            for name in files:
                full = Path(root) / name
                rel = full.relative_to(src_dir).as_posix()
                # Skip hidden files and macOS junk
                if rel.startswith(".") or rel.startswith("__MACOSX") or name == ".DS_Store":
                    continue
                zf.write(full, rel)
                count += 1
                total += full.stat().st_size
                if count % 2000 == 0:
                    print(f"  Packaging… {count} files / {human(total)}", flush=True)

    print(f"  Done: {count} files / {human(total)} -> {out_zip} ({human(out_zip.stat().st_size)})")
    return out_zip


def split_zip(zip_path: Path, out_dir: Path, package_id: str) -> list[dict]:
    """Split a zip into fixed-size chunks; returns the list of chunk descriptors."""
    chunk_dir = out_dir / "chunks" / package_id
    if chunk_dir.exists():
        shutil.rmtree(chunk_dir)
    chunk_dir.mkdir(parents=True, exist_ok=True)

    chunks = []
    index = 0
    with zip_path.open("rb") as f:
        while True:
            block = f.read(CHUNK_SIZE)
            if not block:
                break
            name = f"{index:04d}.bin"
            target = chunk_dir / name
            target.write_bytes(block)
            chunks.append({
                "index": index,
                "url": f"chunks/{package_id}/{name}",
                "bytes": len(block),
                "sha256": hashlib.sha256(block).hexdigest(),
            })
            print(f"  Chunk {name}: {human(len(block))}", flush=True)
            index += 1

    if not chunks:
        raise SystemExit(f"zip is empty: {zip_path}")
    return chunks


def build_package(src_dir: Path, out_dir: Path, work_dir: Path,
                  package_id: str, label: str) -> dict:
    print(f"[Package {package_id}]")
    zip_path = work_dir / f"{package_id}.zip"
    make_zip(src_dir, zip_path)

    total = zip_path.stat().st_size
    digest = sha256_file(zip_path)
    print(f"  Whole-package SHA-256: {digest}")

    chunks = split_zip(zip_path, out_dir, package_id)

    # Check: sum of chunk sizes == whole-package size, otherwise the client will reject it
    assert sum(c["bytes"] for c in chunks) == total, "Sum of chunk sizes does not equal the whole package"

    return {
        "id": package_id,
        "label": label,
        "bytes": total,
        "sha256": digest,
        "chunks": chunks,
    }


def upload_r2(out_dir: Path, bucket: str, account_id: str, access_key: str,
              secret_key: str) -> None:
    """Upload to R2 with boto3 (S3-compatible). Prompts to install boto3 if it is missing."""
    try:
        import boto3
        from botocore.config import Config
    except ImportError:
        raise SystemExit(
            "Uploading requires boto3: pip install boto3\n"
            "(To skip uploading, drop --upload; the output in dist/cdn/ can be uploaded manually)"
        )

    endpoint = f"https://{account_id}.r2.cloudflarestorage.com"
    s3 = boto3.client(
        "s3",
        endpoint_url=endpoint,
        aws_access_key_id=access_key,
        aws_secret_access_key=secret_key,
        config=Config(signature_version="s3v4"),
        region_name="auto",
    )

    files = sorted(p for p in out_dir.rglob("*") if p.is_file())
    total = sum(p.stat().st_size for p in files)
    print(f"\nUploading {len(files)} files / {human(total)} -> r2://{bucket}")

    done = 0
    for path in files:
        key = path.relative_to(out_dir).as_posix()
        extra = {}
        if key.endswith(".json"):
            extra = {"ContentType": "application/json", "CacheControl": "no-cache"}
        else:
            # Chunk contents never change: long cache. Resume depends on Range (natively supported by R2)
            extra = {"ContentType": "application/octet-stream",
                     "CacheControl": "public, max-age=31536000, immutable"}
        s3.upload_file(str(path), bucket, key, ExtraArgs=extra)
        done += path.stat().st_size
        print(f"  {key}  ({done * 100 // max(total,1)}%)", flush=True)

    print("Upload complete")


def main() -> int:
    ap = argparse.ArgumentParser(description="Package and publish the game data package")
    ap.add_argument("--game", help="Game data directory (packaged as the game-data package)")
    ap.add_argument("--online", help="Online data directory (packaged as the online-data package)")
    ap.add_argument("--version", default="1", help="Data version; the client uses it to decide whether to re-download")
    ap.add_argument("--out", default="dist/cdn", help="Output directory (default dist/cdn)")
    ap.add_argument("--work", default="dist/cdn-work", help="Temporary zip directory")
    ap.add_argument("--min-client", type=int, default=0,
                    help="Minimum required client versionCode (0 = no limit)")
    ap.add_argument("--upload", action="store_true", help="Upload to Cloudflare R2")
    ap.add_argument("--bucket", default=os.environ.get("R2_BUCKET", ""))
    ap.add_argument("--account-id", default=os.environ.get("R2_ACCOUNT_ID", ""))
    ap.add_argument("--access-key", default=os.environ.get("R2_ACCESS_KEY_ID", ""))
    ap.add_argument("--secret-key", default=os.environ.get("R2_SECRET_ACCESS_KEY", ""))
    args = ap.parse_args()

    if not args.game and not args.online:
        ap.error("Provide at least one of --game or --online")

    out_dir = Path(args.out)
    work_dir = Path(args.work)
    out_dir.mkdir(parents=True, exist_ok=True)
    work_dir.mkdir(parents=True, exist_ok=True)

    manifest: dict = {
        "schema": MANIFEST_SCHEMA,
        "version": str(args.version),
    }
    if args.min_client > 0:
        manifest["minClientVersionCode"] = args.min_client

    if args.game:
        pkg = build_package(Path(args.game), out_dir, work_dir, "game-data", "Game data")
        manifest["game"] = {
            "label": "GTA:SA game data",
            "totalBytes": pkg["bytes"],
            "required": True,
            "packages": [pkg],
        }

    if args.online:
        pkg = build_package(Path(args.online), out_dir, work_dir, "online-data", "Online data package")
        manifest["online"] = {
            "label": "SA-MP online data",
            "totalBytes": pkg["bytes"],
            "required": True,
            "packages": [pkg],
        }

    manifest_path = out_dir / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")

    total = sum(g["totalBytes"] for g in manifest.values() if isinstance(g, dict) and "totalBytes" in g)
    print(f"\nManifest generated: {manifest_path}  (total {human(total)})")

    if args.upload:
        missing = [k for k, v in {
            "bucket": args.bucket, "account-id": args.account_id,
            "access-key": args.access_key, "secret-key": args.secret_key,
        }.items() if not v]
        if missing:
            raise SystemExit("Missing upload parameters: " + ", ".join(missing)
                             + "\nAvailable via environment variables R2_BUCKET / R2_ACCOUNT_ID / "
                               "R2_ACCESS_KEY_ID / R2_SECRET_ACCESS_KEY")
        upload_r2(out_dir, args.bucket, args.account_id, args.access_key, args.secret_key)
    else:
        print("Not uploaded. You can drag the output straight into the R2 console, or add --upload to upload automatically.")
        print(f"After uploading, configure the client with this URL:")
        print(f"  SAMP_GAMEDATA_MANIFEST=https://<your-domain>/manifest.json")

    return 0


if __name__ == "__main__":
    sys.exit(main())
