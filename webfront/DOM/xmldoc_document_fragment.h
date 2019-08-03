#ifndef __WEBFRONT_XMLDOC_DOCUMENT_FRAGMENT_H_
#define __WEBFRONT_XMLDOC_DOCUMENT_FRAGMENT_H_

#include "xmldoc.h"

namespace WebFront { namespace XMLDoc
{

class DocumentFragment : public Node
{
protected:
    DocumentFragment() : Node(DOCUMENT_FRAGMENT_NODE) {}
    friend class Document;
};

}}
#endif // __WEBFRONT_XMLDOC_DOCUMENT_FRAGMENT_H_
