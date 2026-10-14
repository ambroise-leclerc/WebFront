/// @brief Installed web tools (the wf tool manager, #234): validation of the names and paths a
/// browser sends, the files of each tool under <project>/.webfront/tools/, and the lock file
/// <project>/webfront-tools.lock.json that records them.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace webfront::fs {

/// One installed tool, as recorded in the lock file.
struct ToolRecord {
    std::string name{};
    std::string version{};
    std::string integrity{};
    std::string entry{};  ///< Page opened by 'wf open', relative to the package root
    std::string title{};

    bool operator==(const ToolRecord&) const = default;
};

namespace tools {

namespace detail {

inline std::filesystem::path fromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

inline bool isNamePart(std::string_view part) {
    return !part.empty() && part.front() != '.' && part.front() != '_' && std::ranges::all_of(part, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
    });
}

}  // namespace detail

/// Accepts an npm package name: lowercase URL-safe characters, an optional "@scope/" prefix, and no
/// part starting with '.' or '_' (so neither "." nor ".." can appear). Throws std::invalid_argument.
inline void validatePackageName(std::string_view name) {
    bool valid = name.size() <= 214;
    if (valid && name.starts_with('@')) {
        const auto slash = name.find('/');
        valid            = slash != std::string_view::npos && detail::isNamePart(name.substr(1, slash - 1)) && detail::isNamePart(name.substr(slash + 1));
    } else
        valid = valid && detail::isNamePart(name);
    if (!valid)
        throw std::invalid_argument("Invalid package name '" + std::string(name) + "'");
}

/// Accepts a semantic version: a digit followed by letters, digits, '.', '+' or '-'. Throws
/// std::invalid_argument.
inline void validateVersion(std::string_view version) {
    const bool valid = !version.empty() && version.size() <= 256 && version.front() >= '0' && version.front() <= '9'
                       && std::ranges::all_of(version, [](char c) {
                              return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '+' || c == '-';
                          });
    if (!valid)
        throw std::invalid_argument("Invalid package version '" + std::string(version) + "'");
}

/// Converts a '/'-separated path received from the browser into a relative path that stays inside
/// its directory. Rejects absolute paths, empty, "." and ".." components, backslashes, colons (drive
/// letters and alternate streams) and NUL characters with std::invalid_argument.
inline std::filesystem::path safeRelativePath(std::string_view path) {
    using namespace std::string_view_literals;
    const auto unsafe = [path] {
        return std::invalid_argument("Unsafe path '" + std::string(path) + "'");
    };
    if (path.empty() || path.size() > 4096 || path.find_first_of("\\:\0"sv) != std::string_view::npos)
        throw unsafe();
    std::filesystem::path result;
    for (const auto part : std::views::split(path, '/')) {
        const std::string_view component(part.begin(), part.end());
        if (component.empty() || component == "." || component == "..")
            throw unsafe();
        result /= detail::fromUtf8(component);
    }
    return result;
}

namespace detail {

/// Strict reader for the lock file's JSON. Objects and strings are read; other values are skipped.
class JsonReader {
public:
    explicit JsonReader(std::string_view json) : text(json) {}

    /// Reads an object, calling onMember(key) for each member; onMember must consume the value.
    void object(const std::function<void(const std::string&)>& onMember) {
        expect('{');
        if (consumeIf('}'))
            return;
        do {
            const auto key = string();
            expect(':');
            onMember(key);
        } while (consumeIf(','));
        expect('}');
    }

    std::string string() {
        expect('"');
        std::string value;
        for (char c = next(); c != '"'; c = next()) {
            if (static_cast<unsigned char>(c) < 0x20)
                fail("control character in a string");
            if (c == '\\')
                escaped(value);
            else
                value += c;
        }
        return value;
    }

