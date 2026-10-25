/** @brief Lazy, server-owned gzip worker with bounded jobs, waiters and owned payload storage. */
#pragma once
#include "../networking/Tasks.hpp"
#include "Gzip.hpp"

#include <atomic>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace webfront::http::gzip {

struct DecodeLimits {
    std::size_t jobs    = DefaultBuffersPolicy::maxGzipDecodeJobs;
    std::size_t waiters = DefaultBuffersPolicy::maxGzipDecodeWaiters;
    std::size_t bytes   = DefaultBuffersPolicy::maxGzipDecodeBytes;
    std::size_t encoded = DefaultBuffersPolicy::maxGzipEncodedAssetSize;
};

template <BuffersPolicyType Policy>
constexpr DecodeLimits decodeLimits() {
    DecodeLimits limits;
    if constexpr (requires { Policy::maxGzipDecodeJobs; })
        limits.jobs = Policy::maxGzipDecodeJobs;
    if constexpr (requires { Policy::maxGzipDecodeWaiters; })
        limits.waiters = Policy::maxGzipDecodeWaiters;
    if constexpr (requires { Policy::maxGzipDecodeBytes; })
        limits.bytes = Policy::maxGzipDecodeBytes;
    if constexpr (requires { Policy::maxGzipEncodedAssetSize; })
        limits.encoded = Policy::maxGzipEncodedAssetSize;
    return limits;
}

struct Decoder {
    bool operator()(std::string_view encoded, std::string& output, std::size_t limit) const {
        return decodeGzipInto(encoded, output, limit);
    }
};

// An internal executor seam lets deterministic tests gate the real decoder without delaying I/O.
template <typename Decode = Decoder>
class DecodeExecutor {
    struct Work;
    struct Waiter {
        virtual bool stopped() const noexcept = 0;
        virtual void complete() noexcept      = 0;
        virtual ~Waiter()                     = default;
    };
    struct State {
        explicit State(DecodeLimits value) : limits(value) {}
        DecodeLimits                       limits;
        std::mutex                         mutex;
        std::condition_variable            changed;
        std::atomic_bool                   stopping{false};
        std::vector<std::weak_ptr<Work>>   revisions;
        std::vector<std::shared_ptr<Work>> queue;
        std::atomic_size_t                 jobs{0}, waiters{0}, bytes{0}, decodes{0};
    };
    struct Work {
        explicit Work(std::shared_ptr<State> owner) : state(std::move(owner)) {}
        std::shared_ptr<State> state;
        std::string            encoded;
        std::string            output;
        std::size_t            limit{};
        std::size_t            charge{};
        std::vector<Waiter*>   waiters;
        bool                   complete = false;
        bool                   valid    = false;
        bool                   decoded  = false;
        ~Work() {
            if (charge) {
                std::string{}.swap(encoded);
                std::string{}.swap(output);
                state->bytes.fetch_sub(charge);
                state->jobs.fetch_sub(1);
            }
        }
    };

public:
    /** @brief Owns the decode and the reservation for its eventual HTTP body until send completes. */
    class Lease {
        friend class DecodeExecutor;
        std::shared_ptr<State> state;
        std::shared_ptr<Work>  work;
        std::string            body;
        std::size_t            charge{};
        bool                   head{};
        Lease(std::shared_ptr<Work> value, std::string buffer, bool isHead)
            : state(value->state), work(std::move(value)), body(std::move(buffer)), charge(body.capacity() + 1), head(isHead) {}
        Lease(std::shared_ptr<State> owner, std::size_t bytes) : state(std::move(owner)), charge(bytes) {}

    public:
        Lease()                        = default;
        Lease(const Lease&)            = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept
            : state(std::move(other.state)), work(std::move(other.work)), body(std::move(other.body)), charge(other.charge), head(other.head) {}
        ~Lease() {
            if (state) {
                std::string{}.swap(body);
                state->bytes.fetch_sub(charge);
                state->waiters.fetch_sub(1);
            }
        }
        explicit operator bool() const {
            return static_cast<bool>(state);
        }
        bool valid() const {
            return work->valid;
        }
        std::string_view encoded() const {
            return work->encoded;
        }
        std::string_view decoded() const {
            return work->output;
        }
        std::string takeBody() {
            if (!head)
                body.assign(work->output);
            return std::move(body);
        }
    };

