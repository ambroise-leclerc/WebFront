/*
 * Implementation of curve P-256 (ECDH and ECDSA)
 *
 * Author: Manuel Pégourié-Gonnard.
 * SPDX-License-Identifier: Apache-2.0
 * License text is in the repository LICENSE.
 */
// Subset of p256-m (44af59e0cff5d3b1d653bc333814077ef830e1bd).
#pragma once
#if defined(__GNUC__)
#pragma GCC system_header
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <cstddef>
#include <cstdint>
namespace webfront::tls::detail::p256 {
using std::size_t;
using std::uint8_t;
using std::uint16_t;
using std::uint32_t;
using std::uint64_t;
inline void zeroize(void *d, size_t n)
{
    volatile char *p = static_cast<volatile char*>(d);
    while( n-- )
        *p++ = 0;
}


/**********************************************************************
 *
 * Operations on fixed-width unsigned integers
 *
 * Represented using 32-bit limbs, least significant limb first.
 * That is: x = x[0] + 2^32 x[1] + ... + 2^224 x[7] for 256-bit.
 *
 **********************************************************************/

/*
 * 256-bit set to 32-bit value
 *
 * in: x in [0, 2^32)
 * out: z = x
 */
inline void u256_set32(uint32_t z[8], uint32_t x)
{
    z[0] = x;
    for (unsigned i = 1; i < 8; i++) {
        z[i] = 0;
    }
}

/*
 * 256-bit addition
 *
 * in: x, y in [0, 2^256)
 * out: z = (x + y) mod 2^256
 *      c = (x + y) div 2^256
 * That is, z + c * 2^256 = x + y
 *
 * Note: as a memory area, z must be either equal to x or y, or not overlap.
 */
inline uint32_t u256_add(uint32_t z[8],
                         const uint32_t x[8], const uint32_t y[8])
{
    uint32_t carry = 0;

    for (unsigned i = 0; i < 8; i++) {
        uint64_t sum = (uint64_t) carry + x[i] + y[i];
        z[i] = (uint32_t) sum;
        carry = (uint32_t) (sum >> 32);
    }

    return carry;
}

/*
 * 256-bit subtraction
 *
 * in: x, y in [0, 2^256)
 * out: z = (x - y) mod 2^256
 *      c = 0 if x >=y, 1 otherwise
 * That is, z = c * 2^256 + x - y
 *
 * Note: as a memory area, z must be either equal to x or y, or not overlap.
 */
inline uint32_t u256_sub(uint32_t z[8],
                         const uint32_t x[8], const uint32_t y[8])
{
    uint32_t carry = 0;

    for (unsigned i = 0; i < 8; i++) {
        uint64_t diff = (uint64_t) x[i] - y[i] - carry;
        z[i] = (uint32_t) diff;
        carry = -(uint32_t) (diff >> 32);
    }

    return carry;
}

/*
 * 256-bit conditional assignment
 *
 * in: x in [0, 2^256)
 *     c in [0, 1]
 * out: z = x if c == 1, z unchanged otherwise
 *
 * Note: as a memory area, z must be either equal to x, or not overlap.
 */
inline void u256_cmov(uint32_t z[8], const uint32_t x[8], uint32_t c)
{
    const uint32_t x_mask = -c;
    for (unsigned i = 0; i < 8; i++) {
        z[i] = (z[i] & ~x_mask) | (x[i] & x_mask);
    }
}

/*
 * 256-bit compare for equality
 *
 * in: x in [0, 2^256)
 *     y in [0, 2^256)
 * out: 0 if x == y, unspecified non-zero otherwise
 */
inline uint32_t u256_diff0(const uint32_t x[8])
{
    uint32_t diff = 0;
    for (unsigned i = 0; i < 8; i++) {
        diff |= x[i];
    }
    return diff;
}

inline uint64_t u32_muladd64(uint32_t x, uint32_t y, uint32_t z, uint32_t t)
{
    /* x = xl + 2**16 xh, y = yl + 2**16 yh */
    const uint16_t xl = (uint16_t) x;
    const uint16_t yl = (uint16_t) y;
    const uint16_t xh = x >> 16;
    const uint16_t yh = y >> 16;

    /* x*y = xl*yl + 2**16 (xh*yl + yl*yh) + 2**32 xh*yh
     *     = lo    + 2**16 (m1    + m2   ) + 2**32 hi    */
    const uint32_t lo = (uint32_t) xl * yl;
    const uint32_t m1 = (uint32_t) xh * yl;
    const uint32_t m2 = (uint32_t) xl * yh;
    const uint32_t hi = (uint32_t) xh * yh;

    uint64_t acc = lo + ((uint64_t) (hi + (m1 >> 16) + (m2 >> 16)) << 32);
    acc += m1 << 16;
    acc += m2 << 16;
    acc += z;
    acc += t;

    return acc;
}
inline uint32_t u288_muladd(uint32_t z[9], uint32_t x, const uint32_t y[8])
{
    uint32_t carry = 0;

    for (unsigned i = 0; i < 8; i++) {
        const uint64_t prod = u32_muladd64(x, y[i], z[i], carry);
        z[i] = static_cast<uint32_t>(prod);
        carry = static_cast<uint32_t>(prod >> 32);
    }

    uint64_t sum = (uint64_t) z[8] + carry;
    z[8] = (uint32_t) sum;
    carry = (uint32_t) (sum >> 32);

    return carry;
}

/*
 * 288-bit in-place right shift by 32 bits
 *
 * in: z in [0, 2^288)
 *     c in [0, 2^32)
 * out: z_out = z_in div 2^32 + c * 2^256
 *            = (z_in + c * 2^288) div 2^32
 *
 * This is a helper for Montgomery multiplication.
 */
inline void u288_rshift32(uint32_t z[9], uint32_t c)
{
    for (unsigned i = 0; i < 8; i++) {
        z[i] = z[i + 1];
    }
    z[8] = c;
}

/*
 * 256-bit import from big-endian bytes
 *
 * in: p = p0, ..., p31
 * out: z = p0 * 2^248 + p1 * 2^240 + ... + p30 * 2^8 + p31
 */
inline void u256_from_bytes(uint32_t z[8], const uint8_t p[32])
{
    for (unsigned i = 0; i < 8; i++) {
        unsigned j = 4 * (7 - i);
        z[i] = ((uint32_t) p[j + 0] << 24) |
               ((uint32_t) p[j + 1] << 16) |
               ((uint32_t) p[j + 2] <<  8) |
               ((uint32_t) p[j + 3] <<  0);
    }
}

/*
 * 256-bit export to big-endian bytes
 *
 * in: z in [0, 2^256)
 * out: p = p0, ..., p31 such that
 *      z = p0 * 2^248 + p1 * 2^240 + ... + p30 * 2^8 + p31
 */
inline void u256_to_bytes(uint8_t p[32], const uint32_t z[8])
{
    for (unsigned i = 0; i < 8; i++) {
        unsigned j = 4 * (7 - i);
        p[j + 0] = (uint8_t) (z[i] >> 24);
        p[j + 1] = (uint8_t) (z[i] >> 16);
        p[j + 2] = (uint8_t) (z[i] >>  8);
        p[j + 3] = (uint8_t) (z[i] >>  0);
    }
}

/**********************************************************************
 *
 * Operations modulo a 256-bit prime m
 *
 * These are done in the Montgomery domain, that is x is represented by
 *  x * 2^256 mod m
 * Numbers need to be converted to that domain before computations,
 * and back from it afterwards.
 *
 * Inversion is computed using Fermat's little theorem.
 *
 * Assumptions on m:
 * - Montgomery operations require that m is odd.
 * - Fermat's little theorem require it to be a prime.
 * - m256_inv() further requires that m % 2^32 >= 2.
 * - m256_inv() also assumes that the value of m is not a secret.
 *
 * In practice operations are done modulo the curve's p and n,
 * both of which satisfy those assumptions.
 *
 **********************************************************************/

/*
 * Data associated to a modulus for Montgomery operations.
 *
 * m in [0, 2^256) - the modulus itself, must be odd
 * R2 = 2^512 mod m
 * ni = -m^-1 mod 2^32
 */
typedef struct {
    uint32_t m[8];
    uint32_t R2[8];
    uint32_t ni;
}
m256_mod;

/*
 * Data for Montgomery operations modulo the curve's p
 */
inline constexpr m256_mod p256_p = {
    {   /* the curve's p */
        0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000000,
        0x00000000, 0x00000000, 0x00000001, 0xFFFFFFFF,
    },
    {   /* 2^512 mod p */
        0x00000003, 0x00000000, 0xffffffff, 0xfffffffb,
        0xfffffffe, 0xffffffff, 0xfffffffd, 0x00000004,
    },
    0x00000001, /* -p^-1 mod 2^32 */
};

/*
 * Data for Montgomery operations modulo the curve's n
 */
inline constexpr m256_mod p256_n = {
    {   /* the curve's n */
        0xFC632551, 0xF3B9CAC2, 0xA7179E84, 0xBCE6FAAD,
        0xFFFFFFFF, 0xFFFFFFFF, 0x00000000, 0xFFFFFFFF,
    },
    {   /* 2^512 mod n */
        0xbe79eea2, 0x83244c95, 0x49bd6fa6, 0x4699799c,
        0x2b6bec59, 0x2845b239, 0xf3d95620, 0x66e12d94,
    },
    0xee00bc4f, /* -n^-1 mod 2^32 */
};

/*
 * Modular addition
 *
 * in: x, y in [0, m)
 *     mod must point to a valid m256_mod structure
 * out: z = (x + y) mod m, in [0, m)
 *
 * Note: as a memory area, z must be either equal to x or y, or not overlap.
 */
inline void m256_add(uint32_t z[8],
                     const uint32_t x[8], const uint32_t y[8],
                     const m256_mod *mod)
{
    uint32_t r[8];
    uint32_t carry_add = u256_add(z, x, y);
    uint32_t carry_sub = u256_sub(r, z, mod->m);
    /* Need to subract m if:
     *      x+y >= 2^256 > m (that is, carry_add == 1)
     *   OR z >= m (that is, carry_sub == 0) */
    uint32_t use_sub = carry_add | (1 - carry_sub);
    u256_cmov(z, r, use_sub);
}

/*
 * Modular addition mod p
 *
 * in: x, y in [0, p)
 * out: z = (x + y) mod p, in [0, p)
 *
 * Note: as a memory area, z must be either equal to x or y, or not overlap.
 */
inline void m256_add_p(uint32_t z[8],
                       const uint32_t x[8], const uint32_t y[8])
{
    m256_add(z, x, y, &p256_p);
}

/*
 * Modular subtraction
 *
 * in: x, y in [0, m)
 *     mod must point to a valid m256_mod structure
 * out: z = (x - y) mod m, in [0, m)
 *
 * Note: as a memory area, z must be either equal to x or y, or not overlap.
 */
inline void m256_sub(uint32_t z[8],
                     const uint32_t x[8], const uint32_t y[8],
                     const m256_mod *mod)
{
    uint32_t r[8];
    uint32_t carry = u256_sub(z, x, y);
    (void) u256_add(r, z, mod->m);
    /* Need to add m if and only if x < y, that is carry == 1.
     * In that case z is in [2^256 - m + 1, 2^256 - 1], so the
     * addition will have a carry as well, which cancels out. */
    u256_cmov(z, r, carry);
}

/*
 * Modular subtraction mod p
 *
 * in: x, y in [0, p)
 * out: z = (x + y) mod p, in [0, p)
 *
 * Note: as a memory area, z must be either equal to x or y, or not overlap.
 */
inline void m256_sub_p(uint32_t z[8],
                       const uint32_t x[8], const uint32_t y[8])
{
    m256_sub(z, x, y, &p256_p);
}

/*
 * Montgomery modular multiplication
 *
 * in: x, y in [0, m)
 *     mod must point to a valid m256_mod structure
 * out: z = (x * y) / 2^256 mod m, in [0, m)
 *
 * Note: as a memory area, z may overlap with x or y.
 */
inline void m256_mul(uint32_t z[8],
                     const uint32_t x[8], const uint32_t y[8],
                     const m256_mod *mod)
{
    /*
     * Algorithm 14.36 in Handbook of Applied Cryptography with:
     * b = 2^32, n = 8, R = 2^256
     */
    uint32_t m_prime = mod->ni;
    uint32_t a[9];

    for (unsigned i = 0; i < 9; i++) {
        a[i] = 0;
    }

    for (unsigned i = 0; i < 8; i++) {
        /* the "mod 2^32" is implicit from the type */
        uint32_t u = (a[0] + x[i] * y[0]) * m_prime;

        /* a = (a + x[i] * y + u * m) div b */
        uint32_t c = u288_muladd(a, x[i], y);
        c += u288_muladd(a, u, mod->m);
        u288_rshift32(a, c);
    }

    /* a = a > m ? a - m : a */
    uint32_t carry_add = a[8];  // 0 or 1 since a < 2m, see HAC Note 14.37
    uint32_t carry_sub = u256_sub(z, a, mod->m);
    uint32_t use_sub = carry_add | (1 - carry_sub);     // see m256_add()
    u256_cmov(z, a, 1 - use_sub);
}

/*
 * Montgomery modular multiplication modulo p.
 *
 * in: x, y in [0, p)
 * out: z = (x * y) / 2^256 mod p, in [0, p)
 *
 * Note: as a memory area, z may overlap with x or y.
 */
inline void m256_mul_p(uint32_t z[8],
                       const uint32_t x[8], const uint32_t y[8])
{
    m256_mul(z, x, y, &p256_p);
}

/*
 * In-place conversion to Montgomery form
 *
 * in: z in [0, m)
 *     mod must point to a valid m256_mod structure
 * out: z_out = z_in * 2^256 mod m, in [0, m)
 */
inline void m256_prep(uint32_t z[8], const m256_mod *mod)
{
    m256_mul(z, z, mod->R2, mod);
}

/*
 * In-place conversion from Montgomery form
 *
 * in: z in [0, m)
 *     mod must point to a valid m256_mod structure
 * out: z_out = z_in / 2^256 mod m, in [0, m)
 * That is, z_in was z_actual * 2^256 mod m, and z_out is z_actual
 */
inline void m256_done(uint32_t z[8], const m256_mod *mod)
{
    uint32_t one[8];
    u256_set32(one, 1);
    m256_mul(z, z, one, mod);
}

/*
 * Set to 32-bit value
 *
 * in: x in [0, 2^32)
 *     mod must point to a valid m256_mod structure
 * out: z = x * 2^256 mod m, in [0, m)
 * That is, z is set to the image of x in the Montgomery domain.
 */
inline void m256_set32(uint32_t z[8], uint32_t x, const m256_mod *mod)
{
    u256_set32(z, x);
    m256_prep(z, mod);
}

/*
 * Modular inversion in Montgomery form
 *
 * in: x in [0, m)
 *     mod must point to a valid m256_mod structure
 *     such that mod->m % 2^32 >= 2, assumed to be public.
 * out: z = x^-1 * 2^512 mod m if x != 0,
 *      z = 0 if x == 0
 * That is, if x = x_actual    * 2^256 mod m, then
 *             z = x_actual^-1 * 2^256 mod m
 *
 * Note: as a memory area, z may overlap with x.
 */
inline void m256_inv(uint32_t z[8], const uint32_t x[8],
                     const m256_mod *mod)
{
    /*
     * Use Fermat's little theorem to compute x^-1 as x^(m-2).
     *
     * Take advantage of the fact that both p's and n's least significant limb
     * is at least 2 to perform the subtraction on the flight (no carry).
     *
     * Use plain right-to-left binary exponentiation;
     * branches are OK as the exponent is not a secret.
     */
    uint32_t bitval[8];
    u256_cmov(bitval, x, 1);    /* copy x before writing to z */

    m256_set32(z, 1, mod);

    unsigned i = 0;
    uint32_t limb = mod->m[i] - 2;
    while (1) {
        for (unsigned j = 0; j < 32; j++) {
            if ((limb & 1) != 0) {
                m256_mul(z, z, bitval, mod);
            }
            m256_mul(bitval, bitval, bitval, mod);
            limb >>= 1;
        }

        if (i == 7)
            break;

        i++;
        limb = mod->m[i];
    }
}

/*
 * Import modular integer from bytes to Montgomery domain
 *
 * in: p = p0, ..., p32
 *     mod must point to a valid m256_mod structure
 * out: z = (p0 * 2^248 + ... + p31) * 2^256 mod m, in [0, m)
 *      return 0 if the number was already in [0, m), or -1.
 *      z may be incorrect and must be discared when -1 is returned.
 */
inline int m256_from_bytes(uint32_t z[8],
                           const uint8_t p[32], const m256_mod *mod)
{
    u256_from_bytes(z, p);

    uint32_t t[8];
    uint32_t lt_m = u256_sub(t, z, mod->m);
    if (lt_m != 1)
        return -1;

    m256_prep(z, mod);
    return 0;
}

/*
 * Export modular integer from Montgomery domain to bytes
 *
 * in: z in [0, 2^256)
 *     mod must point to a valid m256_mod structure
 * out: p = p0, ..., p31 such that
 *      z = (p0 * 2^248 + ... + p31) * 2^256 mod m
 */
inline void m256_to_bytes(uint8_t p[32],
                          const uint32_t z[8], const m256_mod *mod)
{
    uint32_t zi[8];
    u256_cmov(zi, z, 1);
    m256_done(zi, mod);

    u256_to_bytes(p, zi);
}

/**********************************************************************
 *
 * Operations on curve points
 *
 * Points are represented in two coordinates system:
 *  - affine (x, y) - extended to represent 0 (see below)
 *  - jacobian (x:y:z)
 * In either case, coordinates are integers modulo p256_p and
 * are always represented in the Montgomery domain.
 *
 * For background on jacobian coordinates, see for example [GECC] 3.2.2:
 * - conversions go (x, y) -> (x:y:1) and (x:y:z) -> (x/z^2, y/z^3)
 * - the curve equation becomes y^2 = x^3 - 3 x z^4 + b z^6
 * - 0 (aka the origin aka point at infinity) is (x:y:0) with y^2 = x^3.
 * - point negation goes -(x:y:z) = (x:-y:z)
 *
 * Normally 0 (the point at infinity) can't be represented in affine
 * coordinates. However we extend affine coordinates with the convention that
 * (0, 0) (which is normally not a point on the curve) is interpreted as 0.
 *
 * References:
 * - [GECC]: Guide to Elliptic Curve Cryptography; Hankerson, Menezes,
 *   Vanstone; Springer, 2004.
 * - [CMO98]: Efficient Elliptic Curve Exponentiation Using Mixed Coordinates;
 *   Cohen, Miyaji, Ono; Springer, ASIACRYPT 1998.
 *   https://link.springer.com/content/pdf/10.1007/3-540-49649-1_6.pdf
 * - [RCB15]: Complete addition formulas for prime order elliptic curves;
 *   Renes, Costello, Batina; IACR e-print 2015-1060.
 *   https://eprint.iacr.org/2015/1060.pdf
 *
 **********************************************************************/

/*
 * The curve's b parameter in the Short Weierstrass equation
 *  y^2 = x^3 - 3*x + b
 * Compared to the standard, this is converted to the Montgomery domain.
 */
inline constexpr uint32_t p256_gx[8] = { /* G_x * 2^256 mod p */
    0x18a9143c, 0x79e730d4, 0x5fedb601, 0x75ba95fc,
    0x77622510, 0x79fb732b, 0xa53755c6, 0x18905f76,
};
inline constexpr uint32_t p256_gy[8] = { /* G_y * 2^256 mod p */
    0xce95560a, 0xddf25357, 0xba19e45c, 0x8b4ab8e4,
    0xdd21f325, 0xd2e88688, 0x25885d85, 0x8571ff18,
};

inline void point_to_affine(uint32_t x[8], uint32_t y[8], uint32_t z[8])
{
    uint32_t t[8];

    m256_inv(z, z, &p256_p);    /* z = z^-1 */

    m256_mul_p(t, z, z);        /* t = z^-2 */
    m256_mul_p(x, x, t);        /* x = x * z^-2 */

    m256_mul_p(t, t, z);        /* t = z^-3 */
    m256_mul_p(y, y, t);        /* y = y * z^-3 */
}

inline void point_double(uint32_t x[8], uint32_t y[8], uint32_t z[8])
{
    /*
     * This is formula 6 from [CMO98], cited as complete in [RCB15] (table 1).
     * Notations as in the paper, except u added and t ommited (it's x3).
     */
    uint32_t m[8], s[8], u[8];

    /* m = 3 * x^2 + a * z^4 = 3 * (x + z^2) * (x - z^2) */
    m256_mul_p(s, z, z);
    m256_add_p(m, x, s);
    m256_sub_p(u, x, s);
    m256_mul_p(s, m, u);
    m256_add_p(m, s, s);
    m256_add_p(m, m, s);

    /* s = 4 * x * y^2 */
    m256_mul_p(u, y, y);
    m256_add_p(u, u, u); /* u = 2 * y^2 (used below) */
    m256_mul_p(s, x, u);
    m256_add_p(s, s, s);

    /* u = 8 * y^4 (not named in the paper, first term of y3) */
    m256_mul_p(u, u, u);
    m256_add_p(u, u, u);

    /* x3 = t = m^2 - 2 * s */
    m256_mul_p(x, m, m);
    m256_sub_p(x, x, s);
    m256_sub_p(x, x, s);

    /* z3 = 2 * y * z */
    m256_mul_p(z, y, z);
    m256_add_p(z, z, z);

    /* y3 = -u + m * (s - t) */
    m256_sub_p(y, s, x);
    m256_mul_p(y, y, m);
    m256_sub_p(y, y, u);
}

inline void point_add(uint32_t x1[8], uint32_t y1[8], uint32_t z1[8],
                      const uint32_t x2[8], const uint32_t y2[8])
{
    /*
     * This is formula 5 from [CMO98], with z2 == 1 substituted. We use
     * intermediates with neutral names, and names from the paper in comments.
     */
    uint32_t t1[8], t2[8], t3[8];

    /* u1 = x1 and s1 = y1 (no computations) */

    /* t1 = u2 = x2 z1^2 */
    m256_mul_p(t1, z1, z1);
    m256_mul_p(t2, t1, z1);
    m256_mul_p(t1, t1, x2);

    /* t2 = s2 = y2 z1^3 */
    m256_mul_p(t2, t2, y2);

    /* t1 = h = u2 - u1 */
    m256_sub_p(t1, t1, x1); /* t1 = x2 * z1^2 - x1 */

    /* t2 = r = s2 - s1 */
    m256_sub_p(t2, t2, y1);

    /* z3 = z1 * h */
    m256_mul_p(z1, z1, t1);

    /* t1 = h^3 */
    m256_mul_p(t3, t1, t1);
    m256_mul_p(t1, t3, t1);

    /* t3 = x1 * h^2 */
    m256_mul_p(t3, t3, x1);

    /* x3 = r^2 - 2 * x1 * h^2 - h^3 */
    m256_mul_p(x1, t2, t2);
    m256_sub_p(x1, x1, t3);
    m256_sub_p(x1, x1, t3);
    m256_sub_p(x1, x1, t1);

    /* y3 = r * (x1 * h^2 - x3) - y1 h^3 */
    m256_sub_p(t3, t3, x1);
    m256_mul_p(t3, t3, t2);
    m256_mul_p(t1, t1, y1);
    m256_sub_p(y1, t3, t1);
}

inline void scalar_mult(uint32_t rx[8], uint32_t ry[8],
                        const uint32_t px[8], const uint32_t py[8],
                        const uint32_t s[8])
{
    /*
     * We use a signed binary ladder, see for example slides 10-14 of
     * http://ecc2015.math.u-bordeaux1.fr/documents/hamburg.pdf but with
     * implicit recoding, and a different loop initialisation to avoid feeding
     * 0 to our addition formulas, as they don't support it.
     */
    uint32_t s_odd[8], py_neg[8], py_use[8], rz[8];

    /*
     * Make s odd by replacing it with n - s if necessary.
     *
     * If s was odd, we'll have s_odd = s, and define P' = P.
     * Otherwise, we'll have s_odd = n - s and define P' = -P.
     *
     * Either way, we can compute s * P as s_odd * P'.
     */
    u256_sub(s_odd, p256_n.m, s); /* no carry, result still in [1, n-1] */
    uint32_t negate = ~s[0] & 1;
    u256_cmov(s_odd, s, 1 - negate);

    /* Compute py_neg = - py mod p (that's the y coordinate of -P) */
    u256_set32(py_use, 0);
    m256_sub_p(py_neg, py_use, py);

    /* Initialize R = P' = (x:(-1)^negate * y:1) */
    u256_cmov(rx, px, 1);
    u256_cmov(ry, py, 1);
    m256_set32(rz, 1, &p256_p);
    u256_cmov(ry, py_neg, negate);

    /*
     * For any odd number s_odd = b255 ... b1 1, we have
     *      s_odd = 2^255 + 2^254 sbit(b255) + ... + 2 sbit(b2) + sbit(b1)
     * writing
     *      sbit(b) = 2 * b - 1 = b ? 1 : -1
     *
     * Use that to compute s_odd * P' by repeating R = 2 * R +- P':
     *      s_odd * P' = 2 * ( ... (2 * P' + sbit(b255) P') ... ) + sbit(b1) P'
     *
     * The loop invariant is that when beginning an iteration we have
     *      R = s_i P'
     * with
     *      s_i = 2^(255-i) + 2^(254-i) sbit(b_255) + ...
     * where the sum has 256 - i terms.
     *
     * When updating R we need to make sure the input to point_add() is
     * neither 0 not +-P'. Since that input is 2 s_i P', it is sufficient to
     * see that 1 < 2 s_i < n-1. The lower bound is obvious since s_i is a
     * positive integer, and for the upper bound we distinguish three cases.
     *
     * If i > 1, then s_i < 2^254, so 2 s_i < 2^255 < n-1.
     * Otherwise, i == 1 and we have 2 s_i = s_odd - sbit(b1).
     *      If s_odd <= n-4, then 2 s_1 <= n-3.
     *      Otherwise, s_odd = n-2, and for this curve's value of n,
     *      we have b1 == 1, so sbit(b1) = 1 and 2 s_1 <= n-3.
     */
    for (unsigned i = 255; i > 0; i--) {
        uint32_t bit = (s_odd[i / 32] >> i % 32) & 1;

        /* set (px, py_use) = sbit(bit) P' = sbit(bit) * (-1)^negate P */
        u256_cmov(py_use, py, bit ^ negate);
        u256_cmov(py_use, py_neg, (1 - bit) ^ negate);

        /* Update R = 2 * R +- P' */
        point_double(rx, ry, rz);
        point_add(rx, ry, rz, px, py_use);
    }

    point_to_affine(rx, ry, rz);
}

inline int scalar_from_bytes(uint32_t s[8], const uint8_t p[32])
{
    u256_from_bytes(s, p);

    uint32_t r[8];
    uint32_t lt_n = u256_sub(r, s, p256_n.m);

    u256_set32(r, 1);
    uint32_t lt_1 = u256_sub(r, s, r);

    if (lt_n && !lt_1)
        return 0;

    return -1;
}

inline void ecdsa_m256_mod_n(uint32_t x[8])
{
    uint32_t t[8];
    uint32_t c = u256_sub(t, x, p256_n.m);
    u256_cmov(x, t, 1 - c);
}


}
#if defined(_MSC_VER) && !defined(__GNUC__)
#pragma warning(pop)
#endif
