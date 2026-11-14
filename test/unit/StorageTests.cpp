#include "Bdd.hpp"
#include <webfront/system/MemoryStorage.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace webfront::bdd;

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
    fail("The operation did not fail");
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

Scenario safePathRefuses{"SafePath refuses the names Windows reserves, on every platform", [] {
    given("a reserved device name, whatever its case and extension, or a name Windows would alter", [&] {
        then("safeRelativePath, safePathComponents and isSafeFileName reject it", [&] {
            for (string name : {"con", "CON", "Con.txt", "nul", "nul.tar.gz", "aux", "prn", "com1", "COM9.log",
                                "lpt1", "LPT9", "com\xC2\xB9", "conin$", "CONOUT$", "con .txt", "a.", "a ", "dir/nul/x"}) {
                auto scope = info(name);
                requireThrows<invalid_argument>([&] { fs::safeRelativePath(name); });
                requireThrows<invalid_argument>([&] { fs::safePathComponents(name); });
                require(!fs::isSafeFileName(name));
            }
        });
    });
    given("names that only resemble them", [&] {
        then("they are accepted", [&] {
            for (string name : {"console", "com10", "lpt0", "com", "nullable.txt", "a.b", "auxiliary", ".hidden"}) {
                auto scope = info(name);
                require(fs::isSafeFileName(name));
                require(fs::safePathComponents("dir/" + name) == vector<string>{"dir", name});
            }
        });
    });
    given("the empty path", [&] {
        then("it names the root of a storage", [&] {
            require(fs::safePathComponents("").empty());
        });
    });
}};

Scenario storageError{"A storage error is a bridge error with a storage code", [] {
    const fs::StorageError error(fs::StorageErrc::rootMoved, "moved");
    const BridgeError&     bridge = error;
    require(bridge.code() == "storage.rootMoved");
    require(string(bridge.what()) == "moved");
    require(fs::storageErrorCode(fs::StorageErrc::confinementViolated) == "storage.confinementViolated");
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
        require(BridgeError::isValidCode(fs::storageErrorCode(errc)));
}};

Scenario fileReads{"A file reads its bytes through a provider's byte source", [] {
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
    require(file.knownSize() == 10u);
    require(readAll(std::move(file)) == "0123456789");

    fs::File again{make_unique<Source>()};
    require(again.seek(7));
    array<char, 16> buffer{};
    require(again.read(buffer) == 3);
    require(again.eof());
    require(again.seek(2));
    require(!again.eof());
    requireThrows<invalid_argument>([&] { fs::File{unique_ptr<fs::ByteSource>{}}; });
}};

