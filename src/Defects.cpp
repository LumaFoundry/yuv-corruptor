#include "Defects.hpp"
#include "Fs.hpp"
#include <chrono>
#include <sstream>
#include <iomanip>
#include <cassert>
#include <algorithm>
#include <fstream>
#include <ctime>

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
    char a = 'a' + d(ctx.rng), b='a'+d(ctx.rng);
    return string() + a + b;
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
        "-i", ctx.cfg.in_path
    };
}

bool make_blocky(Context& ctx, std::vector<OutFile>& outs) {
    // factor：缩到 1/f 后再放大，邻近采样制造块感
    std::uniform_int_distribution<int> df(6, 12);
    int f = df(ctx.rng);
    string vf = "scale=iw/" + std::to_string(f) + ":ih/" + std::to_string(f) + ":flags=bilinear,"
                "scale=iw:ih:flags=neighbor";
    string suf = rand_suffix(ctx);
    string out = (ctx.cfg.out_dir / outname(ctx, suf)).string();

    auto cmd = base_in_args(ctx);
    cmd.insert(cmd.end(), {"-vf", vf, "-c:v","libx264","-crf","28", out});
    if (run_cmd(cmd)!=0) return false;

    outs.push_back({fs::path(out).filename().string(), "lowres_blocky",
        "down/up factor="+std::to_string(f)});
    return true;
}

bool make_brightness(Context& ctx, std::vector<OutFile>& outs) {
    // 微亮度偏移：-3..+3（8-bit）
    std::uniform_int_distribution<int> d(-3,3);
    int delta = d(ctx.rng);
    string vf = "lutyuv=y='clip(val+" + std::to_string(delta) + ",0,255)'";
    string suf = rand_suffix(ctx);
    string out = (ctx.cfg.out_dir / outname(ctx, suf)).string();

    auto cmd = base_in_args(ctx);
    cmd.insert(cmd.end(), {"-vf", vf, "-c:v","libx264","-crf","22", out});
    if (run_cmd(cmd)!=0) return false;

    outs.push_back({fs::path(out).filename().string(), "brightness_drift",
        "delta_Y=" + std::to_string(delta) + " (global)"});
    return true;
}

bool make_jitter(Context& ctx, std::vector<OutFile>& outs) {
    // 轻微抖动：每 K 帧向左右/上下平移 1 像素（镜像填充边界）
    std::uniform_int_distribution<int> dk(8, 20);
    std::uniform_int_distribution<int> ds(0,1); // 0:h, 1:v
    int K = dk(ctx.rng);
    bool horiz = ds(ctx.rng)==0;
    // 条件启用 enable= (n % K == 0)
    string cond = "mod(n\\," + std::to_string(K) + ")=0";
    string crop, pad, fill;
    if (horiz) {
        crop = "crop=iw-1:ih:1:0:enable='" + cond + "'";
        pad  = "pad=iw:ih:1:0:enable='" + cond + "'";
        fill = "fillborders=left=1:mode=mirror:enable='" + cond + "'";
    } else {
        crop = "crop=iw:ih-1:0:1:enable='" + cond + "'";
        pad  = "pad=iw:ih:0:1:enable='" + cond + "'";
        fill = "fillborders=top=1:mode=mirror:enable='" + cond + "'";
    }
    string vf = crop + "," + pad + "," + fill;

    string suf = rand_suffix(ctx);
    string out = (ctx.cfg.out_dir / outname(ctx, suf)).string();
    auto cmd = base_in_args(ctx);
    cmd.insert(cmd.end(), {"-vf", vf, "-c:v","libx264","-crf","22", out});
    if (run_cmd(cmd)!=0) return false;

    outs.push_back({fs::path(out).filename().string(), "jitter_1px",
        (horiz? "dir=horiz":"dir=vert") + (string)", period=" + std::to_string(K)});
    return true;
}

