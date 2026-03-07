/**
 * @file test_coroutine_actor.cc
 * @brief C++20 协程 Actor 测试 — 演示 Skynet 风格的服务协程调用
 *
 * 核心演示：服务的函数调用跑在协程中，co_await 挂起/恢复，
 *           代码看起来是同步的，实际是异步执行。
 *
 * 架构：
 *   DatabaseActor（普通 Actor）：KV 存储，响应 get/set 查询
 *   GameServiceActor（CoroutineActor）：游戏逻辑服务
 *     - 收到 "query_player" → co_await Call(db, "get:name") → co_await Call(db, "get:level")
 *       → co_await Sleep(50ms) → 组合结果
 *
 * 对比 Skynet：
 *   Skynet Lua:
 *     function CMD.query_player(source)
 *         local name  = skynet.call(db, "lua", "get", "name")
 *         local level = skynet.call(db, "lua", "get", "level")
 *         skynet.sleep(5)  -- 50ms
 *         skynet.ret(skynet.pack(name .. ":" .. level))
 *     end
 *
 *   本框架 C++20:
 *     ActorTask GameServiceActor::OnCoroutineMessage(ActorMessage msg) {
 *         auto r1 = co_await Call(dbId, {..."get:name"});
 *         auto r2 = co_await Call(dbId, {..."get:level"});
 *         co_await Sleep(50);
 *         Respond(msg, {..."result"});
 *     }
 *
 * 测试项：
 *   Test1: 基本 Call — 协程 Actor 调用数据库 Actor 并等待响应
 *   Test2: 多步串行 Call — 一个协程内多次 co_await Call
 *   Test3: Sleep — 协程休眠
 *   Test4: 并发协程 — 多条消息各自创建协程，交错挂起/恢复
 *   Test5: 链式 Call — A co_await Call B, B co_await Call C, C 响应
 *
 * 编译: make -f Makefile.coroutine
 * 运行: ./run_test_coroutine.sh
 */

#include "precompiled.h"
#include "Actor.h"
#include "CoroutineActor.h"
#include "ActorSystem.h"
#include "Message.h"

#include <cassert>
#include <chrono>
#include <sstream>

using namespace bllsll;

// ============================================================
//  DatabaseActor（普通 Actor）：模拟 KV 数据库
//  接收 "get:key" → 查询并 RespondToCall
//  接收 "set:key:value" → 存储
// ============================================================
class DatabaseActor : public Actor
{
public:
    std::unordered_map<std::string, std::string> db_;
    std::atomic<int> queryCount{0};

    void OnMessage(ActorMessage& msg) override
    {
        if (msg.type != MsgType::UserMessage) return;

        if (msg.data.find("get:") == 0) {
            queryCount.fetch_add(1);
            std::string key = msg.data.substr(4);
            std::string value = db_.count(key) ? db_[key] : "not_found";
            std::cout << "[DatabaseActor] get '" << key << "' = '" << value
                      << "' (session=" << msg.sessionId << ")" << std::endl;

            // 用 RespondToCall 响应：自动设置 sessionId + isResponse
            RespondToCall(msg, ActorMessage{MsgType::UserMessage, 0, -1, value});

        } else if (msg.data.find("set:") == 0) {
            // "set:key:value"
            auto firstColon = msg.data.find(':', 4);
            if (firstColon != std::string::npos) {
                std::string key = msg.data.substr(4, firstColon - 4);
                std::string value = msg.data.substr(firstColon + 1);
                db_[key] = value;
                std::cout << "[DatabaseActor] set '" << key << "' = '" << value << "'" << std::endl;
            }
        }
    }
};

// ============================================================
//  GameServiceActor（CoroutineActor）：游戏逻辑服务
//  所有消息处理都在协程中，可以使用 co_await
// ============================================================
class GameServiceActor : public CoroutineActor
{
public:
    uint32_t dbActorId = 0;

    // 测试验证用
    std::atomic<int> completedQueries{0};
    std::atomic<bool> sleepCompleted{false};
    bllsll::SpinLockQueue<std::string> results;

