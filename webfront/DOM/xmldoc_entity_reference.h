#ifndef __WEBFRONT_XMLDOC_ENTITY_REFERENCE_H_
#define __WEBFRONT_XMLDOC_ENTITY_REFERENCE_H_

#include "xmldoc_text.h"

namespace WebFront { namespace XMLDoc {

class EntityReference : public Node
{
protected:
    EntityReference() : Node(ENTITY_REFERENCE_NODE) {}
    friend class Document;
};


}}
#endif // __WEBFRONT_XMLDOC_ENTITY_REFERENCE_H_