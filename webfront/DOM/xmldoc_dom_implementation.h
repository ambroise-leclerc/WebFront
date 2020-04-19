#ifndef __WEBFRONT_XMLDOC_DOM_IMPLEMENTATION_H_
#define __WEBFRONT_XMLDOC_DOM_IMPLEMENTATION_H_

#include "xmldoc_document.h"
#include "xmldoc_document_type.h"

namespace WebFront { namespace XMLDoc {

class DOMImplementation
{
public:
    DOMImplementation();
    ~DOMImplementation();
    bool hasFeature(const std::string& feature, const std::string& version) const;
    std::unique_ptr<DocumentType> createDocumentType(
        const std::string& qualifiedName,
        const std::string& publicId, 
        const std::string& systemId)                    const throw(DOMException);
    std::shared_ptr<Document> createDocument(
        const std::string& namespaceURI,
        const std::string& qualifiedName, 
        const std::shared_ptr<DocumentType> doctype)    const throw(DOMException);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}}
#endif // __WEBFRONT_XMLDOC_DOM_IMPLEMENTATION_H_