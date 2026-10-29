function(consumer_setup target)
    set_property(TARGET ${target} PROPERTY CONSUMER_SETUP ON)
endfunction()

function(consumer_other_setup target)
    set_property(TARGET ${target} PROPERTY CONSUMER_OTHER_SETUP ON)
endfunction()

if(MUTATE_OTHER)
    set(WEBFRONT_EMBED_CONSUMER_OTHER OFF)
endif()
