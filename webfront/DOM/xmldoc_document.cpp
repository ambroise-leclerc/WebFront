#include "xmldoc_document.h"
#include "xmldoc_attr.h"
#include "xmldoc_c_data_section.h"
#include "xmldoc_comment.h"
#include "xmldoc_document_fragment.h"
#include "xmldoc_document_type.h"
#include "xmldoc_dom_implementation.h"
#include "xmldoc_element.h"
#include "xmldoc_entity_reference.h"
#include "xmldoc_node_list.h"
#include "xmldoc_processing_instruction.h"
#include "xmldoc_text.h"

namespace WebFront { namespace XMLDoc {

std::shared_ptr<DocumentType> Document::getDoctype()
{
    // NOT IMPLEMENTED
    return nullptr;
}

std::shared_ptr<DOMImplementation> Document::getImplementation() const
{
    // NOT IMPLEMENTED
    return nullptr;
}

std::shared_ptr<Element> Document::getDocumentElement() const
{
    // NOT IMPLEMENTED
    return nullptr;
}

std::unique_ptr<Element> Document::createElement(const std::string& tagName) const
{
    // NOT IMPLEMENTED
    return nullptr;
}

std::unique_ptr<DocumentFragment> Document::createDocumentFragment() const
{
    // NOT IMPLEMENTED
    return nullptr;
}

std::unique_ptr<Text> Document::createTextNode(const std::string& data) const
{
    // NOT IMPLEMENTED
    return nullptr;
}

std::unique_ptr<Comment> Document::createComment(const std::string& data) const
{
    // NOT IMPLEMENTED
    return nullptr;
}

std::unique_ptr<CDATASection> Document::createCDATASection(const std::string& data) const
{
    // NOT IMPLEMENTED
    return nullptr;
}

std::unique_ptr<ProcessingInstruction> Document::createProcessingInstruction(
    const std::string& target, const std::string& data) const
{
    // NOT IMPLEMENTED
    return nullptr;
}

std::unique_ptr<Attr> Document::createAttribute(const std::string& name) const
{
    // NOT IMPLEMENTED
    return nullptr;
}

std::unique_ptr<EntityReference> Document::createEntityReference(const std::string& name) const
{
    // NOT IMPLEMENTED
    return nullptr;
}

std::shared_ptr<NodeList> Document::getElementsByTagName(const std::string& tagname) const
{
    // NOT IMPLEMENTED
    return nullptr;
}

std::shared_ptr<Node> Document::importNode(std::shared_ptr<Node> importedNode, bool deep) const
{
    // NOT IMPLEMENTED
    return nullptr;
}

std::unique_ptr<Element> Document::createElementNS(
    const std::string& namespaceURI, const std::string& qualifiedName)
{
    // NOT IMPLEMENTED
    return nullptr;
}

std::unique_ptr<Attr> Document::createAttributeNS(
    const std::string& namespaceURI, const std::string& qualifiedName)
{
    // NOT IMPLEMENTED
    return nullptr;
}

std::shared_ptr<NodeList> Document::getElementsByTagNameNS(
    const std::string& namespaceURI, const std::string& localName) const
{
    // NOT IMPLEMENTED
    return nullptr;
}

std::shared_ptr<Element> Document::getElementById(const std::string& elementId)
{
    // NOT IMPLEMENTED
    return nullptr;
}

}}