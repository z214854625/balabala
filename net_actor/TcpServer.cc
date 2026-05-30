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
                  << ", len=" << msg.data.size() << std::endl;
        // Echo: 原样回传给客户端
        SendToNetwork(msg.fd, msg.data.c_str(), msg.data.size());
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
    acceptor_ = new Acceptor(port, &loop_, serverActorId_);
    // 设置 SubReactor 池（pool 为空时 Acceptor 自动回退到单 Reactor 行为）
    if (subReactorPool_.Enabled()) {
        acceptor_->SetThreadPool(&subReactorPool_);
    }

    // 6. 主线程保持运行（Actor worker 线程处理业务逻辑）
    while (true) {
        usleep(100000);
    }
}
