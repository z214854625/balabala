#pragma once

/**
@auther: chencaiyu
@date: 2024.8.1
@brief: 连接对象接口（Actor模型版本）
       移除了回调类型定义，改用Actor消息机制

业务层统一使用 Send(char*, int)，框架内部根据大小自动优化
*/

#include <cstdint>

namespace bllsll {

class IConnection
{
public:
    virtual ~IConnection(){}
    //发送消息
    virtual void Send(const char* pData, int nLen) = 0;
    //读事件处理
    virtual void HandleRead(int fd, uint32_t events) = 0;
    //写事件处理
    virtual void HandleWrite(int fd, uint32_t events) = 0;
    //accept事件处理
    virtual void HandleAccept(int listenFd, uint32_t events) = 0;
    //getfd
    virtual int GetFd() = 0;
};

} //namespace bllsll
