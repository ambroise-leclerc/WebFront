#ifndef __WEBFRONT_XMLDOC_DOM_EXCEPTION_H_
#define __WEBFRONT_XMLDOC_DOM_EXCEPTION_H_

#include <exception>
#include <string>

namespace WebFront { namespace XMLDoc {

class DOMException : public std::runtime_error
{
public:
    DOMException(short code, const std::string& message) : runtime_error(message) { code_ = code; }
    short code_;
    
    // ExceptionCode
    static const short INDEX_SIZE_ERR = 1, DOMSTRING_SIZE_ERR = 2, HIERARCHY_REQUEST_ERR = 3,
                       WRONG_DOCUMENT_ERR = 4, INVALID_CHARACTER_ERR = 5, NO_DATA_ALLOWED_ERR = 6,
                       NO_MODIFICATION_ALLOWED_ERR = 7, NOT_FOUND_ERR = 8, NOT_SUPPORTED_ERR = 9, 
                       INUSE_ATTRIBUTE_ERR = 10, INVALID_STATE_ERR = 11, SYNTAX_ERR = 12, 
                       INVALID_MODIFICATION_ERR = 13, NAMESPACE_ERR = 14, INVALID_ACCESS_ERR = 15;
};

 #pragma warning(disable: 4290) // Visual Studio does not implement checked exceptions.
}}
#endif // __WEBFRONT_XMLDOC_DOM_EXCEPTION_H_