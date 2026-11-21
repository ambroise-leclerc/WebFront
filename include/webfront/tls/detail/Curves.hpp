#pragma once
#include "Field.hpp"

namespace webfront::tls::detail {
// RFC 7748 section 5. The affine input is little endian and its top bit is ignored.
inline Digest x25519(std::span<const Byte, 32> scalar, std::span<const Byte, 32> peer) {
    using F         = Field<Integer{0xffffffed, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0x7fffffff}>;
    auto k          = decode(scalar, true);
    k[0]           &= ~7u;
    k[7]            = (k[7] & 0x3fffffffu) | 0x40000000u;
    auto coordinate = decode(peer, true);
    coordinate[7]  &= 0x7fffffffu;
    const auto u    = F::enter(F::reduce(coordinate));
    const auto a24  = F::enter({121665});
    auto       x = F::enter({1}), z = Integer{}, nextX = u, nextZ = x;
    const auto swap = [](Integer& a, Integer& b, std::uint32_t choice) {
        const auto old = a;
        a              = select(a, b, choice);
        b              = select(b, old, choice);
    };
    std::uint32_t previous = 0;
    for (unsigned i = 255; i-- != 0;) {
        const auto current = bit(k, i);
        swap(x, nextX, previous ^ current);
        swap(z, nextZ, previous ^ current);
        previous       = current;
        const auto sum = F::add(x, z), difference = F::sub(x, z);
        const auto sumSquared = F::square(sum), differenceSquared = F::square(difference);
        const auto delta = F::sub(sumSquared, differenceSquared);
        const auto left  = F::multiply(F::sub(nextX, nextZ), sum);
        const auto right = F::multiply(F::add(nextX, nextZ), difference);
        nextX            = F::square(F::add(left, right));
        nextZ            = F::multiply(u, F::square(F::sub(left, right)));
        x                = F::multiply(sumSquared, differenceSquared);
        z                = F::multiply(delta, F::add(sumSquared, F::multiply(a24, delta)));
    }
    wipe(k.data(), sizeof(k));
    const auto affine = F::multiply(select(x, nextX, previous), F::invert(select(z, nextZ, previous)));
    return encode(F::leave(affine), true);
}

using P256Field = Field<Integer{0xffffffff, 0xffffffff, 0xffffffff, 0, 0, 0, 1, 0xffffffff}>;
using P256Order = Field<Integer{0xfc632551, 0xf3b9cac2, 0xa7179e84, 0xbce6faad, 0xffffffff, 0xffffffff, 0, 0xffffffff}>;

struct P256Point {
    Integer x{}, y{}, z{};  // Jacobian coordinates. z == 0 represents infinity.
};

// SEC 2 section 2.4.2. Compute kG with a fixed schedule, selecting the next
// Jacobian point with masks. The addend G is always affine.
inline std::array<Integer, 2> p256Base(const Integer& k) {
    using F = P256Field;
    const P256Point base{F::enter({0xd898c296, 0xf4a13945, 0x2deb33a0, 0x77037d81, 0x63a440f2, 0xf8bce6e5, 0xe12c4247, 0x6b17d1f2}),
                         F::enter({0x37bf51f5, 0xcbb64068, 0x6b315ece, 0x2bce3357, 0x7c0f9e16, 0x8ee7eb4a, 0xfe1a7f9b, 0x4fe342e2}),
                         F::enter({1})};
    const auto      choose = [](const P256Point& a, const P256Point& b, std::uint32_t choice) {
        return P256Point{select(a.x, b.x, choice), select(a.y, b.y, choice), select(a.z, b.z, choice)};
    };
    const auto twice = [](const P256Point& point) {
        const auto y2 = F::square(point.y), z2 = F::square(point.z);
        const auto xy2   = F::multiply(point.x, y2);
        auto       s     = F::add(xy2, xy2);
        s                = F::add(s, s);
        const auto slope = F::multiply(F::sub(point.x, z2), F::add(point.x, z2));
        const auto m     = F::add(F::add(slope, slope), slope);  // a = -3
        const auto x     = F::sub(F::square(m), F::add(s, s));
        auto       y4    = F::square(y2);
        for (unsigned i = 0; i != 3; ++i)
            y4 = F::add(y4, y4);
        const auto yz = F::multiply(point.y, point.z);
        return P256Point{x, F::sub(F::multiply(m, F::sub(s, x)), y4), F::add(yz, yz)};
    };
    const auto doubleBase = twice(base);
    P256Point  acc;
    for (unsigned i = 256; i-- != 0;) {
        const auto      doubled = twice(acc);
        const auto      z2      = F::square(doubled.z);
        const auto      h       = F::sub(F::multiply(base.x, z2), doubled.x);
        const auto      r       = F::sub(F::multiply(base.y, F::multiply(z2, doubled.z)), doubled.y);
        const auto      h2 = F::square(h), h3 = F::multiply(h2, h);
        const auto      v = F::multiply(doubled.x, h2);
        const auto      x = F::sub(F::sub(F::square(r), h3), F::add(v, v));
        const P256Point sum{x, F::sub(F::multiply(r, F::sub(v, x)), F::multiply(doubled.y, h3)), F::multiply(doubled.z, h)};
        const auto      same      = (1u ^ nonzero(h)) & (1u ^ nonzero(r));
        const auto      candidate = choose(choose(sum, doubleBase, same), base, 1u ^ nonzero(doubled.z));
        acc                       = choose(doubled, candidate, bit(k, i));
    }
    const auto inverse = F::invert(acc.z), inverseSquared = F::square(inverse);
    return {F::leave(F::multiply(acc.x, inverseSquared)), F::leave(F::multiply(acc.y, F::multiply(inverseSquared, inverse)))};
}
}  // namespace webfront::tls::detail
