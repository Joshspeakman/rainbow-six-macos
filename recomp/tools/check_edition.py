#!/usr/bin/env python3
"""Identify an input executable before applying address-specific DS2 hooks."""
import argparse
from pathlib import Path
from editions import GOG_23, PROFILES, identify
# Only verified layouts may use the GOG-address hooks and native hot loops.

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('exe', type=Path)
    parser.add_argument('--id', action='store_true')
    parser.add_argument('--prepared', action='store_true')
    args = parser.parse_args()
    try:
        edition = identify(args.exe, prepared=args.prepared)
    except (OSError, ValueError) as error: parser.exit(1, f'{error}\nYour original executable has not been modified.\n')
    print(edition if args.id else PROFILES[edition]['name'] + ': recognized executable layout')
    return 0

if __name__ == '__main__': raise SystemExit(main())
