#!/usr/bin/env python3
"""Naive Python vector-search baseline for the technical report.

This intentionally uses only the standard library. It measures a full linear
scan over a flat `array('f')` or a list-of-lists of Python floats, so the
comparison against the C++ contiguous float matrix is explicit and fair.
"""
from __future__ import annotations

import argparse
import array
import random
import resource
import time


def rss_mb() -> float:
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024.0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--n", type=int, default=10000)
    parser.add_argument("--dim", type=int, default=512)
    parser.add_argument("--iters", type=int, default=1)
    parser.add_argument("--mode", choices=("list", "array"), default="list")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    rng = random.Random(12345)
    started = time.perf_counter()

    if args.mode == "list":
        matrix = [[rng.random() for _ in range(args.dim)] for _ in range(args.n)]
    else:
        matrix = array.array("f", (rng.random() for _ in range(args.n * args.dim)))

    query = [rng.random() for _ in range(args.dim)]
    loaded_ms = (time.perf_counter() - started) * 1000.0
    rss = rss_mb()
    bytes_per_record = rss * 1024.0 * 1024.0 / args.n

    samples = []
    for _ in range(args.iters):
        scan_started = time.perf_counter()
        best = -1.0
        if args.mode == "list":
            for row in matrix:
                score = 0.0
                for i in range(args.dim):
                    score += query[i] * row[i]
                if score > best:
                    best = score
        else:
            for row in range(args.n):
                offset = row * args.dim
                score = 0.0
                for i in range(args.dim):
                    score += query[i] * matrix[offset + i]
                if score > best:
                    best = score
        samples.append((time.perf_counter() - scan_started) * 1000.0)

    samples.sort()
    p50 = samples[len(samples) // 2]
    print(f"mode={args.mode} n={args.n} dim={args.dim} iters={args.iters}")
    print(f"load_ms={loaded_ms:.3f} rss_mb={rss:.1f} bytes_per_record={bytes_per_record:.0f}")
    print(f"search_p50_ms={p50:.3f} search_p99_ms={samples[-1]:.3f}")


if __name__ == "__main__":
    main()
