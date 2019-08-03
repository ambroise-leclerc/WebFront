#ifndef __WEBFRONT_XMLDOC_NODE_H_
#define __WEBFRONT_XMLDOC_NODE_H_

#include <memory>
#include <vector>
#include <unordered_map>
#include "xmldoc_dom_exception.h"
#include "xmldoc_named_node_map.h"

namespace WebFront { namespace XMLDoc {

class Document;
class NodeList;
class Node
{
public:
    static const short  ELEMENT_NODE = 1, ATTRIBUTE_NODE = 2, TEXT_NODE = 3, CDATA_SECTION_NODE = 4,
                        ENTITY_REFERENCE_NODE = 5, ENTITY_NODE = 6, PROCESSING_INSTRUCTION_NODE = 7,
                        COMMENT_NODE = 8, DOCUMENT_NODE = 9, DOCUMENT_TYPE_NODE = 10,
                        DOCUMENT_FRAGMENT_NODE = 11, NOTATION_NODE = 12;
public:
    const std::string& getNodeName() const;
    const std::string& getNodeValue() const;
    void setNodeValue(std::string nodeValue)                                    throw(DOMException);
    short getNodeType() const;
    
    std::shared_ptr<Node> getParentNode() const;
    std::unique_ptr<NodeList> getChildNodes() const;
    std::shared_ptr<Node> getFirstChild() const;
    std::shared_ptr<Node> getLastChild() const;
    std::shared_ptr<Node> getPreviousSibling() const;
    std::shared_ptr<Node> getNextSibling() const;
    std::unique_ptr<NamedNodeMap> getAttributes() const;
    std::shared_ptr<Document> getOwnerDocument() const;
    std::shared_ptr<Node> insertBefore( std::shared_ptr<Node> new_child,
                                        std::shared_ptr<Node> ref_child)        throw(DOMException);
    std::shared_ptr<Node> replaceChild( std::shared_ptr<Node> new_child,
                                        std::shared_ptr<Node> old_child)        throw(DOMException);
    std::shared_ptr<Node> removeChild(std::shared_ptr<Node> old_child)          throw(DOMException);
    std::shared_ptr<Node> appendChild(std::shared_ptr<Node> new_child)          throw(DOMException);
    bool hasChildNodes() const;
    std::unique_ptr<Node> cloneNode(bool deep) const;
    void normalize();
    bool isSupported(const std::string& feature, const std::string& version) const;
    const std::string& getNamespaceURI() const;
    const std::string& getPrefix() const;
    void setPrefix(const std::string& prefix);
    const std::string& getLocalName() const;
    bool hasAttributes() const;


    // std::unique_ptr helpers
    template <typename NT> std::shared_ptr<Node> appendChild(std::unique_ptr<NT>& newChild)     throw(DOMException)
    { return appendChild(std::shared_ptr<Node>(newChild.get())); }
    template <typename NT> std::shared_ptr<Node> insertBefore(std::unique_ptr<NT>& newChild,
                                        std::shared_ptr<Node> refChild)         throw(DOMException)
    { return insertBefore(std::shared_ptr<Node>(newChild.get()), refChild); }
    template <typename NT> std::shared_ptr<Node> replaceChild(std::unique_ptr<NT>& newChild,
                                        std::shared_ptr<Node> oldChild)         throw(DOMException)
    { return replaceChild(std::shared_ptr<Node>(newChild.get()), oldChild); }
   


protected:
    std::vector<std::shared_ptr<Node>> childNodes_;
    std::string nodeValue_;
    
private:
    std::unordered_map<std::string, unsigned int> attributes_indexes_;
    std::string nodeName_;
    std::string namespace_uri_;

    std::shared_ptr<Node> parentNode_;
    std::shared_ptr<Node> nextSibling_, previousSibling_;
    std::shared_ptr<Node> firstChild_, lastChild_;
    std::shared_ptr<Document> ownerDocument_;

    unsigned int nodes_count_;
    short nodeType_;
    std::string prefix_;

private:
    enum NodeFlags { kReadOnly = 0x1<<1 };
    unsigned short flags_;

protected:
    friend class DOMImplementation;
    Node(short type);

    bool isReadOnly() const { return (flags_ & kReadOnly) !=0; };
    void setReadOnly(bool read_only) { flags_ = (read_only ? flags_ | kReadOnly : flags_ & ~kReadOnly); }

};

}}
#endif // __WEBFRONT_XMLDOC_NODE_H_