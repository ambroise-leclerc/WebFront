/// @date 25/01/2022 16:00:55
/// @author Ambroise Leclerc
/// @brief Some networking classes which provide common access to C++2x NetworkingTS, Mock networking or internal implementation
#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#  include <type_traits>
namespace std {
template <typename T, typename... Args> concept constructible_from = destructible<T> && is_constructible_v<T, Args...>;
template <typename T,typename U>
concept convertible_to = is_convertible_v<T, U> && requires(add_rvalue_reference_t<T> (&t)()) { static_cast<U>(t()); };
template <class T> concept swappable = requires(T& t, T& t2) { swap(t, t2); };
template <class T> concept move_constructible = constructible_from<T, T> && convertible_to<T, T>;
template < class T, class U > concept common_reference_with = same_as<common_reference_t<T, U>, common_reference_t<U, T>> &&
    convertible_to<T, common_reference_t<T, U>> && convertible_to<U, common_reference_t<T, U>>;
template <typename T, typename U> concept assignable_from = is_lvalue_reference_v<T>
    && common_reference_with<const remove_reference_t<T>&, const remove_reference_t<T>&>
    && requires(T t, U u) { { t = static_cast<T&&>(u) } -> same_as<T>; };

template <class T> concept movable = is_object_v<T> && move_constructible<T> && assignable_from<T&, T> && swappable<T>;
}
#endif


#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <vector>
#include <array>
#include <chrono>
#include <string>
#include <tuple>
#include <experimental/net>
#include <algorithm>
#include <concepts>
#include <iomanip>
#include <ostream>
#include <span>
#include <sstream>
#include <vector>
#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <set>
#include <span>
#include <iostream>
#include <array>
#include <bit>
#include <concepts>
#include <span>
#include <string>
#include <string_view>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <locale>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <vector>
#include <iostream>
#include <bit>
#include <string_view>

namespace webfront {
namespace networking {
template<typename T>
concept Features = requires {
    typename T::Acceptor;
    typename T::Endpoint;
    typename T::IoContext;
    typename T::Resolver;
    typename T::Socket;
    typename T::ConstBuffer;
    typename T::MutableBuffer;
};

namespace buffers {
    class MutableBuffer {
    public:
        MutableBuffer() noexcept : bufData(0), bufSize(0) {}
        MutableBuffer(void* data, std::size_t size) noexcept : bufData(data), bufSize(size) {}
        void* data() const noexcept { return bufData; }
        size_t size() const noexcept { return bufSize; }
        MutableBuffer& operator+=(size_t n) noexcept {
            size_t offset = n < bufSize ? n : bufSize;
            bufData = static_cast<uint8_t*>(bufData) + offset;
            bufSize -= offset;
            return *this;
        }

    private:
        void* bufData;
        std::size_t bufSize;
    };

    class ConstBuffer {
    public:
        ConstBuffer() noexcept : bufData(0), bufSize(0) {}
        ConstBuffer(const void* data, std::size_t size) noexcept : bufData(data), bufSize(size) {}
        ConstBuffer(const MutableBuffer& b) : bufData(b.data()), bufSize(b.size()) {}
        const void* data() const noexcept { return bufData; }
        size_t size() const noexcept { return bufSize; }
        ConstBuffer& operator+=(size_t n) noexcept {
            size_t offset = n < bufSize ? n : bufSize;
            bufData = static_cast<const uint8_t*>(bufData) + offset;
            bufSize -= offset;
            return *this;
        }

    private:
        const void* bufData;
        std::size_t bufSize;
    };
} // namespace buffers

template<typename ConstBufferT = buffers::ConstBuffer, typename MutableBufferT = buffers::MutableBuffer>
class BasicNetworking {
public:
    using ConstBuffer = ConstBufferT;
    using MutableBuffer = MutableBufferT;

    static MutableBuffer Buffer(void* d, size_t s) noexcept { return {d, s}; }
    static ConstBuffer Buffer(const void* d, size_t s) noexcept { return {d, s}; }
    static MutableBuffer Buffer(const MutableBuffer& b) noexcept { return b; }
    static MutableBuffer Buffer(const MutableBuffer& b, size_t s) noexcept { return {b.data(), std::min(b.size(), s)}; }
    static ConstBuffer Buffer(const ConstBuffer& b) noexcept { return b; }
    static ConstBuffer Buffer(const ConstBuffer& b, size_t s) noexcept { return {b.data(), std::min(b.size(), s)}; }
    template<typename T, size_t S>
    static MutableBuffer Buffer(T (&d)[S]) noexcept {
        return ToMutableBuffer(d, S);
    }
    template<typename T, size_t S>
    static ConstBuffer Buffer(const T (&d)[S]) noexcept {
        return ToConstBuffer(d, S);
    }
    template<typename T, size_t S>
    static MutableBuffer Buffer(std::array<T, S>& d) noexcept {
        return ToMutableBuffer(d.data(), S);
    }
    template<typename T, size_t S>
    static ConstBuffer Buffer(std::array<const T, S>& d) noexcept {
        return ToConstBuffer(d.data(), d.size());
    }
    template<typename T, typename Alloc>
    static MutableBuffer Buffer(std::vector<T, Alloc>& d) noexcept {
        return ToMutableBuffer(d.data(), d.size());
    }
    template<typename T, typename Alloc>
    static ConstBuffer Buffer(const std::vector<T, Alloc>& d) noexcept {
        return ToConstBuffer(d.data(), d.size());
    }

    template<typename CharT, typename Traits, typename Alloc>
    static MutableBuffer Buffer(std::basic_string<CharT, Traits, Alloc>& d) noexcept {
        return ToMutableBuffer(&d.front(), d.size());
    }

    template<typename CharT, typename Traits, typename Alloc>
    static ConstBuffer Buffer(const std::basic_string<CharT, Traits, Alloc>& d) noexcept {
        return ToConstBuffer(&d.front(), d.size());
    }

    template<typename CharT, typename Traits>
    static ConstBuffer Buffer(std::basic_string_view<CharT, Traits> d) noexcept {
        return ToConstBuffer(d.data(), d.size());
    }

    template<typename T, size_t S>
    static MutableBuffer Buffer(T (&d)[S], size_t s) noexcept {
        return Buffer(Buffer(d), s * sizeof(T));
    }

    template<typename T, size_t S>
    static ConstBuffer Buffer(const T (&d)[S], size_t s) noexcept {
        return Buffer(Buffer(d), s * sizeof(T));
    }

