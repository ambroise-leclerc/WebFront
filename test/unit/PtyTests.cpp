#include <webfront/testing/Bdd.hpp>
#include "TemporaryDirectory.hpp"
#include <webfront/Pty.hpp>

#include <charconv>
#include <fstream>
#include <future>
#include <string_view>

#ifndef _WIN32
    #include <fcntl.h>
    #include <sys/socket.h>
#endif

namespace {
using namespace std::chrono_literals;
using namespace webfront;
using namespace webfront::bdd;

std::string probe() {
    const auto path = std::filesystem::path(WEBFRONT_PTY_PROBE).u8string();
    return {reinterpret_cast<const char*>(path.data()), path.size()};
}

PtyOptions shell() {
#ifdef _WIN32
    return {
        .argv        = {"cmd.exe", "/d", "/q"},
        .directory   = {},
        .environment = {},
        .size        = {87, 26}
    };
#else
    return {
        .argv        = {"sh", "-i"},
        .directory   = {},
        .environment = {},
        .size        = {87, 26}
    };
#endif
}

void send(Pty& terminal, std::string_view text) {
    require(terminal.write({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}));
}

/// The text without terminal control sequences. A terminal can insert them anywhere in the output (ConPTY sets the
/// window title while a program starts, possibly in the middle of a line), so markers are searched in this form.
std::string plain(std::string_view raw) {
    std::string result;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '\x1b' || i + 1 >= raw.size()) {
            result += raw[i];
            continue;
        }
        const char kind = raw[++i];
        if (kind == '[') { // CSI: parameters, then a final byte in 0x40-0x7e
            while (i + 1 < raw.size() && (raw[i + 1] < '@' || raw[i + 1] > '~'))
                ++i;
            ++i;
        } else if (kind == ']') { // OSC: up to BEL or ESC followed by a backslash
            while (i + 1 < raw.size() && raw[i + 1] != '\a' && raw[i + 1] != '\x1b')
                ++i;
            ++i;
            if (i < raw.size() && raw[i] == '\x1b')
                ++i;
        }
    }
    return result;
}

struct Transcript {
    std::uint64_t      cursor = 0;
    std::string        text;
    std::optional<int> exit;

    void until(Pty& terminal, std::string_view marker) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(8s * impl::timeoutFactor());
        do {
            auto part = terminal.tryRead(cursor);
            cursor    = part.next;
            if (!part.bytes.empty())
                text.append(reinterpret_cast<const char*>(part.bytes.data()), part.bytes.size());
            exit = part.exitCode;
            if (marker.empty() ? exit.has_value() : plain(text).contains(marker))
                return;
            std::this_thread::sleep_for(10ms);
        } while (!exit && std::chrono::steady_clock::now() < deadline);
        auto context = info("waiting for " + std::string(marker) + ", exit=" + (exit ? std::to_string(*exit) : "running") + ", output=" + text);
        require(false);
    }
};

unsigned stallPid(std::string_view raw) {
    const auto text         = plain(raw);
    const auto start        = text.find("STALLED:") + std::string_view("STALLED:").size();
    unsigned   pid          = 0;
    const auto [end, error] = std::from_chars(text.data() + start, text.data() + text.size(), pid);
    require(error == std::errc{});
    require(end < text.data() + text.size() && *end == ':');
    return pid;
}

bool running(unsigned pid) {
#ifdef _WIN32
    const HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!process)
        return false;
    const bool active = ::WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    ::CloseHandle(process);
    return active;
#else
    if (::kill(static_cast<pid_t>(pid), 0) < 0)
        return false;
    #ifdef __linux__
    // The orphan can remain a zombie briefly (or indefinitely under a container PID 1).
    std::ifstream status("/proc/" + std::to_string(pid) + "/stat");
    std::string   line;
    if (std::getline(status, line) && line.substr(line.rfind(')') + 2, 1) == "Z")
        return false;
    #endif
    return true;
#endif
}

