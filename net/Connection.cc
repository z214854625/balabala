#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include "Connection.h"
#include "EventLoop.h"
#include "Poller.h"

using namespace bllsll;
using namespace std;

Connection::Connection(int fd, EventLoop* loop) : ConnectionBase(loop)
{
    socket_ = fd;
}

Connection::~Connection()
{
    if (socket_ != -1) {
        close(socket_);
    }
}

void Connection::OnRecv(RecvCallback&& callback)
{
    ConnectionBase::OnRecv(std::forward<RecvCallback>(callback));
}

void Connection::Send(const char* pData, int nLen)
{
    ConnectionBase::Send(pData, nLen);
}

void Connection::HandleRead(int fd, uint32_t events)
{
    ConnectionBase::HandleRead(fd, events);
}

void Connection::HandleWrite(int fd, uint32_t events)
{
    ConnectionBase::HandleWrite(fd, events);
}

void Connection::OnDisconnected(DisConnCallback&& callback)
{
    ConnectionBase::OnDisconnected(std::forward<DisConnCallback>(callback));
}
