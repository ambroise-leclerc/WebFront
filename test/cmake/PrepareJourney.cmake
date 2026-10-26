# Completes the manifest that wf init and wf install wrote in the project of the developer journey (#316): the entries and
# the provider that wf build and wf embed use, the specs of wf test, a script under permissions; then copies the files
# the starter does not bring (tsconfig.json for wf check, a spec, the script).
# Usage: cmake -DSOURCE=<test/fixtures/journey> -DPROJECT=<project directory> -P PrepareJourney.cmake
set(manifest_path "${PROJECT}/webfront.project.json")
if(NOT EXISTS "${manifest_path}")
    message(FATAL_ERROR "wf init has not created ${manifest_path}")
endif()
file(READ "${manifest_path}" manifest)
string(JSON manifest SET "${manifest}" entries "{\"main\": \"ui/main.tsx\"}")
string(JSON manifest SET "${manifest}" providers "{\"LifeUI\": {\"entry\": \"main\", \"output\": \"generated/LifeUI.hpp\"}}")
string(JSON manifest SET "${manifest}" test "{\"specs\": [\"tests/life.spec.js\"]}")
string(JSON manifest SET "${manifest}" scripts "{\"denied\": \"tools/denied.mjs\"}")
string(JSON manifest SET "${manifest}" permissions "{\"files\": [\"tools\"], \"publish\": [\"generated\"]}")
file(WRITE "${manifest_path}" "${manifest}\n")
file(COPY "${SOURCE}/" DESTINATION "${PROJECT}")