    void skipValue() {
        const char c = peek();
        if (c == '{')
            return object([this](const std::string&) {
                skipValue();
            });
        if (c == '"') {
            string();
            return;
        }
        if (consumeIf('[')) {
            if (consumeIf(']'))
                return;
            do
                skipValue();
            while (consumeIf(','));
            return expect(']');
        }
        const auto start = index;
        while (index < text.size() && (std::isalnum(static_cast<unsigned char>(text[index])) || text[index] == '-' || text[index] == '+' || text[index] == '.'))
            ++index;
        if (index == start)
            fail("value expected");
    }

    void end() {
        if (peek() != '\0' || index != text.size())
            fail("unexpected data after the value");
    }

private:
    std::string_view text;
    std::size_t      index = 0;

    [[noreturn]] void fail(const std::string& reason) const {
        throw std::runtime_error(reason + " at offset " + std::to_string(index));
    }

    char peek() {
        while (index < text.size() && (text[index] == ' ' || text[index] == '\t' || text[index] == '\n' || text[index] == '\r'))
            ++index;
        return index < text.size() ? text[index] : '\0';
    }

    char next() {
        if (index == text.size())
            fail("unterminated string");
        return text[index++];
    }

    bool consumeIf(char c) {
        if (peek() != c)
            return false;
        ++index;
        return true;
    }

    void expect(char c) {
        if (!consumeIf(c))
            fail(std::string("'") + c + "' expected");
    }

    void escaped(std::string& value) {
        switch (const char c = next()) {
            case '"':
            case '\\':
            case '/':
                value += c;
                break;
            case 'b':
                value += '\b';
                break;
            case 'f':
                value += '\f';
                break;
            case 'n':
                value += '\n';
                break;
            case 'r':
                value += '\r';
                break;
            case 't':
                value += '\t';
                break;
            case 'u':
                appendUtf8(value, codePoint());
                break;
            default:
                fail("invalid escape");
        }
    }

    std::uint32_t hex4() {
        std::uint32_t value = 0;
        for (int digit = 0; digit < 4; ++digit) {
            const char c = next();
            value      <<= 4;
            if (c >= '0' && c <= '9')
                value |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                value |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                value |= static_cast<std::uint32_t>(c - 'A' + 10);
            else
                fail("invalid \\u escape");
        }
        return value;
    }

    std::uint32_t codePoint() {
        const auto unit = hex4();
        if (unit >= 0xDC00 && unit <= 0xDFFF)
            fail("unpaired surrogate");
        if (unit < 0xD800 || unit > 0xDBFF)
            return unit;
        if (next() != '\\' || next() != 'u')
            fail("unpaired surrogate");
        const auto low = hex4();
        if (low < 0xDC00 || low > 0xDFFF)
            fail("unpaired surrogate");
        return 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
    }

