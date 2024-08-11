#pragma once

/**
@auther: chencaiyu
@date: 2024.8.1
@brief: 连接对象基类
*/

#include <functional>
#include <string>

namespace sll {

class IConnection;

using RecvCallback = std::function<void(const char* pData, int nLen)>;
using ConnCallback = std::function<void(IConnection*)>;

class IConnection
{
public:
    virtual ~IConnection(){}
    //收到消息
    virtual void OnRecv(RecvCallback&& callback) = 0;
    //发送消息
    virtual void Send(const char* pData, int nLen) = 0;
    //客户端连接成功回调
    virtual void OnConnected(ConnCallback&& callback) = 0;
    //读事件处理
    virtual void HandleRead(int fd, uint32_t events) = 0;
    //写事件处理
    virtual void HandleWrite(int fd, uint32_t events) = 0;

};

} //namespace sll