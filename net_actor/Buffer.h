#pragma once
/**
@auther: AI重写
@date: 2026.5
@brief: 网络发送缓冲区（muduo 风格双游标设计的Buffer）

  内存布局：
    +---------+----------------+----------------+
    | prepend |    readable    |    writable    |
    +---------+----------------+----------------+
    0    readerIndex_     writerIndex_      capacity()

  - prepend 区（0 ~ readerIndex_）：已写出/已发送的空间，可被回收做扩容缓冲
  - readable 区（readerIndex_ ~ writerIndex_）：等待发送的数据
  - writable 区（writerIndex_ ~ capacity()）：剩余可写空间

  关键操作：
    Append(data, len)  : 往 writable 区追加数据（不够时压缩或扩容）
    Peek() / ReadablePtr() : 取出 readable 区起始指针（HandleWrite 直接 write 这块）
    Retrieve(len)      : HandleWrite 写出 len 字节后调用，仅移动 readerIndex_，零拷贝
    RetrieveAll()      : 全部写完后复位（readerIndex_ = writerIndex_ = kPrepend）

  对比原 sendMQ_<string> + lastMsgCache_:
    - 原方案：每次 Send 一次 malloc(string)，HandleWrite 弹一条发一条，发不完缓存
    - 新方案：所有数据连续存放，HandleWrite 可一次 write 大块，发部分只移指针
*/

#include <vector>
#include <cstring>
#include <cassert>
#include <cstddef>
#include <algorithm>

namespace bllsll {

class Buffer
{
public:
    static const size_t kCheapPrepend = 8;     // 预留头部空间（业务可能要前置加协议头）
    static const size_t kInitialSize = 64 * 1024;  // 初始 64KB

    explicit Buffer(size_t initialSize = kInitialSize)
        : buffer_(kCheapPrepend + initialSize),
          readerIndex_(kCheapPrepend),
          writerIndex_(kCheapPrepend)
    {
    }

    // 已写入待发送字节数
    size_t ReadableBytes() const { return writerIndex_ - readerIndex_; }
    // 可写空间字节数
    size_t WritableBytes() const { return buffer_.size() - writerIndex_; }
    // 头部空闲字节数
    size_t PrependableBytes() const { return readerIndex_; }

    // 取出 readable 区起始指针（仅读，不修改游标）
    const char* Peek() const { return Begin() + readerIndex_; }
    char* BeginWrite() { return Begin() + writerIndex_; }
    const char* BeginWrite() const { return Begin() + writerIndex_; }

    // 写入数据：不够时先压缩 prepend 区，仍不够则扩容
    void Append(const char* data, size_t len)
    {
        if (len == 0) return;
        EnsureWritableBytes(len);
        std::memcpy(BeginWrite(), data, len);
        writerIndex_ += len;
    }

    void Append(const std::string& s) { Append(s.data(), s.size()); }

    // 标记已读出 len 字节
    void Retrieve(size_t len)
    {
        assert(len <= ReadableBytes());
        if (len < ReadableBytes()) {
            readerIndex_ += len;
        } else {
            RetrieveAll();
        }
    }

    // 全部读完，复位游标
    void RetrieveAll()
    {
        readerIndex_ = kCheapPrepend;
        writerIndex_ = kCheapPrepend;
    }

    bool Empty() const { return readerIndex_ == writerIndex_; }

    // 确保 writable 区至少能容纳 len 字节
    void EnsureWritableBytes(size_t len)
    {
        if (WritableBytes() < len) {
            MakeSpace(len);
        }
        assert(WritableBytes() >= len);
    }

private:
    char* Begin() { return buffer_.data(); }
    const char* Begin() const { return buffer_.data(); }

    // 腾出 len 字节的 writable 空间
    // 优先压缩 prepend 区（把 readable 数据移到 kCheapPrepend 开头），
    // 仍不够则扩容
    void MakeSpace(size_t len)
    {
        if (WritableBytes() + PrependableBytes() < len + kCheapPrepend) {
            // 总空间不够 → 扩容
            buffer_.resize(writerIndex_ + len);
        } else {
            // 总空间够，但需要把 readable 区前移以释放 prepend 中的空闲
            assert(kCheapPrepend < readerIndex_);
            size_t readable = ReadableBytes();
            std::memmove(Begin() + kCheapPrepend, Begin() + readerIndex_, readable);
            readerIndex_ = kCheapPrepend;
            writerIndex_ = readerIndex_ + readable;
            assert(readable == ReadableBytes());
        }
    }

    std::vector<char> buffer_;
    size_t readerIndex_;
    size_t writerIndex_;
};

} // namespace bllsll
