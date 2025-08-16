#ifndef __WEBFRONT_PAGE_H_
#define __WEBFRONT_PAGE_H_

#include <string>
#include <vector>

namespace WebFront
{
class Site;

class Page
{
public:
    Page(std::string page_name) : page_name_(page_name)
    {
    }

    /*
    Create
    */
    const std::vector<char>& Render();

public:
    std::string page_name_;
};

}

#endif //__WEBFRONT_PAGE_H_