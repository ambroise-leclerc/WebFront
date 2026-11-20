# HTTP media fixture

`seek.webm` is a 20-second black 32×32 VP8 video (10 frames per
second, one keyframe per second), with no audio track. The HTTP range browser
spec loads this native file, seeks forward, resumes playback and seeks back.
It must not require an audio output device on a CI runner.

Generated with FFmpeg and libvpx:

```sh
ffmpeg -f lavfi -i color=c=black:s=32x32:r=10:d=20 -an \
  -c:v libvpx -pix_fmt yuv420p -g 10 -b:v 20k \
  -map_metadata -1 -fflags +bitexact seek.webm
```

The retained file is 5,741 bytes; update the `total` constant in
`test/web/suite/specs/webfront-specs.mjs` if regenerating it changes its size.
FFmpeg is only needed to regenerate this fixture, never to build or run the tests.