    static void appendUtf8(std::string& value, std::uint32_t code) {
        const auto byte = [&value](std::uint32_t bits) {
            value += static_cast<char>(bits);
        };
        if (code < 0x80)
            byte(code);
        else if (code < 0x800) {
            byte(0xC0 | (code >> 6));
            byte(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            byte(0xE0 | (code >> 12));
            byte(0x80 | ((code >> 6) & 0x3F));
            byte(0x80 | (code & 0x3F));
        } else {
            byte(0xF0 | (code >> 18));
            byte(0x80 | ((code >> 12) & 0x3F));
            byte(0x80 | ((code >> 6) & 0x3F));
            byte(0x80 | (code & 0x3F));
        }
    }
};

inline std::string jsonString(std::string_view text) {
    constexpr std::string_view hexDigits = "0123456789abcdef";
    std::string                quoted    = "\"";
    for (const char c : text) {
        const auto code = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\')
            (quoted += '\\') += c;
        else if (code < 0x20)
            ((quoted += "\\u00") += hexDigits[code >> 4]) += hexDigits[code & 0xF];
        else
            quoted += c;
    }
    return quoted += '"';
}

inline void validateRecord(const ToolRecord& record) {
    validatePackageName(record.name);
    validateVersion(record.version);
    safeRelativePath(record.entry);
}

}  // namespace detail

using LockedTools = std::map<std::string, ToolRecord, std::less<>>;

/// Parses a lock file. Unknown members are ignored; every recorded name, version and entry is
/// validated, so a hand-edited lock file cannot point outside the tools directory. Throws
/// std::runtime_error on malformed JSON and std::invalid_argument on an invalid record.
inline LockedTools parseLock(std::string_view json) {
    LockedTools        tools;
    detail::JsonReader reader{json};
    reader.object([&](const std::string& key) {
        if (key != "tools")
            return reader.skipValue();
        reader.object([&](const std::string& name) {
            ToolRecord record{.name = name};
            reader.object([&](const std::string& field) {
                if (field == "version")
                    record.version = reader.string();
                else if (field == "integrity")
                    record.integrity = reader.string();
                else if (field == "entry")
                    record.entry = reader.string();
                else if (field == "title")
                    record.title = reader.string();
                else
                    reader.skipValue();
            });
            detail::validateRecord(record);
            tools.insert_or_assign(name, std::move(record));
        });
    });
    reader.end();
    return tools;
}

/// Writes a lock file's JSON, tools sorted by name.
inline std::string formatLock(const LockedTools& tools) {
    std::string json      = "{\n  \"lockfileVersion\": 1,\n  \"tools\": {";
    const char* separator = "\n";
    for (const auto& [name, record] : tools) {
        json     += separator;
        json     += "    " + detail::jsonString(name) + ": {\n";
        json     += "      \"version\": " + detail::jsonString(record.version) + ",\n";
        json     += "      \"integrity\": " + detail::jsonString(record.integrity) + ",\n";
        json     += "      \"entry\": " + detail::jsonString(record.entry) + ",\n";
        json     += "      \"title\": " + detail::jsonString(record.title) + "\n    }";
        separator = ",\n";
    }
    return json += tools.empty() ? "}\n}\n" : "\n  }\n}\n";
}

}  // namespace tools

/// The tools installed in a project directory:
///   <project>/.webfront/tools/<name>@<version>/   the files of each installed tool
///   <project>/webfront-tools.lock.json            name, version, integrity, entry and title of each
/// An installation is staged under .webfront/tools/.staging/ and replaces the installed version
/// only when committed, so a failed or abandoned installation leaves the installed tool intact.
/// Thread-safe within a process. Errors throw std::invalid_argument (invalid input) or
/// std::runtime_error / std::filesystem::filesystem_error (I/O).
class ToolStore {
public:
    explicit ToolStore(std::filesystem::path projectDirectory) : project(std::move(projectDirectory)) {}

    [[nodiscard]] const std::filesystem::path& projectDir() const {
        return project;
    }
    [[nodiscard]] std::filesystem::path toolsDir() const {
        return project / ".webfront" / "tools";
    }
    [[nodiscard]] std::filesystem::path lockFile() const {
        return project / "webfront-tools.lock.json";
    }
    [[nodiscard]] std::filesystem::path installDir(std::string_view name, std::string_view version) const {
        return toolsDir() / tools::detail::fromUtf8(std::string(name) + "@" + std::string(version));
    }

    /// The installed tools; an absent lock file means none. Throws std::runtime_error if the lock
    /// file is malformed.
    [[nodiscard]] tools::LockedTools installed() const {
        return readLock(lockFile());
    }

    [[nodiscard]] std::optional<ToolRecord> find(std::string_view name) const {
        auto tools = installed();
        if (auto found = tools.find(name); found != tools.end())
            return std::move(found->second);
        return std::nullopt;
    }

    /// Starts (or restarts) the installation of a version, discarding the files already staged for it.
    void begin(std::string_view name, std::string_view version) {
        tools::validatePackageName(name);
        tools::validateVersion(version);
        std::scoped_lock lock{mutex};
        std::filesystem::remove_all(stagingDir(name, version));
        pending.insert(key(name, version));
    }

