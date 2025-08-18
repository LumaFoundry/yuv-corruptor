#pragma once
#include <string>
#include <random>
#include <vector>
#include <filesystem>
#include <optional>
#include "Fs.hpp"
#include "Process.hpp"

struct Settings {
    std::string in_path;
    int w=0, h=0;
    int fps=30;
    std::string pix="yuv420p";
    uint64_t seed=0; // 0 表示用时间
    std::vector<std::string> types; // 空=all
    std::filesystem::path out_dir;
    std::string ffmpeg="ffmpeg";
    std::string ffprobe="ffprobe";
};

struct OutFile {
    std::string filename;
    std::string kind;
    std::string details; // 参数与位置
};

struct Context {
    Settings cfg;
    std::mt19937_64 rng;
    std::string base;      // 输入无扩展名
    size_t total_frames=0; // 仅 yuv420p 8-bit
};

bool init_context(Context& ctx);
std::string rand_suffix(Context& ctx);
bool make_all(Context& ctx, std::vector<OutFile>& outs);

// 各缺陷
bool make_blocky(Context&, std::vector<OutFile>&);
bool make_brightness(Context&, std::vector<OutFile>&);
bool make_jitter(Context&, std::vector<OutFile>&);
bool make_smooth(Context&, std::vector<OutFile>&);
bool make_highclip(Context&, std::vector<OutFile>&);
bool make_chroma_bleed(Context&, std::vector<OutFile>&);
bool make_repeat(Context&, std::vector<OutFile>&); // 保持帧数一致
bool make_luma_bleed(Context&, std::vector<OutFile>&);
bool make_grain(Context&, std::vector<OutFile>&);
bool make_ringing(Context&, std::vector<OutFile>&);
bool make_banding(Context&, std::vector<OutFile>&);
bool make_ghosting(Context&, std::vector<OutFile>&);

// 报告
bool write_manifest(const Context&, const std::vector<OutFile>&);
