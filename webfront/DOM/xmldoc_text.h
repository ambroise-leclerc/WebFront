#ifndef __WEBFRONT_XMLDOC_TEXT_H_
#define __WEBFRONT_XMLDOC_TEXT_H_

#include "xmldoc_character_data.h"

namespace WebFront { namespace XMLDoc {


class Text : public CharacterData
{
public:
    std::shared_ptr<Text> splitText(unsigned int offset)        throw(DOMException);

protected:
    Text() : CharacterData(TEXT_NODE) {}
    friend class Document;
};


}}
#endif // __WEBFRONT_XMLDOC_TEXT_H_