const Scenario ptyShell{"PTY exchanges bytes with an interactive shell and resizes and interrupts its foreground", [] {
                            given("a native shell", [] {
                                Pty        terminal(shell());
                                Transcript output;
#ifdef _WIN32
                                send(terminal, "\"" + probe() + "\" size\r");
                                output.until(terminal, "SIZE:87x26");
#else
        send(terminal, "stty size; printf 'term=%s\\n' \"$TERM\"\n");
        output.until(terminal, "26 87");
        output.until(terminal, "term=xterm-256color");
#endif
                                when("the application changes the terminal size", [&] {
                                    terminal.resize({101, 33});
#ifdef _WIN32
                                    send(terminal, "\"" + probe() + "\" size\r");
                                    output.until(terminal, "SIZE:101x33");
#else
            send(terminal, "stty size\n");
            output.until(terminal, "33 101");
#endif
                                });
                                then("the foreground command is interrupted and the shell exits with its own status", [&] {
#ifdef _WIN32
                                    // The console control event can also affect cmd itself. Exercise it with a directly hosted probe.
                                    Pty        foreground({
                                               .argv        = {probe(), "interrupt"},
                                               .directory   = {},
                                               .environment = {},
                                               .size        = {}
                                    });
                                    Transcript interrupted;
                                    interrupted.until(foreground, "INTERRUPT_READY");
                                    send(foreground, "\x03");
                                    interrupted.until(foreground, "");
                                    require(interrupted.exit == static_cast<int>(0xc000013au));
                                    send(terminal, "echo HELLO_&echo _PTY\rexit 42\r");
                                    output.until(terminal, "_PTY");
#else
            send(terminal, "\"" + probe() + "\" interrupt\n");
            output.until(terminal, "INTERRUPT_READY");
            send(terminal, "\x03");
            send(terminal, "exit 42\n");
#endif
                                    output.until(terminal, "");
                                    require(output.exit == 42);
                                    auto replay = terminal.tryRead(0);
                                    require(!replay.bytes.empty());
                                    require(!replay.truncated);
                                });
                            });
                        }};

const Scenario ptyControlSequences{"PTY test markers are found through terminal control sequences inserted in the output", [] {
                                       // ConPTY sets the window title while the probe starts, here in the middle of the marker.
                                       const std::string raw = "\x1b[2J\x1b[4;1H> probe stall\r\nSTALLED:\x1b]0;cmd.exe - probe\a" "25776:END\r\n\x1b]0;t\x1b\\";
                                       require(plain(raw) == "> probe stall\r\nSTALLED:25776:END\r\n");
                                       require(stallPid(raw) == 25776);
                                   }};

const Scenario ptyHangUp{"PTY refuses oversized input promptly and terminates a stalled foreground descendant", [] {
                             Pty        terminal(shell());
                             Transcript output;
#ifdef _WIN32
                             send(terminal, "\"" + probe() + "\" stall\r");
#else
    send(terminal, "\"" + probe() + "\" stall\n");
#endif
                             output.until(terminal, ":END");
                             const auto pid = stallPid(output.text);
                             require(running(pid));
                             const std::vector<std::uint8_t> paste(4 * 1024 * 1024, 'x');
                             then("large input is refused and the pending queue has a fixed limit", [&] {
                                 const auto start = std::chrono::steady_clock::now();
                                 require(!terminal.write(paste));
                                 const auto packet = std::span{paste}.first(Pty::inputCapacity);
                                 bool       full   = false;
                                 for (int i = 0; i < 20 && !full; ++i)
                                     full = !terminal.write(packet);
                                 require(full);
                                 require(std::chrono::steady_clock::now() - start < 1s);
                             });
                             then("hangUp terminates the foreground descendant and publishes the shell status", [&] {
                                 terminal.hangUp();
                                 output.until(terminal, "");
                                 const auto deadline = std::chrono::steady_clock::now() + 2s;
                                 while (running(pid) && std::chrono::steady_clock::now() < deadline)
                                     std::this_thread::sleep_for(10ms);
                                 require(!running(pid));
                                 require(!terminal.write({}));
                             });
                         }};

const Scenario ptyInterruptRead{"PTY blocking reads observe a stop request", [] {
                                    Pty        terminal({
                                               .argv        = {probe(), "stall"},
                                               .directory   = {},
                                               .environment = {},
                                               .size        = {}
                                    });
                                    Transcript output;
                                    output.until(terminal, ":END");
                                    std::stop_source stop;
                                    auto             result = std::async(std::launch::async, [&] {
                                        return terminal.read(output.cursor, stop.get_token());
                                    });
                                    require(result.wait_for(50ms) == std::future_status::timeout);
                                    stop.request_stop();
                                    const auto ready = result.wait_for(1s);
                                    if (ready != std::future_status::ready)
                                        terminal.hangUp();
                                    require(ready == std::future_status::ready);
                                    const auto cancelled = result.get();
                                    require(cancelled.bytes.empty());
                                    require(cancelled.next == output.cursor);
                                    require(!cancelled.exitCode);
                                }};

