#pragma once
/**
@auther: chencaiyu
@date: 2024.8.18
@brief: IO帮助类
*/

namespace bllsll {

class IOHelper
{
public:
    IOHelper() = default;
    ~IOHelper() = default;

    static void SetNonBlocking(int fd);
    static int SetSockOpt(int fd);
};

} //namespace bllsll