    struct Statistics {
        std::size_t jobs, waiters, bytes, decodes;
        bool        workerStarted;
    };
    explicit DecodeExecutor(DecodeLimits limits = {}, Decode value = {}) : state(std::make_shared<State>(limits)), decode(std::move(value)) {}
    DecodeExecutor(const DecodeExecutor&)            = delete;
    DecodeExecutor& operator=(const DecodeExecutor&) = delete;
    ~DecodeExecutor() {
        stop();
        join();
    }

    void stop() noexcept {
        state->stopping.store(true);
        // Pair with the worker's wait: a stop notification cannot be lost between predicate and sleep.
        { std::lock_guard lock(state->mutex); }
        state->changed.notify_all();
    }
    // Called after external run() callers have returned. Active decoding is non-interruptible.
    void join() noexcept {
        if (worker.joinable())
            worker.join();
    }
    Statistics statistics() const {
        return {state->jobs.load(), state->waiters.load(), state->bytes.load(), state->decodes.load(), started.load()};
    }

    /** @brief Reserve a warm identity response until its network send releases this lease. */
    Lease reserveResponse(std::size_t capacity) {
        std::lock_guard lock(state->mutex);
        const auto      used = state->bytes.load();
        if (state->stopping.load() || state->waiters.load() >= state->limits.waiters || used >= state->limits.bytes || capacity >= state->limits.bytes - used)
            return {};
        state->bytes.fetch_add(capacity + 1);
        state->waiters.fetch_add(1);
        return Lease(state, capacity + 1);
    }

    struct Sender {
        using sender_concept        = networking::ex::sender_t;
        using completion_signatures = networking::ex::
            completion_signatures<networking::ex::set_value_t(Lease), networking::ex::set_error_t(std::exception_ptr), networking::ex::set_stopped_t()>;
        DecodeExecutor* executor;
        std::string     encoded;
        std::size_t     limit;
        bool            head;

        template <typename Receiver>
        struct Operation : Waiter {
            DecodeExecutor*      executor;
            Receiver             receiver;
            std::string          encoded;
            std::size_t          limit;
            bool                 head;
            std::optional<Lease> lease;
            Operation(DecodeExecutor* owner, Receiver value, std::string bytes, std::size_t bound, bool isHead)
                : executor(owner), receiver(std::move(value)), encoded(std::move(bytes)), limit(bound), head(isHead) {}
            Operation(const Operation&) = delete;
            Operation(Operation&&)      = delete;
            bool stopped() const noexcept override {
                return executor->state->stopping.load() || networking::ex::get_stop_token(networking::ex::get_env(receiver)).stop_requested();
            }
            void complete() noexcept override {
                // Rejected/cancelled input must not survive in a pending I/O completion.
                std::string{}.swap(encoded);
                if (stopped()) {
                    lease.reset();
                    networking::ex::set_stopped(std::move(receiver));
                } else {
                    networking::ex::set_value(std::move(receiver), lease ? std::move(*lease) : Lease{});
                }
            }
            void start() noexcept {
                try {
                    if (stopped() || executor->admit(*this))
                        complete();
                } catch (...) {
                    std::string{}.swap(encoded);
                    networking::ex::set_error(std::move(receiver), std::current_exception());
                }
            }
        };
        template <typename Receiver>
        auto connect(Receiver receiver) && {
            return Operation<Receiver>(executor, std::move(receiver), std::move(encoded), limit, head);
        }
    };

    Sender submit(std::string encoded, std::size_t limit, bool head = false) {
        return {this, std::move(encoded), limit, head};
    }

private:
    std::shared_ptr<State> state;
    Decode                 decode;
    std::jthread           worker;
    std::atomic_bool       started{false};

