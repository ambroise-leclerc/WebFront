/// @date 01/03/2022 22:39:42
/// @author Ambroise Leclerc
/// @brief Provides access to local or virtual file system.
#pragma once

#include <array>
#include <bit>
#include <cstring>
#include <optional>
#include <string_view>

namespace webfront {

namespace filesystem {

template<typename T, typename Buffer>
concept File = requires(T f, Buffer buffer, size_t size) { f.read(buffer, size); };

template<typename T>
concept Filesystem = requires(T fs, std::string_view filename) { T::open(filename); };

class IndexFS {
public:
    class File {
    public:
        enum FileId { index, favicon };

        File(FileId id) : fileId(id) {}
        size_t read(auto& buffer) const { return read(buffer, buffer.size()); }
        size_t read(auto& buffer, size_t size) const {
            switch (fileId) {
            case index: return 0;
            case favicon:
                return WebfrontIco::read(reinterpret_cast<char*>(buffer.data()), size).gcount();
            }
            return 0;
        }

    private:
        FileId fileId;

        struct WebfrontIco {
            static constexpr size_t data_size{766};
            static constexpr std::array<uint64_t, 96> data{
              0x0000010001002020, 0x100001000400e802, 0x0000160000002800, 0x0000200000004000, 0x0000010004000000,
              0x0000000000000000, 0x0000000000001000, 0x0000000000003834, 0x3200e09b1200ead9, 0xb0006c676200ae9f,
              0x7500e6b24200524e, 0x4800e1a429009d7b, 0x2f00bfbab900857e, 0x6f00fbf8ef00b887, 0x2000866d3800e8c2,
              0x6d0063594600bb23, 0x0000000000000000, 0x0000000032bbb900, 0x0000000000000000, 0x00000000009b2000,
              0x0000000000000000, 0x000000000002a000, 0x000000006d8cc8d6, 0x0000000000036000, 0x006000d111717111,
              0x1d00000000000000, 0x00006c1777777777, 0x7110000000000000, 0x0006117777777777, 0x7777f00000000000,
              0x00f1777777777777, 0x77771f0000000000, 0x0017777171717177, 0x7777710000000000, 0x0117111717171717,
              0x1717171000000060, 0xf111711111111111, 0x1171111f00600000, 0xc111111111111111, 0x1111111100000600,
              0x1111111217be7e1b, 0xe71111116000000d, 0x111111eb7ebb111b, 0x21111111d000060d, 0x111111227b2b5112,
              0x211111118060000c, 0x111115b5eb72211b, 0xb2251111c0000668, 0x11111221b21eb11b, 0x2ee5111180600608,
              0x11117be5b711be1b, 0xe1111111c060066d, 0x1111eb1221112b1b, 0xe7111111d660066f, 0x1111be7be1117b5b,
              0xbbbbb111f0600666, 0x1111111111111111, 0x1111111166600666, 0xd717171717171711, 0x7171771d66600f66,
              0x6777777177717777, 0x1777717666606666, 0x6377777777777777, 0x7777773666f006ff, 0xf685575777575777,
              0x57775a6f6f6666f6, 0xff3a575555757555, 0x5555affff6f066ff, 0x3f3fa55555555557, 0x555af33636f6f6ff,
              0xf333334555555555, 0xea333333636faff3, 0xf3333333a445444a, 0x3a33333ff664b36f, 0xf333a3aaaaaaaaaa,
              0xaa3a3333f332b243, 0x363f333333333333, 0x33333fffaa2bbbb9, 0x9999999999999999, 0x999999999bbbc000,
              0x0003800000010000, 0x0000000000000000, 0x0000000000000000, 0x0000000000000000, 0x0000000000000000,
              0x0000000000000002, 0x1000001310000005, 0x0000002418000008, 0x9000004890000040, 0x500000905f800000,
              0x0000000000000000, 0x0000000000000000, 0x0000000000000000, 0x0000000000000000, 0x0000800000008000,
              0x0001e00000070000};

            struct istream {
                size_t readIndex, lastReadCount;
                bool eofBit, badBit;

                size_t tellg() const { return readIndex; }
                size_t gcount() const { return lastReadCount; }
                bool eof() const { return eofBit; }
                bool bad() const { return badBit; }
                bool fail() const { return false; }
                explicit operator bool() const { return !fail(); }
                bool operator!() const { return eof() || bad(); }
            };

            static istream& read(char* s, size_t count) { return get(s, count); }
            static istream& get(char* s, size_t count) {
                static istream stream{.readIndex{0}, .lastReadCount{0}, .eofBit{false}, .badBit{false}};
                static std::array<char, 8> chunk;

                stream.lastReadCount = 0;
                if (stream.eof()) return stream;
                for (size_t index = 0; index < count; ++index) {
                    if (stream.eof()) {
                        stream.badBit = true;
                        stream.lastReadCount = index;
                        return stream;
                    }
                    if (stream.readIndex % 8 == 0) memcpy(chunk.data(), &data[stream.readIndex / 8], 8);
                    size_t readIndex = std::endian::native == std::endian::big ? stream.readIndex % 8 : 7 - (stream.readIndex % 8);
                    s[index] = chunk[readIndex];

                    stream.readIndex++;
                    if (stream.readIndex == data_size) stream.eofBit = true;
                }
                stream.lastReadCount = count;
                return stream;
            }
        };
    };

public:
    static std::optional<File> open(std::string_view filename) {
        if (filename == "index.html")
            return File(File::index);
        else if (filename == "favicon.ico")
            return File(File::favicon);
        return {};
    }
};

class NativeFS {};

} // namespace filesystem

} // namespace webfront