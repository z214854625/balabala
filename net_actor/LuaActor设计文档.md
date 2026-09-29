# LuaActor 设计文档 — 多线程 Actor 系统中嵌入 Lua

@auther: chencaiyu
@date: 2026.8

## 目录

- [一、背景与目标](#一背景与目标)
- [二、核心矛盾与总体方案](#二核心矛盾与总体方案)
- [三、线程模型：为什么不需要锁](#三线程模型为什么不需要锁)
- [四、全局变量问题](#四全局变量问题)
- [五、数据共享与序列化](#五数据共享与序列化)
- [六、C++20 协程与 Lua 协程的融合](#六c20-协程与-lua-协程的融合)
- [七、技术选型](#七技术选型)
- [八、错误处理与生命周期](#八错误处理与生命周期)
- [九、代码结构](#九代码结构)
- [十、实现路线](#十实现路线)
- [十一、注意事项与边界](#十一注意事项与边界)

---

## 一、背景与目标

当前 `net_actor` 框架是一个多 worker 线程的 Actor 系统：

- 任意数量的 worker 线程从 `readyQueue_` 取 actor，循环 `ProcessOne()`
- 每个 actor 同一时刻至多被一个 worker 处理（`scheduled_` 原子标记去重）
- Actor 消息处理天然支持 C++20 协程（`co_await Call/Sleep/ClusterCall`）
- 全局状态全部用 `SpinLock` 保护，数据传递走 `ActorMessage`（含 `MessageBuffer` 零拷贝大包）

**目标**：在 Actor 内执行 Lua 脚本（业务逻辑用 Lua 编写），同时解决三大问题：

1. **多线程问题**：多个 worker 并发，Lua 状态如何保证安全
2. **全局变量问题**：Lua 的 `_G` 全局表如何隔离 / 共享
3. **数据共享问题**：Lua 与 C++、Lua 与跨 actor / 跨进程的数据如何交换

**技术选型（已确定）**：

| 组件 | 用途 |
|---|---|
| **Lua 5.4** | 脚本语言本体 |
| **sol2 v3.x** | C++ 绑定层（`sol::state` / `sol::coroutine` / `sol::protected_function`） |
| **lua-cmsgpack** | 消息序列化（Lua 侧 `cmsgpack.pack/unpack`，C++ 侧调用同一实现） |

---

## 二、核心矛盾与总体方案

### 核心矛盾

Lua 语言的设计假设是"一个进程内只有一个执行流"，`lua_State` 内的全局变量天然独占。
而本框架是"多 worker 线程 + 协程可跨线程迁移"。二者直接冲突。

### 总体方案：每 actor 一个独立 Lua 状态（与框架设计同构）

给每个需要跑 Lua 的 actor 关联**独立的 `sol::state`**（内部即 `lua_State*`）。
这把三个问题化归到框架已经解决的层面：

- **多线程问题 → 复用现有消息串行保证**：actor 的消息处理天然串行，每个 Lua 状态**不需要额外锁**
- **全局变量问题 → 每个 actor 的 `_G` 天然隔离**：两个 actor 各持一份 `_G`，互不可见
- **数据共享 → 由 actor 的 C++ 状态持有 + cmsgpack 消息传递**：Lua 侧不存跨 actor 共享数据

**对照成熟参照系 skynet**：skynet 的 Lua 服务 = 一个 `lua_State`（含一个 `lua_newthread` 消息循环），
`skynet.call` 发消息 + `lua_yield` 挂起 Lua 协程，响应回来恢复。
本方案与 skynet 同构，额外多一层 **C++ 协程**，但只需让 C++ 协程遵循同样约束（Lua 只在消息处理窗口内驱动），即完全等价安全。

---

## 三、线程模型：为什么不需要锁

### 串行不变量

`OnCoroutineMessage()` 是唯一进 Lua 的门，而它的调用链是**严格串行**的：

```
workerLoop() 或 Stop()
   └─ Actor::ProcessOne()
        └─ Actor::OnMessage()
             └─ Actor::OnCoroutineMessage()   ← 唯一触碰 sol::state 的入口
```

- `ProcessOne` 只被 `workerLoop` 与 `Stop()`（worker 已 join）调用
- `scheduled_` 去重保证同一 actor 不被并发处理（见 [Actor.h:114](Actor.h#L114)）
- actor 自己给自己发消息时，`scheduled_` 为 true，`compare_exchange_strong` 失败 → 不入队 → 不会并行处理自己

**结论：任意时刻至多一个 worker 在处理某个 actor 的 Lua，无需任何锁。**

### 协程跨 worker 迁移不破坏该性质

C++ 协程可能在 worker A 挂起、worker B 恢复，但这只是"单线程访问"性质在不同线程间转移——
处理本身串行，`lua_State` 是堆对象，不绑定创建线程，**满足 Lua 的单线程约束**。

### 唯一注意点

- **不要**从 `main()` 线程 / EventLoop 线程直接碰 `sol::state`（需走 `QueueInLoop` 或加锁）
- 调试日志也别从别的线程查询 Lua 状态（如 `lua_gettop`）

---

## 四、全局变量问题

### 隔离（默认行为）

每 actor 一个 `sol::state` → `_G` 天然隔离。actor A 里 `x = 1` 不影响 actor B。

### 共享（业务需要时）

两种做法，推荐第一种：

1. **状态服务 actor（推荐）**：全局状态收敛进一个"状态服务 actor"，其他 actor 通过 `Call` / `Send` 访问。
   最符合 actor 模型哲学，无锁，天然支持跨进程集群。
2. **C++ 共享表 + 锁**：C++ 持有共享 `lua_State*` + `std::mutex`，需要时锁上访问。
   实现直观但引入锁竞争，且跨进程不可用。

```cpp
// 状态服务 actor 内部（sol2 便捷读写）
class LuaStateService : public Actor {
    sol::state lua;   // 只在这一 actor 内跑
    // 其他 actor 通过消息 {cmsgpack: "get_balance", user: 1001} 访问
    // C++ 侧用 sol2 直接读写 lua["_G"] 下的共享表
};
```

---

## 五、数据共享与序列化

### 标准交换格式：MessagePack（lua-cmsgpack）

跨 actor / 跨进程的数据，统一用 cmsgpack 序列化，塞进现有 `ActorMessage`：

- **Lua → C++ → 其他 actor**：脚本 `cmsgpack.pack({...})` → 二进制字符串 → `ActorMessage.data`（小包）
  或 `sharedBuf`（大包零拷贝）→ 对端 `cmsgpack.unpack`
- **C++ → Lua**：C++ 构造 cmsgpack 数据 → Lua 侧 `cmsgpack.unpack`
- **集群 / 跨进程**：同一份 cmsgpack 数据直接进 `ClusterPacket`，复用 `ClusterProxy` 回程路由，零转换

### 大小包自动路由

复用现有 `ActorMessage::Make` 机制：

```cpp
// < kBufferThreshold(1024) 走 data (std::string SSO)
// >= kBufferThreshold 走 sharedBuf (MessageBuffer 零拷贝)
ActorMessage msg = ActorMessage::Make(MsgType::UserMessage, actorId_, fd,
                                      packedData.data(), packedData.size());
```

### 阈值建议

cmsgpack 打包后的长度 < 1024 字节用 `data`，否则用 `sharedBuf`。
注意：cmsgpack 的 `pack` 结果要先判断长度再选路径。

---

## 六、C++20 协程与 Lua 协程的融合

### 两套协程正交，不是嵌套

| | C++20 协程 | Lua 协程 |
|---|---|---|
| 实现 | 编译器生成栈帧（`coroutine_handle`） | Lua VM 自己的栈（`lua_State` + thread） |
| 挂起 | `co_await` → 编译器保存栈帧 | `lua_yield` → Lua VM 保存 Lua 栈 |
| 恢复 | `handle.resume()` | `lua_resume()` |
| 存储 | 堆上协程帧 | Lua 堆（GC 管理） |

C++ 协程挂起时，它正在调的 Lua 函数已经**完整返回**（`lua_pcall` 是一次普通 C++ 调用），
不会出现"C++ 协程栈帧夹着半挂起的 Lua 帧"的非法状态。两套协程**交替驱动、互不嵌套**。

### 架构：每条消息一个 Lua dispatch 协程（skynet 模型）

**不要用"单一持久消息循环 + mid-processing yield"。** 如果 `luaMain` 在处理消息时
因为 `actor.call` 挂起 yield 了，等待期间到达的新消息会导致路由混乱：

```
luaMain 处理 msgA → 发异步调用 → yield（挂在 s2 上）
新消息 msgB 到达 → 创建新 C++ 协程 C4 → resume luaMain()
   → luaMain 从 yield 点继续（还在等 s2），不是从消息循环头开始
   → 立刻又 yield → C4 拿到同样的 wait 信号 → C4 卡死，msgB 无人处理
```

**skynet 的解法，也是本方案采用的模型：每条消息 `lua_newthread` 一个独立的 dispatch 协程。** 这样：

- msgA → 协程 LA（做异步调用后挂起，不阻塞循环）
- 等待期间 msgB 到达 → 新建协程 LB，独立处理
- s2 响应到达 → 恢复 LA
- 每条 dispatch 挂起互不阻塞，新消息永远有干净的协程可用

```
Actor::OnCoroutineMessage(msg)                 [C++协程]
 ├─ 若 isResponse：
 │     luaSession 表里查 session → 找到对应的 Lua dispatch 协程
 │     把响应序列化 push 进该协程的 inbox → lua_resume(dispatch) → 返回
 └─ 若新消息：
      序列化 msg → lua_newthread 创建 dispatch 协程
      绑定消息上下文（msg 的 sessionId / sourceId / fd）
      lua_resume(dispatch)
      while 返回 yielded 且信号为 "wait":
          取回目标 + 请求数据
          co_await Call(target, req)           ← C++ 协程挂起，worker 释放
          响应到达后 lua_resume(dispatch)，把响应注入
      结束
```

### Lua session 表（Lua 异步调用专用的路由表）

actor 内并行维护**两张** session 表，避免与 C++ 协程的 `waitMap_` 冲突：

| 表 | 键 | 值 | 用途 |
|---|---|---|---|
| C++ `waitMap_`（现有） | C++ 分配的 sessionId | `std::coroutine_handle<>` | C++ 协程的 `Call/Sleep` |
| `luaSession_`（新增） | 独立分配的 sessionId | Lua dispatch 协程引用 | Lua 侧的 `actor.call` |

- 两个表各自独立分配 sessionId（Lua 表用单独计数器），**互不冲突**
- `OnMessage` 收到 Response 时：**先查 C++ `waitMap_`，查不到再查 `luaSession_`**
  （Lua 协程恢复是唯一新增的 Response 分支）
- Lua dispatch 协程引用用 `sol::coroutine` 存，或存注册表引用（registry reference）确保挂起期间不被 GC

### 挂起信号约定

Lua 协程 `yield` 时带一个返回值（约定表），C++ 侧从 `sol::protected_function_result` 读取：

```lua
-- Lua 侧 actor.call() 实现（示意）：异步调用 + 挂起
function actor.call(target, msg)
    local packed = cmsgpack.pack(msg)
    return coroutine.yield({ action = "wait", target = target, payload = packed })
end
```

```cpp
// C++ 侧恢复 Lua dispatch 协程
sol::protected_function_result r = dispatch();
if (r.status() == sol::call_status::yielded) {
    sol::table sig = r.get<sol::table>();
    if (sig["action"].get<std::string>() == "wait") {
        uint32_t luaSession = AllocLuaSession();   // 独立于 C++ sessionId
        luaSession_[luaSession] = dispatch;        // 存入 Lua session 表
        auto resp = co_await Call(sig["target"].get<uint32_t>(),
                                  /* 请求 + 携带 luaSession 作为回程 sessionId */);
        // 响应到达后：luaSession 表查到 dispatch → 注入响应 → lua_resume
    }
}
```

### Lua 嵌套协程（场景：dispatch 里再启动协程）

dispatch 内 `coroutine.create(c)` + `resume(c)`，`c` 是**纯 Lua 层协程**，与 C++ / sol2 无关：

- `c` 的 `yield` 回到**外层 Lua**（dispatch），不穿过 `lua_resume`，C++ 不感知
- `c` 的创建 / 恢复 / 生命周期全部由 Lua 自己管理，**天然安全**
- 只有当 dispatch 自身（最外层）yield 时才穿过 `lua_resume` 传回 C++，按上面的 wait 信号处理

这支撑"actor1 调 actor2，actor2 里 Lua 起协程异步回调 actor1 并等待触发"的完整链路：

```
actor1: co_await Call(actor2, msgA)          → C1 挂起, waitMap1[s1]=C1
actor2: 收 msgA → 新建 dispatch LA → resume(LA)
        LA 内 coroutine.create(c), resume(c)
        c: actor.call(actor1, msgB) → yield {wait}
        LA 收到 c 的 yield → 自身也 yield {wait} → 穿回 C++
        C++: 记 luaSession_[s2]=LA → co_await Call(actor1, msgB) → C2 挂起
actor1: 收 msgB → 新建协程 C3 → 处理 → Respond(actor2, s2)（不阻塞挂起的 C1）
actor2: 收 s2 响应 → waitMap_ 查不到 → luaSession_ 查到 LA → 注入响应 → resume(LA)
        LA 恢复 → resume(c) → c 完成 → LA 继续执行 actor2 业务
        → 最终 Respond(actor1, s1)
actor1: 收 s1 响应 → waitMap_ 恢复 C1
```

**链能解开的前提**：actor1 挂起的 C1 不阻塞新消息——每条消息 `OnCoroutineMessage`
创建**新**协程（[Actor.cc:119](Actor.cc#L119)），msgB 由 C3 独立处理。

### 死锁提示（业务层，不是框架 bug）

嵌套调用**成环**会死锁：

```
actor1 处理 msgB 时又 co_await Call(actor2, ...)   ← 环
```

actor2 的 LA 挂起等 c（c 等 s2），actor1 的 C3 等 actor2 的响应——互相等，谁也不释放。
这是 actor 模型**通用**的嵌套调用问题（纯 C++ 协程也会），Lua **不新增**死锁类别，
但会让异步结构藏得更深。排查建议：Lua 侧用 `sourceId/sessionId` 打消息链路日志，避免隐性环。

### 超时兜底

`CallAwaiter` 已有超时恢复机制（[Actor.cc:346](Actor.cc#L346)）：超时消息恢复 C++ 协程 →
恢复 Lua dispatch 协程 → Lua 收到 `error=Timeout` 继续执行。**Lua 协程挂起等待永远不会因响应丢失而泄漏。**
Lua session 表的条目在超时恢复时同步清理，避免表泄漏。

### sol2 特有的注意点

**不要把 `sol::function` / `sol::table` 的引用跨 `co_await` 挂起点持有。**
原因：这些对象持有 `lua_State*`，若协程挂起期间 actor 析构导致 `lua_close`，恢复后即悬垂。

规避：**每条消息处理内**临时拿 `sol::function`，用完即弃；挂起/恢复边界上用 cmsgpack 数据（纯值）传递。
Lua dispatch 协程的引用存 registry reference（`sol::object` / `luaL_ref`），挂起期间 `lua_close` 时会随主 state 一起回收，不会悬垂。

---

## 七、技术选型

### 7.1 sol2 v3.x（绑定层）

| 维度 | 评估 |
|---|---|
| 类型转换 | 自动（`sol::function` / `sol::table` 双向），少写胶水 |
| 协程 | 自带 `sol::coroutine` 封装，`status() == yielded` 判断挂起 |
| 栈管理 | `sol::protected_function_result` 自动管理，比裸 `lua_gettop` 安全 |
| 性能 | 泛型转换少量开销，对比 Lua 执行成本可忽略；极致路径可混用裸 `lua_*` |
| 依赖 | 纯头文件（需自带 Lua 5.4 头文件） |

### 7.2 Lua 5.4（本体）

sol2 v3.x 支持 Lua 5.4。需要把 Lua 5.4 的 `.c` 文件（`lua.c` 除外）编译为静态库或直接编进目标。

### 7.3 lua-cmsgpack（序列化）

C 源码（`cmsgpack.c` / `lua_cmsgpack.c`），编译进去，注册为 `cmsgpack` 模块。
提供 `cmsgpack.pack` / `cmsgpack.unpack`，Lua 侧直接用；C++ 侧调用同一 C 实现。

---

## 八、错误处理与生命周期

### 错误处理

- Lua 执行错误：用 `sol::protected_function`（封装 `lua_pcall`）捕获，错误码转日志，
  消息丢弃，actor 继续存活（对齐 `workerLoop` 的 try-catch 语义 [ActorSystem.cc:530](ActorSystem.cc#L530)）
- C++ 桥接函数抛异常：在桥接函数内 catch，转 Lua `error()`，不让异常穿透 `lua_pcall`
- Lua 侧 `pcall` 语义：脚本可自行 `pcall` 包裹业务逻辑

### 生命周期

- actor 析构 → `sol::state` 析构自动 `lua_close`，挂起的 Lua 协程帧随 `lua_close` 回收，**无额外清理代码**
- 依赖的状态服务 actor 退出 → 用现有 `LinkTo` / `ActorDown` 机制通知 Lua actor
- **不要**把 C++ 对象裸指针塞进 Lua 全局表（生命周期悬垂）。用 `sol::userdata` + `__gc` 元方法，或数据留在 C++ 侧、Lua 只通过 C 函数读写

### 内存与 GC

- cmsgpack 打包/解包产生的临时字符串由 Lua GC 回收，注意长驻引用（全局表里的打包结果）会阻止回收
- 大包路径 `MessageBufferPtr` 由引用计数管理，与 Lua GC 无关

---

## 九、代码结构

```
net_actor/
  LuaEnv.h/.cc        # sol::state RAII 封装（lua_State 生命周期 + 错误处理）
  LuaActor.h/.cc      # class LuaActor : public Actor，sol::coroutine 消息循环
  LuaBridge.cc        # actor_send / actor_call / actor_respond 等 sol2 注册的 C 函数
  test_actor_lua.cc   # 复刻 test_actor_msg 链路，脚本侧用 cmsgpack 交换数据
```

```cpp
// LuaActor.h 示意
class LuaActor : public Actor {
public:
    // 脚本入口：每收到消息驱动 luaMain 消息循环
    ActorTask OnCoroutineMessage(ActorMessage msg) override;

protected:
    sol::state lua;              // 主 state
    sol::coroutine luaMain;      // 持久消息循环（lua_newthread）
    void RegisterBridges();      // 注册 actor_send / actor_call / actor_respond
};
```

---

## 十、实现路线

| Phase | 内容 | 验证 |
|---|---|---|
| **Phase 1** | `LuaEnv` + `LuaActor` 同步版（Lua 处理不挂起），cmsgpack 走通消息交换 | `test_actor_lua` 复刻 `test_actor_msg` 链路 |
| **Phase 2** | 错误处理（`protected_function`）、生命周期、共享状态服务 actor（用现有 `Call`） | 状态服务读写测试 |
| **Phase 3** | Lua 协程融合（`sol::coroutine` yield ↔ C++ `co_await` 桥） | `actor.call` 挂起等待测试 |
| **Phase 4** | cmsgpack 集群贯通（复用 `ClusterProxy`）、压测对比 | 跨进程 RPC 测试 + 压测 |

---

## 十一、注意事项与边界

1. **不可重入（唯一硬约束）**：`lua_State` 同一时刻只能被一个执行流驱动。
   - 危险：Lua 调 C 桥接函数，该函数同步回调本 actor 的 `OnCoroutineMessage` → 重入 → 栈损坏
   - 约束：**桥接函数只入队消息 / 注册定时器，永不同步回调**（现有 `SendToActor` 本身就是异步的）
2. **跨线程**：不把 `sol::state` 从别的线程碰（含日志/调试）。
3. **栈深度**：Lua 的 `LUAI_MAXCCALLS` 限制 C 调用深度（~200），递归深的脚本会 `C stack overflow`。
   正常消息处理深度很浅；必要时调大该宏或 `lua_checkstack`。与 C++ 协程无关。
4. **不跨挂起点持 sol 引用**：见第六章。
5. **Lua 版本匹配**：sol2 v3.x 配 Lua 5.4；不要混用 5.3 头文件。
