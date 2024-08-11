#include "TcpClient.h"
#include "Connector.h"

using namespace sll;
using namespace std;

void TcpClient::Start()
{
    loop_.Create(8);
    int port = 9527;
    std::string strIp = "127.0.0.1";
    conn_.reset(new Connector(&loop_, port, strIp));
    //连接成功
    conn_->OnConnected([this](IConnection* pConn){
        std::cout << "connect suc!" << std::endl;
    });
    //发送消息
    std::string msg = "hellp world!";
    conn_->Send(msg.c_str(), msg.length());
    //收到消息
    conn_->OnRecv([this](const char* pData, int nLen) {
        std::cout << "client recv msg: " << pData << std::endl;
    });
    std::cout << "TcpClient::Start suc!" << std::endl;
}