    template<typename T, size_t S>
    static MutableBuffer Buffer(std::array<T, S>& d, size_t s) noexcept {
        return Buffer(Buffer(d), s * sizeof(T));
    }

    template<typename T, size_t S>
    static ConstBuffer Buffer(std::array<const T, S>& d, size_t s) noexcept {
        return Buffer(Buffer(d), s * sizeof(T));
    }

    template<typename T, size_t S>
    static ConstBuffer Buffer(const std::array<T, S>& d, size_t s) noexcept {
        return Buffer(Buffer(d), s * sizeof(T));
    }

    template<typename T, typename Alloc>
    static MutableBuffer Buffer(std::vector<T, Alloc>& d, size_t s) noexcept {
        return Buffer(Buffer(d), s * sizeof(T));
    }

    template<typename T, typename Alloc>
    static ConstBuffer Buffer(const std::vector<T, Alloc>& d, size_t s) noexcept {
        return Buffer(Buffer(d), s * sizeof(T));
    }

    template<typename CharT, typename Traits, typename Alloc>
    static MutableBuffer Buffer(std::basic_string<CharT, Traits, Alloc>& d, size_t s) noexcept {
        return Buffer(Buffer(d), s * sizeof(CharT));
    }

    template<typename CharT, typename Traits, typename Alloc>
    static ConstBuffer Buffer(const std::basic_string<CharT, Traits, Alloc>& d, size_t s) noexcept {
        return Buffer(Buffer(d), s * sizeof(CharT));
    }

    template<typename CharT, typename Traits>
    static ConstBuffer Buffer(std::basic_string_view<CharT, Traits> d, size_t s) noexcept {
        return Buffer(Buffer(d), s * sizeof(CharT));
    }

private:
    template<typename T>
    static MutableBuffer ToMutableBuffer(T* data, size_t s) {
        return {s ? data : nullptr, s * sizeof(T)};
    }
    template<typename T>
    static ConstBuffer ToConstBuffer(const T* data, size_t s) {
        return {s ? data : nullptr, s * sizeof(T)};
    }

protected:
    using super = BasicNetworking;
};



} // namespace networking
} // namespace webfront



namespace webfront {
namespace networking {

namespace {
/// OS abstration layer for socket libraries
class SocketLibOS {
public:
#ifdef _WIN32
    static bool initializeLib() noexcept {
        WSADATA wsa_data;
        int init_value = WSAStartup(MAKEWORD(2, 2), &wsa_data);
        return init_value == 0;
    }

    static void cleanupLib() noexcept { WSACleanup(); }

protected:
    using SocketType = SOCKET;
    using SockLen = int;
    static const SocketType invalidSocket = INVALID_SOCKET;
    static const int socketError = SOCKET_ERROR;
    enum class Shutdown { Read = SD_SEND, Write = SD_RECEIVE, ReadWrite = SD_BOTH };
    enum class Error { Timeout = WSAETIMEDOUT, WouldBlock = WSAEWOULDBLOCK };

    static bool closeSocket(SocketType socket) noexcept { return closesocket(socket) != socketError; }
    static auto getLastError() { return WSAGetLastError(); }
    static std::string getErrorText(bool prependErrorId = true) { return getErrorText(getLastError(), prependErrorId); }
    static std::string getErrorText(int error, bool prependErrorId = true) {
        LPVOID lpMsgBuf;
        auto bufLen = FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
                                    reinterpret_cast<LPTSTR>(&lpMsgBuf), 0, nullptr);
        if (bufLen) {
            auto lpMsgStr = reinterpret_cast<LPCSTR>(lpMsgBuf);
            std::string result(lpMsgStr, lpMsgStr + bufLen);
            LocalFree(lpMsgBuf);
            if (prependErrorId) { result.insert(0, std::to_string(error) + ":"); }
            return result;
        }
        return std::string();
    }

    static constexpr auto toTimeout(const std::chrono::microseconds value) noexcept { return static_cast<long>(value.count() / 1000); }
};
#else
    static bool initializeLib() noexcept { return true; }
    static void cleanupLib() noexcept {}

    static std::string getErrorText(bool prependErrorId = true) { return getErrorText(getLastError(), prependErrorId); }
    static std::string getErrorText(int error, bool prependErrorId = true) {
        auto errorText = strerror(error);
        std::string result(errorText ? errorText : "");
        if (prependErrorId) { result.insert(0, std::to_string(error) + ":"); }
        return result;
    }

    static constexpr auto toTimeout(std::chrono::microseconds uSec) noexcept {
        timeval timeoutTimeValue{static_cast<decltype(timeval::tv_sec)>(uSec.count() / 1000000u),
                                 static_cast<decltype(timeval::tv_usec)>(uSec.count() % 1000000u)};

        return timeoutTimeValue;
    }

protected:
    using SocketType = int;
    using SockLen = socklen_t;
    static const SocketType invalidSocket = -1;
    static const int socketError = -1;

    enum class Shutdown { Read = SHUT_RD, Write = SHUT_WR, ReadWrite = SHUT_RDWR };
    enum class Error { Timeout = EAGAIN, WouldBlock = EWOULDBLOCK };

    static bool closeSocket(SocketType socket) { return ::close(socket) != socketError; }
    static int getLastError() { return errno; }
#endif
};


class TCPSockets : public BasicNetworking<> {
public:
    using super::ConstBuffer;
    using super::MutableBuffer;

};

} // namespace networking
} // namespace webfront/// @date 26/01/2022 11:38:00



namespace webfront {
namespace networking {

class TCPNetworkingTS : public BasicNetworking<std::experimental::net::const_buffer, std::experimental::net::mutable_buffer> {
public:
    using Acceptor = std::experimental::net::ip::tcp::acceptor;
    using Endpoint = std::experimental::net::ip::tcp::endpoint;
    using IoContext = std::experimental::net::io_context;
    using Resolver = std::experimental::net::ip::tcp::resolver;
    using Socket = std::experimental::net::ip::tcp::socket;
    using super::ConstBuffer;
    using super::MutableBuffer;

    template<typename... Args>
    static auto AsyncWrite(Args&&... args) -> decltype(std::experimental::net::async_write(std::forward<Args>(args)...)) {
        return std::experimental::net::async_write(std::forward<Args>(args)...);
    }

    template<typename... Args>
    static auto Write(Args&&... args) -> decltype(std::experimental::net::write(std::forward<Args>(args)...)) {
        return std::experimental::net::write(std::forward<Args>(args)...);
    }

    struct Error {
        static inline auto OperationAborted = std::experimental::net::error::operation_aborted;
    };
};

} // namespace networking
} // namespace webfront


