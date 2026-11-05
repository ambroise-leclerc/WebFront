#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <webfront/system/MemoryStorage.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace std;
using namespace webfront;

namespace {

string readAll(fs::File file) {
    string         content;
    array<char, 7> buffer{};
    while (!file.eof()) {
        const auto count = file.read(buffer);
        content.append(buffer.data(), count);
        if (count == 0)
            break;
    }
    return content;
}

template <fs::Storage S>
void writeFile(S& storage, string_view path, string_view content, fs::WriteMode mode = fs::WriteMode::createNew) {
    auto writer = storage.beginWrite(path, mode);
    writer.write(span(content.data(), content.size()));
    writer.commit();
}

template <typename Operation>
fs::StorageErrc errorOf(Operation&& operation) {
    try {
        operation();
    } catch (const fs::StorageError& error) {
        return error.errc();
    }
    FAIL("The operation did not fail");
    return fs::StorageErrc::ioError;
}

template <fs::Storage S>
vector<string> allNames(S& storage, string_view directory, size_t pageSize) {
    vector<string> names;
    string         after;
    while (true) {
        auto page = storage.list(directory, after, pageSize);
        for (auto& entry : page.entries)
            names.push_back(entry.name);
        if (!page.next)
            return names;
        after = *page.next;
    }
}

}  // namespace

SCENARIO("SafePath refuses the names Windows reserves, on every platform") {
    GIVEN("a reserved device name, whatever its case and extension, or a name Windows would alter") {
        auto name = GENERATE(as<string>{},
                             "con",
                             "CON",
                             "Con.txt",
                             "nul",
                             "nul.tar.gz",
                             "aux",
                             "prn",
                             "com1",
                             "COM9.log",
                             "lpt1",
                             "LPT9",
                             "com\xC2\xB9",
                             "conin$",
                             "CONOUT$",
                             "con .txt",
                             "a.",
                             "a ",
                             "dir/nul/x");
        THEN("safeRelativePath, safePathComponents and isSafeFileName reject it") {
            REQUIRE_THROWS_AS(fs::safeRelativePath(name), invalid_argument);
            REQUIRE_THROWS_AS(fs::safePathComponents(name), invalid_argument);
            REQUIRE_FALSE(fs::isSafeFileName(name));
        }
    }
    GIVEN("names that only resemble them") {
        auto name = GENERATE(as<string>{}, "console", "com10", "lpt0", "com", "nullable.txt", "a.b", "auxiliary", ".hidden");
        THEN("they are accepted") {
            REQUIRE(fs::isSafeFileName(name));
            REQUIRE(fs::safePathComponents("dir/" + name) == vector<string>{"dir", name});
        }
    }
    GIVEN("the empty path") {
        THEN("it names the root of a storage") {
            REQUIRE(fs::safePathComponents("").empty());
        }
    }
}

SCENARIO("A storage error is a bridge error with a storage code") {
    const fs::StorageError error(fs::StorageErrc::rootMoved, "moved");
    const BridgeError&     bridge = error;
    REQUIRE(bridge.code() == "storage.rootMoved");
    REQUIRE(string(bridge.what()) == "moved");
    REQUIRE(fs::storageErrorCode(fs::StorageErrc::confinementViolated) == "storage.confinementViolated");
    for (auto errc : {fs::StorageErrc::notFound,
                      fs::StorageErrc::alreadyExists,
                      fs::StorageErrc::notDirectory,
                      fs::StorageErrc::isDirectory,
                      fs::StorageErrc::notEmpty,
                      fs::StorageErrc::accessDenied,
                      fs::StorageErrc::invalidPath,
                      fs::StorageErrc::linkRefused,
                      fs::StorageErrc::unsafeDirectory,
                      fs::StorageErrc::rootMoved,
                      fs::StorageErrc::confinementViolated,
                      fs::StorageErrc::busy,
                      fs::StorageErrc::ioError})
        REQUIRE(BridgeError::isValidCode(fs::storageErrorCode(errc)));
}

SCENARIO("A file reads its bytes through a provider's byte source") {
    struct Source : fs::ByteSource {
        string bytes{"0123456789"};
        size_t position{};
        size_t read(span<char> buffer) override {
            const auto count = min(buffer.size(), bytes.size() - position);
            copy_n(bytes.data() + position, count, buffer.data());
            position += count;
            return count;
        }
        [[nodiscard]] optional<uint64_t> size() const override {
            return bytes.size();
        }
        [[nodiscard]] bool seek(uint64_t offset) override {
            position = static_cast<size_t>(min<uint64_t>(offset, bytes.size()));
            return true;
        }
    };
    fs::File file{make_unique<Source>()};
    REQUIRE(file.knownSize() == 10u);
    REQUIRE(readAll(std::move(file)) == "0123456789");

    fs::File again{make_unique<Source>()};
    REQUIRE(again.seek(7));
    array<char, 16> buffer{};
    REQUIRE(again.read(buffer) == 3);
    REQUIRE(again.eof());
    REQUIRE(again.seek(2));
    REQUIRE_FALSE(again.eof());
    REQUIRE_THROWS_AS(fs::File{unique_ptr<fs::ByteSource>{}}, invalid_argument);
}