#ifdef _WIN32
const Scenario ptyDescendantOutput{"ConPTY drains a descendant's output after the leader exits", [] {
                                       Pty        terminal({
                                                  .argv        = {probe(), "leave-child"},
                                                  .directory   = {},
                                                  .environment = {},
                                                  .size        = {}
                                       });
                                       Transcript output;
                                       output.until(terminal, "DESCENDANT_OUTPUT");
                                       output.until(terminal, "");
                                       require(output.exit == 42);
                                   }};

const Scenario ptyBatchShim{"PTY resolves Windows npm-style batch shims from the explicit PATH", [] {
                                webfront::test::TemporaryDirectory directory;
                                const auto                         root = directory.path();
                                {
                                    std::ofstream script(root / "pty test.cmd");
                                    script << "@echo off\r\necho [\"%~1\"]\r\nexit /b 42\r\n";
                                }
                                const auto path = root.u8string();
                                Pty        terminal({
                                           .argv        = {"pty test", "a & b"},
                                           .directory   = root,
                                           .environment = {{"Path", {reinterpret_cast<const char*>(path.data()), path.size()}}},
                                           .size        = {}
                                });
                                Transcript output;
                                output.until(terminal, "[\"a & b\"]");
                                output.until(terminal, "");
                                require(output.exit == 42);
                            }};
#endif

#ifndef _WIN32
const Scenario ptyReplay{"PTY resolves PATH in the child directory and bounds its output history", [] {
                             webfront::test::TemporaryDirectory directory;
                             const auto                         root = directory.path();
                             std::ofstream(root / "marker").put('!');
                             std::filesystem::create_symlink(std::filesystem::path(WEBFRONT_PTY_PROBE), root / "terminal-flood");
                             Pty terminal({
                                 .argv        = {"terminal-flood", "flood"},
                                 .directory   = root,
                                 .environment = {{"PATH", ".:/usr/bin:/bin"}, {"WF_TEST_ENVIRONMENT", "explicit"}},
                                 .size        = {}
                             });
                             requireEventually(
                                 [&] {
                                     return terminal.exitCode().has_value();
                                 },
                                 15s);
                             auto context = info("child exit: " + std::to_string(terminal.exitCode().value()));
                             require(terminal.exitCode() == 0);
                             auto part = terminal.tryRead(0);
                             require(part.truncated);
                             require(part.bytes.size() == Pty::readChunk);
                             require(part.next - part.bytes.size() == 1200008 - Pty::replayCapacity);
                             std::uint64_t cursor = part.next;
                             while (!part.exitCode) {
                                 part   = terminal.tryRead(cursor);
                                 cursor = part.next;
                             }
                             const std::string tail(reinterpret_cast<const char*>(part.bytes.data()), part.bytes.size());
                             require(tail.ends_with("COMPLETE"));
                         }};

const Scenario ptyReopen{"PTY resumes output when the live child reopens its slave", [] {
                             Pty        terminal({
                                        .argv        = {probe(), "reopen"},
                                        .directory   = {},
                                        .environment = {},
                                        .size        = {}
                             });
                             Transcript output;
                             output.until(terminal, "REOPENED");
                             output.until(terminal, "");
                             require(output.exit == 0);
                         }};

const Scenario ptyOrphan{"PTY cleans up a foreground group that ignores SIGHUP after the session leader exits", [] {
                             Pty        terminal({
                                        .argv        = {probe(), "orphan"},
                                        .directory   = {},
                                        .environment = {},
                                        .size        = {}
                             });
                             Transcript output;
                             output.until(terminal, ":END");
                             const auto pid = stallPid(output.text);
                             output.until(terminal, "");
                             require(output.exit == 0);
                             requireEventually(
                                 [&] {
                                     return !running(pid);
                                 },
                                 3s);
                         }};

const Scenario ptyDescriptors{"PTY child does not inherit an application descriptor", [] {
                                  pty::detail::PtyFd socket{::socket(AF_INET, SOCK_STREAM, 0)};
                                  require(socket.get() >= 0);
                                  // Reserve a number the loader won't normally use for its own files.
                                  pty::detail::PtyFd descriptor{::fcntl(socket.get(), F_DUPFD, 100)};
                                  require(descriptor.get() >= 3);
                                  require(::fcntl(descriptor.get(), F_GETFD) == 0);
                                  Pty        terminal({
                                             .argv        = {probe(), "fd", std::to_string(descriptor.get())},
                                             .directory   = {},
                                             .environment = {},
                                             .size        = {}
                                  });
                                  Transcript output;
                                  output.until(terminal, "");
                                  require(output.exit == 0);
                              }};
#endif
}  // namespace
