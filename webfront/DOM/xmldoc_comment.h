#ifndef __WEBFRONT_XMLDOC_COMMENT_H_
#define __WEBFRONT_XMLDOC_COMMENT_H_

#include "xmldoc_character_data.h"

namespace WebFront { namespace XMLDoc
{

class Comment : public CharacterData
{
protected:
    Comment() : CharacterData(COMMENT_NODE) {}
    friend class Document;

};

}}
#endif // __WEBFRONT_XMLDOC_COMMENT_H_
