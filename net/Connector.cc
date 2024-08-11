#include "Connector.h"
#include "EventLoop.h"

using namespace sll;
using namespace std;

Connector::Connector(EventLoop* loop, int port, const std::string& strIP)
{
    loop_ = loop;
    //do connect
}

Connector::~Connector()
{
}

void Connector::OnRecv(RecvCallback&& callback)
{
    recvCallback_ = std::forward<RecvCallback>(callback);
}

void Connector::Send(const char* pData, int nLen)
{
    sendMQ_.push(std::string(pData, nLen));
}

void Connector::OnConnected(ConnCallback&& callback)
{
    connCallback_ = std::forward<ConnCallback>(callback);
}