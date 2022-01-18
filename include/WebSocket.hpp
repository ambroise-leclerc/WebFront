#pragma once
#include <functional>
#include <memory>
#include <vector>


namespace webfront {
namespace websocket {

using Handle = uint32_t;

struct Header {
	uint32_t FIN : 1;
	uint32_t RSV1 : 1;
	uint32_t RSV2 : 1;
	uint32_t RSV3 : 1;
	uint32_t opcode : 4;
	uint32_t MASK : 1;
	uint32_t payloadLen : 7;
	uint32_t extendedLen : 16;

};
static_assert(sizeof(Header) == 4, "WebSocket Header size must be 4 bytes");


template<typename Socket>
class WebSocket {
	Socket socket;

public:
	WebSocket(Socket socket) : socket(std::move(socket)) {

	}
};

template<typename Socket>
class WSManager {
	std::vector < std::shared_ptr<WebSocket<Socket>>> webSockets;
	Handle handlesCounter = 0;
public:
	std::function<void(Handle)> onSpawn;

public:
	void createWebSocket(Socket socket) {
		webSockets.push_back(std::make_shared<WebSocket<Socket>>(std::move(socket)));
		handlesCounter = static_cast<Handle>(webSockets.size());
		if (onSpawn) onSpawn(handlesCounter);
	};
};


} // namespace websocket
} // namespace webfront