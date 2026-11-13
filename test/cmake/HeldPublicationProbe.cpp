#include <webfront/system/DeveloperFS.hpp>
#include "../unit/TemporaryDirectory.hpp"
#include <iostream>
#include <vector>
int main() {
    try {
        webfront::test::TemporaryDirectory project;
        webfront::developer::Store store{project.path()};
        webfront::fs::DeveloperFS fs{project.path()};
        auto first=store.begin(".webfront/developer/preview",true);
        store.write(first,"app.js",0,std::vector<std::uint8_t>{'o','l','d'});
        store.commit(first);
        auto held=fs.open("webfront/developer/preview/app.js");
        if(!held) return 2;
        auto second=store.begin(".webfront/developer/preview",true);
        store.write(second,"app.js",0,std::vector<std::uint8_t>{'n','e','w'});
        store.commit(second);
        char content[3]{};
        if(held->read(content)!=3 || std::string_view(content,3)!="old") return 3;
        auto latest=fs.open("webfront/developer/preview/app.js");
        if(!latest || latest->read(content)!=3 || std::string_view(content,3)!="new") return 4;
        const auto staging = project.path()/".webfront/developer/staging";
        if (std::filesystem::directory_iterator(staging) != std::filesystem::directory_iterator{}) {
            std::cerr << "Publication left a backup while a section was retained\n";
            return 5;
        }
        auto third=store.begin(".webfront/developer/preview",true);
        store.write(third,"app.js",0,{});
        store.commit(third);
        auto empty=fs.open("webfront/developer/preview/app.js");
        if(!empty || empty->knownSize()!=0 || empty->read(content)!=0) return 6;
        std::cout << "Publication replaced the directory while its previous HTTP source remained readable\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
