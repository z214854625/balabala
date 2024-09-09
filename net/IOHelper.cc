#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include "IOHelper.h"

using namespace bllsll;
using namespace std;

void IOHelper::SetNonBlocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int IOHelper::SetSockOpt(int fd)
{
    int keepalive = 1;
    int error = setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (void *)&keepalive, sizeof(keepalive));
    if (error != 0) {
        return -1;
    }
    int flags = 1;
    error = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));
    if (error != 0) {
        return -1;
    }
    struct linger ling = {0, 0};
    error = setsockopt(fd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));
    if (error != 0){
        return -1;
    }
    flags = 1;
    error = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (void *)&flags, sizeof(flags));
    if (error != 0) {
        return -1;
    }
    return 0;
}