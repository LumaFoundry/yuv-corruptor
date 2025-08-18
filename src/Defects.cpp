#include "Defects.hpp"
#include "Fs.hpp"
#include <chrono>
#include <sstream>
#include <iomanip>
#include <cassert>
#include <algorithm>
#include <fstream>
#include <ctime>
#include <iostream>

using std::string; namespace fs = std::filesystem;

static inline string ts_now() {
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss<<std::put_time(&tm,"%Y%m%d_%H%M%S");
    return oss.str();
}

// 本地最小工具函数，避免包含顺序导致的可见性问题
namespace {
    inline std::string pstr(const fs::path& p) {
        return p.lexically_normal().generic_string(); // use forward slashes
    }
    inline std::string util_quote(const std::string& s) {
        return std::string("\"") + s + "\"";
    }

    inline bool util_write_text(const fs::path& p, const std::string& text) {
        std::ofstream ofs(p, std::ios::binary);
        if (!ofs) return false;
        ofs << text;
        return true;
    }

    inline std::uintmax_t util_file_size_or(const fs::path& p, std::uintmax_t def=0) {
        std::error_code ec; auto sz = fs::file_size(p, ec);
        return ec ? def : sz;
    }

    inline bool util_ensure_dir(const fs::path& d) {
        std::error_code ec;
        if (fs::exists(d, ec)) return true;
        return fs::create_directories(d, ec);
    }
}