    template <typename Operation>
    bool admit(Operation& operation) {
        std::unique_lock lock(state->mutex);
        if (state->stopping.load())
            return true;
        const auto limits = state->limits;
        if (operation.encoded.size() > limits.encoded || state->waiters.load() >= limits.waiters)
            return true;
        std::erase_if(state->revisions, [](const auto& entry) {
            return entry.expired();
        });
        std::shared_ptr<Work> work;
        for (const auto& revision : state->revisions) {
            auto candidate = revision.lock();
            if (candidate && (!candidate->complete || candidate->decoded) && candidate->limit == operation.limit && candidate->encoded == operation.encoded) {
                work = std::move(candidate);
                break;
            }
        }
        const bool fresh = !work;
        if (fresh && state->jobs.load() >= limits.jobs)
            return true;
        // Guard arithmetic and reject before allocating full output/body buffers.
        const auto used     = state->bytes.load();
        const auto bodySize = operation.head ? std::size_t{0} : operation.limit;
        if (used > limits.bytes || bodySize > limits.bytes - used)
            return true;
        if (fresh
            && (operation.encoded.capacity() > limits.bytes - used - bodySize
                || operation.limit > limits.bytes - used - bodySize - operation.encoded.capacity()))
            return true;
        std::string body;
        if (!operation.head)
            body.reserve(operation.limit);
        std::size_t cost = body.capacity() + 1;
        if (fresh) {
            work          = std::make_shared<Work>(state);
            work->limit   = operation.limit;
            work->encoded = std::move(operation.encoded);
            work->output.reserve(operation.limit);
            work->waiters.reserve(limits.waiters);
            state->queue.reserve(limits.jobs);
            state->revisions.reserve(limits.jobs);
            const auto key = work->encoded.capacity() + 1;
            if (key > limits.bytes - used || work->output.capacity() >= limits.bytes - used - key)
                return true;
            work->charge = key + work->output.capacity() + 1;
            if (cost > limits.bytes - used - work->charge) {
                work->charge = 0;
                return true;
            }
            // Start only on an admissible miss. Allocation/thread-creation failures leave no queued work.
            try {
                if (!worker.joinable()) {
                    worker = std::jthread([owner = state, decoder = decode] {
                        run(owner, decoder);
                    });
                    started.store(true);
                }
            } catch (...) {
                work->charge = 0;
                throw;
            }
            state->jobs.fetch_add(1);
            state->bytes.fetch_add(work->charge);
            state->revisions.push_back(work);
            state->queue.push_back(work);
        } else if (cost > limits.bytes - used)
            return true;
        std::string{}.swap(operation.encoded);
        state->bytes.fetch_add(cost);
        state->waiters.fetch_add(1);
        operation.lease.emplace(Lease(work, std::move(body), operation.head));
        if (work->complete)
            return true;
        work->waiters.push_back(&operation);
        lock.unlock();
        state->changed.notify_one();
        return false;
    }

    static void run(const std::shared_ptr<State>& state, const Decode& decoder) noexcept {
        for (;;) {
            std::shared_ptr<Work> work;
            {
                std::unique_lock lock(state->mutex);
                state->changed.wait(lock, [&] {
                    return state->stopping.load() || !state->queue.empty();
                });
                if (state->queue.empty())
                    return;
                work = std::move(state->queue.front());
                state->queue.erase(state->queue.begin());
                if (!state->stopping.load() && std::any_of(work->waiters.begin(), work->waiters.end(), [](auto* waiter) {
                        return !waiter->stopped();
                    })) {
                    lock.unlock();
                    state->decodes.fetch_add(1);
                    try {
                        work->valid   = decoder(work->encoded, work->output, work->limit);
                        work->decoded = true;
                    } catch (...) {
                        work->output.clear();
                    }
                    lock.lock();
                }
                work->complete = true;
            }
            // Each completion may destroy its operation. No operation pointer is touched afterward.
            for (auto* waiter : work->waiters)
                waiter->complete();
            work->waiters.clear();
        }
    }
};
}  // namespace webfront::http::gzip
