#pragma once
#include "../networking/Tasks.hpp"
#include "../networking/TCPNetworkingStdnet.hpp"
#include "Credentials.hpp"
#include "detail/Protocol.hpp"
#include <cassert>

#ifdef _MSC_VER
// C4737: MSVC cannot always turn the awaits of the handshake coroutines into tail calls. It costs stack depth, not correctness,
// and /WX would otherwise break every consumer of this header.
#pragma warning(push)
#pragma warning(disable : 4737)
#endif

namespace webfront::tls {
/// TCP-style stream provider. Handshake, reads and writes run on the same server I/O context.
class Server : public networking::TCPStdnet {
    using TCP  = networking::TCPStdnet;
    using Task = networking::Task;
    template <typename T>
    using Result = networking::ex::task<T>;

    using Byte  = detail::Byte;
    using Bytes = detail::Bytes;
    using View  = detail::View;

    struct Connection {
        detail::Traffic       read, write;
        detail::Secret<>      readSecret{}, writeSecret{};
        Bytes                 input;
        std::size_t           earlyBytes = 0;
        std::size_t           offset     = 0;
        detail::RecordControl control;
        bool                  compat         = false;
        bool                  writeEncrypted = false;
        bool                  established    = false;
    };

public:
    static constexpr bool secure = true;

    struct Acceptor : TCP::Acceptor {
        using TCP::Acceptor::Acceptor;
        std::shared_ptr<Credentials> credentials;

    private:
        friend class Server;
        mutable std::size_t pendingHandshakes = 0;  // Accessed on the acceptor's I/O thread.
    };

    struct Socket : TCP::Socket {
        explicit Socket(TCP::Socket&& accepted) : TCP::Socket(std::move(accepted)) {}

    private:
        friend class Server;
        std::unique_ptr<Connection> connection;
    };

    static auto Accept(Acceptor& acceptor) {
        return networking::ex::then(TCP::Accept(acceptor), [](TCP::Socket socket) {
            return Socket(std::move(socket));
        });
    }

    /// The time a connection has to complete its handshake. The caller enforces it (http::Server does).
    static constexpr std::chrono::seconds handshakeTimeout{10};

    // Completes on the I/O context. Its stop token cancels the pending reads and writes.
    static Task Handshake(Socket& socket, const Acceptor& acceptor) {
        constexpr std::size_t maxPendingHandshakes = 16;
        if (acceptor.pendingHandshakes >= maxPendingHandshakes)
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again), "TLS handshake limit reached");
        ++acceptor.pendingHandshakes;
        struct Slot {
            std::size_t& pending;
            ~Slot() { --pending; }
        } slot{acceptor.pendingHandshakes};
        co_await runHandshake(socket, acceptor);
    }

    static Result<std::size_t> Receive(Socket& socket, MutableBuffer target) {
        using namespace detail;
        if (target.size() == 0)
            throw std::invalid_argument("TLS: Receive requires a nonempty buffer");
        auto& state = *socket.connection;
        if (state.control.closed)
            co_return std::size_t{0};
        Byte failure = 0;
        try {
            while (!state.control.closed && !state.control.peerNotified) {
                if (state.offset != state.input.size()) {
                    const auto count = std::min(target.size(), state.input.size() - state.offset);
                    std::copy_n(state.input.data() + state.offset, count, static_cast<Byte*>(target.data()));
                    state.offset += count;
                    co_return count;
                }
                const auto type = co_await readRecord(socket, true);
                state.offset    = state.input.size();
                if (state.control.receive(type, state.input, state.read, state.readSecret))
                    state.offset = 0;
            }
        } catch (const Error& error) {
            failure = error.alert;
        } catch (...) {
            // A transport error, or an exception such as bad_alloc: the stream state is unknown, so no alert is attempted.
            state.control.transportLost();
            throw;
        }
        if (state.control.beginClose(failure))
            co_await finishClose(socket);
        if (failure)
            throw std::system_error(std::make_error_code(std::errc::protocol_error));
        co_return std::size_t{0};
    }

    // HTTP and WebSocket serialize Send calls on each socket. The active writer owns any pending close alert.
    static Task Send(Socket& socket, auto buffers) {
        using namespace detail;
        auto& state = *socket.connection;
        if (state.control.closed)
            throw std::system_error(std::make_error_code(std::errc::connection_reset));
        assert(!state.control.sending);
        state.control.sending = true;
        struct Sending {
            bool& active;
            ~Sending() {
                active = false;
            }
        } sending{state.control.sending};
        constexpr std::size_t header = 5;
        std::size_t           length = 0;
        for (const auto buffer : buffers) {
            length += std::min(buffer.size(), maxPlaintext - length);
            if (length == maxPlaintext)
                break;
        }
        Bytes record(header);
        record.reserve(header + length + 17);
        for (auto buffer : buffers) {
            View data{static_cast<const Byte*>(buffer.data()), buffer.size()};
            if (state.control.closed)
                break;
            while (!data.empty() && !state.control.closed) {
                const auto count = std::min(data.size(), maxPlaintext + header - record.size());
                append(record, data.first(count));
                data = data.subspan(count);
                if (record.size() == maxPlaintext + header) {
                    co_await writeApplication(socket, record);
                    record.resize(header);
                }
            }
        }
        if (!state.control.closed && record.size() != header)
            co_await writeApplication(socket, record);
        if (state.control.closed) {
            co_await finishClose(socket);
            throw std::system_error(std::make_error_code(std::errc::connection_aborted));
        }
    }

    // If a send is in progress, that writer owns the final alert and shutdown.
    static Task Close(Socket& socket) {
        if (socket.connection->control.beginClose())
            co_await finishClose(socket);
    }