bool init_context(Context& ctx) {
    fs::path in(ctx.cfg.in_path);
    if (!fs::exists(in)) { std::cerr<<"input not found\n"; return false; }
    // 基名
    ctx.base = in.stem().string();

    if (ctx.cfg.seed==0) {
        ctx.cfg.seed = (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
    }
    ctx.rng.seed(ctx.cfg.seed);

    if (ctx.cfg.out_dir.empty()) {
        ctx.cfg.out_dir = fs::path("out_" + ts_now());
    }
    if (!util_ensure_dir(ctx.cfg.out_dir)) { std::cerr<<"cannot create out dir\n"; return false; }

    // 估算总帧数（仅 yuv420p 8-bit）
    if (ctx.cfg.pix != "yuv420p") {
        std::cerr<<"[warn] frame count estimation assumes yuv420p 8-bit.\n";
    }
    const uint64_t bytes_per_frame = (uint64_t)ctx.cfg.w * ctx.cfg.h * 3 / 2;
    const uint64_t sz = util_file_size_or(in);
    ctx.total_frames = (bytes_per_frame>0) ? (size_t)(sz / bytes_per_frame) : 0;

    return true;
}

string rand_suffix(Context& ctx) {
    std::uniform_int_distribution<int> d(0,25);
    char a = 'a' + d(ctx.rng);
    char b = 'a' + d(ctx.rng);
    char c = 'a' + d(ctx.rng);
    return string() + a + b + c;
}

static string outname(const Context& ctx, const string& suf) {
    return ctx.base + "_" + suf + ".mp4";
}

static std::vector<string> base_in_args(const Context& ctx) {
    return {
        ctx.cfg.ffmpeg,
        "-hide_banner", "-y",
        "-s", std::to_string(ctx.cfg.w) + "x" + std::to_string(ctx.cfg.h),
        "-pix_fmt", ctx.cfg.pix,
        "-r", std::to_string(ctx.cfg.fps),
        "-f", "rawvideo",
        "-i", pstr(fs::absolute(ctx.cfg.in_path))
    };
}

bool make_blocky(Context& ctx, std::vector<OutFile>& outs) {
    // factor：缩到 1/f 后再放大，邻近采样制造块感
    std::uniform_int_distribution<int> df(6, 12);
    int f = df(ctx.rng);
    // 先缩小到固定整数偶数尺寸，再邻近放大回到输入分辨率（偶数）
    int dw = std::max(2, ((ctx.cfg.w / f) / 2) * 2);
    int dh = std::max(2, ((ctx.cfg.h / f) / 2) * 2);
    int ow = (ctx.cfg.w / 2) * 2;
    int oh = (ctx.cfg.h / 2) * 2;
    std::ostringstream vfb;
    vfb << "scale=" << dw << ":" << dh << ":flags=bilinear,"
        << "scale=" << ow << ":" << oh << ":flags=neighbor";
    string vf = vfb.str();
    string suf = rand_suffix(ctx);
    string out = pstr(fs::absolute(ctx.cfg.out_dir / outname(ctx, suf)));

    auto cmd = base_in_args(ctx);
    cmd.insert(cmd.end(), {"-vf", vf, "-c:v","libx264","-crf","28", out});
    if (run_cmd(cmd)!=0) { outs.push_back({fs::path(out).filename().string(), "lowres_blocky", "FAILED"}); return false; }

    outs.push_back({fs::path(out).filename().string(), "lowres_blocky",
        "down/up factor="+std::to_string(f)});
    return true;
}

bool make_brightness(Context& ctx, std::vector<OutFile>& outs) {
    // 微亮度偏移：-3..+3（8-bit）
    std::uniform_int_distribution<int> d(-3,3);
    int delta = d(ctx.rng);
    string vf = "lutyuv=y='clip(val+" + std::to_string(delta) + ",0,255)',scale=trunc(iw/2)*2:trunc(ih/2)*2";
    string suf = rand_suffix(ctx);
    string out = pstr(fs::absolute(ctx.cfg.out_dir / outname(ctx, suf)));

    auto cmd = base_in_args(ctx);
    cmd.insert(cmd.end(), {"-vf", vf, "-c:v","libx264","-crf","22", out});
    if (run_cmd(cmd)!=0) { outs.push_back({fs::path(out).filename().string(), "brightness_drift", "FAILED"}); return false; }

    outs.push_back({fs::path(out).filename().string(), "brightness_drift",
        "delta_Y=" + std::to_string(delta) + " (global)"});
    return true;
}

bool make_jitter(Context& ctx, std::vector<OutFile>& outs) {
    // 轻微抖动：每 K 帧偏移 1 像素（用 pad+fillborders 再 crop，避免对 crop 使用 enable）
    std::uniform_int_distribution<int> dk(8, 20);
    std::uniform_int_distribution<int> ds(0,1); // 0:h, 1:v
    int K = dk(ctx.rng);
    bool horiz = ds(ctx.rng)==0;
    std::ostringstream vfoss;
    if (horiz) {
        vfoss << "pad=iw+1:ih:1:0,fillborders=left=1:mode=mirror,"
              << "crop=iw-1:ih:" << "if(eq(mod(n\\," << K << ")\\,0)\\,1\\,0)" << ":0";
    } else {
        vfoss << "pad=iw:ih+1:0:1,fillborders=top=1:mode=mirror,"
              << "crop=iw:ih-1:0:" << "if(eq(mod(n\\," << K << ")\\,0)\\,1\\,0)";
    }
    string vf = vfoss.str() + ",scale=trunc(iw/2)*2:trunc(ih/2)*2";

    string suf = rand_suffix(ctx);
    string out = pstr(fs::absolute(ctx.cfg.out_dir / outname(ctx, suf)));
    auto cmd = base_in_args(ctx);
    cmd.insert(cmd.end(), {"-vf", vf, "-c:v","libx264","-crf","22", out});
    if (run_cmd(cmd)!=0) { outs.push_back({fs::path(out).filename().string(), "jitter_1px", "FAILED"}); return false; }

    outs.push_back({fs::path(out).filename().string(), "jitter_1px",
        (horiz? "dir=horiz":"dir=vert") + (string)", period=" + std::to_string(K)});
    return true;
}

bool make_smooth(Context& ctx, std::vector<OutFile>& outs) {
    // 轻度平滑（尽量不毁纹理，强调边缘平滑）：gblur 小 sigma
    std::uniform_real_distribution<double> ds(0.7, 1.4);
    double sigma = ds(ctx.rng);
    std::ostringstream vf; vf<< "gblur=sigma=" << std::fixed << std::setprecision(2) << sigma << ",scale=trunc(iw/2)*2:trunc(ih/2)*2";
    string suf = rand_suffix(ctx);
    string out = pstr(fs::absolute(ctx.cfg.out_dir / outname(ctx, suf)));

    auto cmd = base_in_args(ctx);
    cmd.insert(cmd.end(), {"-vf", vf.str(), "-c:v","libx264","-crf","23", out});
    if (run_cmd(cmd)!=0) { outs.push_back({fs::path(out).filename().string(), "edge_oversmooth", "FAILED"}); return false; }

    std::ostringstream d; d<<"sigma="<<std::setprecision(2)<<sigma;
    outs.push_back({fs::path(out).filename().string(), "edge_oversmooth", d.str()});
    return true;
}

bool make_highclip(Context& ctx, std::vector<OutFile>& outs) {
    // 高光裁剪：阈值 235..245（8-bit），把更亮的直接推到 255
    std::uniform_int_distribution<int> th(235,245);
    int T = th(ctx.rng);
    string vf = "lutyuv=y='if(gte(val\\,"+std::to_string(T)+")\\,255\\,val)',scale=trunc(iw/2)*2:trunc(ih/2)*2";
    string suf = rand_suffix(ctx);
    string out = pstr(fs::absolute(ctx.cfg.out_dir / outname(ctx, suf)));

    auto cmd = base_in_args(ctx);
    cmd.insert(cmd.end(), {"-vf", vf, "-c:v","libx264","-crf","22", out});
    if (run_cmd(cmd)!=0) { outs.push_back({fs::path(out).filename().string(), "highlight_clip", "FAILED"}); return false; }

    outs.push_back({fs::path(out).filename().string(), "highlight_clip",
        "Y_threshold=" + std::to_string(T)});
    return true;
}

bool make_chroma_bleed(Context& ctx, std::vector<OutFile>& outs) {
    // 在若干短帧段制造色度错位（chroma-bleeding）
    if (ctx.total_frames==0) { std::cerr<<"[warn] total_frames unknown; assuming short video\n"; }
    int N = (int)ctx.total_frames;
    std::uniform_int_distribution<int> nseg(1, 3);
    std::uniform_int_distribution<int> seglen(2, 5);
    std::uniform_int_distribution<int> shiftH(1, 2); // 1~2 px
    std::uniform_int_distribution<int> shiftV(0, 1); // 0~1 px
    int S = nseg(ctx.rng);

    // 组合 enable 段
    std::vector<std::pair<int,int>> spans;
    for (int i=0;i<S;++i){
        int len = seglen(ctx.rng);
        int start = (N>10)? (int)(std::uniform_int_distribution<int>(5, std::max(5,N-len-5))(ctx.rng)) : 0;
        spans.push_back({start, start+len});
    }
    int cbh = shiftH(ctx.rng), crh = -shiftH(ctx.rng);
    int cbv = shiftV(ctx.rng), crv = -shiftV(ctx.rng);

    // enable 表达式： between(n,a,b)+between(n,c,d)+...
    std::ostringstream en;
    for (size_t i=0;i<spans.size();++i){
        if (i) en<<" + ";
        en<<"between(n\\,"<<spans[i].first<<"\\,"<<spans[i].second<<")";
    }

    // chromashift + 轻度 chroma 模糊
    std::ostringstream vf;
    vf << "chromashift=cbh="<<cbh<<":crh="<<crh<<":cbv="<<cbv<<":crv="<<crv
       << ":enable='"<< en.str() <<"',"
       << "boxblur=0:1:enable='"<< en.str() <<"',"
       << "scale=trunc(iw/2)*2:trunc(ih/2)*2";

    string suf = rand_suffix(ctx);
    string out = pstr(fs::absolute(ctx.cfg.out_dir / outname(ctx, suf)));

    auto cmd = base_in_args(ctx);
    cmd.insert(cmd.end(), {"-vf", vf.str(), "-c:v","libx264","-crf","22", out});
    if (run_cmd(cmd)!=0) { outs.push_back({fs::path(out).filename().string(), "chroma_bleed", "FAILED"}); return false; }

    std::ostringstream det;
    det<<"frames=";
    for (size_t i=0;i<spans.size();++i){
        if (i) det<<",";
        det<<"["<<spans[i].first<<".."<<spans[i].second<<"]";
    }
    det<<" cb_h="<<cbh<<" cr_h="<<crh<<" cb_v="<<cbv<<" cr_v="<<crv;
    outs.push_back({fs::path(out).filename().string(), "chroma_bleed", det.str()});
    return true;
}

bool make_luma_bleed(Context& ctx, std::vector<OutFile>& outs) {
    // 与 chroma_bleed 一致：在若干短帧段内启用轻度亮度拖影（ghosting-like）
    if (ctx.total_frames==0) { std::cerr<<"[warn] total_frames unknown; assuming short video\n"; }
    int N = (int)ctx.total_frames;
    std::uniform_int_distribution<int> nseg(1, 3);
    std::uniform_int_distribution<int> seglen(2, 5);
    int S = nseg(ctx.rng);

    // 组合 enable 段
    std::vector<std::pair<int,int>> spans;
    for (int i=0;i<S;++i){
        int len = seglen(ctx.rng);
        int start = (N>10)? (int)(std::uniform_int_distribution<int>(5, std::max(5,N-len-5))(ctx.rng)) : 0;
        spans.push_back({start, start+len});
    }
    // 轻度参数
    std::uniform_real_distribution<double> sigmaR(0.5, 0.9);
    std::uniform_real_distribution<double> opR(0.25, 0.35);
    double sigma = sigmaR(ctx.rng);
    double opacity = opR(ctx.rng);

    // enable 表达式： between(n,a,b)+between(n,c,d)+...
    std::ostringstream en;
    for (size_t i=0;i<spans.size();++i){ if (i) en<<" + "; en<<"between(n\\,"<<spans[i].first<<"\\,"<<spans[i].second<<")"; }

    // 构建滤镜链：分流 -> 模糊 -> 混合（仅在 spans 启用）
    std::ostringstream vf;
    vf << "split[y][tmp];[tmp]gblur=sigma=" << std::fixed << std::setprecision(2) << sigma
       << "[blur];[y][blur]blend=all_mode=average:all_opacity=" << std::setprecision(2) << opacity
       << ":enable='" << en.str() << "',scale=trunc(iw/2)*2:trunc(ih/2)*2";

    string suf = rand_suffix(ctx);
    string out = pstr(fs::absolute(ctx.cfg.out_dir / outname(ctx, suf)));
    auto cmd = base_in_args(ctx);
    cmd.insert(cmd.end(), {"-vf", vf.str(), "-c:v","libx264","-crf","22", out});
    if (run_cmd(cmd)!=0) { outs.push_back({fs::path(out).filename().string(), "luma_bleed", "FAILED"}); return false; }

    std::ostringstream det;
    det<<"frames=";
    for (size_t i=0;i<spans.size();++i){ if (i) det<<","; det<<"["<<spans[i].first<<".."<<spans[i].second<<"]"; }
    det<<" sigma="<<std::setprecision(2)<<sigma<<" opacity="<<std::setprecision(2)<<opacity;
    outs.push_back({fs::path(out).filename().string(), "luma_bleed", det.str()});
    return true;
}

bool make_grain(Context& ctx, std::vector<OutFile>& outs) {
    // 添加轻度胶片颗粒：noise + 轻微 sharpen，保持偶数尺寸
    std::uniform_int_distribution<int> nstr(2, 6); // 基础强度 2..6
    int s = nstr(ctx.rng) * 5; // 转为 10..30 更可见
    uint32_t allowedSeed = (uint32_t)(ctx.cfg.seed % 2147480000ULL);
    std::ostringstream vf;
    vf << "noise=alls=" << s << ":allf=t+u:all_seed=" << allowedSeed
       << ",unsharp=lx=3:ly=3:la=0.2:cx=3:cy=3:ca=0.0,scale=trunc(iw/2)*2:trunc(ih/2)*2";
    string suf = rand_suffix(ctx);
    string out = pstr(fs::absolute(ctx.cfg.out_dir / outname(ctx, suf)));
    auto cmd = base_in_args(ctx);
    cmd.insert(cmd.end(), {"-vf", vf.str(), "-c:v","libx264","-crf","22", out});
    if (run_cmd(cmd)!=0) { outs.push_back({fs::path(out).filename().string(), "grain", "FAILED"}); return false; }
    outs.push_back({fs::path(out).filename().string(), "grain", "noise+unsharp"});
    return true;
}

bool make_ringing(Context& ctx, std::vector<OutFile>& outs) {
    // 模拟振铃：先锐化再轻度去块，或通过 oversharp + deblock
    std::ostringstream vf;
    vf << "unsharp=lx=5:ly=5:la=1.2:cx=5:cy=5:ca=0.6,deblock=alpha=0.2:beta=0.2,scale=trunc(iw/2)*2:trunc(ih/2)*2";
    string suf = rand_suffix(ctx);
    string out = pstr(fs::absolute(ctx.cfg.out_dir / outname(ctx, suf)));
    auto cmd = base_in_args(ctx);
    cmd.insert(cmd.end(), {"-vf", vf.str(), "-c:v","libx264","-crf","22", out});
    if (run_cmd(cmd)!=0) { outs.push_back({fs::path(out).filename().string(), "ringing", "FAILED"}); return false; }
    outs.push_back({fs::path(out).filename().string(), "ringing", "unsharp+deblock"});
    return true;
}

bool make_banding(Context& ctx, std::vector<OutFile>& outs) {
    // 模拟色带：降低量化或抬升 posterize，在 Y 通道减少级别，再适度模糊
    std::uniform_int_distribution<int> pow2(3, 6); // 2^3=8 .. 2^6=64
    int levels = 1 << pow2(ctx.rng);
    std::ostringstream vf;
    vf << "lutyuv=y='trunc(val/" << levels << ")*" << levels << "',gblur=sigma=0.4,"
       << "scale=trunc(iw/2)*2:trunc(ih/2)*2";
    string suf = rand_suffix(ctx);
    string out = pstr(fs::absolute(ctx.cfg.out_dir / outname(ctx, suf)));
    auto cmd = base_in_args(ctx);
    cmd.insert(cmd.end(), {"-vf", vf.str(), "-c:v","libx264","-crf","22", out});
    if (run_cmd(cmd)!=0) { outs.push_back({fs::path(out).filename().string(), "banding", "FAILED"}); return false; }
    outs.push_back({fs::path(out).filename().string(), "banding", std::string("levels=")+std::to_string(levels)});
    return true;
}

bool make_ghosting(Context& ctx, std::vector<OutFile>& outs) {
    // 轻度 ghosting：tblend 轻微平均，产生时域残影
    // 注意：tblend 需要至少两帧才起效
    std::uniform_real_distribution<double> op(0.25, 0.35);
    double opacity = op(ctx.rng);
    std::ostringstream vf;
    vf << "tblend=all_mode=average:all_opacity=" << std::fixed << std::setprecision(2) << opacity
       << ",scale=trunc(iw/2)*2:trunc(ih/2)*2";
    string suf = rand_suffix(ctx);
    string out = pstr(fs::absolute(ctx.cfg.out_dir / outname(ctx, suf)));
    auto cmd = base_in_args(ctx);
    cmd.insert(cmd.end(), {"-vf", vf.str(), "-c:v","libx264","-crf","22", out});
    if (run_cmd(cmd)!=0) { outs.push_back({fs::path(out).filename().string(), "ghosting", "FAILED"}); return false; }
    std::ostringstream det; det << "opacity=" << std::setprecision(2) << opacity;
    outs.push_back({fs::path(out).filename().string(), "ghosting", det.str()});
    return true;
}

bool make_colorspace_mismatch(Context& ctx, std::vector<OutFile>& outs) {
    // 在解码/过滤阶段假设 BT.709，输出标记/转换为 BT.601（或反之），制造色彩空间错配
    // 这里选择统一将输入当作 bt709 解码，然后在编码输出时标记/转换为 bt601，产生轻微色偏
    // 注：不同 ffmpeg 版本/构建对 colorspace 标记/转换的行为略有差异，取尽量通用的写法
    std::uniform_int_distribution<int> mode(0,1);
    bool to601 = mode(ctx.rng)==0; // 半数转 709->601，半数 601->709
    const char* inspace  = to601 ? "bt709"    : "smpte170m"; // 输入假定
    const char* outspace = to601 ? "smpte170m" : "bt709";    // 输出目标：用 smpte170m 表征 bt601

    // 使用 colormatrix 进行 709<->601 转换（广泛可用），再做偶数尺寸缩放
    std::ostringstream vf;
    vf << "colormatrix=src=" << (to601?"bt709":"bt601") << ":dst=" << (to601?"bt601":"bt709")
       << ",scale=trunc(iw/2)*2:trunc(ih/2)*2";

    string suf = rand_suffix(ctx);
    string out = pstr(fs::absolute(ctx.cfg.out_dir / outname(ctx, suf)));
    auto cmd = base_in_args(ctx);
    cmd.insert(cmd.end(), {"-vf", vf.str(), "-c:v","libx264","-crf","22", out});
    if (run_cmd(cmd)!=0) { outs.push_back({fs::path(out).filename().string(), "colorspace_mismatch", "FAILED"}); return false; }
    std::ostringstream det; det << inspace << "->" << outspace;
    outs.push_back({fs::path(out).filename().string(), "colorspace_mismatch", det.str()});
    return true;
}

bool make_repeat(Context& ctx, std::vector<OutFile>& outs) {
    // 1) 导出 BMP 帧序列
    fs::path tmp = ctx.cfg.out_dir / ("tmp_frames_" + rand_suffix(ctx));
    if (!util_ensure_dir(tmp)) { std::cerr<<"cannot create tmp dir\n"; return false; }
    fs::path pat = tmp / "f_%06d.bmp";
    string dump_out = pstr(pat);

    auto dump = base_in_args(ctx);
    dump.insert(dump.end(), {"-vsync","0", "-start_number","0", "-f","image2", dump_out});
    if (run_cmd(dump)!=0) return false;

    // 2) 生成索引：选择 K 个位置重复，K ~= 1%~2% 帧；并从后面等量丢弃保证总帧数一致
    int N = (int)ctx.total_frames;
    if (N<=0) { // 回退用 ffprobe 估计
        // 不强依赖；如果 N==0，直接选择一个小值
        N = 200;
    }
    int K = std::max(1, N/100); // 约 1%
    std::uniform_int_distribution<int> pick(1, std::max(1,N-2));
    std::vector<int> dup_pos; dup_pos.reserve(K);
    for (int i=0;i<K;++i) dup_pos.push_back(pick(ctx.rng));
    std::sort(dup_pos.begin(), dup_pos.end());
    dup_pos.erase(std::unique(dup_pos.begin(), dup_pos.end()), dup_pos.end());
    K = (int)dup_pos.size();

    // 准备最终帧序列：遍历 0..N-1，遇到 dup_pos 就把该帧写两次；
    // 同时记录需要“抵消”的删除位置（例如每隔 N/K 在后段删一帧）
    std::vector<int> drop_pos;
    if (K>0) {
        int stride = std::max(N/(K+1), 2);
        for (int i=0;i<K;++i) {
            int p = (N/3) + i*stride; if (p>=N) p = N-1-i; if (p<=1) p=2+i;
            drop_pos.push_back(p);
        }
        std::sort(drop_pos.begin(), drop_pos.end());
        drop_pos.erase(std::unique(drop_pos.begin(), drop_pos.end()), drop_pos.end());
        while ((int)drop_pos.size()>K) drop_pos.pop_back();
        while ((int)drop_pos.size()<K) drop_pos.push_back(N-2-(int)drop_pos.size());
    }

    // 3) 写 concat list（使用 concat demuxer，带 header 与每帧 duration）
    fs::path list_path = tmp / "list.txt";
    std::ofstream lof(list_path);
    lof << "ffconcat version 1.0\n";
    int dup_idx=0, drop_idx=0;
    for (int n=0;n<N;++n) {
        if (drop_idx<(int)drop_pos.size() && n==drop_pos[drop_idx]) { // 跳过
            ++drop_idx;
            continue;
        }
        std::ostringstream fn; fn << "f_" << std::setw(6) << std::setfill('0') << n << ".bmp";
        lof << "file " << fn.str() << "\n";
        lof << "duration " << (1.0 / std::max(1, ctx.cfg.fps)) << "\n";
        if (dup_idx<(int)dup_pos.size() && n==dup_pos[dup_idx]) {
            lof << "file " << fn.str() << "\n"; // 再写一次，实现“重复帧”
            lof << "duration " << (1.0 / std::max(1, ctx.cfg.fps)) << "\n";
            ++dup_idx;
        }
    }
    // 末尾再写一次最后一帧，保证最后一个 duration 生效
    if (N>0) {
        std::ostringstream last; last << "f_" << std::setw(6) << std::setfill('0') << (N-1) << ".bmp";
        lof << "file " << last.str() << "\n";
    }
    lof.close();

    // 4) 重编码为 mp4
    string suf = rand_suffix(ctx);
    fs::path out = fs::absolute(ctx.cfg.out_dir / outname(ctx, suf));
    std::vector<string> enc = {
        ctx.cfg.ffmpeg, "-hide_banner", "-y",
        "-fflags","+genpts",
        "-f","concat","-safe","0","-i", pstr(list_path),
        "-pix_fmt","yuv420p","-c:v","libx264","-crf","22",
        pstr(out)
    };
    if (run_cmd(enc)!=0) { outs.push_back({ out.filename().string(), "repeat_frames_keep_count", "FAILED"}); return false; }

    // 5) 清理临时帧（如需保留调试可注释掉）
    std::error_code ec;
    fs::remove_all(tmp, ec);

    // 记录
    std::ostringstream det;
    det << "dups="<<K<<" at ";
    for (size_t i=0;i<dup_pos.size();++i){ if (i) det<<","; det<<dup_pos[i]; }
    det << " ; drops="<<drop_pos.size()<<" at ";
    for (size_t i=0;i<drop_pos.size();++i){ if (i) det<<","; det<<drop_pos[i]; }
    outs.push_back({ out.filename().string(), "repeat_frames_keep_count", det.str() });
    return true;
}

bool make_all(Context& ctx, std::vector<OutFile>& outs) {
    // 顺序可自由调整
    bool ok=true;
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

bool write_manifest(const Context& ctx, const std::vector<OutFile>& outs) {
    fs::path man = ctx.cfg.out_dir / "manifest.txt";
    std::ostringstream ss;
    ss << "seed="<<ctx.cfg.seed<<"\n";
    ss << "input="<<ctx.cfg.in_path<<"\n";
    ss << "size="<<ctx.cfg.w<<"x"<<ctx.cfg.h<<" pix="<<ctx.cfg.pix<<" fps="<<ctx.cfg.fps<<"\n";
    ss << "total_frames~="<<ctx.total_frames<<" (assume yuv420p 8-bit)\n";
    ss << "outputs:\n";
    for (auto& o: outs) {
        ss << "  - "<< o.filename << " | " << o.kind << " | " << o.details << "\n";
    }
    // 统计失败项
    size_t failed=0; for (auto& o: outs) if (o.details=="FAILED") ++failed;
    if (failed>0) ss << "failed_count="<<failed<<"\n";
    return util_write_text(man, ss.str());
}
