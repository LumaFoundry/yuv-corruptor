## yuv-corruptor  
[![Latest Artifacts](https://img.shields.io/badge/下载-最新构建-blue?style=flat-square&logo=github)](https://github.com/BoningZ/yuv-corruptor/actions/workflows/build-windows.yml)

[English](README.md) | [简体中文](README.zh-CN.md)

一个命令行小工具：读取原始 YUV 视频，按预设生成多种“肉眼不易察觉”的缺陷 MP4，并输出一份 manifest（“解密”）文件，记录种子、输入信息与各缺陷的参数/位置。

### 会生成哪些缺陷
- lowres_blocky：先缩小再邻近放大，制造块感
- brightness_drift：整体 Y 亮度轻微偏移
- jitter_1px：周期性 1 像素轻抖（水平/垂直）
- edge_oversmooth：轻高斯模糊，强调边缘变软
- highlight_clip：高光阈值以上裁剪到 255
- chroma_bleed：局部帧段的色度错位 + 轻模糊
- luma_bleed：局部帧段的亮度拖影/扩散（gblur+blend）
- grain：轻度胶片颗粒（时域噪声）+ 轻锐化
- ringing：轻度过锐/振铃（unsharp+deblock）
- banding：降低 Y 级别（近似 posterize）后轻模糊
- ghosting：轻度时域平均（tblend low opacity）
- repeat_frames_keep_count：插入少量重复帧，再从后部等量丢帧，保持总帧数一致

输出的文件会在输入基名后追加随机 3 字母后缀，例如 `input_abc.mp4`（种子可控）。

### 依赖
- C++17 编译器（GCC/Clang/MSVC）
- CMake ≥ 3.20（推荐 Ninja）
- ffmpeg / ffprobe 在 PATH 中（或用 `--ffmpeg`/`--ffprobe` 指定）

Windows 推荐：MSYS2 MinGW64 + Ninja

### 构建
Windows（MSYS2 MinGW64 + Ninja）:
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j 8
```

Linux/macOS:
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

生成的可执行文件在 `build/yuv-corruptor[.exe]`。

### 使用
程序会打印帮助：
```
Usage:
  yuv-corruptor <input.yuv> -r WxH [-f fps] [-p pixfmt] [-s seed]
                  [-t types] [-o outdir] [--ffmpeg ffmpeg] [--ffprobe ffprobe]

Flags:
  -r WxH                分辨率，如 -r 176x144（若未提供，将尝试从文件名如 *_352x288_30_*.yuv 推断 WxH 与 fps）
  -f fps                帧率（默认 30；若未提供且文件名包含 _<fps>_ 将尝试从文件名推断）
  -p pixfmt             像素格式（默认 yuv420p）
  -s seed               随机种子（uint64，默认基于时间）
  -t types              逗号分隔：{blocky,brightness,jitter,smooth,highclip,chroma,luma,grain,ringing,banding,ghosting,repeat,all}
  -o outdir             输出目录（默认 out_<timestamp>）
  --ffmpeg <path>       ffmpeg 可执行文件路径（默认：使用 PATH 中的 ffmpeg）
  --ffprobe <path>      ffprobe 可执行文件路径（默认：使用 PATH 中的 ffprobe）
```

注意：输入按 `-f rawvideo` 解析；manifest 的帧数估算默认假设 `yuv420p 8-bit`。

### 示例
生成全部缺陷（默认种子与输出目录）：
```bash
build/yuv-corruptor.exe input_1920x1080.yuv -r 1920x1080 -f 30 -p yuv420p
```

只生成重复帧和 chroma-bleed，固定种子，自定义输出目录：
```bash
build/yuv-corruptor.exe input.yuv -r 1280x720 -f 25 -s 123456 -t repeat,chroma -o out_test
```

### 输出
- 多个带 `{base}_{abc}.mp4` 的视频文件（`abc` 为随机 3 字母后缀）
- 输出目录下 `manifest.txt` 示例如下：
```
seed=123456
input=D:/data/input.yuv
size=1920x1080 pix=yuv420p fps=30
total_frames~=300 (assume yuv420p 8-bit)
outputs:
  - input_abc.mp4 | lowres_blocky | down/up factor=8
  - input_def.mp4 | luma_bleed | frames=[42..47],[120..123] sigma=0.62 opacity=0.31
```

### 故障排查
- H.264 报“height/width not divisible by 2”：本项目已在所有滤镜末尾追加 `scale=trunc(iw/2)*2:trunc(ih/2)*2`。
- repeat 无法播放/时间戳异常：使用 concat demuxer（`ffconcat version 1.0`）、逐帧 `duration`、末尾补最后一帧，并在编码端 `-fflags +genpts`，像素格式强制 `yuv420p`。
- ffmpeg 不在 PATH：用 `--ffmpeg`/`--ffprobe` 指定完整路径。

### 路线图（更多“肉眼难察觉”预设）
- tmix 轻度时域模糊；微小 hue/saturation/gamma 偏移（短帧段）
- chroma-only 轻柔化、弱 deblock、轻对比度“呼吸”
- 边缘掩码 + 轻蚊噪（更拟真，复杂度更高）

### 许可证
本项目采用 MIT 许可，详见 `LICENSE`。


