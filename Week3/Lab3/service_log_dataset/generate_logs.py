#!/usr/bin/env python3
"""
generate_logs.py
-----------------
Synthetic HTTP-style service request log generator for a Linux C programming
class exercise (single-thread event-loop server, O(N*M) vs O(N+M) stats
aggregation).

Produces a CSV file with columns:

    timestamp_ms,service_id,status,duration_us

Design goals (see project README for the full rationale):
  * Deterministic: same --rows/--services/--seed always produces byte
    identical output.
  * Streamed to disk row by row (no giant in-memory list of rows).
  * Realistic-ish distribution: skewed service popularity, per-service
    duration profiles, a long tail of slow requests, time-of-day request
    rate variation and short error-burst windows.
  * Uses only the Python 3 standard library.

This script never sleeps and never simulates the recorded duration_us by
waiting -- duration_us is purely a *recorded* value for later aggregation
exercises.
"""

import argparse
import bisect
import csv
import math
import random
import sys


# Valid HTTP-style status codes used in the synthetic data set.
SUCCESS_STATUSES = (200, 201, 204)
ERROR_STATUSES = (400, 404, 429, 500, 503)

MIN_DURATION_US = 1
MAX_DURATION_US = 10_000_000


def parse_args():
    """Parse and validate command line arguments."""

    parser = argparse.ArgumentParser(
        description="Generate a synthetic service request log CSV."
    )
    parser.add_argument("--rows", type=int, required=True,
                         help="Number of data rows to generate (excluding header).")
    parser.add_argument("--services", type=int, required=True,
                         help="Number of distinct service IDs (0..services-1).")
    parser.add_argument("--seed", type=int, required=True,
                         help="Random seed; identical seed+args => identical output.")
    parser.add_argument("--output", type=str, required=True,
                         help="Output CSV file path.")
    parser.add_argument("--overwrite", action="store_true",
                         help="Allow overwriting an existing output file.")

    args = parser.parse_args()

    if args.rows <= 0:
        parser.error("--rows must be a positive integer")

    if args.services <= 0:
        parser.error("--services must be a positive integer")

    if args.rows < args.services:
        parser.error("--rows must be >= --services so every service can appear")

    return args


def build_service_profiles(rng, services):
    """
    Build deterministic per-service statistical profiles from the RNG stream.

    Returns:
        cum_weights: cumulative popularity weights, for O(log services) weighted
                      service selection via bisect.
        mean_log:    per-service mean of ln(duration_us), controls typical latency.
        sigma_log:   per-service sigma of ln(duration_us), controls spread.
        base_err:    per-service baseline error probability.
    """

    weights = []
    mean_log = []
    sigma_log = []
    base_err = []

    for _ in range(services):
        # Popularity: heavy-tailed so a handful of services get most traffic,
        # but every service still gets a non-trivial weight.
        weights.append(rng.lognormvariate(0.0, 1.15))

        # Typical duration profile per service, expressed in log-space so the
        # sampled duration_us is itself roughly log-normal (short + long tail).
        mean_log.append(rng.uniform(5.2, 9.3))   # ~e^5.2=181us .. e^9.3=10938us
        sigma_log.append(rng.uniform(0.35, 1.1))

        # Baseline error rate per service (mostly successes).
        base_err.append(rng.uniform(0.004, 0.03))

    cum_weights = []
    running = 0.0
    for w in weights:
        running += w
        cum_weights.append(running)

    return cum_weights, mean_log, sigma_log, base_err


def weighted_service_choice(rng, cum_weights, total_weight):
    """O(log services) weighted pick of a service id using cumulative weights."""

    target = rng.random() * total_weight
    return bisect.bisect_left(cum_weights, target)


