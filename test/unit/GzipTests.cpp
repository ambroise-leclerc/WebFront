#include "GzipFixtures.hpp"
#include <catch2/catch_test_macros.hpp>
#include <webfront/http/Gzip.hpp>
#include <webfront/http/GzipCache.hpp>

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

SCENARIO("Validated gzip cache shares GET content and HEAD metadata within the current output budget") {
    using webfront::http::gzip::ValidatedCache;
    ValidatedCache cache(100000);
    auto           bytes = std::string(gzipfixtures::fixed);
    GIVEN("HEAD validates a revision before GET") {
        REQUIRE(cache.resolve(bytes, 23768, true) == 23768);
        REQUIRE(bytes.empty());
        bytes = gzipfixtures::fixed;
        REQUIRE(cache.resolve(bytes, 23768) == 23768);
        REQUIRE(bytes == payload());
        THEN("GET reuses the validated bytes and length") {
            REQUIRE(cache.statistics().decodes == 1);
            REQUIRE(cache.statistics().hits == 1);
        }
        THEN("a smaller active output budget cannot reuse the larger result") {
            for (const bool head : {false, true}) {
                bytes = gzipfixtures::fixed;
                REQUIRE_FALSE(cache.resolve(bytes, 23767, head));
            }
            REQUIRE(cache.statistics().decodes == 1);
            bytes = gzipfixtures::fixed;
            REQUIRE(cache.resolve(bytes, 23768, true) == 23768);
        }
    }
}

SCENARIO("Validated gzip cache bounds keys and data and evicts the least recently used revision") {
    using webfront::http::gzip::ValidatedCache;
    auto first  = std::string(gzipfixtures::fixed);
    auto second = first;
    auto third  = first;
    // MTIME changes the compressed revision without changing the validated payload.
    second[4] = 1;
    third[4]  = 2;
    ValidatedCache probe(100000);
    auto           bytes = first;
    REQUIRE(probe.resolve(bytes, 23768));
    const auto charge = probe.statistics().retainedBytes;
    REQUIRE(charge >= first.size() + 23768);
    GIVEN("room for exactly two revisions") {
        ValidatedCache cache(2 * charge);
        for (const auto& revision : {first, second, first, third, first}) {
            bytes = revision;
            REQUIRE(cache.resolve(bytes, 23768));
            REQUIRE(cache.statistics().retainedBytes <= 2 * charge);
        }
        THEN("the touched first revision survives while the second is evicted") {
            REQUIRE(cache.statistics().decodes == 3);
            REQUIRE(cache.statistics().hits == 2);
            REQUIRE(cache.statistics().entries == 2);
            bytes = second;
            REQUIRE(cache.resolve(bytes, 23768));
            REQUIRE(cache.statistics().decodes == 4);
        }
    }
    GIVEN("a budget one byte below a complete entry, or disabled caching") {
        for (const auto budget : {std::size_t{0}, charge - 1}) {
            ValidatedCache cache(budget);
            for (const bool head : {false, true}) {
                bytes = first;
                REQUIRE(cache.resolve(bytes, 23768, head));
            }
            THEN("both requests succeed without retaining partial keys or metadata") {
                REQUIRE(cache.statistics().retainedBytes == 0);
                REQUIRE(cache.statistics().entries == 0);
                REQUIRE(cache.statistics().decodes == 2);
            }
        }
    }
    GIVEN("an oversized revision alongside a retained asset") {
        ValidatedCache cache(charge);
        bytes = first;
        REQUIRE(cache.resolve(bytes, 23768));
        bytes = first + first;
        REQUIRE(cache.resolve(bytes, 47536));
        bytes = first;
        REQUIRE(cache.resolve(bytes, 23768));
        REQUIRE(cache.statistics().decodes == 2);
        REQUIRE(cache.statistics().hits == 1);
    }
}

SCENARIO("Validated gzip cache never promotes invalid streams or trusts ISIZE alone") {
    webfront::http::gzip::ValidatedCache cache(100000);
    GIVEN("corrupt CRC, invalid trailer, truncated and concatenated invalid members") {
        auto corrupt                 = std::string(gzipfixtures::fixed);
        corrupt[corrupt.size() - 8] ^= 1;
        auto wrongSize               = std::string(gzipfixtures::fixed);
        wrongSize.back()             = 1;
        for (const auto& invalid : {corrupt, wrongSize, std::string(gzipfixtures::fixed.substr(0, 498)), std::string(gzipfixtures::fixed) + corrupt}) {
            for (const bool head : {false, true}) {
                auto bytes = invalid;
                REQUIRE_FALSE(cache.resolve(bytes, 100000, head));
            }
        }
        THEN("every attempt validates again and nothing is retained") {
            REQUIRE(cache.statistics().decodes == 8);
            REQUIRE(cache.statistics().entries == 0);
            REQUIRE(cache.statistics().retainedBytes == 0);
        }
    }
    GIVEN("an initial decode failing its output budget") {
        auto bytes = std::string(gzipfixtures::fixed);
        REQUIRE_FALSE(cache.resolve(bytes, 1));
        bytes = gzipfixtures::fixed;
        REQUIRE(cache.resolve(bytes, 23768));
        REQUIRE(cache.statistics().decodes == 2);
        REQUIRE(cache.statistics().entries == 1);
    }
}

SCENARIO("Validated gzip cache lifetime is owned and copies do not share application state") {
    using webfront::http::gzip::ValidatedCache;
    std::string response(gzipfixtures::fixed);
    GIVEN("a populated cache") {
        {
            ValidatedCache cache(100000);
            REQUIRE(cache.resolve(response, 23768));
            auto copy = cache;
            REQUIRE(copy.statistics().entries == 0);
            auto bytes = std::string(gzipfixtures::fixed);
            REQUIRE(copy.resolve(bytes, 23768));
            REQUIRE(copy.statistics().decodes == 1);
        }
        THEN("the response survives destruction and a new application starts cold") {
            REQUIRE(response == payload());
            ValidatedCache next(100000);
            response = gzipfixtures::fixed;
            REQUIRE(next.resolve(response, 23768, true));
            REQUIRE(next.statistics().decodes == 1);
            REQUIRE(next.statistics().hits == 0);
        }
    }
}

SCENARIO("Validated gzip cache permits empty streams under a zero output budget") {
    // Empty stored block, with a validated CRC32 and ISIZE of zero.
    const std::string                    empty("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03"
                                               "\x01\x00\x00\xff\xff"
                                               "\x00\x00\x00\x00\x00\x00\x00\x00",
                            23);
    webfront::http::gzip::ValidatedCache cache(1000);
    GIVEN("GET then HEAD of a valid empty asset") {
        for (const bool head : {false, true}) {
            auto bytes = empty;
            REQUIRE(cache.resolve(bytes, 0, head) == 0);
            REQUIRE(bytes.empty());
        }
        REQUIRE(cache.statistics().decodes == 1);
        REQUIRE(cache.statistics().hits == 1);
        REQUIRE(cache.statistics().retainedBytes <= 1000);
    }
}
