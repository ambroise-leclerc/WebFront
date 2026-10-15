#include <components/Terminal.hpp>
#include <WebFront.hpp>

#include <exception>
#include <ostream>
#include <stdexcept>
#include <string>

// LCOV_EXCL_START - interactive example; the terminal shell and its page are covered by the tests.
int main() {
    webfront::TerminalApp app;
    webfront::Terminal    terminal{app, {.prompt = "\x1b[32mdemo>\x1b[0m "}};

    terminal.command("greet", "Greet someone: greet [name]", [](webfront::CommandArgs args) {
        return "Hello, " + (args.empty() ? std::string{"world"} : args[0]) + "!";
    });
    terminal.command("sum", "Add numbers: sum 1 2.5 3", [](webfront::CommandArgs args) {
        double total = 0;
        for (const auto& number : args) {
            try {
                total += std::stod(number);
            } catch (const std::exception&) {  // a throwing command is shown as an error in the terminal
                throw std::invalid_argument("'" + number + "' is not a number");
            }
        }
        return std::to_string(total);
    });
    terminal.command("args", "Show how a line is split into arguments", [](webfront::CommandArgs args, std::ostream& output) {
        for (std::size_t index = 0; index < args.size(); ++index)
            output << index << ": [" << args[index] << "]\n";
    });

    app.show(terminal.page);
}
// LCOV_EXCL_STOP