    // ===== Skynet 风格的协程消息处理 =====
    ActorTask OnCoroutineMessage(ActorMessage msg) override
    {
        std::cout << "[GameService] processing: " << msg.data
                  << " (session=" << msg.sessionId << ")" << std::endl;

        if (msg.data == "query_player") {
            // ---- 类似 skynet.call() 的用法 ----
            // 第一次查询：获取玩家名称
            auto resp1 = co_await Call(dbActorId,
                ActorMessage{MsgType::UserMessage, 0, -1, "get:player_name"});
            std::string name = resp1.data;
            std::cout << "[GameService] got player_name = " << name << std::endl;

            // 第二次查询：获取玩家等级
            auto resp2 = co_await Call(dbActorId,
                ActorMessage{MsgType::UserMessage, 0, -1, "get:player_level"});
            std::string level = resp2.data;
            std::cout << "[GameService] got player_level = " << level << std::endl;

            // 组合结果
            std::string result = name + ":lv" + level;
            results.push(result);
            completedQueries.fetch_add(1);

            std::cout << "[GameService] query_player done: " << result << std::endl;

            // 如果有人 Call 了我们，可以 Respond 回去
            if (msg.sessionId > 0) {
                Respond(msg, ActorMessage{MsgType::UserMessage, 0, -1, result});
            }

        } else if (msg.data == "test_sleep") {
            // ---- 类似 skynet.sleep() 的用法 ----
            std::cout << "[GameService] sleeping 100ms..." << std::endl;
            auto start = std::chrono::steady_clock::now();

            co_await Sleep(100);

            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            std::cout << "[GameService] woke up after " << elapsed << "ms" << std::endl;
            sleepCompleted.store(true);

        } else if (msg.data.find("simple:") == 0) {
            // 简单处理，不需要 co_await（但仍然是协程）
            std::string value = msg.data.substr(7);
            results.push("simple:" + value);
            completedQueries.fetch_add(1);
            std::cout << "[GameService] simple processed: " << value << std::endl;
            co_return;  // 显式 co_return（也可以省略，到函数末尾等效）
        }
    }
};

// ============================================================
//  MiddleServiceActor（CoroutineActor）：中间服务，用于链式调用测试
//  收到请求 → co_await Call 下游 → 加工结果 → Respond
// ============================================================
class MiddleServiceActor : public CoroutineActor
{
public:
    uint32_t downstreamId = 0;
    std::atomic<int> processCount{0};

    ActorTask OnCoroutineMessage(ActorMessage msg) override
    {
        if (msg.data.find("forward:") == 0) {
            std::string query = msg.data.substr(8);
            std::cout << "[MiddleService] forwarding query: " << query << std::endl;

            // co_await Call 下游服务
            auto resp = co_await Call(downstreamId,
                ActorMessage{MsgType::UserMessage, 0, -1, query});

            std::string processed = "middle(" + resp.data + ")";
            processCount.fetch_add(1);
            std::cout << "[MiddleService] result: " << processed << std::endl;

            // 响应上游调用者
            Respond(msg, ActorMessage{MsgType::UserMessage, 0, -1, processed});
        }
    }
};

// ============================================================
//  CallerActor（CoroutineActor）：发起调用的上游服务
// ============================================================
class CallerActor : public CoroutineActor
{
public:
    uint32_t targetId = 0;
    std::atomic<bool> done{false};
    std::string finalResult;

    ActorTask OnCoroutineMessage(ActorMessage msg) override
    {
        if (msg.data == "start_chain") {
            std::cout << "[CallerActor] starting chain call..." << std::endl;

            auto resp = co_await Call(targetId,
                ActorMessage{MsgType::UserMessage, 0, -1, "forward:get:chain_value"});

            finalResult = resp.data;
            done.store(true);
            std::cout << "[CallerActor] chain call result: " << finalResult << std::endl;
        }
    }
};

// ============================================================
//  辅助函数
// ============================================================
static bool WaitFor(std::atomic<bool>& flag, int timeoutMs = 5000)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!flag.load() && std::chrono::steady_clock::now() < deadline) {
        usleep(5000);
    }
    return flag.load();
}

static bool WaitForCount(std::atomic<int>& count, int target, int timeoutMs = 5000)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (count.load() < target && std::chrono::steady_clock::now() < deadline) {
        usleep(5000);
    }
    return count.load() >= target;
}

