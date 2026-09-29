#define SOL_ALL_SAFETIES_ON 1
#include "LuaEnv.h"

#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

namespace bllsll {

namespace {

// 判据：该目录下是否有 lua/actor_bridge.lua（框架桥接脚本必然存在）
bool DirHasBridge(const std::string& dir) {
    struct stat st;
    std::string probe = dir + "/lua/actor_bridge.lua";
    return ::stat(probe.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// realpath 归一化；失败则原样返回（后续 stat 会再判一次，不会误判成功）
std::string CanonDir(const std::string& p) {
    char buf[PATH_MAX];
    if (::realpath(p.c_str(), buf) != nullptr) return std::string(buf);
    return p;
}

// 取路径的目录部分（不依赖 dirname，避免其修改入参 / 静态缓冲区的坑）
std::string DirOf(const std::string& f) {
    std::string::size_type s = f.find_last_of('/');
    if (s == std::string::npos) return ".";
    if (s == 0) return "/";
    return f.substr(0, s);
}

}  // namespace

const std::string& GetLuaRoot() {
    // 函数内 static：C++11 起线程安全的一次性初始化，
    // 多个 LuaActor 并发构造时也只解析一次。
    static const std::string root = [] {
        std::vector<std::string> cand;
        char buf[PATH_MAX];

        // 主路径：定位可执行文件，再向上找一层。
        //   二进制在 net_actor/          → net_actor/lua        命中第 1 个候选
        //   二进制在 net_actor/test_case/ → net_actor/lua       命中第 2 个候选
        ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            std::string d = CanonDir(DirOf(std::string(buf)));
            cand.push_back(d);
            cand.push_back(d + "/..");
        }

        // 兜底：CWD 及其父目录（覆盖 /proc 不可用的场景）
        if (::getcwd(buf, sizeof(buf)) != nullptr) {
            std::string d = CanonDir(std::string(buf));
            cand.push_back(d);
            cand.push_back(d + "/..");
        }

        for (std::vector<std::string>::const_iterator it = cand.begin();
             it != cand.end(); ++it) {
            if (DirHasBridge(*it)) return CanonDir(*it);
        }
        // 找不到就返回 "."：后续 require 会明确报错，不做静默降级
        return std::string(".");
    }();
    return root;
}

void ConfigureLuaPackagePath(sol::state& lua) {
    const std::string& root = GetLuaRoot();
    // 前置（而非覆盖）：保留 Lua 默认路径，本项目脚本优先命中
    std::string luaPath =
        root + "/lua/?.lua;" +
        root + "/lua/?/init.lua;" +
        root + "/test_case/lua/?.lua;" +
        root + "/test_case/lua/?/init.lua";

    sol::table pkg = lua["package"];
    // 直接读 path 字段；避免 sol2 的 get_or<T> 在多重载间歧义
    sol::object pathObj = pkg["path"];
    std::string cur = pathObj.is<std::string>() ? pathObj.as<std::string>() : std::string();
    pkg["path"] = cur.empty() ? luaPath : (luaPath + ";" + cur);
}

LuaEnv::LuaEnv() {
    // sol::state 默认构造调 luaL_newstate + luaL_openlibs，但保险起见显式开 coroutine 库
    // （非标准 Lua 5.4 头文件 build 可能默认不开，导致 actor.call 里 coroutine.yield 报 nil）
    lua_.open_libraries(sol::lib::base, sol::lib::table, sol::lib::string,
                        sol::lib::math, sol::lib::os, sol::lib::io,
                        sol::lib::utf8, sol::lib::coroutine,
                        sol::lib::package, sol::lib::debug);
    // 预加载 cmsgpack：调用 luaopen_cmsgpack，注册为全局 cmsgpack 表
    // luaL_requiref(L, modname, openfn, glb)：调 openfn 推表，写入 package.loaded，
    // glb=1 时同时设为全局；末尾还会把表压栈一次，需 pop 清理
    luaL_requiref(lua_.lua_state(), "cmsgpack", luaopen_cmsgpack, 1);
    lua_pop(lua_.lua_state(), 1);
    // 配置脚本搜索路径。必须在此完成：LuaEnv 是 LuaActor 的成员，
    // 先于 LuaActor 构造体（RegisterBridges 的 require "actor_bridge"
    // 与业务脚本 require）执行，保证 require 时路径已就绪。
    ConfigureLuaPackagePath(lua_);
}

}  // namespace bllsll
