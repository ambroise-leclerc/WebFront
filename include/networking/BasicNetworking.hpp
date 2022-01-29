/// @date 25/01/2022 16:00:55
/// @author Ambroise Leclerc
/// @brief Some networking classes which provide common access to C++2x NetworkingTS, Mock networking or internal implementation
#pragma once
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <vector>


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
        std::size_t size() const noexcept { return bufSize; }
        MutableBuffer& operator+=(std::size_t n) noexcept {
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
        std::size_t size() const noexcept { return bufSize; }
        ConstBuffer& operator+=(std::size_t n) noexcept {
            std::size_t offset = n < bufSize ? n : bufSize;
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

    static MutableBuffer Buffer(void* d, std::size_t s) noexcept { return {d, s}; }
    static ConstBuffer Buffer(const void* d, std::size_t s) noexcept { return {d, s}; }
    static MutableBuffer Buffer(const MutableBuffer& b) noexcept { return b; }
    static MutableBuffer Buffer(const MutableBuffer& b, std::size_t s) noexcept { return {b.data(), std::min(b.size(), s)}; }
    static ConstBuffer Buffer(const ConstBuffer& b) noexcept { return b; }
    static ConstBuffer Buffer(const ConstBuffer& b, std::size_t s) noexcept { return {b.data(), std::min(b.size(), s)}; }
    template<typename T, std::size_t S>
    static MutableBuffer Buffer(T (&d)[S]) noexcept {
        return ToMutableBuffer(d, S);
    }
    template<typename T, std::size_t S>
    static ConstBuffer Buffer(const T (&d)[S]) noexcept {
        return ToConstBuffer(d, S);
    }
    template<typename T, std::size_t S>
    static MutableBuffer Buffer(std::array<T, S>& d) noexcept {
        return ToMutableBuffer(d.data(), S);
    }
    template<typename T, std::size_t S>
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

    template<typename T, std::size_t S>
    static MutableBuffer Buffer(T (&d)[S], std::size_t s) noexcept {
        return Buffer(Buffer(d), s * sizeof(T));
    }

    template<typename T, std::size_t S>
    static ConstBuffer Buffer(const T (&d)[S], std::size_t s) noexcept {
        return Buffer(Buffer(d), s * sizeof(T));
    }

    template<typename T, std::size_t S>
    static MutableBuffer Buffer(std::array<T, S>& d, std::size_t s) noexcept {
        return Buffer(Buffer(d), s * sizeof(T));
    }

    template<typename T, std::size_t S>
    static ConstBuffer Buffer(std::array<const T, S>& d, std::size_t s) noexcept {
        return Buffer(Buffer(d), s * sizeof(T));
    }

    template<typename T, std::size_t S>
    static ConstBuffer Buffer(const std::array<T, S>& d, std::size_t s) noexcept {
        return Buffer(Buffer(d), s * sizeof(T));
    }

    template<typename T, typename Alloc>
    static MutableBuffer Buffer(std::vector<T, Alloc>& d, std::size_t s) noexcept {
        return Buffer(Buffer(d), s * sizeof(T));
    }

    template<typename T, typename Alloc>
    static ConstBuffer Buffer(const std::vector<T, Alloc>& d, std::size_t s) noexcept {
        return Buffer(Buffer(d), s * sizeof(T));
    }

    template<typename CharT, typename Traits, typename Alloc>
    static MutableBuffer Buffer(std::basic_string<CharT, Traits, Alloc>& d, std::size_t s) noexcept {
        return Buffer(Buffer(d), s * sizeof(CharT));
    }

    template<typename CharT, typename Traits, typename Alloc>
    static ConstBuffer Buffer(const std::basic_string<CharT, Traits, Alloc>& d, std::size_t s) noexcept {
        return Buffer(Buffer(d), s * sizeof(CharT));
    }

    template<typename CharT, typename Traits>
    static ConstBuffer Buffer(std::basic_string_view<CharT, Traits> d, std::size_t s) noexcept {
        return Buffer(Buffer(d), s * sizeof(CharT));
    }

private:
    template<typename T>
    static MutableBuffer ToMutableBuffer(T* data, std::size_t s) {
        return {s ? data : nullptr, s * sizeof(T)};
    }
    template<typename T>
    static ConstBuffer ToConstBuffer(const T* data, std::size_t s) {
        return {s ? data : nullptr, s * sizeof(T)};
    }

protected:
    using super = BasicNetworking;
};



} // namespace networking
} // namespace webfront
