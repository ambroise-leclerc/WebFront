#include "GzipFixtures.hpp"
#include "Bdd.hpp"
#include <webfront/http/Gzip.hpp>
#include <webfront/http/GzipCache.hpp>

#include <string>

using namespace webfront::bdd;

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

const Scenario gzipDecoding{"Gzip decoding validates complete bounded members", [] {
    const auto expected = payload();
    given("fixed and dynamic Huffman blocks, with optional gzip fields", [&] {
        for (const auto bytes : {gzipfixtures::fixed, gzipfixtures::dynamic, gzipfixtures::optional}) {
            when("decoding a complete member of ", bytes.size(), " bytes", [&] {
                then("the exact binary content is returned", [&] {
                    require(decodeGzip(bytes) == expected);
                    require(decodeGzip(bytes, expected.size()) == expected);
                    require(!decodeGzip(bytes, expected.size() - 1));
                });
            });
            when("truncating a member of ", bytes.size(), " bytes at every byte boundary", [&] {
                then("every prefix is rejected", [&] {
                    for (std::size_t length = 0; length < bytes.size(); ++length) {
                        auto scope = info("length ", length);
                        require(!decodeGzip(bytes.substr(0, length)));
                    }
                });
            });
        }
    });
    given("a gzip member with a stored DEFLATE block", [&] {
        // A final, byte-aligned stored block containing 'abc', CRC32 and ISIZE.
        const std::string bytes("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03"
                                "\x01\x03\x00\xfc\xff"
                                "abc"
                                "\xc2\x41\x24\x35\x03\x00\x00\x00",
                                26);
        then("stored bytes and their complement are checked", [&] {
            require(decodeGzip(bytes) == "abc");
            auto corrupt = bytes;
            corrupt[13] ^= 1;
            require(!decodeGzip(corrupt));
            require(!decodeGzip(bytes, 2));
            for (std::size_t length = 0; length < bytes.size(); ++length)
                require(!decodeGzip(std::string_view(bytes).substr(0, length)));
        });
    });
    given("empty and concatenated members", [&] {
        const auto bytes = std::string(gzipfixtures::dynamic) + std::string(gzipfixtures::empty) + std::string(gzipfixtures::fixed);
        then("members are decoded independently under one total budget", [&] {
            require(decodeGzip(gzipfixtures::empty, 0) == "");
            require(decodeGzip(bytes) == expected + expected);
            require(!decodeGzip(bytes, expected.size()));
            require(!decodeGzip(std::string(gzipfixtures::dynamic) + "garbage"));
        });
    });
    given("corrupt gzip metadata, trailer or block type", [&] {
        for (const auto offset : {0u, 1u, 2u, 3u, 10u}) {
            auto bytes    = std::string(gzipfixtures::dynamic);
            bytes[offset] = static_cast<char>(0xff);
            require(!decodeGzip(bytes));
        }
        for (std::size_t offset = gzipfixtures::dynamic.size() - 8; offset < gzipfixtures::dynamic.size(); ++offset) {
            auto bytes     = std::string(gzipfixtures::dynamic);
            bytes[offset] ^= 1;
            require(!decodeGzip(bytes));
        }
        auto bytes = std::string(gzipfixtures::optional);
        bytes[10]  = static_cast<char>(0xff);  // Invalid extra-field length.
        require(!decodeGzip(bytes));
        bytes      = gzipfixtures::optional;
        bytes[17] ^= 1;  // Header CRC covers the optional name, too.
        require(!decodeGzip(bytes));
    });
}};

const Scenario gzipRejects{"Gzip rejects invalid Huffman codes and match distances", [] {
    given("malformed DEFLATE blocks", [&] {
        then("decoding rejects the blocks without accessing missing history or table entries", [&] {
            for (const auto bytes : {gzipfixtures::invalidDistance,
                                     gzipfixtures::reservedLiteral,
                                     gzipfixtures::reservedDistance,
                                     gzipfixtures::oversubscribed,
                                     gzipfixtures::repeatWithoutPrevious,
                                     gzipfixtures::repeatOverflow,
                                     gzipfixtures::missingEnd}) {
                require(!decodeGzip(bytes));
            }
        });
    });
}};

