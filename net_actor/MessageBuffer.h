#pragma once
/**
@auther: AI
@date: 2026.5
@brief: 引用计数共享缓冲（大包零拷贝路径）

  用途：
    - 大包跨 Actor 转发时避免反复 memcpy
    - shared_ptr<MessageBuffer> 在 ActorMessage 之间传递，仅原子计数 +1/-1
    - 与 std::string 互补：小包仍走 std::string（SSO 零分配），
      大包走 MessageBuffer（共享）

  设计要点：
    1. 不可变（immutable）：构造后内容只读，避免写时复制的复杂度
    2. 只在构造时分配一次，析构时释放
    3. 通过 shared_ptr 管理生命周期，多 Actor 安全共享

使用示例：
    // 网络层从 socket 读到大包数据时：
    auto buf = std::make_shared<MessageBuffer>(recvBuffer, n);
    ActorMessage msg(MsgType::NetworkRecv, 0, fd);
    msg.sharedBuf = buf;
    sys->Send(actorId, std::move(msg));

    // 业务 Actor 读取：
    const char* p = msg.Data();
    size_t len    = msg.Size();
*/

#include <cstddef>
#include <cstring>
#include <memory>
#include <string>

namespace bllsll {

class MessageBuffer
{
public:
    // 从原始字节构造（一次性 malloc + memcpy）
    MessageBuffer(const char* data, size_t len)
        : size_(len), data_(new char[len])
    {
        if (len > 0 && data != nullptr) {
            std::memcpy(data_, data, len);
        }
    }

    // 从 string 构造（同上，但走 std::string 内容）
    explicit MessageBuffer(const std::string& s)
        : MessageBuffer(s.data(), s.size()) {}

    // 不允许拷贝（防止误用）
    MessageBuffer(const MessageBuffer&) = delete;
    MessageBuffer& operator=(const MessageBuffer&) = delete;

    ~MessageBuffer() {
        delete[] data_;
    }

    const char* Data() const { return data_; }
    size_t Size() const { return size_; }
    bool Empty() const { return size_ == 0; }

    // 转 string（拷贝，仅供需要 string 接口的旧代码用）
    std::string ToString() const { return std::string(data_, size_); }

private:
    size_t size_ = 0;
    char* data_ = nullptr;
};

using MessageBufferPtr = std::shared_ptr<MessageBuffer>;

// 工厂函数
inline MessageBufferPtr MakeBuffer(const char* data, size_t len) {
    return std::make_shared<MessageBuffer>(data, len);
}

inline MessageBufferPtr MakeBuffer(const std::string& s) {
    return std::make_shared<MessageBuffer>(s);
}

} // namespace bllsll
