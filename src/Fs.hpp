#pragma once
#include <string>
#include <filesystem>
#include <vector>
#include <fstream>

namespace fs = std::filesystem;

inline std::string quote(const std::string& s) {
    // 为 Windows shell 简单加引号
    return "\"" + s + "\"";
}

inline bool write_text(const fs::path& p, const std::string& text) {
    std::ofstream ofs(p, std::ios::binary);
    if (!ofs) return false;
    ofs << text;
    return true;
}

inline std::uintmax_t file_size_or(const fs::path& p, std::uintmax_t def=0) {
    std::error_code ec; auto sz=fs::file_size(p, ec);
    return ec? def : sz;
}

inline bool ensure_dir(const fs::path& d) {
    std::error_code ec;
    if (fs::exists(d, ec)) return true;
    return fs::create_directories(d, ec);
}
