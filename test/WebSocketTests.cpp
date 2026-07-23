#include <http/WebSocket.hpp>
#include <networking/NetworkingMock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <functional>
#include <system_error>
#include <vector>

using namespace webfront;
using namespace std;
using Net = networking::NetworkingMock;

namespace {

// Lets a test control exactly when a pending async_read_some completes, and with what error -
// needed to simulate a read finishing after the WebSocket has already been stopped.
class InjectableSocket : public networking::SocketBaseMock {
public:
    void async_read_some(auto buffer, auto completion) {
        readBuffer  = buffer;
        readHandler = std::move(completion);
    }

    size_t write_some(auto input, error_code&) {
        const auto* first = static_cast<const byte*>(input.data());
        written.insert(written.end(), first, first + input.size());
        return input.size();
    }

    void close() { closeCalls++; }
    void shutdown(int) {}

    static void fail(error_code error) {
        REQUIRE(readHandler);
        auto handler = std::move(readHandler);
        handler(error, 0);
    }

    static void reset() {
        readHandler = {};
        written.clear();
        closeCalls = 0;
    }

    static int closeCallCount() { return closeCalls; }

private:
    inline static networking::buffers::MutableBuffer readBuffer;
    inline static function<void(error_code, size_t)>  readHandler;
    inline static vector<byte>                        written;
    inline static int                                 closeCalls = 0;
};

class InjectableNetworking : public networking::NetworkingMock {
public:
    using Socket = InjectableSocket;

    template <typename WriteHandler>
    static void AsyncWrite(Socket socket, auto buffers, WriteHandler handler) {
        error_code error;
        size_t     transferred = 0;
        for (const auto& buffer : buffers)
            transferred += socket.write_some(buffer, error);
        handler(error, transferred);
    }
};

} // namespace

SCENARIO("WebSocket Headers decoding") {
    GIVEN("Some Header data") {
        array<uint8_t, 14> data{0b10000001, 0b10000000 | 63, 0x10, 0x11, 0x12, 0x13};
        WHEN("Casting to header") {
            auto header = reinterpret_cast<const websocket::Header*>(data.data());
            THEN("Fields should be") {
                REQUIRE(header->FIN() == true);
                REQUIRE(header->RSV1() == false);
                REQUIRE(header->RSV2() == false);
                REQUIRE(header->RSV3() == false);
                REQUIRE(header->opcode() == websocket::Header::Opcode::text);
                REQUIRE(header->MASK() == true);
                REQUIRE(header->maskingKey() == std::array<std::byte, 4>{std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13}});
                REQUIRE(header->payloadLenField() == 63);
                REQUIRE(header->payloadSize() == 63);
                REQUIRE(header->headerSize() == 6);
            }
        }
    }

    GIVEN("Header data") {
        array<uint8_t, 14> data{0b10000001, 0b10000000 | 126, 10, 20, 0x10, 0x11, 0x12, 0x13};
        WHEN("Casting to header") {
            auto header = reinterpret_cast<const websocket::Header*>(data.data());
            THEN("Payload size should be 31127 from the 16 bits field") {
                REQUIRE(header->payloadSize() == 2580);
                REQUIRE(header->headerSize() == 8);
            }
        }
    }

    GIVEN("Header data") {
        array<uint8_t, 14> data{0b10000001, 0b10000000 | 127, 10, 20, 30, 40, 50, 60, 70, 80, 0x10, 0x11, 0x12, 0x13};
        WHEN("Casting to header") {
            auto header = reinterpret_cast<const websocket::Header*>(data.data());
            THEN("Payload size should be 13 256 444 from the 64 bits field") {
                REQUIRE(header->payloadSize() == 726238597903828560);
                REQUIRE(header->headerSize() == 14);
            }
        }
    }
}

SCENARIO("WebSocket frame encoding"){

  GIVEN("Some text to send"){

    std::string text = "Hello WS";

WHEN("Encoding it") {
    websocket::Frame<Net> frame(text);
    THEN("A text frame is produced") {
        REQUIRE(frame.FIN() == true);
        REQUIRE(frame.RSV1() == false);
        REQUIRE(frame.RSV2() == false);
        REQUIRE(frame.RSV3() == false);
        REQUIRE(frame.opcode() == websocket::Header::Opcode::text);
        REQUIRE(frame.MASK() == false);
        REQUIRE(frame.payloadLenField() == 8);
        REQUIRE(frame.payloadSize() == 8);
        REQUIRE(frame.headerSize() == 2);

        auto buffers = frame.toBuffers();
        using Buffer = decltype(buffers[0]);
        auto compare = [](Buffer b, std::string s) {
            auto data = reinterpret_cast<const char*>(b.data());
            for (size_t index = 0; index < s.size(); ++index)
                if (data[index] != s[index]) return false;
            return true;
        };

        REQUIRE(compare(buffers[1], "Hello WS"));
    }
}
}
}
;