SCENARIO("The simulated storage follows the storage contract") {
    fs::MemoryStorage storage;

    GIVEN("files and directories created through it") {
        storage.createDirectory("docs");
        writeFile(storage, "docs/a.txt", "alpha");
        writeFile(storage, "b.txt", "beta");

        THEN("they are listed, inspected and read") {
            auto page = storage.list("", {}, 10);
            REQUIRE(page.entries.size() == 2);
            REQUIRE(page.entries[0].name == "b.txt");
            REQUIRE(page.entries[0].type == fs::EntryType::file);
            REQUIRE(page.entries[0].size == 4);
            REQUIRE(page.entries[1].name == "docs");
            REQUIRE(page.entries[1].type == fs::EntryType::directory);
            REQUIRE_FALSE(page.next);
            REQUIRE(storage.status("docs/a.txt").size == 5);
            REQUIRE(storage.status("").type == fs::EntryType::directory);
            REQUIRE(readAll(storage.openRead("docs/a.txt")) == "alpha");
        }
        THEN("a write creating a name that exists is refused, a replacing one replaces it") {
            REQUIRE(errorOf([&] {
                        writeFile(storage, "b.txt", "other");
                    })
                    == fs::StorageErrc::alreadyExists);
            writeFile(storage, "b.txt", "gamma", fs::WriteMode::replace);
            REQUIRE(readAll(storage.openRead("b.txt")) == "gamma");
            REQUIRE(errorOf([&] {
                        writeFile(storage, "docs", "x", fs::WriteMode::replace);
                    })
                    == fs::StorageErrc::isDirectory);
        }
        THEN("an opened file keeps its bytes when the file is replaced") {
            auto file = storage.openRead("b.txt");
            writeFile(storage, "b.txt", "replaced", fs::WriteMode::replace);
            REQUIRE(readAll(std::move(file)) == "beta");
        }
        THEN("removing and renaming follow the contract") {
            REQUIRE(errorOf([&] {
                        storage.remove("docs");
                    })
                    == fs::StorageErrc::notEmpty);
            REQUIRE(errorOf([&] {
                        storage.rename("docs", "docs/inner");
                    })
                    == fs::StorageErrc::invalidPath);
            REQUIRE(errorOf([&] {
                        storage.rename("b.txt", "docs/a.txt");
                    })
                    == fs::StorageErrc::alreadyExists);
            storage.rename("b.txt", "docs/b.txt");
            REQUIRE(errorOf([&] {
                        storage.status("b.txt");
                    })
                    == fs::StorageErrc::notFound);
            storage.remove("docs/a.txt");
            storage.remove("docs/b.txt");
            storage.remove("docs");
            REQUIRE(storage.list("").entries.empty());
        }
        THEN("operations through a file or a missing directory fail with their reason") {
            REQUIRE(errorOf([&] {
                        storage.list("b.txt");
                    })
                    == fs::StorageErrc::notDirectory);
            REQUIRE(errorOf([&] {
                        storage.openRead("missing/x");
                    })
                    == fs::StorageErrc::notFound);
            REQUIRE(errorOf([&] {
                        storage.openRead("docs");
                    })
                    == fs::StorageErrc::isDirectory);
            REQUIRE(errorOf([&] {
                        storage.createDirectory("docs");
                    })
                    == fs::StorageErrc::alreadyExists);
        }
    }

    GIVEN("a write in progress") {
        auto writer = make_unique<fs::MemoryStorage::Writer>(storage.beginWrite("draft.txt", fs::WriteMode::createNew));
        writer->write(span<const char>("partial", 7));
        THEN("nothing is visible before the commit") {
            REQUIRE(errorOf([&] {
                        storage.status("draft.txt");
                    })
                    == fs::StorageErrc::notFound);
        }
        WHEN("the writer is destroyed without committing, as an interrupted write") {
            writer.reset();
            THEN("no partial file exists") {
                REQUIRE(storage.list("").entries.empty());
            }
        }
        WHEN("it commits") {
            writer->commit();
            THEN("the whole file appears at once") {
                REQUIRE(readAll(storage.openRead("draft.txt")) == "partial");
                REQUIRE_THROWS_AS(writer->commit(), logic_error);
            }
        }
    }

    GIVEN("rights refused by the simulated system") {
        storage.createDirectory("locked");
        writeFile(storage, "locked/secret.txt", "s");
        writeFile(storage, "readonly.txt", "r");
        storage.setAccess("locked", false, false);
        storage.setAccess("readonly.txt", true, false);
        THEN("operations needing them fail with accessDenied") {
            REQUIRE(errorOf([&] {
                        storage.list("locked");
                    })
                    == fs::StorageErrc::accessDenied);
            REQUIRE(errorOf([&] {
                        storage.openRead("locked/secret.txt");
                    })
                    == fs::StorageErrc::accessDenied);
            REQUIRE(errorOf([&] {
                        writeFile(storage, "locked/new.txt", "n");
                    })
                    == fs::StorageErrc::accessDenied);
            REQUIRE(errorOf([&] {
                        writeFile(storage, "readonly.txt", "w", fs::WriteMode::replace);
                    })
                    == fs::StorageErrc::accessDenied);
            REQUIRE(readAll(storage.openRead("readonly.txt")) == "r");
            auto info = storage.status("readonly.txt");
            REQUIRE(info.readable);
            REQUIRE_FALSE(info.writable);
        }
    }

    GIVEN("paths that leave the root or that SafePath rejects") {
        auto path = GENERATE(as<string>{}, "../x", "/etc/passwd", "a/../b", "a//b", "C:/x", "a\\b", "nul.txt", "a/./b", "..");
        THEN("every operation refuses them with invalidPath") {
            REQUIRE(errorOf([&] {
                        storage.status(path);
                    })
                    == fs::StorageErrc::invalidPath);
            REQUIRE(errorOf([&] {
                        storage.list(path);
                    })
                    == fs::StorageErrc::invalidPath);
            REQUIRE(errorOf([&] {
                        storage.openRead(path);
                    })
                    == fs::StorageErrc::invalidPath);
            REQUIRE(errorOf([&] {
                        (void)storage.beginWrite(path, fs::WriteMode::replace);
                    })
                    == fs::StorageErrc::invalidPath);
            REQUIRE(errorOf([&] {
                        storage.createDirectory(path);
                    })
                    == fs::StorageErrc::invalidPath);
            REQUIRE(errorOf([&] {
                        storage.remove(path);
                    })
                    == fs::StorageErrc::invalidPath);
            REQUIRE(errorOf([&] {
                        storage.rename("x", path);
                    })
                    == fs::StorageErrc::invalidPath);
        }
    }
    GIVEN("operations that need an entry") {
        THEN("the root is refused") {
            REQUIRE(errorOf([&] {
                        storage.remove("");
                    })
                    == fs::StorageErrc::invalidPath);
            REQUIRE(errorOf([&] {
                        storage.openRead("");
                    })
                    == fs::StorageErrc::invalidPath);
        }
    }
}

