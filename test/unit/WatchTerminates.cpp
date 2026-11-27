// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-27
//
// WebFront — tests/unit
// Role: A process that destroys the handle of a watch from its own visitor: it must write a diagnostic, then terminate (ADR-027, section 5).
// Thread: worker-safe | Alloc: unrestricted
// LLM: process test (WatchDestroyedFromItsVisitor); CTest reads its output, the process must not reach its last line

#include <webfront/Watches.hpp>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <vector>

using namespace webfront;

int main() {
    // Terminating must not open a dialog nor write a core file: the handler says so and leaves with a status of its own.
    std::set_terminate([] {
        std::fputs("terminated\n", stderr);
        std::fflush(stderr);
        std::_Exit(3);
    });
    auto             registry = std::make_shared<inspect::WatchRegistry>();
    std::vector<int> values{1, 2, 3};
    inspect::Watch   handle;
    // A source that refers to the application's objects: its handle cannot be destroyed from its own operation.
    const auto source = [&](inspect::Visit<const std::vector<int>> visit) {
        handle.reset();
        visit(values);
    };
    using Shape = inspect::SourceShape<std::decay_t<decltype(source)>>;
    handle      = inspect::Watch{
        registry,
        registry->add(
            inspect::makeWatch<Shape>("values", "app.cpp:12", source, std::shared_ptr<inspect::NoState>{}, inspect::NoExecutor{}, inspect::NoMutator{}))};
    const auto session = registry->session(1);
    const auto limits  = inspect::Limits{.responseBudget = inspect::minPageBytes, .maxWriteTextBytes = 1024};
    const auto id      = static_cast<double>(handle.id());
    static_cast<void>(registry->children(session, id, "", registry->reset(session, id), 0, 10, limits));
    std::fputs("the process survived\n", stderr);
    return 0;
}
