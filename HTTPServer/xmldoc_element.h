#ifndef __WEBFRONT_XMLDOC_ELEMENT_H_
#define __WEBFRONT_XMLDOC_ELEMENT_H_

#include "xmldoc_node.h"

namespace WebFront { namespace XMLDoc {

class Attr;
class Element : public Node
{
public:
    const std::string& getTagName() const;

    std::string getAttribute(const std::string& name) const;
    void setAttribute(const std::string& name, const std::string& value)        throw(DOMException);
    void removeAttribute(const std::string& name)                               throw(DOMException);
    std::shared_ptr<Attr> getAttributeNode(const std::string& name) const;
    std::shared_ptr<Attr> setAttributeNode(std::shared_ptr<Attr> new_attr);
    std::shared_ptr<Attr> removeAttributeNode(std::shared_ptr<Attr> old_attr)   throw(DOMException);
    std::unique_ptr<NodeList> getElementsByTagName(const std::string& tag_name) const;
    bool hasAttribute(const std::string& name) const;

protected:
    Element();
    friend class Document;
};


}}
#endif // __WEBFRONT_XMLDOC_ELEMENT_H_