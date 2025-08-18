#include "Defects.hpp"
#include "Fs.hpp"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

using std::string;
namespace fs = std::filesystem;

static inline string ts_now() {
  auto t =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y%m%d_%H%M%S");
  return oss.str();
}

// 本地最小工具函数，避免包含顺序导致的可见性问题
namespace {
inline std::string pstr(const fs::path &p) {
  return p.lexically_normal().generic_string(); // use forward slashes
}
inline std::string util_quote(const std::string &s) {
  return std::string("\"") + s + "\"";
}

inline bool util_write_text(const fs::path &p, const std::string &text) {
  std::ofstream ofs(p, std::ios::binary);
  if (!ofs)
    return false;
  ofs << text;
  return true;
}

inline std::uintmax_t util_file_size_or(const fs::path &p,
                                        std::uintmax_t def = 0) {
  std::error_code ec;
  auto sz = fs::file_size(p, ec);
  return ec ? def : sz;
}

inline bool util_ensure_dir(const fs::path &d) {
  std::error_code ec;
  if (fs::exists(d, ec))
    return true;
  return fs::create_directories(d, ec);
}
// 读取首帧 Y 平面的直方图（256 bins）。成功返回 true 并填充 hist
inline bool probe_luma_hist_first_frame(const Context &ctx,
                                        std::vector<uint32_t> &hist) {
  if (ctx.cfg.w <= 0 || ctx.cfg.h <= 0)
    return false;
  const size_t y_bytes = (size_t)ctx.cfg.w * (size_t)ctx.cfg.h;
  std::ifstream ifs(ctx.cfg.in_path, std::ios::binary);
  if (!ifs)
    return false;
  std::vector<unsigned char> buf(y_bytes);
  ifs.read(reinterpret_cast<char *>(buf.data()), (std::streamsize)y_bytes);
  if ((size_t)ifs.gcount() != y_bytes)
    return false;
  hist.assign(256, 0);
  for (size_t i = 0; i < y_bytes; ++i) {
    ++hist[buf[i]];
  }
  return true;
}
// 读取首帧 Y 平面，返回最大亮度（8-bit）。失败返回 -1。
inline int probe_luma_max_first_frame(const Context &ctx) {
  if (ctx.cfg.w <= 0 || ctx.cfg.h <= 0)
    return -1;
  const size_t y_bytes = (size_t)ctx.cfg.w * (size_t)ctx.cfg.h;
  std::ifstream ifs(ctx.cfg.in_path, std::ios::binary);
  if (!ifs)
    return -1;
  std::vector<unsigned char> buf(y_bytes);
  ifs.read(reinterpret_cast<char *>(buf.data()), (std::streamsize)y_bytes);
  if ((size_t)ifs.gcount() != y_bytes)
    return -1;
  int m = 0;
  for (size_t i = 0; i < y_bytes; ++i) {
    if (buf[i] > m)
      m = buf[i];
  }
  return m;
}
} // namespace

bool init_context(Context &ctx) {
  fs::path in(ctx.cfg.in_path);
  if (!fs::exists(in)) {
    std::cerr << "input not found\n";
    return false;
  }
  // 基名
  ctx.base = in.stem().string();

  if (ctx.cfg.seed == 0) {
    ctx.cfg.seed = (uint64_t)std::chrono::high_resolution_clock::now()
                       .time_since_epoch()
                       .count();
  }
  ctx.rng.seed(ctx.cfg.seed);

  if (ctx.cfg.out_dir.empty()) {
    ctx.cfg.out_dir = fs::path("out_" + ts_now());
  }
  if (!util_ensure_dir(ctx.cfg.out_dir)) {
    std::cerr << "cannot create out dir\n";
    return false;
  }

  // 估算总帧数（仅 yuv420p 8-bit）
  if (ctx.cfg.pix != "yuv420p") {
    std::cerr << "[warn] frame count estimation assumes yuv420p 8-bit.\n";
  }
  const uint64_t bytes_per_frame = (uint64_t)ctx.cfg.w * ctx.cfg.h * 3 / 2;
  const uint64_t sz = util_file_size_or(in);
  ctx.total_frames = (bytes_per_frame > 0) ? (size_t)(sz / bytes_per_frame) : 0;

  return true;
}

