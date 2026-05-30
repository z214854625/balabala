#pragma once
/**
@auther: chencaiyu
@date: 2025.3.1
@brief: Actor消息类型定义

改进记录：
  [P2] A.8 消息类型系统：添加 std::any payload 支持类型安全消息传递
  [P3] A.10 优先级消息：添加 priority 字段支持高优先级消息
  [P3] A.11 跨进程集群：添加 sourceNodeId/sourceActorName 支持跨进程回程路由
  [2026.5] 引用计数共享 buffer：sharedBuf 字段支持大包跨 Actor 零拷贝
*/

#include <string>
#include <cstdint>
#include <cstring>
#include <any>
#include <typeinfo>
#include "MessageBuffer.h"

namespace bllsll {

enum class MsgType : uint32_t {
    None = 0,
    NetworkRecv,      // 收到网络数据
    Connected,        // 新连接建立
    Disconnected,     // 连接断开
    UserMessage,      // 用户自定义消息
    ActorDown,        // 被 Link 的 Actor 退出/注销（data 中含退出原因）
};

struct ActorMessage {
    MsgType type = MsgType::None;
    uint32_t sourceId = 0;     // 来源actor id (0表示系统/网络消息，仅本进程有效)
    int fd = -1;               // 关联的fd
    std::string data;          // 消息数据（字符串格式，向后兼容；小包路径）
    uint32_t sessionId = 0;    // 协程会话ID（用于Call/Response配对）
    bool isResponse = false;   // 是否为Call的响应消息
    bool priority = false;     // [P3] A.10 是否为高优先级消息

    // [P3] A.11 跨进程集群回程路由信息
    // 当消息来自远程节点时，这两个字段标识发送方的身份
    // 本地消息时为空，仅集群消息时填充
    std::string sourceNodeId;      // 发送方节点 ID（如 "nodeA"）
    std::string sourceActorName;   // 发送方 Actor 名字（如 "echo_service"）

    // [P2] A.8 类型安全 payload（替代纯字符串传递，可选使用）
    std::any payload;

    // [2026.5] 大包零拷贝路径：跨 Actor 转发时仅增加引用计数，不 memcpy
    //   - 小包（< kBufferThreshold）仍走 data 字段（SSO 零分配）
    //   - 大包用 sharedBuf，业务侧通过 Data()/Size() 统一访问
    // 注意：sharedBuf 非空时 data 字段忽略
    MessageBufferPtr sharedBuf;

    // 阈值建议：超过此长度的消息建议用 sharedBuf；
    // 小于此值用 std::string（SSO + 一次 malloc 已经很高效）
    static constexpr size_t kBufferThreshold = 1024;

    ActorMessage() = default;
    ActorMessage(MsgType t, uint32_t src, int f, std::string d)
        : type(t), sourceId(src), fd(f), data(std::move(d)) {}

    // [2026.5] 用 MessageBufferPtr 构造（大包零拷贝路径）
    ActorMessage(MsgType t, uint32_t src, int f, MessageBufferPtr buf)
        : type(t), sourceId(src), fd(f), sharedBuf(std::move(buf)) {}

    // 检查是否为跨进程消息（有远程来源信息）
    bool IsRemote() const { return !sourceNodeId.empty(); }

    // ===== [2026.5] 统一数据访问接口（自动二选一） =====

    // 数据起始指针：优先返回 sharedBuf，否则返回 data
    const char* Data() const {
        return sharedBuf ? sharedBuf->Data() : data.data();
    }

    // 数据长度：同上
    size_t Size() const {
        return sharedBuf ? sharedBuf->Size() : data.size();
    }

    // 是否为空
    bool DataEmpty() const {
        return sharedBuf ? sharedBuf->Empty() : data.empty();
    }

    // 转 string（必要时拷贝）
    std::string CopyToString() const {
        return sharedBuf ? sharedBuf->ToString() : data;
    }

    // 工厂方法：根据长度自动选择 std::string 或 MessageBuffer
    // 业务侧/网络层构造消息时推荐使用，让框架自动走小/大包优化路径
    static ActorMessage Make(MsgType t, uint32_t src, int f,
                             const char* pData, size_t len) {
        ActorMessage msg;
        msg.type = t;
        msg.sourceId = src;
        msg.fd = f;
        if (len < kBufferThreshold) {
            // 小包：走 std::string（SSO 命中或一次 malloc）
            msg.data.assign(pData, len);
        } else {
            // 大包：走 sharedBuf（一次 malloc，之后跨 Actor 共享）
            msg.sharedBuf = MakeBuffer(pData, len);
        }
        return msg;
    }

    // 工厂方法：从已有 std::string 构造（如果 string 较大，移动进 data；否则同上）
    // GatewayActor 解粘包后转发场景使用
    static ActorMessage Make(MsgType t, uint32_t src, int f, std::string&& s) {
        ActorMessage msg;
        msg.type = t;
        msg.sourceId = src;
        msg.fd = f;
        if (s.size() < kBufferThreshold) {
            // 小包：直接 move（仅指针交换）
            msg.data = std::move(s);
        } else {
            // 大包：构造 sharedBuf 跨 Actor 共享（仍需 1 次拷贝，但之后零拷贝）
            msg.sharedBuf = MakeBuffer(s.data(), s.size());
            // 注意：这里也可以让 MessageBuffer 直接接管 std::string 的堆，
            // 但需要给 MessageBuffer 增加 from_string 接口；当前简化处理
        }
        return msg;
    }

    // ===== [P2] A.8 类型安全 payload 辅助方法 =====

    // 设置 payload（模板方法）
    template<typename T>
    void SetPayload(T&& value) {
        payload = std::forward<T>(value);
    }

    // 获取 payload（返回指针，失败返回 nullptr）
    template<typename T>
    const T* GetPayload() const {
        return std::any_cast<T>(&payload);
    }

    // 获取 payload（返回指针，可修改版本）
    template<typename T>
    T* GetPayload() {
        return std::any_cast<T>(&payload);
    }

    // 获取 payload（抛异常版本，不推荐用于性能敏感场景）
    template<typename T>
    const T& GetPayloadRef() const {
        return std::any_cast<const T&>(payload);
    }

    // 检查 payload 是否有值
    bool HasPayload() const { return payload.has_value(); }

    // 检查 payload 是否为指定类型
    template<typename T>
    bool IsPayloadType() const {
        return payload.type() == typeid(T);
    }
};

} // namespace bllsll
