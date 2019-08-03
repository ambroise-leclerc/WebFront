#include "http_request.h"
#include <array>
#include <cstdint>
#include <iostream>
#include <string>

#define HT                  9
#define SP                  ' '
#define CR                  '\r'
#define LF                  '\n'
#define AFFECT              ':'

class HTTPRequest::Impl
{
private:
    enum HeaderField;

public:
    Impl()
    {
        Reinitialize();
    }

    void Reinitialize()
    {
        set_method(kUndefined);
        set_parse_status(kUnparsed);
        set_connection_keepalive(true);
        buffer_parser_ = buffer_.data();
        buffer_end_ = buffer_parser_;
    }

    bool ParseData(size_t bytes)
    {
        static char* affect_sign=nullptr;

        if (kUnparsed == parse_status())
        {
            // Find end of first line (CRLF)
            while (LF != *buffer_end_)
            {
                if (0 == bytes)         // if there is no more data to parse before CRLF, that means
                    return true;        // that we get to wait for more data (in the next call to ParseData)
                --bytes;
                buffer_end_++;
            }


            if (CR == buffer_end_[-1])
            {
                // CRLF found : we have a first header line. Let's fine the method
                switch (buffer_parser_[0])
                {
                case 'G':
                    if (('E' == buffer_parser_[1]) && ('T' == buffer_parser_[2]))
                    {
                        set_method(kGet);
                        set_parse_status(kMethodParsed);
                        buffer_parser_+=3;;
                    };
                    break;
                case 'H':
                    if (('E' == buffer_parser_[1]) && ('A' == buffer_parser_[2]) && ('D' == buffer_parser_[3]))
                    {
                        set_method(kHead);
                        set_parse_status(kMethodParsed);
                        buffer_parser_+=4;;
                    }
                    break;
                }

                // if no method had been recognized, we return and ask for connection termination
                if (kMethodParsed != parse_status())                    
                    return false;

                // Extract URL
                while (SP == *buffer_parser_ || HT == *buffer_parser_)
                    ++buffer_parser_;
                while (SP != *buffer_parser_ && HT != *buffer_parser_)
                {
                    url_.append(buffer_parser_, 1);
                    ++buffer_parser_;
                }

                // Extract HTTP version
                while (SP == *buffer_parser_ || HT == *buffer_parser_)
                    ++buffer_parser_;
                while (SP != *buffer_parser_ && HT != *buffer_parser_ && buffer_parser_<(buffer_end_-1))
                {
                    http_version_.append(buffer_parser_, 1);
                    ++buffer_parser_;
                }

                buffer_parser_ = buffer_end_;
                set_parse_status(kMethodLineParsed);                
            }
        }

        while (kMethodLineParsed == parse_status())
        {
            while (LF != *buffer_end_)
            {
                if (0 == bytes) return true;
                if (nullptr == affect_sign)
                {
                    if (AFFECT == *buffer_end_) affect_sign = buffer_end_;
                }
                --bytes;
                buffer_end_++;
            }
            if (CR == buffer_end_[-1])
            {
                // if the completed line begins with a space character then it is
                // the continuation of the precedent affection.
                if ((SP == *buffer_parser_) || (HT == *buffer_parser_))
                {
                    ++buffer_parser_;
                    while (SP == *buffer_parser_ || HT == *buffer_parser_)  // remove leading spaces
                        ++buffer_parser_;

                    AffectFieldValue(last_header_field_, buffer_parser_, buffer_end_-1);
                }
                else
                // If the completed line has a ':', then we can extract the field association
                if (nullptr != affect_sign)
                {
                    ExtractAssociation(buffer_parser_, affect_sign, buffer_end_-1);
                    affect_sign = nullptr;
                }


                if (LF == buffer_end_[-2] && CR == buffer_end_[-3])
                {
                    // CRLFCRLF found : the header is completely parsed
                    set_parse_status(kHeaderParsed);
                    return true;
                }

                ++buffer_end_;
                buffer_parser_ = buffer_end_;
            }
            else 
            {
                // CR missing : we abort the connection
                return false;
            }
        }
        return true;
    }

