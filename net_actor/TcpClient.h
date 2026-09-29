#pragma once
/**
@auther: chencaiyu
@date: 2024.8.11
@brief: tcp客户端（Actor模型版本）
*/

#include "precompiled.h"
#include "EventLoop.h"
#include "ActorSystem.h"
#include "Actor.h"
#include <atomic>
#include <memory>

namespace bllsll {

class Connector;

// 客户端Actor：处理与服务器的通信事件
class EchoClientActor : public Actor
{
public:
    ActorTask OnCoroutineMessage(ActorMessage msg) override;
};

class TcpClient
{
public:
    void Start(int port, const std::string strIp);
    // [P1-8 修复] 优雅关停：先停 I/O（loop）→ 再停 ActorSystem → 释放 connector_
    void Stop();

private:
    // [P1-8 修复] 成员声明顺序：actorSystem_ 先声明 → 后析构（保证 I/O 先停，ActorSystem 后停）
    // 原顺序 loop_ 在前、actorSystem_ 在后，导致 actorSystem_ 先析构时 loop_ 仍在跑 → UAF
    ActorSystem actorSystem_;
    EventLoop loop_;
    std::unique_ptr<Connector> connector_;   // [P0-2 修复] 改为 unique_ptr 管理
    uint32_t clientActorId_ = 0;
    std::atomic<bool> running_{false};       // [P1-8] 主循环保活标志
};

} // namespace bllsll
