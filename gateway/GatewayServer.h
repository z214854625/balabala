#pragma once
/**
@auther: chencaiyu
@date: 2025.1.5
@brief: 网关服务器
*/

namespace bllsll {

class GatewayServer
{
public:
    GatewayServer() = default;
    ~GatewayServer() = default;
    //初始化
    bool Create();
    //释放
    void Release();
};

} // namespace bllsll
