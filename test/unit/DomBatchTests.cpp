#include <webfront/testing/Bdd.hpp>
#include <webfront/weblink/DomBatch.hpp>

#include <algorithm>
#include <type_traits>

using namespace webfront::bdd;

using namespace webfront::msg;

namespace {
std::vector<std::byte> packet(std::initializer_list<unsigned> payload) {
    std::vector<std::byte> bytes{std::byte{5}, std::byte{1}, std::byte{0x78}, std::byte{0x56}, std::byte{0x34}, std::byte{0x12}};
    for (const auto value : payload)
        bytes.push_back(std::byte(value));
    return bytes;
}
}  // namespace

const Scenario dOMCommands{"DOM commands have a portable golden encoding and round trip every operation", [] {
    given("A little-endian correlation id and interned UTF-8 names", [&] {
        DomBatch batch(0x12345678);
        batch.add({DomOp::name, 0, 0, 0, "div"});
        batch.add({DomOp::element, 128, 0});
        then("The exact bytes use canonical LEB128 independently of host endianness", [&] {
            const auto expected = packet({1, 0, 3, 'd', 'i', 'v', 2, 0x80, 1, 0});
            require(std::ranges::equal(batch.data(), expected));
            require(batch.header().size() == 6);
            require(batch.payload().size() == expected.size() - 6);
        });
    });
    given("Every numbered operation, uint32 boundaries, and multi-byte UTF-8", [&] {
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
        then("Decoding recovers all fields without borrowing the input", [&] {
            const auto [id, decoded] = DomBatch::decode(batch.data());
            require(id == 0x12345678);
            require(decoded == expected);
        });
    });
}};

const Scenario dOMCodecs{"DOM codecs reject malformed input and enforce the complete command budget", [] {
    given("Malformed opcodes, varints, strings and listener flags", [&] {
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
        then("Each is rejected", [&] {
            for (const auto& bytes : invalid)
                requireThrows<std::invalid_argument>([&] { DomBatch::decode(bytes); });
            auto header = packet({});
            for (std::size_t size = 0; size < 6; ++size)
                requireThrows([&] { DomBatch::decode(std::span(header).first(size)); });
            header[0] = std::byte{4};
            requireThrows([&] { DomBatch::decode(header); });
            header[0] = std::byte{5};
            header[1] = std::byte{0};
            requireThrows([&] { DomBatch::decode(header); });
        });
    });
    given("A text operation filling exactly 1 MiB including headers", [&] {
        DomBatch batch(1);
        batch.add({DomOp::text, 1, 0, 0, std::string(DomBatch::maxBytes - 11, 'x')});
        then("The boundary succeeds, the next operation fails atomically, and the decoder rejects oversized input", [&] {
            require(batch.data().size() == DomBatch::maxBytes);
            require(DomBatch::decode(batch.data()).second.front().value.size() == DomBatch::maxBytes - 11);
            requireThrows<std::length_error>([&] { batch.add({DomOp::remove, 1}); });
            require(batch.data().size() == DomBatch::maxBytes);
            auto bytes = std::vector(batch.data().begin(), batch.data().end());
            bytes.push_back(std::byte{0});
            requireThrows<std::length_error>([&] { DomBatch::decode(bytes); });
        });
    });
    given("An invalid outgoing operation", [&] {
        DomBatch batch(1);
        then("No prefix is appended", [&] {
            requireThrows([&] { batch.add({DomOp::text, 1, 0, 0, std::string("\xc0\x80", 2)}); });
            requireThrows([&] { batch.add({DomOp::listen, 1, 2, 4}); });
            requireThrows([&] { batch.add({static_cast<DomOp>(255), 1}); });
            require(batch.data().size() == DomBatch::headerBytes);
        });
    });
}};

const Scenario dOMBatch{"DOM batch identifiers and operands are explicit and canonical", [] {
    static_assert(!(std::is_default_constructible_v<DomBatch>));
    given("the reserved uncorrelated identifier zero", [&] {
        then("neither encoder nor decoder accepts it as a real batch", [&] {
            requireThrows<std::invalid_argument>([&] { DomBatch(0); });
            auto raw = packet({});
            std::fill(raw.begin() + 2, raw.begin() + 6, std::byte{0});
            requireThrows<std::invalid_argument>([&] { DomBatch::decode(raw); });
        });
    });
    given("nonzero operands absent from the operation's wire layout", [&] {
        DomBatch                        batch(1);
        const std::vector<DomOperation> invalid{
            {DomOp::remove, 1, 2},
            {DomOp::text, 1, 0, 3, "text"},
            {DomOp::append, 1, 2, 3},
            {DomOp::listen, 1, 2, 0, "unused"}
        };
        then("each encoding fails without appending a prefix", [&] {
            for (const auto& op : invalid)
                requireThrows<std::invalid_argument>([&] { batch.add(op); });
            require(batch.data().size() == DomBatch::headerBytes);
        });
    });
}};

const Scenario dOMOperation{"DOM operation counts bound owning decoder allocation independently of wire bytes", [] {
    given("a batch containing exactly the maximum number of minimum-sized operations", [&] {
        DomBatch batch(1);
        for (std::size_t i = 0; i < DomBatch::maxOperations; ++i)
            batch.add({DomOp::remove, 1});
        then("the exact boundary decodes, and both codecs reject one additional operation", [&] {
            require(DomBatch::decode(batch.data()).second.size() == DomBatch::maxOperations);
            requireThrows<std::length_error>([&] { batch.add({DomOp::remove, 1}); });
            auto raw = std::vector(batch.data().begin(), batch.data().end());
            raw.push_back(std::byte(DomOp::remove));
            raw.push_back(std::byte{1});
            require(raw.size() < DomBatch::maxBytes);
            requireThrows<std::length_error>([&] { DomBatch::decode(raw); });
        });
    });
}};