string rand_suffix(Context &ctx) {
  std::uniform_int_distribution<int> d(0, 25);
  char a = 'a' + d(ctx.rng);
  char b = 'a' + d(ctx.rng);
  char c = 'a' + d(ctx.rng);
  return string() + a + b + c;
}

static string outname(const Context &ctx, const string &suf) {
  return ctx.base + "_" + suf + ".mp4";
}

static std::vector<string> base_in_args(const Context &ctx) {
  return {ctx.cfg.ffmpeg,
          "-hide_banner",
          "-y",
          "-s",
          std::to_string(ctx.cfg.w) + "x" + std::to_string(ctx.cfg.h),
          "-pix_fmt",
          ctx.cfg.pix,
          "-r",
          std::to_string(ctx.cfg.fps),
          "-f",
          "rawvideo",
          "-i",
          pstr(fs::absolute(ctx.cfg.in_path))};
}

bool make_blocky(Context &ctx, std::vector<OutFile> &outs) {
  // 低码率+快速预设：通过编码器压缩产生块状/马赛克伪影（更贴近解码/传输失真）
  string vf = "scale=trunc(iw/2)*2:trunc(ih/2)*2"; // 保证偶数尺寸
  string suf = rand_suffix(ctx);
  string out = pstr(fs::absolute(ctx.cfg.out_dir / outname(ctx, suf)));

  auto cmd = base_in_args(ctx);
  cmd.insert(cmd.end(), {"-vf", vf, "-c:v", "libx264", "-b:v", "500k",
                         "-preset", "veryfast", out});
  if (run_cmd(cmd) != 0) {
    outs.push_back(
        {fs::path(out).filename().string(), "bitrate_blocky", "FAILED"});
    return false;
  }

  outs.push_back({fs::path(out).filename().string(), "bitrate_blocky",
                  "b=500k preset=veryfast"});
  return true;
}

bool make_brightness(Context &ctx, std::vector<OutFile> &outs) {
  // 微亮度偏移：-3..+3（8-bit）
  std::uniform_int_distribution<int> d(-3, 3);
  int delta = d(ctx.rng);
  string vf = "lutyuv=y='clip(val+" + std::to_string(delta) +
              ",0,255)',scale=trunc(iw/2)*2:trunc(ih/2)*2";
  string suf = rand_suffix(ctx);
  string out = pstr(fs::absolute(ctx.cfg.out_dir / outname(ctx, suf)));

  auto cmd = base_in_args(ctx);
  cmd.insert(cmd.end(), {"-vf", vf, "-c:v", "libx264", "-crf", "22", out});
  if (run_cmd(cmd) != 0) {
    outs.push_back(
        {fs::path(out).filename().string(), "brightness_drift", "FAILED"});
    return false;
  }

  outs.push_back({fs::path(out).filename().string(), "brightness_drift",
                  "delta_Y=" + std::to_string(delta) + " (global)"});
  return true;
}

