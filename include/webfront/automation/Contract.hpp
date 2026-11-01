/// @brief Trusted versioned JSON Schema contract, projected into CLI discovery and MCP schemas.
#pragma once
#include "Json.hpp"

namespace webfront::automation {
inline constexpr std::string_view contractSource = R"JSON({
 "$schema":"https://json-schema.org/draft/2020-12/schema",
 "$id":"urn:webfront:automation:v1",
 "title":"WebFront developer operation contract v1",
 "type":"object","additionalProperties":false,"required":["session","operation"],
 "properties":{
   "session":{"type":"string","minLength":1},
   "operation":{"type":"string","enum":["check","build","test","preview","inspect","click","fill","init","create","install","restore","embed"]},
   "arguments":{"type":"object","additionalProperties":false,"properties":{
     "entry":{"type":"string","maxLength":4096,"description":"Project-relative path or manifest entry; no leading option or path traversal."},
     "pattern":{"type":"string","maxLength":4096,"description":"Spec name substring, never a command option."},
     "pageId":{"type":"string","minLength":1,"maxLength":4096},
     "elementId":{"type":"string","minLength":1,"maxLength":4096},
     "value":{"type":"string","maxLength":4096},
     "name":{"type":"string","maxLength":4096,"description":"Project name written by init; never a command option."},
     "starter":{"type":"string","maxLength":4096,"description":"Starter name for create, never a command option."},
     "directory":{"type":"string","maxLength":4096,"description":"Project-relative directory for create; wf refuses an existing one."},
     "package":{"type":"string","maxLength":4096,"description":"One package name with optional range for install, from the configured registry; omit to install the locked graph."},
     "provider":{"type":"string","maxLength":4096,"description":"Manifest provider label for embed."},
     "output":{"type":"string","maxLength":4096,"description":"Project-relative provider header for embed with an entry; an existing file is replaced."}
   }}
 },
 "$defs":{
   "commandResult":{"type":"object","required":["exitCode","output","report"],"properties":{
     "exitCode":{"type":"integer","minimum":0,"maximum":255},"output":{"type":"string"},
     "cancelled":{"type":"boolean"},"report":{"type":["object","null"]},"diagnostics":{"type":"array"},"data":{"type":"object"},"truncated":{"type":"boolean"}}},
   "errors":{"enum":["not_authorized","wrong_session","busy","invalid_arguments","not_ready","unknown_run","disconnected","timeout"]},
   "run":{"type":"object","required":["runId","operation","state","cancelRequested","output","result"],"properties":{
     "runId":{"type":"string"},"operation":{"type":"string"},"state":{"enum":["queued","running","succeeded","failed","cancelled","timeout","disconnected"]},
     "cancelRequested":{"type":"boolean"},"output":{"type":"string"},"result":{"oneOf":[{"$ref":"#/$defs/commandResult"},{"type":"null"}]}}}
 },
 "x-webfront-version":1,
 "x-webfront-limits":{"requestBytes":60000,"jsonDepth":32,"argumentBytes":4096,"retainedRuns":64},
 "x-webfront-operations":{
   "check":{"permission":"check","arguments":[],"required":[]},
   "build":{"permission":"build","arguments":["entry"],"required":[]},
   "test":{"permission":"test","arguments":["pattern"],"required":[]},
   "preview":{"permission":"preview","arguments":["entry"],"required":[]},
   "inspect":{"permission":"inspect","arguments":[],"required":[]},
   "click":{"permission":"interact","arguments":["pageId","elementId"],"required":["pageId","elementId"]},
   "fill":{"permission":"interact","arguments":["pageId","elementId","value"],"required":["pageId","elementId","value"]},
   "init":{"permission":"init","since":"0.6.4","arguments":["name"],"required":[]},
   "create":{"permission":"create","since":"0.6.4","arguments":["starter","directory"],"required":["starter"]},
   "install":{"permission":"install","since":"0.6.4","arguments":["package"],"required":[]},
   "restore":{"permission":"restore","since":"0.6.4","arguments":[],"required":[]},
   "embed":{"permission":"embed","since":"0.6.4","arguments":["provider","entry","output"],"required":[]}
 }
})JSON";

inline std::vector<std::string> contractStrings(std::string_view value) {
    std::vector<std::string> strings;
    json::Reader             reader{value};
    reader.array([&] {
        strings.push_back(reader.string());
    });
    reader.end();
    return strings;
}
class OperationError : public std::runtime_error {
public:
    OperationError(std::string category, std::string message) : std::runtime_error(std::move(message)), code(std::move(category)) { }
    const std::string code;
};
/// The channel to the native owner failed: the outcome of the request in flight is unknown.
class TransportError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
inline const Object& contractObject() {
    static const Object contract{contractSource};
    return contract;
}
inline const Object& argumentProperties() {
    static const Object properties{Object(Object(contractObject().raw("properties")).raw("arguments")).raw("properties")};
    return properties;
}
struct OperationDefinition {
    Object                   schema;
    std::string              permission;
    std::vector<std::string> arguments, required;
    std::string              since;  ///< First environment version that runs the operation; empty when every automation environment does.
};
inline const auto& operationDefinitions() {
    static const auto definitions = [] {
        std::map<std::string, OperationDefinition, std::less<>> values;
        const Object                                            schemas{contractObject().raw("x-webfront-operations")};
        for (const auto& [name, raw] : schemas.members()) {
            const Object schema{raw};
            values.emplace(
                name,
                OperationDefinition{schema, schema.string("permission"), contractStrings(schema.raw("arguments")), contractStrings(schema.raw("required")), schema.string("since")});
        }
        return values;
    }();
    return definitions;
}
inline const OperationDefinition& operationDefinition(std::string_view operation) {
    const auto& definitions = operationDefinitions();
    const auto  found       = definitions.find(operation);
    if (found == definitions.end())
        throw std::invalid_argument("Unknown automation operation");
    return found->second;
}
inline const Object& operationContract(std::string_view operation) {
    return operationDefinition(operation).schema;
}
inline const std::string& startSchema() {
    static const auto schema = objectJson({
        {                "type",       contractObject().raw("type")},
        {"additionalProperties",                            "false"},
        {            "required",   contractObject().raw("required")},
        {          "properties", contractObject().raw("properties")}
    });
    return schema;
}
inline void validateOperation(std::string_view operation, const Object& arguments) {
    const auto& definition = operationDefinition(operation);
    const auto& allowed    = definition.arguments;
    for (const auto& [key, raw] : arguments.members()) {
        if (std::ranges::find(allowed, key) == allowed.end())
            throw std::invalid_argument("Unexpected operation argument: " + key);
        const auto value = stringValue(raw);
        if (value.size() > 4096 || value.contains('\0'))
            throw std::invalid_argument("Invalid operation argument");
    }
    for (const auto& key : definition.required) {
        if (!arguments.has(key) || (key != "value" && arguments.string(key).empty()))
            throw std::invalid_argument("Missing operation argument: " + key);
    }
}
}  // namespace webfront::automation
