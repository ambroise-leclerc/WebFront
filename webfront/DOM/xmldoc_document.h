#ifndef __WEBFRONT_XMLDOC_DOCUMENT_H_
#define __WEBFRONT_XMLDOC_DOCUMENT_H_

#include "xmldoc_node.h"

namespace WebFront { namespace XMLDoc {

class DocumentType;
class DOMImplementation;
class Element;
class DocumentFragment;
class Text;
class Comment;
class CDATASection;
class ProcessingInstruction;
class Attr;
class EntityReference;
class NodeList;

class Document : public Node
{
public:
    std::shared_ptr<DocumentType> getDoctype();
    std::shared_ptr<DOMImplementation> getImplementation() const;
    std::shared_ptr<Element> getDocumentElement() const;
    std::unique_ptr<Element> createElement(const std::string& tagName) const                throw(DOMException);
    std::unique_ptr<DocumentFragment> createDocumentFragment() const;
    std::unique_ptr<Text> createTextNode(const std::string& data) const;
    std::unique_ptr<Comment> createComment(const std::string& data) const;
    std::unique_ptr<CDATASection> createCDATASection(const std::string& data) const         throw(DOMException);
    std::unique_ptr<ProcessingInstruction> createProcessingInstruction(const std::string& target,
                                                    const std::string& data) const          throw(DOMException);
    std::unique_ptr<Attr> createAttribute(const std::string& name) const                    throw(DOMException);
    std::unique_ptr<EntityReference> createEntityReference(const std::string& name) const   throw(DOMException);
    std::shared_ptr<NodeList> getElementsByTagName(const std::string& tagname) const;
    std::shared_ptr<Node> importNode(std::shared_ptr<Node> importedNode, bool deep) const   throw(DOMException);

    std::unique_ptr<Element> createElementNS(const std::string& namespaceURI,
                                             const std::string& qualifiedName) throw(DOMException);
    std::unique_ptr<Attr> createAttributeNS(const std::string& namespaceURI,
                                            const std::string& qualifiedName) throw(DOMException);
    std::shared_ptr<NodeList> getElementsByTagNameNS(const std::string& namespaceURI,
                                                     const std::string& localName) const;
    std::shared_ptr<Element> getElementById(const std::string& elementId);

protected:
    Document() : Node(DOCUMENT_NODE) {}
    friend class DOMImplementation;

};


}}
#endif // __WEBFRONT_XMLDOC_DOCUMENT_H_
