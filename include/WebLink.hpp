/// @date 31/01/2022 21:19:42
/// @author Ambroise Leclerc
/// @brief WebLink represents a connexion to an HTML/JS renderer
#pragma once
#include "tooling/Logger.hpp"
#include "http/WebSocket.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace webfront {

enum class CodedType : uint8_t {
    undefined,
    booleanTrue,  // opcode if boolean is true
    booleanFalse, // opcode if boolean is false
    number,       // opcode + 8 bytes IEEE754 floating point number
    smallString,  // opcode + 1 byte size
    string,       // opcode + 2 bytes size
};

std::string_view toString(CodedType t) {
    switch (t) {
    case CodedType::booleanTrue: return "boolean (true)";
    case CodedType::booleanFalse: return "boolean (false)";
    case CodedType::number: return "number";
    case CodedType::smallString: return "smallString";
    case CodedType::string: return "string";
    default: return "undefined";
    }
}


using WebLinkId = uint16_t;

struct WebLinkEvent {
    enum class Code { linked, closed };
 
    WebLinkEvent(Code eventCode, WebLinkId id) : code(eventCode), webLinkId(id) {}
    Code code;
    WebLinkId webLinkId;
};

enum class JSEndian : uint8_t { little = 0, big = 1, mixed = little + big };
enum class TxtOpcode : uint8_t { debugLog, injectScript };

enum class Command : uint8_t {
        handshake,
        ack,
        textCommand,
        callFunction,       // 1:Command  1:ParamsCount 2:padding  4:ParamsDataSize
        functionReturn,     // 1:Command  1:ParamsCount 2:padding  4:ParamsDataSize
};

namespace msg {

struct Ack {
    std::span<const std::byte> header() { return std::span(reinterpret_cast<const std::byte*>(&head), sizeof(Header)); }
    std::span<const std::byte> payload() { return {}; }

private:
    struct Header {
        Command command = Command::ack;
        JSEndian endian = std::endian::native == std::endian::little ? JSEndian::little : JSEndian::big;
    } head;
    static_assert(sizeof(Header) == 2, "Ack header has to be 2 bytes long");

};

/// Commands with a text payload
struct TextCommand {
    TextCommand(TxtOpcode opcode, std::string_view text)
        : head{.opcode = opcode, .lengthHi = static_cast<uint8_t>(text.size() >> 8), .lengthLo = static_cast<uint8_t>(text.size())},
          payloadSpan{reinterpret_cast<const std::byte*>(text.data()), text.size()} {}

    std::span<const std::byte> header() { return {reinterpret_cast<const std::byte*>(&head), sizeof(Header)}; }
    std::span<const std::byte> payload() { return payloadSpan; }
private:
    struct Header {
        Command command = Command::textCommand;
        TxtOpcode opcode;
        uint8_t lengthHi, lengthLo; // Payload length = (256 * lengthHi) + lengthLo
    } head;
    static_assert(sizeof(Header) == 4, "TextCommand header has to be 4 bytes long");

    std::span<const std::byte> payloadSpan;
};

struct FunctionCall {
    void setParametersCount(uint8_t parametersCount) { head.parametersCount = parametersCount; }
    void setParametersDataSize(uint32_t size) { head.parametersDataSize = size; }
    uint8_t getParametersCount() const { return head.parametersCount; }
    size_t getParametersDataSize() const { return head.parametersDataSize; }

    std::span<const std::byte> header() const { return { reinterpret_cast<const std::byte*>(&head), sizeof(Header) }; }
    std::span<const std::byte> payload() const { return { &header()[sizeof(Header)], getParametersDataSize() }; }
private:
    struct Header {
        Command command = Command::callFunction;
        uint8_t parametersCount = 0;
        std::array<uint8_t, 2> padding {};
        uint32_t parametersDataSize = 0;
    } head;
    static_assert(sizeof(Header) == 8, "FunctionCall header has to be 8 bytes long");
};

struct FunctionReturn {
    std::span<const std::byte> header() { return { reinterpret_cast<const std::byte*>(&head), sizeof(Header)}; }
private:
    struct Header {
        Command command = Command::functionReturn;
        uint8_t parametersCount = 0;
        std::array<uint8_t, 2> padding {};
        uint32_t parametersDataSize = 0;
    } head;
    static_assert(sizeof(Header) == 8, "FunctionReturn header has to be 8 bytes long");
};

} // namespace msg

