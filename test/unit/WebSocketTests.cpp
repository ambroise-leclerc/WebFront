#include "InjectableNetworking.hpp"
#include "InlineTaskScope.hpp"
#include "Bdd.hpp"
#include <webfront/http/WebSocket.hpp>
#include <webfront/networking/NetworkingMock.hpp>

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

using namespace webfront::bdd;

using namespace webfront;
using namespace std;
using Net = networking::NetworkingMock;
using webfront::testing::InjectableNetworking;
using webfront::testing::InjectableSocket;

const Scenario stoppedWebSocket{"A stopped WebSocket does not read again", [] {
    InjectableSocket::reset();
    int closeHandlerCalls = 0;
    webfront::testing::InlineTaskScope scope;
    auto                               ws = websocket::WebSocket<InjectableNetworking>::create(InjectableSocket{}, scope);
    ws->onClose([&](websocket::CloseEvent) { ++closeHandlerCalls; });
    ws->start();
    require(InjectableSocket::readCallCount() == 1);

    given("A read is pending when the WebSocket is stopped", [&] {
        ws->stop();

        when("The read completes", [&] {
            InjectableSocket::fail({});

            then("No other read is started and an explicit stop is not reported as a closure", [&] {
                require(InjectableSocket::readCallCount() == 1);
                require(closeHandlerCalls == 0);
            });
        });
    });
}};

const Scenario failedWrite{"A failed write closes the WebSocket", [] {
    InjectableSocket::reset();
    int closeHandlerCalls = 0;
    webfront::testing::InlineTaskScope scope;
    auto                               ws = websocket::WebSocket<InjectableNetworking>::create(InjectableSocket{}, scope);
    ws->onClose([&](websocket::CloseEvent) { ++closeHandlerCalls; });
    ws->start();

    given("A pending read and a write which fails", [&] {
        InjectableSocket::failWrites(make_error_code(errc::broken_pipe));
        ws->write("pending write");

        then("The closure is reported once", [&] { require(closeHandlerCalls == 1); });

        when("The read completes", [&] {
            InjectableSocket::fail({});

            then("No other read is started and the closure is not reported again", [&] {
                require(InjectableSocket::readCallCount() == 1);
                require(closeHandlerCalls == 1);
            });
        });
    });
}};

const Scenario webSocketKeeps{"A WebSocket keeps only the latest frame of a key that has too many frames waiting", [] {
    InjectableSocket::reset();
    webfront::testing::InlineTaskScope scope;
    auto                               ws = websocket::WebSocket<InjectableNetworking>::create(InjectableSocket{}, scope);
    ws->start();
    InjectableSocket::deferWrites();
    const auto shared = [](std::string_view text) {
        return websocket::Frame<InjectableNetworking>(text).share();
    };

    given("another frame being written and a limit of one frame for the key", [&] {
        ws->write("other-frame");
        ws->writeLatest(shared("state-1"), "onState", 1);
        const auto intermediate = shared("state-2");
        ws->writeLatest(intermediate, "onState", 1);
        ws->writeLatest(shared("state-3"), "onState", 1);

        then("the other frame does not count, and the intermediate frame was released when replaced", [&] {
            require(ws->pendingWrites() == 2);
            require(intermediate.use_count() == 1);
        });
        when("the network completes the writes", [&] {
            while (InjectableSocket::completeDeferredWrites() != 0) { }
            then("the first and latest frames are written in order, not the intermediate one", [&] {
                require(InjectableSocket::wrote("other-frame"));
                require(InjectableSocket::wrote("state-1"));
                require(InjectableSocket::wrote("state-3"));
                require(!InjectableSocket::wrote("state-2"));
                require(ws->pendingWrites() == 0);
            });
        });
    });
    given("a frame held back behind a write that fails", [&] {
        InjectableSocket::failWrites(make_error_code(errc::broken_pipe));
        ws->writeLatest(shared("state-1"), "onState", 1);
        const auto held = shared("state-2");
        ws->writeLatest(held, "onState", 1);
        require(held.use_count() == 2);

        when("the write fails", [&] {
            while (InjectableSocket::completeDeferredWrites() != 0) { }
            then("the held frame is released with the queue", [&] {
                require(held.use_count() == 1);
                require(ws->pendingWrites() == 0);
            });
        });
    });
    InjectableSocket::deferWrites(false);
    while (InjectableSocket::completeDeferredWrites() != 0) { }
    ws->stop();
    InjectableSocket::reset();
}};

