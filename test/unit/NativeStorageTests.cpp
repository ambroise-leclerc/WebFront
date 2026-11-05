#include "TemporaryDirectory.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <webfront/system/NativeStorage.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
    #include <pwd.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif

using namespace std;
using namespace webfront;
namespace stdfs = std::filesystem;

namespace {

string readAll(fs::File file) {
    string          content;
    array<char, 64> buffer{};
    while (!file.eof()) {
        const auto count = file.read(buffer);
        content.append(buffer.data(), count);
        if (count == 0)
            break;
    }
    return content;
}

void writeFile(fs::NativeStorage& storage, string_view path, string_view content, fs::WriteMode mode = fs::WriteMode::createNew) {
    auto writer = storage.beginWrite(path, mode);
    writer.write(span(content.data(), content.size()));
    writer.commit();
}

void writeDisk(const stdfs::path& path, string_view content) {
    ofstream(path, ios::binary) << content;
}

string readDisk(const stdfs::path& path) {
    ifstream file(path, ios::binary);
    return {istreambuf_iterator<char>(file), {}};
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

// The names of a directory on disk, temporary files included.
vector<string> namesOn(const stdfs::path& directory) {
    vector<string> names;
    for (const auto& entry : stdfs::directory_iterator(directory))
        names.push_back(entry.path().filename().string());
    ranges::sort(names);
    return names;
}

// A link at `link` to the directory `target`: a symbolic link on POSIX, a junction on Windows (which needs no privilege).
void linkDirectory(const stdfs::path& target, const stdfs::path& link) {
#ifdef _WIN32
    const auto command = "cmd /c mklink /J \"" + link.string() + "\" \"" + target.string() + "\" >NUL";
    REQUIRE(std::system(command.c_str()) == 0);
#else
    stdfs::create_directory_symlink(target, link);
#endif
}

// A root and, beside it, a directory outside the root that no operation may reach.
struct Sandbox {
    test::TemporaryDirectory base;
    stdfs::path              root    = base.path() / "root";
    stdfs::path              outside = base.path() / "outside";

    Sandbox() {
        stdfs::create_directory(root);
        stdfs::create_directory(outside);
        writeDisk(outside / "secret.txt", "secret");
#ifndef _WIN32
        // The tests' ownership checks need directories nobody else can write in, whatever the umask.
        for (const auto& directory : {base.path(), root, outside})
            stdfs::permissions(directory, stdfs::perms::group_write | stdfs::perms::others_write, stdfs::perm_options::remove);
#endif
    }

    [[nodiscard]] bool outsideUntouched() const {
        return namesOn(outside) == vector<string>{"secret.txt"} && readDisk(outside / "secret.txt") == "secret";
    }
};

shared_ptr<fs::StorageHooks> hooksAt(function<void(fs::StoragePoint, string_view)> at) {
    auto hooks = make_shared<fs::StorageHooks>();
    hooks->at  = std::move(at);
    return hooks;
}

}  // namespace

SCENARIO("A native storage reads and writes under its root") {
    Sandbox           sandbox;
    fs::NativeStorage storage(sandbox.root);

    THEN("its location is the canonical root") {
        REQUIRE(storage.location() == stdfs::canonical(sandbox.root));
    }
    GIVEN("entries created through it") {
        storage.createDirectory("docs");
        writeFile(storage, "docs/a.txt", "alpha");
        writeFile(storage, "b.txt", "beta");

        THEN("they are on disk, listed, inspected and read") {
            REQUIRE(readDisk(sandbox.root / "docs" / "a.txt") == "alpha");
            auto page = storage.list("");
            REQUIRE(page.entries.size() == 2);
            REQUIRE(page.entries[0].name == "b.txt");
            REQUIRE(page.entries[0].type == fs::EntryType::file);
            REQUIRE(page.entries[0].size == 4);
            REQUIRE(page.entries[0].readable);
            REQUIRE(page.entries[0].writable);
            REQUIRE(page.entries[1].type == fs::EntryType::directory);
            REQUIRE(storage.status("docs/a.txt").size == 5);
            REQUIRE(storage.status("").type == fs::EntryType::directory);
            auto file = storage.openRead("docs/a.txt");
            REQUIRE(file.knownSize() == 5u);
            REQUIRE(readAll(std::move(file)) == "alpha");
        }
        THEN("a file is read from any position") {
            auto            file = storage.openRead("docs/a.txt");
            array<char, 16> buffer{};
            REQUIRE(file.seek(3));
            REQUIRE(file.read(buffer) == 2);
            REQUIRE(string_view(buffer.data(), 2) == "ha");
        }
        THEN("a write replaces atomically only when asked to") {
            REQUIRE(errorOf([&] {
                        writeFile(storage, "b.txt", "other");
                    })
                    == fs::StorageErrc::alreadyExists);
            writeFile(storage, "b.txt", "gamma", fs::WriteMode::replace);
            REQUIRE(readDisk(sandbox.root / "b.txt") == "gamma");
            REQUIRE(errorOf([&] {
                        writeFile(storage, "docs", "x", fs::WriteMode::replace);
                    })
                    == fs::StorageErrc::isDirectory);
        }
        THEN("entries are renamed without replacing and removed") {
            REQUIRE(errorOf([&] {
                        storage.rename("b.txt", "docs/a.txt");
                    })
                    == fs::StorageErrc::alreadyExists);
            storage.rename("b.txt", "docs/b.txt");
            REQUIRE(stdfs::exists(sandbox.root / "docs" / "b.txt"));
            REQUIRE(errorOf([&] {
                        storage.remove("docs");
                    })
                    == fs::StorageErrc::notEmpty);
            storage.remove("docs/a.txt");
            storage.remove("docs/b.txt");
            storage.remove("docs");
            REQUIRE(namesOn(sandbox.root).empty());
        }
        THEN("operations through a file or a missing entry fail with their reason") {
            REQUIRE(errorOf([&] {
                        storage.list("b.txt");
                    })
                    == fs::StorageErrc::notDirectory);
            REQUIRE(errorOf([&] {
                        storage.openRead("b.txt/x");
                    })
                    == fs::StorageErrc::notDirectory);
            REQUIRE(errorOf([&] {
                        storage.openRead("missing.txt");
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
    GIVEN("paths SafePath rejects") {
        auto path = GENERATE(as<string>{}, "../outside/secret.txt", "/etc/passwd", "a/../../outside", "C:/x", "nul", "a\\..\\b");
        THEN("they are refused before touching the disk") {
            REQUIRE(errorOf([&] {
                        storage.openRead(path);
                    })
                    == fs::StorageErrc::invalidPath);
            REQUIRE(errorOf([&] {
                        storage.createDirectory(path);
                    })
                    == fs::StorageErrc::invalidPath);
            REQUIRE(sandbox.outsideUntouched());
        }
    }
    GIVEN("a location that is not a directory") {
        writeDisk(sandbox.base.path() / "file", "x");
        THEN("it cannot be declared") {
            REQUIRE(errorOf([&] {
                        fs::NativeStorage{sandbox.base.path() / "missing"};
                    })
                    == fs::StorageErrc::notFound);
            REQUIRE(errorOf([&] {
                        fs::NativeStorage{sandbox.base.path() / "file"};
                    })
                    == fs::StorageErrc::notDirectory);
        }
    }
}

SCENARIO("A native storage reserves the names of its temporary files") {
    Sandbox           sandbox;
    fs::NativeStorage storage(sandbox.root);
    REQUIRE(errorOf([&] {
                writeFile(storage, ".wf-tmp-report.txt", "report");
            })
            == fs::StorageErrc::invalidPath);
    REQUIRE(errorOf([&] {
                storage.rename("a", ".wf-tmp-a");
            })
            == fs::StorageErrc::invalidPath);
    REQUIRE(namesOn(sandbox.root).empty());
}

SCENARIO("An interrupted write leaves no partial file") {
    Sandbox           sandbox;
    fs::NativeStorage storage(sandbox.root);
    writeFile(storage, "kept.txt", "original");

    GIVEN("writes destroyed before their commit, new or replacing") {
        {
            auto writer = storage.beginWrite("new.txt", fs::WriteMode::createNew);
            writer.write(span<const char>("partial", 7));
            REQUIRE(namesOn(sandbox.root).size() == 2);  // kept.txt and the temporary file
            REQUIRE(storage.list("").entries.size() == 1);
            REQUIRE(errorOf([&] {
                        storage.status("new.txt");
                    })
                    == fs::StorageErrc::notFound);
        }
        {
            auto writer = storage.beginWrite("kept.txt", fs::WriteMode::replace);
            writer.write(span<const char>("half", 4));
        }
        THEN("neither the partial file nor the temporary one remains, and the replaced file is intact") {
            REQUIRE(namesOn(sandbox.root) == vector<string>{"kept.txt"});
            REQUIRE(readDisk(sandbox.root / "kept.txt") == "original");
        }
    }
}

SCENARIO("Native listings are paginated and stable") {
    Sandbox sandbox;
    for (int index = 10; index < 40; ++index)
        writeDisk(sandbox.root / ("f" + to_string(index)), "x");
#ifndef _WIN32
    writeDisk(sandbox.root / "trailing.", "unaddressable");  // a name SafePath rejects is not listed
#endif
    fs::NativeStorage storage(sandbox.root);

    auto first = storage.list("", {}, 10);
    REQUIRE(first.entries.size() == 10);
    REQUIRE(first.next == "f19");
    writeDisk(sandbox.root / "f05", "x");
    writeDisk(sandbox.root / "f195", "x");
    stdfs::remove(sandbox.root / "f30");
    vector<string> seen;
    for (auto& entry : first.entries)
        seen.push_back(entry.name);
    for (auto after = first.next; after;) {
        auto page = storage.list("", *after, 10);
        for (auto& entry : page.entries)
            seen.push_back(entry.name);
        after = page.next;
    }
    vector<string> expected;
    for (int index = 10; index < 40; ++index)
        if (index != 30)
            expected.push_back("f" + to_string(index));
    expected.push_back("f195");
    ranges::sort(expected);
    REQUIRE(seen == expected);
}

SCENARIO("Links never lead a native storage outside its root") {
    Sandbox sandbox;
    stdfs::create_directory(sandbox.root / "inside");
    writeDisk(sandbox.root / "inside" / "note.txt", "note");
    linkDirectory(sandbox.outside, sandbox.root / "escape");

    GIVEN("a root that refuses links (the default)") {
        fs::NativeStorage storage(sandbox.root);
        THEN("every operation through the link is refused and the outside is untouched") {
            REQUIRE(errorOf([&] {
                        storage.list("escape");
                    })
                    == fs::StorageErrc::linkRefused);
            REQUIRE(errorOf([&] {
                        storage.openRead("escape/secret.txt");
                    })
                    == fs::StorageErrc::linkRefused);
            REQUIRE(errorOf([&] {
                        writeFile(storage, "escape/new.txt", "x");
                    })
                    == fs::StorageErrc::linkRefused);
            REQUIRE(errorOf([&] {
                        writeFile(storage, "escape/secret.txt", "x", fs::WriteMode::replace);
                    })
                    == fs::StorageErrc::linkRefused);
            REQUIRE(errorOf([&] {
                        storage.createDirectory("escape/dir");
                    })
                    == fs::StorageErrc::linkRefused);
            REQUIRE(errorOf([&] {
                        storage.remove("escape/secret.txt");
                    })
                    == fs::StorageErrc::linkRefused);
            REQUIRE(errorOf([&] {
                        storage.rename("escape/secret.txt", "stolen.txt");
                    })
                    == fs::StorageErrc::linkRefused);
            REQUIRE(errorOf([&] {
                        storage.rename("inside/note.txt", "escape/note.txt");
                    })
                    == fs::StorageErrc::linkRefused);
            REQUIRE(sandbox.outsideUntouched());
        }
        THEN("the link is listed as a link, not followed") {
            auto page = storage.list("");
            REQUIRE(page.entries.size() == 2);
            REQUIRE(page.entries[0].name == "escape");
            REQUIRE(page.entries[0].type == fs::EntryType::link);
            REQUIRE(storage.status("escape").type == fs::EntryType::link);
        }
        THEN("removing the link removes the link only") {
            storage.remove("escape");
            REQUIRE_FALSE(stdfs::exists(stdfs::symlink_status(sandbox.root / "escape")));
            REQUIRE(sandbox.outsideUntouched());
        }
    }
#ifndef _WIN32
    GIVEN("a file link to the outside") {
        stdfs::create_symlink(sandbox.outside / "secret.txt", sandbox.root / "secret-link");
        fs::NativeStorage storage(sandbox.root);
        THEN("it is not read, and replacing it replaces the link, not its target") {
            REQUIRE(errorOf([&] {
                        storage.openRead("secret-link");
                    })
                    == fs::StorageErrc::linkRefused);
            writeFile(storage, "secret-link", "replaced", fs::WriteMode::replace);
            REQUIRE(stdfs::is_regular_file(stdfs::symlink_status(sandbox.root / "secret-link")));
            REQUIRE(sandbox.outsideUntouched());
        }
    }
    GIVEN("a root that follows links inside it") {
        stdfs::create_directory_symlink("inside", sandbox.root / "alias");
        stdfs::create_symlink("../inside/note.txt", sandbox.root / "inside" / "self");
        stdfs::create_directory_symlink("../../outside", sandbox.root / "inside" / "up");
        stdfs::create_symlink(sandbox.outside / "secret.txt", sandbox.root / "absolute");
        stdfs::create_symlink("loop", sandbox.root / "loop");
        stdfs::create_symlink("..", sandbox.root / "parent");
        fs::NativeStorage storage(sandbox.root, {.links = fs::LinkPolicy::followInside});
        THEN("a link whose resolution stays inside is followed") {
            REQUIRE(readAll(storage.openRead("alias/note.txt")) == "note");
            REQUIRE(readAll(storage.openRead("inside/self")) == "note");
            writeFile(storage, "alias/added.txt", "added");
            REQUIRE(readDisk(sandbox.root / "inside" / "added.txt") == "added");
            REQUIRE(storage.list("alias").entries.size() == 4);  // note.txt, self, up, added.txt
        }
        THEN("a link that leaves the root, an absolute link and a loop are refused") {
            REQUIRE(errorOf([&] {
                        storage.list("escape");
                    })
                    == fs::StorageErrc::linkRefused);
            REQUIRE(errorOf([&] {
                        storage.openRead("inside/up/secret.txt");
                    })
                    == fs::StorageErrc::linkRefused);
            REQUIRE(errorOf([&] {
                        writeFile(storage, "inside/up/new.txt", "x");
                    })
                    == fs::StorageErrc::linkRefused);
            REQUIRE(errorOf([&] {
                        storage.openRead("absolute");
                    })
                    == fs::StorageErrc::linkRefused);
            REQUIRE(errorOf([&] {
                        storage.openRead("loop");
                    })
                    == fs::StorageErrc::linkRefused);
            REQUIRE(sandbox.outsideUntouched());
        }
    }
#endif
}

SCENARIO("A component replaced by a link during the resolution does not lead outside") {
    Sandbox sandbox;

    // Before each operation: root/a/b/file.txt. When `a` has been opened and `b` not yet, b is replaced by a link to
    // the outside directory, as a concurrent local process could do between two resolution steps.
    auto prepare = [&] {
        stdfs::remove_all(sandbox.root / "a");
        stdfs::create_directories(sandbox.root / "a" / "b");
        writeDisk(sandbox.root / "a" / "b" / "secret.txt", "inner");
    };
    auto              swapB = hooksAt([&](fs::StoragePoint point, string_view name) {
        if (point == fs::StoragePoint::componentOpened && name == "a") {
            stdfs::rename(sandbox.root / "a" / "b", sandbox.root / "b-moved");
            linkDirectory(sandbox.outside, sandbox.root / "a" / "b");
        }
    });
    fs::NativeStorage storage(sandbox.root, {}, swapB);

    const vector<function<void()>> operations{[&] {
                                                  storage.openRead("a/b/secret.txt");
                                              },
                                              [&] {
                                                  storage.list("a/b");
                                              },
                                              [&] {
                                                  writeFile(storage, "a/b/new.txt", "x");
                                              },
                                              [&] {
                                                  writeFile(storage, "a/b/secret.txt", "x", fs::WriteMode::replace);
                                              },
                                              [&] {
                                                  storage.createDirectory("a/b/dir");
                                              },
                                              [&] {
                                                  storage.remove("a/b/secret.txt");
                                              },
                                              [&] {
                                                  storage.rename("a/b/secret.txt", "stolen.txt");
                                              },
                                              [&] {
                                                  storage.status("a/b/secret.txt");
                                              }};
    const auto                     operation = operations[static_cast<size_t>(GENERATE(Catch::Generators::range(0, 8)))];
    prepare();
    stdfs::remove_all(sandbox.root / "b-moved");
    REQUIRE(errorOf(operation) == fs::StorageErrc::linkRefused);
    REQUIRE(sandbox.outsideUntouched());
}

SCENARIO("A final file replaced by a link after the resolution does not lead outside") {
    Sandbox sandbox;
    auto    swapFile = hooksAt([&](fs::StoragePoint point, string_view name) {
        if (point == fs::StoragePoint::resolved && name == "target.txt") {
            stdfs::remove(sandbox.root / "target.txt");
#ifdef _WIN32
            linkDirectory(sandbox.outside, sandbox.root / "target.txt");
#else
            stdfs::create_symlink(sandbox.outside / "secret.txt", sandbox.root / "target.txt");
#endif
        }
    });
    fs::NativeStorage storage(sandbox.root, {}, swapFile);
    writeDisk(sandbox.root / "target.txt", "inner");

    WHEN("it is read") {
        THEN("the link is refused") {
            REQUIRE(errorOf([&] {
                        storage.openRead("target.txt");
                    })
                    == fs::StorageErrc::linkRefused);
        }
    }
    WHEN("it is removed") {
        storage.remove("target.txt");
        THEN("the link goes, not the outside file") {
            REQUIRE_FALSE(stdfs::exists(stdfs::symlink_status(sandbox.root / "target.txt")));
        }
    }
    WHEN("it is inspected") {
        THEN("the link is reported, not followed") {
            REQUIRE(storage.status("target.txt").type == fs::EntryType::link);
        }
    }
    REQUIRE(sandbox.outsideUntouched());
}

#ifndef _WIN32
SCENARIO("Repeated races between a link swap and the operations never reach the outside") {
    Sandbox           sandbox;
    fs::NativeStorage storage(sandbox.root);
    stdfs::create_directory(sandbox.root / "a");
    atomic<bool> stop{false};
    // Swaps `a` between a real directory and a link to the outside, as fast as it can.
    thread swapper([&] {
        const auto directory = sandbox.root / "a";
        const auto parked    = sandbox.root / "parked";
        const auto link      = sandbox.base.path() / "link";
        while (!stop) {
            std::error_code ignored;
            stdfs::create_directory_symlink(sandbox.outside, link, ignored);
            stdfs::rename(directory, parked, ignored);
            stdfs::rename(link, directory, ignored);
            std::this_thread::yield();
            stdfs::rename(directory, link, ignored);
            stdfs::rename(parked, directory, ignored);
        }
    });
    int    refused = 0;
    auto   attempt = [&](auto&& operation) {
        try {
            operation();
        } catch (const fs::StorageError& error) {
            refused += error.errc() == fs::StorageErrc::linkRefused;
        }
    };
    for (int iteration = 0; iteration < 2000; ++iteration) {
        const auto name = "f" + to_string(iteration);
        attempt([&] {
            writeFile(storage, "a/" + name, "x");
        });
        attempt([&] {
            storage.createDirectory("a/d" + name);
        });
        attempt([&] {
            storage.remove("a/secret.txt");
        });
        attempt([&] {
            storage.rename("a/secret.txt", "stolen" + name);
        });
        attempt([&] {
            readAll(storage.openRead("a/secret.txt"));
            FAIL("The outside file was read");
        });
    }
    stop = true;
    swapper.join();
    REQUIRE(sandbox.outsideUntouched());
    REQUIRE(refused > 0);  // the operations did meet the link
}

SCENARIO("A directory moved out of the root by the same user is detected before the commit") {
    Sandbox sandbox;
    stdfs::create_directories(sandbox.root / "a" / "b");
    const auto moved = sandbox.outside / "b";

    GIVEN("a directory moved out while the temporary file is written") {
        auto              hooks = hooksAt([&](fs::StoragePoint point, string_view) {
            if (point == fs::StoragePoint::temporaryCreated)
                stdfs::rename(sandbox.root / "a" / "b", moved);
        });
        fs::NativeStorage storage(sandbox.root, {}, hooks);
        auto              writer = storage.beginWrite("a/b/file.txt", fs::WriteMode::createNew);
        writer.write(span<const char>("data", 4));
        THEN("the commit is abandoned with confinementViolated, without a file nor a temporary file outside") {
            REQUIRE(errorOf([&] {
                        writer.commit();
                    })
                    == fs::StorageErrc::confinementViolated);
            REQUIRE(stdfs::is_empty(moved));
        }
    }
    GIVEN("a directory moved out just before the commit's check") {
        auto              hooks = hooksAt([&](fs::StoragePoint point, string_view) {
            if (point == fs::StoragePoint::beforeCommit)
                stdfs::rename(sandbox.root / "a", sandbox.outside / "a");
        });
        fs::NativeStorage storage(sandbox.root, {}, hooks);
        THEN("the rename does not happen") {
            REQUIRE(errorOf([&] {
                        writeFile(storage, "a/b/file.txt", "data");
                    })
                    == fs::StorageErrc::confinementViolated);
            REQUIRE(stdfs::is_empty(sandbox.outside / "a" / "b"));
        }
    }
    GIVEN("a directory moved out between its opening and the operation") {
        bool              once  = true;
        auto              hooks = hooksAt([&](fs::StoragePoint point, string_view) {
            if (point == fs::StoragePoint::resolved && std::exchange(once, false))
                stdfs::rename(sandbox.root / "a" / "b", moved);
        });
        fs::NativeStorage storage(sandbox.root, {}, hooks);
        THEN("the operation reports the violation it detected afterwards") {
            REQUIRE(errorOf([&] {
                        storage.createDirectory("a/b/dir");
                    })
                    == fs::StorageErrc::confinementViolated);
            REQUIRE(errorOf([&] {
                        storage.status("a/b");
                    })
                    == fs::StorageErrc::notFound);
        }
    }
}

SCENARIO("A root moved by the same user is no longer used") {
    Sandbox    sandbox;
    const auto elsewhere = sandbox.base.path() / "elsewhere";

    GIVEN("a root moved before an operation") {
        fs::NativeStorage storage(sandbox.root);
        stdfs::rename(sandbox.root, elsewhere);
        THEN("every operation fails with rootMoved, without effect, even once a directory is back at the location") {
            REQUIRE(errorOf([&] {
                        storage.createDirectory("dir");
                    })
                    == fs::StorageErrc::rootMoved);
            REQUIRE(stdfs::is_empty(elsewhere));
            stdfs::rename(elsewhere, sandbox.root);
            REQUIRE(errorOf([&] {
                        storage.list("");
                    })
                    == fs::StorageErrc::rootMoved);
            REQUIRE(stdfs::is_empty(sandbox.root));
        }
    }
    GIVEN("another directory put at the location") {
        fs::NativeStorage storage(sandbox.root);
        stdfs::rename(sandbox.root, elsewhere);
        stdfs::create_directory(sandbox.root);
        THEN("it is not taken for the root") {
            REQUIRE(errorOf([&] {
                        writeFile(storage, "file.txt", "x");
                    })
                    == fs::StorageErrc::rootMoved);
            REQUIRE(stdfs::is_empty(sandbox.root));
        }
    }
    GIVEN("a root moved during a write") {
        auto              hooks = hooksAt([&](fs::StoragePoint point, string_view) {
            if (point == fs::StoragePoint::temporaryCreated)
                stdfs::rename(sandbox.root, elsewhere);
        });
        fs::NativeStorage storage(sandbox.root, {}, hooks);
        THEN("the commit is abandoned with confinementViolated") {
            REQUIRE(errorOf([&] {
                        writeFile(storage, "file.txt", "x");
                    })
                    == fs::StorageErrc::confinementViolated);
            REQUIRE(stdfs::is_empty(elsewhere));
        }
    }
}

SCENARIO("POSIX directories another user could move refuse the root or the operation") {
    Sandbox                              sandbox;
    const auto                           otherUser = ::geteuid() == 4242u ? 4243u : 4242u;
    const auto                           base      = stdfs::canonical(sandbox.base.path());
    const auto                           root      = base / "root";
    auto                                 hooks     = make_shared<fs::StorageHooks>();
    map<stdfs::path, fs::DirectoryFacts> overrides;
    hooks->facts = [&](const stdfs::path& path, fs::DirectoryFacts& facts) {
        if (auto found = overrides.find(path); found != overrides.end())
            facts = found->second;
    };
    const auto own   = static_cast<uint32_t>(::geteuid());
    auto       facts = [](uint32_t owner, uint32_t mode) {
        return fs::DirectoryFacts{owner, S_IFDIR | mode};
    };

    GIVEN("a root owned by another user, even in mode 0755") {
        overrides[root] = facts(otherUser, 0755);
        THEN("its declaration is refused") {
            REQUIRE(errorOf([&] {
                        fs::NativeStorage(sandbox.root, {}, hooks);
                    })
                    == fs::StorageErrc::unsafeDirectory);
        }
    }
    GIVEN("a root its group can write") {
        stdfs::permissions(sandbox.root, stdfs::perms::group_write, stdfs::perm_options::add);
        THEN("its declaration is refused, unless the root is declared shared") {
            REQUIRE(errorOf([&] {
                        fs::NativeStorage(sandbox.root);
                    })
                    == fs::StorageErrc::unsafeDirectory);
            fs::NativeStorage shared(sandbox.root, {.shared = true});
            shared.createDirectory("dir");
        }
    }
    GIVEN("an ancestor owned by another user, in mode 0755") {
        overrides[base] = facts(otherUser, 0755);
        THEN("the declaration is refused") {
            REQUIRE(errorOf([&] {
                        fs::NativeStorage(sandbox.root, {}, hooks);
                    })
                    == fs::StorageErrc::unsafeDirectory);
        }
    }
    GIVEN("an ancestor writable by the others") {
        overrides[base] = facts(own, 0777);
        THEN("the declaration is refused") {
            REQUIRE(errorOf([&] {
                        fs::NativeStorage(sandbox.root, {}, hooks);
                    })
                    == fs::StorageErrc::unsafeDirectory);
        }
    }
    GIVEN("a sticky ancestor owned by another user") {
        overrides[base] = facts(otherUser, 01777);
        THEN("the declaration is refused") {
            REQUIRE(errorOf([&] {
                        fs::NativeStorage(sandbox.root, {}, hooks);
                    })
                    == fs::StorageErrc::unsafeDirectory);
        }
    }
    GIVEN("a sticky ancestor of the superuser, as /tmp") {
        overrides[base] = facts(0, 01777);
        THEN("a next element of the effective user is accepted") {
            fs::NativeStorage storage(sandbox.root, {}, hooks);
            storage.createDirectory("dir");
        }
        THEN("a next element of another user is refused") {
            overrides[root] = facts(otherUser, 0755);
            REQUIRE(errorOf([&] {
                        fs::NativeStorage(sandbox.root, {}, hooks);
                    })
                    == fs::StorageErrc::unsafeDirectory);
        }
    }
    GIVEN("a root declared, then an ancestor that becomes unsafe") {
        fs::NativeStorage storage(sandbox.root, {}, hooks);
        overrides[base] = facts(otherUser, 0755);
        THEN("the next operation is refused before any effect") {
            REQUIRE(errorOf([&] {
                        storage.createDirectory("dir");
                    })
                    == fs::StorageErrc::unsafeDirectory);
            REQUIRE(stdfs::is_empty(sandbox.root));
        }
    }
    GIVEN("directories inside the root another user could move") {
        stdfs::create_directories(sandbox.root / "group" / "deep");
        stdfs::create_directories(sandbox.root / "foreign" / "deep");
        stdfs::permissions(sandbox.root / "group", stdfs::perms::group_write, stdfs::perm_options::add);
        overrides[root / "foreign"] = facts(otherUser, 0755);
        fs::NativeStorage storage(sandbox.root, {}, hooks);
        THEN("operations traversing them are refused before any effect") {
            REQUIRE(errorOf([&] {
                        storage.createDirectory("group/deep/new");
                    })
                    == fs::StorageErrc::unsafeDirectory);
            REQUIRE(errorOf([&] {
                        writeFile(storage, "foreign/deep/new.txt", "x");
                    })
                    == fs::StorageErrc::unsafeDirectory);
            REQUIRE(stdfs::is_empty(sandbox.root / "group" / "deep"));
            REQUIRE(stdfs::is_empty(sandbox.root / "foreign" / "deep"));
        }
    }
    GIVEN("directories the storage creates, whatever the umask") {
        const auto        previous = ::umask(0);
        fs::NativeStorage storage(sandbox.root);
        storage.createDirectory("created");
        writeFile(storage, "created/file.txt", "x");
        ::umask(previous);
        THEN("they stay usable by the storage") {
            REQUIRE((stdfs::status(sandbox.root / "created").permissions() & (stdfs::perms::group_write | stdfs::perms::others_write)) == stdfs::perms::none);
            REQUIRE(storage.list("created").entries.size() == 1);
        }
    }
}

    #if defined(__APPLE__)
SCENARIO("macOS ACLs that let another user change a directory refuse the root or the operation") {
    Sandbox sandbox;
    auto    grant = [](const stdfs::path& directory, const string& who) {
        const auto command = "chmod +a '" + who + " allow add_file,add_subdirectory,delete_child' '" + directory.string() + "'";
        REQUIRE(std::system(command.c_str()) == 0);
    };

    GIVEN("a root in mode 0700 whose ACL lets everyone add and remove entries") {
        stdfs::permissions(sandbox.root, stdfs::perms::owner_all, stdfs::perm_options::replace);
        grant(sandbox.root, "everyone");
        THEN("its declaration is refused, unless the root is declared shared") {
            REQUIRE(errorOf([&] {
                        fs::NativeStorage(sandbox.root);
                    })
                    == fs::StorageErrc::unsafeDirectory);
            fs::NativeStorage shared(sandbox.root, {.shared = true});
            shared.createDirectory("dir");
        }
    }
    GIVEN("an ancestor whose ACL lets everyone remove its entries") {
        grant(sandbox.base.path(), "everyone");
        THEN("the declaration is refused") {
            REQUIRE(errorOf([&] {
                        fs::NativeStorage(sandbox.root);
                    })
                    == fs::StorageErrc::unsafeDirectory);
        }
    }
    GIVEN("a directory inside the root granted to everyone after the declaration") {
        fs::NativeStorage storage(sandbox.root);
        stdfs::create_directories(sandbox.root / "shared" / "deep");
        grant(sandbox.root / "shared", "everyone");
        THEN("operations through it are refused before any effect") {
            REQUIRE(errorOf([&] {
                        storage.createDirectory("shared/deep/new");
                    })
                    == fs::StorageErrc::unsafeDirectory);
            REQUIRE(stdfs::is_empty(sandbox.root / "shared" / "deep"));
        }
    }
    GIVEN("an ACL that grants those rights to the effective user only") {
        grant(sandbox.root, "user:" + string(::getpwuid(::geteuid())->pw_name));
        THEN("the root is accepted") {
            fs::NativeStorage storage(sandbox.root);
            storage.createDirectory("dir");
        }
    }
}
    #endif

SCENARIO("A root under the real /tmp of the superuser is accepted") {
    // /tmp is owned by the superuser, writable by all and sticky: an element of the effective user under it is safe.
    std::error_code error;
    if (!stdfs::is_directory("/tmp", error))
        SKIP("No /tmp on this system");
    struct ::stat status{};
    REQUIRE(::stat("/tmp", &status) == 0);
    if (status.st_uid != 0 || (status.st_mode & S_ISVTX) == 0)
        SKIP("/tmp is not a sticky directory of the superuser here");
    const auto root = stdfs::path("/tmp") / ("webfront_storage_" + to_string(::getpid()));
    stdfs::create_directory(root);
    stdfs::permissions(root, stdfs::perms::owner_all, stdfs::perm_options::replace);
    fs::NativeStorage storage(root);
    writeFile(storage, "file.txt", "x");
    REQUIRE(storage.list("").entries.size() == 1);
    stdfs::remove_all(root);
}
#endif

#ifdef _WIN32
SCENARIO("Windows holds the directories of an operation so that nobody moves them") {
    Sandbox sandbox;
    stdfs::create_directories(sandbox.root / "a" / "b");
    std::error_code rootRename, directoryRename, ancestorRename, baseRename;

    GIVEN("moves attempted while the temporary file is written and before the commit") {
        auto              hooks = hooksAt([&](fs::StoragePoint point, string_view) {
            if (point == fs::StoragePoint::temporaryCreated) {
                stdfs::rename(sandbox.root / "a" / "b", sandbox.outside / "b", directoryRename);
                // An ancestor of the root is not held: NTFS refuses to rename a directory below which a handle is open.
                stdfs::rename(sandbox.base.path(), sandbox.base.path().string() + "-moved", baseRename);
            }
            if (point == fs::StoragePoint::beforeCommit)
                stdfs::rename(sandbox.root / "a", sandbox.outside / "a", ancestorRename);
        });
        fs::NativeStorage storage(sandbox.root, {}, hooks);
        stdfs::rename(sandbox.root, sandbox.base.path() / "elsewhere", rootRename);
        writeFile(storage, "a/b/file.txt", "data");
        THEN("each move fails and the write ends under the root") {
            REQUIRE(rootRename);
            REQUIRE(directoryRename);
            REQUIRE(ancestorRename);
            REQUIRE(baseRename);
            REQUIRE(readDisk(sandbox.root / "a" / "b" / "file.txt") == "data");
            REQUIRE(sandbox.outsideUntouched());
        }
    }
}
#endif
