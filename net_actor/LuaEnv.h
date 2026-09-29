#pragma once
/**
@auther: chencaiyu
@date: 2026.8
@brief: sol::state 的 RAII 封装（lua_State 生命周期 + cmsgpack 预加载）

设计文档 Phase 1 基础设施：
  - 构造时 open_libraries + 预加载 cmsgpack 为全局
  - 析构时 sol::state 自动 lua_close
  - 配套 LuaActor 使用，每 actor 一份独立 Lua 状态
  - 构造时配置 package.path，使框架/业务脚本可通过 require 按名加载
*/

#include <sol/sol.hpp>
#include <string>

extern "C" int luaopen_cmsgpack(lua_State* L);  // 来自 -l:cmsgpack.so

namespace bllsll {

// 定位脚本根目录：即"包含 lua/actor_bridge.lua 的那个目录"（正常是 net_actor/）。
// 解析顺序：/proc/self/exe 所在目录 → 其父目录 → CWD → CWD 父目录，
// 取第一个满足 <dir>/lua/actor_bridge.lua 存在的。全部失败时返回 "."。
// 结果进程内缓存（static），只解析一次。
// 之所以不依赖 CWD：测试从 test_case/ 运行，而脚本在 net_actor/lua、
// net_actor/test_case/lua 下，靠 CWD 会随启动方式漂移。
const std::string& GetLuaRoot();

// 把脚本目录前置进 package.path（保留原有条目），使 require 可按名加载：
//   <root>/lua/?.lua           框架/正式脚本
//   <root>/test_case/lua/?.lua 测试脚本
// 必须在任何 require / loadfile 之前调用。
void ConfigureLuaPackagePath(sol::state& lua);

class LuaEnv {
public:
    LuaEnv();
    ~LuaEnv() = default;
    LuaEnv(const LuaEnv&) = delete;
    LuaEnv& operator=(const LuaEnv&) = delete;

    sol::state& State() { return lua_; }
    lua_State* L() { return lua_.lua_state(); }

private:
    sol::state lua_;
};

}  // namespace bllsll
