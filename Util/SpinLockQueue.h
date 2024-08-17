
#pragma once
/**
@auther: chencaiyu
@date: 2024.8.17
@brief: 自旋锁队列
*/

#include <queue>
#include <optional>
#include "SpinLock.h"

namespace sll {

template <class T>
class SpinLockQueue
{
public:
    SpinLockQueue() = default;
    ~SpinLockQueue() = default;

    void push(const T& value) {
        lock.lock();
        queue.push(value);
        lock.unlock();
    }

    std::optional<T> pop() {
        lock.lock();
        if (queue.empty()) {
            lock.unlock();
            return std::nullopt;
        }
        T value = queue.front();
        queue.pop();
        lock.unlock();
        return value;
    }

    bool empty() const {
        lock.lock();
        bool isEmpty = queue.empty();
        lock.unlock();
        return isEmpty;
    }

    std::size_t size() const {
        lock.lock();
        std::size_t size = queue.size();
        lock.unlock();
        return size;
    }

private:
    mutable sll::SpinLock lock; // 使用自旋锁保护队列访问
    std::queue<T> queue;
};

} //namespace sll