namespace webfront {
namespace networking {

class NetworkingMock : public BasicNetworking<> {
public:
    using super::ConstBuffer;
    using super::MutableBuffer;

    template<typename... Args>
    static auto AsyncWrite(Args&&... /*args*/) -> void {
        return;
    }
};

} // namespace networking
} // namespace webfront/// @file Serializer.hpp
/// @date 22/01/2022 16:00:55
/// @author Ambroise Leclerc
/// @brief A serializer-deserializer for webfront exchanges over a websocket
#pragma once

namespace webfront {


} // namespace webfront/// @file HexDump.hpp




namespace webfront {
namespace utils {

template<typename T>
concept Buffer = std::movable<T> || requires(T t) {
    t.data();
    t.size();
    T::value_type;
};

/// Provides an hexadecimal dump of a container or a buffer
template<Buffer BufferType>
struct HexDump {
    HexDump(const BufferType& buf, size_t startAddr = 0) : buffer(buf), startAddress(startAddr) {}

    const BufferType& buffer;
    size_t startAddress;
};

template<typename Container>
std::ostream& operator<<(std::ostream& os, const HexDump<Container>& hexDump) {
    using namespace std;
    size_t address = 0;
    os << hex << setfill('0');
    auto buffer = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(hexDump.buffer.data()),
                                           hexDump.buffer.size() * sizeof(typename pointer_traits<typename Container::pointer>::element_type));
    auto size = size_t(buffer.size());
    while (address < size) {
        os << setw(8) << address + hexDump.startAddress;

        for (auto index = address; index < address + 16; ++index) {
            if (index % 8 == 0) os << ' ';
            if (index < size)
                os << ' ' << setw(2) << +buffer[index];
            else
                os << "   ";
        }

        os << "  ";
        for (auto index = address; index < address + 16; ++index) {
            if (index < size) {
                auto car = buffer[index];
                os << (car < 32 ? '.' : index < size ? car : '.');
            }
        }

        os << "\n";
        address += 16;
    }
    os << resetiosflags(ios_base::basefield | ios_base::adjustfield);
    return os;
}

/*

/// Generates an hexadecimal dump string of a given buffer
///
/// @param   buffer
/// @param   startAddress optional offset for displayed adresses
///
/// @return  a multi-lines string with the dumped data
static std::string hexDump(const Buffer auto& buffer, size_t startAddress = 0) {
    std::stringstream ss;
    ss << HexDump<decltype(buffer)>(buffer, startAddress);

    return ss.str();
}

/// Generates an hexadecimal dump string of two buffers as if they were contiguous
///
/// @param   buffer1
/// @param   buffer2
///
/// @return  a multi-lines string with the dumped data
static std::string hexDump(const Buffer auto& buffer1, const Buffer auto& buffer2) {
    std::vector<uint8_t> buffer;
    buffer.resize(buffer1.size() + buffer2.size());
    std::copy_n(buffer1.data(), buffer1.size(), buffer.data());
    std::copy_n(buffer2.data(), buffer2.size(), buffer.data() + buffer1.size());

    std::stringstream ss;
    ss << HexDump<decltype(buffer)>(buffer);

    return ss.str();
}
*/
} // namespace utils
} // namespace webfront/// @date 16/01/2022 22:27:42



namespace webfront {

template<typename ConnectionType>
class Connections {
public:
    Connections(const Connections&) = delete;
    Connections& operator=(const Connections&) = delete;
    Connections() = default;

    void start(std::shared_ptr<ConnectionType> connection) {
        connections.insert(connection);
        connection->start();
    }

    void stop(std::shared_ptr<ConnectionType> connection) {
        connections.erase(connection);
        connection->stop();
    }

    void stopAll() {
        for (auto connection : connections) connection->stop();
        connections.clear();
    }

private:
    std::set<std::shared_ptr<ConnectionType>> connections;
};

namespace websocket {
using Handle = uint32_t;

struct Header {
    std::array<std::byte, 14> raw;
    Header() : raw{} {}

    enum class Opcode {
        continuation,
        text,
        binary,
        reserved1,
        reserved2,
        reserved3,
        reserved4,
        reserved5,
        connectionClose,
        ping,
        pong,
        ctrl1,
        ctrl2,
        ctrl3,
        ctrl4,
        ctrl5
    };

    bool FIN() const { return test(0, 7); }
    bool RSV1() const { return test(0, 6); }
    bool RSV2() const { return test(0, 5); }
    bool RSV3() const { return test(0, 4); }
    Opcode opcode() const { return static_cast<Opcode>(std::to_integer<uint8_t>(raw[0] & std::byte(0b1111))); }
    bool MASK() const { return test(1, 7); }
    uint8_t payloadLenField() const { return std::to_integer<uint8_t>(raw[1] & std::byte(0b1111111)); }
    uint64_t extendedLenField() const {
        auto s = [this](size_t i, uint8_t shift = 0) constexpr { return std::to_integer<uint64_t>(raw[i]) << shift; };
        return payloadLenField() == 126 ? s(2, 8) | s(3) : s(2, 56) | s(3, 48) | s(4, 40) | s(5, 32) | s(6, 24) | s(7, 16) | s(8, 8) | s(9, 0);
    }

    size_t headerSize() const {
        if (payloadLenField() < 126) return MASK() ? 6 : 2;
        if (payloadLenField() == 126) return MASK() ? 8 : 4;
        return MASK() ? 14 : 10;
    }

    std::array<std::byte, 4> maskingKey() const {
        if (!MASK()) return {};
        auto index = headerSize() - 4;
        return {raw[index], raw[index + 1], raw[index + 2], raw[index + 3]};
    }

    uint64_t payloadSize() const {
        auto len = payloadLenField();
        return len < 126 ? len : extendedLenField();
    }

    uint64_t getFrameSize() const { return payloadSize() + headerSize(); }

    void dump() const {
        using namespace std;
        cout << "FIN  : " << FIN() << "\n";
        cout << "RSV1 : " << RSV1() << "\n";
        cout << "RSV2 : " << RSV2() << "\n";
        cout << "RSV3 : " << RSV3() << "\n";
        cout << "opcode : " << static_cast<int>(opcode()) << "\n";
        cout << "MASK : " << MASK() << "\n";
        cout << "payloadLenField : " << +payloadLenField() << "\n";
        cout << "extendedLenField : " << extendedLenField() << "\n";
        auto k = maskingKey();
        cout << "maskingKey : 0x" << hex << to_integer<int>(k[0]) << to_integer<int>(k[1]) << to_integer<int>(k[2]) << to_integer<int>(k[3]) << dec << "\n";
        cout << "getPayloadSize : " << payloadSize() << "\n";
        cout << "headerSize : " << headerSize() << "\n";
    }