const Scenario gzipRejectsReserved{"Gzip rejects reserved distance declarations and trailing padding", [] {
    given("dynamic blocks declaring 31 or 32 distance symbols", [&] {
        then("the declarations are rejected even when no distance is used", [&] {
            require(!decodeGzip(gzipfixtures::distanceCount31));
            require(!decodeGzip(gzipfixtures::distanceCount32));
        });
    });
    given("zero padding after a complete member", [&] {
        const auto bytes = std::string(gzipfixtures::empty) + std::string(8, '\0');
        then("strict member parsing rejects the padding", [&] {
            require(!decodeGzip(bytes));
        });
    });
}};

const Scenario gzipDefault{"Gzip default output budget is four MiB across concatenated members", [] {
    const auto  memberSize  = payload().size();
    const auto  memberCount = (4 * 1024 * 1024) / memberSize + 1;
    std::string bytes;
    for (std::size_t index = 0; index < memberCount; ++index)
        bytes += gzipfixtures::fixed;
    then("the default rejects a response just above four MiB", [&] {
        require(!decodeGzip(bytes));
    });
    then("an explicitly larger budget can permit the same valid content", [&] {
        const auto decoded = decodeGzip(bytes, memberCount * memberSize);
        require(decoded);
        require(decoded->size() == memberCount * memberSize);
    });
}};

const Scenario gzipMatch{"Gzip match copies preserve overlapping history across string growth", [] {
    using webfront::http::gzip::InflateContext;
    given("a periodic match longer than its distance", [&] {
        std::string    output = "abc";
        InflateContext context{output, 0, 1000};
        context.copyMatch(258, 3);
        then("newly written bytes extend the same periodic sequence", [&] {
            require(output.size() == 261);
            for (std::size_t index = 0; index < output.size(); ++index)
                require(output[index] == "abc"[index % 3]);
        });
    });
    given("a repeated byte and an exact output budget", [&] {
        std::string    output = "x";
        InflateContext context{output, 0, 259};
        context.copyMatch(258, 1);
        then("the copy grows safely and the next copy fails without modifying output", [&] {
            require(output == std::string(259, 'x'));
            requireThrows<webfront::http::gzip::InvalidGzip>([&] { context.copyMatch(3, 1); });
            require(output == std::string(259, 'x'));
        });
    });
}};

const Scenario gzipBulk{"Gzip bulk reads require alignment and complete input", [] {
    webfront::http::gzip::GzipReader reader("abc");
    given("an unaligned reader", [&] {
        reader.bits(1);
        requireThrows<webfront::http::gzip::InvalidGzip>([&] { reader.readBytes(1); });
        reader.align();
        then("aligned reads consume only available bytes", [&] {
            require(reader.readBytes(2) == "bc");
            require(reader.done());
            require(reader.readBytes(0).empty());
            requireThrows<webfront::http::gzip::InvalidGzip>([&] { reader.readBytes(1); });
        });
    });
    given("a truncated read", [&] {
        requireThrows<webfront::http::gzip::InvalidGzip>([&] { reader.readBytes(4); });
        then("failure leaves the reader position unchanged", [&] {
            require(reader.readBytes(3) == "abc");
        });
    });
}};

const Scenario gzipCRC32{"Gzip CRC32 matches standard and binary reference vectors", [] {
    using webfront::http::gzip::gzipCrc;
    given("the empty input and the standard check vector", [&] {
        then("the IEEE CRC32 values match", [&] {
            require(gzipCrc("") == 0u);
            require(gzipCrc("123456789") == 0xcbf43926u);
        });
    });
    given("every byte value, including bytes with the high bit set", [&] {
        std::string bytes;
        for (unsigned byte = 0; byte < 256; ++byte)
            bytes.push_back(static_cast<char>(byte));
        then("the result matches the independent zlib reference", [&] {
            require(gzipCrc(bytes) == 0x29058c73u);
        });
    });
}};

