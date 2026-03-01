#include "TcpServer.h"
#include "Acceptor.h"
#include "Message.h"

using namespace bllsll;
using namespace std;

void EchoServerActor::OnMessage(ActorMessage& msg)
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
}

void TcpServer::Start(int port)
{
    std::cout << "TcpServer::Start (Actor Model)" << std::endl;
    // 1. 启动EventLoop（epoll I/O线程）
    loop_.Create();
    // 2. 启动ActorSystem（4个工作线程处理Actor消息）
    actorSystem_.Start(4, &loop_);
    loop_.SetActorSystem(&actorSystem_);
    // 3. 创建服务端Actor（处理所有客户端事件）
    serverActorId_ = actorSystem_.RegisterActor(std::make_unique<EchoServerActor>());
    // 4. 启动Acceptor监听，新连接绑定到serverActor
    acceptor_ = new Acceptor(port, &loop_, serverActorId_);
    // 5. 主线程保持运行（Actor工作线程处理业务逻辑）
    while (true) {
        usleep(100000);
    }
}