    /// Writes a chunk of a staged file. Offset 0 creates or truncates the file; any other offset must
    /// equal the number of bytes already written, so a missing or repeated chunk is detected.
    void write(std::string_view name, std::string_view version, std::string_view path, std::uint64_t offset, std::span<const std::uint8_t> bytes) {
        const auto       relative = tools::safeRelativePath(path);
        std::scoped_lock lock{mutex};
        requirePending(name, version);
        const auto target = stagingDir(name, version) / relative;
        if (offset != 0) {
            std::error_code error;
            const auto      written = std::filesystem::file_size(target, error);
            if (error || written != offset)
                throw std::invalid_argument("Chunk of '" + std::string(path) + "' at offset " + std::to_string(offset)
                                            + " does not follow the bytes already written");
        }
        std::filesystem::create_directories(target.parent_path());
        std::ofstream file(target, std::ios::binary | (offset == 0 ? std::ios::trunc : std::ios::app));
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file.flush())
            throw std::runtime_error("Cannot write '" + std::string(path) + "'");
    }

    /// Installs the staged files as record.version, replacing any installed version of the tool, and
    /// records it in the lock file. The entry page must have been written.
    void commit(const ToolRecord& record) {
        tools::detail::validateRecord(record);
        std::scoped_lock lock{mutex};
        requirePending(record.name, record.version);
        const auto staged = stagingDir(record.name, record.version);
        if (!std::filesystem::is_regular_file(staged / tools::safeRelativePath(record.entry)))
            throw std::invalid_argument("Entry page '" + record.entry + "' of " + record.name + " was not written");

        auto       tools  = readLock(lockFile());
        const auto target = installDir(record.name, record.version);
        std::filesystem::remove_all(target);
        std::filesystem::create_directories(target.parent_path());
        std::filesystem::rename(staged, target);
        std::optional<std::filesystem::path> replaced;
        if (const auto previous = tools.find(record.name); previous != tools.end() && previous->second.version != record.version)
            replaced = installDir(record.name, previous->second.version);
        tools.insert_or_assign(record.name, record);
        writeLock(tools);
        pending.erase(key(record.name, record.version));
        if (replaced)
            std::filesystem::remove_all(*replaced);
    }

    /// Uninstalls a tool. Returns false if it is not installed.
    bool remove(std::string_view name) {
        tools::validatePackageName(name);
        std::scoped_lock lock{mutex};
        auto             tools = readLock(lockFile());
        const auto       found = tools.find(name);
        if (found == tools.end())
            return false;
        const auto directory = installDir(name, found->second.version);
        tools.erase(found);
        writeLock(tools);
        std::filesystem::remove_all(directory);
        return true;
    }

    static tools::LockedTools readLock(const std::filesystem::path& file) {
        std::ifstream stream(file, std::ios::binary);
        if (!stream.is_open())
            return {};
        std::ostringstream content;
        content << stream.rdbuf();
        try {
            return tools::parseLock(content.view());
        } catch (const std::exception& error) {
            throw std::runtime_error("Malformed " + file.filename().string() + ": " + error.what());
        }
    }

private:
    std::filesystem::path project;
    std::mutex            mutex;
    std::set<std::string> pending;

    static std::string key(std::string_view name, std::string_view version) {
        return std::string(name) + "@" + std::string(version);
    }

    [[nodiscard]] std::filesystem::path stagingDir(std::string_view name, std::string_view version) const {
        return toolsDir() / ".staging" / tools::detail::fromUtf8(key(name, version));
    }

    void requirePending(std::string_view name, std::string_view version) const {
        if (!pending.contains(key(name, version)))
            throw std::invalid_argument("No installation of " + key(name, version) + " in progress");
    }

    // Written next to the lock file, then renamed over it, so a reader never sees a partial file.
    void writeLock(const tools::LockedTools& tools) const {
        const auto file      = lockFile();
        auto       temporary = file;
        temporary           += ".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            stream << tools::formatLock(tools);
            if (!stream.flush())
                throw std::runtime_error("Cannot write " + temporary.string());
        }
        std::filesystem::rename(temporary, file);
    }
};

}  // namespace webfront::fs