    void ExtractAssociation(const char* begin, const char* affect_sign, const char* end)
    {
        std::string name, value;
        const char* end_name = affect_sign;
        while ((SP == *begin) || (HT == *begin)) ++begin;                   // Remove leading spaces
        while ((SP == *end_name) || (HT == *end_name)) --end_name;          // Remove trailing spaces
        name.append(begin, end_name);

        const char* begin_value = affect_sign + 1;
        while ((SP == *begin_value) || (HT == *begin_value)) ++begin_value; // Remove leading spaces
        while ((SP == *end) || (HT == *end)) --end;                         // Remove trailing spaces        
        value.append(begin_value, end);
        
//        for (auto& c : name) if (c>='A'&& c<='Z') c-='A'-'a';                   // Convert name to lowercase




        switch (name[0])
        {
        case 'A':   // Accept, Accept-encoding, Accept-language, Accept-charset, Authorization
            if (name.length()>=6)
            {
                switch (name[1])
                {
                case 'c':
                    if (0 == name.compare(2, 4, "cept"))
                    {
                        if (6 ==name.length()) last_header_field_ = kAccept;
                        else
                        {
                            if (0 == name.compare(6, 9, "-encoding")) last_header_field_ = kAccept_encoding;
                            else
                                if (0 == name.compare(6, 9, "-language")) last_header_field_ = kAccept_language;
                                else
                                    if (0 == name.compare(6, 8, "-charset")) last_header_field_ = kAccept_charset;
                        }
                    }
                    break;

                case 'u':
                    if (0 == name.compare(2, 11, "thorization")) last_header_field_ = kAuthorization;
                    break;
                }
            }
            break;            

        case 'C':   // Connection, Cookie
            if (0 == name.compare(1, 9, "onnection")) last_header_field_ = kConnection;
            else
                if (0 == name.compare(1, 5, "ookie")) last_header_field_ = kCookie;
                else
                    if (0 == name.compare(1, 12, "ache-Control")) last_header_field_ = kCache_Control;
            break;

        case 'H':   // Host
            if (0 == name.compare(1, 3, "ost")) last_header_field_ = kHost;
            break;

        case 'R':   // Referer
            if (0 == name.compare(1, 6, "eferer")) last_header_field_ = kReferer;
            break;

        case 'U':   // Upgrade, User-agent
            if (0 == name.compare(1, 6, "pgrade")) last_header_field_ = kUpgrade;
            else
                if (0 == name.compare(1, 9, "ser-Agent")) last_header_field_ = kUser_Agent;
            break;
        }
       
        AffectFieldValue(last_header_field_, begin_value, end);
//        std::cout << "ExtractAssociation : " << name << " -> " << value << std::endl;
    }
    
    void AffectFieldValue(HeaderField field, const char* value_begin, const char* value_end)
    {
        switch (field)
        {
        case kNone:
            break;
        case kAccept:
            accept_.append(value_begin, value_end);
            break;
        case kAccept_encoding:
            accept_encoding_.append(value_begin, value_end);
            break;
        case kAccept_language:
            accept_language_.append(value_begin, value_end);
            break;
        case kAccept_charset:
            accept_charset_.append(value_begin, value_end);
            break;
        case kAuthorization:
            authorization_.append(value_begin, value_end);
            break;
        case kConnection:
            if ('c' == *value_begin)            // close
               set_connection_keepalive(false);
            break;
        case kCookie:
            cookie_.append(value_begin, value_end);
            break;
        case kCache_Control:
            cache_control_.append(value_begin, value_end);
            break;
        case kHost:
            host_.append(value_begin, value_end);
            break;
        case kReferer:
            referer_.append(value_begin, value_end);
            break;
        case kUpgrade:
            upgrade_.append(value_begin, value_end);
            break;
        case kUser_Agent:
            user_agent_.append(value_begin, value_end);
            break;
        }
    }