    /// @return true if first 'size' bytes constitute a complete header
    bool isComplete(size_t size) const {
        if (size < 2) return false;
        return std::to_integer<uint8_t>(raw[1] & std::byte(0b1111111)) < 126 ? size >= 6 : size >= 14;
    }

    void setFIN(bool set) {
        if (set)
            raw[0] |= std::byte(1 << 7);
        else
            raw[0] &= std::byte(0b1111111);
    }
    void setOpcode(Opcode code) {
        raw[0] &= std::byte(0b11110000);
        raw[0] |= static_cast<std::byte>(code);
    }
    void setPayloadSize(size_t size) {
        raw[1] &= std::byte(0b10000000);
        auto f = [&size, this ](size_t i, uint8_t shift = 0) constexpr { return raw[i] = static_cast<std::byte>(size >> shift); };
        if (size < 126)
            raw[1] |= static_cast<std::byte>(size);
        else if (size < 65536) {
            raw[1] |= std::byte(126);
            f(2, 8);
            f(3);
        }
        else {
            raw[1] |= std::byte(127);
            f(2, 56);
            f(3, 48);
            f(4, 40);
            f(5, 32);
            f(6, 24);
            f(7, 16);
            f(8, 8);
            f(9);
        }
    }

protected:
    bool test(size_t index, uint8_t bit) const { return std::to_integer<bool>(raw[index] & std::byte(1 << bit)); }
};

struct CloseEvent {
    uint16_t status;
    std::string reason;
};

struct Frame : public Header {
    Frame(std::string_view text) {
        setFIN(true);
        setOpcode(Opcode::text);
        setPayloadSize(text.size());
        dataSpan = std::span(reinterpret_cast<const std::byte*>(text.data()), text.size());
    }
    size_t size() const { return payloadSize(); };
    const std::byte* data() const { return reinterpret_cast<const std::byte*>(this + headerSize()); }

    template<typename Net>
    std::vector<typename Net::ConstBuffer> toBuffers() const {
        std::vector<typename Net::ConstBuffer> buffers;
        buffers.push_back(typename Net::ConstBuffer(raw.data(), headerSize()));
        buffers.push_back(typename Net::ConstBuffer(dataSpan.data(), dataSpan.size()));
        return buffers;
    }

    std::span<const std::byte> dataSpan;
};

class FrameDecoder {
public:
    Header::Opcode frameType;

public:
    FrameDecoder() : payloadBuffer(sizeof(Header)) { reset(); }
    std::span<const std::byte> payload() const { return std::span(payloadBuffer.cbegin(), payloadSize); }

    // Parses some incoming data and tries to decode it.
    // @return true if the frame is complete, false if it needs more data
    bool parse(std::span<const std::byte> buffer) {
        auto decodePayload = [&](std::span<const std::byte> encoded) -> size_t {
            for (auto in : encoded) payloadBuffer.push_back(in ^ mask[maskIndex++ % 4]);
            return payloadBuffer.size();
        };
        auto decodeHeader = [&](auto input) {
            payloadSize = reinterpret_cast<const Header*>(input)->payloadSize();
            headerSize = reinterpret_cast<const Header*>(input)->headerSize();
            mask = reinterpret_cast<const Header*>(input)->maskingKey();
            frameType = reinterpret_cast<const Header*>(input)->opcode();
            payloadBuffer.reserve(payloadSize);
        };
        auto bufferizeHeaderData = [&](std::span<const std::byte> input) -> size_t {
            for (size_t index = 0; index < input.size(); ++index) {
                headerBuffer.raw[headerBufferParser++] = *(input.data() + index);
                if (headerBuffer.isComplete(headerBufferParser)) return index + 1;
            }
            return input.size();
        };

        switch (state) {
        case DecodingState::starting:
            if (reinterpret_cast<const Header*>(buffer.data())->isComplete(buffer.size())) {
                decodeHeader(buffer.data());
                if (decodePayload(buffer.subspan(headerSize, std::min(buffer.size() - headerSize, payloadSize))) == payloadSize) return true;
                state = DecodingState::decodingPayload;
            }
            else {
                bufferizeHeaderData(buffer);
                state = DecodingState::partialHeader;
            }
            break;
        case DecodingState::partialHeader: {
            auto consumedData = bufferizeHeaderData(buffer);
            if (headerBuffer.isComplete(headerBufferParser)) {
                decodeHeader(headerBuffer.raw.data());
                if (decodePayload(buffer.subspan(consumedData, std::min(buffer.size() - consumedData, payloadSize))) == payloadSize) return true;
                state = DecodingState::decodingPayload;
            }
        } break;
        case DecodingState::decodingPayload: {
            decodePayload(buffer.first(std::min(payloadSize - payloadBuffer.size(), buffer.size())));
            return (buffer.size() >= (payloadSize - payloadBuffer.size()));
        }
        }
        return false;
    }

    void reset() {
        maskIndex = 0;
        headerBufferParser = 0;
        payloadBuffer.clear();
        state = DecodingState::starting;
    }

private:
    enum class DecodingState { starting, partialHeader, decodingPayload } state;
    std::vector<std::byte> payloadBuffer;
    Header headerBuffer;
    size_t headerBufferParser;
    size_t payloadSize, headerSize;
    std::array<std::byte, 4> mask;
    uint8_t maskIndex;
};

template<typename Net>
class WebSocket : public std::enable_shared_from_this<WebSocket<Net>> {
    typename Net::Socket socket;
    Connections<WebSocket>& webSockets;

public:
    explicit WebSocket(typename Net::Socket netSocket, Connections<WebSocket>& connections) : socket(std::move(netSocket)), webSockets(connections) {}
    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;

    void start() { read(); }
    void stop() { socket.close(); }

