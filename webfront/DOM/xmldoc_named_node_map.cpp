#include "xmldoc_named_node_map.h"
#include "xmldoc_node.h"
#include "xmldoc_attr.h"

namespace WebFront { namespace XMLDoc
{
    NamedNodeMap::NamedNodeMap() : read_only_(false)
    {
    }


    /**
     * Retrieves a node specified by name.
     * @param[in] name The nodeName of the node to retrieve.
     * @return A Node with the specified nodeName of nullptr/
     */
    std::shared_ptr<Node> NamedNodeMap::getNamedItem(const std::string& name) const
    {
        try
        {
            return nodes_.at(name);
        }
        catch (std::out_of_range&)
        {
            return nullptr;
        }
    }


    /**
     * Adds a node using its <code>nodeName</code> attribute. If a node with 
     * that name is already present in this map, it is replaced by the new 
     * one.
     * @param[in] node The node to store in the map.
     * @return If the new Node replaces an existing node, the replaced node
     * is returned, otherwise nullptr is returned.
     * @exception DOMException
     *   WRONG_DOCUMENT_ERR: Raised if <code>node</code> was created from a 
     *   different document than the one that created this map.
     *   <br>NO_MODIFICATION_ALLOWED_ERR: Raised if this map is readonly.
     *   <br>INUSE_ATTRIBUTE_ERR: Raised if <code>node</code> is an 
     *   <code>Attr</code> that is already an attribute of another 
     *   <code>Element</code> object. The DOM user must explicitly clone 
     *   <code>Attr</code> nodes to re-use them in other elements.
     */
    std::shared_ptr<Node> NamedNodeMap::setNamedItem(std::shared_ptr<Node> node)
    {
        if (node->getOwnerDocument() != owner_document_)
        {
            throw DOMException(DOMException::WRONG_DOCUMENT_ERR,
                "node was created from a different document than the one that created this NamedNodeMap.");
        }

        if (read_only_)
        {
            throw DOMException(DOMException::NO_MODIFICATION_ALLOWED_ERR, "NamedNodeMap is readonly.");
        }

        if (Node::ATTRIBUTE_NODE == node->getNodeType())
        {
            if (nullptr != std::static_pointer_cast<Attr>(node)->getOwnerElement())
            {
                throw DOMException(DOMException::INUSE_ATTRIBUTE_ERR, "node is already an attribute of another Element.");
            }
        }

        auto old_node_iter = nodes_.find(node->getNodeName());
        std::shared_ptr<Node> old_node;
        if (nodes_.end() == old_node_iter)
        {
            old_node = nullptr;
        }
        else
        {
            old_node = old_node_iter->second;
        }

        nodes_[node->getNodeName()] = node;
        return old_node;
    }


    /**
     * Removes a node specified by name.
     * @param[in] name The <code>nodeName</code> of the node to remove.
     * @return The node removed from this map if a node with such a name 
     *   exists.
     * @exception DOMException
     *   NOT_FOUND_ERR: Raised if there is no node named <code>name</code> in 
     *   this map.
     *   <br>NO_MODIFICATION_ALLOWED_ERR: Raised if this map is readonly.
     */
    std::shared_ptr<Node> NamedNodeMap::removeNamedItem(const std::string& name)
    {
        if (read_only_)
        {
            throw DOMException(DOMException::NO_MODIFICATION_ALLOWED_ERR, "NamedNodeMap is readonly.");
        }
        auto old_node_iter = nodes_.find(name);
        if (nodes_.end() == old_node_iter)
        {
            throw DOMException(DOMException::NOT_FOUND_ERR, "No node with this name found.");
        }
        return old_node_iter->second;
        
    }


    /**
     * Returns the <code>index</code>th item in the map. If <code>index</code> 
     * is greater than or equal to the number of nodes in this map, this 
     * returns <code>nullptr</code>.
     * @param[in] index Index into this map.
     * @return The node at the <code>index</code>th position in the map, or 
     *   <code>nullptr</code> if that is not a valid index.
     */
    std::shared_ptr<Node> NamedNodeMap::item(unsigned int index) const
    {
        for (auto node : nodes_)
        {
            if (0 == index)
            {
                return node.second;
            }
            else
            {
                index--;
            }
        }
        return nullptr;
    }

    /**
     * The number of nodes in this map.
     * @return The number of nodes in this map.
     */
    unsigned int NamedNodeMap::getLength() const
    {
        return nodes_.size();
    }
}}