Scenario simulatedStorage{"The simulated storage follows the storage contract", [] {
    fs::MemoryStorage storage;

    given("files and directories created through it", [&] {
        storage.createDirectory("docs");
        writeFile(storage, "docs/a.txt", "alpha");
        writeFile(storage, "b.txt", "beta");

        then("they are listed, inspected and read", [&] {
            auto page = storage.list("", {}, 10);
            require(page.entries.size() == 2);
            require(page.entries[0].name == "b.txt");
            require(page.entries[0].type == fs::EntryType::file);
            require(page.entries[0].size == 4);
            require(page.entries[1].name == "docs");
            require(page.entries[1].type == fs::EntryType::directory);
            require(!page.next);
            require(storage.status("docs/a.txt").size == 5);
            require(storage.status("").type == fs::EntryType::directory);
            require(readAll(storage.openRead("docs/a.txt")) == "alpha");
        });
        then("a write creating a name that exists is refused, a replacing one replaces it", [&] {
            require(errorOf([&] {
                        writeFile(storage, "b.txt", "other");
                    })
                    == fs::StorageErrc::alreadyExists);
            writeFile(storage, "b.txt", "gamma", fs::WriteMode::replace);
            require(readAll(storage.openRead("b.txt")) == "gamma");
            require(errorOf([&] {
                        writeFile(storage, "docs", "x", fs::WriteMode::replace);
                    })
                    == fs::StorageErrc::isDirectory);
        });
        then("an opened file keeps its bytes when the file is replaced", [&] {
            auto file = storage.openRead("b.txt");
            writeFile(storage, "b.txt", "replaced", fs::WriteMode::replace);
            require(readAll(std::move(file)) == "beta");
        });
        then("removing and renaming follow the contract", [&] {
            require(errorOf([&] {
                        storage.remove("docs");
                    })
                    == fs::StorageErrc::notEmpty);
            require(errorOf([&] {
                        storage.rename("docs", "docs/inner");
                    })
                    == fs::StorageErrc::invalidPath);
            require(errorOf([&] {
                        storage.rename("b.txt", "docs/a.txt");
                    })
                    == fs::StorageErrc::alreadyExists);
            storage.rename("b.txt", "docs/b.txt");
            require(errorOf([&] {
                        storage.status("b.txt");
                    })
                    == fs::StorageErrc::notFound);
            storage.remove("docs/a.txt");
            storage.remove("docs/b.txt");
            storage.remove("docs");
            require(storage.list("").entries.empty());
        });
        then("operations through a file or a missing directory fail with their reason", [&] {
            require(errorOf([&] {
                        storage.list("b.txt");
                    })
                    == fs::StorageErrc::notDirectory);
            require(errorOf([&] {
                        storage.openRead("missing/x");
                    })
                    == fs::StorageErrc::notFound);
            require(errorOf([&] {
                        storage.openRead("docs");
                    })
                    == fs::StorageErrc::isDirectory);
            require(errorOf([&] {
                        storage.createDirectory("docs");
                    })
                    == fs::StorageErrc::alreadyExists);
        });
    });

    given("a write in progress", [&] {
        auto writer = make_unique<fs::MemoryStorage::Writer>(storage.beginWrite("draft.txt", fs::WriteMode::createNew));
        writer->write(span<const char>("partial", 7));
        then("nothing is visible before the commit", [&] {
            require(errorOf([&] {
                        storage.status("draft.txt");
                    })
                    == fs::StorageErrc::notFound);
        });
        when("the writer is destroyed without committing, as an interrupted write", [&] {
            writer.reset();
            then("no partial file exists", [&] {
                require(storage.list("").entries.empty());
            });
        });
        when("it commits", [&] {
            writer->commit();
            then("the whole file appears at once", [&] {
                require(readAll(storage.openRead("draft.txt")) == "partial");
                requireThrows<logic_error>([&] { writer->commit(); });
            });
        });
    });

    given("rights refused by the simulated system", [&] {
        storage.createDirectory("locked");
        writeFile(storage, "locked/secret.txt", "s");
        writeFile(storage, "readonly.txt", "r");
        storage.setAccess("locked", false, false);
        storage.setAccess("readonly.txt", true, false);
        then("operations needing them fail with accessDenied", [&] {
            require(errorOf([&] {
                        storage.list("locked");
                    })
                    == fs::StorageErrc::accessDenied);
            require(errorOf([&] {
                        storage.openRead("locked/secret.txt");
                    })
                    == fs::StorageErrc::accessDenied);
            require(errorOf([&] {
                        writeFile(storage, "locked/new.txt", "n");
                    })
                    == fs::StorageErrc::accessDenied);
            require(errorOf([&] {
                        writeFile(storage, "readonly.txt", "w", fs::WriteMode::replace);
                    })
                    == fs::StorageErrc::accessDenied);
            require(readAll(storage.openRead("readonly.txt")) == "r");
            auto info = storage.status("readonly.txt");
            require(info.readable);
            require(!info.writable);
        });
    });

    given("paths that leave the root or that SafePath rejects", [&] {
        then("every operation refuses them with invalidPath", [&] {
            for (string path : {"../x", "/etc/passwd", "a/../b", "a//b", "C:/x", "a\\b", "nul.txt", "a/./b", ".."}) {
                auto scope = info(path);
                require(errorOf([&] {
                            storage.status(path);
                        })
                        == fs::StorageErrc::invalidPath);
                require(errorOf([&] {
                            storage.list(path);
                        })
                        == fs::StorageErrc::invalidPath);
                require(errorOf([&] {
                            storage.openRead(path);
                        })
                        == fs::StorageErrc::invalidPath);
                require(errorOf([&] {
                            (void)storage.beginWrite(path, fs::WriteMode::replace);
                        })
                        == fs::StorageErrc::invalidPath);
                require(errorOf([&] {
                            storage.createDirectory(path);
                        })
                        == fs::StorageErrc::invalidPath);
                require(errorOf([&] {
                            storage.remove(path);
                        })
                        == fs::StorageErrc::invalidPath);
                require(errorOf([&] {
                            storage.rename("x", path);
                        })
                        == fs::StorageErrc::invalidPath);
            }
        });
    });
    given("names starting with the prefix reserved to temporary files", [&] {
        then("they are refused, so that a listing never hides a committed file", [&] {
            require(errorOf([&] {
                        writeFile(storage, ".wf-tmp-report.txt", "report");
                    })
                    == fs::StorageErrc::invalidPath);
            require(errorOf([&] {
                        storage.createDirectory("dir/.wf-tmp-x");
                    })
                    == fs::StorageErrc::invalidPath);
            require(errorOf([&] {
                        storage.status(".wf-tmp-report.txt");
                    })
                    == fs::StorageErrc::invalidPath);
            require(fs::isSafeFileName(".wf-tmp-report.txt"));  // SafePath alone accepts it: the reservation is the storage's
            for (auto name : {".WF-TMP-report.txt", ".Wf-Tmp-x", ".wF-tMp-"})
                require(errorOf([&] {
                            storage.status(name);
                        })
                        == fs::StorageErrc::invalidPath);
            require(fs::isTemporaryName(".WF-tmp-a"));
            require(!fs::isTemporaryName(".wf-tmp"));
            require(!fs::isTemporaryName("x.wf-tmp-a"));
        });
    });
    given("operations that need an entry", [&] {
        then("the root is refused", [&] {
            require(errorOf([&] {
                        storage.remove("");
                    })
                    == fs::StorageErrc::invalidPath);
            require(errorOf([&] {
                        storage.openRead("");
                    })
                    == fs::StorageErrc::invalidPath);
        });
    });
}};

