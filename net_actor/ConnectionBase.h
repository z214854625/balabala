#pragma once
/**
@auther: chencaiyu
@date: 2024.8.1
@brief: 连接对象基类，处理epoll读写事件（Actor模型版本）
       读到数据后通过ActorSystem路由给绑定的Actor

[2026.5 改造] 写路径用 Buffer 替代 sendMQ_<string> + lastMsgCache_
  - 数据连续存储，减少 malloc/拷贝
  - HandleWrite 写出可一次大块 write，发不完时仅移指针

[2026.5] 大包跨线程优化：Send(char*, int) 内部根据 nLen 自动选择：
  - 小包 → std::string 路径（SSO 友好）
  - 大包 → MessageBuffer 共享路径（避免 std::string 大包构造）
  业务层无感知，统一只用 Send(char*, int)
*/

#include "IConnection.h"
#include "Buffer.h"
#include "MessageBuffer.h"

namespace bllsll {

class EventLoop;

class ConnectionBase : public IConnection
{
public:
    ConnectionBase(EventLoop* loop);
    ~ConnectionBase();

    //发送消息（统一接口）
    virtual void Send(const char* pData, int nLen);
    //读事件处理
    virtual void HandleRead(int fd, uint32_t events);
    //写事件处理
    virtual void HandleWrite(int fd, uint32_t events);
    //accept事件处理
    virtual void HandleAccept(int listenFd, uint32_t events) {}
    //getfd
    virtual int GetFd() { return 0; }
    // [Send fast-path] 是否处于可直接 write 的状态
    // 基类默认 true；Connector 在 connecting_ 期间返回 false（连接还没建立完）
    virtual bool CanWriteDirectly() const { return true; }

protected:
    int socket_;
    int state_;
    // 发送缓冲区（仅 loop 线程访问；Send 通过 RunInLoop 投递到 loop 线程后再 Append）
    Buffer outputBuffer_;
    EventLoop* loop_;

private:
    // [2026.5] 私有辅助：在 loop 线程内尝试直发，发不完进 outputBuffer_。
    // 三条 Send 路径（fast-path、slow-path 大包、slow-path 小包）共用此实现，
    // 避免 write 循环 + 兜底逻辑重复。
    // 必须在 loop 线程内调用。
    void sendInLoop(int fd, const char* p, size_t len);
};

} //namespace bllsll
