# LuaActor 现状分析与改造点

@date: 2026.8
@scope: 针对"把 Lua 引入架构做业务开发"这一目标，评估 Phase 1-4 落地形态的偏差，并列出改造点

配套文档：
- [LuaActor设计文档.md](LuaActor设计文档.md) — 原始设计
- [LuaActor使用与流程文档.md](LuaActor使用与流程文档.md) — Phase 1-4 实现说明

**状态**：
- P0 三项 + 3.2 节的坑**已修复**（代码已改，等 VM 可达后编译验证）。
- **P1-5（脚本文件化 / require / 模块化）已实施**，VM 上 7 个 Lua 测试全部 `[PASS]`。详见[〇章 P1-5 实施记录](#p1-5-实施记录)。
- 其余 P1/P2/P3 待评审后实施。

---

## 目录

- [〇、修改记录](#〇修改记录)
- [一、结论先行](#一结论先行)
- [二、当前设计的执行模型](#二当前设计的执行模型)
- [三、偏差清单](#三偏差清单)
- [四、偏差 ↔ 改造点映射](#四偏差--改造点映射)
- [五、机制层做对了的部分](#五机制层做对了的部分)
- [六、需要修改的点](#六需要修改的点)
  - [P0 — 实现缺陷修正（已修）](#p0--实现缺陷修正已修)
  - [P1 — 业务 API 层](#p1--业务-api-层)
  - [P2 — 健壮性与沙箱](#p2--健壮性与沙箱)
  - [P3 — 规模化](#p3--规模化)
- [七、依赖关系与建议顺序](#七依赖关系与建议顺序)
- [八、待定决策](#八待定决策)

---

## 〇、修改记录

本文初稿与实际排查结论有若干出入，此处集中记录，避免后续读者被旧口径误导。

| 项 | 初稿口径 | 修正后口径 | 依据 |
|---|---|---|---|
| **P0-1 栈问题** | "`lua_pop(th,1)` 后 break，末尾 `lua_pop(th,nres)` 重复弹 → 下溢" | **原因说错了**。`!sig.valid` 分支内已有 `nres = 0`，该路径本就正确。真正的问题是 `lua_pop(th, 1)` 硬编码 1，未按 `nres` 清栈 | 复核 `LuaActor.cc:182` |
| **P0-1 严重性** | 仅"泄漏栈槽" | **更严重**：`yield(a,b)` 时 `ReadYieldSignal` 读栈顶 `-1` 拿到的是 `b`，**信号本身就读错了**，不只是泄漏 | 栈布局推演 |
| **P0-2 是否成立** | 列为确定 bug | 一度降级为"待验证"，**现已实测确认成立** | 见下 |
| **P0-2 根因** | "`packed` 跨 catch 未析构" | **错**。try 块局部量在进入 catch 前已正常析构。真因是 **longjmp 从 catch 块内跳出**：`__cxa_end_catch` 被跳过 + 跨 sol2 中间帧不展开 | C++ 异常语义 |
| **偏差 #4 定性** | "把 Lua 语言原语改坏了" | **不公平**。`yield` 只回到最近 `resume` 是 Lua 语言语义，改不了；skynet 亦另起 `skynet.coroutine`。`run_co` 是对语言约束的合理应对，只是不够完整 | Lua 5.4 协程语义 |
| **3.2 `if src ~= 0`** | "写法本身就是个坑" | **升级为真实 bug**：跨进程 `sourceId` 恒 0，该服务被 `cluster_call` 时**永不回包** | `ClusterProxy.cc:58` |
| **条目数** | 偏差 9 条 | 改造点 15 条 = 9 条映射（含 1 合并、1 拆分）+ P0 三项 + 新增 4 项 | 见[第四章](#四偏差--改造点映射) |

### P0-2 的实测判定

初稿说"取决于 Lua 是 C 还是 C++ 编译，需 `nm` 验证"。实测结论：

```
liblua-5.4.so 内符号引用计数：
  __cxa_throw        → 0
  __gxx_personality  → 0
  _Unwind_Resume     → 0
  stdc++             → 0
  luaD_throw         → 2   ← 存在
```

**无任何 C++ 运行时符号 → Lua 以 C 编译 → `LUAI_THROW` 是 `longjmp`。P0-2 成立。**

进一步确认修复方向正确：本项目 Lua 5.4 非 LuaJIT，故
`SOL_PROPAGATE_EXCEPTIONS` 取 `SOL_DEFAULT_OFF`（`sol/compatibility/lua_version.hpp:156`），
sol2 trampoline 走 try/catch 分支，其 `lua_error` 位于**所有 catch 块之外**
（`sol/trampoline.hpp:127`）——正是安全调用点。**桥接函数抛 C++ 异常交给 trampoline 处理，
是 sol2 的预期用法。**

### P1-5 实施记录

P1-5（脚本文件化 / `require` / 模块化）已完成并验证。以下是实施口径，供后续读者核对。

**目录布局**（正式/框架脚本与测试脚本分目录）：

```
net_actor/
  lua/
    actor_bridge.lua                 # 框架桥接（actor.call / cluster_call / run_co）
  test_case/
    lua/                             # 测试脚本，按业务功能模块命名
      ping_server.lua / ping_client.lua        # 基础消息 ping/pong
      state_service.lua                       # actor 内共享状态
      call_service.lua / call_caller.lua       # actor.call 协程融合
      runco_service.lua / runco_caller.lua     # 嵌套协程 run_co
      bench_service.lua / bench_caller.lua     # 性能基准
      cluster_service.lua / cluster_caller.lua # 跨进程 cluster_call
      battle_db.lua / battle_item.lua / battle_service.lua  # 跨服战斗
```

共 15 个 `.lua` 文件，内容从原 `R"LUA(...)LUA"` 常量**逐字搬运**（只去掉包裹符）。

**加载机制**：统一 `package.path` + `require`，不依赖 CWD。

- `LuaEnv` 构造时调 `ConfigureLuaPackagePath(lua_)`，把
  `<root>/lua/?.lua`、`<root>/test_case/lua/?.lua` 等模板前置进 `package.path`（保留原有条目）。
- `<root>` 由 `GetLuaRoot()` 定位：`readlink("/proc/self/exe")` 拿二进制路径 →
  取所在目录 → 向上找一层，判据是 `dir/lua/actor_bridge.lua` 存在。
  兜底走 CWD 及父目录。结果 `static` 缓存，进程内只解析一次。
  之所以不靠 CWD：测试从 `test_case/` 运行，而脚本在 `net_actor/lua`、
  `net_actor/test_case/lua`，CWD 会随启动方式漂移。

**C++ 改动 4 个文件**：

| 文件 | 改动 |
|---|---|
| `LuaEnv.h` / `LuaEnv.cc` | 加 `GetLuaRoot()` + `ConfigureLuaPackagePath(sol::state&)`；构造里在 `open_libraries`/预加载 cmsgpack 之后调用。`package.path` 读取用 `sol::object` + `is<std::string>()`/`as<std::string>()`（`get_or<T>` 在 sol2 两 overload 间歧义） |
| `LuaBridge.cc` | `lua.safe_script(R"LUA(...)LUA")` 换成 `require("actor_bridge")`；加载失败 `throw std::runtime_error` fail-fast（已无内嵌 fallback） |
| `LuaActor.h` / `LuaActor.cc` | 构造函数合并为单个公开构造 `explicit LuaActor(std::string moduleName)`，内部 `require` 加载模块；删掉原字符串构造 `LuaActor(std::string script)`、`FromFile` 工厂、`FileTag` 私有构造三处。原字符串构造在 P1-5 初版保留为死代码，合并时确认全无调用点后删除 |

**7 个测试文件**：删掉全部 `static const char* kXxxScript = R"LUA(...)"`，
构造从 `std::make_unique<LuaActor>(kXxxScript)` 改为 `std::make_unique<LuaActor>("<模块名>")`
（模块名经 `package.path` 解析为对应 `.lua` 文件）。
运行期注入的全局（`serviceId`/`dbServiceId`/`test_record`/`cmsgpack`）注入点不变，
脚本调用时才读，顺序兼容。

**Makefile 零改动**：helper 全在已链接的 `LuaEnv.cc` 里；`.lua` 是运行期资产，不参与编译。

**时序保证**（`require` 依赖的运行期全局在加载时必须就绪）：

1. `package.path` 在 `LuaEnv` 构造里设置 → `LuaEnv` 是 `LuaActor` 成员，先于构造体运行
2. `actor` 表在 `RegisterBridges` 里 `create_named_table` 先建 → `actor_bridge.lua` 里 `function actor.call(...)` 有表可挂
3. `cmsgpack` / `coroutine` 均由 `LuaEnv` 预加载/开库

**语义要点**：`require` 以 `_ENV = 全局表` 执行 chunk，顶层 `function OnMessage(...)`
成为全局 `OnMessage`，与旧 `safe_script` 等价。每 `LuaActor` 持独立 `LuaEnv`
（独立 `package.loaded`），模块每 actor 加载一次，无跨 actor 缓存泄漏。

**验证结果**（VM CentOS 7 + g++ 11.2.0）：

| 测试 | 结果 |
|---|---|
| `test_actor_lua`（ping/pong） | `[PASS]` ping=10 pong=10 |
| `test_actor_lua_state` | `[PASS]` ok=5/5 |
| `test_actor_lua_call` | `[PASS]` ok=5/5 |
| `test_actor_lua_runco` | `[PASS]` ok=5/5 |
| `test_actor_lua_bench` | `[PASS]` Lua/C++ 延迟比 1.11x（与[五章](#五机制层做对了的部分)基线一致） |
| `test_actor_lua_cluster` | `[PASS]` ok=5/5 |
| `test_actor_lua_battle` | `[PASS]` ok=2 fail=1（3 轮跨服扣道具，DB 状态 10→7→2→reject） |

**负向验证**：临时 `mv lua/actor_bridge.lua /tmp/`，运行 `test_actor_lua`，
立即见 `RegisterBridges: require 'actor_bridge' failed: module 'actor_bridge' not found`；
恢复后重新 `[PASS]`。证明确实是运行期从 `.lua` 加载，无静默回退。

**未做**（P1-5 明确范围之外）：
- **热更**：`require` 缓存于 `package.loaded`，同 state 内不会重载。热更牵涉
  "运行中替换函数表 + 保留 upvalue 状态 + 已挂起协程怎么办"，是独立课题，等 P1-2 业务 API 稳定后再议。
- **bytecode 共享**（P3-1）：仍每实例 `require` 一次（含编译）。仅在 LuaActor 数量级达千/万时才需做。
- **第三方 Lua 库兼容**：当前脚本全自研，未引入第三方库；将来若引入，
  `require` 走 `package.path` 的机制对标准库模块透明。

---

## 一、结论先行

> **当前实现是"Lua 协程 ↔ C++20 协程互操作的机制验证"，不是"用 Lua 写业务的框架"。**

Phase 1-4 验证的全部是**机制**：能不能 yield、能不能跨进程、嵌套几层、性能差多少。这些都验证通过了，
且实测 Lua/C++ 延迟比仅 1.11~1.24x，机制层**可以定型**。

缺的是机制之上的**业务 API 层**。判据很直接：写一个业务脚本，需要知道多少框架内幕？

当前答案是"很多"——见[第三章偏差清单](#三偏差清单)。

**粒度决策（已确认）**：一个 actor 一个 `lua_State`。这与当前实现一致，也与 skynet 同构，**保留不改**。
但这条只回答了"隔离方式"，没回答"数量级"，后者仍待定，见[第八章](#八待定决策)。

---

## 二、当前设计的执行模型

```
ActorSystem worker 线程
  └─ Actor::ProcessOne()
      └─ Actor::OnMessage()               ← Response 走 waitMap_，新消息走下面
          └─ LuaActor::OnCoroutineMessage(msg)      [C++20 协程]
              ├─ lua_newthread(mainL) → th          每条消息一个 Lua 协程
              ├─ luaL_ref 注册到 registry 防 GC     （LuaRegRef RAII）
              ├─ cmsgpack.unpack(msg.Data()) → payload
              ├─ lua_resume(th, OnMessage, 7 args)
              └─ while (status == LUA_YIELD):
                     ReadYieldSignal(th) → {action, target|nodeId+actorName, payload}
                     co_await Call(...) / ClusterCall(...)    ← C++ 挂起，worker 释放
                     lua_pushlstring(th, resp) 或 lua_pushnil(th)
                     lua_resume(th, 1)
```

三个关键设计点：

1. **每消息一个 `lua_newthread`**（skynet 模型）— 避免设计文档第六章描述的"持久消息循环 + mid-processing yield"路由错乱
2. **单 session 表** — 放弃设计文档原方案的 `luaSession_` 双表，改由 C++ 协程局部变量 `th` / `thRef` 跨 `co_await` 承载 Lua dispatch 引用
3. **`actor.call` 是纯 Lua** — `cmsgpack.pack` + `coroutine.yield`，不写 C 函数调 `lua_yield`

---

## 三、偏差清单

按"离目标有多远"排序。

| # | 偏差 | 现状定位 | 影响 |
|---|---|---|---|
| 1 | **只能吃 `UserMessage`** | `LuaActor.cc:115` 直接 `co_return` | `NetworkRecv`/`Connected`/`Disconnected` 静默丢弃；网关、玩家 agent 这类主力场景 Lua 写不了 |
| 2 | **无脚本文件 / `require` / 热更** ✅ 文件+require 已做（P1-5） | ~~`LuaActor(std::string script)`~~ → 现已支持 `LuaActor(moduleName)` + `require`；仅剩热更未做 | 脚本不再只能是 C++ 字符串常量；改 Lua 不必重编译。热更仍是独立课题 |
| 3 | **回包要手工分本地/远程** | `LuaBridge.cc:37`(respond) vs `:58`(respond_remote) | 业务必须接住 `is_remote/src_node/src_name` 并自行分支；skynet 只有一个 `skynet.ret()` |
| 4 | **原生 `coroutine.create` 里不能 `actor.call`** | 必须改用 `LuaBridge.cc:120` 的 `actor.run_co` | Lua 语言原语被改坏，业务得记住绕开 |
| 5 | **无服务发现** | 靠 C++ 侧 `actor->Lua()["dbServiceId"] = id` 注入（见 `test_actor_lua_battle.cc:193`） | 脚本无法自治，拓扑硬编码在 C++ |
| 6 | **无 `actor.sleep`** | C++ 侧有 `SleepAwaiter`(`Actor.cc:437`)，Lua 侧未暴露 | 冷却 / 重试 / tick 写不了。改动量最小的一条 |
| 7 | **错误不回传** | 被调方抛错 / 漏 respond / cmd 没匹配 → 无任何回包 | 调用方干等 10s 超时，生产排查噩梦 |
| 8 | **超时与 nil 同义** | `LuaBridge.cc:83` 超时返回 `nil` | 服务正常返回 nil 也是 `nil`，语义二义 |
| 9 | **无沙箱、无指令计数** | `LuaEnv.cc:9` 开了 `io`/`os`/`debug` | 一句 `os.exit()` 整进程没了；`while true do end` 永久占死一个 worker |

### 3.1 偏差的具象化

业务同学写一个最简单的 KV 服务，当前必须写成这样：

```lua
function OnMessage(payload, src, sid, fd, is_remote, src_node, src_name)
    --          ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^ 7 个位置参数全要接住
    if payload.cmd == "get" then
        local resp = {value = db[payload.key]}
        if is_remote then                              -- ← 传输细节泄漏
            actor.respond_remote(src_node, src_name, sid, resp)
        else
            actor.respond(src, sid, resp)
        end
    end
end
```

而目标形态应该是：

```lua
local actor = require "actor"
actor.dispatch {
    get = function(req) return {value = db[req.key]} end,   -- return 即回包
}
```

### 3.2 `if src ~= 0` 是真实 bug（已确认并修复）

`test_actor_lua_battle.cc` 的 db_service 原用 `if src ~= 0 then` 判断要不要回包，
注释写的是"跳过来自 main 的 seed"。

而 `ClusterProxy.cc:58`（`ClusterReceiver::OnPacketReceived`）与 `:307`（`ClusterGateway`）
两处都明确写着：

```cpp
msg.sourceId = 0;  // 跨进程 sourceId 无意义
```

**即：任何跨进程来的消息 `src` 恒为 0。** 该 db_service 一旦被 cross-node 调用，将永不回包，
调用方吃满 10s 超时。正确判据是 `sid ~= 0`（有 sessionId 才是 Call，才需要回包）。

这个坑不是写法不优雅的问题，是业务被迫理解框架内幕后**理解错了**的问题——恰好印证了偏差 #3 的危害。

**已修复**：`test_actor_lua_battle.cc` 与 `LuaActor使用与流程文档.md` 中的 db_service 示例
均改为 `sid ~= 0`，并补注释说明为何不能用 `src`。

> 该测试当前恰好能通过，是因为 db_service 只被**本地** `actor.call` 调用（src 非 0）。
> 一旦有人复制这段"示范代码"去写一个会被跨服调用的服务，就会踩中。
> 示例代码的传播性使这个坑比它在测试里的实际影响更危险。

---

## 四、偏差 ↔ 改造点映射

偏差清单 9 条，改造点 15 条，不是一一对应。完整映射如下：

| 偏差 | 改造点 | 关系 |
|---|---|---|
| #1 只吃 UserMessage | P1-1 | 1:1 |
| #2 无脚本文件 | P1-5 | 1:1 |
| #3 回包分本地/远程 | P1-2 | 1:1 |
| #4 嵌套协程不能 call | P1-7 | 1:1 |
| #5 无服务发现 | P1-6 | 1:1 |
| #6 无 sleep | P1-4 | 1:1 |
| #7 错误不回传 | P1-3 | **2 → 1 合并**（同一条回程错误通道，一起改） |
| #8 超时与 nil 同义 | P1-3 | 同上 |
| #9 无沙箱、无指令计数 | P2-1 | **1 → 2 拆分**（裁库） |
| #9 同上 | P2-2 | 同上（指令计数 hook，与裁库无依赖） |
| — | **P0-1 / P0-2 / P0-3** | 实现缺陷，不属于"设计偏差"，独立成组 |
| — | **P2-3** 调用环可观测性 | 本文新增（设计文档第六章已提示死锁，但无观测手段） |
| — | **P3-1** bytecode 共享 | 本文新增（仅玩家级 actor 触发） |
| — | **P3-2** 内存基线核算 | 本文新增（同上） |

**9 条偏差 → 9 个 P1/P2 改造点**（#7+#8 合并抵消 #9 的拆分），
**再加 3 个 P0 实现缺陷 + 3 个新增项 = 15**。

---

## 五、机制层做对了的部分

**以下不要动**：

| 决策 | 评价 |
|---|---|
| 每条消息 `lua_newthread` 独立 dispatch 协程 | 正确。避免了设计文档第六章分析的路由错乱 |
| 复用 actor 串行不变量免锁 | 正确。`scheduled_` CAS 去重保证同一 actor 不并发 |
| **单 session 表**（放弃 `luaSession_` 双表） | **优于原设计**。C++ 协程局部变量天然承载 Lua dispatch 引用，无需跨消息查找 |
| `LuaRegRef` RAII + `DestroyAllCoroutines` 时机 | 细节到位。`h.destroy()` 时 locals 析构（C++20 规范保证），此时 mainL 仍存活，`luaL_unref` 安全 |
| cmsgpack 跨 actor / 跨进程零转换 | 正确。同一份 bytes 直接进 `ClusterPacket` |
| 大小包自动路由（`ActorMessage::Make`） | 正确，复用现有机制 |

性能实测（VM CentOS 7 + g++ 11.2.0，1000 次 roundtrip）：

- Lua `actor.call`: ~54 µs/op
- C++ `co_await Call`: ~46 µs/op
- 延迟比 1.11~1.24x — 桥接层开销可接受，**机制层可以定型**

---

## 六、需要修改的点

### P0 — 实现缺陷修正（已修）

> 三项均已改完，等 VM 可达后编译验证。

#### P0-1 `lua_pop(th, 1)` 忽略 `nres` — 信号读错 / 栈下溢 / 栈泄漏 ✅

**位置**：`LuaActor.cc` yield 循环开头

**原代码**：

```cpp
while (status == LUA_YIELD) {
    YieldSignal sig = ReadYieldSignal(th);   // 内部读栈顶 -1
    lua_pop(th, 1);                          // ← 硬编码 1，不看 nres
```

`lua_resume` 的 `nres` 是 yield 带回的值个数：

| 业务写法 | `nres` | 后果 |
|---|---|---|
| `coroutine.yield({...})` | 1 | 正常 |
| `coroutine.yield()` | 0 | `lua_pop(th, 1)` 在空栈上弹 → **栈下溢** |
| `coroutine.yield(a, b)` | 2 | `ReadYieldSignal` 读 `-1` 拿到的是 **`b`**（信号表在 `-2`）→ **信号读错**；且只弹 1 个 → 每轮**泄漏 1 个栈槽** |

> 初稿把此条描述为"重复 pop 导致下溢"，**那是错的**——`!sig.valid` 分支内已有 `nres = 0`，
> 末尾 `lua_pop(th, nres)` 是 no-op。真正的问题是上面这一处硬编码。

**已实施的修正**：

1. `ReadYieldSignal` 增加 `idx` 参数，内部用 `lua_absindex` 归一化为绝对索引
   （必须归一化：函数内多次 `lua_getfield` 会推值改变栈顶，相对索引会漂移）
2. 调用处按约定"信号表是 yield 的第一个值"传 `-nres`
3. `nres >= 1` 才读；`lua_pop(th, nres)` 按实际个数清栈；随后 `nres = 0`
4. 删除 `!sig.valid` 分支内已冗余的 `nres = 0`

---

#### P0-2 `luaL_error` 从 catch 块内 longjmp ✅

**位置**：`LuaBridge.cc` 三个桥接函数（send / respond / respond_remote），共 6 处 `luaL_error`

**成立性已实测确认**（见[〇章](#p0-2-的实测判定)）：`liblua-5.4.so` 无任何 C++ 运行时符号
→ Lua 以 C 编译 → `LUAI_THROW` 是 `longjmp`。

**根因（修正初稿口径）**：

初稿说"`packed` 跨 catch 未析构"，**这是错的**——try 块内的局部量在进入 catch 前**已正常析构**。

真正的问题是 `longjmp` **从 catch 块内部跳出**：

1. 异常永远走不完 `__cxa_end_catch` → 异常对象不释放、运行时"正在处理异常"状态不清
2. `longjmp` 跨过 sol2 trampoline 等中间 C++ 栈帧时**不做栈展开**，这些帧的局部对象析构被跳过

**已实施的修正**：引入 `GuardedBridge(what, fn)` helper，桥接函数**只抛 C++ 异常**，
由 sol2 trampoline 在安全边界上转成 Lua error：

```cpp
template <typename F>
void GuardedBridge(const char* what, F&& fn) {
    try { fn(); }
    catch (const std::exception& e) {
        throw std::runtime_error(std::string(what) + ": " + e.what());
    }
}
```

这正是 sol2 的预期用法：`SOL_PROPAGATE_EXCEPTIONS` 为 OFF 时，trampoline 捕获异常后
在**所有 catch 块之外**调用 `lua_error`（`sol/trampoline.hpp:127`）。
`sol::error` 派生自 `std::exception`，一个 catch 即可覆盖原来的两个分支。

---

#### P0-3 `fd` 写死 0 ✅

**位置**：`LuaActor.cc` yield 循环内构造 `req`；`LuaBridge.cc` 三处 `ActorMessage::Make`

`fd = 0` 是 stdin 的合法 fd，会让下游误判为"有关联连接"。应为 `-1`（`ActorMessage` 默认值）。

**已实施的修正**：4 处 `Make(..., 0, ...)` 全部改为 `-1`。

> **未做**：原消息 `msg.fd` 的透传。当前 `actor.call` 发出的请求不携带原始 fd，
> 被调方拿到的 `fd` 恒为 -1。是否需要透传取决于业务形态（网关场景大概率需要），
> 归入 P1-1 一并处理。

---

### P1 — 业务 API 层

> 这是主体工作。以下每一条都与 actor 数量级无关，服务级 / 玩家级都要做。

#### P1-1 消息类型分发：让网络消息进得来

**对应偏差 #1**。当前 `LuaActor.cc:115` 一刀切丢弃非 `UserMessage`。

**改造方向**：按 `msg.type` 分派到不同 Lua 入口，且**只有 `UserMessage` 走 cmsgpack 解包**：

| `MsgType` | Lua 入口 | payload 形态 |
|---|---|---|
| `UserMessage` | `OnMessage` | `cmsgpack.unpack` 后的 table |
| `NetworkRecv` | `OnNetworkRecv(fd, data)` | **裸字符串**，不解包 |
| `Connected` | `OnConnected(fd)` | 无 |
| `Disconnected` | `OnDisconnected(fd)` | 无 |
| `ActorDown` | `OnActorDown(id, reason)` | 原因字符串 |

脚本未定义对应函数时静默跳过（当前行为的兼容退化）。

**注意**：`NetworkRecv` 走裸字符串是关键——网络字节流不是 cmsgpack，当前无条件 `unpack` 必然失败。

**风险**：中（改动 `OnCoroutineMessage` 主干）。**工作量**：中。

---

#### P1-2 统一回包：`return` 即 ret，干掉 respond / respond_remote 分支

**对应偏差 #3 + 3.2 节的坑**。

框架侧在 dispatch 时把回程上下文（`src` / `sid` / `is_remote` / `src_node` / `src_name`）
**存进 C++ 协程局部**，Lua 侧提供单一 `actor.ret(v)`，由 C++ 自行决定走
`RespondToCall` 还是 `RespondRemote`。

判据用 `sid ~= 0`（**不是 `src ~= 0`**，见 3.2 节）。

再往上包一层 `actor.dispatch{}`，让业务函数 `return` 值即自动 `ret`：

```lua
actor.dispatch {
    get = function(req) return {value = db[req.key]} end,
    set = function(req) db[req.key] = req.value; return {ok = true} end,
}
```

`OnMessage` 的 7 个位置参数随之收敛——业务只见 `req`，需要上下文时走 `actor.context()`。

**风险**：中（改变业务侧 API，现有 5 个测试脚本要跟着改）。**工作量**：中。
**收益**：最高。这一条直接消除了"业务必须理解传输层"的根因。

---

#### P1-3 错误回传 + 区分超时与 nil

**对应偏差 #7 + #8**。

两件事：

1. **被调方出错要回包**。C++ 侧在 `lua_resume` 返回非 `LUA_OK` 时（`LuaActor.cc:214`），
   若当前消息有 `sid`，自动回一个带 `CallError` 的错误响应，而不是只打 `std::cerr` 就结束。
   `Message.h:34` 的 `CallError` 已预留 `UserError = 100` 起的业务错误段，直接用。

2. **调用方要能区分**。`actor.call` 改为返回 `(result, err)` 二元组：

```lua
local r, err = actor.call(svc, req)
if err then ... end          -- err = "timeout" / "target_missing" / "callee_error: ..."
```

当前 `LuaBridge.cc:83` 超时返回 `nil`，与"服务正常返回 nil"不可区分。

**风险**：低-中。**工作量**：中。**收益**：高，直接决定线上可排查性。

---

#### P1-4 `actor.sleep`

**对应偏差 #6**。C++ 侧 `SleepAwaiter`（`Actor.cc:437`）已就绪，只需在 yield 信号里加一个 action：

```lua
function actor.sleep(ms)
    coroutine.yield({action = "sleep", ms = ms})
end
```

C++ 侧 `ReadYieldSignal` 加 `ms` 字段，yield 循环加分支：

```cpp
} else if (sig.action == "sleep") {
    co_await Sleep(sig.ms);
    lua_pushnil(th);          // sleep 无返回值
}
```

**风险**：极低。**工作量**：最小。**建议**：可以和 P0 一批做，验证 yield 信号扩展路径是否顺畅。

---

#### P1-5 脚本文件 / `require` / 模块化 ✅

**对应偏差 #2**。**已实施**，详见[〇章 P1-5 实施记录](#p1-5-实施记录)。

实施要点：
- `LuaActor` 构造函数合并为单个公开构造 `explicit LuaActor(std::string moduleName)`，内部 `require` 按名加载业务脚本
- `LuaEnv` 构造时 `ConfigureLuaPackagePath` 把 `lua/`、`test_case/lua/` 前置进 `package.path`
- 项目根由 `GetLuaRoot()` 从 `/proc/self/exe` 定位，**CWD 无关**
- 框架桥接脚本（`actor.call` / `cluster_call` / `run_co`）移至 `net_actor/lua/actor_bridge.lua`，
  `RegisterBridges` 用 `require("actor_bridge")` 加载
- 测试脚本按业务功能模块命名，放 `net_actor/test_case/lua/`（与正式脚本分目录）
- 原 `LuaActor(std::string script)` 字符串构造在 P1-5 初版保留为死代码，合并时确认全无调用点后删除，API 从 3 处（字符串 ctor + `FromFile` + `FileTag` ctor）收敛为 1 处
- Makefile 零改动

**热更**先不做——它牵涉"运行中替换函数表 + 保留状态"，是独立课题，等业务 API 稳定后再议。

**风险**：低。**工作量**：小-中。**结果**：VM 上 7 个 Lua 测试全部 `[PASS]`。

---

#### P1-6 服务发现 `actor.query(name)`

**对应偏差 #5**。当前靠 C++ 侧 `actor->Lua()["dbServiceId"] = id` 注入。

`ActorSystem` 已有 `RegisterName` / `FindActorByName`（`Actor.h:185`），
只需桥出去：`actor.query("db_service") → uint32_t | nil`。

同时 `actor.call` 接受名字或 id 两种形态。

**风险**：低。**工作量**：小。

---

#### P1-7 让嵌套协程里能直接 `actor.call`（干掉 `run_co`）

**对应偏差 #4**。

**先澄清定性**：初稿说"框架把 Lua 语言原语改坏了"，这话**不公平**。
`coroutine.yield` 只回到最近的 `coroutine.resume`，是 **Lua 语言语义，改不了**。
skynet 也没改——它提供 `skynet.coroutine` 让业务用，而不是修改原生 `coroutine`。
**`run_co` 是对该语言约束的合理应对**，问题只是不够完整（不支持 `wrap`，业务需显式包裹）。

两个改进选项：

**选项 A（skynet 做法）**：提供 `actor.coroutine.{create,resume,yield,wrap}`，
内部维护 `parent[co] = running_co` 链，nested 协程 yield 特殊信号时逐层向上 propagate。
业务用 `actor.coroutine` 而非 `coroutine`。
—— 比 `run_co` 好在支持 `wrap`、支持任意层、语义完整；但业务仍需记住用哪个。

**选项 B（更彻底）**：因为**每个 actor 有独立 `_G`**，可以直接在 `_G` 里**覆盖 `coroutine` 表**
为包装版。业务写原生 `coroutine.create` 语法，拿到的是包装实现。
—— skynet 出于兼容第三方库的顾虑没这么做；本项目脚本全是自己的，可行。

**权衡**：选项 B 对业务最友好（真正"无感"），但覆盖标准库会让第三方 Lua 库（若将来引入）行为异常。
建议 **A 做实现、B 做可选开关**。**决策见[第八章 8.3](#83-p1-7-选-a-还是-b)。**

**风险**：中（协程语义是易错区，需要充分测试）。**工作量**：中。

---

### P2 — 健壮性与沙箱

#### P2-1 裁剪标准库

**位置**：`LuaEnv.cc:9-12`

当前开了 `base, table, string, math, os, io, utf8, coroutine, package, debug`。

- `os` — 业务脚本一句 `os.exit()` 整个进程没了；`os.execute` 更危险
- `io` — 任意文件读写
- `debug` — 可绕过任何沙箱

**建议**：默认只开 `base, table, string, math, utf8, coroutine`；
`os` 只保留 `os.time` / `os.clock`（白名单重建）；
`package` 按 P1-5 需要保留但限定 `package.path`。

**风险**：低（但要确认现有脚本没用到被裁的库）。**工作量**：小。

---

#### P2-2 指令计数钩子，防死循环占死 worker

一个 `while true do end` 会永久占住一个 worker 线程，且**无法从外部中断**。

`lua_sethook(th, hook, LUA_MASKCOUNT, N)` 在 hook 里检查执行时长，超限 `luaL_error` 打断。

**注意**：钩子要设在**每条消息的 `th` 上**，不是 mainL。

**风险**：低。**工作量**：小。**优先级**：取决于脚本是否可信。自研业务脚本可后置。

---

#### P2-3 死循环 Call 环的可观测性

设计文档第六章已指出"嵌套调用成环会死锁"，且正确判断这是 actor 模型通用问题、Lua 不新增死锁类别。

但 Lua 让调用链藏得更深。建议在 yield 信号里带上调用链深度 / trace id，超过阈值告警。

**风险**：低。**工作量**：小。**优先级**：低，可等出问题再做。

---

### P3 — 规模化

> **仅在 LuaActor 为玩家级（上万实例）时需要。服务级（几十个）可完全跳过。**

#### P3-1 脚本 bytecode 共享

**位置**：`LuaActor.cc:100`

```cpp
sol::protected_function_result scriptRes = env_.State().safe_script(script);
```

每个 `LuaActor` 实例把同一份脚本**重新词法 + 语法分析一遍**。1 万玩家 = 编译 1 万次。

**注意**：这不违背"一 actor 一 state"的原则——state 仍然独立，只是**加载路径**改为：

```
一次:   luaL_loadbuffer(tmpL, script) → lua_dump() → std::string bytecode   （进程级缓存）
每实例: luaL_loadbuffer(L, bytecode) → lua_pcall                            （跳过编译）
```

`lua_dump` 产出的 bytecode 可安全跨 state 复用（同一 Lua 版本、同一平台）。

**触发条件**：actor 数量级达到千/万。**风险**：低。**工作量**：小-中。

---

#### P3-2 内存基线核算

每个 `sol::state` 开 10 个标准库，基线几十 KB。1 万实例 = 数百 MB 起，且各自持有 GC 堆。

P2-1 的库裁剪会直接降低这条基线，两件事可以一起做。

**触发条件**：同 P3-1。

---

## 七、依赖关系与建议顺序

```
P0-1 栈  ─┐
P0-2 长跳 ─┼─ 已完成（等 VM 编译验证）
P0-3 fd  ─┘

P1-4 sleep ──────── 独立，最小，可作为 yield 信号扩展的试点

P1-1 消息类型分发 ──┐
                    ├──> P1-2 统一 ret / dispatch ──> 现有 5 个测试脚本需同步改
P1-6 服务发现 ──────┘                                   （API 变更的集中爆发点）

P1-3 错误回传 ────── 依赖 P1-2（回程上下文已在 C++ 侧集中管理）

P1-5 文件/require ── 独立于上述，但建议在 P1-2 之后
                     （否则脚本要为 API 变更改两遍）

P1-7 协程包装 ────── 独立，但语义易错，建议单独一批 + 充分测试

P2-* ─────────────── 独立，按脚本可信度决定优先级

P3-* ─────────────── 仅玩家级 actor 需要，先定数量级再说
```

**建议顺序**：

1. ~~**P0 全部**~~ ✅ 已完成 —— **待编译验证**
2. **P1-4**（sleep）— 最小、独立，验证 yield 信号扩展路径通畅
3. **P1-1 + P1-2 + P1-6** — 一批做完，业务 API 形态一次性定型，测试脚本只改一遍
4. **P1-3** — 错误回传，补上可排查性
5. **P1-5** — 脚本文件化，业务开始真正落地
6. **P1-7** — 协程包装，单独批次
7. **P2** — 按脚本可信度
8. **P3** — 仅当数量级触发

第 3 步是 API 变更的集中爆发点，`test_actor_lua*.cc` 五个测试的脚本都要跟着改。**合并做，别拆开。**

---

## 八、待定决策

### 8.1 LuaActor 的数量级

已确认"一 actor 一 lua_State"（隔离方式），但**数量级未定**，它只影响 P3：

| 数量级 | 典型形态 | 结论 |
|---|---|---|
| 几十个 | battle / item / db / rank 等逻辑服务 | 当前构造路径直接可用，**P3 全部跳过** |
| 上万个 | 1 玩家 1 actor | 仍是一 actor 一 state，但**必须做 P3-1 bytecode 共享 + P3-2 库裁剪** |

现有 5 个测试全是服务级。这个决策**不影响 P0/P1/P2 任何一条**，可以延后。

### 8.2 热更新是否要做

P1-5 只做"从文件加载"，不做热更。热更牵涉"运行中替换函数表 + 保留 upvalue 状态 +
已挂起协程怎么办"，是独立课题。建议等业务 API（P1-2）稳定后单独评估。

### 8.3 P1-7 选 A 还是 B

选项 A（`actor.coroutine`，skynet 做法）vs 选项 B（覆盖 `_G.coroutine`）。
取决于将来是否引入第三方 Lua 库。若确定全部脚本自研，B 的业务体验明显更好。

### 8.4 `fd` 是否需要沿调用链透传

P0-3 只把 `fd` 从 0 改为 -1（语义正确化），**没做透传**。
当前 `actor.call` 发出的请求不携带原始 `msg.fd`，被调方拿到的 `fd` 恒为 -1。
网关 / 玩家 agent 场景大概率需要透传，但这依赖 P1-1 的消息类型分发方案，一并决定。

---

## 附：本文引用的代码位置

> 行号为改动前的位置，P0 三项修复后行号已变动，故只保留文件与语义定位。

| 文件 | 内容 | 状态 |
|---|---|---|
| `LuaActor.cc` | 非 UserMessage 直接 co_return（偏差 #1） | 待 P1-1 |
| `LuaActor.cc` | `lua_pop(th, 1)` 忽略 nres（P0-1） | ✅ 已修 |
| `LuaActor.cc` | `ReadYieldSignal` 增 idx 参数 + `lua_absindex`（P0-1） | ✅ 已修 |
| `LuaActor.cc` | `fd = 0` → `-1`（P0-3） | ✅ 已修 |
| `LuaActor.cc` | 每实例 `safe_script` 重编译（P3-1） | 待定（看数量级） |
| `LuaActor.cc` | resume 出错只打 cerr，不回包（P1-3） | 待 P1-3 |
| `LuaBridge.cc` | `luaL_error` 从 catch 内 longjmp（P0-2） | ✅ 已修（`GuardedBridge`） |
| `LuaBridge.cc` | `Make(..., 0, ...)` → `-1`（P0-3） | ✅ 已修（3 处） |
| `LuaBridge.cc` | respond / respond_remote 分裂（P1-2） | 待 P1-2 |
| `LuaBridge.cc` | 超时返回 nil，语义二义（P1-3） | 待 P1-3 |
| `LuaBridge.cc` | `actor.run_co`（P1-7） | 待 P1-7 |
| `LuaEnv.cc` | 标准库全开，含 io/os/debug（P2-1） | 待 P2-1 |
| `LuaEnv.h` / `LuaEnv.cc` | `GetLuaRoot` + `ConfigureLuaPackagePath`（P1-5） | ✅ 已修 |
| `LuaBridge.cc` | `safe_script(R"LUA")` → `require("actor_bridge")`（P1-5） | ✅ 已修 |
| `LuaActor.h` / `LuaActor.cc` | 构造合并为单个 `LuaActor(std::string moduleName)`（P1-5） | ✅ 已修 |
| `net_actor/lua/actor_bridge.lua` | 框架桥接脚本文件（P1-5） | ✅ 新建 |
| `net_actor/test_case/lua/*.lua` | 14 个测试脚本文件（P1-5） | ✅ 新建 |
| 7 个 `test_case/test_actor_lua*.cc` | 删 `kXxxScript` 常量，构造改用 `std::make_unique<LuaActor>("<模块名>")`（P1-5） | ✅ 已修 |
| `ClusterProxy.cc:58 / :307` | `sourceId = 0` — 跨进程 src 恒 0（3.2 节依据） | 框架行为，不改 |
| `Actor.cc` | `SleepAwaiter` 已就绪，未桥出（P1-4） | 待 P1-4 |
| `Actor.h` | `FindActorByName` 已就绪，未桥出（P1-6） | 待 P1-6 |
| `Message.h` | `CallError` 枚举，`UserError = 100` 可用（P1-3） | 待 P1-3 |
| `test_actor_lua_battle.cc` | `if src ~= 0` → `sid ~= 0`（3.2 节） | ✅ 已修 |
| `LuaActor使用与流程文档.md` | 同上示例同步修正（3.2 节） | ✅ 已修 |
| `test_actor_lua_battle.cc` | C++ 手工注入依赖 id（P1-6） | 待 P1-6 |
| `sol/compatibility/lua_version.hpp:156` | `SOL_PROPAGATE_EXCEPTIONS` DEFAULT_OFF（P0-2 依据） | 第三方，不改 |
| `sol/trampoline.hpp:127` | `lua_error` 在 catch 块外（P0-2 依据） | 第三方，不改 |
