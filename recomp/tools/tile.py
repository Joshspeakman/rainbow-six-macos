"""tile.py <out.png> <images...> : screenshots side by side (2 columns, 512x384 each) for a quick look."""
import sys
from PIL import Image
ims = [Image.open(f).convert('RGB').resize((512, 288 if Image.open(f).size[0] / Image.open(f).size[1] > 1.5 else 384)) for f in sys.argv[2:]]
h = max(i.height for i in ims) if ims else 1
w = Image.new('RGB', (1034, (h + 10) * ((len(ims) + 1) // 2)))
for k, im in enumerate(ims): w.paste(im, ((k % 2) * 522, (k // 2) * (h + 10)))
w.save(sys.argv[1])
