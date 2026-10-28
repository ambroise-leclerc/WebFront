/// @brief Optional local developer session controller. Permissions are fixed by its native owner.
#pragma once

#include "../system/DeveloperPaths.hpp"
#include "Contract.hpp"

#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <random>

namespace webfront::automation {
struct SessionOptions {
    std::filesystem::path    project;
    std::vector<std::string> allow;
    std::chrono::seconds     timeout{120};
};

class Session {
    using Clock = std::chrono::steady_clock;
    struct Run {
        std::string       id, operation, arguments, state{"queued"}, result{"null"}, output;
        bool              cancel = false;
        Clock::time_point deadline;
    };

public:
    explicit Session(SessionOptions configuration) : options(std::move(configuration)) {
        this->options.project = developer::canonicalDirectory(this->options.project);
        if (this->options.timeout < std::chrono::seconds{1} || this->options.timeout > std::chrono::seconds{3600})
            throw std::invalid_argument("Automation timeout must be 1..3600 seconds");
        for (const auto& permission : this->options.allow)
            if (!knownPermission(permission))
                throw std::invalid_argument("Unknown automation permission: " + permission);
        std::random_device random;
        for (int i = 0; i < 4; ++i)
            identity += std::to_string(random()) + "-";
    }

    std::string describe() const {
        std::scoped_lock lock{mutex};
        return "{\"version\":1,\"session\":" + json::quote(identity) + ",\"project\":" + json::quote(options.project.generic_string()) + ",\"state\":"
               + json::quote(closed  ? "closed"
                             : ready ? "ready"
                                     : "starting")
               + ",\"allow\":" + stringArray(options.allow) + ",\"failure\":" + json::quote(failure) + ",\"audit\":" + auditJson() + "}";
    }

    std::string start(const Object& request) {
        request.only({"session", "operation", "arguments"});
        std::scoped_lock lock{mutex};
        try {
            authorize(request);
            if (!ready || closed)
                throw OperationError("not_ready", "Developer session is not ready");
            if (active)
                throw OperationError("busy", "Developer session is busy");
            const auto  operation  = request.string("operation");
            const auto& permission = operationDefinition(operation).permission;
            if (!knownPermission(permission) || std::ranges::find(options.allow, permission) == options.allow.end())
                throw OperationError("not_authorized", "Automation operation is not authorized: " + operation);
            const Object arguments{request.raw("arguments", "{}")};
            validateArguments(operation, arguments);
            if (runs.size() == 64)
                runs.pop_front();  // no active run is evicted; only one may execute
            const auto id = "run-" + std::to_string(++counter);
            runs.push_back({id, operation, objectJson(arguments.members()), "queued", "null", "", false, Clock::now() + options.timeout});
            active = id;
            audit("allowed", operation, id);
            return snapshot(runs.back());
        } catch (...) {
            audit("denied", "start", "");
            throw;
        }
    }

    std::string result(const Object& request) const {
        request.only({"session", "runId"});
        std::scoped_lock lock{mutex};
        authorize(request);
        return snapshot(find(request.string("runId")));
    }

    std::string cancel(const Object& request) {
        request.only({"session", "runId"});
        std::scoped_lock lock{mutex};
        authorize(request);
        auto& run = find(request.string("runId"));
        if (active && *active == run.id)
            run.cancel = true;
        return snapshot(run);
    }

    // Only the browser selected by the native binder may call these methods.
    std::string poll(std::string output = {}) {
        std::scoped_lock lock{mutex};
        if (closed)
            return "{\"close\":true}";
        ready = true;
        if (!active)
            return "null";
        auto& run = find(*active);
        if (output.size() > 8192)
            throw std::invalid_argument("Automation progress exceeds 8192 bytes");
        run.output = std::move(output);
        if (run.cancel) {
            if (run.state == "queued") {
                run.state = "cancelled";
                active.reset();
                return "null";
            }
            return "{\"cancel\":" + json::quote(run.id) + "}";
        }
        if (run.state != "queued")
            return "null";
        run.state = "running";
        return "{\"runId\":" + json::quote(run.id) + ",\"operation\":" + json::quote(run.operation) + ",\"arguments\":" + run.arguments + "}";
    }

