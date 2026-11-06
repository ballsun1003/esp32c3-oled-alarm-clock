"""Render the README media from the host display test (requires Pillow)."""
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
FRAMES = ROOT / "_backup"
OUT = ROOT / "docs" / "media"
OUT.mkdir(parents=True, exist_ok=True)


def oled(name):
    magic, width, height, *pixels = (FRAMES / f"{name}.pbm").read_text().split()
    assert (magic, width, height, len(pixels)) == ("P1", "128", "64", 8192)
    frame = Image.frombytes("L", (128, 64), bytes(255 if p == "0" else 0 for p in pixels))
    return frame.resize((384, 192), Image.Resampling.NEAREST)


def strip(filename, names):
    panels = [oled(n) for n in names]
    sheet = Image.new("L", (400 * len(panels), 208), 30)
    for i, panel in enumerate(panels):
        sheet.paste(panel, (8 + i * 400, 8))
    sheet.save(OUT / filename)


frames = [oled(f"demo-{n:03}") for n in range(120)]
frames[0].save(OUT / "clock.gif", save_all=True, append_images=frames[1:], duration=100, loop=0)
strip("controls.png", ["main-top", "alarm-edit", "days"])
strip("reset.png", ["reset-step1-empty", "reset-step1-half", "reset-step2-full"])
