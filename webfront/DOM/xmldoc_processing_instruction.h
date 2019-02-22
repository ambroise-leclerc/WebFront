#ifndef __WEBFRONT_XMLDOC_PROCESSING_INSTRUCTION_H_
#define __WEBFRONT_XMLDOC_PROCESSING_INSTRUCTION_H_

#include "xmldoc_node.h"

namespace WebFront { namespace XMLDoc {

class ProcessingInstruction : public Node
{
public:
    const std::string& getTarget() const;
    const std::string& getData() const;
    void setData(const std::string& data)       throw(DOMException);

protected:
    ProcessingInstruction() : Node(PROCESSING_INSTRUCTION_NODE) {}
    friend class Document;
};


}}
#endif // __WEBFRONT_XMLDOC_PROCESSING_INSTRUCTION_H_