#include "TcpClient.h"
#include "Connector.h"
#include "Message.h"

using namespace bllsll;
using namespace std;

ActorTask EchoClientActor::OnCoroutineMessage(ActorMessage msg)
{
    switch (msg.type) {
    case MsgType::Connected:
        std::cout << "[ClientActor] connected to server! fd=" << msg.fd << std::endl;
        break;
    case MsgType::NetworkRecv: {
        // [P0-4 修复] 使用 CopyToString() 统一访问，支持大包（sharedBuf）和小包（data）
        std::string data = msg.CopyToString();
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
    co_return;
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
    // [P0-2 修复] 使用 unique_ptr 管理 Connector 生命周期
    connector_ = std::make_unique<Connector>(&loop_, port, strIp, clientActorId_);
    // 5. 主线程读取stdin发送消息
    // [P1-8 修复] 用 running_ 标志替代 while(true)，支持优雅关停
    running_.store(true);
    while (running_.load()) {
        std::cout << "enter msg: " << std::endl;
        std::string msg;
        std::getline(std::cin, msg);
        if (connector_ == nullptr || connector_->GetFd() < 0) {
            std::cout << "connection lost!" << std::endl;
            break;
        }
        std::cout << "send msg: " << msg << std::endl;
        connector_->Send(msg.c_str(), msg.length());
    }
}

void TcpClient::Stop()
{
    // [P1-8 修复] 优雅关停顺序：
    //   1. 先停 EventLoop（停止 I/O，排空 pending tasks）
    //   2. 停 ActorSystem（停止 worker，排空邮箱）
    //   3. 释放 connector_（在 loop 停止后安全析构）
    std::cout << "TcpClient::Stop begin" << std::endl;
    running_.store(false);
    loop_.Stop();
    actorSystem_.Stop();
    connector_.reset();
    std::cout << "TcpClient::Stop done" << std::endl;
}
