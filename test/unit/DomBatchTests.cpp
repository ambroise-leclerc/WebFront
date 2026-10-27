#include <catch2/catch_test_macros.hpp>
#include <webfront/weblink/DomBatch.hpp>

#include <algorithm>

using namespace webfront::msg;

namespace {
std::vector<std::byte> packet(std::initializer_list<unsigned> payload) {
    std::vector<std::byte> bytes{std::byte{5}, std::byte{1}, std::byte{0x78}, std::byte{0x56}, std::byte{0x34}, std::byte{0x12}};
    for (const auto value : payload)
        bytes.push_back(std::byte(value));
    return bytes;
}
}  // namespace

SCENARIO("DOM commands have a portable golden encoding and round trip every operation") {
    GIVEN("A little-endian correlation id and interned UTF-8 names") {
        DomBatch batch(0x12345678);
        batch.add({DomOp::name, 0, 0, 0, "div"});
        batch.add({DomOp::element, 128, 0});
        THEN("The exact bytes use canonical LEB128 independently of host endianness") {
            const auto expected = packet({1, 0, 3, 'd', 'i', 'v', 2, 0x80, 1, 0});
            REQUIRE(std::ranges::equal(batch.data(), expected));
            REQUIRE(batch.header().size() == 6);
            REQUIRE(batch.payload().size() == expected.size() - 6);
        }
    }
    GIVEN("Every numbered operation, uint32 boundaries, and multi-byte UTF-8") {
        const std::vector<DomOperation> expected{
            {DomOp::name, 0, 0, 0, "\xef\xbb\xbf"},
            {DomOp::element, 0xffffffff, 127},
            {DomOp::text, 128, 0, 0, "été 🌍"},
            {DomOp::attribute, 1, 2, 0, "title"},
            {DomOp::unsetAttribute, 1, 2},
            {DomOp::property, 1, 2, 0, "false"},
            {DomOp::setText, 128, 0, 0, ""},
            {DomOp::append, 1, 128},
            {DomOp::insertBefore, 1, 2, 128},
            {DomOp::remove, 2},
            {DomOp::listen, 1, 3, 3},
            {DomOp::unlisten, 1, 3},
            {DomOp::mount, 1, 4}
        };
        DomBatch batch(0x12345678);
        for (const auto& op : expected)
            batch.add(op);
        THEN("Decoding recovers all fields without borrowing the input") {
            const auto [id, decoded] = DomBatch::decode(batch.data());
            REQUIRE(id == 0x12345678);
            REQUIRE(decoded == expected);
        }
    }
}

SCENARIO("DOM codecs reject malformed input and enforce the complete command budget") {
    GIVEN("Malformed opcodes, varints, strings and listener flags") {
        const std::vector<std::vector<std::byte>> invalid{packet({0}),
                                                          packet({14}),
                                                          packet({10}),
                                                          packet({10, 0x80}),
                                                          packet({10, 0x80, 0}),
                                                          packet({10, 0xff, 0xff, 0xff, 0xff, 0x10}),
                                                          packet({10, 0x80, 0x80, 0x80, 0x80, 0x80, 1}),
                                                          packet({3, 1, 3, 'x'}),
                                                          packet({3, 1, 2, 0xc0, 0x80}),
                                                          packet({3, 1, 3, 0xed, 0xa0, 0x80}),
                                                          packet({3, 1, 4, 0xf4, 0x90, 0x80, 0x80}),
                                                          packet({3, 1, 1, 0xc2}),
                                                          packet({3, 1, 2, 0xc2, 'a'}),
                                                          packet({11, 1, 2, 4})};
        THEN("Each is rejected") {
            for (const auto& bytes : invalid)
                REQUIRE_THROWS(DomBatch::decode(bytes));
            auto header = packet({});
            for (std::size_t size = 0; size < 6; ++size)
                REQUIRE_THROWS(DomBatch::decode(std::span(header).first(size)));
            header[0] = std::byte{4};
            REQUIRE_THROWS(DomBatch::decode(header));
            header[0] = std::byte{5};
            header[1] = std::byte{0};
            REQUIRE_THROWS(DomBatch::decode(header));
        }
    }
    GIVEN("A text operation filling exactly 1 MiB including headers") {
        DomBatch batch;
        batch.add({DomOp::text, 1, 0, 0, std::string(DomBatch::maxBytes - 11, 'x')});
        THEN("The boundary succeeds, the next operation fails atomically, and the decoder rejects oversized input") {
            REQUIRE(batch.data().size() == DomBatch::maxBytes);
            REQUIRE(DomBatch::decode(batch.data()).second.front().value.size() == DomBatch::maxBytes - 11);
            REQUIRE_THROWS_AS(batch.add({DomOp::remove, 1}), std::length_error);
            REQUIRE(batch.data().size() == DomBatch::maxBytes);
            auto bytes = std::vector(batch.data().begin(), batch.data().end());
            bytes.push_back(std::byte{0});
            REQUIRE_THROWS_AS(DomBatch::decode(bytes), std::length_error);
        }
    }
    GIVEN("An invalid outgoing operation") {
        DomBatch batch;
        THEN("No prefix is appended") {
            REQUIRE_THROWS(batch.add({DomOp::text, 1, 0, 0, std::string("\xc0\x80", 2)}));
            REQUIRE_THROWS(batch.add({DomOp::listen, 1, 2, 4}));
            REQUIRE_THROWS(batch.add({static_cast<DomOp>(255), 1}));
            REQUIRE(batch.data().size() == DomBatch::headerBytes);
        }
    }
}
