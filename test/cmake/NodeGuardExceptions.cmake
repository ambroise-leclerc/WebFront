# The exceptions of the guard of #303 (CheckNoNode.cmake): "<file>|<class>|<why>". Each is a file and the one class of match
# it may keep; a new match anywhere else fails the guard, and an exception that matches nothing any more fails it too.
# The contract of #303 allows only: the registries, formats and metadata of npm; the locks and notices of dependencies;
# historical elements (Git history, earlier versions of the ADRs, migration notes); and the fixtures, diagnostics and tests
# that check the rejection of Node APIs, imports and executables.
set(NODE_GUARD_EXCEPTIONS
    # The guard itself: the words of its patterns and of its documentation, and the injections that it must reject.
    "test/cmake/CheckNoNode.cmake|tool-word|names the tools the guard looks for"
    "test/cmake/CheckNoNode.cmake|shebang|a pattern and a comment of the guard"
    "test/cmake/CheckNoNode.cmake|node-config|names the configuration the guard looks for"
    "test/cmake/CheckNoNode.cmake|positioning|names the positioning the guard looks for"
    "test/cmake/CheckNoNodeNegative.cmake|node-import|injection that the guard must reject"
    "test/cmake/CheckNoNodeNegative.cmake|commonjs|injection that the guard must reject"
    "test/cmake/CheckNoNodeNegative.cmake|node-api|injection that the guard must reject"
    "test/cmake/CheckNoNodeNegative.cmake|npm-command|injection that the guard must reject"
    "test/cmake/CheckNoNodeNegative.cmake|tool-word|injection that the guard must reject"
    "test/cmake/CheckNoNodeNegative.cmake|shebang|injection that the guard must reject"
    "test/cmake/CheckNoNodeNegative.cmake|node-config|injection that the guard must reject"
    "test/cmake/CheckNoNodeNegative.cmake|requirement|injection that the guard must reject"
    "test/cmake/CheckNoNodeNegative.cmake|positioning|injection that the guard must reject"
    "test/cmake/NodeGuardExceptions.cmake|node-config|names the directory whose TypeScript layout is an exception"
    # Specs that check the rejection of Node imports and of installation scripts.
    "test/developer/specs/environment-specs.mjs|node-import|checks that an import of a Node module is refused"
    "test/developer/specs/environment-specs.mjs|node-command|a package whose installation script is refused"
    "test/developer/specs/scripts-specs.mjs|node-import|checks that Node built-in modules are recognized and refused"
    # The proof that no JavaScript runtime is needed: the action that deletes them names them.
    ".github/actions/remove-node/action.yml|tool-word|names the tools it removes from the runner"
    # Migration note and history.
    "docs/NODE_TOOLS_MIGRATION.md|npm-command|migration note: names the former commands"
    "docs/NODE_TOOLS_MIGRATION.md|tool-word|migration note: names the former tools"
    "docs/CHANGELOG.md|node-config|history of the releases, which names the options that existed"
    # Diagnostics and tests of the rejection of Node APIs.
    "js/developer/environment/typecheck-snapshot.mjs|node-config|the layout of /project/node_modules that the TypeScript module resolver reads: wf check cannot resolve a package without it"
    "docs/DEVELOPER_ENVIRONMENT.md|node-config|states which directories wf check never lists, the TypeScript layout among them"
    # Data of dependencies.
    "webfront-packages.lock.json|node-command|the lock records the package.json scripts of its dependencies verbatim, and nothing runs them"
    "webfront-packages.lock.json|npm-command|the lock records the package.json scripts of its dependencies verbatim, and nothing runs them"
    "webfront-packages.lock.json|node-config|the lock records the file lists of its dependencies verbatim"
    "js/developer/environment/packages.bundle.mjs|node-api|semver's debug probe, guarded by typeof process, inside the bundle that is a checked-in generated artifact (lock and notices in PACKAGES-LICENSE.txt)")
