#!/usr/bin/env python3
"""
Minimal static server with Range support, for verifying resume locally.

The standard library http.server does not support Range (it ignores the Range header and returns a full 200),
and resume depends entirely on 206 Partial Content, so tests must use this server.

    python tools/gamedata/dev_rangeserver.py --root /tmp/cdn --port 8899

Comes with two debug switches for verifying the client's fallback paths:
    --no-range    Pretend Range is unsupported (should trigger the "download from scratch" branch)
    --slow N      Sleep N ms per 64 KB, simulating a slow network
"""

from __future__ import annotations

import argparse
import os
import re
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

RANGE_RE = re.compile(r"bytes=(\d*)-(\d*)")


class Handler(BaseHTTPRequestHandler):
    root: Path
    no_range: bool = False
    slow_ms: int = 0

    def log_message(self, fmt, *args):  # Silent, to avoid flooding the log
        pass

    def _resolve(self) -> Path | None:
        rel = self.path.split("?")[0].lstrip("/")
        if ".." in rel:
            return None
        p = (self.root / rel).resolve()
        try:
            p.relative_to(self.root.resolve())
        except ValueError:
            return None
        return p if p.is_file() else None

    def do_HEAD(self):
        self._send(include_body=False)

    def do_GET(self):
        self._send(include_body=True)

    def _send(self, include_body: bool):
        path = self._resolve()
        if path is None:
            self.send_error(404, "Not Found")
            return

        size = path.stat().st_size
        rng = self.headers.get("Range")
        start, end = 0, size - 1
        partial = False

        if rng and not self.no_range:
            m = RANGE_RE.match(rng.strip())
            if m:
                if m.group(1):
                    start = int(m.group(1))
                if m.group(2):
                    end = min(int(m.group(2)), size - 1)
                if start >= size:
                    self.send_response(416)
                    self.send_header("Content-Range", f"bytes */{size}")
                    self.end_headers()
                    return
                partial = True

        length = end - start + 1
        self.send_response(206 if partial else 200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(length))
        self.send_header("Accept-Ranges", "none" if self.no_range else "bytes")
        if partial:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()

        if not include_body:
            return

        with path.open("rb") as f:
            f.seek(start)
            left = length
            while left > 0:
                block = f.read(min(65536, left))
                if not block:
                    break
                try:
                    self.wfile.write(block)
                except (BrokenPipeError, ConnectionResetError):
                    return  # Client cancelled the download; normal
                left -= len(block)
                if self.slow_ms:
                    time.sleep(self.slow_ms / 1000.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--port", type=int, default=8899)
    ap.add_argument("--no-range", action="store_true")
    ap.add_argument("--slow", type=int, default=0, help="Delay in ms per 64 KB")
    args = ap.parse_args()

    Handler.root = Path(args.root)
    Handler.no_range = args.no_range
    Handler.slow_ms = args.slow

    srv = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    print(f"serving {Handler.root} on 0.0.0.0:{args.port} "
          f"(range={'off' if args.no_range else 'on'}, slow={args.slow}ms)", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
