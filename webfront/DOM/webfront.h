#ifndef __WEBFRONT_SITE_H_
#define __WEBFRONT_SITE_H_

#include <string>
#include <memory>

#include "page.h"

namespace WebFront
{
class Site
{
public:
    Site();
    Site(unsigned int port);
    Site(std::string protocol);
    ~Site();

    std::shared_ptr<Page> CreatePage(const std::string page_name);
    std::shared_ptr<Page> GetPage(const std::string page_name);
    std::shared_ptr<Page> RemovePage(const std::string page_name);

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};
}
#endif //__WEBFRONT_H_
