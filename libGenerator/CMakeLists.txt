CPMAddPackage("gh:jarro2783/cxxopts@3.0.0")
CPMAddPackage("gh:nlohmann/json@3.10.5")

add_executable(libgen LibGenerator.cpp)
target_link_libraries(libgen PRIVATE project_options project_warnings nlohmann_json::nlohmann_json cxxopts::cxxopts )