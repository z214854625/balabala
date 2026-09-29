#include "TcpServer.h"
#include "Acceptor.h"
#include "Message.h"

using namespace bllsll;
using namespace std;

ActorTask EchoServerActor::OnCoroutineMessage(ActorMessage msg)
{
    switch (msg.type) {
    case MsgType::Connected:
        std::cout << "[ServerActor] new client connected! fd=" << msg.fd << std::endl;
        break;
    case MsgType::NetworkRecv:
        std::cout << "[ServerActor] recv data! fd=" << msg.fd
                  << ", len=" << msg.Size() << std::endl;
        // Echo: 原样回传给客户端
        // [P0-4 修复] 使用 Data()/Size() 统一访问，支持大包（sharedBuf）和小包（data）
        SendToNetwork(msg.fd, msg.Data(), static_cast<int>(msg.Size()));
        break;
    case MsgType::Disconnected:
        std::cout << "[ServerActor] client disconnected! fd=" << msg.fd << std::endl;
        break;
    default:
        break;
    }
    co_return;
}

void TcpServer::Start(int port, int subReactorNum, int workerCount)
{
    std::cout << "TcpServer::Start (Actor Model)"
              << ", subReactorNum=" << subReactorNum
              << ", workerCount=" << workerCount << std::endl;

    // 1. 启动主 EventLoop（epoll I/O 线程，跑 Acceptor）
    loop_.Create();

    // 2. [2026.5 多 Reactor] 可选启动 SubReactor 线程池
    //    subReactorNum>0 时启用主从模式，否则保持单 Reactor 行为
    if (subReactorNum > 0) {
        subReactorPool_.SetThreadNum(subReactorNum);
        subReactorPool_.Start(&actorSystem_);
    }

    // 3. 启动 ActorSystem
    actorSystem_.Start(workerCount, &loop_);
    loop_.SetActorSystem(&actorSystem_);

    // 4. 创建服务端 Actor（处理所有客户端事件）
    serverActorId_ = actorSystem_.RegisterActor(std::make_unique<EchoServerActor>());

    // 5. 启动 Acceptor 监听
    // [P0-2 修复] acceptor_ 改为 unique_ptr 管理
    acceptor_ = std::make_unique<Acceptor>(port, &loop_, serverActorId_);
    // 设置 SubReactor 池（pool 为空时 Acceptor 自动回退到单 Reactor 行为）
    if (subReactorPool_.Enabled()) {
        acceptor_->SetThreadPool(&subReactorPool_);
    }

    // 6. 主线程保活，直到 Stop() 被调用
    // [P1-8 修复] 用 running_ 标志替代 while(true)，支持优雅关停
    running_.store(true);
    while (running_.load()) {
        usleep(100000);
    }
}

void TcpServer::Stop()
{
    // [P1-8 修复] 优雅关停顺序：
    //   1. 先停 EventLoop（停止 accept / I/O，排空 pending tasks）
    //   2. 停 SubReactor 池
    //   3. 停 ActorSystem（停止 worker，排空邮箱）
    //   4. 释放 acceptor_（在 loop 停止后安全析构）
    // 这样保证 HandleRead 不再访问已析构的 ActorSystem
    std::cout << "TcpServer::Stop begin" << std::endl;
    running_.store(false);
    loop_.Stop();
    if (subReactorPool_.Enabled()) {
        subReactorPool_.Stop();
    }
    actorSystem_.Stop();
    acceptor_.reset();
    std::cout << "TcpServer::Stop done" << std::endl;
}
