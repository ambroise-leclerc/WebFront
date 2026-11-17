# WebFront artwork

- `webfront-swallow.svg`: the tricolour swallow on a transparent background.
- `webfront-icon.svg`: the compact 64 × 64 SVG for a future WebFront developer-tools launcher. Its pale rounded tile preserves contrast on light and dark backgrounds; the white neck separator and lime throat are widened for small sizes. The repository README displays it at 24 × 24 px beside the title; its pale tile also keeps the bird visible in GitHub’s dark theme. Prefer 24 or 32 px for a launcher button.

Both files are self-contained vectors with explicit forest-green, lime and white fills, and no external resources. Preserve all three colours when reusing them. For an icon-only button, give the button an accessible name such as “WebFront tools” and treat the image as decorative (`alt=""`).

The artwork is available for reuse; adding the developer-tools button and its behaviour is separate application work.

- `automation-agent.svg`: the animated transcript of the README's automation section (an agent attaching to an application's own window). It is a self-contained SVG with SMIL animation and no script or external resource; the commands and answers are abridged from the `AutomationConsumerWindow` regression test.
- `host-loop-boing.gif`: a 12-second native WebKitGTK capture on a Jetson Orin Nano, computed with CUDA: eight seconds of the checkered ball followed by four seconds of illuminated glass and beams. A fixed 25 fps simulation clock preserves normal motion through the material switch; statistics are hidden. An illustrative native title bar, window controls, border and shadow are added by `render-boing.py`; the capture API supplies the window content. The guide includes the `--capture-demo --quality 0` command and GIF assembly recipe ([guide](../HOST_LOOP_EXAMPLE.md)).
