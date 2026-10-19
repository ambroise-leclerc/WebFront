#include "xmldoc_text.h"

namespace WebFront { namespace XMLDoc {


/**
 * Splits this node into two nodes at the specified <code>offset</code>.
 * @param[in] offset The offset at which to split.
 * @return The new node.
 * @exception DOMException
 *   INDEX_SIZE_ERR: Raised if the specified offset is out of range.
 *   <br>NO_MODIFICATION_ALLOWED_ERR: Raised if this node is readonly.
 */
std::shared_ptr<Text> Text::splitText(unsigned int offset)
{
    /*NOT IMPLEMENTED*/
/*
* keeping both in the tree as siblings. After being split, this node 
* will contain all the content up to the <code>offset</code> point. A 
* new node of the same type, which contains all the content at and 
* after the <code>offset</code> point, is returned. If the original 
* node had a parent node, the new node is inserted as the next sibling 
* of the original node. When the <code>offset</code> is equal to the 
* length of this node, the new node has no data.
*/
    return nullptr;
}

}}