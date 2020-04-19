#include <webfront/net/TCPServer.hpp>

#include <iostream>
#include <list>


int main() {
    using namespace std;

    try {
        webfront::net::TCPServer server(40080);
        list<shared_ptr<webfront::net::TCPClient>> clients;

        server.onNewClient = [&clients](shared_ptr<webfront::net::TCPClient> newClient) {
            clients.push_back(newClient);
            newClient->onReceive = [](span<uint8_t> data) {
                cout << "Received " << data.size() << '\n';
            };
        };

        server.start();
    }
    catch (const exception& e) {
        cout << "Uncaugth exception : " << e.what() << '\n';
    }

    cout << "Hello world\n";

    return 0;
}