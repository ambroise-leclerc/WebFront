#include "xmldoc_attr.h"

namespace WebFront { namespace XMLDoc {

Attr::Attr() : Node(ATTRIBUTE_NODE)
{
}

/**
 * Returns the name of this attribute.
 * @return The name of this attribute.
 */
const std::string& Attr::getName() const
{
    return getNodeName();
}


/**
 * Tells if the attribute value has been specified in the original
 * document.
 * @return <code>true</code> if the attribute has an assigned value
 * in the Document (and the value is the assigned value).
 * <code>false</code> if the attribute value came from the default
 * value declared in the Document's DTD.
 */
bool Attr::getSpecified()
{
    return specified_;
}


const std::string& Attr::getValue() const
{
    return getNodeValue();
}


void Attr::setValue(const std::string& value)
{
    setNodeValue(value);
}


std::shared_ptr<Element> Attr::getOwnerElement() const
{
    // NOT IMPLEMENTED
    return nullptr;
}


}}