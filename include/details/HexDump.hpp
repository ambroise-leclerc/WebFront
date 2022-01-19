#pragma once
#include <algorithm>
#include <concepts>
#include <iomanip>
#include <ostream>
#include <span>
#include <sstream>
#include <vector>

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
    HexDump(const BufferType& buffer, size_t startAddress = 0) : buffer(buffer), startAddress(startAddress) {}

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
                char car = buffer[index];
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
} // namespace webfront