SCENARIO("WebSocket frames can own payloads for asynchronous writes") {
    GIVEN("A frame assembled from temporary message buffers") {
        array<byte, 2> header{byte{0x01}, byte{0x02}};
        array<byte, 3> payload{byte{0x03}, byte{0x04}, byte{0x05}};
        websocket::Frame<Net> frame(header);
        frame.addBuffer(payload);

        WHEN("The frame is frozen before the source buffers change") {
            frame.freeze();
            frame.freeze();
            header.fill(byte{0xff});
            payload.fill(byte{0xff});
            const auto buffers = frame.toBuffers();

            THEN("Repeated freezing keeps the encoded payload valid") {
                const auto* encodedHeader = static_cast<const byte*>(buffers[1].data());
                const auto* encodedPayload = static_cast<const byte*>(buffers[2].data());
                REQUIRE(buffers[1].size() == 2);
                REQUIRE(encodedHeader[0] == byte{0x01});
                REQUIRE(encodedHeader[1] == byte{0x02});
                REQUIRE(buffers[2].size() == 3);
                REQUIRE(encodedPayload[0] == byte{0x03});
                REQUIRE(encodedPayload[1] == byte{0x04});
                REQUIRE(encodedPayload[2] == byte{0x05});
            }
        }
    }
}

SCENARIO("WebSocket decoder") {
    GIVEN("Some frame data and a decoder") {
        array<uint8_t, 22> frame{0x1,
                                 0b10000000 | 8,
                                 0x10,
                                 0x11,
                                 0x12,
                                 0x13,
                                 uint8_t{'H' ^ 0x10},
                                 uint8_t{'e' ^ 0x11},
                                 uint8_t{'l' ^ 0x12},
                                 uint8_t{'l' ^ 0x13},
                                 uint8_t{'o' ^ 0x10},
                                 uint8_t{' ' ^ 0x11},
                                 uint8_t{'W' ^ 0x12},
                                 uint8_t{'S' ^ 0x13}};
        websocket::FrameDecoder decoder;
        WHEN("All data is received in one unique chunk") {
            REQUIRE(decoder.parse(std::span(reinterpret_cast<const std::byte*>(frame.data()), frame.size())));
            auto bufferParser = std::cbegin(decoder.payload());
            THEN("Payload is correctly decoded") {
                for (auto c : {'H', 'e', 'l', 'l', 'o', ' ', 'W', 'S'}) REQUIRE(std::to_integer<uint8_t>(*bufferParser++) == c);
            }
        }

        WHEN("Data is received in two chunks but the first has a complete header") {
            REQUIRE(decoder.parse(std::span(reinterpret_cast<const std::byte*>(frame.data()), 7)) == false);
            REQUIRE(decoder.parse(std::span(reinterpret_cast<const std::byte*>(frame.data() + 7), frame.size() - 7)));
            auto bufferParser = std::cbegin(decoder.payload());
            THEN("Payload is correctly decoded") {
                for (auto c : {'H', 'e', 'l', 'l', 'o', ' ', 'W', 'S'}) REQUIRE(std::to_integer<uint8_t>(*bufferParser++) == c);
            }
        }

        WHEN("Data is received in two chunks but the first has an incomplete header") {
            REQUIRE(decoder.parse(std::span(reinterpret_cast<const std::byte*>(frame.data()), 3)) == false);
            REQUIRE(decoder.parse(std::span(reinterpret_cast<const std::byte*>(frame.data() + 3), frame.size() - 3)));
            auto bufferParser = std::cbegin(decoder.payload());
            THEN("Payload is correctly decoded") {
                for (auto c : {'H', 'e', 'l', 'l', 'o', ' ', 'W', 'S'}) REQUIRE(std::to_integer<uint8_t>(*bufferParser++) == c);
            }
        }
    }
}

