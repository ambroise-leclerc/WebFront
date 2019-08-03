#ifndef __WEBFRONT_XMLDOC_DOCUMENT_TYPE_H_
#define __WEBFRONT_XMLDOC_DOCUMENT_TYPE_H_

#include "xmldoc_node.h"

namespace WebFront { namespace XMLDoc {

class DocumentType : public Node
{
public:
    std::string getName() const;
    std::unique_ptr<NamedNodeMap> getEntities() const;
    std::unique_ptr<NamedNodeMap> getNotations() const;
    const std::string& getPublicId() const;
    const std::string& getSystemId() const;
    const std::string& getInternalSubset() const;


private:
    friend class DOMImplementation;
    DocumentType() : Node(DOCUMENT_TYPE_NODE) {}

private:
    std::string name_, public_id_, system_id_, internal_subset_;
};


}}
#endif // __WEBFRONT_XMLDOC_DOCUMENT_TYPE_H_