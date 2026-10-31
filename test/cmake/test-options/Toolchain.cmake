# Normal variables, as a toolchain file sets them: TOOLCHAIN_LEGACY sets the former name, TOOLCHAIN_PREFIXED the new one.
if(DEFINED TOOLCHAIN_LEGACY)
    set(ENABLE_TESTING ${TOOLCHAIN_LEGACY})
endif()
if(DEFINED TOOLCHAIN_PREFIXED)
    set(WEBFRONT_ENABLE_TESTING ${TOOLCHAIN_PREFIXED})
endif()
