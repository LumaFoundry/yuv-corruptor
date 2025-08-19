#include "Defects.hpp"
#include <filesystem>
#include <iostream>
#include <regex>

static void usage() {
  std::cout
      << "Usage:\n"
         "  yuv-corruptor <input.yuv> -r WxH [-f fps] [-p pixfmt] [-s seed]\n"
         "                  [-t types] [-o outdir] [--ffmpeg ffmpeg] "
         "[--ffprobe ffprobe]\n"
         "\n"
         "Positional:\n"
         "  <input.yuv>           Path to raw YUV file (8-bit by default)\n"
         "\n"
         "Flags:\n"
         "  -r WxH                Resolution, e.g. -r 176x144 (if omitted, try "
         "infer from filename like *_352x288_30_*.yuv)\n"
         "  -f fps                Frame rate (default 30)\n"
         "  -p pixfmt             Pixel format (default yuv420p)\n"
         "  -s seed               RNG seed (uint64). Default: time-based\n"
         "  -t types              CSV in "
         "{blocky,brightness,jitter,smooth,highclip,chroma,luma,grain,ringing,"
         "banding,ghosting,colorspace,repeat,all}\n"
         "  -o outdir             Output directory (default out_<timestamp>)\n"
         "  --ffmpeg <path>       ffmpeg executable (default: ffmpeg in PATH)\n"
         "  --ffprobe <path>      ffprobe executable (default: ffprobe in "
         "PATH)\n"
         "\n"
         "Backward compatible (optional): "
         "--in/--w/--h/--fps/--pix/--seed/--types/--out\n";
}

static bool infer_from_filename(const std::string &in_path, int &w, int &h,
                                int &fps) {
  try {
    std::filesystem::path p(in_path);
    std::string name = p.stem().string();
    // Match ..._<W>x<H>[_<FPS>]_...
    std::regex re(
        R"((?:^|[^0-9])([0-9]+)\s*x\s*([0-9]+)(?:[^0-9]+([0-9]{1,3}))?)",
        std::regex::icase);
    std::smatch m;
    if (std::regex_search(name, m, re)) {
      int iw = std::stoi(m[1].str());
      int ih = std::stoi(m[2].str());
      int ifps = fps; // keep incoming default unless group 3 exists
      if (m.size() >= 4 && m[3].matched) {
        try {
          ifps = std::stoi(m[3].str());
        } catch (...) {
        }
      }
      if (iw > 0 && ih > 0) {
        w = iw;
        h = ih;
        fps = ifps;
        return true;
      }
    }
  } catch (...) {
  }
  return false;
}

static bool parse_WxH(const std::string &s, int &w, int &h) {
  std::regex re(R"(^\s*(\d+)\s*x\s*(\d+)\s*$)", std::regex::icase);
  std::smatch m;
  if (!std::regex_match(s, m, re))
    return false;
  try {
    w = std::stoi(m[1].str());
    h = std::stoi(m[2].str());
    return (w > 0 && h > 0);
  } catch (...) {
    return false;
  }
}