const Scenario webSocketReassembles{"A WebSocket reassembles the messages a browser sends in several frames", [] {
    InjectableSocket::reset();
    vector<vector<byte>> binaryMessages;
    vector<string>       textMessages;
    int                  closeHandlerCalls = 0;
    webfront::testing::InlineTaskScope scope;
    auto                               ws = websocket::WebSocket<InjectableNetworking>::create(InjectableSocket{}, scope);
    ws->onMessage([&](span<const byte> data) { binaryMessages.emplace_back(data.begin(), data.end()); });
    ws->onMessage([&](string_view text) { textMessages.emplace_back(text); });
    ws->onClose([&](websocket::CloseEvent) { ++closeHandlerCalls; });
    ws->start();
    const auto bytes = [](string_view text) { return as_bytes(span(text)); };
    const auto receive = [&](string_view payload, uint8_t finAndOpcode) {
        InjectableSocket::receive(webfront::testing::clientFrame(bytes(payload), byte{finAndOpcode}));
    };

    given("A binary message in three fragments, with a control frame between them", [&] {
        receive("ab", 0x02);
        receive("", 0x89);  // ping
        receive("cd", 0x00);
        require(binaryMessages.empty());
        receive("ef", 0x80);

        then("The handler receives the whole message once", [&] {
            const auto whole = bytes("abcdef");
            require(binaryMessages == vector<vector<byte>>{{whole.begin(), whole.end()}});
            require(closeHandlerCalls == 0);
        });
        andWhen("A text message follows in two fragments", [&] {
            receive("hel", 0x01);
            receive("lo", 0x80);
            then("It is delivered as text", [&] {
                require(textMessages == vector<string>{"hello"});
                require(binaryMessages.size() == 1);
            });
        });
    });
    given("A continuation frame without a message to continue", [&] {
        receive("x", 0x80);
        then("The connection fails without delivering it", [&] {
            require(closeHandlerCalls == 1);
            require(binaryMessages.empty());
        });
    });
    given("A new message before the previous one is finished", [&] {
        receive("a", 0x02);
        receive("b", 0x82);
        then("The connection fails without delivering either", [&] {
            require(closeHandlerCalls == 1);
            require(binaryMessages.empty());
        });
    });
    given("A fragmented ping", [&] {
        receive("p", 0x09);
        then("The connection fails, since control frames cannot be fragmented", [&] {
            require(closeHandlerCalls == 1);
        });
    });
    InjectableSocket::reset();
}};

