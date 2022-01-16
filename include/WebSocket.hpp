#pragma once

namespace webfront {
namespace websocket {

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

} // namespace websocket
} // namespace webfront