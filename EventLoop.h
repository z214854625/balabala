#pragma once
/**
@auther: chencaiyu
@date: 2024.8.1
@brief: I/O事件循环处理
正确的线程模型：
- 1个 epoll loop 线程：负责所有I/O操作（accept/read/write），保证同一fd的I/O串行
- 主线程：通过 OnDispatch() 处理业务逻辑（收到的消息回调）
- Send() 可从任意线程调用，通过 SpinLockQueue + epoll_ctl 保证线程安全
*/

#include "precompiled.h"
#include "../Util/SpinLockQueue.h"
#include "IConnection.h"

namespace bllsll {

class Poller;

class EventLoop
{
public:
    using Functor = std::function<void()>;
    using Callback = std::function<void(int, uint32_t)>;
    using recvMsgType = std::tuple<int, std::string, RecvCallback>;

    EventLoop();
    ~EventLoop();
    //创建（启动epoll loop线程）
    void Create();
    //事件循环（在loop线程中直接执行I/O回调）
    void run(int timeout = -1);
    //添加io事件
    void AddEvent(int fd, uint32_t events, Callback&& cb);
    //修改io事件
    void ModifyEvent(int fd, uint32_t events, Callback&& cb);
    //删除io事件
    void RemoveEvent(int fd);
    //获取poller对象
    Poller* GetPoller() { return poller_.get(); };
    //派发任务（主线程调用，处理收到的消息）
    void OnDispatch(int timeout = 0);
    //获取系统毫秒
    int64_t GetMilliSeconds();
    //加入消息队列
    void AddMsg(recvMsgType&& p);
    //加入连接对象列表
    void AddConnection(bllsll::IConnection* pConn);
    //获取连接对象列表
    bllsll::IConnection* GetConnection(int fd);
    //删除连接对象（同时移除epoll事件和回调）
    void RemoveConnection(int fd);
private:
    std::unique_ptr<Poller> poller_;
    bool stop_;
    std::thread loopThread_;                        //epoll loop线程
    bllsll::SpinLockQueue<recvMsgType> msgQueue_;   //消息队列（loop线程→主线程）
    bllsll::SpinLock cbSpinLock_;                    //保护callbacks_
    std::unordered_map<int, Callback> callbacks_;    //fd→回调映射
    bllsll::SpinLock connSpinLock_;                  //保护mapConn_
    std::unordered_map<int, bllsll::IConnection*> mapConn_; //连接对象
};

} //namespace bllsll