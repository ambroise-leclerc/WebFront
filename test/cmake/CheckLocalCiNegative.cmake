# Proves that CheckLocalCi.cmake rejects drift and accepts equivalent command/matrix layouts.
#   cmake -DROOT=<checkout> -DWORK_DIR=<scratch> -P test/cmake/CheckLocalCiNegative.cmake
cmake_minimum_required(VERSION 3.31)

if(NOT ROOT OR NOT WORK_DIR)
    message(FATAL_ERROR "ROOT and WORK_DIR are required")
endif()

# Mutate copies by replacing `from` with `to`. A nonempty `expect` requires failure
# with that diagnostic; an empty one requires success.
function(check_mutation name file from to expect)
    set(dir "${WORK_DIR}/${name}")
    file(REMOVE_RECURSE "${dir}")
    file(MAKE_DIRECTORY "${dir}/workflows")
    file(COPY "${ROOT}/.github/workflows/BuildAndTest.yml" "${ROOT}/.github/workflows/MddlogIntegration.yml" DESTINATION "${dir}/workflows")
    file(COPY "${ROOT}/cmake/local-ci/lanes.json" DESTINATION "${dir}")
    file(COPY "${ROOT}/docs/LOCAL_CI.md" DESTINATION "${dir}")
    if(file STREQUAL "lanes.json" OR file STREQUAL "LOCAL_CI.md")
        set(target "${dir}/${file}")
    else()
        set(target "${dir}/workflows/${file}")
    endif()
    file(READ "${target}" text)
    string(FIND "${text}" "${from}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "${name}: '${from}' is not in ${file}, the mutation proves nothing")
    endif()
    string(REPLACE "${from}" "${to}" text "${text}")
    file(WRITE "${target}" "${text}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -DROOT=${ROOT} -DWORKFLOWS_DIR=${dir}/workflows
            -DLANES_FILE=${dir}/lanes.json -DDOC_FILE=${dir}/LOCAL_CI.md -P "${CMAKE_CURRENT_LIST_DIR}/CheckLocalCi.cmake"
        RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(expect STREQUAL "")
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "${name}: equivalent workflow was rejected:\n${out}${err}")
        endif()
        return()
    endif()
    if(result EQUAL 0)
        message(FATAL_ERROR "${name}: the drift was not rejected")
    endif()
    string(FIND "${out}${err}" "${expect}" reported)
    if(reported EQUAL -1)
        message(FATAL_ERROR "${name}: expected '${expect}' in:\n${out}${err}")
    endif()
endfunction()

check_mutation(new-job BuildAndTest.yml "\n  web-integration:\n" "\n  extra-job:\n    runs-on: x\n    steps: []\n\n  web-integration:\n" "extra-job")
check_mutation(matrix-value BuildAndTest.yml "sanitizer: [address-undefined, thread]" "sanitizer: [address-undefined, thread, memory]" "memory")
check_mutation(cmake-version BuildAndTest.yml "version: '4.4.4'" "version: '4.4.3'" "4.4.3")
check_mutation(mddlog-ref MddlogIntegration.yml "ref: 5b43265664d83c3b7135d328f611e685792a1b02" "ref: 0000000000000000000000000000000000000000" "mddlog")
check_mutation(compiler BuildAndTest.yml "clang-23" "clang-24" "clang-23")
check_mutation(option BuildAndTest.yml "-DENABLE_COVERAGE=ON" "-DENABLE_COVERAGE=OFF" "-DENABLE_COVERAGE=ON")
check_mutation(ctest-label BuildAndTest.yml "-L web-integration" "-L web" "-L web-integration")
check_mutation(lane-removed lanes.json "\"id\": \"web-integration-gcc\"" "\"id\": \"web-integration-gcc-2\"" "web-integration-gcc-2")
check_mutation(undocumented-lane LOCAL_CI.md "`sanitizers-thread`" "`sanitizers-tsan`" "sanitizers-thread")

# Compare both directions: additions and removal from either side are drift too.
check_mutation(extra-option BuildAndTest.yml "-DENABLE_COVERAGE=ON" "-DENABLE_COVERAGE=ON -DENABLE_SANITIZER_ADDRESS=ON" "configure options")
check_mutation(extra-ctest-filter BuildAndTest.yml "ctest --test-dir build -C" "ctest --test-dir build -L web-integration -C" "CTest arguments")
check_mutation(extra-ctest-exclude BuildAndTest.yml "ctest --test-dir build -C" "ctest --test-dir build -LE web-integration -C" "CTest arguments")
check_mutation(extra-ctest-regex BuildAndTest.yml "ctest --test-dir build -C" "ctest --test-dir build -R transport -C" "CTest arguments")
check_mutation(ctest-parallel-operand BuildAndTest.yml "--no-tests=error --parallel 4" "--no-tests=error --parallel 3" "CTest arguments")
check_mutation(extra-target BuildAndTest.yml "--target webtest developer_integration webfront-wf" "--target webtest developer_integration webfront-wf tests" "build targets")
check_mutation(removed-declared-option lanes.json "\"-DENABLE_COVERAGE=ON\"" "\"-DENABLE_COVERAGE=OFF\"" "configure options")

# The flags still occur in the job, but belong to the wrong matrix rows.
check_mutation(swapped-sanitizers MddlogIntegration.yml
    "- name: address-undefined\n          flags: -fsanitize=address,undefined -fno-sanitize-recover=undefined\n        - name: thread\n          flags: -fsanitize=thread"
    "- name: address-undefined\n          flags: -fsanitize=thread\n        - name: thread\n          flags: -fsanitize=address,undefined -fno-sanitize-recover=undefined"
    "configure options")
check_mutation(swapped-sanitizer-branches BuildAndTest.yml
    "matrix.sanitizer == 'thread' &&" "matrix.sanitizer == 'address-undefined' &&" "configure options")
check_mutation(unresolved-matrix-command BuildAndTest.yml
    "matrix.options }}" "matrix.unknown_options }}" "unsupported command expression")

# Additional axes and exclusions need explicit support, not a partial expansion.
check_mutation(extra-inline-axis BuildAndTest.yml
    "compiler: [gcc, clang]" "compiler: [gcc, clang]\n        optimization: [O1, O2]" "unsupported matrix field 'optimization'")
check_mutation(extra-block-axis BuildAndTest.yml
    "compiler: [gcc, clang]" "compiler: [gcc, clang]\n        optimization:\n          - O1\n          - O2" "unsupported matrix field 'optimization'")
check_mutation(matrix-exclusion BuildAndTest.yml
    "compiler: [gcc, clang]" "compiler: [gcc, clang]\n        exclude:\n          - compiler: clang" "unsupported matrix field 'exclude'")

# Every execution mode must keep its own commands, not merely evidence elsewhere.
check_mutation(installed-tests-removed MddlogIntegration.yml
    "run: ctest --test-dir build-mddlog-install --output-on-failure --no-tests=error"
    "run: echo tests skipped" "ctests count")
check_mutation(installed-option-changed MddlogIntegration.yml
    [[-DCMAKE_PREFIX_PATH=${{ github.workspace }}/mddlog-install]]
    [[-DCMAKE_PREFIX_PATH=${{ github.workspace }}/wrong-install]] "configure options")
check_mutation(catch-order-changed MddlogIntegration.yml
    "--order rand" "--order lex" "Catch2 arguments")
check_mutation(catch-full-suite-removed MddlogIntegration.yml
    "run: ./build-sanitized/test/unit/tests --order rand" "run: echo tests skipped" "catch_calls count")
check_mutation(catch-repeat-changed MddlogIntegration.yml
    "for iteration in 1 2 3 4 5; do" "for iteration in 1 2 3; do" "Catch2 arguments")

check_mutation(extra-target-equals BuildAndTest.yml
    "cmake --build build --config" "cmake --build build --target=tests --config" "build targets")
check_mutation(installed-tests-wrong-directory MddlogIntegration.yml
    "ctest --test-dir build-mddlog-install" "ctest --test-dir build-mddlog-source" "configured and tested directories")
check_mutation(duplicate-matrix-value BuildAndTest.yml
    "compiler: [gcc, clang]" "compiler: [gcc, clang, clang]" "matrix has duplicate values")
check_mutation(ctest-repeat-changed BuildAndTest.yml
    "until-fail:" "until-pass:" "CTest arguments")
check_mutation(comment-cannot-replace-option BuildAndTest.yml
    "options: -DWEBFRONT_EMBED_WEBVIEW2=ON -DWEBFRONT_WEBTEST_FRONTEND=WebView2"
    "# -DWEBFRONT_EMBED_WEBVIEW2=ON\n            options: -DWEBFRONT_WEBTEST_FRONTEND=WebView2" "configure options")

# Equivalent layouts must stay valid: ordering and shell quoting are not drift.
check_mutation(reordered-ctest-options BuildAndTest.yml
    "--output-on-failure --no-tests=error" "--no-tests=error --output-on-failure" "")
check_mutation(quoted-cmake-option BuildAndTest.yml
    "-DENABLE_COVERAGE=ON" "'-DENABLE_COVERAGE=ON'" "")
check_mutation(reordered-matrix-rows MddlogIntegration.yml
    "- name: address-undefined\n          flags: -fsanitize=address,undefined -fno-sanitize-recover=undefined\n        - name: thread\n          flags: -fsanitize=thread"
    "- name: thread\n          flags: -fsanitize=thread\n        - name: address-undefined\n          flags: -fsanitize=address,undefined -fno-sanitize-recover=undefined" "")