bool make_smooth(Context& ctx, std::vector<OutFile>& outs) {
    // 轻度平滑（尽量不毁纹理，强调边缘平滑）：gblur 小 sigma
    std::uniform_real_distribution<double> ds(0.7, 1.4);
    double sigma = ds(ctx.rng);
    std::ostringstream vf; vf<< "gblur=sigma=" << std::fixed << std::setprecision(2) << sigma;
    string suf = rand_suffix(ctx);
    string out = (ctx.cfg.out_dir / outname(ctx, suf)).string();

    auto cmd = base_in_args(ctx);
    cmd.insert(cmd.end(), {"-vf", vf.str(), "-c:v","libx264","-crf","23", out});
    if (run_cmd(cmd)!=0) return false;

    std::ostringstream d; d<<"sigma="<<std::setprecision(2)<<sigma;
    outs.push_back({fs::path(out).filename().string(), "edge_oversmooth", d.str()});
    return true;
}

bool make_highclip(Context& ctx, std::vector<OutFile>& outs) {
    // 高光裁剪：阈值 235..245（8-bit），把更亮的直接推到 255
    std::uniform_int_distribution<int> th(235,245);
    int T = th(ctx.rng);
    string vf = "lutyuv=y='if(gte(val\\,"+std::to_string(T)+")\\,255\\,val)'";
    string suf = rand_suffix(ctx);
    string out = (ctx.cfg.out_dir / outname(ctx, suf)).string();

    auto cmd = base_in_args(ctx);
    cmd.insert(cmd.end(), {"-vf", vf, "-c:v","libx264","-crf","22", out});
    if (run_cmd(cmd)!=0) return false;

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
    vf << "chromashift=cb_h="<<cbh<<":cr_h="<<crh<<":cb_v="<<cbv<<":cr_v="<<crv
       << ":enable='"<< en.str() <<"',"
       << "boxblur=0:1:enable='"<< en.str() <<"'";

    string suf = rand_suffix(ctx);
    string out = (ctx.cfg.out_dir / outname(ctx, suf)).string();

    auto cmd = base_in_args(ctx);
    cmd.insert(cmd.end(), {"-vf", vf.str(), "-c:v","libx264","-crf","22", out});
    if (run_cmd(cmd)!=0) return false;

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

bool make_repeat(Context& ctx, std::vector<OutFile>& outs) {
    // 1) 导出 BMP 帧序列
    fs::path tmp = ctx.cfg.out_dir / ("tmp_frames_" + rand_suffix(ctx));
    util_ensure_dir(tmp);
    fs::path pat = tmp / "f_%06d.bmp";
    string dump_out = pat.string();

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

    // 3) 写 concat list
    fs::path list_path = tmp / "list.txt";
    std::ofstream lof(list_path);
    int dup_idx=0, drop_idx=0;
    for (int n=0;n<N;++n) {
        if (drop_idx<(int)drop_pos.size() && n==drop_pos[drop_idx]) { // 跳过
            ++drop_idx;
            continue;
        }
        fs::path f = tmp / (std::stringstream{}<<std::setw(6)<<std::setfill('0')<<n, ""); // trick
        std::ostringstream fn; fn<< (tmp / "f_") .string() << std::setw(6) << std::setfill('0') << n << ".bmp";
        lof << "file " << util_quote(fn.str()) << "\n";
        if (dup_idx<(int)dup_pos.size() && n==dup_pos[dup_idx]) {
            lof << "file " << util_quote(fn.str()) << "\n"; // 再写一次，实现“重复帧”
            ++dup_idx;
        }
    }
    lof.close();

    // 4) 重编码为 mp4
    string suf = rand_suffix(ctx);
    fs::path out = ctx.cfg.out_dir / outname(ctx, suf);
    std::vector<string> enc = {
        ctx.cfg.ffmpeg, "-hide_banner", "-y",
        "-r", std::to_string(ctx.cfg.fps),
        "-f","concat","-safe","0","-i", list_path.string(),
        "-c:v","libx264","-crf","22",
        out.string()
    };
    if (run_cmd(enc)!=0) return false;

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
    return util_write_text(man, ss.str());
}