const Scenario validatedGzip{"Validated gzip cache shares GET content and HEAD metadata within the current output budget", [] {
    using webfront::http::gzip::ValidatedCache;
    ValidatedCache cache(100000);
    auto           bytes = std::string(gzipfixtures::fixed);
    given("HEAD validates a revision before GET", [&] {
        require(cache.resolve(bytes, 23768, true) == 23768);
        require(bytes.empty());
        bytes = gzipfixtures::fixed;
        require(cache.resolve(bytes, 23768) == 23768);
        require(bytes == payload());
        then("GET reuses the validated bytes and length", [&] {
            require(cache.statistics().decodes == 1);
            require(cache.statistics().hits == 1);
        });
        then("a smaller active output budget cannot reuse the larger result", [&] {
            for (const bool head : {false, true}) {
                bytes = gzipfixtures::fixed;
                require(!cache.resolve(bytes, 23767, head));
            }
            require(cache.statistics().decodes == 1);
            bytes = gzipfixtures::fixed;
            require(cache.resolve(bytes, 23768, true) == 23768);
        });
    });
}};

const Scenario validatedGzipCache{"Validated gzip cache bounds keys and data and evicts the least recently used revision", [] {
    using webfront::http::gzip::ValidatedCache;
    auto first  = std::string(gzipfixtures::fixed);
    auto second = first;
    auto third  = first;
    // MTIME changes the compressed revision without changing the validated payload.
    second[4] = 1;
    third[4]  = 2;
    ValidatedCache probe(100000);
    auto           bytes = first;
    require(probe.resolve(bytes, 23768));
    const auto charge = probe.statistics().retainedBytes;
    require(charge >= first.size() + 23768);
    given("room for exactly two revisions", [&] {
        ValidatedCache cache(2 * charge);
        for (const auto& revision : {first, second, first, third, first}) {
            bytes = revision;
            require(cache.resolve(bytes, 23768));
            require(cache.statistics().retainedBytes <= 2 * charge);
        }
        then("the touched first revision survives while the second is evicted", [&] {
            require(cache.statistics().decodes == 3);
            require(cache.statistics().hits == 2);
            require(cache.statistics().entries == 2);
            bytes = second;
            require(cache.resolve(bytes, 23768));
            require(cache.statistics().decodes == 4);
        });
    });
    given("a budget one byte below a complete entry, or disabled caching", [&] {
        for (const auto budget : {std::size_t{0}, charge - 1}) {
            ValidatedCache cache(budget);
            for (const bool head : {false, true}) {
                bytes = first;
                require(cache.resolve(bytes, 23768, head));
            }
            then("both requests succeed without retaining partial keys or metadata", [&] {
                require(cache.statistics().retainedBytes == 0);
                require(cache.statistics().entries == 0);
                require(cache.statistics().decodes == 2);
            });
        }
    });
    given("an oversized revision alongside a retained asset", [&] {
        ValidatedCache cache(charge);
        bytes = first;
        require(cache.resolve(bytes, 23768));
        bytes = first + first;
        require(cache.resolve(bytes, 47536));
        bytes = first;
        require(cache.resolve(bytes, 23768));
        require(cache.statistics().decodes == 2);
        require(cache.statistics().hits == 1);
    });
}};

const Scenario validatedGzipCacheNever{"Validated gzip cache never promotes invalid streams or trusts ISIZE alone", [] {
    webfront::http::gzip::ValidatedCache cache(100000);
    given("corrupt CRC, invalid trailer, truncated and concatenated invalid members", [&] {
        auto corrupt                 = std::string(gzipfixtures::fixed);
        corrupt[corrupt.size() - 8] ^= 1;
        auto wrongSize               = std::string(gzipfixtures::fixed);
        wrongSize.back()             = 1;
        for (const auto& invalid : {corrupt, wrongSize, std::string(gzipfixtures::fixed.substr(0, 498)), std::string(gzipfixtures::fixed) + corrupt}) {
            for (const bool head : {false, true}) {
                auto bytes = invalid;
                require(!cache.resolve(bytes, 100000, head));
            }
        }
        then("every attempt validates again and nothing is retained", [&] {
            require(cache.statistics().decodes == 8);
            require(cache.statistics().entries == 0);
            require(cache.statistics().retainedBytes == 0);
        });
    });
    given("an initial decode failing its output budget", [&] {
        auto bytes = std::string(gzipfixtures::fixed);
        require(!cache.resolve(bytes, 1));
        bytes = gzipfixtures::fixed;
        require(cache.resolve(bytes, 23768));
        require(cache.statistics().decodes == 2);
        require(cache.statistics().entries == 1);
    });
}};

