// Stdio must be safe as the first Windows include, before the socket declarations.
#include <catch2/catch_test_macros.hpp>
#include <webfront/automation/Channel.hpp>
#include <webfront/automation/Stdio.hpp>

#ifdef _WIN32
SCENARIO("Windows MCP stdin drains a final unterminated line after its pipe closes") {
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
    REQUIRE(CreatePipe(&pipe.reader, &pipe.writer, nullptr, 0));
    REQUIRE(SetStdHandle(STD_INPUT_HANDLE, pipe.reader));
    DWORD written = 0;
    REQUIRE(WriteFile(pipe.writer, "one\ntwo\npart", 12, &written, nullptr));
    REQUIRE(written == 12);
    webfront::automation::StdioInput input;
    std::string                      line;
    REQUIRE(input.read(line) == webfront::automation::StdioInput::Status::line);
    REQUIRE(line == "one");
    REQUIRE(input.read(line) == webfront::automation::StdioInput::Status::line);
    REQUIRE(line == "two");
    REQUIRE(input.read(line) == webfront::automation::StdioInput::Status::waiting);
    REQUIRE(CloseHandle(pipe.writer));
    pipe.writer = nullptr;
    REQUIRE(input.read(line) == webfront::automation::StdioInput::Status::line);
    REQUIRE(line == "part");
    REQUIRE(input.read(line) == webfront::automation::StdioInput::Status::closed);
}
#endif