    void onMessage(std::function<void(std::string_view)> handler) { textHandler = std::move(handler); }
    void onMessage(std::function<void(std::span<const std::byte>)> handler) { binaryHandler = std::move(handler); }
    void onClose(std::function<void(CloseEvent)> handler) { closeHandler = std::move(handler); }
    void write(std::string_view text) {
        Frame frame(text);
        auto self(this->shared_from_this());
        std::error_code ec;
        Net::Write(socket, frame.toBuffers<Net>(), ec);
        if (ec) {
            std::clog << "Error during write : ec.value() = " << ec.value() << "\n";
            webSockets.stop(self);
        }
    }

private:
    std::array<std::byte, 8192> readBuffer;
    FrameDecoder decoder;
    std::function<void(std::string_view)> textHandler;
    std::function<void(std::span<const std::byte>)> binaryHandler;
    std::function<void(CloseEvent)> closeHandler;

private:
    void read() {
        auto self(this->shared_from_this());
        socket.async_read_some(Net::Buffer(readBuffer), [this, self](std::error_code ec, std::size_t bytesTransferred) {
            if (!ec) {
                std::cout << "Received " << bytesTransferred << " bytes\n" << utils::HexDump(std::span(readBuffer.data(), bytesTransferred)) << "\n";
                if (decoder.parse(std::span(readBuffer.data(), bytesTransferred))) {
                    auto data = decoder.payload();
                    switch (decoder.frameType) {
                    case Header::Opcode::text:
                        if (textHandler) textHandler(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
                        break;
                    case Header::Opcode::binary:
                        if (binaryHandler) binaryHandler(data);
                        break;
                    case Header::Opcode::connectionClose: webSockets.stop(self); break;
                    default: std::cout << "Unhandled frameType";
                    };
                    decoder.reset();
                }
                read();
            }
            else {
                std::clog << "Error in websocket::read() : " << ec << "\n";
                webSockets.stop(self);
            }
        });
    }
};

struct WSManagerConfigurationError : std::runtime_error {
    WSManagerConfigurationError() : std::runtime_error("WSManager handler configuration error") {}
};

template<typename Net>
class WSManager {
    Connections<WebSocket<Net>> webSockets;
    std::function<void(std::shared_ptr<WebSocket<Net>>)> openHandler;

public:
    void onOpen(std::function<void(std::shared_ptr<WebSocket<Net>>)> handler) { openHandler = std::move(handler); }

public:
    void createWebSocket(typename Net::Socket socket) {
        if (!openHandler) throw WSManagerConfigurationError();

        auto ws = std::make_shared<WebSocket<Net>>(std::move(socket), webSockets);
        openHandler(ws);
        webSockets.start(ws);
    };
};

} // namespace websocket
} // namespace webfront/// @file Encodings.hpp


namespace webfront {
namespace uri {

inline std::string encode(std::string_view uri) {
    std::string encoded;
    static const char* digits = "0123456789ABCDEF";
    for (auto c : uri) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            encoded += c;
        else {
            encoded += "%";
            encoded += digits[c >> 4];
            encoded += digits[c % 16];
        }
    }
    return encoded;
}

inline std::string decode(std::string_view uri) {
    std::string decoded;
    decoded.reserve(uri.size());
    for (std::size_t index = 0; index < uri.size(); ++index) {
        switch (uri[index]) {
        case '%':
            if (index + 3 <= uri.size()) {
                auto hexToValue = [](char c) -> uint8_t {
                    if (c >= '0' && c <= '9') return uint8_t(c - '0');
                    if (c >= 'A' && c <= 'F') return uint8_t(c - 'A' + 10);
                    if (c >= 'a' && c <= 'f') return uint8_t(c - 'a' + 10);
                    return 0;
                };
                decoded += char(hexToValue(uri[index + 1]) * 16 + hexToValue(uri[index + 2]));
                index += 2;
            }
            break;
        case '+': decoded += ' '; break;
        default: decoded += uri[index];
        }
    }
    return decoded;
}

} // namespace uri

namespace base64 {

namespace {

inline std::string encode(const uint8_t* input, size_t size) {
    static constexpr char code[] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V',
                                    'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r',
                                    's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/'};

    std::string output((4 * (size + 2) / 3), '\0');
    auto coded = output.begin();
    size_t i = 0;
    for (; i < size - 2; i += 3) {
        *coded++ = code[(input[i] >> 2) & 0x3F];
        *coded++ = code[((input[i] & 0x3) << 4) | ((input[i + 1] & 0xF0) >> 4)];
        *coded++ = code[((input[i + 1] & 0xF) << 2) | ((input[i + 2] & 0xC0) >> 6)];
        *coded++ = code[input[i + 2] & 0x3F];
    }
    if (i < size) {
        *coded++ = code[(input[i] >> 2) & 0x3F];
        if (i == (size - 1)) {
            *coded++ = code[((input[i] & 0x3) << 4)];
            *coded++ = '=';
        }
        else {
            *coded++ = code[((input[i] & 0x3) << 4) | ((input[i + 1] & 0xF0) >> 4)];
            *coded++ = code[((input[i + 1] & 0xF) << 2)];
        }
        *coded++ = '=';
    }
    if (coded != output.end()) output.erase(coded);

    return output;
}

#if __cplusplus <= 202002L // std::bswap will be in C++23
#if defined(_MSC_VER)
inline auto byteswap(uint64_t v) noexcept {
    return _byteswap_uint64(v);
}
inline auto byteswap(uint32_t v) noexcept {
    return _byteswap_ulong(v);
}
inline auto byteswap(uint16_t v) noexcept {
    return _byteswap_ushort(v);
}
#else
constexpr auto byteswap(uint64_t v) noexcept {
    return __builtin_bswap64(v);
}
constexpr auto byteswap(uint32_t v) noexcept {
    return __builtin_bswap32(v);
}
constexpr auto byteswap(uint16_t v) noexcept {
    return __builtin_bswap16(v);
}
#endif
#endif
} // namespace

template<typename T>
concept Container = ::std::movable<T> || requires(T t) {
    t.data();
    t.size();
    T::value_type;
};

inline std::string encode(Container auto input) {
    return encode(reinterpret_cast<const uint8_t*>(input.data()), input.size() * sizeof(typename decltype(input)::value_type));
}

inline std::string encodeInNetworkOrder(Container auto input) {
    using namespace std;
    if constexpr (endian::native == endian::little)
        for (auto& elem : input) elem = byteswap(elem);

    return encode(move(input));
}

} // namespace base64