// ============================================================
//  Test1: 基本 Call — 协程 Actor 调用 Database Actor
// ============================================================
bool Test1_BasicCall()
{
    std::cout << "\n========== Test1: Basic Call ==========" << std::endl;
    std::cout << "  GameServiceActor co_await Call(DatabaseActor, \"get:key\")" << std::endl;

    ActorSystem sys;
    sys.Start(4, nullptr);

    // 创建 DatabaseActor 并预填数据
    auto dbPtr = std::make_unique<DatabaseActor>();
    DatabaseActor* db = dbPtr.get();
    db->db_["player_name"] = "Alice";
    db->db_["player_level"] = "42";
    uint32_t dbId = sys.RegisterActor(std::move(dbPtr));

    // 创建 GameServiceActor
    auto svcPtr = std::make_unique<GameServiceActor>();
    GameServiceActor* svc = svcPtr.get();
    svcPtr->dbActorId = dbId;
    uint32_t svcId = sys.RegisterActor(std::move(svcPtr));

    // 发送查询消息
    sys.Send(svcId, ActorMessage{MsgType::UserMessage, 0, -1, "query_player"});

    // 等待完成
    bool ok = WaitForCount(svc->completedQueries, 1, 5000);

    if (ok) {
        auto result = svc->results.pop();
        if (result && *result == "Alice:lv42") {
            std::cout << "[PASS] Basic Call: result = " << *result << std::endl;
        } else {
            std::cout << "[FAIL] Basic Call: unexpected result = "
                      << (result ? *result : "none") << std::endl;
            ok = false;
        }
    } else {
        std::cout << "[FAIL] Basic Call: timeout!" << std::endl;
    }

    // 验证 DB 被查询了 2 次（name + level）
    if (db->queryCount.load() != 2) {
        std::cout << "[FAIL] DB queryCount=" << db->queryCount.load() << ", expected=2" << std::endl;
        ok = false;
    }

    sys.Stop();
    return ok;
}

// ============================================================
//  Test2: Sleep — 协程休眠
// ============================================================
bool Test2_Sleep()
{
    std::cout << "\n========== Test2: Sleep ==========" << std::endl;
    std::cout << "  GameServiceActor co_await Sleep(100ms)" << std::endl;

    ActorSystem sys;
    sys.Start(2, nullptr);

    auto svcPtr = std::make_unique<GameServiceActor>();
    GameServiceActor* svc = svcPtr.get();
    uint32_t svcId = sys.RegisterActor(std::move(svcPtr));

    auto start = std::chrono::steady_clock::now();
    sys.Send(svcId, ActorMessage{MsgType::UserMessage, 0, -1, "test_sleep"});

    bool ok = WaitFor(svc->sleepCompleted, 5000);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    if (ok) {
        if (elapsed >= 80) {  // 允许一些误差
            std::cout << "[PASS] Sleep: completed in " << elapsed << "ms (>= 80ms)" << std::endl;
        } else {
            std::cout << "[FAIL] Sleep: completed too fast (" << elapsed << "ms)" << std::endl;
            ok = false;
        }
    } else {
        std::cout << "[FAIL] Sleep: timeout!" << std::endl;
    }

    sys.Stop();
    return ok;
}

// ============================================================
//  Test3: 并发协程 — 多条消息各创建协程，交错执行
//
//  这是 Skynet 的核心特性：
//  同一个 Actor 收到 3 条消息，各自创建协程：
//    协程1: co_await Call(db, get:name) → 挂起
//    协程2: co_await Call(db, get:level) → 挂起
//    协程3: 无 co_await，直接完成
//  当 DB 响应到达时，协程 1、2 分别恢复。
//  三个协程在同一个 Actor 上交错执行，但串行（邮箱保证）。
// ============================================================
bool Test3_ConcurrentCoroutines()
{
    std::cout << "\n========== Test3: Concurrent Coroutines ==========" << std::endl;
    std::cout << "  3 messages → 3 coroutines on same Actor, interleaved" << std::endl;

    ActorSystem sys;
    sys.Start(4, nullptr);

    auto dbPtr = std::make_unique<DatabaseActor>();
    DatabaseActor* db = dbPtr.get();
    db->db_["player_name"] = "Bob";
    db->db_["player_level"] = "99";
    uint32_t dbId = sys.RegisterActor(std::move(dbPtr));

    auto svcPtr = std::make_unique<GameServiceActor>();
    GameServiceActor* svc = svcPtr.get();
    svcPtr->dbActorId = dbId;
    uint32_t svcId = sys.RegisterActor(std::move(svcPtr));

    // 同时发送 3 条消息
    sys.Send(svcId, ActorMessage{MsgType::UserMessage, 0, -1, "query_player"});
    sys.Send(svcId, ActorMessage{MsgType::UserMessage, 0, -1, "query_player"});
    sys.Send(svcId, ActorMessage{MsgType::UserMessage, 0, -1, "simple:fast_path"});

    // 等待所有完成（2 个 query_player + 1 个 simple = 3）
    bool ok = WaitForCount(svc->completedQueries, 3, 5000);

    if (ok) {
        std::cout << "[PASS] Concurrent: all 3 coroutines completed" << std::endl;
        std::cout << "  DB queries: " << db->queryCount.load()
                  << " (expected 4: 2 per query_player × 2)" << std::endl;
    } else {
        std::cout << "[FAIL] Concurrent: only " << svc->completedQueries.load()
                  << "/3 completed" << std::endl;
    }

    // DB 应该被查询 4 次（2 个 query_player，每个查 2 次）
    if (db->queryCount.load() != 4) {
        std::cout << "[FAIL] DB queryCount=" << db->queryCount.load() << ", expected=4" << std::endl;
        ok = false;
    }

    sys.Stop();
    return ok;
}

