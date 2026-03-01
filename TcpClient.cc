#include "TcpClient.h"
#include "Connector.h"

using namespace bllsll;
using namespace std;

void TcpClient::Start(int port, const std::string strIp)
{
    std::cout << "TcpClient::Start 1--" << std::endl;
    loop_.Create();
    // Connector由EventLoop的mapConn_管理生命周期
    conn_ = new Connector(&loop_, port, strIp);
    //连接成功
    conn_->OnConnected([this](IConnection* pSvrConn){
        std::cout << "connect suc!" << std::endl;
        //收到消息（仅打印，不回传，避免与echo服务端形成无限ping-pong）
        pSvrConn->OnRecv([](IConnection* pConn, const char* pData, int nLen) {
            std::string data(pData, nLen);
            std::cout << "cli recv msg! len= " << nLen << ", data= " << data << ", fd=" << pConn->GetFd() << std::endl;
        });
        //断开连接（RemoveConnection会delete对象，需要将conn_置空防止悬挂指针）
        pSvrConn->OnDisconnected([this](IConnection* pConn) {
            std::cout << "server disconnected! fd=" << pConn->GetFd() << std::endl;
            conn_ = nullptr;
        });
    });
    auto t1 = std::thread([this](){
        while (1) {
            loop_.OnDispatch(500);
            usleep(25000);
        }
    });
    //读取一行信息
    while(1) {
        // 清理缓冲区以准备读取整行输入
        std::cout << "enter msg: " << std::endl;
        std::string msg;
        std::getline(std::cin, msg);
        if (conn_ == nullptr) {
            std::cout << "connection lost!" << std::endl;
            break;
        }
        std::cout << "send msg: " << msg << std::endl;
        conn_->Send(msg.c_str(), msg.length());
    }
    t1.join();
}