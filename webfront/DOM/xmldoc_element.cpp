#include "xmldoc_element.h"
#include "xmldoc_attr.h"
#include "xmldoc_document.h"
#include "xmldoc_node_list.h"

namespace WebFront { namespace XMLDoc {


/**
 * The name of the element.
 */
const std::string& Element::getTagName() const
{
    return getNodeName();
}


/**
 * Retrieves an attribute value by name.
 * @param[in] name The name of the attribute to retrieve.
 * @return The attribute value.
*/
std::string Element::getAttribute(const std::string& name) const
{
    auto attribute = getAttributeNode(name);
    if (attribute)
    {
        return attribute->getNodeValue();
    }
    return std::string();
}


/**
 * Adds a new attribute. 
 * @param[in] name The name of the attribute to create or alter.
 * @param[in] value Value to set in string form.
 * @exception DOMException
 *   NO_MODIFICATION_ALLOWED_ERR: Raised if this node is readonly.
 */
void Element::setAttribute(const std::string& name, const std::string& value)
{
    if (isReadOnly()) throw DOMException(DOMException::NO_MODIFICATION_ALLOWED_ERR, "node is read only");   
    auto attribute = getAttributeNode(name);
    if (nullptr == attribute)
    {
        auto new_attribute = getOwnerDocument()->createAttribute(name);
        new_attribute->setValue(value);
        appendChild(new_attribute);
    }
    else
    {
        attribute->setNodeValue(value);
    }
}

/**
 * Removes an attribute by name.
 * @param[in] name The name of the attribute to remove.
 * @exception DOMException
 *   NO_MODIFICATION_ALLOWED_ERR: Raised if this node is readonly.
 */
void Element::removeAttribute(const std::string& name)
{
    if (isReadOnly()) throw DOMException(DOMException::NO_MODIFICATION_ALLOWED_ERR, "node is read only");   
    auto attribute = getAttributeNode(name);
    if (nullptr != attribute)
    {
        removeAttributeNode(attribute);
    }
}


/**
 * Retrieves an attribute node by name.
 * @param[in] name The name of the attribute to retrieve.
 * @return The <code>Attr</code> node with the specified name (
 *   <code>nodeName</code>) or <code>nullptr</code>.
 */
std::shared_ptr<Attr> Element::getAttributeNode(const std::string& name) const
{
    for (auto node : childNodes_)
    {
        if (ATTRIBUTE_NODE == node->getNodeType())
        {
            auto attr = std::static_pointer_cast<Attr>(node);
            if (0 == name.compare(attr->getNodeName()))
            {
                return attr;
            }
        }
    }
    return nullptr;
}


/**
 * Removes the given attribute node.
 * @param old_attr The <code>Attr</code> node to remove.
 * @return The <code>Attr</code> node that was removed.
 * @exception DOMException
 *   NO_MODIFICATION_ALLOWED_ERR: Raised if this node is readonly.
 *   <br>NOT_FOUND_ERR: Raised if <code>oldAttr</code> is not an attribute 
 *   of the element.
 */
std::shared_ptr<Attr> Element::removeAttributeNode(std::shared_ptr<Attr> old_attr)
{
    removeChild(old_attr);
    return old_attr;
}


/**
 * Returns a NodeList of all descendant Elements with a given tag name.
 * @param[in] tag_name The name of the tag to match on. The special value "*" 
 *   matches all tags.
 * @return A list of matching <code>Element</code> nodes.
 */
std::unique_ptr<NodeList> Element::getElementsByTagName(const std::string& tag_name) const
{
    std::list<std::shared_ptr<Node>> tagged_list;
    auto node = getFirstChild();
    while (nullptr != node)
    {
        if (ELEMENT_NODE == node->getNodeType())
        {
            if (0 == tag_name.compare(std::static_pointer_cast<Element>(node)->getTagName())
                || tag_name == "*")
            {
                tagged_list.push_back(node);
            }
        }
        if (node->hasChildNodes())
        {
            node = node->getFirstChild();
        }
        else
        {
            while (nullptr == node->getNextSibling() && node.get() != this)
            {
                node = node->getParentNode();
            }
            node = node->getNextSibling();
        }
    }
    return std::unique_ptr<NodeList>(new NodeList(tagged_list));
}


/**
* Returns <code>true</code> if named attribute exists.
* @param[in] name The name of the attribute to look for.
* @return <code>true</code> if an attribute with the given name is 
*   specified on this element or has a default value, <code>false</code>
*    otherwise.
*/
bool Element::hasAttribute(const std::string& name) const
{
    return (getAttributeNode(name) != nullptr);
}



}};