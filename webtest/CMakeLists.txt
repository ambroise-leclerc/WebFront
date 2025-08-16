add_executable(webtest JasmineTest.cpp)

# Link basic libraries
target_link_libraries(webtest PRIVATE WebFront)

# Add CEF support if available (same as WebFrontApp)
if(WEBFRONT_EMBED_CEF AND TARGET cef)
    # Add CEF include directories
    target_include_directories(webtest SYSTEM PRIVATE "${CMAKE_BINARY_DIR}/_deps/cef_binaries-src")
    
    # Link CEF libraries
    target_link_libraries(webtest PRIVATE cef)
    
    # Define that CEF is available
    target_compile_definitions(webtest PRIVATE WEBFRONT_EMBED_CEF)
endif()

# Link the CEF wrapper library if it exists
if(WEBFRONT_EMBED_CEF AND TARGET libcef_dll_wrapper)
    target_link_libraries(webtest PRIVATE libcef_dll_wrapper)
endif()

# Link the main CEF framework directly if CEF target exists
if(WEBFRONT_EMBED_CEF AND TARGET cef)
    target_link_libraries(webtest PRIVATE cef)
endif()