Scenario listingPages{"Listing pages are stable while the directory changes", [] {
    fs::MemoryStorage storage;
    for (int index = 10; index < 40; ++index)
        writeFile(storage, "f" + to_string(index), "x");

    given("a listing read page by page", [&] {
        auto first = storage.list("", {}, 10);
        require(first.entries.size() == 10);
        require(first.entries.front().name == "f10");
        require(first.next == "f19");

        when("entries are added before and after the cursor and removed after it", [&] {
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
                    require(seen.insert(entry.name).second);
                if (!page.next)
                    break;
                after = *page.next;
            }
            then("every entry present during the whole listing appears exactly once", [&] {
                for (int index = 10; index < 40; ++index)
                    if (index != 30)
                        require(seen.contains("f" + to_string(index)));
                require(seen.contains("f195"));
                require(seen.contains("f99"));
                require(!seen.contains("f05"));
            });
        });
    });
    given("a page size out of bounds", [&] {
        then("it is brought back into [1, maxStoragePageSize]", [&] {
            require(storage.list("", {}, 0).entries.size() == 1);
            require(storage.list("", {}, 1'000'000).entries.size() == 30);
            require(allNames(storage, "", 7).size() == 30);
        });
    });
    given("an invalid cursor", [&] {
        then("the listing is refused", [&] {
            require(errorOf([&] {
                        storage.list("", "../f10");
                    })
                    == fs::StorageErrc::invalidPath);
        });
    });
}};
