// Interactive user-mode shell for seele-os.
//
// Runs in U-mode as a standalone ELF64 executable embedded into the kernel and
// communicates exclusively through the read / write / shutdown syscalls.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "crt/io.h"
#include "crt/syscall.h"
#include "uapi/syscall.h"

namespace {

constexpr std::size_t LINE_CAPACITY = 128;

struct input_line {
    char text[LINE_CAPACITY]{};
    std::size_t length = 0;
};

struct command_line {
    std::string_view command;
    std::string_view arguments;
};

void write_char(char c) noexcept {
    user::write_all(uapi::STDOUT_FD, std::string_view{&c, 1});
}

void prompt() noexcept {
    user::print("seele> ");
}

// Split "[spaces]command [spaces]arguments", keeping the argument text intact.
// Slices are built by hand rather than with string_view::substr() to avoid
// pulling std::terminate() into freestanding builds.
[[nodiscard]] command_line parse(std::string_view input) noexcept {
    std::size_t begin = 0;
    while(begin < input.size() && input[begin] == ' ') {
        ++begin;
    }

    std::size_t end = begin;
    while(end < input.size() && input[end] != ' ') {
        ++end;
    }

    std::size_t arguments = end;
    while(arguments < input.size() && input[arguments] == ' ') {
        ++arguments;
    }

    return command_line{
        std::string_view{input.data() + begin,     end - begin             },
        std::string_view{input.data() + arguments, input.size() - arguments},
    };
}

// Read and echo one input line; Ctrl-C cancels the current line without exiting.
[[nodiscard]] bool read_line(input_line& line) noexcept {
    line.length = 0;

    while(true) {
        char c = '\0';
        const int64_t n = user::sys::read(uapi::STDIN_FD, &c, 1);
        if(n <= 0) {
            continue;
        }

        switch(c) {
            case '\r':
            case '\n': {
                write_char('\n');
                return true;
            }
            case 0x7f:  // DEL from a typical terminal
            case '\b': {
                if(line.length != 0) {
                    --line.length;
                    user::write_all(uapi::STDOUT_FD, "\b \b");
                }
                break;
            }
            case 0x03: {  // Ctrl-C: abandon the line, keep the shell alive
                user::write_all(uapi::STDOUT_FD, "^C\n");
                line.length = 0;
                return false;
            }
            default: {
                if(c >= ' ' && c <= '~' && line.length + 1 < LINE_CAPACITY) {
                    line.text[line.length++] = c;
                    write_char(c);
                }
                break;
            }
        }
    }
}

void print_help() noexcept {
    user::println("commands:");
    user::println("  help          show this list");
    user::println("  echo <text>   print <text>");
    user::println("  poweroff      shut down the system");
}

void execute(const input_line& line) noexcept {
    const command_line command = parse(std::string_view{line.text, line.length});
    if(command.command.empty()) {
        return;
    }

    if(command.command == "help") {
        print_help();
    } else if(command.command == "echo") {
        user::println("{}", command.arguments);
    } else if(command.command == "poweroff") {
        user::println("shutting down...");
        const int64_t err = user::sys::shutdown(0);
        if(err < 0) {
            user::println("shell: poweroff: no power device attached");
        }
    } else {
        user::println("shell: {}: command not found (type 'help')", command.command);
    }
}

}  // namespace

int main() {
    user::println("seele-os shell: type 'help' for commands");

    input_line line;
    while(true) {
        prompt();
        if(!read_line(line)) {
            continue;
        }
        execute(line);
    }
}
