## yuv-corruptor  
[![Latest Artifacts](https://img.shields.io/badge/Download-Latest%20Build-blue?style=flat-square&logo=github)](https://github.com/BoningZ/yuv-corruptor/actions/workflows/build-windows.yml)

[:us: English (Simplified)](README.en-US.md) | [:uk: English (Traditional)](../../README.md) | [:cn: Chinese (Simplified)](README.zh-CN.md) | [:macau: Chinese (Traditional)](README.zh-Hant.md)

A small command-line tool that takes a raw YUV video and produces multiple subtly flawed MP4s for QA/testing. It also writes a manifest ("decryption") file describing the seed, input metadata, and the exact defect/positions per output.

### What it generates
- bitrate_blocky: low-bitrate H.264 at fast preset to induce encoding blockiness
- brightness_drift: slight global Y brightness shift
- jitter_1px: periodic 1-pixel jitter (horizontal or vertical)
- edge_oversmooth: mild gaussian blur targeting edge softness
- highlight_clip: clip highlights above a random Y threshold
- chroma_bleed: short frame spans with chroma misalignment + mild blur
- luma_bleed: slight luma smearing/bleeding via mild blur blending on Y
- grain: add subtle film grain (temporal noise) with slight sharpening
- ringing: emphasize ringing by oversharpening then mild deblocking
- banding: reduce luma levels (posterize-like) then lightly blur to reveal banding
- ghosting: subtle temporal blending (tblend average at low opacity)
- colorspace_mismatch: decode/encode with different color spaces (bt709 vs bt601)
- repeat_frames_keep_count: duplicate a frame and drop the same amount later to keep the total frame count unchanged

## Defect details and examples

The following notes are based on `CoastGuard_1920x1080_30.yuv`, with seed `1755512385570399000`. Screenshots were captured using the open-source visual inspection tool [YUViz](https://github.com/LokiW-03/YUViz).

### bitrate_blocky
Low-bitrate H.264 at a fast preset produces visible macroblock boundaries, especially in flat areas and motion. The right panel below shows the low-bitrate result.

![bitrate_blocky](../../public/bitrate_blocky.png)

### jitter_1px
Periodic 1px wrap shift horizontally or vertically. Compare reference vs jittered frame; pay attention to thin vertical/horizontal edges. In Diff mode the two states alternate periodically.

<img src="../../public/jitter_off.png" height="220" /> <img src="../../public/jitter_on.png" height="220" />

### edge_oversmooth
Mild gaussian blur softens edges and textures; fine details look smeared. In Diff mode, object contours become prominent; for example, the boat and the corner logo edges are highlighted.

![edge_oversmooth](../../public/edge_smooth.png)

### highlight_clip
Highlights above a threshold are clipped to white; bright regions lose specular detail. Left to right: original, clipped, Diff.

![highlight_clip](../../public/highlight_clip.png)

### chroma_bleed
Chroma misalignment causes color fringes bleeding across edges; often small horizontal/vertical shifts in Cb/Cr. In the example, the right panel shows chroma bleeding with colors shifted left.

![chroma_bleed](../../public/chroma_bleeding.png)

### grain
Subtle film grain adds fine noise; flat areas show stochastic texture. The right panel shows the grain-applied result.

![grain](../../public/grain.png)

### ringing
Oversharpening/deblocking interplay yields halos around high-contrast edges. In Diff mode, static object edges stand out (e.g., the corner logo).

![ringing](../../public/ringing.png)

### banding
Reduced luma levels or posterization reveals visible bands in smooth gradients.

![banding](../../public/banding.png)

### ghosting
Temporal blending leaves faint trails following motion. In Diff mode, moving-object edges are emphasized while static ones are not; e.g., sea and boat edges are clear while the logo changes little.

![ghosting](../../public/ghosting.png)

### colorspace_mismatch
Decode as one colorspace (e.g., BT.709) but mark/convert to another (e.g., BT.601), leading to hue/saturation bias (skin tones shift, reds/oranges change). The right panel shows the intentional color bias.

![colorspace_mismatch](../../public/colourspace.png)

Outputs are randomly named using the base filename plus a three-letter suffix, e.g. `input_abc.mp4`, controlled by a seed.

### Requirements
- C++17 compiler (GCC/Clang/MSVC)
- CMake >= 3.20 (Ninja recommended)
- ffmpeg and ffprobe available on PATH (or pass via `--ffmpeg`/`--ffprobe`)

Windows (recommended): MSYS2 MinGW64 toolchain with Ninja.

### Build
Windows (MSYS2 MinGW64 + Ninja):
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j 8
```

Linux/macOS:
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The executable will be at `build/yuv-corruptor[.exe]`.

### Usage
Printed by the program:
```
Usage:
  yuv-corruptor <input.yuv> -r WxH [-f fps] [-p pixfmt] [-s seed]
                  [-t types] [-o outdir] [--ffmpeg ffmpeg] [--ffprobe ffprobe]

Positional:
  <input.yuv>           Path to raw YUV file (8-bit by default)

Flags:
  -r WxH                Resolution, e.g. -r 176x144 (required if the filename does not contain)
  -f fps                Frame rate (default 30)
  -p pixfmt             Pixel format (default yuv420p)
  -s seed               RNG seed (uint64). Default: time-based
  -t types              CSV in {blocky,brightness,jitter,smooth,highclip,chroma,luma,grain,ringing,banding,ghosting,colorspace,repeat,all}
  -o outdir             Output directory (default out_<timestamp>)
  --ffmpeg <path>       ffmpeg executable (default: ffmpeg in PATH)
  --ffprobe <path>      ffprobe executable (default: ffprobe in PATH)

Backward compatible (optional): --in/--w/--h/--fps/--pix/--seed/--types/--out
```

Notes:
- Input is treated as `-f rawvideo` with the given `-r/-f/-p`. Frame-count estimation in the manifest assumes `yuv420p 8-bit`.
- `-t all` or leaving `-t` empty will generate all defect variants.

### Examples
Generate all variants with default seed/time and auto output folder:
```bash
build/yuv-corruptor.exe video_1920x1080.yuv -r 1920x1080 -f 30 -p yuv420p
```

Only repeat and chroma-bleed with a fixed seed, custom output dir:
```bash
build/yuv-corruptor.exe input.yuv -r 1280x720 -f 25 -s 123456 -t repeat,chroma -o out_test
```

### Outputs
- MP4s named `{base}_{ab}.mp4` where `{ab}` is a random 2-letter suffix
- A `manifest.txt` in the output directory, e.g.:
```
seed=123456
input=D:/data/input.yuv
size=1920x1080 pix=yuv420p fps=30
total_frames~=300 (assume yuv420p 8-bit)
outputs:
  - input_aa.mp4 | lowres_blocky | down/up factor=8
  - input_bb.mp4 | brightness_drift | delta_Y=2 (global)
  - input_cc.mp4 | repeat_frames_keep_count | repeat_at=99 times=6 drop=[100..105]
```

### Troubleshooting
- Ensure `ffmpeg`/`ffprobe` are installed and resolvable on PATH.
- If your IDE shows squiggles but the build passes, point the IDE to the CMake-generated `compile_commands.json` and ensure it uses the same compiler and C++ standard.
- If H.264 encoder complains height/width not divisible by 2, this project force-appends `scale=trunc(iw/2)*2:trunc(ih/2)*2` to all filter chains.
- If the repeat output fails to open, make sure your ffmpeg is recent enough and that concat list contains `file f_xxxxxx.bmp` lines without extra quoting. This project also writes per-frame `duration` to stabilize timestamps.

### Roadmap ideas
- Parameter presets and reproducible profiles
- More codec-specific artifacts (AQ issues, B-frame pulsing)
- Motion-compensation artifacts, halos, chroma siting offsets
- More subtle presets: tmix-based temporal blur, tiny hue shift, slight desaturation, tiny gamma bias,
  chroma-only softening, weak deblock, breathing-like contrast pulsing, edge-masked mosquito noise

### License
This project is licensed under the MIT License. See `LICENSE` for details.



