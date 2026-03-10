/**
 * @file test_actor_battle.cc
 * @brief Actor 模型下的 MMO 战斗场景测试（纯 Actor 间消息，无网络）
 *
 * 核心问题：两个 PlayerActor 在不同 worker 线程上运行，如何安全地战斗？
 *
 * 答案：引入 BattleActor（中介者），所有战斗操作都通过消息发给 BattleActor，
 *       BattleActor 串行处理所有战斗消息，不存在并发问题。
 *
 * 架构：
 *   PlayerActorA ──"attack"──> BattleActor ──"damage_result"──> PlayerActorB
 *   PlayerActorB ──"attack"──> BattleActor ──"damage_result"──> PlayerActorA
 *
 *   BattleActor 拥有战斗状态（HP、回合等），串行处理所有攻击请求，
 *   计算伤害后将结果通知双方。
 *
 * 为什么没有多线程问题：
 *   1. PlayerActorA 的 OnMessage() 只被一个 worker 线程调用（mailbox 串行）
 *   2. PlayerActorB 的 OnMessage() 只被一个 worker 线程调用
 *   3. BattleActor 的 OnMessage() 也只被一个 worker 线程调用
 *   4. 三者可能在不同线程上，但各自的状态互不干扰
 *   5. 共享的战斗状态（HP）归 BattleActor 所有，通过消息传递同步
 *
 * 对比传统多线程：
 *   传统方式：PlayerA.Attack(PlayerB) → 需要同时锁 A 和 B → 死锁风险
 *   Actor方式：PlayerA → SendToActor(BattleActor, "attack") → 无锁，消息队列保证顺序
 *
 * 编译: make -f Makefile.battle
 * 运行: ./run_test_battle.sh
 */

#include "precompiled.h"
#include "Actor.h"
#include "ActorSystem.h"
#include "EventLoop.h"
#include "Message.h"

#include <cassert>
#include <chrono>
#include <sstream>

using namespace bllsll;

// ============================================================
//  消息协议（用字符串模拟，实际项目用 protobuf/结构体）
//
//  "start_battle:playerA_id:playerB_id"  -> 开始战斗
//  "attack:attacker_id:skill_name"       -> 攻击指令
//  "damage:target_id:amount:remainHP"    -> 伤害结果通知
//  "battle_end:winner_id"                -> 战斗结束
//  "you_win" / "you_lose"                -> 个人结果通知
// ============================================================

// 辅助：解析 ':' 分隔的字符串
static std::vector<std::string> Split(const std::string& s, char delim)
{
    std::vector<std::string> tokens;
    std::istringstream iss(s);
    std::string token;
    while (std::getline(iss, token, delim)) {
        tokens.push_back(token);
    }
    return tokens;
}

// ============================================================
//  BattleActor: 战斗中介（每场战斗一个）
//
//  拥有战斗状态，串行处理所有战斗消息
//  即使两个玩家在不同线程同时发出攻击，BattleActor 的 mailbox
//  会将这些消息排队，OnMessage() 逐条处理，无并发问题
// ============================================================
class BattleActor : public Actor
{
public:
    // 战斗状态（仅 BattleActor 拥有，线程安全）
    struct FighterInfo {
        uint32_t actorId = 0;
        std::string name;
        int hp = 100;
        int atk = 15;
    };

    FighterInfo fighterA_;
    FighterInfo fighterB_;
    bool battleActive_ = false;
    int turnCount_ = 0;

    // 用于测试验证
    std::atomic<bool> battleStarted{false};
    std::atomic<bool> battleEnded{false};
    std::atomic<int> totalAttacks{0};
    std::string winner;

