"""imgdiff.py <a.png> <b.png> [diff.png] : how close two 1280x720 screenshots are (native vs reference): mean absolute
difference per channel (0-255), PSNR, the share of pixels off by more than 32, and an amplified difference image."""
import sys, numpy as np
from PIL import Image
a = np.asarray(Image.open(sys.argv[1]).convert('RGB').resize((1280, 720)), dtype=np.float64)
b = np.asarray(Image.open(sys.argv[2]).convert('RGB').resize((1280, 720)), dtype=np.float64)
d = np.abs(a - b); mse = np.mean((a - b) ** 2)
print('mean abs %.2f  PSNR %.1f dB  >32 off %.1f%%  mean brightness %.1f vs %.1f' % (d.mean(), 10 * np.log10(255 ** 2 / max(mse, 1e-9)),
      100 * np.mean(d.max(axis=2) > 32), a.mean(), b.mean()))
if len(sys.argv) > 3: Image.fromarray(np.clip(d * 4, 0, 255).astype(np.uint8)).save(sys.argv[3])
