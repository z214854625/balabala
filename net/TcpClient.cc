#include "TcpClient.h"
#include "Connector.h"

using namespace bllsll;
using namespace std;

void TcpClient::Start(int port, const std::string strIp)
{
    std::cout << "TcpClient::Start 1--" << std::endl;
    loop_.Create(4);
    conn_.reset(new Connector(&loop_, port, strIp));
    //连接成功
    conn_->OnConnected([&](IConnection* pSvrConn){
        std::cout << "connect suc!" << std::endl;
        //收到消息
        conn_->OnRecv([&](IConnection* pConn, const char* pData, int nLen) {
            std::cout << "cli recv msg! len= " << nLen << ", data= " << "" << ", fd=" << pConn->GetFd() << std::endl;
            pConn->Send(pData, nLen);
        });
        //断开连接
        conn_->OnDisconnected([&](IConnection* pConn) {
            std::cout << "server disconnected! fd=" << pConn->GetFd() << std::endl;
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
        conn_->Send(msg.c_str(), msg.length());
        std::cout << "send msg: " << msg << std::endl;
    }
    t1.join();
}