    ActorTask OnCoroutineMessage(ActorMessage msg) override
    {
        if (msg.type != MsgType::UserMessage) co_return;

        auto parts = Split(msg.data, ':');
        if (parts.empty()) co_return;

        if (parts[0] == "start_battle" && parts.size() >= 5) {
            // "start_battle:nameA:idA:nameB:idB"
            fighterA_.name = parts[1];
            fighterA_.actorId = std::stoul(parts[2]);
            fighterA_.hp = 100;
            fighterA_.atk = 15;
            fighterB_.name = parts[3];
            fighterB_.actorId = std::stoul(parts[4]);
            fighterB_.hp = 100;
            fighterB_.atk = 20;
            battleActive_ = true;
            battleStarted.store(true);
            turnCount_ = 0;

            std::cout << "[BattleActor] === BATTLE START ===" << std::endl;
            std::cout << "[BattleActor] " << fighterA_.name << "(HP=" << fighterA_.hp
                      << ",ATK=" << fighterA_.atk << ") VS "
                      << fighterB_.name << "(HP=" << fighterB_.hp
                      << ",ATK=" << fighterB_.atk << ")" << std::endl;

            // 通知双方战斗开始
            SendToActor(fighterA_.actorId,
                ActorMessage{MsgType::UserMessage, 0, -1,
                    "battle_started:" + fighterB_.name});
            SendToActor(fighterB_.actorId,
                ActorMessage{MsgType::UserMessage, 0, -1,
                    "battle_started:" + fighterA_.name});

        } else if (parts[0] == "attack" && parts.size() >= 3) {
            // "attack:attacker_name:skill_name"
            if (!battleActive_) {
                std::cout << "[BattleActor] battle not active, ignoring attack" << std::endl;
                co_return;
            }

            totalAttacks.fetch_add(1);
            turnCount_++;
            std::string attackerName = parts[1];
            std::string skillName = parts[2];

            // 确定攻击者和防御者
            FighterInfo* attacker = nullptr;
            FighterInfo* defender = nullptr;
            if (attackerName == fighterA_.name) {
                attacker = &fighterA_;
                defender = &fighterB_;
            } else {
                attacker = &fighterB_;
                defender = &fighterA_;
            }

            // 计算伤害（BattleActor 独占此逻辑，无并发问题）
            int damage = attacker->atk + (turnCount_ % 3 == 0 ? 5 : 0); // 每3回合额外伤害
            defender->hp -= damage;
            if (defender->hp < 0) defender->hp = 0;

            std::cout << "[BattleActor] Turn " << turnCount_ << ": "
                      << attacker->name << " uses [" << skillName << "] -> "
                      << defender->name << " takes " << damage << " damage, HP="
                      << defender->hp << std::endl;

            // 通知双方伤害结果
            std::string dmgMsg = "damage:" + defender->name + ":" +
                                 std::to_string(damage) + ":" +
                                 std::to_string(defender->hp);
            SendToActor(attacker->actorId,
                ActorMessage{MsgType::UserMessage, 0, -1, dmgMsg});
            SendToActor(defender->actorId,
                ActorMessage{MsgType::UserMessage, 0, -1, dmgMsg});

            // 检查战斗是否结束
            if (defender->hp <= 0) {
                battleActive_ = false;
                battleEnded.store(true);
                winner = attacker->name;

                std::cout << "[BattleActor] === BATTLE END === Winner: "
                          << attacker->name << " ===" << std::endl;

                // 通知双方结果
                SendToActor(attacker->actorId,
                    ActorMessage{MsgType::UserMessage, 0, -1, "you_win"});
                SendToActor(defender->actorId,
                    ActorMessage{MsgType::UserMessage, 0, -1, "you_lose"});
            }
        }
        co_return;
    }
};

// ============================================================
//  PlayerActor: 玩家（每人一个 Actor）
//
//  关键：PlayerActor 不直接修改对方的 HP，而是发消息给 BattleActor
//  这样即使两个 PlayerActor 在不同线程上同时调用 SendToActor，
//  BattleActor 的 mailbox 保证串行处理
// ============================================================
class PlayerActor : public Actor
{
public:
    std::string name_;
    uint32_t battleActorId_ = 0;
    int hp_ = 100;  // 本地缓存的 HP（从 BattleActor 的通知中更新）

    // 战斗状态（用于测试验证）
    std::atomic<bool> inBattle{false};
    std::atomic<bool> battleResult{false};  // true=win, false=lose
    std::atomic<bool> gotResult{false};
    std::atomic<int> damageDealt{0};
    std::atomic<int> damageTaken{0};

    PlayerActor(const std::string& name) : name_(name) {}

    ActorTask OnCoroutineMessage(ActorMessage msg) override
    {
        if (msg.type != MsgType::UserMessage) co_return;

        auto parts = Split(msg.data, ':');
        if (parts.empty()) co_return;

        if (parts[0] == "battle_started") {
            inBattle.store(true);
            std::cout << "[" << name_ << "] battle started against " << parts[1] << std::endl;

        } else if (parts[0] == "do_attack") {
            // 收到外部指令，发起攻击（发给 BattleActor）
            if (!inBattle.load()) co_return;
            std::string skill = parts.size() >= 2 ? parts[1] : "normal_attack";
            std::cout << "[" << name_ << "] attacking with [" << skill << "]..." << std::endl;

            // 不直接改对方 HP！而是发消息给 BattleActor
            SendToActor(battleActorId_,
                ActorMessage{MsgType::UserMessage, 0, -1,
                    "attack:" + name_ + ":" + skill});

        } else if (parts[0] == "damage" && parts.size() >= 4) {
            // 收到 BattleActor 的伤害通知
            std::string target = parts[1];
            int amount = std::stoi(parts[2]);
            int remainHP = std::stoi(parts[3]);

            if (target == name_) {
                // 自己被打了
                hp_ = remainHP;
                damageTaken.fetch_add(amount);
                std::cout << "[" << name_ << "] took " << amount
                          << " damage! HP=" << hp_ << std::endl;
            } else {
                // 自己打中对方了
                damageDealt.fetch_add(amount);
                std::cout << "[" << name_ << "] dealt " << amount
                          << " damage! enemy HP=" << remainHP << std::endl;
            }

        } else if (msg.data == "you_win") {
            battleResult.store(true);
            gotResult.store(true);
            inBattle.store(false);
            std::cout << "[" << name_ << "] I WIN!" << std::endl;

        } else if (msg.data == "you_lose") {
            battleResult.store(false);
            gotResult.store(true);
            inBattle.store(false);
            std::cout << "[" << name_ << "] I LOSE..." << std::endl;
        }
        co_return;
    }

