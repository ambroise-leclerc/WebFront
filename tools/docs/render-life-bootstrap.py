#!/usr/bin/env python3
"""Render the README walkthrough (requires Pillow); uses the real Life screenshot.

This is an editorial animation: terminal output and timing are condensed, and
preview height is expanded to keep the application legible at README size.
"""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont, ImageSequence

ROOT = Path(__file__).resolve().parents[2]
WIDTH, HEIGHT = 800, 765
BACKGROUND = '#141821'
# Keep preview pixels inside the window's border and rounded lower corners.
WINDOW_MASK = Image.new('L', (WIDTH, HEIGHT))
ImageDraw.Draw(WINDOW_MASK).rounded_rectangle(
    (1, 1, WIDTH - 2, HEIGHT - 2), radius=11, fill=255)
FONT_PATHS = ('/System/Library/Fonts/Menlo.ttc', '/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf')
font_path = next((path for path in FONT_PATHS if Path(path).exists()), None)
if not font_path:
    raise SystemExit('Install Menlo or DejaVu Sans Mono to render the walkthrough')
font = ImageFont.truetype(font_path, 15)
small = ImageFont.truetype(font_path, 13)
shot = Image.open(ROOT / 'docs/images/life-preview.jpg').convert('RGB')
# Remove native title chrome; retain the actual application interface.
shot = shot.crop((0, 64, shot.width, shot.height))
shot = shot.resize((WIDTH, 600), Image.Resampling.LANCZOS)
frames, durations = [], []


def frame(lines, preview=0, duration=100):
    image = Image.new('RGB', (WIDTH, HEIGHT), BACKGROUND)
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((0, 0, WIDTH - 1, HEIGHT - 1), radius=12, outline='#394451', width=2)
    draw.rectangle((1, 28, WIDTH - 2, 39), fill='#222b36')
    draw.rounded_rectangle((1, 1, WIDTH - 2, 37), radius=11, fill='#222b36')
    for x, color in ((18, '#ff6259'), (38, '#ffbd2e'), (58, '#28c840')):
        draw.ellipse((x, 13, x + 10, 23), fill=color)
    draw.text((87, 10), 'WebFront', font=small, fill='#e8edf4')
    for index, line in enumerate(lines):
        draw.text((18, 52 + index * 24), line, font=font,
                  fill='#a8eacb' if line.startswith('wf>') else '#a0adbc')
    if preview:
        image.paste(shot.crop((0, 0, WIDTH, preview)), (0, 164),
                    WINDOW_MASK.crop((0, 164, WIDTH, 164 + preview)))
    frames.append(image)
    durations.append(duration)


lines = ['Developer environment ready.', 'wf> ']
frame(lines, duration=1100)
commands = ('wf create life', 'wf install react@19.3.1 react-dom@19.3.1', 'wf preview ui/main.tsx')
history = []
for command in commands:
    for end in range(0, len(command), 3):
        frame(history + ['wf> ' + command[:end]], duration=70)
    history.append('wf> ' + command)
    frame(history, duration=950)
# The preview appears in two steps while the command history stays visible.
frame(history, preview=125, duration=650)
frame(history, preview=600, duration=4600)
# Native captures of the next twenty C++ generations (27–46), cropped and
# scaled exactly like the still above. Keep all introductory frames unchanged.
playback_palette = frames[-1].quantize(colors=256)
with Image.open(ROOT / 'docs/images/life-generations.webp') as playback:
    for capture in ImageSequence.Iterator(playback):
        shot = capture.convert('RGB')
        frame(history, preview=600, duration=150)
        # A shared palette keeps unchanged UI pixels stable between generations.
        frames[-1] = frames[-1].quantize(palette=playback_palette,
                                         dither=Image.Dither.FLOYDSTEINBERG)
durations[-1] = 1800
frames[0].save(ROOT / 'docs/images/life-bootstrap.gif', save_all=True,
               append_images=frames[1:], duration=durations, loop=0, optimize=True)
print(ROOT / 'docs/images/life-bootstrap.gif')
