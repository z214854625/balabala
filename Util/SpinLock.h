#include <atomic>

#pragma once
/**
@auther: chencaiyu
@date: 2024.8.17
@brief: 自旋锁
*/

namespace sll {

class SpinLock
{
public:
    SpinLock() : flag(ATOMIC_FLAG_INIT) {}

    void lock() {
        // busy-wait loop (spin)
        while (flag.test_and_set(std::memory_order_acquire)) {
            // Optionally, add a pause instruction to prevent excess CPU usage
            // on hyper-threaded CPUs. Uncomment the next line on x86/64:
            // _mm_pause();
            // std::this_thread::yield(); // Alternatively, yield the thread
        }
    }

    void unlock() {
        flag.clear(std::memory_order_release);
    }

private:
    std::atomic_flag flag;
};

} //namespace sll