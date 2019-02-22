#include "xmldoc_node.h"
#include "xmldoc_node_list.h"

namespace WebFront { namespace XMLDoc {

Node::Node(short type) : nodeType_(type), flags_(0)
{
}


const std::string& Node::getNodeName() const
{
    return nodeName_;
}


const std::string& Node::getNodeValue() const 
{
    return nodeValue_;
}


void Node::setNodeValue(std::string nodeValue) 
{
    if (isReadOnly())
    {
        throw DOMException(DOMException::NO_MODIFICATION_ALLOWED_ERR, "node is read only");
    }
    nodeValue_ = nodeValue;
}


short Node::getNodeType() const 
{
    return nodeType_;
}


std::shared_ptr<Node> Node::getParentNode() const 
{
    return parentNode_;
}


std::shared_ptr<Node> Node::getFirstChild() const
{
    return firstChild_;
}


std::shared_ptr<Node> Node::getLastChild() const 
{
    return lastChild_;
}


std::shared_ptr<Node> Node::getPreviousSibling() const 
{
    return previousSibling_;
}


std::shared_ptr<Node> Node::getNextSibling() const 
{ 
    return nextSibling_;
}


std::unique_ptr<NamedNodeMap> Node::getAttributes() const
{
    // NOT IMPLEMENTED
    return nullptr;
}


std::shared_ptr<Document> Node::getOwnerDocument() const
{
    // NOT IMPLEMENTED
    return nullptr;
}


std::shared_ptr<Node> Node::insertBefore(   std::shared_ptr<Node> new_child,
                                            std::shared_ptr<Node> ref_child)
{
    // NOT IMPLEMENTED
    return nullptr;
}


std::shared_ptr<Node> Node::replaceChild(   std::shared_ptr<Node> new_child,
                                            std::shared_ptr<Node> old_child)
{
    // NOT IMPLEMENTED
    return nullptr;
}


/**
 * Removes the given child node.
 * @param[in] old_child The node to remove.
 * @return The node that was removed.
 * @exception DOMException
 *   NO_MODIFICATION_ALLOWED_ERR: Raised if this node is readonly.
 *   <br>NOT_FOUND_ERR: Raised if <code>oldAttr</code> is not an attribute 
 *   of the element.
 */
std::shared_ptr<Node> Node::removeChild(std::shared_ptr<Node> old_child)
{
    if (isReadOnly()) throw DOMException(DOMException::NO_MODIFICATION_ALLOWED_ERR, "node is read only");   
    for (auto node : childNodes_)
    {
        if (node == old_child)
        {
            removeChild(old_child);
            return node;
        }
    }
    throw DOMException(DOMException::NOT_FOUND_ERR, "not an attribute of the element");
}


std::shared_ptr<Node> Node::appendChild(std::shared_ptr<Node> new_child)
{
    // NOT IMPLEMENTED
    return nullptr;
}


bool Node::hasChildNodes() const 
{
    return nodes_count_ > 0;
}


std::unique_ptr<Node> Node::cloneNode(bool deep) const
{
    // NOT IMPLEMENTED
    return nullptr;
}


void Node::normalize()
{
    // NOT IMPLEMENTED
}


bool Node::isSupported(const std::string& feature, const std::string& version) const
{
    // NOT IMPLEMENTED
    return false;
}


const std::string& Node::getNamespaceURI() const
{
    // NOT IMPLEMENTED
    return namespace_uri_;
}


const std::string& Node::getPrefix() const 
{
    return prefix_;
}


void Node::setPrefix(const std::string& prefix) 
{
    prefix_ = prefix;
}


const std::string& Node::getLocalName() const
{
    // NOT IMPLEMENTED
    return nodeName_;
}


bool Node::hasAttributes() const
{
    // NOT IMPLEMENTED
    return false;
}


std::unique_ptr<NodeList> Node::getChildNodes() const
{
    return std::unique_ptr<NodeList>(new NodeList(childNodes_));
}
    

}}