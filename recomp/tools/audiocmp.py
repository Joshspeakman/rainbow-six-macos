#!/usr/bin/env python3
"""audiocmp.py <native.raw> <reference audio file> [rate] : how closely a stream the port played (a W32_DSDUMP
dump: s16 stereo at the mixer rate, 44100 Hz) matches a reference decode (anything ffmpeg reads). Finds the reference offset
of a few windows of the dump by cross-correlation and prints, per window, that offset and the normalised correlation
(1.0 = identical up to gain), plus the gain ratio. A port that plays the stream right scores > 0.99 everywhere at
offsets that advance with the window."""
import sys, subprocess
import numpy as np

nat_path, ref_path = sys.argv[1], sys.argv[2]
rate = int(sys.argv[3]) if len(sys.argv) > 3 else 44100
nat = np.fromfile(nat_path, dtype='<i2').astype(np.float64).reshape(-1, 2).mean(1)
ref = np.frombuffer(subprocess.run(['ffmpeg', '-v', 'error', '-i', ref_path, '-f', 's16le', '-ac', '2', '-ar', str(rate), '-'],
                                   capture_output=True, check=True).stdout, dtype='<i2').astype(np.float64).reshape(-1, 2).mean(1)
print('native %.1f s, reference %.1f s' % (len(nat) / rate, len(ref) / rate))
win = 2 * rate
first = np.argmax(np.abs(nat) > 300)        # skip leading silence
starts = [first + k * len(nat) // 6 for k in range(5)]
for s in starts:
    seg = nat[s:s + win]
    if len(seg) < win or np.std(seg) < 50: print('  %6.1f s: (quiet)' % (s / rate)); continue
    n = 1 << int(np.ceil(np.log2(len(ref) + win)))
    xc = np.fft.irfft(np.fft.rfft(ref, n) * np.conj(np.fft.rfft(seg, n)), n)[:len(ref) - win]
    off = int(np.argmax(xc)); r = ref[off:off + win]
    c = float(np.dot(seg, r) / (np.linalg.norm(seg) * np.linalg.norm(r) + 1e-9))
    print('  %6.1f s of the dump -> %6.2f s of the reference: correlation %.4f, gain %.3f' % (s / rate, off / rate, c, np.std(seg) / (np.std(r) + 1e-9)))