const Scenario webSocketLimits{"A WebSocket limits the size of the messages it reassembles", [] {
    InjectableSocket::reset();
    vector<vector<byte>> messages;
    optional<uint16_t>   closeCode;
    using Limited = http::CustomBuffersPolicy<8192, 8192, 32768, 4>;
    webfront::testing::InlineTaskScope scope;
    auto                               ws = websocket::WebSocket<InjectableNetworking, Limited>::create(InjectableSocket{}, scope);
    ws->onMessage([&](span<const byte> data) { messages.emplace_back(data.begin(), data.end()); });
    ws->onClose([&](websocket::CloseEvent event) { closeCode = event.status; });
    ws->start();
    const auto receive = [&](string_view payload, uint8_t finAndOpcode) {
        InjectableSocket::receive(webfront::testing::clientFrame(as_bytes(span(payload)), byte{finAndOpcode}));
    };
    const auto messageSize = static_cast<uint16_t>(std::make_error_code(std::errc::message_size).value());

    given("Fragments adding up to the limit", [&] {
        receive("ab", 0x02);
        receive("cd", 0x80);
        then("The message is delivered", [&] {
            require(messages.size() == 1);
            require(!closeCode);
        });
    });
    // Only the header of a frame (2 bytes, then the 4-byte mask): the payload never comes.
    const auto receiveHeader = [&](string_view payload, uint8_t finAndOpcode) {
        auto frame = webfront::testing::clientFrame(as_bytes(span(payload)), byte{finAndOpcode});
        frame.resize(6);
        InjectableSocket::receive(frame);
    };

    given("A ping of 125 bytes, more than the message limit", [&] {
        receive(string(125, 'p'), 0x89);
        then("The connection stays open, since control frames have their own limit", [&] {
            require(!closeCode);
            receive("ok", 0x82);
            require(messages.size() == 1);
        });
    });
    given("The header of a ping announcing 126 bytes", [&] {
        // FIN and ping; masked 16-bit length 126; masking key.
        InjectableSocket::receive(vector<byte>{byte{0x89}, byte{0xfe}, byte{0}, byte{126}, byte{0x12}, byte{0x34}, byte{0x56}, byte{0x78}});
        then("The connection fails with protocol_error", [&] {
            require(closeCode == static_cast<uint16_t>(std::make_error_code(std::errc::protocol_error).value()));
        });
    });
    given("A continuation announcing more than what is left of the limit", [&] {
        receive("ab", 0x02);
        receiveHeader("cde", 0x80);
        then("The connection fails with message_size on its header", [&] {
            require(messages.empty());
            require(closeCode == messageSize);
        });
    });
    given("The header of a first fragment announcing more than the limit", [&] {
        receiveHeader("abcde", 0x02);
        then("The connection fails with message_size without waiting for the payload", [&] {
            require(closeCode == messageSize);
        });
    });
    given("The header of a message in one frame announcing more than the limit", [&] {
        receiveHeader("abcde", 0x82);
        then("The connection fails with message_size", [&] {
            require(messages.empty());
            require(closeCode == messageSize);
        });
    });
    InjectableSocket::reset();
}};

const Scenario webSocketRejects{"A WebSocket rejects a frame announcing gigabytes before buffering it", [] {
    InjectableSocket::reset();
    optional<uint16_t> closeCode;
    webfront::testing::InlineTaskScope scope;
    auto                               ws = websocket::WebSocket<InjectableNetworking>::create(InjectableSocket{}, scope);
    ws->onMessage([](span<const byte>) {});
    ws->onClose([&](websocket::CloseEvent event) { closeCode = event.status; });
    ws->start();

    given("The header of a first fragment announcing 8 GiB, with the default limit", [&] {
        // FIN clear, binary; masked 64-bit length 0x200000000; masking key.
        InjectableSocket::receive(vector<byte>{byte{0x02}, byte{0xff}, byte{0}, byte{0}, byte{0}, byte{2}, byte{0}, byte{0}, byte{0}, byte{0},
                                               byte{0x12}, byte{0x34}, byte{0x56}, byte{0x78}});
        then("The connection fails with message_size", [&] {
            require(closeCode == static_cast<uint16_t>(std::make_error_code(std::errc::message_size).value()));
        });
    });
    InjectableSocket::reset();
}};