SCENARIO("WebSocket decoder preserves coalesced frames") {
    GIVEN("Two masked text frames received in one chunk") {
        const array<uint8_t, 16> frames{0x81,
                                        0x82,
                                        0x10,
                                        0x11,
                                        0x12,
                                        0x13,
                                        uint8_t{'o' ^ 0x10},
                                        uint8_t{'n' ^ 0x11},
                                        0x81,
                                        0x82,
                                        0x20,
                                        0x21,
                                        0x22,
                                        0x23,
                                        uint8_t{'o' ^ 0x20},
                                        uint8_t{'k' ^ 0x21}};
        const auto               bytes = std::span(reinterpret_cast<const std::byte*>(frames.data()), frames.size());
        websocket::FrameDecoder  decoder;

        WHEN("The first frame is decoded") {
            REQUIRE(decoder.parse(bytes));
            REQUIRE(decoder.consumed() == 8);
            REQUIRE(decoder.payload().size() == 2);
            REQUIRE(std::to_integer<char>(decoder.payload()[0]) == 'o');
            REQUIRE(std::to_integer<char>(decoder.payload()[1]) == 'n');

            THEN("The unconsumed bytes decode as the second frame") {
                const auto firstFrameSize = decoder.consumed();
                decoder.reset();
                REQUIRE(decoder.parse(bytes.subspan(firstFrameSize)));
                REQUIRE(decoder.consumed() == 8);
                REQUIRE(decoder.payload().size() == 2);
                REQUIRE(std::to_integer<char>(decoder.payload()[0]) == 'o');
                REQUIRE(std::to_integer<char>(decoder.payload()[1]) == 'k');
            }
        }
    }
}

SCENARIO("WebSocket decoder waits for an entire split payload") {
    GIVEN("A masked text frame split across three reads") {
        const array<uint8_t, 14> frame{0x81,
                                       0x88,
                                       0x10,
                                       0x11,
                                       0x12,
                                       0x13,
                                       uint8_t{'H' ^ 0x10},
                                       uint8_t{'e' ^ 0x11},
                                       uint8_t{'l' ^ 0x12},
                                       uint8_t{'l' ^ 0x13},
                                       uint8_t{'o' ^ 0x10},
                                       uint8_t{' ' ^ 0x11},
                                       uint8_t{'W' ^ 0x12},
                                       uint8_t{'S' ^ 0x13}};
        const auto               bytes = std::span(reinterpret_cast<const std::byte*>(frame.data()), frame.size());
        websocket::FrameDecoder  decoder;

        // The middle chunk (6 bytes) delivers more than half but not all of the 7 outstanding
        // payload bytes. A naive "recompute remaining after appending" check (remaining=7-1=6
        // before this call, buffer.size()=6, but comparing against remaining *after* appending -
        // 8-(1+6)=1 - would wrongly see 6 >= 1 and report the frame complete one byte early. This
        // split is chosen specifically to fail under that bug; smaller middle chunks (e.g. 3 of 7)
        // don't exercise it, since old and new logic coincidentally agree there.
        WHEN("The middle read still leaves payload bytes outstanding") {
            REQUIRE_FALSE(decoder.parse(bytes.first(7)));
            REQUIRE(decoder.consumed() == 7);
            REQUIRE_FALSE(decoder.parse(bytes.subspan(7, 6)));
            REQUIRE(decoder.consumed() == 6);

            THEN("Only the final read completes the frame") {
                REQUIRE(decoder.parse(bytes.subspan(13)));
                REQUIRE(decoder.consumed() == 1);
                REQUIRE(decoder.payload().size() == 8);
            }
        }
    }
}

SCENARIO("WebSocket stop() is idempotent") {
    InjectableSocket::reset();
    int  closeHandlerCalls = 0;
    auto ws = websocket::WebSocket<InjectableNetworking>::create(InjectableSocket{});
    ws->onClose([&closeHandlerCalls](websocket::CloseEvent) { ++closeHandlerCalls; });

    GIVEN("a started WebSocket that is stopped explicitly while a read is pending") {
        ws->start();
        ws->stop();

        WHEN("the still-pending read then completes with an error") {
            InjectableSocket::fail(make_error_code(errc::bad_file_descriptor));

            THEN("the error is treated as harmless: no duplicate close handler call or socket close") {
                REQUIRE(closeHandlerCalls == 0);
                REQUIRE(InjectableSocket::closeCallCount() == 1);
            }
        }

        WHEN("stop() is called again") {
            THEN("the second call does not close the socket a second time") {
                REQUIRE_NOTHROW(ws->stop());
                REQUIRE(InjectableSocket::closeCallCount() == 1);
            }
        }
    }

    GIVEN("a started WebSocket whose pending read fails with no prior explicit stop") {
        ws->start();

        WHEN("the read completes with an error") {
            InjectableSocket::fail(make_error_code(errc::bad_file_descriptor));

            THEN("the close handler fires exactly once and the socket is closed exactly once") {
                REQUIRE(closeHandlerCalls == 1);
                REQUIRE(InjectableSocket::closeCallCount() == 1);
            }
        }
    }
}
