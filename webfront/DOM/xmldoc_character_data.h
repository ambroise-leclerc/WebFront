#ifndef __WEBFRONT_XMLDOC_CHARACTER_DATA_H_
#define __WEBFRONT_XMLDOC_CHARACTER_DATA_H_

#include "xmldoc_node.h"

namespace WebFront { namespace XMLDoc {

class CharacterData : public Node
{
public:

    const std::string& getData() const;
    void setData(const std::string& data)                                               throw(DOMException);
    unsigned int getLength() const;
    std::string substringData(unsigned int offset, unsigned int count) const            throw(DOMException);
    void appendData(const std::string& arg);
    void insertData(unsigned int offset, const std::string& arg)                        throw(DOMException);
    void deleteData(unsigned int offset, unsigned int count)                            throw(DOMException);
    void replaceData(unsigned int offset, unsigned int count, const std::string& arg)   throw(DOMException);

protected:
    CharacterData(unsigned short nodeType) : Node(nodeType) {}

};


}}
#endif // __WEBFRONT_XMLDOC_CHARACTER_DATA_H_