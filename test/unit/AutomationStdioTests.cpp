// Stdio must be safe as the first Windows include, before the socket declarations.
// clang-format off
#include <webfront/automation/Stdio.hpp>
#include <webfront/automation/Channel.hpp>
// clang-format on
#include "Bdd.hpp"
#include <webfront/automation/Interrupt.hpp>

using namespace webfront::bdd;

Scenario nativeInterrupt{"Native interrupt handlers record catchable signals without doing shutdown work", [] {
    webfront::automation::Interrupt interrupt;
    require(interrupt.signal() == 0);
    require(std::raise(SIGINT) == 0);
    require(interrupt.signal() == SIGINT);
    require(std::raise(SIGTERM) == 0);
    require(interrupt.signal() == SIGTERM);
}};

#ifdef _WIN32
Scenario windowsMCP{"Windows MCP stdin drains a final unterminated line after its pipe closes", [] {
    struct Pipe {
        HANDLE saved = GetStdHandle(STD_INPUT_HANDLE), reader = nullptr, writer = nullptr;
        ~Pipe() {
            SetStdHandle(STD_INPUT_HANDLE, saved);
            if (reader)
                CloseHandle(reader);
            if (writer)
                CloseHandle(writer);
        }
    } pipe;
    require(CreatePipe(&pipe.reader, &pipe.writer, nullptr, 0));
    require(SetStdHandle(STD_INPUT_HANDLE, pipe.reader));
    DWORD written = 0;
    require(WriteFile(pipe.writer, "one\ntwo\npart", 12, &written, nullptr));
    require(written == 12);
    webfront::automation::StdioInput input;
    std::string                      line;
    require(input.read(line) == webfront::automation::StdioInput::Status::line);
    require(line == "one");
    require(input.read(line) == webfront::automation::StdioInput::Status::line);
    require(line == "two");
    require(input.read(line) == webfront::automation::StdioInput::Status::waiting);
    require(CloseHandle(pipe.writer));
    pipe.writer = nullptr;
    require(input.read(line) == webfront::automation::StdioInput::Status::line);
    require(line == "part");
    require(input.read(line) == webfront::automation::StdioInput::Status::closed);
}};
#endif
