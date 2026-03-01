#include "TcpClient.h"
#include "Connector.h"
#include "Message.h"

using namespace bllsll;
using namespace std;

void EchoClientActor::OnMessage(ActorMessage& msg)
{
    switch (msg.type) {
    case MsgType::Connected:
        std::cout << "[ClientActor] connected to server! fd=" << msg.fd << std::endl;
        break;
    case MsgType::NetworkRecv: {
        std::string data(msg.data);
        std::cout << "[ClientActor] recv from server! fd=" << msg.fd
                  << ", len=" << data.size() << ", data=" << data << std::endl;
        break;
    }
    case MsgType::Disconnected:
        std::cout << "[ClientActor] server disconnected! fd=" << msg.fd << std::endl;
        break;
    default:
        break;
    }
}

void TcpClient::Start(int port, const std::string strIp)
{
    std::cout << "TcpClient::Start (Actor Model)" << std::endl;
    // 1. 启动EventLoop（epoll I/O线程）
    loop_.Create();
    // 2. 启动ActorSystem（2个工作线程处理Actor消息）
    actorSystem_.Start(2, &loop_);
    loop_.SetActorSystem(&actorSystem_);
    // 3. 创建客户端Actor
    clientActorId_ = actorSystem_.RegisterActor(std::make_unique<EchoClientActor>());
    // 4. 创建Connector连接服务器（构造时即发起连接）
    conn_ = new Connector(&loop_, port, strIp, clientActorId_);
    // 5. 主线程读取stdin发送消息
    while (true) {
        std::cout << "enter msg: " << std::endl;
        std::string msg;
        std::getline(std::cin, msg);
        if (conn_ == nullptr || conn_->GetFd() < 0) {
            std::cout << "connection lost!" << std::endl;
            break;
        }
        std::cout << "send msg: " << msg << std::endl;
        conn_->Send(msg.c_str(), msg.length());
    }
}