const Scenario webSocketHeaders{"WebSocket Headers decoding", [] {
    given("Some Header data", [&] {
        array<uint8_t, 14> data{0b10000001, 0b10000000 | 63, 0x10, 0x11, 0x12, 0x13};
        when("Casting to header", [&] {
            auto header = reinterpret_cast<const websocket::Header*>(data.data());
            then("Fields should be", [&] {
                require(header->FIN() == true);
                require(header->RSV1() == false);
                require(header->RSV2() == false);
                require(header->RSV3() == false);
                require(header->opcode() == websocket::Header::Opcode::text);
                require(header->MASK() == true);
                require(header->maskingKey() == std::array<std::byte, 4>{std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13}});
                require(header->payloadLenField() == 63);
                require(header->payloadSize() == 63);
                require(header->headerSize() == 6);
            });
        });
    });

    given("Header data", [&] {
        array<uint8_t, 14> data{0b10000001, 0b10000000 | 126, 10, 20, 0x10, 0x11, 0x12, 0x13};
        when("Casting to header", [&] {
            auto header = reinterpret_cast<const websocket::Header*>(data.data());
            then("Payload size should be 31127 from the 16 bits field", [&] {
                require(header->payloadSize() == 2580);
                require(header->headerSize() == 8);
            });
        });
    });

    given("Header data", [&] {
        array<uint8_t, 14> data{0b10000001, 0b10000000 | 127, 10, 20, 30, 40, 50, 60, 70, 80, 0x10, 0x11, 0x12, 0x13};
        when("Casting to header", [&] {
            auto header = reinterpret_cast<const websocket::Header*>(data.data());
            then("Payload size should be 13 256 444 from the 64 bits field", [&] {
                require(header->payloadSize() == 726238597903828560);
                require(header->headerSize() == 14);
            });
        });
    });
}};

const Scenario webSocketFrame{"WebSocket frame encoding", [] {

  given("Some text to send", [&] {

    std::string text = "Hello WS";

when("Encoding it", [&] {
    websocket::Frame<Net> frame(text);
    then("A text frame is produced", [&] {
        require(frame.FIN() == true);
        require(frame.RSV1() == false);
        require(frame.RSV2() == false);
        require(frame.RSV3() == false);
        require(frame.opcode() == websocket::Header::Opcode::text);
        require(frame.MASK() == false);
        require(frame.payloadLenField() == 8);
        require(frame.payloadSize() == 8);
        require(frame.headerSize() == 2);

        auto buffers = frame.toBuffers();
        using Buffer = decltype(buffers[0]);
        auto compare = [](Buffer b, std::string s) {
            auto data = reinterpret_cast<const char*>(b.data());
            for (size_t index = 0; index < s.size(); ++index)
                if (data[index] != s[index]) return false;
            return true;
        };

        require(compare(buffers[1], "Hello WS"));
    });
});
});
}};
;

const Scenario webSocketFrames{"WebSocket frames can own payloads for asynchronous writes", [] {
    given("A frame assembled from temporary message buffers", [&] {
        array<byte, 2> header{byte{0x01}, byte{0x02}};
        array<byte, 3> payload{byte{0x03}, byte{0x04}, byte{0x05}};
        websocket::Frame<Net> frame(header);
        frame.addBuffer(payload);

        when("The frame is frozen before the source buffers change", [&] {
            frame.freeze();
            frame.freeze();
            header.fill(byte{0xff});
            payload.fill(byte{0xff});
            const auto buffers = frame.toBuffers();

            then("Repeated freezing keeps the encoded payload valid", [&] {
                const auto* encodedHeader = static_cast<const byte*>(buffers[1].data());
                const auto* encodedPayload = static_cast<const byte*>(buffers[2].data());
                require(buffers[1].size() == 2);
                require(encodedHeader[0] == byte{0x01});
                require(encodedHeader[1] == byte{0x02});
                require(buffers[2].size() == 3);
                require(encodedPayload[0] == byte{0x03});
                require(encodedPayload[1] == byte{0x04});
                require(encodedPayload[2] == byte{0x05});
            });
        });
    });
}};

