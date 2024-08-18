#pragma once
/**
@auther: chencaiyu
@date: 2024.8.1
@brief: I/O事件循环处理
*/

#include "precompiled.h"
#include "../Util/SpinLockQueue.h"
#include "IConnection.h"

namespace sll {

class Poller;

class EventLoop
{
public:
    using Functor = std::function<void()>;
    using Callback = std::function<void(int, uint32_t)>;
    using recvMsgType = std::tuple<int, std::string, RecvCallback>;

    EventLoop();
    ~EventLoop();
    //创建
    void Create(int);
    //事件循环
    void run(int timeout = -1);
    //线程函数
    void WorkerThread();
    //添加io事件
    void AddEvent(int fd, uint32_t events, Callback&& cb);
    //修改io事件
    void ModifyEvent(int fd, uint32_t events, Callback&& cb);
    //删除io事件
    void RemoveEvent(int fd);
    //获取poller对象
    Poller* GetPoller() { return poller_.get(); };
    //派发任务
    void OnDispatch(int timeout = 0);
    //获取系统毫秒
    int64_t GetMilliSeconds();
    //加入消息队列
    void AddMsg(recvMsgType&& p);
    //加入连接对象列表
    void AddConnection(sll::IConnection* pConn);
    //获取连接对象列表
    sll::IConnection* GetConnection(int fd);
    //删除连接对象
    void RemoveConnection(int fd);
private:
    std::unique_ptr<Poller> poller_;
    bool stop_;
    std::vector<std::thread> threadPool_;
    sll::SpinLockQueue<std::function<void()>> taskQueue_;
    sll::SpinLockQueue<recvMsgType> msgQueue_;
    sll::SpinLock spinLock_;
    std::unordered_map<int, Callback> callbacks_;
    std::unordered_map<int, sll::IConnection*> mapConn_; //连接对象
};

} //namespace sll