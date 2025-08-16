/// @data 31/07/2017 19:32:53
/// @author Ambroise Leclerc
/// @brief StringHash provides constexpr string hash function in order to generate
/// compile-time integer hashes for string :
/// switch (StringHash::calc(command.c_str())) {
///     case "click"_hash : onClick.call(); break;
///     case "mouseup"_hash : onMouseUp.call(); break;
///  }

//
// Copyright (c) 2017, Ambroise Leclerc
//   All rights reserved.
//
//   Redistribution and use in source and binary forms, with or without
//   modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright
//     notice, this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright
//     notice, this list of conditions and the following disclaimer in
//     the documentation and/or other materials provided with the
//     distribution.
//   * Neither the name of the copyright holders nor the names of
//     contributors may be used to endorse or promote products derived
//     from this software without specific prior written permission.
//
//  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS 'AS IS'
//  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
//  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
//  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
//  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
//  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
//  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
//  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
//  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
//  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
//  POSSIBILITY OF SUCH DAMAGE.

#include <cstdint>

class StringHash {
public:
    using type = uint32_t;

    static type calc(const char* zStr, type seed = 0) {
        while (*zStr) {
            seed = seed * 101 + *zStr++;
        }
        return seed;
    }

    template<typename Iter>
    static type calc(Iter first, Iter last, type seed = 0) {
        while (first != last) {
            seed = seed * 101 + *first++;
        }
        return seed;
    }



private:
    static constexpr type compileTime(const char* zStr, type seed = 0) {
        return *zStr ? (compileTime(zStr + 1, (seed * 101ull) + *zStr)) : seed;
    }

    friend constexpr type operator""_hash(char const* zStr, size_t);
};

constexpr StringHash::type operator""_hash(char const* zStr, size_t) {
    return StringHash::compileTime(zStr);
}
