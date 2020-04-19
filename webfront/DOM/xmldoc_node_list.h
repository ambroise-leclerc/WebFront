#ifndef __WEBFRONT_XMLDOC_NODE_LIST_H_
#define __WEBFRONT_XMLDOC_NODE_LIST_H_

#include "xmldoc.h"
#include <algorithm>
namespace WebFront { namespace XMLDoc
{
class Node;
class NodeList
{
public:
    template <typename NodesContainer> NodeList(const NodesContainer& container)
    { nodes_.resize(container.size()); std::copy(container.cbegin(), container.cend(), nodes_.begin());    }

    size_t length() const { return nodes_.size(); }
    std::shared_ptr<Node> item(size_t index) const { if (index<nodes_.size()) return nodes_[index]; else return nullptr; }
    std::shared_ptr<Node> operator[](size_t index) const { return item(index); }
private:
    std::vector<std::shared_ptr<Node>> nodes_;
};

}}
#endif // __WEBFRONT_XMLDOC_NODE_LIST_H_