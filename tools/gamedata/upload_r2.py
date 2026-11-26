#!/usr/bin/env python3
"""
Only uploads already-packaged output to Cloudflare R2 (does not re-package).

Packaging (publish_gamedata.py) and uploading are separate because packaging 2 GB takes several minutes,
while an upload may be retried many times after network interruptions — it should not be re-packaged every time.

    python tools/gamedata/upload_r2.py --account-id XXX --bucket samp-mobile
    # Credentials are read from environment variables, so they never land in shell history:
    set R2_ACCESS_KEY_ID=...        # Windows
    set R2_SECRET_ACCESS_KEY=...
    python tools/gamedata/upload_r2.py --account-id XXX --bucket samp-mobile

Features:
- Objects that already exist with a matching size are skipped (an R2 HeadObject is far cheaper than an upload)
- A failed single file retries 3 times, with exponential backoff
- Uploads sorted by size: small files first (so the manifest is usable as early as possible), large chunks follow
- After uploading, a sampled HEAD check confirms the remote Content-Length matches the local one
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path


def human(n: int) -> str:
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if n < 1024:
            return f"{n:.2f} {unit}" if unit != "B" else f"{n} B"
        n /= 1024.0
    return f"{n:.2f} PB"


def make_client(account_id: str, access_key: str, secret_key: str):
    try:
        import boto3
        from botocore.config import Config
    except ImportError:
        raise SystemExit("boto3 is required: python -m pip install boto3")

    from boto3.s3.transfer import TransferConfig

    return boto3.client(
        "s3",
        endpoint_url=f"https://{account_id}.r2.cloudflarestorage.com",
        aws_access_key_id=access_key,
        aws_secret_access_key=secret_key,
        region_name="auto",
        config=Config(
            signature_version="s3v4",
            retries={"max_attempts": 3, "mode": "standard"},
            max_pool_connections=8,
        ),
    ), TransferConfig(
        # 16 MB chunks: if one block fails, only that block is re-uploaded, not the whole file.
        # A 2 GB file is about 135 blocks; with concurrency 4, even cross-border links can saturate.
        multipart_threshold=16 * 1024 * 1024,
        multipart_chunksize=16 * 1024 * 1024,
        max_concurrency=4,
        use_threads=True,
    )


def remote_size(client, bucket: str, key: str) -> int | None:
    try:
        resp = client.head_object(Bucket=bucket, Key=key)
        return resp["ContentLength"]
    except Exception:
        return None


def upload_one(client, bucket: str, path: Path, key: str, xfer) -> bool:
    """Upload a single file, retrying up to 3 times on failure."""
    size = path.stat().st_size

    existing = remote_size(client, bucket, key)
    if existing == size:
        print(f"  Skipped (already exists with matching size): {key}")
        return True

    extra = {"ContentType": "application/json", "CacheControl": "no-cache"} \
        if key.endswith(".json") else \
        {"ContentType": "application/octet-stream",
         "CacheControl": "public, max-age=31536000, immutable"}

    for attempt in range(1, 4):
        try:
            client.upload_file(str(path), bucket, key, ExtraArgs=extra, Config=xfer)
            return True
        except Exception as e:
            print(f"  Attempt {attempt} failed for {key}: {e}")
            if attempt == 3:
                return False
            time.sleep(2 ** attempt)
    return False


def main() -> int:
    ap = argparse.ArgumentParser(description="Upload the data package to Cloudflare R2")
    ap.add_argument("--dir", default="dist/cdn", help="Output directory (default dist/cdn)")
    ap.add_argument("--account-id", default=os.environ.get("R2_ACCOUNT_ID", ""))
    ap.add_argument("--bucket", default=os.environ.get("R2_BUCKET", ""))
    ap.add_argument("--access-key", default=os.environ.get("R2_ACCESS_KEY_ID", ""))
    ap.add_argument("--secret-key", default=os.environ.get("R2_SECRET_ACCESS_KEY", ""))
    ap.add_argument("--sample-check", type=int, default=5,
                    help="Number of objects to sample-check after upload (0 = no check)")
    args = ap.parse_args()

    root = Path(args.dir)
    if not (root / "manifest.json").is_file():
        raise SystemExit(f"{root} has no manifest.json; run publish_gamedata.py first")

    missing = [n for n, v in {
        "account-id": args.account_id, "bucket": args.bucket,
        "access-key": args.access_key, "secret-key": args.secret_key,
    }.items() if not v]
    if missing:
        raise SystemExit(
            "Missing parameters: " + ", ".join(missing) + "\n"
            "Pass credentials via environment variables (they stay out of shell history):\n"
            "  export R2_ACCESS_KEY_ID=...\n"
            "  export R2_SECRET_ACCESS_KEY=...\n"
            "Create credentials in the Cloudflare dashboard -> R2 -> Manage R2 API Tokens,\n"
            "with Object Read & Write permission, scoped to the target bucket.")

    client, xfer = make_client(args.account_id, args.access_key, args.secret_key)

    files = [p for p in root.rglob("*") if p.is_file()]
    # The manifest goes first (small, and the client uses it to decide what to download); the rest go in ascending size order
    files.sort(key=lambda p: (0 if p.name == "manifest.json" else 1, p.stat().st_size))

    total = sum(p.stat().st_size for p in files)
    print(f"Preparing to upload {len(files)} files / {human(total)} -> r2://{args.bucket}")

    done_bytes = 0
    failed: list[str] = []
    started = time.time()

    for i, path in enumerate(files, 1):
        key = path.relative_to(root).as_posix()
        size = path.stat().st_size
        ok = upload_one(client, args.bucket, path, key, xfer)
        if not ok:
            failed.append(key)
        done_bytes += size

        elapsed = max(time.time() - started, 0.001)
        speed = done_bytes / elapsed
        eta = (total - done_bytes) / speed if speed > 0 else 0
        print(f"  [{i}/{len(files)}] {key}  "
              f"{human(done_bytes)}/{human(total)}  "
              f"{human(speed)}/s  {int(eta)}s remaining", flush=True)

    if failed:
        print(f"\n{len(failed)} failed:")
        for k in failed:
            print("  " + k)
        return 1

    # Sample check: confirm the remote Content-Length matches the local one
    if args.sample_check > 0:
        print(f"\nSample-checking {min(args.sample_check, len(files))}…")
        step = max(1, len(files) // args.sample_check)
        bad = 0
        for path in files[::step][:args.sample_check]:
            key = path.relative_to(root).as_posix()
            rs = remote_size(client, args.bucket, key)
            ls = path.stat().st_size
            ok = rs == ls
            if not ok:
                bad += 1
            print(f"  {'OK ' if ok else 'BAD'} {key} remote={rs} local={ls}")
        if bad:
            print(f"\n{bad} objects have mismatched sizes")
            return 1

    print(f"\nUpload complete, took {int(time.time() - started)}s")
    print(f"Manifest URL: https://<your-custom-domain>/manifest.json")
    print(f"(If no domain is bound yet, you can test temporarily with R2's public r2.dev URL)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
