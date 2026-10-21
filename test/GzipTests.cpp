#include "GzipFixtures.hpp"
#include <catch2/catch_test_macros.hpp>
#include <http/Gzip.hpp>

#include <string>

using webfront::http::gzip::decodeGzip;

namespace {
std::string payload() {
    std::string result;
    for (unsigned index = 0; index < 1000; ++index)
        result += "WebFront gzip fallback\n";
    for (unsigned repeat = 0; repeat < 3; ++repeat)
        for (unsigned byte = 0; byte < 256; ++byte)
            result.push_back(static_cast<char>(byte));
    return result;
}
}  // namespace

SCENARIO("Gzip decoding validates complete bounded members") {
    const auto expected = payload();
    GIVEN("fixed and dynamic Huffman blocks, with optional gzip fields") {
        for (const auto bytes : {gzipfixtures::fixed, gzipfixtures::dynamic, gzipfixtures::optional}) {
            WHEN("decoding a complete member of " << bytes.size() << " bytes") {
                THEN("the exact binary content is returned") {
                    REQUIRE(decodeGzip(bytes) == expected);
                    REQUIRE(decodeGzip(bytes, expected.size()) == expected);
                    REQUIRE_FALSE(decodeGzip(bytes, expected.size() - 1));
                }
            }
            WHEN("truncating a member of " << bytes.size() << " bytes at every byte boundary") {
                THEN("every prefix is rejected") {
                    for (std::size_t length = 0; length < bytes.size(); ++length) {
                        INFO(length);
                        REQUIRE_FALSE(decodeGzip(bytes.substr(0, length)));
                    }
                }
            }
        }
    }
    GIVEN("a gzip member with a stored DEFLATE block") {
        // A final, byte-aligned stored block containing 'abc', CRC32 and ISIZE.
        const std::string bytes("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03"
                                "\x01\x03\x00\xfc\xff"
                                "abc"
                                "\xc2\x41\x24\x35\x03\x00\x00\x00",
                                26);
        THEN("stored bytes and their complement are checked") {
            REQUIRE(decodeGzip(bytes) == "abc");
            auto corrupt = bytes;
            corrupt[13] ^= 1;
            REQUIRE_FALSE(decodeGzip(corrupt));
            REQUIRE_FALSE(decodeGzip(bytes, 2));
            for (std::size_t length = 0; length < bytes.size(); ++length)
                REQUIRE_FALSE(decodeGzip(std::string_view(bytes).substr(0, length)));
        }
    }
    GIVEN("empty and concatenated members") {
        const auto bytes = std::string(gzipfixtures::dynamic) + std::string(gzipfixtures::empty) + std::string(gzipfixtures::fixed);
        THEN("members are decoded independently under one total budget") {
            REQUIRE(decodeGzip(gzipfixtures::empty, 0) == "");
            REQUIRE(decodeGzip(bytes) == expected + expected);
            REQUIRE_FALSE(decodeGzip(bytes, expected.size()));
            REQUIRE_FALSE(decodeGzip(std::string(gzipfixtures::dynamic) + "garbage"));
        }
    }
    GIVEN("corrupt gzip metadata, trailer or block type") {
        for (const auto offset : {0u, 1u, 2u, 3u, 10u}) {
            auto bytes    = std::string(gzipfixtures::dynamic);
            bytes[offset] = static_cast<char>(0xff);
            REQUIRE_FALSE(decodeGzip(bytes));
        }
        for (std::size_t offset = gzipfixtures::dynamic.size() - 8; offset < gzipfixtures::dynamic.size(); ++offset) {
            auto bytes     = std::string(gzipfixtures::dynamic);
            bytes[offset] ^= 1;
            REQUIRE_FALSE(decodeGzip(bytes));
        }
        auto bytes = std::string(gzipfixtures::optional);
        bytes[10]  = static_cast<char>(0xff);  // Invalid extra-field length.
        REQUIRE_FALSE(decodeGzip(bytes));
        bytes      = gzipfixtures::optional;
        bytes[17] ^= 1;  // Header CRC covers the optional name, too.
        REQUIRE_FALSE(decodeGzip(bytes));
    }
}