    bool HeaderIsComplete() const   { return parse_status() >= kHeaderParsed; }
    char* GetBuffer()               { return buffer_end_; }
    size_t GetBufferSize() const    { return buffer_.size(); }
    Method method() const           { return method_; }

    const std::string& url() const               { return url_; }
    const std::string& host() const             { return host_; }
    const std::string& user_agent() const       { return user_agent_; }
    const std::string& accept() const           { return accept_; }
    const std::string& accept_encoding() const  { return accept_encoding_; }
    const std::string& accept_language() const  { return accept_language_; }
    const std::string& accept_charset() const   { return accept_charset_; }
    const std::string& authorization() const    { return authorization_; }
    const std::string& cache_control() const    { return cache_control_; }
    const std::string& cookie() const           { return cookie_; }
    const std::string& referer() const          { return referer_; }
    const std::string& upgrade() const          { return upgrade_; }
    bool connection_keepalive() const           { return connection_keepalive_; }

private:
    enum ParseStatus;

    void set_method(Method method)              { method_ = method; }
    void set_parse_status(ParseStatus status)   { parse_status_ = status; }
    void set_connection_keepalive(bool keep)    { connection_keepalive_ = keep; }
    ParseStatus parse_status() const            { return parse_status_; }


    // Request header data
    Method method_;
    std::string url_;
    std::string http_version_;
    std::string host_;
    std::string user_agent_;
    std::string accept_;
    std::string accept_encoding_;
    std::string accept_language_;
    std::string accept_charset_;
    std::string authorization_;
    std::string cache_control_;
    std::string cookie_;
    std::string referer_;
    std::string upgrade_;
    bool connection_keepalive_;

private:
    enum ParseStatus
    {
        kUnparsed = 1, kMethodParsed, kMethodLineParsed, kHeaderParsed, kContentParsed
    } parse_status_;

    enum HeaderField 
    {
        kNone, kAccept, kAccept_encoding, kAccept_language, kAccept_charset, kAuthorization,
        kCache_Control, kConnection, kCookie, kHost, kReferer, kUpgrade, kUser_Agent
    };



    static const unsigned int kReceiveBufferSize = 16384;
    std::array<char, kReceiveBufferSize> buffer_;
    char* buffer_end_;                                      //< First unfilled character in the buffer
    char* buffer_parser_;                                   //< Next character to be parsed
    HeaderField last_header_field_;
};

HTTPRequest::HTTPRequest() : pimpl_(new Impl()) {}
HTTPRequest::~HTTPRequest() {};
void HTTPRequest::Reinitialize() { pimpl_->Reinitialize(); }
bool HTTPRequest::ParseData(size_t bytes) { return pimpl_->ParseData(bytes); }
bool HTTPRequest::HeaderIsComplete() const { return pimpl_->HeaderIsComplete(); }
char* HTTPRequest::GetBuffer() { return pimpl_->GetBuffer(); }
size_t HTTPRequest::GetBufferSize() const { return pimpl_->GetBufferSize(); }
HTTPRequest::Method HTTPRequest::method() const { return pimpl_->method(); }
const std::string& HTTPRequest::url() const             { return pimpl_->url(); }
const std::string& HTTPRequest::host() const            { return pimpl_->host(); }
const std::string& HTTPRequest::user_agent() const      { return pimpl_->user_agent(); }
const std::string& HTTPRequest::accept_encoding() const { return pimpl_->accept_encoding(); }
const std::string& HTTPRequest::accept_language() const { return pimpl_->accept_language(); }
const std::string& HTTPRequest::accept_charset() const  { return pimpl_->accept_charset(); }
const std::string& HTTPRequest::authorization() const   { return pimpl_->authorization(); }
const std::string& HTTPRequest::cache_control() const   { return pimpl_->cache_control(); }
const std::string& HTTPRequest::cookie() const          { return pimpl_->cookie(); }
const std::string& HTTPRequest::referer() const         { return pimpl_->referer(); }
const std::string& HTTPRequest::upgrade() const         { return pimpl_->upgrade(); }
bool HTTPRequest::connection_keepalive() const          { return pimpl_->connection_keepalive(); }
