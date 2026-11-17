#!/usr/bin/env python3
"""Assemble the README's captured CUDA sequence with illustrative native window chrome."""

import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


def window(content):
    """Keep captured pixels intact inside a title bar and border, with transparent outer corners."""
    frame = Image.new("RGBA", (506, 418), (0, 0, 0, 0))
    draw = ImageDraw.Draw(frame)
    draw.rounded_rectangle((12, 12, 493, 405), radius=9, fill="#f1f2f4", outline="#aeb4bd")
    draw.line((13, 44, 492, 44), fill="#c1c6ce")
    draw.rounded_rectangle((24, 22, 37, 35), radius=3, fill="#27754f")
    draw.line((27, 26, 30, 31, 34, 25), fill="white", width=2)
    try:
        font = ImageFont.truetype("DejaVuSans.ttf", 12)
    except OSError:
        font = ImageFont.load_default()
    draw.text((46, 21), "WebFront · Boing Ball", font=font, fill="#252a31")
    draw.line((421, 29, 429, 29), fill="#454b54", width=1)
    draw.rectangle((444, 24, 452, 32), outline="#454b54")
    draw.ellipse((465, 18, 485, 38), fill="#e0e3e8")
    draw.line((472, 25, 478, 31), fill="#454b54", width=1)
    draw.line((472, 31, 478, 25), fill="#454b54", width=1)
    frame.paste(content, (13, 45))
    return frame.crop((12, 12, 494, 406))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", type=Path, help="directory containing frame-0000.png through frame-0299.png")
    parser.add_argument("--output", type=Path, default=Path(__file__).with_name("host-loop-boing.gif"))
    args = parser.parse_args()
    paths = sorted(args.captures.glob("frame-*.png"))
    expected = [args.captures / f"frame-{i:04}.png" for i in range(300)]
    if paths != expected:
        parser.error("expected exactly 300 consecutive frames, numbered 0000 through 0299")
    frames = []
    for path in paths:
        with Image.open(path) as image:
            content = image.convert("RGB").resize((480, 360), Image.Resampling.LANCZOS)
        frames.append(window(content))
    # Share one palette so the decoration and scene colours stay stable throughout.
    samples = Image.new("RGB", (482 * 4, 394 * 3))
    for i, frame in enumerate(frames[::25]):
        samples.paste(frame.convert("RGB"), ((i % 4) * 482, (i // 4) * 394))
    palette = samples.quantize(colors=255)
    indexed = []
    for frame in frames:
        image = frame.convert("RGB").quantize(palette=palette, dither=Image.Dither.NONE)
        # Reserve index 255 exclusively for transparency, including when a padded palette maps black to it.
        image = image.point(list(range(255)) + [254])
        image.paste(255, mask=frame.getchannel("A").point(lambda alpha: 255 if alpha == 0 else 0))
        indexed.append(image)
    indexed[0].save(args.output, save_all=True, append_images=indexed[1:], duration=40, loop=0,
                    transparency=255, disposal=2, optimize=False)
    print(f"Wrote {args.output}: 300 frames, 25 fps, 8 seconds opaque + 4 seconds glass")


if __name__ == "__main__":
    main()
