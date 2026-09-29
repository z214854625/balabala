# LuaActor 使用与流程文档

@auther: chencaiyu
@date: 2026.8

配套 [LuaActor设计文档.md](LuaActor设计文档.md) 的实现层使用说明、设计图与业务流程图。覆盖 Phase 1-4 的最终落地形态。

## 目录

- [一、Lua 侧 API](#一lua-侧-api)
- [二、C++ 侧接口](#二c-侧接口)
- [三、架构设计图](#三架构设计图)
- [四、业务流程图](#四业务流程图)
- [五、决策记录](#五决策记录)

---

## 一、Lua 侧 API

### 1.1 API 速查表

所有 API 都挂在全局 `actor` 表上（由 `RegisterBridges` 注册到 `sol::state`）：

| API | 用途 | 是否阻塞 | 何时可用 |
|---|---|---|---|
| `actor.self()` | 返回本 actor 的 actorId（uint32） | 否 | 任何时候 |
| `actor.send(target, table)` | fire-and-forget 发消息 | 否 | 任何时候 |
| `actor.respond(src, sid, table)` | 响应 C++ 侧 `co_await Call` 的请求 | 否 | 收到本地 UserMessage 时 |
| `actor.call(target, table)` | 同步等待响应（Lua 协程 yield） | 是 | 仅 OnMessage 内 |
| `actor.cluster_call(node, name, table)` | 跨进程同步等待 | 是 | 仅 OnMessage 内 |
| `actor.respond_remote(node, name, sid, table)` | 响应跨进程 `ClusterCall` | 否 | 收到远端 UserMessage 时 |
| `actor.run_co(fn, ...)` | 在 OnMessage 内运行嵌套 Lua 协程，自动 propagate yield 到 C++ | 是（内部多次 yield） | 仅 OnMessage 内 |

### 1.2 OnMessage 协议

```lua
function OnMessage(payload, src, sid, fd, is_remote, src_node, src_name)
    -- payload:    table    cmsgpack.unpack 出来的业务数据
    -- src:        uint32   来源 actorId（本地消息时有效；远端消息时为 0）
    -- sid:        uint32   sessionId，用于 Call/Response 配对
    -- fd:         int      关联的网络 fd（无网络时为 -1）
    -- is_remote:  bool     true 表示来自跨进程
    -- src_node:   string   远端节点 ID（如 "nodeA"）；本地消息时为 ""
    -- src_name:   string   远端 actor 名字（如 "lua_caller"）；本地时为 ""
end
```

### 1.3 完整 Lua 脚本示例

```lua
-- 本地共享状态服务（同时支持本地 Call 和跨进程 ClusterCall）
local state = {}

function OnMessage(payload, src, sid, fd, is_remote, src_node, src_name)
    if payload.cmd == "set" then
        state[payload.key] = payload.value
        local resp = {ok = true, key = payload.key, value = state[payload.key]}
        if is_remote then
            actor.respond_remote(src_node, src_name, sid, resp)
        else
            actor.respond(src, sid, resp)
        end

    elseif payload.cmd == "get" then
        local resp = {value = state[payload.key]}
        if is_remote then
            actor.respond_remote(src_node, src_name, sid, resp)
        else
            actor.respond(src, sid, resp)
        end

    elseif payload.cmd == "aggregate" then
        -- 链式调用：先调本地 serviceB，再调远端 serviceC
        local r1 = actor.call(serviceBId, {cmd="query", data=payload.data})
        if not r1 then
            actor.respond(src, sid, {ok = false, reason = "serviceB timeout"})
            return
        end

        local r2 = actor.cluster_call("nodeC", "service_c", {cmd="query", data=r1.result})
        if not r2 then
            actor.respond(src, sid, {ok = false, reason = "serviceC timeout"})
            return
        end

        actor.respond(src, sid, {ok = true, b = r1.result, c = r2.result})
    end
end
```

### 1.4 嵌套协程用法（C++ 协程 ↔ Lua 嵌套协程）

**场景**：业务 Lua 脚本需要在 OnMessage 内启动嵌套 Lua 协程（如把复杂流程包成 `coroutine.create(fn)`），嵌套协程内部又要 `actor.call` / `actor.cluster_call` yield 等待响应——最后控制权回到 C++ 协程。

**Lua yield 语义**：`coroutine.yield` 总是返回给**最近的 `coroutine.resume`**，不会自动穿越到顶层 `lua_resume`（C++ 侧）。所以嵌套协程的 yield 不会自动被 C++ 看到，需要外层 Lua 显式 propagate。

```
C++ lua_resume(th)              ← 顶层 resume（C++ 发起）
    │
    ▼
  OnMessage(...)                ← dispatch 协程（顶层 Lua 协程）
    │
    │ coroutine.resume(nested_co)
    ▼
  nested_co:                    ← 嵌套 Lua 协程
    actor.call(...) → yield {wait, ...}
    │
    └─ yield 返回给 ↓（不是给 C++！）
              │
  OnMessage 的 coroutine.resume(nested_co) 拿到 yield 值
```

#### 用法 A：直接在 OnMessage 里调 `actor.call`（最简单，推荐）

```lua
function OnMessage(payload, src, sid, fd, is_remote, src_node, src_name)
    -- actor.call 内部就是 coroutine.yield
    -- yield 直接穿回 C++ 的 lua_resume，C++ 自动处理
    local resp = actor.call(serviceId, {cmd="query", data="x"})
    actor.respond(src, sid, {result = resp.result})
end
```

C++ 侧看到 yield → `co_await Call` → 响应回来 → `lua_resume` 推响应 → OnMessage 继续。**默认推荐写法**。

#### 用法 B：OnMessage 里再启动 Lua 嵌套协程（手动 propagate）

```lua
function OnMessage(payload, src, sid, fd, is_remote, src_node, src_name)
    local co = coroutine.create(function()
        local resp = actor.call(serviceId, {cmd="query", data="x"})
        return resp
    end)

    local ok, yieldSig = coroutine.resume(co)
    -- ★ 关键：必须把 yield 再 yield 一次给 C++
    while coroutine.status(co) ~= "dead" do
        local respPacked = coroutine.yield(yieldSig)  -- 穿回 C++
        ok, yieldSig = coroutine.resume(co, respPacked) -- 响应回来喂给嵌套协程
    end

    -- yieldSig 现在是嵌套协程的 return value
    actor.respond(src, sid, yieldSig)
end
```

#### 用法 C：用 `actor.run_co` helper（推荐，无感 propagate）

`actor.run_co` 由 `LuaBridge.cc` 注册时自动定义，业务侧无需手写 propagate 循环：

```lua
function OnMessage(payload, src, sid, fd, is_remote, src_node, src_name)
    local result = actor.run_co(function()
        local r1 = actor.call(svcA, {cmd="q"})       -- 嵌套 yield 自动 propagate
        local r2 = actor.cluster_call("nodeB", "svcB", {cmd="q", data=r1.x})
        return {a = r1.x, b = r2.y}
    end)
    actor.respond(src, sid, result)
end
```

`actor.run_co` 实现（已内置，业务无需关心）：

```lua
function actor.run_co(fn, ...)
    local co = coroutine.create(fn)
    local ok, sig = coroutine.resume(co, ...)
    if not ok then error("run_co: " .. tostring(sig), 2) end
    while coroutine.status(co) ~= "dead" do
        local respPacked = coroutine.yield(sig)    -- propagate 到 C++
        ok, sig = coroutine.resume(co, respPacked) -- 响应回来喂给嵌套协程
        if not ok then error("run_co: " .. tostring(sig), 2) end
    end
    return sig  -- 嵌套协程的 return 值
end
```

#### 控制流图（C++ 协程 ↔ Lua 嵌套协程）

```
 C++ OnCoroutineMessage(协程)
    │
    │ lua_resume(th, 7)
    ▼
 OnMessage(...)  ← 顶层 Lua 协程
    │
    │ actor.run_co(fn) → coroutine.resume(co)
    ▼
 nested co(fn)  ← 嵌套 Lua 协程
    │ actor.call(svcA, req)
    │  └─ coroutine.yield({action="wait", target, payload})
    │
    └─→ yield 穿回 actor.run_co 的 resume(co)
              │
              │ actor.run_co: coroutine.yield(sig) ← propagate 到顶层
              ▼
        C++ lua_resume 返回 LUA_YIELD
              │
              │ C++ ReadYieldSignal → co_await Call(target, payload)
              │ (C++ 协程挂起,worker 释放)
              │ 响应到达 → C++ 协程恢复
              │ lua_pushlstring(th, resp) + lua_resume(th, 1)
              ▼
        actor.run_co 的 coroutine.yield 返回 respPacked
              │
              │ coroutine.resume(co, respPacked)
              ▼
        nested co: actor.call 返回 unpack(respPacked)
              │ 继续 fn 内的下一个 actor.call...
              │ 最终 fn return
              ▼
        actor.run_co 返回 final result
              │
              ▼
        OnMessage: actor.respond(src, sid, result)
              │
              ▼
        C++ lua_resume 返回 LUA_OK
```

**关键点**：C++ 侧 `LuaActor::OnCoroutineMessage` 的 yield 循环已经能处理任意层 Lua 嵌套——只要最外层 OnMessage 把嵌套协程的 yield 信号 propagate 上来。`actor.run_co` 把这层 propagate 自动化，业务脚本完全无感。

#### 三种模式对比

| 模式 | 是否支持 | 写法 |
|---|---|---|
| C++ 协程 → Lua 函数直接 `actor.call` | ✓ 直接 | OnMessage 里写 `actor.call(...)` |
| C++ 协程 → Lua 函数 → 嵌套协程 wait | ✓ 需 propagate | 手写循环 **或** 用 `actor.run_co` |
| C++ 协程 → Lua 函数（不 yield） | ✓ | 普通 `lua_pcall` 路径（Phase 1/2） |

### 1.5 跨服战斗业务示例

**场景**：A 服战斗发起前，需要到 B 服异步检查并扣道具（B 服先读 DB，再扣减），扣减成功才回到 A 服继续战斗。

**拓扑**：

```
┌────────── nodeA ──────────┐        ┌────────── nodeB ──────────┐
│                            │        │                            │
│  battle_service (Lua)      │        │  item_service (Lua)        │
│   OnMessage(start_battle)  │        │   OnMessage(check_and_     │
│   ├─ actor.run_co(fn)      │        │     deduct)                │
│   │  └─ actor.cluster_call │        │   ├─ actor.run_co(fn)      │
│   │     ("nodeB",          │────────│──▶│  ├─ actor.call(db,   │
│   │      "item_service",   │ cluster│   │   {cmd="get", ...})   │
│   │      {check_and_deduct,│  call  │   │  ├─ check sufficient   │
│   │       ...})            │◀────────│──│  ├─ actor.call(db,    │
│   │  └─ 战斗逻辑            │ respond│   │   {cmd="set", ...})   │
│   └─ test_record           │ _remote│   │  └─ return {ok, ...}  │
│                            │        │   └─ actor.respond_remote  │
│                            │        │                            │
│                            │        │  db_service (Lua, 本地)     │
│                            │        │   OnMessage(get/set)      │
└────────────────────────────┘        └────────────────────────────┘
```

**db_service 脚本**（简单 KV，模拟数据库）：

```lua
local db = {}

-- 回包判据用 sid ~= 0（有 sessionId 才是 Call，才需要回包）。
-- ★ 不要用 src ~= 0：跨进程消息的 sourceId 恒为 0（ClusterProxy.cc:58
--   "跨进程 sourceId 无意义"），用 src 判断会导致被 cluster_call 时永不回包。
function OnMessage(payload, src, sid, fd, is_remote, src_node, src_name)
    if payload.cmd == "get" then
        if sid ~= 0 then
            actor.respond(src, sid, {value = db[payload.key] or 0})
        end
    elseif payload.cmd == "set" then
        db[payload.key] = payload.value
        if sid ~= 0 then
            actor.respond(src, sid, {ok = true})
        end
    end
end
```

**item_service 脚本**（B 服道具服务，用 `run_co` 串联多个 `actor.call`）：

```lua
function OnMessage(payload, src, sid, fd, is_remote, src_node, src_name)
    if payload.cmd == "check_and_deduct" then
        -- run_co 包裹:内部多个 actor.call 都会 yield,自动 propagate
        local result = actor.run_co(function()
            -- 1. 异步读 DB
            local db_resp = actor.call(dbServiceId, {
                cmd = "get",
                key = payload.player_id .. ":" .. payload.item_id
            })
            if not db_resp then
                return {ok = false, reason = "db read timeout"}
            end

            local current = db_resp.value or 0
            if current < payload.count then
                return {ok = false, reason = "insufficient",
                        has = current, need = payload.count}
            end

            -- 2. 充足则扣减
            local deduct_resp = actor.call(dbServiceId, {
                cmd = "set",
                key = payload.player_id .. ":" .. payload.item_id,
                value = current - payload.count
            })
            if not (deduct_resp and deduct_resp.ok) then
                return {ok = false, reason = "deduct failed"}
            end

            return {ok = true, remaining = current - payload.count}
        end)

        -- 跨进程响应回 nodeA 的 battle_service
        actor.respond_remote(src_node, src_name, sid, result)
    end
end
```

**battle_service 脚本**（A 服战斗服务，跨服调 B 服）：

```lua
function OnMessage(payload, src, sid, fd, is_remote, src_node, src_name)
    if payload.cmd == "start_battle" then
        local result = actor.run_co(function()
            -- 战斗前:跨进程检查并扣道具
            local check_resp = actor.cluster_call("nodeB", "item_service", {
                cmd = "check_and_deduct",
                player_id = payload.player_id,
                item_id = payload.item_id,
                count = payload.count
            })

            if not check_resp then
                return {ok = false, reason = "item_service timeout"}
            end

            if not check_resp.ok then
                -- 道具不足,战斗取消
                return {ok = false, reason = check_resp.reason,
                        has = check_resp.has, need = check_resp.need}
            end

            -- 道具扣减成功,执行战斗逻辑(真实业务这里调战斗系统)
            return {
                ok = true,
                battle = "victory",
                remaining_items = check_resp.remaining
            }
        end)

        if result.ok then
            print("[Battle] player=" .. payload.player_id ..
                  " count=" .. payload.count ..
                  " outcome=" .. result.battle ..
                  " remaining=" .. result.remaining_items)
            test_record("ok")
        else
            print("[Battle] player=" .. payload.player_id ..
                  " count=" .. payload.count ..
                  " REJECTED reason=" .. (result.reason or "unknown"))
            test_record("fail")
        end
    end
end
```

**C++ main 关键步骤**（完整代码见 [test_case/test_actor_lua_battle.cc](test_case/test_actor_lua_battle.cc)）：

```cpp
// 1. nodeB: db_service + item_service
EventLoop loopB; loopB.Create();
ActorSystem sysB; sysB.Start(2, &loopB); loopB.SetActorSystem(&sysB);
LoopbackTransport transportB; transportB.SetLocalNodeId("nodeB");
sysB.RegisterTransport(&transportB);

auto dbService = std::make_unique<LuaActor>("battle_db");
uint32_t dbServiceId = sysB.RegisterActor(std::unique_ptr<Actor>(std::move(dbService)));

auto itemService = std::make_unique<LuaActor>("battle_item");
uint32_t itemServiceId = sysB.RegisterActor(std::unique_ptr<Actor>(std::move(itemService)));
sysB.RegisterName("item_service", itemServiceId);
itemServicePtr->Lua()["dbServiceId"] = dbServiceId;  // 注入依赖

// 2. nodeA: battle_service
EventLoop loopA; loopA.Create();
ActorSystem sysA; sysA.Start(2, &loopA); loopA.SetActorSystem(&sysA);
LoopbackTransport transportA; transportA.SetLocalNodeId("nodeA");
sysA.RegisterTransport(&transportA);

auto battleService = std::make_unique<LuaActor>("battle_service");
uint32_t battleServiceId = sysA.RegisterActor(std::unique_ptr<Actor>(std::move(battleService)));
sysA.RegisterName("battle_service", battleServiceId);  // ClusterCall 要求调用方注册名字

// 3. 双向 LoopbackTransport 连通
ClusterReceiver receiverA(&sysA), receiverB(&sysB);
transportA.RegisterRemoteHandler("nodeB", [&](const ClusterPacket& p){ receiverB.OnPacketReceived(p); });
transportB.RegisterRemoteHandler("nodeA", [&](const ClusterPacket& p){ receiverA.OnPacketReceived(p); });

// 4. 预置 DB: p1 有 10 瓶 potion
sysB.Send(dbServiceId, /* packed {cmd="set", key="p1:potion", value=10} */);

// 5. 串行触发 3 场战斗(避免并发读 DB 竞态)
Send(battleServiceId, {cmd="start_battle", player_id="p1", item_id="potion", count=3});
//   → ok (10→7)
Send(battleServiceId, {cmd="start_battle", player_id="p1", item_id="potion", count=5});
//   → ok (7→2)
Send(battleServiceId, {cmd="start_battle", player_id="p1", item_id="potion", count=10});
//   → fail (2<10, 不扣)
```

**完整流程图**（一轮 battle，含 4 次 Lua yield）：

```
 C++ main                                                  nodeB
   │
   │── Send(battle_service, start_battle, count=3) ────────▶│
   │                                                        │
                                       ┌───────────────────┘
                                       │ battle_service.OnMessage
                                       │  actor.run_co(fn):
                                       │    yield#1: cluster_call("nodeB","item_service", check_and_deduct)
                                       │       │
   ┌───────────────────────────────────│───────│───────────────┐
   │ C++ 协程:co_await ClusterCall ────│───────│──────────▶    │
   │                                   │       │   item_service.OnMessage
   │                                   │       │    actor.run_co(fn'):
   │                                   │       │      yield#2: actor.call(db, {cmd="get"})
   │                                   │       │         │
   │                                   │       │    ┌────│───┐
   │                                   │       │    │ db.OnMessage
   │                                   │       │    │ get → respond
   │                                   │       │    └────│───┘
   │                                   │       │      yield#2 返回 → db_resp = {value=10}
   │                                   │       │      check 10 >= 3 ✓
   │                                   │       │      yield#3: actor.call(db, {cmd="set", value=7})
   │                                   │       │         │
   │                                   │       │    ┌────│───┐
   │                                   │       │    │ db.OnMessage
   │                                   │       │    │ set → respond
   │                                   │       │    └────│───┘
   │                                   │       │      yield#3 返回 → deduct_resp = {ok=true}
   │                                   │       │      return {ok=true, remaining=7}
   │                                   │       │    actor.respond_remote(src_node="nodeA", src_name="battle_service", sid, result)
   │                                   │       │       │
   │◀──────────────────────────────────│───────│───────│─── respond_remote 路由
   │                                   │       │       │
   │ C++ 协程恢复 → push resp ─────────│───────│       │
   │                                   │       yield#1 返回 → check_resp = {ok=true, remaining=7}
   │                                   │       战斗逻辑 → result = {ok=true, battle="victory", remaining_items=7}
   │                                   │    run_co 退出
   │                                   │    test_record("ok")
   │                                   │
   │◀── g_okCount == 1 ────────────────│
```

**测试验证**（[test_case/test_actor_lua_battle.cc](test_case/test_actor_lua_battle.cc)，VM 实测）：

```
[Battle] player=p1 item=potion count=3 outcome=victory remaining=7
[Battle] player=p1 item=potion count=5 outcome=victory remaining=2
[Battle] player=p1 item=potion count=10 REJECTED reason=insufficient has=2 need=10
[Verify] ok=2 fail=1 (expect ok=2 fail=1)
[PASS] Cross-server battle example OK!
```

**关键点**：

1. **三层 Lua 协程嵌套**：A 的 `run_co` 内 `cluster_call` → B 的 `run_co` 内 `actor.call(db, ...)` × 2 → 全程 yield 自动 propagate，业务脚本完全无感
2. **状态持久化**：DB 状态跨多次调用持久化（10 → 7 → 2），第 3 次因 2 < 10 被拒
3. **跨进程回程路由**：`actor.respond_remote` 用 OnMessage 的 `src_node + src_name` 自动路由回 nodeA 的 battle_service
4. **竞态注意**：多轮战斗需**串行触发**（等一轮结束再发下一轮），否则并发读 DB 会看到同一初始值。真实业务中如果允许同玩家并发操作，需要在 item_service 侧加锁/串行化（按 player_id 哈希到固定 actor 即可天然串行）

---

## 二、C++ 侧接口

```cpp
// 创建 LuaActor：按模块名构造，经 package.path 解析为 <root>/lua/<name>.lua
// 或 <root>/test_case/lua/<name>.lua，require 加载后顶层 function OnMessage
// 成为该 state 的全局 OnMessage
auto actor = std::make_unique<LuaActor>("my_service");
uint32_t id = sys.RegisterActor(std::unique_ptr<Actor>(std::move(actor)));

// 测试/外部访问 Lua 状态（仅 main 线程、注册前/无消息窗口内安全）
actor->Lua()["myCfg"] = 42;                          // 注入全局变量
actor->Lua().set_function("test_record", callback);  // 注入 C 函数

// 派生类也可继承 LuaActor 重写 OnCoroutineMessage（一般不需要）
```

**关键约束**（来自设计文档第十一章）：

- 桥接函数只入队消息 / 注册定时器，**永不同步回调**本 actor 的 `OnCoroutineMessage`（防重入）
- 不从 `main()` / `EventLoop` 线程直接碰 `sol::state`，必须走 `ActorSystem::Send` 触发
- 不跨 `co_await` 挂起点持 `sol::function` / `sol::table`（防 `lua_close` 后悬垂）

---

## 三、架构设计图

### 3.1 分层架构

```
┌─────────────────────────────────────────────────────────────┐
│                      业务 Lua 脚本                          │
│   function OnMessage(payload, src, sid, fd, ...) ... end   │
└──────────────────────────────┬──────────────────────────────┘
                               │ actor.send / actor.call / ...
┌──────────────────────────────┴──────────────────────────────┐
│                     LuaBridge（C 桥接层）                   │
│   actor.self / send / respond / call / cluster_call / ...  │
│   - actor.call / cluster_call 是纯 Lua（cmsgpack + yield）  │
│   - 其他是 sol2 lambda 捕获 LuaActor*                       │
└──────────────────────────────┬──────────────────────────────┘
                               │ sol::state_view
┌──────────────────────────────┴──────────────────────────────┐
│              LuaActor（C++ 派生 Actor）                     │
│   - LuaEnv env_                  RAII sol::state            │
│   - OnCoroutineMessage(msg)     每条消息建 lua_newthread     │
│   - lua_resume → yield → co_await Call → lua_resume         │
└──────────────────┬───────────────────────────┬──────────────┘
                   │                           │
       ┌───────────▼──────────┐     ┌──────────▼──────────┐
       │  Actor 基类          │     │  LuaEnv             │
       │  - Call/Send/Respond │     │  - sol::state       │
       │  - ClusterCall       │     │  - open_libraries   │
       │  - waitMap_          │     │  - preload cmsgpack │
       │  - scheduled_        │     └─────────────────────┘
       └───────────┬──────────┘
                   │
       ┌───────────▼──────────┐
       │  ActorSystem         │
       │  - workerLoop × N    │
       │  - readyQueue_       │
       │  - RegisterActor      │
       │  - Send / SendToRemote│
       └──────────────────────┘
```

### 3.2 线程模型（为什么不需要锁）

```
        ┌─────┐  ┌─────┐  ┌─────┐  ┌─────┐
        │ W1  │  │ W2  │  │ W3  │  │ W4  │   N 个 worker 线程
        └──┬──┘  └──┬──┘  └──┬──┘  └──┬──┘
           │        │        │        │
           └────────┴────────┴────────┘
                        │
                        ▼
              ┌──────────────────┐
              │  readyQueue_     │  ← CAS scheduled_ 去重
              └────────┬─────────┘
                       │ pop
        ┌──────────────┴──────────────┐
        │  Actor::ProcessOne()          │  串行不变量：
        │  └─ OnMessage(msg)            │  ① 同一 actor 同时只被一个 worker 处理
        │     └─ OnCoroutineMessage    │  ② 协程跨 worker 迁移不破坏①
        │        └─ lua_resume(L_th)    │  ③ main/EventLoop 线程不碰 sol::state
        └──────────────────────────────┘

  结论：任意时刻至多一个 worker 在驱动某 actor 的 lua_State，无需锁
```

### 3.3 协程融合（C++20 ↔ Lua）

两套协程**正交、交替驱动、互不嵌套**：

```
                  ┌─────────────────────────────────┐
                  │     C++ 协程（OnCoroutineMessage）│
                  │     保存：coroutine_frame        │
                  │     局部：lua_State* th, thRef   │
                  └────────┬────────────────────────┘
                           │ lua_resume(th, ...)
                           ▼
            ┌─────────────────────────────────────────┐
            │     Lua 协程（lua_newthread 派生）       │
            │     保存：Lua VM 内的协程栈             │
            │     ┌──────────────────────────────┐    │
            │     │  OnMessage(payload, ...)     │    │
            │     │  └─ actor.call(target, req)  │    │
            │     │      └─ coroutine.yield(sig)│────┼──→ 返回 LUA_YIELD 给 C++
            │     │         (挂起，等待恢复)     │    │
            │     └──────────────────────────────┘    │
            └─────────────────────────────────────────┘
                           ▲
                           │ lua_resume(th, 1, &nres) — 推响应 bytes
                           │
                  ┌────────┴────────────────────────┐
                  │     C++ 协程恢复                  │
                  │     co_await Call(...) 完成后     │
                  │     把 resp.Data() 推到 th 栈     │
                  └─────────────────────────────────┘
```

**关键点**：

- Lua `coroutine.yield` 时，C++ 的 `lua_resume` 返回 `LUA_YIELD`——这是普通 C 函数调用返回，**不是 C++ 协程栈帧夹着半挂起的 Lua 帧**
- C++ 协程在 `co_await Call` 处挂起，worker 释放；响应到达后从挂起点恢复
- 恢复后 C++ 把响应 bytes push 到 th 栈，再次 `lua_resume`——Lua 从 `coroutine.yield` 处继续

### 3.4 单 Session 表（简化方案）

设计文档原本要求两张 session 表（C++ `waitMap_` + Lua `luaSession_`），实际 Phase 3 实现简化为**一张表**——C++ 协程持有 Lua dispatch 引用作为局部变量跨 `co_await` 保留，无需 lookup：

```
设计文档方案（未采纳）：              实际实现（简化）：
┌─────────────────────────┐         ┌──────────────────────────┐
│ C++ waitMap_[sid] → C++ │         │ C++ waitMap_[sid] → C++  │
│                        │         │   coroutine_handle       │
│ Lua luaSession_[sid2]  │         │                          │
│   → dispatch ref       │         │ C++ coroutine 局部变量：  │
│                        │         │   lua_State* th           │
│ OnMessage 收到 Response │         │   int thRef (RAII)        │
│   先查 waitMap_         │         │                          │
│   再查 luaSession_      │         │ （跨 co_await 保留）       │
└─────────────────────────┘         └──────────────────────────┘
```

**为什么能简化**：Lua 协程总是在 C++ 协程内部启动、yield、resume——C++ 协程的局部状态足够承载 Lua dispatch 引用，不需要跨消息查找。Response 路由仍走 C++ `waitMap_` → 恢复 C++ 协程 → 再 resume Lua。

---

## 四、业务流程图

### 4.1 Phase 1 — 同步 cmsgpack ping-pong

```
   main                 Client LuaActor         Server LuaActor
    │                          │                        │
    │── Send(kickoff) ────────▶│                        │
    │                          │                        │
    │                     ┌────▼─────┐                  │
    │                     │OnMessage │                  │
    │                     │(start)   │                  │
    │                     │  for i=1..10:                │
    │                     │    actor.send(serverId, ping)│
    │                     └────┬─────┘                  │
    │                          │                        │
    │                          │── cmsgpack.pack ──▶    │
    │                          │── SendToActor ──────▶  │
    │                          │                     ┌──▼───┐
    │                          │                     │OnMsg │
    │                          │                     │ping  │
    │                          │                     │      │
    │                          │  ◀── actor.send(src, pong) │
    │                          │  ◀────── test_record("ping")
    │                          │                     └──────┘
    │                     ┌────▼─────┐                  │
    │                     │OnMessage │                  │
    │                     │(pong)    │                  │
    │                     │test_record("pong")         │
    │                     └──────────┘                  │
    │                          │                        │
    │◀── poll g_pongCount == 10 │                        │
    │                          │                        │
```

### 4.2 Phase 2 — C++ co_await Call ↔ LuaActor respond

```
   main                 CppCallerActor         LuaStateService
    │                          │                        │
    │── Send(trigger) ────────▶│                        │
    │                          │                        │
    │                     ┌────▼─────┐                  │
    │                     │OnCoroutine│                 │
    │                     │Message    │                 │
    │                     │(trigger)  │                 │
    │                     │           │                 │
    │                     │  co_await Call(stateSvc,    │
    │                     │     set_and_get req)         │
    │                     │     ──┐                     │
    │                     │       │ C++ 协程挂起        │
    │                     │       │ waitMap_[sid]=h     │
    │                     │       │ worker 释放         │
    │                     │       ▼                     │
    │                     │   Send(stateSvc, req, sid) │
    │                     │   ────────────────────────▶ │
    │                     │                          ┌──▼──────┐
    │                     │                          │OnMessage│
    │                     │                          │set_and_  │
    │                     │                          │ get     │
    │                     │                          │ state[k]=v│
    │                     │   ◀────────────────────  │ actor.   │
    │                     │   ◀── RespondToCall(req,  │ respond(│
    │                     │       resp, sid)          │  src,sid│
    │                     │                          │  ,resp) │
    │                     │       ┌─ isResponse=true  └─────────┘
    │                     │       │ sessionId=sid
    │                     │       ▼
    │                     │  OnMessage → ResumeWaiting│
    │                     │  waitMap_[sid] → h.resume()│
    │                     │       │                   │
    │                     │  C++ 协程恢复              │
    │                     │  resp = await_resume()    │
    │                     │       │                   │
    │                     │  g_okCount++              │
    │                     └──────────┘                 │
```

### 4.3 Phase 3 — Lua actor.call yield ↔ C++ co_await Call

```
 Caller LuaActor                C++ 协程                    Service LuaActor
      │                            │                              │
  ┌──▼──────┐                     │                              │
  │OnMessage│                     │                              │
  │(trigger)│                     │                              │
  │ actor.  │                     │                              │
  │ call(svc,req)                 │                              │
  │  cmsgpack.pack(req)            │                              │
  │  coroutine.yield               │                              │
  │   ({action="wait",             │                              │
  │     target, payload})         │                              │
  └─────┬───┘                     │                              │
        │ LUA_YIELD                │                              │
        └────────────────────────▶│                              │
                                  │ ReadYieldSignal(th)         │
                                  │  - action="wait"             │
                                  │  - target=svc                │
                                  │  - payload=packed            │
                                  │                             │
                                  │ co_await Call(target,        │
                                  │   payload) ──┐              │
                                  │   (C++ 协程挂起,             │
                                  │    waitMap_[sid]=h,          │
                                  │    worker 释放)              │
                                  │             │                │
                                  │             ▼                │
                                  │  SendToActor(target,         │
                                  │   req, sid)─────────────────▶│
                                  │                          ┌───▼────┐
                                  │                          │OnMessage│
                                  │                          │(query)  │
                                  │                          │actor.   │
                                  │                          │ respond(│
                                  │   ◀───────────────────── │  src,sid│
                                  │   ◀── RespondToCall(req,  │  ,resp) │
                                  │       resp, sid)          └─────────┘
                                  │  isResponse=true           │
                                  │  sessionId=sid             │
                                  │             │              │
                                  │             ▼              │
                                  │ OnMessage → ResumeWaiting  │
                                  │  waitMap_[sid] → h.resume()│
                                  │             │              │
                                  │ C++ 协程恢复                │
                                  │ resp = await_resume()       │
                                  │             │              │
                                  │ push resp.Data() to th     │
                                  │ lua_resume(th, 1) ─────────▶│
                                  │             │               │
                                  ▼             │               │
                              (yield 返回 bytes)               │
                                              │                │
                                  ◀───────────┘                │
                              r = cmsgpack.unpack(bytes)       │
                              actor.call 返回 r                 │
                                  │                            │
                              OnMessage 继续                    │
                              test_record("ok")                │
```

### 4.4 Phase 4 — 跨进程 Lua actor.cluster_call

```
 nodeA / lua_caller            nodeB / lua_service
 ─────────────────             ──────────────────
 ┌────────────────┐
 │OnMessage       │
 │(trigger,       │
 │ is_remote=     │
 │ false)         │
 │                │
 │ actor.cluster_ │
 │  call("nodeB", │
 │   "lua_service"│
 │   , req)       │
 │  yield         │
 │   {action=     │
 │    "cluster_   │
 │     call",    │
 │    nodeId,     │
 │    actorName,  │
 │    payload}   │
 └───────┬───────┘
         │ LUA_YIELD
         ▼
 ┌────────────────┐
 │ C++ 协程:      │
 │ ReadYieldSignal│
 │  action=       │
 │   "cluster_call"│
 │  nodeId,       │
 │  actorName,    │
 │  payload       │
 │                │
 │ co_await       │
 │  ClusterCall(  │
 │   nodeId,      │
 │   actorName,   │
 │   payload)     │
 │  ──┐           │      ┌────────────────────────┐
 │    │ (挂起,    │      │  transportA.SendPacket │
 │    │  waitMap_ │      │  targetNodeId="nodeB"  │
 │    │  [sid]=h, │      └───────────┬────────────┘
 │    │  worker   │                  │ LoopbackTransport
 │    │  释放)    │                  ▼
 │    │           │      ┌────────────────────────┐
 │    │           │      │  nodeB ClusterReceiver  │
 │    │           │      │   .OnPacketReceived     │
 │    │           │      │   SendByName("lua_     │
 │    │           │      │    service", msg)       │
 │    │           │      └───────────┬────────────┘
 │    │           │                  ▼
 │    │           │              ┌───────────────────────┐
 │    │           │              │  nodeB / lua_service  │
 │    │           │              │  OnMessage(payload,   │
 │    │           │              │   src, sid, fd,       │
 │    │           │              │   is_remote=TRUE,    │
 │    │           │              │   src_node="nodeA",  │
 │    │           │              │   src_name="lua_     │
 │    │           │              │    caller")          │
 │    │           │              │                      │
 │    │           │              │  actor.respond_remote│
 │    │           │              │   (src_node,src_name,│
 │    │           │              │    sid, resp)        │
 │    │           │              └───────────┬───────────┘
 │    │           │                          │ RespondRemote →
 │    │           │                          │ transportB.SendPacket
 │    │           │                          ▼
 │    │           │              ┌────────────────────────┐
 │    │           │              │  nodeA ClusterReceiver│
 │    │           │              │   → SendByName        │
 │    │           │              │     ("lua_caller",   │
 │    │           │              │      response)       │
 │    │           │              └───────────┬────────────┘
 │    │           │                          ▼
 │    │           │              ┌────────────────────────┐
 │    │           │              │  OnMessage(Response)  │
 │    │           │              │   isResponse=true      │
 │    │           │              │   sessionId=sid        │
 │    │           │              │   ResumeWaiting(h)    │
 │    │           │              └───────────┬────────────┘
 │    │           │                          │
 │    │           │◀─────────────────────────┘
 │    │           │
 │  C++ 协程恢复 │
 │  resp = await │
 │   _resume()   │
 │  push resp.   │
 │   Data() to th│
 │  lua_resume   │
 │   (th, 1)    │
 └──────┬───────┘
        ▼
  Lua: actor.cluster_call 返回 unpack(resp)
  OnMessage 继续 → test_record("ok")
```

---

## 五、决策记录

| 决策 | 选择 | 理由 |
|---|---|---|
| Lua 状态隔离 | 每 actor 一个 `sol::state` | 复用 actor 串行不变量，无需锁 |
| 序列化格式 | cmsgpack（Lua/C++ 双向共用） | 设计文档指定，跨 actor/进程零转换 |
| `actor.call` 实现 | 纯 Lua（`cmsgpack.pack + coroutine.yield`） | 比写 C 函数调 `lua_yield` 简单，sol2 set_function 不直接支持 yield |
| Lua 协程创建 | `lua_newthread`（每条消息一个） | skynet 模型，避免 yield 互相阻塞 |
| Lua 引用防 GC | `luaL_ref` 到 registry + RAII 包装 | 跨 `co_await` 持有，析构时安全释放 |
| Session 表 | **单表**（C++ `waitMap_`），Lua dispatch 作 C++ 协程局部 | 比设计文档原方案简化，不需 luaSession_ 表 |
| 桥接函数 | sol2 lambda 捕获 `LuaActor*` | 简洁，析构顺序天然安全 |
| 错误处理 | `sol::protected_function` + try-catch + `luaL_error` | 对齐 ActorSystem.cc:530 try-catch 语义 |
| cmsgpack 加载 | 静态链接 `-l:cmsgpack.so` + `luaL_requiref` 预加载 | 无运行时路径解析，无 dlopen |
| GCC 11.2 兼容 | `ActorSystem::RegisterActor` 加 template overload | 一处修复，50+ 调用点全过 |
| 嵌套协程 propagate | `actor.run_co(fn, ...)` Lua helper（自动 yield propagate） | 业务无感；C++ 侧零改动；支持任意层嵌套 |

---

## 附：测试入口

所有测试位于 [test_case/](test_case/)，构建产物（二进制）也在 `test_case/`。

```bash
# 单跑
bash test_case/run_test_lua.sh          # Phase 1: 同步 cmsgpack ping-pong
bash test_case/run_test_lua_state.sh    # Phase 2: C++ Call ↔ LuaActor respond
bash test_case/run_test_lua_call.sh     # Phase 3: Lua actor.call yield
bash test_case/run_test_lua_runco.sh    # 嵌套协程 propagate（actor.run_co）
bash test_case/run_test_lua_cluster.sh  # Phase 4: Lua actor.cluster_call
bash test_case/run_test_lua_battle.sh   # 跨服战斗业务示例
bash test_case/run_test_lua_bench.sh    # Phase 4: Lua vs C++ 性能基准

# 一键跑全部（不含 multi_reactor_bench）
bash test_case/run_all.sh
```

Phase 4 benchmark 实测数据（VM CentOS 7 + g++ 11.2.0，1000 次 roundtrip）：

- Lua `actor.call`: ~54 µs/op, ~18k ops/sec
- C++ `co_await Call`: ~46 µs/op, ~22k ops/sec
- **Lua/C++ 延迟比: 1.11~1.24x**（Lua 桥接层仅多 ~15-20% 开销）