SCENARIO("Gzip rejects invalid Huffman codes and match distances") {
    GIVEN("malformed DEFLATE blocks") {
        THEN("decoding rejects the blocks without accessing missing history or table entries") {
            for (const auto bytes : {gzipfixtures::invalidDistance,
                                     gzipfixtures::reservedLiteral,
                                     gzipfixtures::reservedDistance,
                                     gzipfixtures::oversubscribed,
                                     gzipfixtures::repeatWithoutPrevious,
                                     gzipfixtures::repeatOverflow,
                                     gzipfixtures::missingEnd}) {
                REQUIRE_FALSE(decodeGzip(bytes));
            }
        }
    }
}

SCENARIO("Gzip rejects reserved distance declarations and trailing padding") {
    GIVEN("dynamic blocks declaring 31 or 32 distance symbols") {
        THEN("the declarations are rejected even when no distance is used") {
            REQUIRE_FALSE(decodeGzip(gzipfixtures::distanceCount31));
            REQUIRE_FALSE(decodeGzip(gzipfixtures::distanceCount32));
        }
    }
    GIVEN("zero padding after a complete member") {
        const auto bytes = std::string(gzipfixtures::empty) + std::string(8, '\0');
        THEN("strict member parsing rejects the padding") {
            REQUIRE_FALSE(decodeGzip(bytes));
        }
    }
}

SCENARIO("Gzip default output budget is four MiB across concatenated members") {
    const auto  memberSize  = payload().size();
    const auto  memberCount = (4 * 1024 * 1024) / memberSize + 1;
    std::string bytes;
    for (std::size_t index = 0; index < memberCount; ++index)
        bytes += gzipfixtures::fixed;
    THEN("the default rejects a response just above four MiB") {
        REQUIRE_FALSE(decodeGzip(bytes));
    }
    THEN("an explicitly larger budget can permit the same valid content") {
        const auto decoded = decodeGzip(bytes, memberCount * memberSize);
        REQUIRE(decoded);
        REQUIRE(decoded->size() == memberCount * memberSize);
    }
}

SCENARIO("Gzip match copies preserve overlapping history across string growth") {
    using webfront::http::gzip::InflateContext;
    GIVEN("a periodic match longer than its distance") {
        std::string    output = "abc";
        InflateContext context{output, 0, 1000};
        context.copyMatch(258, 3);
        THEN("newly written bytes extend the same periodic sequence") {
            REQUIRE(output.size() == 261);
            for (std::size_t index = 0; index < output.size(); ++index)
                REQUIRE(output[index] == "abc"[index % 3]);
        }
    }
    GIVEN("a repeated byte and an exact output budget") {
        std::string    output = "x";
        InflateContext context{output, 0, 259};
        context.copyMatch(258, 1);
        THEN("the copy grows safely and the next copy fails without modifying output") {
            REQUIRE(output == std::string(259, 'x'));
            REQUIRE_THROWS_AS(context.copyMatch(3, 1), webfront::http::gzip::InvalidGzip);
            REQUIRE(output == std::string(259, 'x'));
        }
    }
}

SCENARIO("Gzip bulk reads require alignment and complete input") {
    webfront::http::gzip::GzipReader reader("abc");
    GIVEN("an unaligned reader") {
        reader.bits(1);
        REQUIRE_THROWS_AS(reader.readBytes(1), webfront::http::gzip::InvalidGzip);
        reader.align();
        THEN("aligned reads consume only available bytes") {
            REQUIRE(reader.readBytes(2) == "bc");
            REQUIRE(reader.done());
            REQUIRE(reader.readBytes(0).empty());
            REQUIRE_THROWS_AS(reader.readBytes(1), webfront::http::gzip::InvalidGzip);
        }
    }
    GIVEN("a truncated read") {
        REQUIRE_THROWS_AS(reader.readBytes(4), webfront::http::gzip::InvalidGzip);
        THEN("failure leaves the reader position unchanged") {
            REQUIRE(reader.readBytes(3) == "abc");
        }
    }
}

SCENARIO("Gzip CRC32 matches standard and binary reference vectors") {
    using webfront::http::gzip::gzipCrc;
    GIVEN("the empty input and the standard check vector") {
        THEN("the IEEE CRC32 values match") {
            REQUIRE(gzipCrc("") == 0u);
            REQUIRE(gzipCrc("123456789") == 0xcbf43926u);
        }
    }
    GIVEN("every byte value, including bytes with the high bit set") {
        std::string bytes;
        for (unsigned byte = 0; byte < 256; ++byte)
            bytes.push_back(static_cast<char>(byte));
        THEN("the result matches the independent zlib reference") {
            REQUIRE(gzipCrc(bytes) == 0x29058c73u);
        }
    }
}