def build_burst_windows(rng, rows, services):
    """
    Precompute a handful of short "error burst" row-index windows.

    During a burst window, a random subset of services experiences a much
    higher error probability -- simulating e.g. a downstream dependency
    hiccup. Windows are short relative to the whole file.
    """

    bursts = []
    num_bursts = max(1, rows // 250_000)

    for _ in range(num_bursts):
        start = rng.randint(0, max(0, rows - 1))
        length = rng.randint(max(50, rows // 20000), max(200, rows // 2000))
        end = min(rows, start + length)

        affected_count = max(1, services // 20)
        affected = set(rng.sample(range(services), min(affected_count, services)))

        bursts.append((start, end, affected, rng.uniform(6.0, 20.0)))

    # Sort by start index so we can advance a pointer while streaming rows.
    bursts.sort(key=lambda b: b[0])
    return bursts


def current_burst_multiplier(bursts, idx, service_id, active_ptr):
    """
    Return the error-rate multiplier in effect for (idx, service_id), advancing
    active_ptr (a mutable single-element list) past windows that have ended.
    """

    # Drop windows that have fully ended before idx.
    while active_ptr[0] < len(bursts) and bursts[active_ptr[0]][1] <= idx:
        active_ptr[0] += 1

    multiplier = 1.0
    # Only a few windows are ever open at once, so a short linear scan from
    # active_ptr forward is fine.
    for j in range(active_ptr[0], len(bursts)):
        start, end, affected, mult = bursts[j]
        if start > idx:
            break
        if start <= idx < end and service_id in affected:
            multiplier = max(multiplier, mult)

    return multiplier


def rate_multiplier(idx, rows):
    """
    Smooth time-of-day-like request rate variation: several sinusoidal
    "days" across the file, busier periods produce smaller inter-arrival
    gaps (higher rate), quieter periods produce larger gaps.
    """

    cycles = 6.0
    phase = (idx / rows) * cycles * 2.0 * math.pi
    # Range roughly [0.4, 1.8]: busy periods speed up arrivals, quiet ones slow down.
    return 1.1 + 0.7 * math.sin(phase)


def generate(args):
    """Stream-generate the CSV file row by row."""

    rng = random.Random(args.seed)

    cum_weights, mean_log, sigma_log, base_err = build_service_profiles(rng, args.services)
    total_weight = cum_weights[-1]

    bursts = build_burst_windows(rng, args.rows, args.services)
    active_ptr = [0]

    # Guarantee every service id appears at least once: reserve the first
    # `services` rows for a random permutation of all service ids. This does
    # not violate the "not sorted by service id" requirement because these
    # rows are still emitted in increasing timestamp order and the
    # permutation itself is shuffled (not ascending).
    forced_ids = list(range(args.services))
    rng.shuffle(forced_ids)

    timestamp_ms = 1_767_225_600_000  # arbitrary fixed synthetic epoch start

    seen_services = set()

    with open(args.output, "w", newline="\n", encoding="utf-8") as f:
        f.write("timestamp_ms,service_id,status,duration_us\n")

        buffer_lines = []
        FLUSH_EVERY = 20_000

        for idx in range(args.rows):
            # --- pick service id ---
            if idx < args.services:
                service_id = forced_ids[idx]
            else:
                service_id = weighted_service_choice(rng, cum_weights, total_weight)

            seen_services.add(service_id)

            # --- advance timestamp (non-decreasing, variable rate) ---
            mult = rate_multiplier(idx, args.rows)
            # Base gap in ms: busier => smaller gap. Mostly small integers,
            # occasionally 0 so several requests can share the same ms.
            gap_scale = max(0.05, 1.0 / mult)
            gap = rng.random() * 3.0 * gap_scale
            timestamp_ms += int(gap)  # may add 0

            # --- decide status (error probability influenced by bursts) ---
            burst_mult = current_burst_multiplier(bursts, idx, service_id, active_ptr)
            p_error = min(0.9, base_err[service_id] * burst_mult)

            is_error = rng.random() < p_error
            if is_error:
                status = rng.choice(ERROR_STATUSES)
            else:
                # Weighted towards 200 for realism.
                status = rng.choices(SUCCESS_STATUSES, weights=(0.82, 0.12, 0.06))[0]

            # --- decide duration (log-normal per-service, long tail) ---
            duration = rng.lognormvariate(mean_log[service_id], sigma_log[service_id])

            # Timeouts / server errors tend to run longer.
            if status in (429, 503, 500):
                duration *= rng.uniform(2.0, 6.0)

            duration_us = int(duration)
            if duration_us < MIN_DURATION_US:
                duration_us = MIN_DURATION_US
            if duration_us > MAX_DURATION_US:
                duration_us = MAX_DURATION_US

            buffer_lines.append(
                "%d,%d,%d,%d" % (timestamp_ms, service_id, status, duration_us)
            )

            if len(buffer_lines) >= FLUSH_EVERY:
                f.write("\n".join(buffer_lines))
                f.write("\n")
                buffer_lines.clear()

        if buffer_lines:
            f.write("\n".join(buffer_lines))
            f.write("\n")

    if len(seen_services) != args.services:
        missing = args.services - len(seen_services)
        print(f"WARNING: {missing} service ids never appeared in the output", file=sys.stderr)
        sys.exit(1)

    return timestamp_ms


def main():
    args = parse_args()

    import os
    if os.path.exists(args.output) and not args.overwrite:
        print(
            f"ERROR: output file '{args.output}' already exists. "
            "Pass --overwrite to replace it.",
            file=sys.stderr,
        )
        sys.exit(1)

    print(f"Generating {args.rows} rows across {args.services} services "
          f"(seed={args.seed}) -> {args.output}")

    last_ts = generate(args)

    print(f"Done. Last timestamp_ms = {last_ts}")


if __name__ == "__main__":
    main()