// ============================================================
//  Test4: 链式 Call — CallerActor → MiddleServiceActor → DatabaseActor
//
//  CallerActor:  co_await Call(middle, "forward:get:chain_value")
//  MiddleService: co_await Call(db, "get:chain_value") → Respond("middle(result)")
//  DatabaseActor: RespondToCall(value)
//
//  展示：协程 Call 可以跨多个 Actor 链式传递，
//        每层都可以用 co_await 等待下层结果。
// ============================================================
bool Test4_ChainCall()
{
    std::cout << "\n========== Test4: Chain Call ==========" << std::endl;
    std::cout << "  CallerActor → MiddleServiceActor → DatabaseActor" << std::endl;

    ActorSystem sys;
    sys.Start(4, nullptr);

    // 创建 DatabaseActor
    auto dbPtr = std::make_unique<DatabaseActor>();
    dbPtr->db_["chain_value"] = "treasure";
    uint32_t dbId = sys.RegisterActor(std::move(dbPtr));

    // 创建 MiddleServiceActor
    auto midPtr = std::make_unique<MiddleServiceActor>();
    MiddleServiceActor* mid = midPtr.get();
    midPtr->downstreamId = dbId;
    uint32_t midId = sys.RegisterActor(std::move(midPtr));

    // 创建 CallerActor
    auto callerPtr = std::make_unique<CallerActor>();
    CallerActor* caller = callerPtr.get();
    callerPtr->targetId = midId;
    uint32_t callerId = sys.RegisterActor(std::move(callerPtr));

    // 触发链式调用
    sys.Send(callerId, ActorMessage{MsgType::UserMessage, 0, -1, "start_chain"});

    bool ok = WaitFor(caller->done, 5000);

    if (ok) {
        if (caller->finalResult == "middle(treasure)") {
            std::cout << "[PASS] Chain Call: result = " << caller->finalResult << std::endl;
        } else {
            std::cout << "[FAIL] Chain Call: unexpected result = "
                      << caller->finalResult << std::endl;
            ok = false;
        }
    } else {
        std::cout << "[FAIL] Chain Call: timeout!" << std::endl;
    }

    if (mid->processCount.load() != 1) {
        std::cout << "[FAIL] MiddleService processCount="
                  << mid->processCount.load() << ", expected=1" << std::endl;
        ok = false;
    }

    sys.Stop();
    return ok;
}

// ============================================================
//  Test5: 综合测试 — 多个 CoroutineActor + Call + Sleep + 并发
//
//  模拟 MMO 场景：
//    PlayerServiceActor（CoroutineActor）处理玩家登录：
//      1. co_await Call(db, "get:player_data")
//      2. co_await Sleep(30ms)  // 模拟处理延迟
//      3. co_await Call(db, "set:last_login:now")
//      4. 完成
// ============================================================
class PlayerServiceActor : public CoroutineActor
{
public:
    uint32_t dbActorId = 0;
    std::atomic<int> loginCount{0};

    ActorTask OnCoroutineMessage(ActorMessage msg) override
    {
        if (msg.data.find("login:") == 0) {
            std::string playerName = msg.data.substr(6);
            std::cout << "[PlayerService] handling login for " << playerName << "..." << std::endl;

            // 1. 查询玩家数据
            auto resp = co_await Call(dbActorId,
                ActorMessage{MsgType::UserMessage, 0, -1, "get:player_" + playerName});
            std::cout << "[PlayerService] " << playerName
                      << " data = " << resp.data << std::endl;

            // 2. 模拟处理延迟
            co_await Sleep(30);
            std::cout << "[PlayerService] " << playerName
                      << " processing done after sleep" << std::endl;

            // 3. 更新最后登录时间
            // 注意：set 操作不需要 co_await（fire-and-forget）
            SendToActor(dbActorId,
                ActorMessage{MsgType::UserMessage, 0, -1,
                    "set:last_login_" + playerName + ":now"});

            loginCount.fetch_add(1);
            std::cout << "[PlayerService] " << playerName << " login complete!" << std::endl;
        }
    }
};

