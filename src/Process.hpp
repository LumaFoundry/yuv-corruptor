#pragma once
#include <string>
#include <vector>
#include <iostream>
#include <cstdlib>

inline int run_cmd(const std::vector<std::string>& args) {
    // 组装成一条命令字符串（简单做法：system）
    std::string cmd;
    for (size_t i=0;i<args.size();++i) {
        if (i) cmd += " ";
        const std::string& a = args[i];
        const bool alreadyQuoted = a.size() && (a.front()=='"' || a.front()=='\'');
        const bool hasSpaceOrQuote = a.find_first_of(" \t\"") != std::string::npos;
        if (alreadyQuoted) {
            cmd += a;
        } else if (i==0) {
            // 对于可执行文件名，如果没有空格，不加引号，避免 PATH 搜索异常
            if (hasSpaceOrQuote) cmd += "\"" + a + "\""; else cmd += a;
        } else {
            cmd += "\"" + a + "\"";
        }
    }
    std::cerr << "[cmd] " << cmd << "\n";
    return std::system(cmd.c_str());
}