template<typename Net>
class WebLink {
    websocket::WebSocket<Net> ws;
    WebLinkId id;
    bool sameEndian;
    std::optional<size_t> logSink;
    std::function<void(WebLinkEvent)> eventsHandler;

public:
    WebLink(typename Net::Socket&& socket, WebLinkId webLinkId, std::function<void(WebLinkEvent)> eventHandler)
        : ws(std::move(socket)), id(webLinkId), eventsHandler(eventHandler) {
        log::debug("New WebLink created with id:{}", id);

        ws.onMessage([this](std::string_view text) {
            log::debug("onMessage(text) :{}", text);
            ws.write("This is my response");
        });
        ws.onMessage([this](std::span<const std::byte> data) {
            switch (static_cast<Command>(data[0])) {
            case Command::handshake:
                sendCommand(msg::Ack{});
                logSink = log::addSinks([this](std::string_view t) { sendCommand(msg::TextCommand(TxtOpcode::debugLog, t)); });
                sameEndian = (std::endian::native == std::endian::little && static_cast<JSEndian>(data[1]) == JSEndian::little) ||
                             (std::endian::native == std::endian::big && static_cast<JSEndian>(data[1]) == JSEndian::big);
                log::info("Endianness - platform:{}, client:{}, identical:{}", std::endian::native == std::endian::little ? "little" : "big",
                          static_cast<JSEndian>(data[1]) == JSEndian::little ? "little" : "big", sameEndian);
                eventsHandler(WebLinkEvent(WebLinkEvent::Code::linked, id));
                break;

            case Command::callFunction: {
                log::info("Function called !");
                auto command = reinterpret_cast<const msg::FunctionCall*>(data.data());
                if ((command->header().size() + command->payload().size()) > data.size())
                    throw std::runtime_error("Erroneous callFunction message received : data size mismatch");
                
                callCppFunction(command->getParametersCount(), command->payload());

            } break; 

            default: break;
            }
            log::infoHex("onMessage(binary) :", data);
        });

        ws.start();
    }
    WebLink(const WebLink&) = delete;
    WebLink(WebLink&&) = delete;
    WebLink& operator=(const WebLink&) = delete;
    WebLink& operator=(WebLink&&) = delete;
    

    ~WebLink() {
        log::debug("WebLink destructor");
        if (logSink) log::removeSinks(logSink.value());
    }

    void sendCommand(auto message) { ws.write(message.header(), message.payload()); }
    void sendFrame(websocket::Frame<Net> frame) { ws.write(std::move(frame)); }

private:

    void callCppFunction(size_t parametersCount, std::span<const std::byte> data) {
        if (parametersCount > 0) {
            size_t offset = 0;
            auto[functionName, bytesDecoded] = decodeParameter<std::string>(data.subspan(offset));
            offset += bytesDecoded;
            log::debug("Function called : {}", functionName);
        }
    }

    template<typename T>
    std::tuple<T, size_t> decodeParameter(std::span<const std::byte> data) {
        if (data.size() == 0) throw std::runtime_error("Not enough data for WebLink::decodeParameter");
        auto codedType = static_cast<CodedType>(data[0]);
        switch (codedType) {
            case CodedType::smallString:
                if constexpr (std::is_same_v<T, std::string>) {
                    if (data.size() < 1) throw std::runtime_error("Erroneous data feeded to WebLink::decodeParameter");
                    auto size = static_cast<size_t>(data[1]);
                    if (data.size() < 2 + size) throw std::runtime_error("Erroneous data feeded to WebLink::decodeParameter");
                    return { std::string(reinterpret_cast<const char*>(&data[2]), size), 2 + size };
                }

                break;
            default:
                return {{}, 0};
        }

    }
};

} // namespace webfront
