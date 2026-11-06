"""After test_display.cmd with CO2_CAPTURE_DEMO=1, render the README GIFs (Pillow)."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
FRAMES = ROOT / "_backup"
OUT = ROOT / "docs" / "media"
OUT.mkdir(parents=True, exist_ok=True)


def oled(name):
    magic, width, height, *pixels = (FRAMES / f"{name}.pbm").read_text().split()
    assert (magic, width, height, len(pixels)) == ("P1", "128", "64", 8192)
    frame = Image.frombytes("L", (128, 64), bytes(255 if p == "0" else 0 for p in pixels))
    return frame.resize((384, 192), Image.Resampling.NEAREST)


def interaction(name):
    # One mask byte followed by 8 pages of 128 vertical eight-pixel columns.
    data = (FRAMES / f"{name}.frames").read_bytes()
    stride = 1 + 1024
    assert data and len(data) % stride == 0
    frames, durations = [], []
    previous = None
    font = ImageFont.load_default(size=15)
    for start in range(0, len(data), stride):
        record = data[start:start + stride]
        if record == previous:
            durations[-1] += 40
            continue
        previous = record
        mask, raster = record[0], record[1:]
        pixels = bytes((raster[(y // 8) * 128 + x] >> (y % 8)) & 1
                       for y in range(64) for x in range(128))
        panel = Image.frombytes("P", (128, 64), pixels).resize((384, 192), Image.Resampling.NEAREST)
        canvas = Image.new("P", (404, 244), 2)
        canvas.putpalette([0, 0, 0, 246, 247, 244, 25, 29, 31, 87, 98, 102,
                           196, 204, 205, 109, 231, 193])
        canvas.paste(panel, (10, 10))
        draw = ImageDraw.Draw(canvas)
        for i, label in enumerate(("LEFT", "RIGHT", "OK")):
            x = 74 + i * 88
            pressed = mask & (1 << i)
            draw.rounded_rectangle((x, 211, x + 79, 235), radius=6,
                                   fill=5 if pressed else 2, outline=5 if pressed else 3)
            draw.text((x + 40, 223), label, font=font, anchor="mm", fill=0 if pressed else 4)
        frames.append(canvas)
        durations.append(40)
    frames[0].save(OUT / f"{name}.gif", save_all=True, append_images=frames[1:],
                   duration=durations, loop=0, optimize=False, disposal=1)


frames = [oled(f"demo-{n:03}") for n in range(120)]
frames[0].save(OUT / "clock.gif", save_all=True, append_images=frames[1:], duration=100, loop=0)
for clip in ("alarm-shortcut", "focus-and-days", "sensor-setup", "inline-setting",
             "alarm-more", "hold-to-reset", "menu-warning", "city-keyboard"):
    interaction(clip)
