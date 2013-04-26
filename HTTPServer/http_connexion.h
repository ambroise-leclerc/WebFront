#ifndef __HTTP_CONNEXION_H_
#define __HTTP_CONNEXION_H_
#include <memory>
#include "socket_helper.h"


class HTTPConnexion
{
public:
    HTTPConnexion(SocketType socket);
    ~HTTPConnexion();
    void Serve();                           //< Connexion main loop
    static void CloseAllConnexions();       //< Close all open HTTPConnexions sockets

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

#endif //__HTTP_CONNEXION_H_