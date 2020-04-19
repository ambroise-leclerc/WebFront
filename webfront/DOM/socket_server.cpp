#include "socket_server.h"
#include <vector>
#include <iostream>
#include <thread>

#include "socket_helper.h"
#include "thread_pool.h"
#include "http_request.h"
#include "http_connexion.h"


class SocketServer::Impl : public SocketHelper
{
public:
    Impl(int port) : server_status_(kUninitialized), server_info_(nullptr)
    {
        InitializeSocket(std::to_string(port));
    }

    Impl(std::string protocol) : server_status_(kUninitialized), server_info_(nullptr)
    {
        InitializeSocket(protocol);
    }

    ~Impl()
    {
        freeaddrinfo(server_info_);
        CleanupLib();
    }

    bool Start()
    {
        if (nullptr == server_info_)
            return false;
        listening_socket_ = socket(server_info_->ai_family, server_info_->ai_socktype, server_info_->ai_protocol);
        if (kInvalidSocket == listening_socket_)
            return false;
        
        if (0 != bind(listening_socket_, server_info_->ai_addr, server_info_->ai_addrlen))
            return false;

        if (0 != listen(listening_socket_, SOMAXCONN))
            return false;

        listening_ = true;
        listening_thread_ = std::thread(&Impl::ListenLoop, this);
        set_server_status(kStarted);
        return true;
    }

    void Stop()
    {
        Shutdown(listening_socket_);

        listening_ = false;
        CloseSocket(listening_socket_);
        std::cout << "socket closed" << std::endl;
        listening_thread_.join();
        std::cout << "thread joined" << std::endl;
        set_server_status(kStopped);
    }

    ServerStatus server_status() const { return server_status_; }
    
private:
    void InitializeSocket(std::string protocol)
    {
        set_server_status(kUninitialized);
        listening_ = false;
        if (!InitializeLib())
        {
            set_server_status(kInitializationFailed);
            return;
        }

        int status;
        addrinfo hints = {};

        hints.ai_family = AF_UNSPEC;                // don't care IPv4 or IPv6
        hints.ai_socktype = SOCK_STREAM;            // TCP stream sockets
        hints.ai_flags = AI_PASSIVE;                // Fill my IP for me
        status = getaddrinfo(nullptr, protocol.c_str(), &hints, &server_info_);
        
        if (0 != status)
        {
            set_server_status(kInitializationFailed);
            return;
        }

        set_server_status(kInitialized);
    }

    void set_server_status(ServerStatus status)
    {
        const char* nom[] = { "Uninitialized", "InitializationFailed", "Initialized", "Started", "Stopped" };
        server_status_ = status;
        std::cout << "server_status changed to " << nom[status] << std::endl;
    }

    void ListenLoop()
    {
        std::cout << "ListenLoop started" << std::endl;
        sockaddr_storage client_addr;
        socklen_t client_addr_size;
        SocketType client_socket;

        ThreadPool thread_pool;

        client_addr_size = sizeof client_addr;
        while (listening_)
        {
            client_socket = accept(listening_socket_, reinterpret_cast<sockaddr*>(&client_addr), &client_addr_size);
            if (kInvalidSocket == client_socket)
            {
                std::cout << "Erreur socket" << std::endl;
            }
            else
            {
                HTTPConnexion* connexion = new HTTPConnexion(client_socket);
                std::function<void()> myfunc = std::bind(&HTTPConnexion::Serve, connexion);
                thread_pool.EnqueueTask(myfunc);
            }
        }
        HTTPConnexion::CloseAllConnexions();
        std::cout << "ListenLoop end" << std::endl;
    }

private:
    addrinfo *server_info_;                                 //< Listening socket address structure
    ServerStatus server_status_;
    SocketType listening_socket_;
    std::thread listening_thread_;
    bool listening_;                                        //< Is the listening loop started ?

};


SocketServer::SocketServer(int port) : pimpl_(new Impl(port)) {}
SocketServer::SocketServer(std::string protocol) : pimpl_(new Impl(protocol)) {}
SocketServer::~SocketServer() {}
bool SocketServer::Start() { return pimpl_->Start(); }
void SocketServer::Stop() { return pimpl_->Stop(); }
SocketServer::ServerStatus SocketServer::server_status() const { return pimpl_->server_status(); }
