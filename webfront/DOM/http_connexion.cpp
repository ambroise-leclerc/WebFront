#include "http_connexion.h"
#include "http_request.h"
#include <thread>
#include <list>
#include <mutex>

#include <iostream>
#include <iomanip>

class HTTPConnexion::Impl : public SocketHelper
{
public:
    Impl(SocketType socket) : socket_(socket)
    {
       // Test();
        
        {   // Add connexion to connexions list
            std::unique_lock<std::mutex>(connexions_mutex);
            connexions.push_back(std::unique_ptr<Impl>(this));
        }
    }

    void Disconnect()
    {
        Shutdown(socket_);
        CloseSocket(socket_);
        {   // Remove this connexion from connexions list
            std::unique_lock<std::mutex>(connexions_mutex);
            connexions.remove_if([this](std::unique_ptr<Impl>& cnx) { return cnx.get() == this; });
        }
    }

    static void CloseAllConnexions()
    {
        std::unique_lock<std::mutex>(connexions_mutex);
        while (!connexions.empty())
        {
            connexions.front()->Disconnect();
        }
    }

    void Serve()
    {
//        std::cout << "thread:" << std::this_thread::get_id() << " Serve" << std::endl;
        
        int received_bytes;

        send(socket_, "SERVER OK\r\n", 11, 0);
        
        HTTPRequest request;
        while ((received_bytes = recv(socket_, request.GetBuffer(), request.GetBufferSize(), 0)) > 0)
        {
            //std::cout << "Recv " << received_bytes << std::endl;
            //HexDump(buffer.data(), received_bytes);
            request.GetBuffer()[received_bytes] = 0;

            request.ParseData(received_bytes);
            if (request.HeaderIsComplete())
            {
                switch (request.method())
                {
                case HTTPRequest::kGet:
                    std::cout << "Method GET on url " << request.url() << std::endl;
                    std::cout << "  host : " << request.host() << std::endl;
                    break;
                case HTTPRequest::kHead:
                    std::cout << "Method HEAD" << std::endl;
                    break;
                }
            }

            
        }
        std::cout << "thread:" << std::this_thread::get_id() << " Serve ended" << std::endl;

        Disconnect();
    }

    void Test()
    {
        HTTPRequest request;
        std::string test_header = "GET /myurl HTTP/1.1\r\nHost:1\r\nUser-Agent:2\r\n  0\r\n\r\n";

        char* buffer = request.GetBuffer();
        for (auto c : test_header)
            *buffer++ = c;
        
        request.ParseData(test_header.length());
        if (HTTPRequest::kGet == request.method()) std::cout << "GET ok" << std::endl; else std::cout << "GET no" << std::endl;
        std::cout << "Url:" << request.url() << std::endl;
        std::cout << "Host:" << request.host() << std::endl;
        std::cout << "User-Agent:" << request.user_agent() << std::endl;
    }

private:
    SocketType socket_;

    static std::list<std::unique_ptr<Impl>> connexions;     //< List all connections 
    static std::mutex connexions_mutex;

private:
    static void HexDump(void* data, unsigned int bytes)
    {
        char *parser, *last=reinterpret_cast<char*>(data);
        parser = last;
        last += bytes;

        char output_line[] = "                                                 | ................";
        
        for (; parser<=last; parser+=16)
        {
            for (unsigned int charac=0; charac<16; charac++)
            {
                if (parser+charac < last)
                {
                    char towrite = parser[charac];
                    if ((towrite>>4)<10)
                        output_line[charac*3] = (towrite>>4) + 48;
                    else
                        output_line[charac*3] = (towrite>>4) + 55;
                    if ((towrite%16)<10)
                        output_line[charac*3+1] = (towrite%16) + 48;
                    else
                        output_line[charac*3+1] = (towrite%16) + 55;
                    if (towrite>31)
                    {
                        output_line[51+charac] = towrite;
                    }
                    else
                    {
                        output_line[51+charac] = '.';
                    }
                }
                else
                {
                    output_line[charac*3] = '.';
                    output_line[charac*3+1] = '.';
                    output_line[51+charac] = '.';
                }
            }
            std::cout << std::setw(8) << std::setfill('0') << std::hex << reinterpret_cast<void*>(parser);
            std::cout << " " << output_line << std::endl;
        }
    }
};


HTTPConnexion::HTTPConnexion(SocketType socket) : pimpl_(new Impl(socket)) {}
HTTPConnexion::~HTTPConnexion() {}
void HTTPConnexion::Serve() { pimpl_->Serve(); }
void HTTPConnexion::CloseAllConnexions() { Impl::CloseAllConnexions(); }

std::list<std::unique_ptr<HTTPConnexion::Impl>> HTTPConnexion::Impl::connexions;
std::mutex HTTPConnexion::Impl::connexions_mutex;