bool make_jitter(Context &ctx, std::vector<OutFile> &outs) {
  // 轻微抖动：每 K 帧触发两帧“往返”抖动（±1px），pad 黑边后裁回，保证分辨率一致
  std::uniform_int_distribution<int> dk(5, 10);
  std::uniform_int_distribution<int> ds(0, 1); // 0:h, 1:v
  std::uniform_int_distribution<int> dd(0, 1); // 0:正向(右/下), 1:反向(左/上)
  int K = dk(ctx.rng);
  bool horiz = ds(ctx.rng) == 0;
  bool forward = dd(ctx.rng) == 0;
  std::ostringstream vfoss;
  if (horiz) {
    // 环绕移位（水平）：构建一条 wrap 分支 [sh]，并在触发帧 overlay 到原始帧
    if (forward) {
      // 右移 1px：右侧 1 列 + 左侧其余
      vfoss << "split[o][s];[s]split[s0][s1];"
            << "[s0]crop=iw-1:ih:0:0[left];"
            << "[s1]crop=1:ih:iw-1:0[edge];"
            << "[edge][left]hstack=inputs=2[sh];"
            << "[o][sh]overlay=x=0:y=0:enable='eq(mod(n\\," << K << ")\\,0)'";
    } else {
      // 左移 1px：右侧其余 + 左侧 1 列
      vfoss << "split[o][s];[s]split[s0][s1];"
            << "[s0]crop=iw-1:ih:1:0[right];"
            << "[s1]crop=1:ih:0:0[edge];"
            << "[right][edge]hstack=inputs=2[sh];"
            << "[o][sh]overlay=x=0:y=0:enable='eq(mod(n\\," << K << ")\\,0)'";
    }
  } else {
    // 环绕移位（垂直）
    if (forward) {
      // 下移 1px：底部 1 行 + 顶部其余
      vfoss << "split[o][s];[s]split[s0][s1];"
            << "[s0]crop=iw:ih-1:0:0[top];"
            << "[s1]crop=iw:1:0:ih-1[edge];"
            << "[edge][top]vstack=inputs=2[sh];"
            << "[o][sh]overlay=x=0:y=0:enable='eq(mod(n\\," << K << ")\\,0)'";
    } else {
      // 上移 1px：底部其余 + 顶部 1 行
      vfoss << "split[o][s];[s]split[s0][s1];"
            << "[s0]crop=iw:ih-1:0:1[bottom];"
            << "[s1]crop=iw:1:0:0[edge];"
            << "[bottom][edge]vstack=inputs=2[sh];"
            << "[o][sh]overlay=x=0:y=0:enable='eq(mod(n\\," << K << ")\\,0)'";
    }
  }
  // 在 4:4:4 下安全地做 1px 切分/拼接，结束后转回 4:2:0 并保持偶数尺寸
  std::ostringstream finalvf;
  finalvf << "format=yuv444p," << vfoss.str()
          << ",format=yuv420p,scale=trunc(iw/2)*2:trunc(ih/2)*2";
  string vf = finalvf.str();

  string suf = rand_suffix(ctx);
  string out = pstr(fs::absolute(ctx.cfg.out_dir / outname(ctx, suf)));
  auto cmd = base_in_args(ctx);
  cmd.insert(cmd.end(), {"-vf", vf, "-c:v", "libx264", "-crf", "22", out});
  if (run_cmd(cmd) != 0) {
    outs.push_back({fs::path(out).filename().string(), "jitter_1px", "FAILED"});
    return false;
  }

  std::ostringstream det;
  det << (horiz ? "dir=horiz" : "dir=vert")
      << ", wrap=on, shift=1px, period=" << K << ", sense="
      << (forward ? (horiz ? "right" : "down") : (horiz ? "left" : "up"));
  outs.push_back({fs::path(out).filename().string(), "jitter_1px", det.str()});
  return true;
}

bool make_smooth(Context &ctx, std::vector<OutFile> &outs) {
  // 轻度平滑（尽量不毁纹理，强调边缘平滑）：gblur 小 sigma
  std::uniform_real_distribution<double> ds(0.7, 1.4);
  double sigma = ds(ctx.rng);
  std::ostringstream vf;
  vf << "gblur=sigma=" << std::fixed << std::setprecision(2) << sigma
     << ",scale=trunc(iw/2)*2:trunc(ih/2)*2";
  string suf = rand_suffix(ctx);
  string out = pstr(fs::absolute(ctx.cfg.out_dir / outname(ctx, suf)));

  auto cmd = base_in_args(ctx);
  cmd.insert(cmd.end(),
             {"-vf", vf.str(), "-c:v", "libx264", "-crf", "23", out});
  if (run_cmd(cmd) != 0) {
    outs.push_back(
        {fs::path(out).filename().string(), "edge_oversmooth", "FAILED"});
    return false;
  }

  std::ostringstream d;
  d << "sigma=" << std::setprecision(2) << sigma;
  outs.push_back(
      {fs::path(out).filename().string(), "edge_oversmooth", d.str()});
  return true;
}

