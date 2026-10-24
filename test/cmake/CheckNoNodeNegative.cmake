# The guard of #303 (CheckNoNode.cmake) must fail on what #303 removes and pass on what it keeps. The injections live in a
# scratch tree, never in the checkout: an import of node:fs, `npm ci` in a workflow, `engines.node` in a manifest, a Node
# script that nothing calls, an old command in a guide, a find_program of node, a shebang, a CommonJS require, the "like
# node.js" positioning. An npm registry URL, the formats and metadata of npm packages, the compile-time constant of the
# bundlers and a test that checks the rejection of a Node import pass.
#   cmake -DROOT=<checkout> -DWORK_DIR=<scratch directory> -P test/cmake/CheckNoNodeNegative.cmake
cmake_minimum_required(VERSION 3.31)

if(NOT ROOT OR NOT WORK_DIR)
    message(FATAL_ERROR "ROOT and WORK_DIR are required")
endif()
set(guard "${ROOT}/test/cmake/CheckNoNode.cmake")
file(WRITE "${WORK_DIR}/no-exceptions.cmake" "set(NODE_GUARD_EXCEPTIONS \"\")\n")

# audit(<expected status> <description> <class that must be named, or empty>): audits the tree of WORK_DIR/tree.
function(audit expected description class)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DROOT=${WORK_DIR}/tree" "-DEXCEPTIONS=${WORK_DIR}/no-exceptions.cmake" -P "${guard}"
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if(NOT status EQUAL expected)
        message(FATAL_ERROR "${description}: expected status ${expected}, got ${status}\n${output}${errors}")
    endif()
    if(class AND NOT errors MATCHES "\\[${class}\\]")
        message(FATAL_ERROR "${description}: the class ${class} is not reported\n${errors}")
    endif()
endfunction()

# fresh(<path> <content>): the scratch tree holds one file.
function(fresh path content)
    file(REMOVE_RECURSE "${WORK_DIR}/tree")
    file(WRITE "${WORK_DIR}/tree/${path}" "${content}")
endfunction()

fresh("README.md" "# WebFront\n\nBuild with CMake. Packages come from the npm registry https://registry.npmjs.org/.\n")
audit(0 "a clean tree" "")

# What must fail.
fresh("tools/check.mjs" "import fs from 'node:fs';\nfs.readFileSync('x');\n")
audit(1 "an import of node:fs" "node-import")
fresh(".github/workflows/Build.yml" "steps:\n  - run: npm ci --prefix tools\n")
audit(1 "npm ci in a workflow" "npm-command")
fresh(".github/workflows/Build.yml" "steps:\n  - uses: actions/setup-node@v6\n")
audit(1 "setup-node in a workflow" "node-config")
fresh("package.json" "{\n  \"name\": \"x\",\n  \"engines\": {\"node\": \">=22\"}\n}\n")
audit(1 "engines.node in a manifest of the project" "node-config")
fresh("tools/unused.mjs" "const [, , input] = process.argv;\nconsole.log(input);\n")
audit(1 "a Node script that nothing calls" "node-api")
fresh("docs/GUIDE.md" "Regenerate the assets:\n\n```sh\nnode tools/build.mjs out\n```\n")
audit(1 "an old command in a guide" "node-command")
fresh("docs/GUIDE.md" "Install first with `npx esbuild`.\n")
audit(1 "npx in a guide" "tool-word")
fresh("CMakeLists.txt" "find_program(NODE_EXECUTABLE NAMES node REQUIRED)\n")
audit(1 "a find_program of node" "node-config")
fresh("bin/tool" "#!/usr/bin/env node\nconsole.log(1);\n")
audit(1 "a shebang" "shebang")
fresh("tools/legacy.js" "const fs = require('fs');\n")
audit(1 "a CommonJS require" "commonjs")
fresh("docs/GUIDE.md" "Tests require Node.js 22 or newer.\n")
audit(1 "a requirement in a guide" "requirement")
fresh("docs/GUIDE.md" "wf works à la node.js.\n")
audit(1 "the positioning of #303" "positioning")

# What must pass.
fresh("src/bundle.mjs" "define: {'process.env.NODE_ENV': '\"production\"'}\n")
audit(0 "the compile-time constant of the bundlers" "")
fresh("lock.json" "{\"packages\": {\"semver@7.7.3\": {\"integrity\": \"sha512-x\", \"resolved\": \"https://registry.npmjs.org/semver/-/semver-7.7.3.tgz\"}}}\n")
audit(0 "an npm registry URL and the metadata of a package" "")
fresh("spec/rejects.mjs" "it('refuses a Node import', () => expect(compile('import fs from \"fs\"')).toThrowError(/Node API/));\n")
audit(0 "a test that checks the rejection of a Node import" "")
fresh("docs/GUIDE.md" "wf install reads package.json files and the dist-tags of the registry.\n")
audit(0 "the formats of npm packages" "")

# An exception that matches nothing is itself an error, and one that matches is accepted.
fresh("docs/HISTORY.md" "The old build ran `npm ci`.\n")
file(WRITE "${WORK_DIR}/allowing.cmake" "set(NODE_GUARD_EXCEPTIONS \"docs/HISTORY.md|npm-command|history of the migration\")\n")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DROOT=${WORK_DIR}/tree" "-DEXCEPTIONS=${WORK_DIR}/allowing.cmake" -P "${guard}"
    RESULT_VARIABLE status ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "A listed exception must be accepted:\n${errors}")
endif()
fresh("docs/HISTORY.md" "The old build was replaced.\n")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DROOT=${WORK_DIR}/tree" "-DEXCEPTIONS=${WORK_DIR}/allowing.cmake" -P "${guard}"
    RESULT_VARIABLE status ERROR_VARIABLE errors)
if(status EQUAL 0 OR NOT errors MATCHES "match nothing any more")
    message(FATAL_ERROR "An exception that matches nothing must fail:\n${errors}")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