const Scenario validatedGzipCacheLifetime{"Validated gzip cache lifetime is owned and copies do not share application state", [] {
    using webfront::http::gzip::ValidatedCache;
    std::string response(gzipfixtures::fixed);
    given("a populated cache", [&] {
        {
            ValidatedCache cache(100000);
            require(cache.resolve(response, 23768));
            auto copy = cache;
            require(copy.statistics().entries == 0);
            auto bytes = std::string(gzipfixtures::fixed);
            require(copy.resolve(bytes, 23768));
            require(copy.statistics().decodes == 1);
        }
        then("the response survives destruction and a new application starts cold", [&] {
            require(response == payload());
            ValidatedCache next(100000);
            response = gzipfixtures::fixed;
            require(next.resolve(response, 23768, true));
            require(next.statistics().decodes == 1);
            require(next.statistics().hits == 0);
        });
    });
}};

const Scenario validatedGzipCachePermits{"Validated gzip cache permits empty streams under a zero output budget", [] {
    // Empty stored block, with a validated CRC32 and ISIZE of zero.
    const std::string                    empty("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03"
                                               "\x01\x00\x00\xff\xff"
                                               "\x00\x00\x00\x00\x00\x00\x00\x00",
                            23);
    webfront::http::gzip::ValidatedCache cache(1000);
    given("GET then HEAD of a valid empty asset", [&] {
        for (const bool head : {false, true}) {
            auto bytes = empty;
            require(cache.resolve(bytes, 0, head) == 0);
            require(bytes.empty());
        }
        require(cache.statistics().decodes == 1);
        require(cache.statistics().hits == 1);
        require(cache.statistics().retainedBytes <= 1000);
    });
}};

const Scenario gzipDecodingFills{"Gzip decoding fills a preallocated bounded worker buffer without growing it", [] {
    std::string output;
    output.reserve(23768);
    const auto capacity = output.capacity();
    for (const auto bytes : {gzipfixtures::fixed, gzipfixtures::dynamic, gzipfixtures::optional}) {
        require(webfront::http::gzip::decodeGzipInto(bytes, output, 23768));
        require(output == payload());
        require(output.capacity() == capacity);
        require(!webfront::http::gzip::decodeGzipInto(bytes, output, 23767));
        require(output.empty());
    }
}};

