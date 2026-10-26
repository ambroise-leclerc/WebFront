#!/usr/bin/env python3
"""Render the README walkthrough (requires Pillow); uses the real Life screenshot.

The C++ application is drawn inside the animation, left of the WebFront window, so
the whole walkthrough scales as one image and is never cropped by the page layout.
The code is read from the README snippet; terminal output and timing are condensed
and the preview is expanded for readability.
"""
import re
from pathlib import Path
from PIL import Image, ImageColor, ImageDraw, ImageFont, ImageSequence

ROOT = Path(__file__).resolve().parents[2]
BACKGROUND = '#141821'
CODE_WIDTH, WINDOW_WIDTH, GAP = 432, 640, 4
PREVIEW_HEIGHT, TERMINAL_TOP = 480, 142
WIDTH = CODE_WIDTH + GAP + WINDOW_WIDTH
HEIGHT = TERMINAL_TOP + PREVIEW_HEIGHT
WINDOW_X = CODE_WIDTH + GAP
WINDOW_BOX = (WINDOW_X, 1, WIDTH - 2, HEIGHT - 2)
RADIUS = 12
BUTTONS = ('#ff6259', '#ffbd2e', '#28c840')
# Keep preview pixels inside the window's border and rounded lower corners.
WINDOW_MASK = Image.new('L', (WIDTH, HEIGHT))
ImageDraw.Draw(WINDOW_MASK).rounded_rectangle(
    (WINDOW_X + 1, 2, WIDTH - 3, HEIGHT - 3), radius=RADIUS - 1, fill=255)
FONT_PATHS = ('/System/Library/Fonts/Menlo.ttc', '/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf')
font_path = next((path for path in FONT_PATHS if Path(path).exists()), None)
if not font_path:
    raise SystemExit('Install Menlo or DejaVu Sans Mono to render the walkthrough')
code_font = ImageFont.truetype(font_path, 12)
font = ImageFont.truetype(font_path, 13)
small = ImageFont.truetype(font_path, 11)

# The first C++ block of the README is the introductory application.
readme = (ROOT / 'README.md').read_text(encoding='utf-8')
source = re.search(r'```cpp\n(.*?)```', readme, re.S).group(1).rstrip('\n').split('\n')

KEYWORDS = {'auto', 'constexpr', 'const', 'for', 'if', 'int', 'return', 'using', 'namespace', 'bool'}
TOKEN = re.compile(r'(#include|<[\w/.]+>|"[^"]*"|\b\d+\b|\b\w+\b|\s+|.)')


def color(token, previous):
    if token.startswith('#') or (token.startswith('<') and previous == '#include'):
        return '#c792ea'
    if token.startswith('"'):
        return '#a8eacb'
    if token.isdigit():
        return '#f78c6c'
    if token in KEYWORDS:
        return '#82aaff'
    if token in ('Grid', 'bitset', 'size_t', 'NativeApp', 'webfront'):
        return '#ffcb6b'
    return '#d6deeb'


def draw_code(draw):
    line_height = (HEIGHT - 12) / len(source)
    advance = draw.textlength('M', font=code_font)
    for row, line in enumerate(source):
        x, previous = 6, ''
        for token in TOKEN.findall(line):
            draw.text((x, 6 + row * line_height), token, font=code_font, fill=color(token, previous))
            x += advance * len(token)
            if token.strip():
                previous = token


shot = Image.open(ROOT / 'docs/images/life-preview.jpg').convert('RGB')
# Remove native title chrome; retain the actual application interface.
shot = shot.crop((0, 64, shot.width, shot.height))
shot = shot.resize((WINDOW_WIDTH, PREVIEW_HEIGHT), Image.Resampling.LANCZOS)
frames, durations = [], []


def frame(lines, preview=0, duration=100):
    image = Image.new('RGB', (WIDTH, HEIGHT), BACKGROUND)
    draw = ImageDraw.Draw(image)
    draw_code(draw)
    draw.rounded_rectangle(WINDOW_BOX, radius=RADIUS, fill='#141821', outline='#394451', width=1)
    draw.rounded_rectangle((WINDOW_X + 1, 2, WIDTH - 3, 30), radius=RADIUS - 1, fill='#222b36')
    draw.rectangle((WINDOW_X + 1, 20, WIDTH - 3, 30), fill='#222b36')
    for x, tint in zip((14, 32, 50), BUTTONS):
        draw.ellipse((WINDOW_X + x, 11, WINDOW_X + x + 10, 21), fill=tint)
    draw.text((WINDOW_X + 74, 9), 'WebFront', font=small, fill='#e8edf4')
    for index, line in enumerate(lines):
        draw.text((WINDOW_X + 10, 42 + index * 20), line, font=font,
                  fill='#a8eacb' if line.startswith('wf>') else '#a0adbc')
    if preview:
        image.paste(shot.crop((0, 0, WINDOW_WIDTH, preview)), (WINDOW_X, TERMINAL_TOP),
                    WINDOW_MASK.crop((WINDOW_X, TERMINAL_TOP, WIDTH, TERMINAL_TOP + preview)))
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
frame(history, preview=100, duration=650)
frame(history, preview=PREVIEW_HEIGHT, duration=4600)
# Native captures of the next twenty C++ generations (27–46), cropped and
# scaled exactly like the still above. Keep all introductory frames unchanged.
with Image.open(ROOT / 'docs/images/life-generations.webp') as playback:
    for capture in ImageSequence.Iterator(playback):
        shot = capture.convert('RGB').resize(
            (WINDOW_WIDTH, PREVIEW_HEIGHT), Image.Resampling.LANCZOS)
        frame(history, preview=PREVIEW_HEIGHT, duration=150)
durations[-1] = 1800
# One palette shared by every frame keeps unchanged pixels stable between frames;
# the code and the window each get their own colors, or the small text would lose its syntax colors.
last = frames[-1]
code_colors = last.crop((0, 0, CODE_WIDTH, HEIGHT)).quantize(32, method=Image.Quantize.MEDIANCUT)
window_colors = last.crop((WINDOW_X, 0, WIDTH, HEIGHT)).quantize(216, method=Image.Quantize.MEDIANCUT)
chrome = [ImageColor.getrgb(c) for c in BUTTONS + ('#222b36', '#394451', '#e8edf4', '#a8eacb', '#a0adbc')]
colors = [tuple(c) for c in zip(*[iter((code_colors.getpalette() or [])[:96])] * 3)]
colors += [tuple(c) for c in zip(*[iter((window_colors.getpalette() or [])[:648])] * 3)]
palette = Image.new('P', (1, 1))
palette.putpalette([v for c in (colors[:248] + chrome) for v in c])
frames = [f.quantize(palette=palette, dither=Image.Dither.NONE) for f in frames]
frames[0].save(ROOT / 'docs/images/life-bootstrap.gif', save_all=True,
               append_images=frames[1:], duration=durations, loop=0, optimize=True)
print(ROOT / 'docs/images/life-bootstrap.gif')