namespace crypto {

constexpr std::array<uint32_t, 5> sha1(std::string_view input) {
    std::array<uint32_t, 5> digest = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint32_t block[64]{0};
    size_t blockByteIndex{0}, byteCount{0};
    auto next = [&](uint8_t byte) {
        block[blockByteIndex++] = byte;
        ++byteCount;
        if ((blockByteIndex = blockByteIndex % 64) == 0) {
            auto leftRotate = [](uint32_t value, size_t count) { return (value << count) ^ (value >> (32 - count)); };
            uint32_t a{digest[0]}, b{digest[1]}, c{digest[2]}, d{digest[3]}, e{digest[4]}, w[80];
            for (size_t i = 0; i < 16; i++) w[i] = (block[i * 4 + 0] << 24) | (block[i * 4 + 1] << 16) | (block[i * 4 + 2] << 8) | (block[i * 4 + 3]);
            for (size_t i = 16; i < 80; i++) w[i] = leftRotate((w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]), 1);
            for (std::size_t i = 0; i < 80; ++i) {
                uint32_t f{0}, k{0};
                if (i < 20) {
                    f = (b & c) | (~b & d);
                    k = 0x5A827999;
                }
                else if (i < 40) {
                    f = b ^ c ^ d;
                    k = 0x6ED9EBA1;
                }
                else if (i < 60) {
                    f = (b & c) | (b & d) | (c & d);
                    k = 0x8F1BBCDC;
                }
                else {
                    f = b ^ c ^ d;
                    k = 0xCA62C1D6;
                }
                auto temp = leftRotate(a, 5) + f + e + k + w[i];
                e = d;
                d = c;
                c = leftRotate(b, 30);
                b = a;
                a = temp;
            }
            digest[0] += a;
            digest[1] += b;
            digest[2] += c;
            digest[3] += d;
            digest[4] += e;
        }
    };

    for (auto c : input) next(static_cast<uint8_t>(c));
    auto b = byteCount * 8;
    next(0x80);
    if (blockByteIndex > 56)
        while (blockByteIndex != 0) next(0);
    while (blockByteIndex < 56) next(0);
    for (auto value : {uint8_t(0), uint8_t(0), uint8_t(0), uint8_t(0), uint8_t(b >> 24), uint8_t(b >> 16), uint8_t(b >> 8), uint8_t(b)}) next(value);

    return digest;
}

inline std::string sha1String(std::string_view input) {
    static const char* digits = "0123456789abcdef";
    std::string output;
    for (auto d : sha1(input))
        for (int8_t shift = 28; shift >= 0; shift -= 4) output += digits[(d >> shift) & 0xF];

    return output;
}

} // namespace crypto
} // namespace webfront/// @date 16/01/2022 22:27:42


namespace webfront {
namespace http {

struct Header {
    Header() = default;
    Header(std::string n, std::string v) : name(std::move(n)), value(std::move(v)) {}
    std::string name;
    std::string value;
};

struct Request {
    enum class Method { Connect, Delete, Get, Head, Options, Patch, Post, Put, Trace, Undefined };
    Method method;
    std::string uri;
    int httpVersionMajor;
    int httpVersionMinor;
    std::vector<Header> headers;

    void reset() {
        headers.clear();
        uri.clear();
    }

    constexpr Method getMethodFromString(std::string_view text) const {
        return text == "GET"       ? Method::Get
               : text == "HEAD"    ? Method::Head
               : text == "CONNECT" ? Method::Connect
               : text == "DELETE"  ? Method::Delete
               : text == "OPTIONS" ? Method::Options
               : text == "PATCH"   ? Method::Patch
               : text == "POST"    ? Method::Post
               : text == "PUT"     ? Method::Put
               : text == "TRACE"   ? Method::Trace
                                   : Method::Undefined;
    }

    void setMethod(std::string_view text) { method = getMethodFromString(text); }

    bool isUpgradeRequest(std::string_view protocol) const { return headerContains("Connection", "upgrade") && headerContains("Upgrade", protocol); }

    std::optional<std::string> getHeaderValue(std::string_view headerName) const {
        for (auto& header : headers)
            if (caseInsensitiveEqual(header.name, headerName)) return header.value;
        return {};
    }

private:
    /// @return true if text is contained in the value field of headerName (case insensitive)
    bool headerContains(std::string_view headerName, std::string_view text) const {
        auto header = getHeaderValue(headerName);
        if (header) {
            if (std::search(header.value().cbegin(), header.value().cend(), text.cbegin(), text.cend(),
                            [](char c1, char c2) { return (c1 == c2 || std::toupper(c1) == std::toupper(c2)); }) != header.value().cend())
                return true;
        }
        return false;
    }

    static constexpr bool caseInsensitiveEqual(std::string_view s1, std::string_view s2) {
        return ((s1.size() == s2.size()) &&
                std::equal(s1.begin(), s1.end(), s2.begin(), [](char c1, char c2) { return (c1 == c2 || std::toupper(c1) == std::toupper(c2)); }));
    }
};

struct Response {
    enum StatusCode : uint16_t { switchingProtocols = 101, ok = 200, badRequest = 400, notFound = 404, notImplemented = 501 };
    StatusCode statusCode;
    std::vector<Header> headers;
    std::string content;

    static Response getStatusResponse(StatusCode code) {
        Response response;
        response.statusCode = code;
        response.content = "<html><head><title>" + toString(code) + "</title></head>";
        response.content += "<body><h1>" + std::to_string(code) + " " + toString(code) + "</h1></body></html>";
        response.headers.emplace_back("Content-Length", std::to_string(response.content.size()));
        response.headers.emplace_back("Content-Type", "text/html");

        return response;
    }

    template<typename Net>
    std::vector<typename Net::ConstBuffer> toBuffers() const {
        std::vector<typename Net::ConstBuffer> buffers;
        static std::string httpStatus;
        httpStatus = "HTTP/1.1 " + std::to_string(statusCode) + " " + toString(statusCode) + "\r\n";
        buffers.push_back(Net::Buffer(httpStatus));

        static const char separator[] = {':', ' '};
        static const char crlf[] = {'\r', '\n'};
        for (auto& header : headers) {
            buffers.push_back(Net::Buffer(header.name));
            buffers.push_back(Net::Buffer(separator));
            buffers.push_back(Net::Buffer(header.value));
            buffers.push_back(Net::Buffer(crlf));
        }
        buffers.push_back(Net::Buffer(crlf));
        if (!content.empty()) buffers.push_back(Net::Buffer(content));

        return buffers;
    }

private:
    static std::string toString(StatusCode code) {
        switch (code) {
        case switchingProtocols: return "Switching Protocols";
        case ok: return "OK";
        case badRequest: return "Bad Request";
        case notFound: return "Not Found";
        case notImplemented: return "Not Implemented";
        }
        return {};
    }
};

struct MimeType {
    enum Type { plain, html, css, js, jpg, png, gif };
    Type type;

    MimeType(Type mimeType) : type(mimeType) {}

