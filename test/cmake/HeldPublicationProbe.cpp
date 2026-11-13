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
        std::cout << "Publication replaced the directory while its previous HTTP source remained readable\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
