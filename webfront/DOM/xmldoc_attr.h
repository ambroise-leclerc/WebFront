#ifndef __WEBFRONT_XMLDOC_ATTR_H_
#define __WEBFRONT_XMLDOC_ATTR_H_

#include "xmldoc_node.h"
#include "xmldoc_element.h"

namespace WebFront { namespace XMLDoc {
class Attr : public Node
{
public:
    const std::string& getName() const;
    bool getSpecified();
    const std::string& getValue() const;
    void setValue(const std::string& value)                             throw(DOMException);
    std::shared_ptr<Element> getOwnerElement() const;

private:
    std::shared_ptr<Element> ownerElement_; // The Element node this attribute is attached to.
    bool specified_;                        // True if the attribute has an assigned value in the Document.

protected:
    Attr();
    friend class Document;
};

}}
#endif // __WEBFRONT_XMLDOC_ATTR_H_