const Scenario webSocketDecoder{"WebSocket decoder", [] {
    given("Some frame data and a decoder", [&] {
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
        when("All data is received in one unique chunk", [&] {
            require(decoder.parse(std::span(reinterpret_cast<const std::byte*>(frame.data()), frame.size())));
            auto bufferParser = std::cbegin(decoder.payload());
            then("Payload is correctly decoded", [&] {
                for (auto c : {'H', 'e', 'l', 'l', 'o', ' ', 'W', 'S'}) require(std::to_integer<uint8_t>(*bufferParser++) == c);
            });
        });

        when("Data is received in two chunks but the first has a complete header", [&] {
            require(decoder.parse(std::span(reinterpret_cast<const std::byte*>(frame.data()), 7)) == false);
            require(decoder.parse(std::span(reinterpret_cast<const std::byte*>(frame.data() + 7), frame.size() - 7)));
            auto bufferParser = std::cbegin(decoder.payload());
            then("Payload is correctly decoded", [&] {
                for (auto c : {'H', 'e', 'l', 'l', 'o', ' ', 'W', 'S'}) require(std::to_integer<uint8_t>(*bufferParser++) == c);
            });
        });

        when("Data is received in two chunks but the first has an incomplete header", [&] {
            require(decoder.parse(std::span(reinterpret_cast<const std::byte*>(frame.data()), 3)) == false);
            require(decoder.parse(std::span(reinterpret_cast<const std::byte*>(frame.data() + 3), frame.size() - 3)));
            auto bufferParser = std::cbegin(decoder.payload());
            then("Payload is correctly decoded", [&] {
                for (auto c : {'H', 'e', 'l', 'l', 'o', ' ', 'W', 'S'}) require(std::to_integer<uint8_t>(*bufferParser++) == c);
            });
        });
    });
}};

const Scenario webSocketDecoderPreserves{"WebSocket decoder preserves coalesced frames", [] {
    given("Two masked text frames received in one chunk", [&] {
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

        when("The first frame is decoded", [&] {
            require(decoder.parse(bytes));
            require(decoder.consumed() == 8);
            require(vector<byte>(decoder.payload().begin(), decoder.payload().end()) == vector{byte{'o'}, byte{'n'}});

            then("The unconsumed bytes decode as the second frame", [&] {
                const auto firstFrameSize = decoder.consumed();
                decoder.reset();
                require(decoder.parse(bytes.subspan(firstFrameSize)));
                require(decoder.consumed() == 8);
                require(vector<byte>(decoder.payload().begin(), decoder.payload().end()) == vector{byte{'o'}, byte{'k'}});
            });
        });
    });
}};

const Scenario webSocketHeader{"WebSocket header completeness follows its encoded length and mask", [] {
    given("An encoded WebSocket header", [&] {
        then("It becomes complete at exactly the header boundary", [&] {
            for (const auto lengthField : {125, 126, 127})
                for (const auto masked : {false, true}) {
                    auto              scope = info(lengthField, masked ? " masked" : "");
                    websocket::Header header;
                    header.raw[1]             = byte(lengthField | (masked ? 0x80 : 0));
                    const size_t expectedSize = (lengthField == 125 ? 2u : lengthField == 126 ? 4u : 10u) + (masked ? 4u : 0u);
                    require(!header.isComplete(expectedSize - 1));
                    require(header.isComplete(expectedSize));
                    require(header.isComplete(expectedSize + 1));
                }
        });
    });
}};

const Scenario shortWebSocket{"Short WebSocket headers do not interpret payload bytes as an extended length", [] {
    websocket::Header header;
    header.raw.fill(byte{0xff});
    header.raw[1] = byte{0x82};

    given("A short masked header with nonzero bytes after its length field", [&] {
        then("Only the inline length is used", [&] {
            require(header.extendedLenField() == 0);
            require(header.payloadSize() == 2);
            require(header.headerSize() == 6);
        });
    });
}};