bool make_highclip(Context &ctx, std::vector<OutFile> &outs) {
  // 高光裁剪：自适应阈值，尽量确保至少某些区域被 clip
  int y_max = probe_luma_max_first_frame(ctx);
  int T = 240; // 回退默认
  if (y_max >= 2) {
    // 先尝试基于直方图的 top-X%（1%~5%）定位阈值，确保“大片”高光区域被裁剪
    std::vector<uint32_t> hist;
    if (probe_luma_hist_first_frame(ctx, hist)) {
      uint64_t total = 0;
      for (uint32_t v : hist)
        total += v;
      std::uniform_real_distribution<double> pct(0.01, 0.05);
      double target = pct(ctx.rng);
      uint64_t cutoff = (uint64_t)(total * (1.0 - target));
      uint64_t acc = 0;
      int cdf_idx = 255;
      for (int i = 0; i < 256; ++i) {
        acc += hist[i];
        if (acc >= cutoff) {
          cdf_idx = i;
          break;
        }
      }
      int lower = std::max(8, cdf_idx - 20);
      int upper =
          std::min(250, std::max(lower + 1, std::min(y_max - 1, cdf_idx)));
      std::uniform_int_distribution<int> th(lower, upper);
      T = th(ctx.rng);
    } else {
      int lower = std::max(10, y_max - 40);
      int upper = std::min(250, std::max(lower + 1, y_max - 1));
      std::uniform_int_distribution<int> th(lower, upper);
      T = th(ctx.rng);
    }
  }
  string vf = "lutyuv=y='if(gte(val\\," + std::to_string(T) +
              ")\\,255\\,val)',scale=trunc(iw/2)*2:trunc(ih/2)*2";
  string suf = rand_suffix(ctx);
  string out = pstr(fs::absolute(ctx.cfg.out_dir / outname(ctx, suf)));

  auto cmd = base_in_args(ctx);
  cmd.insert(cmd.end(), {"-vf", vf, "-c:v", "libx264", "-crf", "22", out});
  if (run_cmd(cmd) != 0) {
    outs.push_back(
        {fs::path(out).filename().string(), "highlight_clip", "FAILED"});
    return false;
  }

  outs.push_back({fs::path(out).filename().string(), "highlight_clip",
                  "Y_threshold=" + std::to_string(T)});
  return true;
}

