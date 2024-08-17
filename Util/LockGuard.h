#include <atomic>

#pragma once
/**
@auther: chencaiyu
@date: 2024.8.17
@brief: LockGuard 实现
*/

namespace sll {

template <typename Lock>
class LockGuard {
public:
    explicit LockGuard(Lock& lock) : lock_(lock) {
        lock_.lock();
    }

    ~LockGuard() {
        lock_.unlock();
    }

    // 禁用拷贝构造和拷贝赋值操作
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    Lock& lock_;
};


} //namespace sll