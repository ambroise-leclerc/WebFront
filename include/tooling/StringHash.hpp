
#include <cstdint>
#include <string_view>

namespace webfront {

/// Produces an uint32_t hashcode of strings. Compile-time version of the algorithm can be
/// used to generate hashcodes from litterals using the "_hash" suffix.
/// Example with switch on strings :
///
/// using namespace webfront;
///
/// void switchOnStrings(std::string command) {
///     switch (stringHash(command)) {
///     case "Start"_hash:
///        ...
///     case "Stop"_hash:
///        ...
///     case "DoSomething"_hash:
///        ...
///     }
/// }
constexpr uint32_t hashConst = 113;

constexpr uint32_t stringHash(std::string_view string, uint32_t seed = 0) {
    return string.empty() ? seed : (stringHash(string.substr(1), (seed * hashConst) + static_cast<uint32_t>(string[0]))); 
}

constexpr uint32_t operator""_hash(char const* zStr, size_t size) { return stringHash(std::string_view(zStr, size)); }

} // namespace webfront
