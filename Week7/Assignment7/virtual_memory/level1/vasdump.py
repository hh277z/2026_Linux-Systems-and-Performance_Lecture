#!/usr/bin/env python3
"""Interface skeleton. Implement /proc parsing and reports without inspection libraries."""
import argparse
import sys

FIELDS = ("Size", "Rss", "Pss", "Shared_Clean", "Shared_Dirty",
          "Private_Clean", "Private_Dirty", "Anonymous")
CATEGORIES = ("executable", "heap", "stack", "shared-library",
              "other-file", "anonymous", "special")

def parse_smaps(path):
    # TODO: parse headers and all FIELDS; preserve spaces and (deleted) in paths.
    # Each record should contain start/end, permissions, offset, dev/inode, path,
    # and a dictionary of field values in kB. Missing fields need explicit handling.
    raise NotImplementedError("implement smaps parsing")

def classify(vma, executable):
    # TODO: implement Section 5.2 priorities, not section-name guesses.
    raise NotImplementedError("implement VMA classification")

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('-v', action='store_true')
    parser.add_argument('--delay', type=float, default=1.0)
    parser.add_argument('--diff', type=float)
    parser.add_argument('target', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.delay < 0 or (args.diff is not None and args.diff <= 0):
        parser.error('delay must be nonnegative and diff must be positive')
    if not args.target:
        parser.error('provide a PID or -- command [args...]')
    # TODO: implement PID/command modes, bounded cleanup, aggregation, verbose
    # output, diff matching/sorting, and clear failures (Sections 4.1 and 5.2).
    print('vasdump: student implementation required', file=sys.stderr)
    return 2

if __name__ == '__main__':
    sys.exit(main())
