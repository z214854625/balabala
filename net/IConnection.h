#pragma once

/**
@auther: chencaiyu
@date: 2024.8.1
@brief: 连接对象基类
*/

#include <functional>
#include <string>

namespace sll {

using RecvCallback = std::function<void(const char* pData, int nLen)>;

class IConnection
{
public:
    virtual ~IConnection(){}
    //收到消息
    virtual void OnRecv(RecvCallback&&) = 0;
    //发送消息
    virtual void Send(const char* pData, int nLen) = 0;
};

} //namespace sll