const Scenario gzipBuffered{"Gzip buffered reads preserve logical byte offsets and exact member boundaries", [] {
    using webfront::http::gzip::GzipReader;
    using webfront::http::gzip::InvalidGzip;
    given("a binary stream read with every supported width and initial bit alignment", [&] {
        const std::string bytes("\x81\x72\xe3\x54\xa5\x96", 6);
        then("every value and offset counts consumed bits without consuming later bytes", [&] {
            for (unsigned alignment = 0; alignment < 8; ++alignment) {
                for (unsigned width = 0; width <= 32; ++width) {
                    GzipReader reader(bytes);
                    reader.bits(alignment);
                    std::uint32_t expected = 0;
                    for (unsigned bit = 0; bit < width; ++bit) {
                        const auto offset = alignment + bit;
                        expected         |= ((static_cast<unsigned char>(bytes[offset / 8]) >> (offset % 8)) & 1u) << bit;
                    }
                    auto scope = info("alignment ", alignment, ", width ", width);
                    require(reader.bits(width) == expected);
                    require(reader.offset() == (alignment + width) / 8);
                    require(!reader.done());
                    reader.align();
                    const auto consumed = (alignment + width + 7) / 8;
                    require(reader.offset() == consumed);
                    require(reader.readBytes(bytes.size() - consumed) == std::string_view(bytes).substr(consumed));
                    require(reader.done());
                    require(reader.bits(0) == 0);
                    requireThrows<InvalidGzip>([&] { reader.bits(1); });
                }
            }
        });
    });
    given("a one-byte input and a partially consumed final byte", [&] {
        GzipReader reader(std::string_view("\xff", 1));
        require(reader.bits(1) == 1);
        require(reader.offset() == 0);
        require(!reader.done());
        require(reader.bits(7) == 127);
        require(reader.offset() == 1);
        require(reader.done());
        requireThrows<InvalidGzip>([&] { reader.bits(1); });
        requireThrows<InvalidGzip>([&] { reader.bits(33); });
    });
    given("optional header checksums and empty members between Huffman members", [&] {
        const auto bytes = std::string(gzipfixtures::optional) + std::string(gzipfixtures::empty) + std::string(gzipfixtures::fixed);
        then("header CRC ranges and every member trailer stay exact", [&] {
            require(decodeGzip(bytes) == payload() + payload());
            for (std::size_t size = bytes.size() - 8; size < bytes.size(); ++size)
                require(!decodeGzip(std::string_view(bytes).substr(0, size)));
            require(!decodeGzip(bytes + std::string(1, '\0')));
        });
    });
}};

const Scenario gzipLookahead{"Gzip lookahead retains prefetched bytes through alignment and bulk reads", [] {
    using webfront::http::gzip::GzipReader;
    using webfront::http::gzip::InvalidGzip;
    GzipReader reader("abcdef");
    unsigned   prefix;
    given("lookahead across four bytes without consuming them", [&] {
        require(reader.peek(32, prefix));
        require(prefix == 0x64636261u);
        require(reader.offset() == 0);
        require(reader.readBytes(2) == "ab");
        require(reader.bits(8) == 'c');
        require(reader.peek(16, prefix));
        require(prefix == 0x6564u);
        then("alignment keeps complete prefetched bytes", [&] {
            reader.bits(1);
            reader.align();
            require(reader.offset() == 4);
            require(reader.readBytes(2) == "ef");
            require(reader.done());
        });
    });
    given("lookahead beyond the end of an exact input span", [&] {
        require(reader.readBytes(5) == "abcde");
        require(!reader.peek(16, prefix));
        then("a failed probe preserves the last byte for a shorter code", [&] {
            require(reader.offset() == 5);
            require(reader.bits(8) == 'f');
            require(reader.done());
            requireThrows<InvalidGzip>([&] { reader.consume(1); });
        });
    });
}};

const Scenario gzipHuffman{"Gzip Huffman lookup preserves short tails, long codes and empty distance trees", [] {
    using namespace webfront::http::gzip;
    given("a single-symbol incomplete alphabet at the very last input bit", [&] {
        const std::array<unsigned, 1> lengths{1};
        DeflateTree                   tree(lengths);
        GzipReader                    reader(std::string_view("\x7f", 1));
        reader.bits(7);
        require(tree.decode(reader) == 0);
        require(reader.done());
    });
    given("a complete tree with codes longer than the direct table", [&] {
        std::array<unsigned, 512> tooMany{};
        requireThrows<InvalidGzip>([&] { (void)DeflateTree(tooMany); });
        // One code at each length 1..8 and two at length 9 make a complete alphabet.
        const std::array<unsigned, 10> lengths{1, 2, 3, 4, 5, 6, 7, 8, 9, 9};
        DeflateTree                    tree(lengths);
        GzipReader                     reader(std::string_view("\xff\x01", 2));
        require(tree.decode(reader) == 9);
        require(reader.offset() == 1);
        require(reader.bits(7) == 0);
        require(reader.done());
    });
    given("an empty distance alphabet and a full input byte", [&] {
        const std::array<unsigned, 1> lengths{0};
        DeflateTree                   tree(lengths);
        GzipReader                    reader(std::string_view("\0\0", 2));
        requireThrows<InvalidGzip>([&] { tree.decode(reader); });
    });
}};