bool make_chroma_bleed(Context &ctx, std::vector<OutFile> &outs) {
  // 在若干短帧段制造色度错位（chroma-bleeding）
  if (ctx.total_frames == 0) {
    std::cerr << "[warn] total_frames unknown; assuming short video\n";
  }
  int N = (int)ctx.total_frames;
  std::uniform_int_distribution<int> nseg(1, 3);
  std::uniform_int_distribution<int> seglen(2, 5);
  std::uniform_int_distribution<int> shiftH(1, 2); // 1~2 px
  std::uniform_int_distribution<int> shiftV(0, 1); // 0~1 px
  int S = nseg(ctx.rng);

  // 组合 enable 段
  std::vector<std::pair<int, int>> spans;
  for (int i = 0; i < S; ++i) {
    int len = seglen(ctx.rng);
    int start = (N > 10) ? (int)(std::uniform_int_distribution<int>(
                               5, std::max(5, N - len - 5))(ctx.rng))
                         : 0;
    spans.push_back({start, start + len});
  }
  int cbh = shiftH(ctx.rng), crh = -shiftH(ctx.rng);
  int cbv = shiftV(ctx.rng), crv = -shiftV(ctx.rng);

  // enable 表达式： between(n,a,b)+between(n,c,d)+...
  std::ostringstream en;
  for (size_t i = 0; i < spans.size(); ++i) {
    if (i)
      en << " + ";
    en << "between(n\\," << spans[i].first << "\\," << spans[i].second << ")";
  }

  // chromashift + 轻度 chroma 模糊
  std::ostringstream vf;
  vf << "chromashift=cbh=" << cbh << ":crh=" << crh << ":cbv=" << cbv
     << ":crv=" << crv << ":enable='" << en.str() << "',"
     << "boxblur=0:1:enable='" << en.str() << "',"
     << "scale=trunc(iw/2)*2:trunc(ih/2)*2";

  string suf = rand_suffix(ctx);
  string out = pstr(fs::absolute(ctx.cfg.out_dir / outname(ctx, suf)));

  auto cmd = base_in_args(ctx);
  cmd.insert(cmd.end(),
             {"-vf", vf.str(), "-c:v", "libx264", "-crf", "22", out});
  if (run_cmd(cmd) != 0) {
    outs.push_back(
        {fs::path(out).filename().string(), "chroma_bleed", "FAILED"});
    return false;
  }

  std::ostringstream det;
  det << "frames=";
  for (size_t i = 0; i < spans.size(); ++i) {
    if (i)
      det << ",";
    det << "[" << spans[i].first << ".." << spans[i].second << "]";
  }
  det << " cb_h=" << cbh << " cr_h=" << crh << " cb_v=" << cbv
      << " cr_v=" << crv;
  outs.push_back(
      {fs::path(out).filename().string(), "chroma_bleed", det.str()});
  return true;
}

bool make_luma_bleed(Context &ctx, std::vector<OutFile> &outs) {
  // 与 chroma_bleed 一致：在若干短帧段内启用轻度亮度拖影（ghosting-like）
  if (ctx.total_frames == 0) {
    std::cerr << "[warn] total_frames unknown; assuming short video\n";
  }
  int N = (int)ctx.total_frames;
  std::uniform_int_distribution<int> nseg(1, 3);
  std::uniform_int_distribution<int> seglen(2, 5);
  int S = nseg(ctx.rng);

  // 组合 enable 段
  std::vector<std::pair<int, int>> spans;
  for (int i = 0; i < S; ++i) {
    int len = seglen(ctx.rng);
    int start = (N > 10) ? (int)(std::uniform_int_distribution<int>(
                               5, std::max(5, N - len - 5))(ctx.rng))
                         : 0;
    spans.push_back({start, start + len});
  }
  // 轻度参数
  std::uniform_real_distribution<double> sigmaR(0.5, 0.9);
  std::uniform_real_distribution<double> opR(0.25, 0.35);
  double sigma = sigmaR(ctx.rng);
  double opacity = opR(ctx.rng);

  // enable 表达式： between(n,a,b)+between(n,c,d)+...
  std::ostringstream en;
  for (size_t i = 0; i < spans.size(); ++i) {
    if (i)
      en << " + ";
    en << "between(n\\," << spans[i].first << "\\," << spans[i].second << ")";
  }

  // 构建滤镜链：分流 -> 模糊 -> 混合（仅在 spans 启用）
  std::ostringstream vf;
  vf << "split[y][tmp];[tmp]gblur=sigma=" << std::fixed << std::setprecision(2)
     << sigma << "[blur];[y][blur]blend=all_mode=average:all_opacity="
     << std::setprecision(2) << opacity << ":enable='" << en.str()
     << "',scale=trunc(iw/2)*2:trunc(ih/2)*2";

  string suf = rand_suffix(ctx);
  string out = pstr(fs::absolute(ctx.cfg.out_dir / outname(ctx, suf)));
  auto cmd = base_in_args(ctx);
  cmd.insert(cmd.end(),
             {"-vf", vf.str(), "-c:v", "libx264", "-crf", "22", out});
  if (run_cmd(cmd) != 0) {
    outs.push_back({fs::path(out).filename().string(), "luma_bleed", "FAILED"});
    return false;
  }

  std::ostringstream det;
  det << "frames=";
  for (size_t i = 0; i < spans.size(); ++i) {
    if (i)
      det << ",";
    det << "[" << spans[i].first << ".." << spans[i].second << "]";
  }
  det << " sigma=" << std::setprecision(2) << sigma
      << " opacity=" << std::setprecision(2) << opacity;
  outs.push_back({fs::path(out).filename().string(), "luma_bleed", det.str()});
  return true;
}

