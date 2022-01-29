/// @file HexDump.hpp
/// @date 16/01/2022 22:27:42
/// @author Ambroise Leclerc
/// @brief HexDump tool for debugging/development purposes
#pragma once
#include <algorithm>
#include <concepts>
#include <iomanip>
#include <ostream>
#include <span>
#include <sstream>
#include <vector>

#ifdef __APPLE__
#  include <type_traits>
namespace std {
template <typename T, typename... Args> concept constructible_from = destructible<T> && is_constructible_v<T, Args...>;
template <typename T,typename U>
concept convertible_to = is_convertible_v<T, U> && requires(add_rvalue_reference_t<T> (&t)()) { static_cast<U>(t()); };
template <class T> concept swappable = requires(T& t, T& t2) { swap(t, t2); };
template <class T> concept move_constructible = constructible_from<T, T> && convertible_to<T, T>;
template <class T> concept movable = is_object_v<T> && move_constructible<T> && swappable<T>;
}

#endif


namespace webfront {
namespace utils {

template<typename T>
concept Buffer = std::movable<T> || requires(T t) {
    t.data();
    t.size();
};

/// Provides an hexadecimal dump of a container or a buffer
template<Buffer BufferType>
struct HexDump {
    HexDump(const BufferType& buf, size_t startAddr = 0) : buffer(buf), startAddress(startAddr) {}

    const BufferType& buffer;
    size_t startAddress;
};

template<typename Container>
std::ostream& operator<<(std::ostream& os, const HexDump<Container>& h) {
    using namespace std;
    size_t address = 0;
    os << hex << setfill('0');
    auto buffer = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(h.buffer.data()),
                                           h.buffer.size() * sizeof(typename pointer_traits<decltype(h.buffer.data())>::element_type));
    auto size = size_t(buffer.size());
    while (address < size) {
        os << setw(8) << address + h.startAddress;

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

} // namespace utils
} // namespace webfront