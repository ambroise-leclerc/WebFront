cmake_minimum_required(VERSION 3.31)
project(Quote LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(WEBFRONT_EMBED_WKWEBVIEW ON CACHE BOOL "" FORCE)
add_subdirectory(WebFront)
enable_testing()
add_executable(Quote main.cpp)
target_link_libraries(Quote PRIVATE WebFront::WebFront)
webfront_use_native_window(Quote)
webfront_add_jasmine_test(NAME QuoteJourney TARGET Quote
    SPECS tests/ui/quote.spec.js FRONTEND WKWebView TIMEOUT 60)
set_tests_properties(QuoteJourney PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