bool make_grain(Context &ctx, std::vector<OutFile> &outs) {
  // 添加轻度胶片颗粒：noise + 轻微 sharpen，保持偶数尺寸
  std::uniform_int_distribution<int> nstr(2, 6); // 基础强度 2..6
  int s = nstr(ctx.rng) * 5;                     // 转为 10..30 更可见
  uint32_t allowedSeed = (uint32_t)(ctx.cfg.seed % 2147480000ULL);
  std::ostringstream vf;
  vf << "noise=alls=" << s << ":allf=t+u:all_seed=" << allowedSeed
     << ",unsharp=lx=3:ly=3:la=0.2:cx=3:cy=3:ca=0.0,scale=trunc(iw/"
        "2)*2:trunc(ih/2)*2";
  string suf = rand_suffix(ctx);
  string out = pstr(fs::absolute(ctx.cfg.out_dir / outname(ctx, suf)));
  auto cmd = base_in_args(ctx);
  cmd.insert(cmd.end(),
             {"-vf", vf.str(), "-c:v", "libx264", "-crf", "22", out});
  if (run_cmd(cmd) != 0) {
    outs.push_back({fs::path(out).filename().string(), "grain", "FAILED"});
    return false;
  }
  outs.push_back({fs::path(out).filename().string(), "grain", "noise+unsharp"});
  return true;
}

bool make_ringing(Context &ctx, std::vector<OutFile> &outs) {
  // 模拟振铃：先锐化再轻度去块，或通过 oversharp + deblock
  std::ostringstream vf;
  vf << "unsharp=lx=5:ly=5:la=1.2:cx=5:cy=5:ca=0.6,deblock=alpha=0.2:beta=0.2,"
        "scale=trunc(iw/2)*2:trunc(ih/2)*2";
  string suf = rand_suffix(ctx);
  string out = pstr(fs::absolute(ctx.cfg.out_dir / outname(ctx, suf)));
  auto cmd = base_in_args(ctx);
  cmd.insert(cmd.end(),
             {"-vf", vf.str(), "-c:v", "libx264", "-crf", "22", out});
  if (run_cmd(cmd) != 0) {
    outs.push_back({fs::path(out).filename().string(), "ringing", "FAILED"});
    return false;
  }
  outs.push_back(
      {fs::path(out).filename().string(), "ringing", "unsharp+deblock"});
  return true;
}

bool make_banding(Context &ctx, std::vector<OutFile> &outs) {
  // 模拟色带：降低量化或抬升 posterize，在 Y 通道减少级别，再适度模糊
  std::uniform_int_distribution<int> pow2(3, 6); // 2^3=8 .. 2^6=64
  int levels = 1 << pow2(ctx.rng);
  std::ostringstream vf;
  vf << "lutyuv=y='trunc(val/" << levels << ")*" << levels
     << "',gblur=sigma=0.4,"
     << "scale=trunc(iw/2)*2:trunc(ih/2)*2";
  string suf = rand_suffix(ctx);
  string out = pstr(fs::absolute(ctx.cfg.out_dir / outname(ctx, suf)));
  auto cmd = base_in_args(ctx);
  cmd.insert(cmd.end(),
             {"-vf", vf.str(), "-c:v", "libx264", "-crf", "22", out});
  if (run_cmd(cmd) != 0) {
    outs.push_back({fs::path(out).filename().string(), "banding", "FAILED"});
    return false;
  }
  outs.push_back({fs::path(out).filename().string(), "banding",
                  std::string("levels=") + std::to_string(levels)});
  return true;
}