private:
    static Task runHandshake(Socket& socket, const Acceptor& acceptor) {
        std::optional<detail::Error> failure;
        try {
            socket.connection = std::make_unique<Connection>();
            co_await negotiate(socket, acceptor.credentials->snapshot());
            co_return;
        } catch (const detail::Error& error) {
            failure.emplace(error);
        }
        try {
            const std::array<Byte, 2> alert{2, failure->alert};
            co_await writeRecord(socket, detail::alert, alert);
        } catch (const std::system_error&) {}
        throw std::system_error(std::make_error_code(std::errc::protocol_error), failure->what());
    }

    static Task finishClose(Socket& socket) {
        const auto failure = socket.connection->control.takeAlert();
        if (!failure)
            co_return;
        const std::array<Byte, 2> response{static_cast<Byte>(*failure ? 2 : 1), *failure};
        try {
            co_await writeRecord(socket, detail::alert, response);
        } catch (const std::system_error&) {
            // An alert is best effort once the peer has closed the transport.
        }
        std::error_code ignored;
        socket.shutdown(Socket::shutdown_both, ignored);
    }

    static Task readExact(Socket& socket, std::span<Byte> target) {
        while (!target.empty()) {
            const auto count = co_await TCP::Receive(socket, Buffer(target.data(), target.size()));
            if (count == 0)
                throw std::system_error(std::make_error_code(std::errc::connection_reset));
            target = target.subspan(count);
        }
    }

    static Result<Byte> readRecord(Socket& socket, bool encrypted, bool initial = false) {
        using namespace detail;
        auto& state = *socket.connection;
        for (;;) {
            std::array<Byte, 5> header;
            co_await readExact(socket, header);
            const auto [type, size] = parseRecord(header, encrypted || state.earlyBytes != 0);
            if (size > state.input.capacity())
                state.input.reserve(size);
            state.input.resize(size);
            co_await readExact(socket, state.input);
            if (type == changeCipherSpec && !state.established && !initial && !state.compat && state.input == Bytes{1}) {
                state.compat = true;
                continue;
            }
            if (type == applicationData) {
                if (encrypted) {
                    try {
                        const auto inner = state.read.open(header, state.input);
                        state.earlyBytes = 0;
                        co_return inner;
                    } catch (const Error& error) {
                        if (error.alert != 20 || state.earlyBytes == 0)
                            throw;
                    }
                }
                if (state.earlyBytes != 0) {
                    const auto wireSize = size + header.size();
                    if (wireSize > state.earlyBytes)
                        throw Error("too much rejected early data", 10);
                    state.earlyBytes -= wireSize;
                    continue;
                }
            }
            if (!encrypted && (type == handshakeRecord || type == alert)) {
                state.earlyBytes = 0;  // rejected early data can only precede a ClientHello
                co_return type;
            }
            throw Error("unexpected record type", 10);
        }
    }

    static Task writeRecord(Socket& socket, Byte type, View body) {
        using namespace detail;
        auto& state = *socket.connection;
        Bytes record{type, 3, 3};
        record.reserve(5 + body.size() + (state.writeEncrypted ? 17 : 0));
        put(record, body.size(), 2);
        append(record, body);
        if (state.writeEncrypted)
            state.write.seal(type, record);
        co_await sendRecord(socket, record);
    }

    static Task sendRecord(Socket& socket, const Bytes& record) {
        const std::array buffers{Buffer(record)};
        co_await TCP::Send(socket, buffers);
    }

    static Task writeApplication(Socket& socket, Bytes& record) {
        using namespace detail;
        auto& state = *socket.connection;
        if (state.control.closed)
            throw std::system_error(std::make_error_code(std::errc::connection_reset));
        while (std::exchange(state.control.updateRequested, false)) {
            constexpr std::array<Byte, 5> update{24, 0, 0, 1, 0};
            co_await writeRecord(socket, handshakeRecord, update);
            state.writeSecret = {expandLabel(state.writeSecret, "traffic upd")};
            state.write.install(state.writeSecret);
            if (state.control.closed)
                co_return;
        }
        state.write.seal(applicationData, record);
        co_await sendRecord(socket, record);
    }

    static Result<Bytes> readHandshake(Socket& socket, unsigned limit, bool encrypted, bool initial = false) {
        using namespace detail;
        Bytes       message;
        std::size_t expected = 4;
        do {
            const auto  type  = co_await readRecord(socket, encrypted, initial);
            const auto& input = socket.connection->input;
            if (type == alert && input.size() == 2)
                throw std::system_error(std::make_error_code(std::errc::connection_aborted));
            if (type != handshakeRecord)
                throw Error("expected handshake record", 10);
            if (input.empty() || input.size() > limit - message.size())
                throw Error("invalid handshake fragment");
            append(message, input);
            if (message.size() >= 4) {
                expected = 4 + Reader{View(message).subspan(1)}.number(3);
                if (expected > limit || message.size() > expected)
                    throw Error("invalid handshake size");
            }
        } while (message.size() < expected);
        co_return message;
    }

    static Task writeHandshake(Socket& socket, detail::Sha256& transcript, View message) {
        transcript.update(message);
        while (!message.empty()) {
            const auto fragment = message.first(std::min<std::size_t>(message.size(), detail::maxPlaintext));
            co_await writeRecord(socket, detail::handshakeRecord, fragment);
            message = message.subspan(fragment.size());
        }
    }

