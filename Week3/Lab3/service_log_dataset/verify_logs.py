#!/usr/bin/env python3
"""
verify_logs.py
---------------
Independent verification of a requests.csv log file against an
expected_stats.csv summary.

This script re-reads the CSV from disk (it never trusts numbers cached by
generate_logs.py) and checks:

  1. Format: correct header, exactly 4 integer fields per row, allowed
     status codes, service_id and duration_us within range.
  2. Row count matches --expected-rows (if given).
  3. timestamp_ms is non-decreasing across the whole file.
  4. Per-service aggregates (request_count, error_count, total_duration_us,
     min_duration_us, max_duration_us) match expected_stats.csv exactly.
  5. Sum of per-service request_count equals the total row count.

Exit code is 0 only if every check passes; otherwise it is non-zero and a
clear failure reason is printed.

Usage:
    python3 verify_logs.py --csv requests.csv --stats expected_stats.csv \
        --services 1024 --expected-rows 2000000
"""

import argparse
import csv
import sys


VALID_STATUSES = {200, 201, 204, 400, 404, 429, 500, 503}
MIN_DURATION_US = 1
MAX_DURATION_US = 10_000_000


def parse_args():
    parser = argparse.ArgumentParser(description="Verify a synthetic request log CSV.")
    parser.add_argument("--csv", required=True, help="Path to requests.csv")
    parser.add_argument("--stats", required=True, help="Path to expected_stats.csv")
    parser.add_argument("--services", type=int, required=True,
                         help="Number of distinct service IDs expected (0..services-1)")
    parser.add_argument("--expected-rows", type=int, default=None,
                         help="Optional exact row count to require")
    return parser.parse_args()


def load_expected_stats(path, services):
    """Load expected_stats.csv into a dict keyed by service_id."""

    expected = {}

    with open(path, "r", newline="", encoding="utf-8") as f:
        reader = csv.reader(f)
        header = next(reader)
        expected_header = [
            "service_id", "request_count", "error_count",
            "total_duration_us", "min_duration_us", "max_duration_us",
        ]
        if header != expected_header:
            fail(f"expected_stats.csv header mismatch: {header}")

        prev_id = -1
        for row in reader:
            sid, count, errs, total, dmin, dmax = (int(x) for x in row)
            if sid != prev_id + 1:
                fail(f"expected_stats.csv service_id not strictly ascending "
                     f"at {sid} (previous {prev_id})")
            prev_id = sid
            expected[sid] = (count, errs, total, dmin, dmax)

    if len(expected) != services:
        fail(f"expected_stats.csv has {len(expected)} services, expected {services}")

    return expected


def fail(message):
    print(f"FAIL: {message}", file=sys.stderr)
    sys.exit(1)


def verify(args):
    expected = load_expected_stats(args.stats, args.services)

    # Running per-service aggregates recomputed from scratch.
    request_count = [0] * args.services
    error_count = [0] * args.services
    total_duration = [0] * args.services
    min_duration = [None] * args.services
    max_duration = [None] * args.services

    row_count = 0
    prev_timestamp = None

    with open(args.csv, "r", newline="", encoding="utf-8") as f:
        reader = csv.reader(f)

        header = next(reader)
        if header != ["timestamp_ms", "service_id", "status", "duration_us"]:
            fail(f"requests.csv header mismatch: {header}")

        for line_no, row in enumerate(reader, start=2):
            if len(row) != 4:
                fail(f"line {line_no}: expected 4 fields, got {len(row)}")

            try:
                timestamp_ms, service_id, status, duration_us = (int(x) for x in row)
            except ValueError:
                fail(f"line {line_no}: non-integer field in {row}")

            if timestamp_ms <= 0:
                fail(f"line {line_no}: timestamp_ms must be positive, got {timestamp_ms}")

            if prev_timestamp is not None and timestamp_ms < prev_timestamp:
                fail(f"line {line_no}: timestamp_ms decreased "
                     f"({timestamp_ms} < {prev_timestamp})")
            prev_timestamp = timestamp_ms

            if not (0 <= service_id < args.services):
                fail(f"line {line_no}: service_id {service_id} out of range "
                     f"[0, {args.services})")

            if status not in VALID_STATUSES:
                fail(f"line {line_no}: invalid status {status}")

            if not (MIN_DURATION_US <= duration_us <= MAX_DURATION_US):
                fail(f"line {line_no}: duration_us {duration_us} out of range")

            request_count[service_id] += 1
            if status >= 400:
                error_count[service_id] += 1
            total_duration[service_id] += duration_us

            if min_duration[service_id] is None or duration_us < min_duration[service_id]:
                min_duration[service_id] = duration_us
            if max_duration[service_id] is None or duration_us > max_duration[service_id]:
                max_duration[service_id] = duration_us

            row_count += 1

    if args.expected_rows is not None and row_count != args.expected_rows:
        fail(f"row count {row_count} != expected {args.expected_rows}")

    for sid in range(args.services):
        if request_count[sid] == 0:
            fail(f"service_id {sid} never appears in requests.csv")

        recomputed = (
            request_count[sid], error_count[sid], total_duration[sid],
            min_duration[sid], max_duration[sid],
        )
        if recomputed != expected[sid]:
            fail(
                f"service_id {sid} mismatch: recomputed {recomputed} "
                f"!= expected {expected[sid]}"
            )

    total_requests_via_services = sum(request_count)
    if total_requests_via_services != row_count:
        fail(
            f"sum of per-service request_count ({total_requests_via_services}) "
            f"!= total row count ({row_count})"
        )

    print("OK: all checks passed")
    print(f"  rows checked        : {row_count}")
    print(f"  services checked    : {args.services}")
    print(f"  total_duration sum  : {sum(total_duration)}")


def main():
    args = parse_args()
    verify(args)


if __name__ == "__main__":
    main()
