## yuv-corruptor  
[![Latest Artifacts](https://img.shields.io/badge/下載-最新構建-blue?style=flat-square&logo=github)](https://github.com/BoningZ/yuv-corruptor/actions/workflows/build-windows.yml)

[English (Simplified) 🇺🇸](README.en-US.md) | [English (Traditional) 🇬🇧](../../README.md) | [Chinese (Simplified) 🇨🇳](README.zh-CN.md) | [Chinese (Traditional) 🇲🇴](README.zh-Hant.md)

一個命令列小工具：讀取原始 YUV 影片，按預設生成多種「肉眼不易察覺」的缺陷 MP4，並輸出一份 manifest（「解密」）文件，記錄種子、輸入資訊與各缺陷的參數/位置。

### 會生成哪些缺陷
- bitrate_blocky：低碼率 + 快預設編碼產生的區塊偽影（更貼近真實解碼失真）
- brightness_drift：整體 Y 亮度輕微偏移
- jitter_1px：週期性 1 像素輕抖（水平/垂直）
- edge_oversmooth：輕高斯模糊，強調邊緣變軟
- highlight_clip：高光閾值以上裁剪到 255
- chroma_bleed：局部幀段的色度錯位 + 輕模糊
- luma_bleed：局部幀段的亮度拖影/擴散（gblur+blend）
- grain：輕度膠片顆粒（時域雜訊）+ 輕銳化
- ringing：輕度過銳/振鈴（unsharp+deblock）
- banding：降低 Y 等級（近似 posterize）後輕模糊
- ghosting：輕度時域平均（tblend 低不透明度）
- colorspace_mismatch：解碼/編碼階段使用不同色彩空間（bt709 與 bt601）
- repeat_frames_keep_count：在位置 p 重複 r 次，並等量從後續丟棄，保持總幀數一致

輸出的檔案會在輸入基名後追加隨機 3 字母後綴，例如 `input_abc.mp4`（種子可控）。

## 缺陷詳解與示例

以下說明基於本目錄下 `CoastGuard_1920x1080_30.yuv` 生成，種子為 `1755512385570399000`。截圖取自開源的視覺檢視工具：[YUViz](https://github.com/LokiW-03/YUViz)

### bitrate_blocky（低碼率區塊）
採用低碼率 + 快速預設進行編碼，平坦區域、運動區域容易出現明顯巨集區塊邊界/馬賽克。下圖右側為低碼率編碼的結果。

![bitrate_blocky](../../public/bitrate_blocky.png)

### jitter_1px（1 像素抖動）
週期性 1px 水平/垂直環繞位移。對比參考幀與抖動幀，細豎線/橫線邊緣會出現位置跳動。下圖中 Diff 模式會週期性交替顯示，呈現閃爍狀。

<img src="../../public/jitter_off.png" height="220" /> <img src="../../public/jitter_on.png" height="220" />

### edge_oversmooth（邊緣過平滑）
輕度高斯模糊使邊緣與紋理變軟，細節略有塗抹感。
Diff 模式下物體邊緣會格外突出。例如下圖中快艇與右下角 logo 邊緣都被突出顯示。

![edge_oversmooth](../../public/edge_smooth.png)

### highlight_clip（高光裁剪）
超過閾值的高光被裁為純白，高亮區域的高光細節丟失。下圖從左到右依次是：原影片、高光裁剪後、Diff。

![highlight_clip](../../public/highlight_clip.png)

### chroma_bleed（色度「溢出/錯位」）
色度通道發生輕微錯位，邊緣處出現彩色邊/拖影；常見為 Cb/Cr 的水平/垂直微位移。下圖右側為色彩溢出的結果，色彩向左側偏移。

![chroma_bleed](../../public/chroma_bleeding.png)

### grain（膠片顆粒）
加入細微雜訊，平坦區域顯示輕度隨機紋理。右圖為顆粒效果結果。

![grain](../../public/grain.png)

### ringing（振鈴/光暈）
過銳與輕去塊配合，在強對比邊緣附近出現光暈/二次紋理。
Diff 模式下主要體現為靜態物體邊緣格外突出，例如圖中右下角 logo。

![ringing](../../public/ringing.png)

### banding（色帶）
降低亮度等級（近似色階化）後更易出現條帶狀過渡，漸變區域尤為明顯。

![banding](../../public/banding.png)

### ghosting（拖影）
時域混合造成運動目標後方出現淡淡尾跡。
Diff 模式下，運動物體邊緣更為明顯，而靜態物體變化不大。

![ghosting](../../public/ghosting.png)

### colorspace_mismatch（色彩空間不匹配）
以一種色彩空間（如 BT.709）解碼卻以另一種（如 BT.601）標記/轉換輸出，畫面出現輕微色偏（膚色、紅橙等尤為明顯）。右圖為刻意造成的色偏示例。

![colorspace_mismatch](../../public/colourspace.png)

### 依賴
- C++17 編譯器（GCC/Clang/MSVC）
- CMake ≥ 3.20（推薦 Ninja）
- PATH 中提供 ffmpeg / ffprobe（或以 `--ffmpeg`/`--ffprobe` 指定）

Windows 推薦：MSYS2 MinGW64 + Ninja

### 構建
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

生成的可執行檔於 `build/yuv-corruptor[.exe]`。

### 使用
程式會印出說明：
```
Usage:
  yuv-corruptor <input.yuv> -r WxH [-f fps] [-p pixfmt] [-s seed]
                  [-t types] [-o outdir] [--ffmpeg ffmpeg] [--ffprobe ffprobe]

Flags:
  -r WxH                解析度，例如 -r 176x144
  -f fps                幀率（預設 30）
  -p pixfmt             像素格式（預設 yuv420p）
  -s seed               隨機種子（uint64，預設基於時間）
  -t types              逗號分隔：{blocky,brightness,jitter,smooth,highclip,chroma,luma,grain,ringing,banding,ghosting,colorspace,repeat,all}
  -o outdir             輸出目錄（預設 out_<timestamp>）
  --ffmpeg <path>       ffmpeg 可執行檔
  --ffprobe <path>      ffprobe 可執行檔
```

注意：輸入按 `-f rawvideo` 解析；manifest 的幀數估算預設假設 `yuv420p 8-bit`。

### 範例
產生全部缺陷（預設種子與輸出目錄）：
```bash
build/yuv-corruptor.exe video_1920x1080.yuv -r 1920x1080 -f 30 -p yuv420p
```

只產生重複幀與 chroma-bleed，固定種子，自訂輸出目錄：
```bash
build/yuv-corruptor.exe input.yuv -r 1280x720 -f 25 -s 123456 -t repeat,chroma -o out_test
```

### 輸出
- 多個 `{base}_{ab}.mp4` 檔案，其中 `{ab}` 為隨機兩字母後綴
- `manifest.txt` 示例如下：
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

### 疑難排解
- 確保 `ffmpeg`/`ffprobe` 已在 PATH 中或指定路徑。
- 若 H.264 報寬/高非偶數，本專案在所有濾鏡鏈末尾附帶 `scale=trunc(iw/2)*2:trunc(ih/2)*2`。
- repeat 無法正常播放：請使用 concat demuxer，逐幀 `duration`，並在編碼端 `-fflags +genpts`，像素格式強制 `yuv420p`。

### 授權
本專案採 MIT 授權，詳見 `LICENSE`。