#ifdef _MSC_VER
// MSVC reports C4737 as an error from its optimizer. The handshake stages run once per connection: build them unoptimized,
// and keep the data path (Receive, Send) optimized.
#pragma optimize("", off)
#endif
    // MSVC refuses (C4737) one coroutine holding the whole flight, so the handshake runs in three stages.
    static Task negotiate(Socket& socket, std::shared_ptr<const detail::Identity> identity) {
        using namespace detail;
        Sha256         transcript;
        Bytes          message;
        ClientHello    hello;
        Secret<>       shared{}, clientHandshake{};
        Digest         afterFinished{};
        co_await receiveHello(socket, transcript, message, hello, shared);
        co_await sendFlight(socket, transcript, *identity, hello.alpn, shared, clientHandshake, afterFinished);
        co_await receiveFinished(socket, clientHandshake, afterFinished);
    }

    // From the first ClientHello to the ServerHello, including a HelloRetryRequest. Fills the X25519 shared secret.
    static Task receiveHello(Socket& socket, detail::Sha256& transcript, detail::Bytes& message, detail::ClientHello& hello,
                             detail::Secret<>& shared) {
        using namespace detail;
        auto& state = *socket.connection;
        message     = co_await readHandshake(socket, 65536, false, true);
        hello       = parseHello(message, false);
        if (hello.earlyData)
            state.earlyBytes = 64 * 1024;
        bool                          retry = hello.share.empty();
        constexpr std::array<Byte, 1> ccs{1};
        if (retry) {
            const auto first  = hello.invariant;
            const bool compat = !hello.session.empty();
            transcript.update(handshake(254, hash(message)));
            co_await writeHandshake(socket, transcript, serverHello(hello.session, retryRandom, {}));
            if (compat)
                co_await writeRecord(socket, changeCipherSpec, ccs);
            message          = co_await readHandshake(socket, 65536, false);
            state.earlyBytes = 0;
            hello            = parseHello(message, true);
            if (hello.invariant != first || hello.share.empty() || hello.shareCount != 1)
                throw Error("invalid second ClientHello", 47);
        }
        transcript.update(message);
        Digest   random{}, publicKey{};
        Secret<> scalar{};
        entropy(random);
        entropy(scalar);
        publicKey = x25519(scalar, Digest{9});
        shared    = {x25519(scalar, hello.share.first<32>())};
        if (equalSecret(shared, zeros))
            throw Error("invalid X25519 shared secret", 47);
        co_await writeHandshake(socket, transcript, serverHello(hello.session, random, publicKey));
        if (!retry && !hello.session.empty())
            co_await writeRecord(socket, changeCipherSpec, ccs);
    }

    // Plain functions: fewer locals live across the awaits of sendFlight, which MSVC compiles with a missed tail call.
    static detail::Bytes encryptedExtensionsMessage(bool alpn) {
        using namespace detail;
        Bytes extensions;
        if (alpn) {
            Bytes protocol;
            vector(protocol, bytes("http/1.1"), 1);
            Bytes protocols;
            vector(protocols, protocol, 2);
            extension(extensions, 16, protocols);
        }
        Bytes body;
        vector(body, extensions, 2);
        return handshake(8, body);
    }

    static detail::Bytes certificateVerifyMessage(const detail::Identity& identity, const detail::Digest& transcriptHash) {
        using namespace detail;
        Bytes input(64, 0x20);
        append(input, bytes("TLS 1.3, server CertificateVerify"));
        input.push_back(0);
        append(input, transcriptHash);
        Bytes verify;
        put(verify, signatureScheme, 2);
        vector(verify, sign(identity.key, hash(input)), 2);
        return handshake(15, verify);
    }

    // A braced list inside a co_await expression is the construct MSVC cannot compile here (C4737): keep it out of the coroutine.
    static detail::Bytes finishedMessage(const detail::Secret<>& finishedKey, const detail::Digest& transcriptHash) {
        return detail::handshake(20, detail::hmac(finishedKey, {transcriptHash}));
    }

    // EncryptedExtensions to server Finished, then the application keys.
    static Task sendFlight(Socket& socket, detail::Sha256& transcript, const detail::Identity& identity, bool alpn, const detail::Secret<>& shared,
                           detail::Secret<>& client, detail::Digest& afterFinished) {
        using namespace detail;
        auto&          state = *socket.connection;
        const auto     empty = hash();
        const Secret<> early{hmac(zeros, {zeros})};
        const Secret<> derived{expandLabel(early, "derived", empty)};
        const Secret<> secret{hmac(derived, {shared})};
        const auto     afterHello = transcript.digest();
        client                    = {expandLabel(secret, "c hs traffic", afterHello)};
        const Secret<> server{expandLabel(secret, "s hs traffic", afterHello)};
        state.read.install(client);
        state.write.install(server);
        state.writeEncrypted = true;
        co_await writeHandshake(socket, transcript, encryptedExtensionsMessage(alpn));
        co_await writeHandshake(socket, transcript, identity.certificateMessage);
        co_await writeHandshake(socket, transcript, certificateVerifyMessage(identity, transcript.digest()));
        const Secret<> serverFinished{expandLabel(server, "finished")};
        co_await writeHandshake(socket, transcript, finishedMessage(serverFinished, transcript.digest()));

        const Secret<> masterSalt{expandLabel(secret, "derived", empty)};
        const Secret<> master{hmac(masterSalt, {zeros})};
        afterFinished     = transcript.digest();
        state.readSecret  = {expandLabel(master, "c ap traffic", afterFinished)};
        state.writeSecret = {expandLabel(master, "s ap traffic", afterFinished)};
        state.write.install(state.writeSecret);
    }

    static Task receiveFinished(Socket& socket, const detail::Secret<>& client, const detail::Digest& afterFinished) {
        using namespace detail;
        auto&      state    = *socket.connection;
        const auto finished = co_await readHandshake(socket, 36, true);
        Reader     reader{finished};
        if (reader.number(1) != 20)
            throw Error("expected client Finished", 10);
        const auto verification = reader.vector(3);
        reader.end();
        const Secret<> clientFinished{expandLabel(client, "finished")};
        const auto     expected = hmac(clientFinished, {afterFinished});
        if (!equalSecret(expected, verification))
            throw Error("invalid client Finished", 51);
        state.read.install(state.readSecret);
        state.input.clear();
        state.established = true;
    }
#ifdef _MSC_VER
#pragma optimize("", on)
#endif
};
}  // namespace webfront::tls

#ifdef _MSC_VER
#pragma warning(pop)
#endif
