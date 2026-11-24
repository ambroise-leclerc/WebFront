#!/usr/bin/env bash
# Fails when a tracked C++ source differs from what its clang-format configuration produces (#653).
#   .github/scripts/check-clang-format.sh [clang-format]     the executable defaults to clang-format
# Each source uses the .clang-format of its directory (include/webfront/, test/, examples/); cmake/ has none and follows include/webfront/.
# Generated or fixed data is exempt; the list matches test/cmake/CheckSourceHeaders.cmake. The reference version is the one pinned in
# .github/workflows/SourceFormat.yml (clang-format 23.1.5): another major version may format differently.
set -euo pipefail

clang_format="${1:-clang-format}"
cd "$(git rev-parse --show-toplevel)"

reference_major=23
version="$("$clang_format" --version)"
if ! grep -Eq "version ${reference_major}\." <<< "$version"; then
    echo "warning: the reference is clang-format ${reference_major}.x, this is: $version" >&2
fi

exempt='^(include/webfront/system/(DeveloperBootstrapData|DeveloperIndexHtmlData|DeveloperReleaseData|DomInterpreterData|FallbackIndexHtmlData|FallbackModuleData|FaviconData|WebFrontJsData)\.hpp|test/unit/GzipFixtures\.hpp)$'

status=0
count=0
while IFS= read -r file; do
    style=file
    case "$file" in
        cmake/*) style=file:include/webfront/.clang-format ;;
    esac
    count=$((count + 1))
    if ! "$clang_format" --dry-run --Werror --style="$style" "$file" 2> /dev/null; then
        echo "not formatted: $file"
        status=1
    fi
done < <(git ls-files '*.hpp' '*.cpp' '*.h' '*.cu' '*.cuh' '*.mm' | grep -Ev "$exempt")

if [ "$status" -ne 0 ]; then
    echo "Run clang-format -i on the files above (version: $("$clang_format" --version))." >&2
else
    echo "$count files formatted"
fi
exit "$status"