bool make_ghosting(Context &ctx, std::vector<OutFile> &outs) {
  // 轻度 ghosting：tblend 轻微平均，产生时域残影
  // 注意：tblend 需要至少两帧才起效
  std::uniform_real_distribution<double> op(0.25, 0.35);
  double opacity = op(ctx.rng);
  std::ostringstream vf;
  vf << "tblend=all_mode=average:all_opacity=" << std::fixed
     << std::setprecision(2) << opacity << ",scale=trunc(iw/2)*2:trunc(ih/2)*2";
  string suf = rand_suffix(ctx);
  string out = pstr(fs::absolute(ctx.cfg.out_dir / outname(ctx, suf)));
  auto cmd = base_in_args(ctx);
  cmd.insert(cmd.end(),
             {"-vf", vf.str(), "-c:v", "libx264", "-crf", "22", out});
  if (run_cmd(cmd) != 0) {
    outs.push_back({fs::path(out).filename().string(), "ghosting", "FAILED"});
    return false;
  }
  std::ostringstream det;
  det << "opacity=" << std::setprecision(2) << opacity;
  outs.push_back({fs::path(out).filename().string(), "ghosting", det.str()});
  return true;
}

bool make_colorspace_mismatch(Context &ctx, std::vector<OutFile> &outs) {
  // 在解码/过滤阶段假设 BT.709，输出标记/转换为
  // BT.601（或反之），制造色彩空间错配 这里选择统一将输入当作 bt709
  // 解码，然后在编码输出时标记/转换为 bt601，产生轻微色偏 注：不同 ffmpeg
  // 版本/构建对 colorspace 标记/转换的行为略有差异，取尽量通用的写法
  std::uniform_int_distribution<int> mode(0, 1);
  bool to601 = mode(ctx.rng) == 0; // 半数转 709->601，半数 601->709
  const char *inspace = to601 ? "bt709" : "smpte170m"; // 输入假定
  const char *outspace =
      to601 ? "smpte170m" : "bt709"; // 输出目标：用 smpte170m 表征 bt601

  // 使用 colormatrix 进行 709<->601 转换（广泛可用），再做偶数尺寸缩放
  std::ostringstream vf;
  vf << "colormatrix=src=" << (to601 ? "bt709" : "bt601")
     << ":dst=" << (to601 ? "bt601" : "bt709")
     << ",scale=trunc(iw/2)*2:trunc(ih/2)*2";

  string suf = rand_suffix(ctx);
  string out = pstr(fs::absolute(ctx.cfg.out_dir / outname(ctx, suf)));
  auto cmd = base_in_args(ctx);
  cmd.insert(cmd.end(),
             {"-vf", vf.str(), "-c:v", "libx264", "-crf", "22", out});
  if (run_cmd(cmd) != 0) {
    outs.push_back(
        {fs::path(out).filename().string(), "colorspace_mismatch", "FAILED"});
    return false;
  }
  std::ostringstream det;
  det << inspace << "->" << outspace;
  outs.push_back(
      {fs::path(out).filename().string(), "colorspace_mismatch", det.str()});
  return true;
}

