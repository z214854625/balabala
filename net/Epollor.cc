#include "Epollor.h"

using namespace sll;
using namespace std;

Epollor::Epollor(int maxEvents) : maxEvents_(maxEvents), events_(maxEvents)
{
    epollFd_ = epoll_create1(0);
    if (epollFd_ == -1) {
        throw std::runtime_error("Failed to create epoll file descriptor");
    }
}

Epollor::~Epollor()
{
    close(epollFd_);
}

void Epollor::AddEvent(int fd, uint32_t events)
{
    //std::cout << "AddEvent fd=" << fd << ", events="<< events << std::endl;
    epoll_event event{};
    event.events = events;
    event.data.fd = fd;
    if (epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &event) == -1) {
        throw std::runtime_error("Failed to add file descriptor to epoll! fd="+ to_string(fd) + ", event=" + to_string(events));
    }
}

void Epollor::ModifyEvent(int fd, uint32_t events)
{
    //std::cout << "ModifyEvent fd=" << fd << ", events="<< events << std::endl;
    epoll_event event{};
    event.events = events;
    event.data.fd = fd;
    if (epoll_ctl(epollFd_, EPOLL_CTL_MOD, fd, &event) == -1) {
        throw std::runtime_error("Failed to modify file descriptor in epoll! fd=" + to_string(fd) + ", event=" + to_string(events));
    }
}

void Epollor::RemoveEvent(int fd)
{
    //std::cout << "RemoveEvent fd=" << fd << std::endl;
    if (epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr) == -1) {
        throw std::runtime_error("Failed to remove file descriptor from epoll! fd=" + to_string(fd));
    }
}

int Epollor::Wait(int timeout)
{
    return epoll_wait(epollFd_, events_.data(), maxEvents_, timeout);
}