int main(int argc, char **argv) {
  Settings s;
  bool ok = true;
  // defaults changed here:
  s.fps = 30;
  s.pix = "yuv420p";

  // Parse
  bool fps_set_by_cli = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&](int n = 1) {
      if (i + n >= argc) {
        ok = false;
        return false;
      }
      return true;
    };

    if (!a.empty() && a[0] != '-') {
      if (s.in_path.empty())
        s.in_path = a;
      else {
        std::cerr << "Unexpected positional arg: " << a << "\n";
        ok = false;
      }
    } else if (a == "-r" && need()) {
      std::string wxh = argv[++i];
      if (!parse_WxH(wxh, s.w, s.h)) {
        std::cerr << "Invalid -r " << wxh << "\n";
        ok = false;
      }
    } else if (a == "-f" && need()) {
      s.fps = std::stoi(argv[++i]);
      fps_set_by_cli = true;
    } else if (a == "-p" && need()) {
      s.pix = argv[++i];
    } else if (a == "-s" && need()) {
      s.seed = std::stoull(argv[++i]);
    } else if (a == "-t" && need()) {
      std::string v = argv[++i];
      s.types.clear();
      size_t p = 0;
      while (true) {
        auto q = v.find(',', p);
        s.types.push_back(v.substr(p, q == std::string::npos ? q : q - p));
        if (q == std::string::npos)
          break;
        p = q + 1;
      }
    } else if (a == "-o" && need()) {
      s.out_dir = argv[++i];
    } else if (a == "--ffmpeg" && need()) {
      s.ffmpeg = argv[++i];
    } else if (a == "--ffprobe" && need()) {
      s.ffprobe = argv[++i];
    }

    // ---- Backward compatible flags (optional) ----
    else if (a == "--in" && need()) {
      s.in_path = argv[++i];
    } else if (a == "--w" && need()) {
      s.w = std::stoi(argv[++i]);
    } else if (a == "--h" && need()) {
      s.h = std::stoi(argv[++i]);
    } else if (a == "--fps" && need()) {
      s.fps = std::stoi(argv[++i]);
      fps_set_by_cli = true;
    } else if (a == "--pix" && need()) {
      s.pix = argv[++i];
    } else if (a == "--seed" && need()) {
      s.seed = std::stoull(argv[++i]);
    } else if (a == "--types" && need()) {
      std::string v = argv[++i];
      s.types.clear();
      size_t p = 0;
      while (true) {
        auto q = v.find(',', p);
        s.types.push_back(v.substr(p, q == std::string::npos ? q : q - p));
        if (q == std::string::npos)
          break;
        p = q + 1;
      }
    } else if (a == "--out" && need()) {
      s.out_dir = argv[++i];
    }

    else {
      std::cerr << "Unknown or incomplete arg: " << a << "\n";
      ok = false;
    }
  }

  // Try infer WxH / fps from filename if missing
  if (ok && !s.in_path.empty() && (s.w <= 0 || s.h <= 0 || !fps_set_by_cli)) {
    int iw = s.w, ih = s.h, ifps = s.fps;
    if (infer_from_filename(s.in_path, iw, ih, ifps)) {
      if (s.w <= 0 || s.h <= 0) {
        s.w = iw;
        s.h = ih;
      }
      if (!fps_set_by_cli) {
        s.fps = ifps;
      }
    }
  }

  // 对 raw YUV 仍需 -r WxH；对 .y4m 可由容器推断尺寸
  auto need_wh = [&]() {
    std::filesystem::path pin(s.in_path);
    std::string ext = pin.extension().string();
    for (auto &c : ext)
      c = (char)std::tolower((unsigned char)c);
    return ext != ".y4m";
  };
  if (!ok || s.in_path.empty() || (need_wh() && (s.w <= 0 || s.h <= 0))) {
    usage();
    return 1;
  }

  Context ctx{s};
  if (!init_context(ctx))
    return 2;

  std::vector<OutFile> outs;
  auto has = [&](const std::string &t) {
    if (s.types.empty() ||
        (s.types.size() == 1 && (s.types[0].empty() || s.types[0] == "all")))
      return true;
    for (auto &x : s.types)
      if (x == t)
        return true;
    return false;
  };

  bool all_ok = true;
  if (has("blocky"))
    all_ok &= make_blocky(ctx, outs);
  if (has("brightness"))
    all_ok &= make_brightness(ctx, outs);
  if (has("jitter"))
    all_ok &= make_jitter(ctx, outs);
  if (has("smooth"))
    all_ok &= make_smooth(ctx, outs);
  if (has("highclip"))
    all_ok &= make_highclip(ctx, outs);
  if (has("chroma"))
    all_ok &= make_chroma_bleed(ctx, outs);
  if (has("luma"))
    all_ok &= make_luma_bleed(ctx, outs);
  if (has("grain"))
    all_ok &= make_grain(ctx, outs);
  if (has("ringing"))
    all_ok &= make_ringing(ctx, outs);
  if (has("banding"))
    all_ok &= make_banding(ctx, outs);
  if (has("ghosting"))
    all_ok &= make_ghosting(ctx, outs);
  if (has("colorspace"))
    all_ok &= make_colorspace_mismatch(ctx, outs);
  if (has("repeat"))
    all_ok &= make_repeat(ctx, outs);

  if (!write_manifest(ctx, outs)) {
    std::cerr << "failed to write manifest\n";
  }

  std::cout << "Done. Outputs in: " << ctx.cfg.out_dir << "\n";
  return all_ok ? 0 : 3;
}
