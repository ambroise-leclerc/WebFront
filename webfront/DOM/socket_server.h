#ifndef __SOCKET_SERVER_H_
#define __SOCKET_SERVER_H_

#include <memory>
#include <string>


class SocketServer
{
public:
    enum ServerStatus
    {   kUninitialized,
        kInitializationFailed,
        kInitialized,
        kStarted,
        kStopped };

public:
    SocketServer(int port);
    SocketServer(std::string protocol);
    ~SocketServer();

    bool Start();
    void Stop();
    ServerStatus server_status() const;

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};



#endif //__SOCKET_SERVER_H_