bool make_repeat(Context &ctx, std::vector<OutFile> &outs) {
  // 纯 ffmpeg 滤镜：在位置 p 将该帧重复 r 次，并丢弃其后的 r 帧，保持总帧数一致
  int N = (int)ctx.total_frames;
  if (N <= 0) {
    N = 200; // 回退估计
  }

  std::uniform_int_distribution<int> rep(2, 8); // 重复次数 r
  int r = rep(ctx.rng);
  int safe = std::max(5, r + 1);
  std::uniform_int_distribution<int> pick(safe, std::max(safe, N - safe - 1));
  int p = pick(ctx.rng);
  int drop_end = std::min(p + r, std::max(1, N - 2)); // 丢弃 [p+1..p+r]

  // 构建滤镜：三段 select + loop；各段重置 PTS，从 0 开始；concat 后用 fps
  // 归一到原 fps
  std::ostringstream vf;
  vf << "split=3[v0][v1][v2];"
     << "[v0]select='lte(n\\," << p << ")',setpts=PTS-STARTPTS[a];"
     << "[v1]select='eq(n\\," << p << ")',loop=" << r
     << ":1:0,setpts=PTS-STARTPTS[b];"
     << "[v2]select='gt(n\\," << drop_end << ")',setpts=PTS-STARTPTS[c];"
     << "[a][b][c]concat=n=3:v=1:a=0,fps=" << ctx.cfg.fps
     << ",scale=trunc(iw/2)*2:trunc(ih/2)*2";

  string suf = rand_suffix(ctx);
  string out = pstr(fs::absolute(ctx.cfg.out_dir / outname(ctx, suf)));

  auto cmd = base_in_args(ctx);
  cmd.insert(cmd.end(), {"-fflags", "+genpts", "-vsync", "cfr", "-r",
                         std::to_string(ctx.cfg.fps), "-vf", vf.str(), "-c:v",
                         "libx264", "-crf", "22", out});
  if (run_cmd(cmd) != 0) {
    outs.push_back({fs::path(out).filename().string(),
                    "repeat_frames_keep_count", "FAILED"});
    return false;
  }

  std::ostringstream det;
  det << "repeat_at=" << p << " times=" << r << " drop=[" << (p + 1) << ".."
      << drop_end << "]";
  outs.push_back({fs::path(out).filename().string(), "repeat_frames_keep_count",
                  det.str()});
  return true;
}

bool make_all(Context &ctx, std::vector<OutFile> &outs) {
  // 顺序可自由调整
  bool ok = true;
  ok &= make_blocky(ctx, outs);
  ok &= make_brightness(ctx, outs);
  ok &= make_jitter(ctx, outs);
  ok &= make_smooth(ctx, outs);
  ok &= make_highclip(ctx, outs);
  ok &= make_chroma_bleed(ctx, outs);
  ok &= make_luma_bleed(ctx, outs);
  ok &= make_grain(ctx, outs);
  ok &= make_ringing(ctx, outs);
  ok &= make_banding(ctx, outs);
  ok &= make_ghosting(ctx, outs);
  ok &= make_colorspace_mismatch(ctx, outs);
  ok &= make_repeat(ctx, outs);
  return ok;
}

bool write_manifest(const Context &ctx, const std::vector<OutFile> &outs) {
  fs::path man = ctx.cfg.out_dir / "manifest.txt";
  std::ostringstream ss;
  ss << "seed=" << ctx.cfg.seed << "\n";
  ss << "input=" << ctx.cfg.in_path << "\n";
  ss << "size=" << ctx.cfg.w << "x" << ctx.cfg.h << " pix=" << ctx.cfg.pix
     << " fps=" << ctx.cfg.fps << "\n";
  ss << "total_frames~=" << ctx.total_frames << " (assume yuv420p 8-bit)\n";
  ss << "outputs:\n";
  for (auto &o : outs) {
    ss << "  - " << o.filename << " | " << o.kind << " | " << o.details << "\n";
  }
  // 统计失败项
  size_t failed = 0;
  for (auto &o : outs)
    if (o.details == "FAILED")
      ++failed;
  if (failed > 0)
    ss << "failed_count=" << failed << "\n";
  return util_write_text(man, ss.str());
}
