# A target of another directory, building into the directory of the target "binned": it shares its copy target.
add_custom_target(elsewhere)
set_target_properties(elsewhere PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
copy_for(elsewhere)
expect("${planned}" "${CMAKE_BINARY_DIR}/bin${config}" "absolute output directory")
string(MD5 key "${planned}")
get_target_property(dependencies elsewhere MANUALLY_ADDED_DEPENDENCIES)
expect("${dependencies}" "webfront_cef_runtime_${key}" "shared copy target across directories")