    static MimeType fromExtension(std::string_view e) {
        if (e.starts_with('.')) e = e.substr(1);
        if (e == "htm" || e == "html") return html;
        if (e == "css") return css;
        if (e == "js" || e == "mjs") return js;
        if (e == "jpg" || e == "jpeg") return jpg;
        if (e == "png") return png;
        if (e == "gif") return gif;
        return plain;
    }

    std::string toString() const {
        std::string names[]{"text/plain", "text/html", "text/css", "application/javascript", "image/jpeg", "image/png", "image/gif"};
        return names[type];
    }
};

template<typename Net>
class RequestHandler {
public:
    RequestHandler(const RequestHandler&) = delete;
    RequestHandler& operator=(const RequestHandler&) = delete;

    explicit RequestHandler(std::filesystem::path root) : documentRoot(root) {}

    Response handleRequest(Request request) {
        auto requestUri = uri::decode(request.uri);
        if (requestUri.empty() || requestUri[0] != '/' || requestUri.find("..") != std::string::npos)
            return Response::getStatusResponse(Response::badRequest);
        auto requestPath = documentRoot / std::filesystem::path(requestUri).relative_path();
        if (!requestPath.has_filename()) requestPath /= "index.html";
        
        Response response;
        switch (request.method) {
        case Request::Method::Get:
            if (request.isUpgradeRequest("websocket")) {
                auto key = request.getHeaderValue("Sec-WebSocket-Key");
                if (key) {
                    response.statusCode = Response::StatusCode::switchingProtocols;
                    response.headers.emplace_back("Upgrade", "websocket");
                    response.headers.emplace_back("Connection", "Upgrade");
                    response.headers.emplace_back("Sec-WebSocket-Accept",
                                                  base64::encodeInNetworkOrder(crypto::sha1(key.value() + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")));
                    response.headers.emplace_back("Sec-WebSocket-Protocol", "WebFront_0.1");
                    return response;
                }
            }
            [[fallthrough]];
        case Request::Method::Head: {
            std::ifstream file(requestPath, std::ios::in | std::ios::binary);
            if (!file) return Response::getStatusResponse(Response::notFound);

            if (request.method == Request::Method::Get) {
                char buffer[512];
                while (file.read(buffer, sizeof(buffer)).gcount() > 0)
                     response.content.append(buffer, static_cast<size_t>(file.gcount()));
            }
        } break;

        default: return Response::getStatusResponse(Response::notImplemented);
        };

        response.statusCode = Response::ok;
        response.headers.emplace_back("Content-Length", std::to_string(response.content.size()));
        response.headers.emplace_back("Content-Type", MimeType::fromExtension(requestPath.extension().string()).toString());

        return response;
    }

private:
    const std::filesystem::path documentRoot;
};

struct BadRequestException : public std::runtime_error {
    BadRequestException() : std::runtime_error("Bad HTTP request") {}
};

class RequestParser {
public:
    RequestParser() : state(State::methodStart) {}

    void reset() {
        currentRequest.reset();
        state = State::methodStart;
    }

    template<typename InputIterator>
    std::optional<Request> parse(InputIterator begin, InputIterator end) {
        while (begin != end)
            if (completeRequest(*begin++, currentRequest)) return currentRequest;

        return {};
    }

private:
    enum class State {
        methodStart,
        method,
        uri,
        versionH,
        versionT1,
        versionT2,
        versionP,
        versionSlash,
        versionMajorStart,
        versionMajor,
        versionMinorStart,
        versionMinor,
        newline1,
        headerLineStart,
        headerLws,
        headerName,
        spaceBeforeHeaderValue,
        headerValue,
        newline2,
        newline3
    };
    State state;
    Request currentRequest;

private: // clang-format off
    bool completeRequest(char input, Request& req) {
        auto isChar = [](char c) { return c >= 0; };
        auto isCtrl = [](char c) { return (c >= 0 && c <= 31) || (c == 127); };
        auto isSpecial = [](char c) {   switch (c) {
            case '(': case ')': case '<': case '>': case '@': case ',': case ';': case ':': case '\\': case '"':
            case '/': case '[': case ']': case '?': case '=': case '{': case '}': case ' ': case '\t': return true;
            default: return false;
        }};
        auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
        auto check = [this](bool cond, State next) {
            state = next;
            if (cond) throw BadRequestException();
        };

        static std::string buffer;

        switch (state) {
            case State::methodStart:
                check(!isChar(input) || isCtrl(input) || isSpecial(input), State::method);
                buffer = input;
                break;
            case State::method: if (input == ' ') { state = State::uri; req.setMethod(buffer); }
                              else if (!isChar(input) || isCtrl(input) || isSpecial(input)) { throw BadRequestException(); }
                              else buffer.push_back(input);
                break;
            case State::uri: if (input == ' ') { state = State::versionH; break; }
                           else if (isCtrl(input)) throw BadRequestException();
                           else { req.uri.push_back(input); break; }
            case State::versionH: check(input != 'H', State::versionT1); break;
            case State::versionT1: check(input != 'T', State::versionT2); break;
            case State::versionT2: check(input != 'T', State::versionP); break;
            case State::versionP: check(input != 'P', State::versionSlash); break;
            case State::versionSlash: check(input != '/', State::versionMajorStart); req.httpVersionMajor = 0; req.httpVersionMinor = 0; break;
            case State::versionMajorStart: check(!isDigit(input), State::versionMajor); req.httpVersionMajor = req.httpVersionMajor * 10 + input - '0'; break;
            case State::versionMajor: if (input == '.') state = State::versionMinorStart;
                                    else if (isDigit(input)) req.httpVersionMajor = req.httpVersionMajor * 10 + input - '0';
                                    else throw BadRequestException();
                break;
            case State::versionMinorStart:
                check(!isDigit(input), State::versionMinor);
                req.httpVersionMinor = req.httpVersionMinor * 10 + input - '0';
                break;
            case State::versionMinor: if (input == '\r') state = State::newline1;
                                    else if (isDigit(input)) req.httpVersionMinor = req.httpVersionMinor * 10 + input - '0';
                                    else throw BadRequestException();
                break;
            case State::newline1: check(input != '\n', State::headerLineStart); break;
            case State::headerLineStart: if (input == '\r') { state = State::newline3; break; }
                                       else if (!req.headers.empty() && (input == ' ' || input == '\t')) { state = State::headerLws; break; }
                                       else if (!isChar(input) || isCtrl(input) || isSpecial(input)) throw BadRequestException();
                                       else { req.headers.push_back(Header()); req.headers.back().name.push_back(input); state = State::headerName; break; }
            case State::headerLws:if (input == '\r') { state = State::newline2; break; }
                                 else if (input == ' ' || input == '\t') break;
                                 else if (isCtrl(input)) throw BadRequestException();
                                 else { state = State::headerValue; req.headers.back().value.push_back(input); break; }
            case State::headerName: if (input == ':') { state = State::spaceBeforeHeaderValue; break; }
                                  else if (!isChar(input) || isCtrl(input) || isSpecial(input)) throw BadRequestException();
                                  else { req.headers.back().name.push_back(input); break; }
            case State::spaceBeforeHeaderValue: check(input != ' ', State::headerValue); break;
            case State::headerValue: if (input == '\r') { state = State::newline2; break; }
                                   else if (isCtrl(input)) throw BadRequestException();
                                   else { req.headers.back().value.push_back(input); break; }
            case State::newline2: check(input != '\n', State::headerLineStart); break;
            case State::newline3: if (input == '\n') return true; else throw BadRequestException();
            default: throw BadRequestException();
        }
        return false;
    }
}; // clang-format on

template<typename Net>
class Connection : public std::enable_shared_from_this<Connection<Net>> {
public:
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    explicit Connection(typename Net::Socket sock, Connections<Connection>& connectionsHandler, RequestHandler<Net>& handler)
        : socket(std::move(sock)), connections(connectionsHandler), requestHandler(handler) {
        std::clog << "New connection\n";
    }

    void start() { read(); }
    void stop() { socket.close(); }

public:
    std::function<void(typename Net::Socket)> onUpgrade;

private:
    typename Net::Socket socket;
    Connections<Connection<Net>>& connections;
    RequestHandler<Net>& requestHandler;
    std::array<char, 8192> buffer;
    RequestParser requestParser;
    Response response;
    enum class Protocol { HTTP, HTTPUpgrading, WebSocket };
    Protocol protocol = Protocol::HTTP;

    void read() {
        auto self(this->shared_from_this());
        socket.async_read_some(Net::Buffer(buffer), [this, self](std::error_code ec, std::size_t bytesTransferred) {
            if (!ec) {
                switch (protocol) {
                case Protocol::HTTP:
                    try {
                        auto request = requestParser.parse(buffer.data(), buffer.data() + bytesTransferred);
                        if (request) {
                            response = requestHandler.handleRequest(request.value());
                            write();
                            if (response.statusCode == Response::switchingProtocols) protocol = Protocol::HTTPUpgrading;
                        }
                        else
                            read();
                    }
                    catch (const BadRequestException&) {
                        response = Response::getStatusResponse(Response::badRequest);
                        write();
                    }
                    break;

                default: std::clog << "Connection is no longer in HTTP protocol. Connection::read() is disabled.\n";
                }
            }
            else if (ec != Net::Error::OperationAborted)
                connections.stop(self);
        });
    }

    void write() {
        auto self(this->shared_from_this());
        Net::AsyncWrite(socket, response.toBuffers<Net>(), [this, self](std::error_code ec, std::size_t /*bytesTransferred*/) {
            if (protocol == Protocol::HTTPUpgrading) {
                protocol = Protocol::WebSocket;
                onUpgrade(std::move(socket));
            }
            else {
                if (!ec) socket.shutdown(Net::Socket::shutdown_both);
                if (ec != Net::Error::OperationAborted) connections.stop(self);
            }
        });
    }
};

template<typename Net>
requires networking::Features<Net>
class Server {
public:
    websocket::WSManager<Net> webSockets;

public:
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    explicit Server(std::string_view address, std::string_view port, std::filesystem::path docRoot = ".") : acceptor(ioContext), requestHandler(docRoot) {
        typename Net::Resolver resolver(ioContext);
        typename Net::Endpoint endpoint = *resolver.resolve(address, port).begin();
        acceptor.open(endpoint.protocol());
        acceptor.set_option(typename Net::Acceptor::reuse_address(true));
        acceptor.bind(endpoint);
        acceptor.listen();
        accept();
    }

    void run() { ioContext.run(); }
    void runOne() { ioContext.run_one(); }

private:
    typename Net::IoContext ioContext;
    typename Net::Acceptor acceptor;
    Connections<Connection<Net>> connections;
    RequestHandler<Net> requestHandler;

    void accept() {
        acceptor.async_accept([this](std::error_code ec, typename Net::Socket socket) {
            if (!acceptor.is_open()) return;
            auto newConnection = std::make_shared<Connection<Net>>(std::move(socket), connections, requestHandler);
            newConnection->onUpgrade = [this](typename Net::Socket sock) { webSockets.createWebSocket(std::move(sock)); };
            if (!ec) connections.start(newConnection);
            accept();
        });
    }

    void upgradeConnection(typename Net::Socket socket) { webSockets.createWebSocket(std::move(socket)); }
};

} // namespace http
} // namespace webfront/// @file WebFront.hpp


namespace webfront {

template<typename NetProvider>
class BasicUI {
    using WebSocketPtr = std::shared_ptr<websocket::WebSocket<NetProvider>>;
    http::Server<NetProvider> httpServer;

