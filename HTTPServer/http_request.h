#ifndef __HTTP_REQUEST_H_
#define __HTTP_REQUEST_H_

#include <memory>
#include <string>

class HTTPRequest
{
public:
    enum Method 
    {
        kUndefined, kGet, kHead
    };

public:
    HTTPRequest();
    ~HTTPRequest();
    void Reinitialize();
    bool ParseData(size_t bytes);   //< Parse newly received data. Return false if there is a non-recoverable parse error.
    bool HeaderIsComplete() const;  //< Returns true if a complete header has been parsed
    char* GetBuffer();
    size_t GetBufferSize() const;
    Method method() const;
    const std::string& url() const;
    const std::string& host() const;
    const std::string& user_agent() const;
    const std::string& accept_encoding() const;
    const std::string& accept_language() const;
    const std::string& accept_charset() const;
    const std::string& authorization() const;
    const std::string& cache_control() const;
    const std::string& cookie() const;
    const std::string& referer() const;
    const std::string& upgrade() const;
    bool connection_keepalive() const;

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

#endif