const Scenario webSocketDecoderPreservesSplit{"WebSocket decoder preserves split extended headers and the following frame", [] {
    for (const size_t split : {1u, 2u, 7u, 8u, 9u, 13u})
        section("split after ", split, " bytes", [&] {
            // A masked 16-bit length frame followed by an empty masked text frame.
            vector<byte>       frames{byte{0x81}, byte{0xfe}, byte{0}, byte{126}, byte{0x10}, byte{0x11}, byte{0x12}, byte{0x13}};
            const vector<byte> expected(126, byte{'x'});
            const array        mask{byte{0x10}, byte{0x11}, byte{0x12}, byte{0x13}};
            for (size_t i = 0; i < expected.size(); ++i)
                frames.push_back(expected[i] ^ mask[i % mask.size()]);
            frames.insert(frames.end(), {byte{0x81}, byte{0x80}, byte{0}, byte{0}, byte{0}, byte{0}});
            websocket::FrameDecoder decoder;
            const auto              bytes = span<const byte>(frames);

            given("The first frame starts in one read and finishes alongside another frame", [&] {
                require(!decoder.parse(bytes.first(split)));
                require(decoder.consumed() == split);

                when("The rest of the stream arrives", [&] {
                    require(decoder.parse(bytes.subspan(split)));
                    require(decoder.consumed() == 134 - split);
                    require(vector<byte>(decoder.payload().begin(), decoder.payload().end()) == expected);
                    const auto nextFrameOffset = split + decoder.consumed();
                    decoder.reset();

                    then("The next frame has not been consumed as part of the payload", [&] {
                        require(decoder.parse(bytes.subspan(nextFrameOffset)));
                        require(decoder.consumed() == 6);
                        require(decoder.payload().empty());
                    });
                });
            });
        });
}};

const Scenario webSocketDecoderWaits{"WebSocket decoder waits for an entire split payload", [] {
    given("A masked text frame split across three reads", [&] {
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

        require(!decoder.parse(bytes.first(7)));
        require(decoder.consumed() == 7);

        // Six of the seven outstanding bytes arrive next. Comparing this chunk's size
        // against the remaining length after appending would report completion one byte early.
        when("The middle read still leaves payload bytes outstanding", [&] {
            require(!decoder.parse(bytes.subspan(7, 6)));
            require(decoder.consumed() == 6);

            then("Only the final read completes the frame", [&] {
                require(decoder.parse(bytes.subspan(13)));
                require(decoder.consumed() == 1);
                require(decoder.payload().size() == 8);
            });
        });
    });
}};

const Scenario webSocketStop{"WebSocket stop() is idempotent", [] {
    InjectableSocket::reset();
    int  closeHandlerCalls = 0;
    webfront::testing::InlineTaskScope scope;
    auto                               ws = websocket::WebSocket<InjectableNetworking>::create(InjectableSocket{}, scope);
    ws->onClose([&closeHandlerCalls](websocket::CloseEvent) { ++closeHandlerCalls; });

    given("a started WebSocket that is stopped explicitly while a read is pending", [&] {
        ws->start();
        ws->stop();

        when("the still-pending read then completes with an error", [&] {
            InjectableSocket::fail(make_error_code(errc::bad_file_descriptor));

            then("the error is treated as harmless: no duplicate close handler call", [&] {
                require(closeHandlerCalls == 0);
            });
        });

        when("stop() is called again", [&] {
            then("the second call has no effect", [&] { requireNothrow([&] { ws->stop(); }); });
        });
    });

    given("a started WebSocket whose pending read fails with no prior explicit stop", [&] {
        ws->start();

        when("the read completes with an error", [&] {
            InjectableSocket::fail(make_error_code(errc::bad_file_descriptor));

            then("the close handler fires exactly once", [&] { require(closeHandlerCalls == 1); });
        });
    });
}};

const Scenario webSocketFramesEncode{"WebSocket frames encode extended payload lengths big-endian", [] {
    struct Expected {
        size_t   size;
        unsigned lengthField;
        size_t   headerSize;
    };
    static constexpr std::array<Expected, 4> cases{{{125, 125, 2}, {126, 126, 4}, {65535, 126, 4}, {65536, 127, 10}}};
    for (const auto& expected : cases) {
        const std::string     text(expected.size, 'x');
        websocket::Frame<Net> frame(text);
        check(frame.payloadLenField() == expected.lengthField);
        check(frame.payloadSize() == expected.size);
        check(frame.headerSize() == expected.headerSize);
    }
}};
