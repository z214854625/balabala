#include "TcpServer.h"
#include "Connection.h"

using namespace sll;
using namespace std;

void TcpServer::Start(int port)
{
    std::cout << "TcpServer::Start 1--" << std::endl;
    loop_.Create(4);
    conn_.reset(new Connection(port, &loop_));
    //接收数据
    conn_->OnRecv([this](const char* pData, int nLen) {
        std::cout << "server recv msg: " << pData << std::endl;
        conn_->Send(pData, nLen);
    });
    //断开连接
    conn_->OnDisconnected([this](IConnection* pClientConn) {
        std::cout << "pClientConn disconnected! " << std::endl;
    });
}