SCENARIO("Listing pages are stable while the directory changes") {
    fs::MemoryStorage storage;
    for (int index = 10; index < 40; ++index)
        writeFile(storage, "f" + to_string(index), "x");

    GIVEN("a listing read page by page") {
        auto first = storage.list("", {}, 10);
        REQUIRE(first.entries.size() == 10);
        REQUIRE(first.entries.front().name == "f10");
        REQUIRE(first.next == "f19");

        WHEN("entries are added before and after the cursor and removed after it") {
            writeFile(storage, "f05", "x");
            writeFile(storage, "f195", "x");
            writeFile(storage, "f99", "x");
            storage.remove("f30");
            set<string> seen;
            for (auto& entry : first.entries)
                seen.insert(entry.name);
            string after = *first.next;
            while (true) {
                auto page = storage.list("", after, 10);
                for (auto& entry : page.entries)
                    REQUIRE(seen.insert(entry.name).second);
                if (!page.next)
                    break;
                after = *page.next;
            }
            THEN("every entry present during the whole listing appears exactly once") {
                for (int index = 10; index < 40; ++index)
                    if (index != 30)
                        REQUIRE(seen.contains("f" + to_string(index)));
                REQUIRE(seen.contains("f195"));
                REQUIRE(seen.contains("f99"));
                REQUIRE_FALSE(seen.contains("f05"));
            }
        }
    }
    GIVEN("a page size out of bounds") {
        THEN("it is brought back into [1, maxStoragePageSize]") {
            REQUIRE(storage.list("", {}, 0).entries.size() == 1);
            REQUIRE(storage.list("", {}, 1'000'000).entries.size() == 30);
            REQUIRE(allNames(storage, "", 7).size() == 30);
        }
    }
    GIVEN("an invalid cursor") {
        THEN("the listing is refused") {
            REQUIRE(errorOf([&] {
                        storage.list("", "../f10");
                    })
                    == fs::StorageErrc::invalidPath);
        }
    }
}
