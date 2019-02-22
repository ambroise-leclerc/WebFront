#include <unordered_map>

#include "webfront.h"
#include "socket_server.h"


#define HTTP_PROTOCOL "http"
namespace WebFront
{

class Site::Impl
{
public:
    template <typename Protocol> Impl(Protocol protocol) : server_(protocol)
    {
        server_.Start();
    }

    std::shared_ptr<Page> CreatePage(const std::string page_name)
    {
        std::shared_ptr<Page> new_page = std::make_shared<Page>(page_name);
        pages_.insert(PagesMap::value_type(page_name, new_page));
        return new_page;
    }

    std::shared_ptr<Page> GetPage(const std::string page_name) const
    {
        const auto page = pages_.find(page_name);
        if (pages_.end() == page)
        {
            return nullptr;
        }
        return page->second;
    }

    std::shared_ptr<Page> RemovePage(const std::string page_name)
    {
        auto page = pages_.find(page_name);
        if (pages_.end() == page)
        {
            return nullptr;
        }
        pages_.erase(page);
        return page->second;
    }

private:

private:
    SocketServer server_;
    typedef std::unordered_map<std::string, std::shared_ptr<Page>> PagesMap;
    PagesMap pages_;
};

Site::Site() : pimpl_(new Impl(HTTP_PROTOCOL)) { }
Site::Site(unsigned int port) : pimpl_(new Impl(port)) {}
Site::Site(std::string protocol) : pimpl_(new Impl(protocol)) {}
Site::~Site() {}

/** Creates a new Page and returns a pointer on it.
 * If the page_name already exist, the Page is created but not linked to the site.
 * \param[in] page_name name/path of the Page
 * \return The created Page
 */
std::shared_ptr<Page> Site::CreatePage(const std::string page_name) { return pimpl_->CreatePage(page_name); }

/** Get a Page from Site. A pointer on the page is returned.
 * \param[in] page_name name/path of the Page
 * \return The removed Page. nullptr if page_name doesn't exist.
 */
std::shared_ptr<Page> Site::GetPage(const std::string page_name) { return pimpl_->GetPage(page_name); }

/** Removes a Page from Site. A pointer on the removed page is returned.
 * \param[in] page_name name/path of the Page
 * \return The removed Page. nullptr if page_name doesn't exist.
 */
std::shared_ptr<Page> Site::RemovePage(const std::string page_name) { return pimpl_->RemovePage(page_name); }

}