    // 发起攻击（通过消息触发，可从任意线程调用）
    void DoAttack(const std::string& skill)
    {
        if (GetSystem()) {
            // 给自己发消息（走 mailbox 串行化，保证线程安全）
            GetSystem()->Send(GetActorId(),
                ActorMessage{MsgType::UserMessage, 0, -1, "do_attack:" + skill});
        }
    }
};

// ============================================================
//  辅助：等待条件
// ============================================================
static bool WaitFor(std::atomic<bool>& flag, int timeoutMs = 5000)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!flag.load() && std::chrono::steady_clock::now() < deadline) {
        usleep(5000);
    }
    return flag.load();
}

// ============================================================
//  测试主函数
// ============================================================
int main()
{
    std::cout << "=================================================" << std::endl;
    std::cout << "  Actor Battle Test: Thread-Safe Combat via Actor" << std::endl;
    std::cout << "=================================================" << std::endl;

    // 启动 ActorSystem（4个工作线程模拟多线程环境）
    // 注意：不需要 EventLoop/网络，纯 Actor 间消息通信
    EventLoop loop;
    loop.Create();
    ActorSystem sys;
    sys.Start(4, &loop);

    // -------------------------------------------------------
    //  1. 创建角色
    // -------------------------------------------------------
    std::cout << "\n[1] Creating actors..." << std::endl;

    auto playerAPtr = std::make_unique<PlayerActor>("Warrior");
    PlayerActor* playerA = playerAPtr.get();
    uint32_t playerAId = sys.RegisterActor(std::move(playerAPtr));

    auto playerBPtr = std::make_unique<PlayerActor>("Mage");
    PlayerActor* playerB = playerBPtr.get();
    uint32_t playerBId = sys.RegisterActor(std::move(playerBPtr));

    auto battlePtr = std::make_unique<BattleActor>();
    BattleActor* battle = battlePtr.get();
    uint32_t battleId = sys.RegisterActor(std::move(battlePtr));

    // 设置战斗 Actor ID
    playerA->battleActorId_ = battleId;
    playerB->battleActorId_ = battleId;

    std::cout << "  Warrior(id=" << playerAId << "), Mage(id=" << playerBId
              << "), Battle(id=" << battleId << ")" << std::endl;

    // -------------------------------------------------------
    //  2. 开始战斗
    // -------------------------------------------------------
    std::cout << "\n[2] Starting battle..." << std::endl;

    // 发起战斗请求给 BattleActor
    sys.Send(battleId, ActorMessage{MsgType::UserMessage, 0, -1,
        "start_battle:Warrior:" + std::to_string(playerAId) +
        ":Mage:" + std::to_string(playerBId)});

    if (!WaitFor(battle->battleStarted, 3000)) {
        std::cout << "[FAIL] Battle did not start!" << std::endl;
        sys.Stop();
        return 1;
    }
    usleep(50000);

    // -------------------------------------------------------
    //  3. 回合制战斗（双方交替攻击）
    //     即使这些 DoAttack 调用可能在主线程并发执行，
    //     BattleActor 的 mailbox 保证串行处理，无数据竞争
    // -------------------------------------------------------
    std::cout << "\n[3] Combat rounds..." << std::endl;
    std::cout << "    (Both players attack from main thread," << std::endl;
    std::cout << "     BattleActor processes serially via mailbox)" << std::endl;

    int round = 0;
    while (!battle->battleEnded.load() && round < 20) {
        round++;

        // Warrior 攻击（ATK=15）
        playerA->DoAttack("slash");
        usleep(50000);

        if (battle->battleEnded.load()) break;

        // Mage 攻击（ATK=20）
        playerB->DoAttack("fireball");
        usleep(50000);
    }

    // 等待战斗结束
    if (!WaitFor(battle->battleEnded, 5000)) {
        std::cout << "[FAIL] Battle did not end!" << std::endl;
        sys.Stop();
        return 1;
    }

    // 等待双方收到结果
    WaitFor(playerA->gotResult, 3000);
    WaitFor(playerB->gotResult, 3000);

    usleep(100000);

    // -------------------------------------------------------
    //  4. 验证结果
    // -------------------------------------------------------
    std::cout << "\n[4] Verifying results..." << std::endl;

    bool ok = true;

    // 战斗应该已结束
    if (!battle->battleEnded.load()) {
        std::cout << "[FAIL] Battle not ended!" << std::endl;
        ok = false;
    }

    // 应该有攻击发生
    int attacks = battle->totalAttacks.load();
    std::cout << "  Total attacks: " << attacks << std::endl;
    if (attacks < 2) {
        std::cout << "[FAIL] Too few attacks: " << attacks << std::endl;
        ok = false;
    }

    // 双方都应该收到了结果
    if (!playerA->gotResult.load()) {
        std::cout << "[FAIL] PlayerA did not get result!" << std::endl;
        ok = false;
    }
    if (!playerB->gotResult.load()) {
        std::cout << "[FAIL] PlayerB did not get result!" << std::endl;
        ok = false;
    }

    // 应该有一个赢家一个输家
    bool aWin = playerA->battleResult.load();
    bool bWin = playerB->battleResult.load();
    if (aWin == bWin) {
        std::cout << "[FAIL] Both players have same result!" << std::endl;
        ok = false;
    }

    // 验证伤害数据一致性（BattleActor 串行处理保证的）
    int aDamageDealt = playerA->damageDealt.load();
    int aDamageTaken = playerA->damageTaken.load();
    int bDamageDealt = playerB->damageDealt.load();
    int bDamageTaken = playerB->damageTaken.load();

    std::cout << "  Warrior: dealt=" << aDamageDealt << ", taken=" << aDamageTaken
              << ", result=" << (aWin ? "WIN" : "LOSE") << std::endl;
    std::cout << "  Mage:    dealt=" << bDamageDealt << ", taken=" << bDamageTaken
              << ", result=" << (bWin ? "WIN" : "LOSE") << std::endl;
    std::cout << "  Winner:  " << battle->winner << std::endl;

    // Warrior 造成的伤害 = Mage 承受的伤害
    if (aDamageDealt != bDamageTaken) {
        std::cout << "[FAIL] Damage mismatch: Warrior dealt " << aDamageDealt
                  << " but Mage took " << bDamageTaken << std::endl;
        ok = false;
    }
    // Mage 造成的伤害 = Warrior 承受的伤害
    if (bDamageDealt != aDamageTaken) {
        std::cout << "[FAIL] Damage mismatch: Mage dealt " << bDamageDealt
                  << " but Warrior took " << aDamageTaken << std::endl;
        ok = false;
    }

    // 输家的 HP 应该 <= 0
    if (aWin && playerB->hp_ > 0) {
        std::cout << "[FAIL] Loser Mage HP=" << playerB->hp_ << " > 0!" << std::endl;
        ok = false;
    }
    if (bWin && playerA->hp_ > 0) {
        std::cout << "[FAIL] Loser Warrior HP=" << playerA->hp_ << " > 0!" << std::endl;
        ok = false;
    }

    // -------------------------------------------------------
    //  5. 清理
    // -------------------------------------------------------
    sys.Stop();

    // -------------------------------------------------------
    //  6. 输出结果
    // -------------------------------------------------------
    std::cout << "\n=================================================" << std::endl;
    if (ok) {
        std::cout << "  [PASS] Actor Battle Test passed!" << std::endl;
        std::cout << std::endl;
        std::cout << "  Key design points (no multithreading issues):" << std::endl;
        std::cout << "  1. PlayerActor does NOT directly modify other's HP" << std::endl;
        std::cout << "  2. All combat logic runs in BattleActor (serial mailbox)" << std::endl;
        std::cout << "  3. Damage data is consistent (A.dealt == B.taken)" << std::endl;
        std::cout << "  4. No locks, no mutex, no race conditions" << std::endl;
        std::cout << std::endl;
        std::cout << "  Traditional OOP (unsafe):  PlayerA.Attack(PlayerB)" << std::endl;
        std::cout << "    -> need to lock both A and B -> deadlock risk" << std::endl;
        std::cout << std::endl;
        std::cout << "  Actor model (safe):  PlayerA -> BattleActor -> PlayerB" << std::endl;
        std::cout << "    -> messages serialize access -> no locks needed" << std::endl;
    } else {
        std::cout << "  [FAIL] Some checks failed!" << std::endl;
    }
    std::cout << "=================================================" << std::endl;

    return ok ? 0 : 1;
}