    // A completion the page cannot make valid (for example text that is not UTF-8) must not leave the run active until the
    // deadline revokes the whole session: the run fails, with the reason as its output.
    void complete(std::string_view id, std::string result) {
        std::scoped_lock lock{mutex};
        if (!active || *active != id)
            throw std::runtime_error("Unexpected or late automation completion");
        auto&       run    = find(*active);
        int         status = 1;
        std::string output;
        try {
            const Object checked{result};
            status = completionStatus(checked);
            output = checked.string("output");
        } catch (const std::exception& error) {
            std::string reason{error.what()};
            if (reason.size() > 1024) {
                std::size_t end = 1024;
                while (end && (static_cast<unsigned char>(reason[end]) & 0xC0) == 0x80)
                    --end;
                reason.resize(end);
            }
            output = "Invalid automation completion: " + reason;
            result = objectJson({
                {"exitCode",                 "1"},
                {  "output", json::quote(output)}
            });
        }
        run.result = std::move(result);
        run.output = std::move(output);
        run.state  = status == 0 ? "succeeded" : status == 130 ? "cancelled" : "failed";
        audit(run.state, run.operation, run.id);
        active.reset();
    }

    void close(std::string reason = {}) {
        std::scoped_lock lock{mutex};
        closeLocked(std::move(reason));
    }

    // A deadline closes this session: the renderer may still execute code, so it must not be reused.
    bool expire(Clock::time_point now = Clock::now()) {
        std::scoped_lock lock{mutex};
        if (!active || now < find(*active).deadline)
            return false;
        auto& run = find(*active);
        run.state = "timeout";
        audit("timeout", run.operation, run.id);
        active.reset();
        closeLocked("Native operation deadline exceeded; session revoked");
        return true;
    }

    bool isClosed() const {
        std::scoped_lock lock{mutex};
        return closed;
    }
    bool wasReady() const {
        std::scoped_lock lock{mutex};
        return ready;
    }

private:
    SessionOptions             options;
    std::string                identity, failure;
    mutable std::mutex         mutex;
    std::deque<Run>            runs;
    std::deque<std::string>    auditEntries;
    std::optional<std::string> active;
    std::uint64_t              counter = 0;
    bool                       ready = false, closed = false;

    void audit(std::string_view decision, std::string_view operation, std::string_view run) {
        if (auditEntries.size() == 64)
            auditEntries.pop_front();
        auditEntries.push_back(objectJson({
            { "decision",  json::quote(decision)},
            {"operation", json::quote(operation)},
            {    "runId",       json::quote(run)}
        }));
    }
    std::string auditJson() const {
        std::string value = "[";
        for (const auto& entry : auditEntries)
            value += (value.size() > 1 ? "," : "") + entry;
        return value + "]";
    }

    static int completionStatus(const Object& checked) {
        checked.only({"exitCode", "output", "report", "truncated", "data", "diagnostics"});
        const auto code         = checked.raw("exitCode");
        int        status       = -1;
        const auto [end, error] = std::from_chars(code.data(), code.data() + code.size(), status);
        if (error != std::errc{} || end != code.data() + code.size() || status < 0 || status > 255)
            throw std::invalid_argument("Invalid automation exitCode");
        checked.string("output");
        return status;
    }
    static bool knownPermission(std::string_view name) {
        for (const auto& [operation, definition] : operationDefinitions())
            if (definition.permission == name)
                return true;
        return false;
    }
    void authorize(const Object& request) const {
        if (request.string("session") != identity)
            throw OperationError("wrong_session", "Wrong automation session");
    }
    static void validateArguments(std::string_view operation, const Object& args) {
        validateOperation(operation, args);
        for (const auto& [key, raw] : args.members()) {
            const auto value = stringValue(raw);
            if (key == "entry" && !value.empty() && (value.starts_with('-') || value.contains('\\') || value.starts_with('/')))
                throw std::invalid_argument("Entry must be a project-relative path or manifest entry");
            if (key == "entry" && !value.empty())
                fs::safeRelativePath(value);
            if (key == "pattern" && value.starts_with('-'))
                throw std::invalid_argument("Pattern must not be an option");
        }
    }
    Run& find(std::string_view id) {
        for (auto& run : runs)
            if (run.id == id)
                return run;
        throw OperationError("unknown_run", "Unknown or expired runId");
    }
    const Run& find(std::string_view id) const {
        for (const auto& run : runs)
            if (run.id == id)
                return run;
        throw OperationError("unknown_run", "Unknown or expired runId");
    }
    static std::string snapshot(const Run& run) {
        return "{\"runId\":" + json::quote(run.id) + ",\"operation\":" + json::quote(run.operation) + ",\"state\":" + json::quote(run.state)
               + ",\"cancelRequested\":" + (run.cancel ? "true" : "false") + ",\"output\":" + json::quote(run.output) + ",\"result\":" + run.result + "}";
    }
    void closeLocked(std::string reason) {
        if (closed)
            return;
        closed  = true;
        failure = std::move(reason);
        if (active) {
            find(*active).state = "disconnected";
            audit("disconnected", find(*active).operation, *active);
            active.reset();
        }
    }
};
}  // namespace webfront::automation
