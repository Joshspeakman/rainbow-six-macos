#!/usr/bin/env python3
"""Probe native compiler correctness before compiling lifted register operations."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


def compatible_flags(compiler):
    source = Path(__file__).resolve().parents[1] / 'tests/compiler_partial.c'
    with tempfile.TemporaryDirectory(prefix='ds2-compiler-') as temporary:
        binary = Path(temporary) / 'probe'

        def passes(flags):
            build = subprocess.run([compiler, '-O2', *flags, str(source), '-o', str(binary)],
                                   capture_output=True, text=True)
            return build.returncode == 0 and subprocess.run([str(binary)], capture_output=True).returncode == 0

        if passes([]):
            return []
        # Observed with CachyOS Clang 23.1.1: optimized register coalescing
        # loses the upper bytes. Probe the workaround instead of guessing
        # whether a compiler version contains the affected optimization.
        workaround = ['-mllvm', '-join-liveintervals=false']
        if passes(workaround):
            print('Compiler partial-register check requires -join-liveintervals=false.', file=sys.stderr)
            return workaround
    raise RuntimeError('The native compiler failed the partial-register correctness check.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('compiler')
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    try:
        flags = compatible_flags(args.compiler)
        args.output.write_text(' '.join(flags) + '\n')
    except (OSError, RuntimeError) as error:
        parser.exit(1, f'{error}\n')
