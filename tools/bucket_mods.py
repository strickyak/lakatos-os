#!/usr/bin/env python3
"""Hash playable MOD basenames into 36 buckets (0-9, a-z) under /tmp/net-modfiles."""

import collections
import hashlib
import os
import shutil

BUCKETS = "0123456789abcdefghijklmnopqrstuvwxyz"
SRC_LIST = "/tmp/playable.txt"
DST_ROOT = "/tmp/net-modfiles"


def bucket_for_name(basename: str) -> str:
    digest = hashlib.sha256(basename.encode("utf-8")).digest()
    idx = int.from_bytes(digest[:4], "big") % len(BUCKETS)
    return BUCKETS[idx]


def main() -> None:
    if os.path.exists(DST_ROOT):
        shutil.rmtree(DST_ROOT)
    for b in BUCKETS:
        os.makedirs(os.path.join(DST_ROOT, b), exist_ok=True)

    counts = collections.Counter()
    seen_dst = set()
    collisions = 0
    total = 0

    with open(SRC_LIST, "r", encoding="utf-8") as f:
        for raw in f:
            src = raw.strip()
            if not src:
                continue
            base = os.path.basename(src)
            b = bucket_for_name(base)
            dst = os.path.join(DST_ROOT, b, base)
            if dst in seen_dst:
                collisions += 1
                continue
            seen_dst.add(dst)
            shutil.copy2(src, dst)
            counts[b] += 1
            total += 1

    vals = [counts[b] for b in BUCKETS]
    print(f"Copied {total} files into {len(BUCKETS)} buckets under {DST_ROOT} (collisions={collisions})")
    print(f"Bucket sizes: min={min(vals)}, max={max(vals)}, avg={total / len(BUCKETS):.1f}")
    for b in BUCKETS:
        print(f"  {b}: {counts[b]}")


if __name__ == "__main__":
    main()