bool Test5_Integration()
{
    std::cout << "\n========== Test5: Integration (MMO Login) ==========" << std::endl;
    std::cout << "  3 players login concurrently via CoroutineActor" << std::endl;

    ActorSystem sys;
    sys.Start(4, nullptr);

    // 创建 DatabaseActor
    auto dbPtr = std::make_unique<DatabaseActor>();
    DatabaseActor* db = dbPtr.get();
    db->db_["player_Alice"] = "warrior_lv50";
    db->db_["player_Bob"] = "mage_lv30";
    db->db_["player_Charlie"] = "archer_lv45";
    uint32_t dbId = sys.RegisterActor(std::move(dbPtr));

    // 创建 PlayerServiceActor
    auto svcPtr = std::make_unique<PlayerServiceActor>();
    PlayerServiceActor* svc = svcPtr.get();
    svcPtr->dbActorId = dbId;
    uint32_t svcId = sys.RegisterActor(std::move(svcPtr));

    // 3 个玩家同时登录（产生 3 个并发协程）
    sys.Send(svcId, ActorMessage{MsgType::UserMessage, 0, -1, "login:Alice"});
    sys.Send(svcId, ActorMessage{MsgType::UserMessage, 0, -1, "login:Bob"});
    sys.Send(svcId, ActorMessage{MsgType::UserMessage, 0, -1, "login:Charlie"});

    bool ok = WaitForCount(svc->loginCount, 3, 10000);

    if (ok) {
        std::cout << "[PASS] Integration: all 3 logins completed" << std::endl;
        std::cout << "  DB queries: " << db->queryCount.load() << std::endl;
        // 验证 last_login 被设置
        bool hasAlice = db->db_.count("last_login_Alice") > 0;
        bool hasBob = db->db_.count("last_login_Bob") > 0;
        bool hasCharlie = db->db_.count("last_login_Charlie") > 0;
        if (!hasAlice || !hasBob || !hasCharlie) {
            std::cout << "[FAIL] Missing last_login entries: Alice="
                      << hasAlice << " Bob=" << hasBob << " Charlie=" << hasCharlie << std::endl;
            ok = false;
        }
    } else {
        std::cout << "[FAIL] Integration: only " << svc->loginCount.load()
                  << "/3 logins completed" << std::endl;
    }

    sys.Stop();
    return ok;
}

// ============================================================
//  main
// ============================================================
int main()
{
    std::cout << "==================================================" << std::endl;
    std::cout << "  C++20 Coroutine Actor Test (Skynet-style)" << std::endl;
    std::cout << "==================================================" << std::endl;
    std::cout << std::endl;
    std::cout << "  Skynet 风格 API 对照:" << std::endl;
    std::cout << "    skynet.call(addr, ...)   <=>  co_await Call(actorId, msg)" << std::endl;
    std::cout << "    skynet.ret(...)          <=>  Respond(msg, response)" << std::endl;
    std::cout << "    skynet.sleep(n)          <=>  co_await Sleep(ms)" << std::endl;
    std::cout << "    skynet.send(addr, ...)   <=>  SendToActor(actorId, msg)" << std::endl;
    std::cout << std::endl;

    int passed = 0;
    int failed = 0;

    if (Test1_BasicCall())             ++passed; else ++failed;
    if (Test2_Sleep())                 ++passed; else ++failed;
    if (Test3_ConcurrentCoroutines())  ++passed; else ++failed;
    if (Test4_ChainCall())             ++passed; else ++failed;
    if (Test5_Integration())           ++passed; else ++failed;

    std::cout << "\n==================================================" << std::endl;
    std::cout << "  Result: " << passed << " passed, " << failed << " failed" << std::endl;

    if (failed == 0) {
        std::cout << std::endl;
        std::cout << "  Key design points:" << std::endl;
        std::cout << "  1. OnCoroutineMessage() 是协程函数，每条消息创建一个协程" << std::endl;
        std::cout << "  2. co_await Call() 挂起协程，释放 worker 线程" << std::endl;
        std::cout << "  3. 响应到达时自动恢复协程，代码看起来完全同步" << std::endl;
        std::cout << "  4. 同一 Actor 上多个协程可以交错执行（类似 Skynet）" << std::endl;
        std::cout << "  5. 链式 Call 自然支持：A → B → C → 响应逐层返回" << std::endl;
    }

    std::cout << "==================================================" << std::endl;

    return (failed > 0) ? 1 : 0;
}
