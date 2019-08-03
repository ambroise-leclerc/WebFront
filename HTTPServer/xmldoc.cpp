#include "xmldoc.h"

namespace WebFront { namespace XMLDoc
{







/*void Traverse()
{
    auto node = getFirstChild();
    while (nullptr != node)
    {
        // std::cout << node.getNodeName() << " = " << std::getNodeValue());

        if (node->hasChildNodes())
        {
            node = node->getFirstChild();
        }
        else
        {   // leaf
            // Get to the parent level
            while (nullptr == node->getNextSibling() && node.get() != this)
                node = node->getParentNode();
            node = node->getNextSibling();
        }
    }
}
*/
}}