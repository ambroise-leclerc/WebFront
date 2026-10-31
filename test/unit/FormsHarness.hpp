// A DOM document without a browser for the forms tests (#383): it records the operations of each flushed batch and
// delivers events to C++ listeners as the interpreter would.
#pragma once
#include <webfront/dom/Document.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace forms_fixture {

struct Browser {
    std::shared_ptr<webfront::dom::detail::State> state;
    auto                                          domState() const {
        return state;
    }
};

class Recorder {
public:
    std::shared_ptr<webfront::dom::detail::State> state = std::make_shared<webfront::dom::detail::State>();
    webfront::dom::Document<Browser>              doc{Browser{state}};

    Recorder() {
        state->ready = true;
        state->send  = [this](const webfront::msg::DomBatch& batch) {
            for (auto& op : webfront::msg::DomBatch::decode(batch.data()).second) {
                if (op.code == webfront::msg::DomOp::name)
                    names[op.a] = op.value;
                sent.push_back(std::move(op));
            }
        };
    }
    Recorder(const Recorder&)            = delete;
    Recorder& operator=(const Recorder&) = delete;

    /// Flushes the document and returns the operations sent since the previous call.
    std::vector<webfront::msg::DomOperation> take() {
        doc.flush();
        return std::exchange(sent, {});
    }
    std::string name(std::uint32_t index) const {
        return names.at(index);
    }
    /// The browser reports an event on a node, with its value.
    void event(const webfront::dom::Node& node, std::string_view event, std::string value) {
        doc.flush();
        state->event(node.id(), state->names.find(event)->second, std::move(value));
    }

private:
    std::vector<webfront::msg::DomOperation> sent;
    std::map<std::uint32_t, std::string>     names;
};

}  // namespace forms_fixture
