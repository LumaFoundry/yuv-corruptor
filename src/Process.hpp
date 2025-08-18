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
        // 如果已经带引号则直接追加；否则加引号
        if (args[i].size() && (args[i][0]=='"' || args[i][0]=='\'')) cmd += args[i];
        else cmd += "\"" + args[i] + "\"";
    }
    std::cerr << "[cmd] " << cmd << "\n";
    return std::system(cmd.c_str());
}
