# Include CEF support module
include(CEFSupport)

add_executable(webtest JasmineTest.cpp)

# Link basic libraries
target_link_libraries(webtest PRIVATE WebFront)

# Setup complete CEF integration for webtest (same as WebFrontApp)
# This includes:
# - CEF compilation configuration and linking
# - Platform-specific runtime file deployment  
# - Resource file copying (with Linux ICU fix)
# - Locale directory setup
setup_cef_for_target(webtest)

