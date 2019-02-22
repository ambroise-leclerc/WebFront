#ifndef __WEBFRONT_XMLDOC_NAMED_NODE_MAP_H_
#define __WEBFRONT_XMLDOC_NAMED_NODE_MAP_H_

#include "xmldoc_dom_exception.h"
#include <unordered_map>
#include <memory>

namespace WebFront { namespace XMLDoc
{
class Node;
class NamedNodeMap
{
public:

    std::shared_ptr<Node> getNamedItem(const std::string& name) const;
    std::shared_ptr<Node> setNamedItem(std::shared_ptr<Node> node)              throw(DOMException);
    std::shared_ptr<Node> removeNamedItem(const std::string& name)              throw(DOMException);
    std::shared_ptr<Node> item(unsigned int index) const;
    unsigned int getLength() const;

private:
    NamedNodeMap();
    friend class Document;

    std::shared_ptr<Document> owner_document_;
    std::unordered_map<std::string, std::shared_ptr<Node>> nodes_;
    bool read_only_;
};

}}
#endif // __WEBFRONT_XMLDOC_NAMED_NODE_MAP_H_