    enum class Command { Handshake = 72 };
    enum class JSEndian { little = 0, big = 1, mixed = little + big };

public:
    BasicUI(std::string_view port, std::filesystem::path docRoot = ".") : httpServer("0.0.0.0", port, docRoot) {
        httpServer.webSockets.onOpen([this](WebSocketPtr webSocket) {
            std::cout << "onOpen\n";
            webSocket->onMessage([webSocket](std::string_view text) {
                std::cout << "onMessage(text) : " << text << "\n";
                webSocket->write("This is my response");
            });

            webSocket->onMessage([this, webSocket](std::span<const std::byte> data) {
                switch (static_cast<Command>(data[0])) {
                case Command::Handshake:
                        sameEndian = (std::endian::native == std::endian::little && static_cast<JSEndian>(data[1]) == JSEndian::little)
                        || (std::endian::native == std::endian::big && static_cast<JSEndian>(data[1]) == JSEndian::big);
                        std::cout << "Platform and client share the same endianness\n";
                        break;
                }

                std::cout << "onMessage(binary) : " << utils::HexDump(data) << "\n";
            });
        });
    }

    void run() { httpServer.run(); }
    void runOne() { httpServer.runOne(); }
    auto runAsync() { return std::async(std::launch::async, &BasicUI::run, this); }

private:
    bool sameEndian = true;

private:
};

using UI = BasicUI<networking::TCPNetworkingTS>;
} // namespace webfront