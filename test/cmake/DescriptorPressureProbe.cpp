#include <webfront/system/NativeStorage.hpp>
#include "../unit/TemporaryDirectory.hpp"
#include <sys/resource.h>
#include <unistd.h>
#include <vector>
#include <exception>
#include <iostream>
int main(int argc, char** argv) {
    webfront::test::TemporaryDirectory project;
    webfront::fs::NativeStorage storage{project.path()};
    if(argc>1 && std::string_view(argv[1])=="warm") {
        try { static_cast<void>(storage.openRead("missing")); }
        catch(const webfront::fs::StorageError& error) {
            if(error.errc()!=webfront::fs::StorageErrc::notFound) return 2;
        }
    }
    rlimit original{};
    if(::getrlimit(RLIMIT_NOFILE,&original)) return 3;
    auto lowered=original;
    lowered.rlim_cur=std::min<rlim_t>(original.rlim_cur,256);
    if(::setrlimit(RLIMIT_NOFILE,&lowered)) return 4;
    std::vector<int> held;
    for(int fd=::dup(0);fd>=0;fd=::dup(0)) held.push_back(fd);
    if(held.empty()) return 5;
    ::close(held.back());held.pop_back();
    std::exception_ptr failure;
    try { storage.createDirectory("under-pressure"); }
    catch(...) {failure=std::current_exception();}
    for(int fd:held) ::close(fd);
    if(::setrlimit(RLIMIT_NOFILE,&original)) return 6;
    if(!failure) return 7;
    try { std::rethrow_exception(failure); }
    catch(const webfront::fs::StorageError& error) {
        if(error.errc()==webfront::fs::StorageErrc::rootMoved) return 8;
    }
    storage.createDirectory("after-pressure");
    std::cout << "Descriptor failure and recovery passed with sanitizer checks enabled\n";
}
