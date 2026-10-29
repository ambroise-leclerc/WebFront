#pragma once
#include "../Life.hpp"
#include <webfront/components/react/Store.hpp>
#include <webfront/WebFront.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>

namespace webfront::demo {
// Kept by the registered callbacks, one simulation and one coalesced store per browser.
template <typename App>
[[nodiscard]] UISubscription installLifeReact(App& app) {
    using BrowserUI = typename App::UI;
    using Snapshot  = std::tuple<LifeGrid, std::uint32_t, std::uint32_t>;
    struct Session {
        react::Store<BrowserUI, Snapshot> store;
        LifeGrid                          grid;
        std::uint32_t                     generation{};
        dom::Node                         label;
        void                              publish() {
            label.setText("Generation " + std::to_string(generation) + " · " + std::to_string(grid.count()) + " living cells");
            store.set({grid, generation, static_cast<std::uint32_t>(grid.count())});
        }
    };
    auto sessions = std::make_shared<std::map<WebLinkId, std::shared_ptr<Session>>>();
    // Closure erasure frees the store and callbacks; old retained handles remain invalid.
    app.cppFunction("life.react.start", [sessions](BrowserUI ui) {
        if (sessions->contains(ui.id()))
            throw std::runtime_error("Life React already started");
        auto doc     = ui.document();
        auto panel   = doc.island("life-panel");
        auto heading = doc.element("h2");
        heading.append(doc.text("C++ simulation panel"));
        panel.append(heading);
        auto label  = doc.text("");
        auto status = doc.element("p");
        status.append(label);
        panel.append(status);
        auto session     = std::make_shared<Session>(Session{
            react::Store<BrowserUI, Snapshot>{ui, "life"},
            {},
            0,
            label
        });
        session->grid[1] = session->grid[34] = session->grid[64] = session->grid[65] = session->grid[66] = true;
        auto button                                                                                      = doc.element("button");
        button.attribute("data-testid", "cpp-step");
        button.append(doc.text("Next generation"));
        // Weak reference avoids a cycle through the document's owned event handlers.
        button.listen("click", [weak = std::weak_ptr(session)](std::string) {
            if (auto s = weak.lock()) {
                s->grid = nextLife(s->grid);
                ++s->generation;
                s->publish();
            }
        });
        panel.append(button);
        sessions->emplace(ui.id(), session);
        session->publish();
    });
    app.cppFunction("life.react.toggle", [sessions](BrowserUI ui, std::uint32_t index) {
        if (index >= 1024)
            throw std::out_of_range("Life cell index");
        const auto found = sessions->find(ui.id());
        if (found == sessions->end())
            throw std::runtime_error("Life React has not started");
        auto session = found->second;
        session->grid.flip(index);
        // Editing a cell starts a new simulation from generation zero.
        session->generation = 0;
        session->publish();
    });
    return app.subscribeUIClosed([sessions](BrowserUI ui) {
        sessions->erase(ui.id());
    });
}
}  // namespace webfront::demo
