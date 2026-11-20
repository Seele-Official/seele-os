#include "proc/shell.h"

#include <cstddef>
#include <string_view>

#include "console.h"
#include "hart.h"
#include "riscv.h"

namespace proc {
namespace {

// One input line, kept on the shell's own kernel stack. The shell has no heap
// objects and no dynamic buffers: a line is edited in place and parsed as a
// string_view over the same bytes.
constexpr std::size_t LINE_CAPACITY = 128;

struct input_line {
    char text[LINE_CAPACITY]{};
    std::size_t length = 0;
};

struct command_line {
    std::string_view command;
    std::string_view arguments;
};

// Raw console writes. The typed console prints characters one at a time, but
// the format layer only knows numbers and strings, so a single char becomes a
// one-byte string_view here.
void write(std::string_view text) noexcept {
    vprint(text, format_args{});
}

void write(char c) noexcept {
    write(std::string_view{&c, 1});
}

void prompt() noexcept {
    print("seele> ");
}

// Split "[spaces]command [spaces]arguments", with the argument text kept whole
// so `echo hello, world` prints exactly what followed the command.
[[nodiscard]] command_line parse(std::string_view input) noexcept {
    size_t begin = 0;
    while(begin < input.size() && input[begin] == ' ') {
        ++begin;
    }

    size_t end = begin;
    while(end < input.size() && input[end] != ' ') {
        ++end;
    }

    size_t arguments = end;
    while(arguments < input.size() && input[arguments] == ' ') {
        ++arguments;
    }

    // Build the two views by hand: string_view::substr() carries a range-check
    // path that drags std::terminate() (and with it libstdc++) into the kernel.
    return command_line{
        std::string_view{input.data() + begin,     end - begin             },
        std::string_view{input.data() + arguments, input.size() - arguments}
    };
}

// Read and echo one line; Ctrl-C cancels execution of that line. Input waiting
// belongs to the console device, which may sleep this thread until it has data.
[[nodiscard]] bool read_line(input_line& line) noexcept {
    line.length = 0;

    while(true) {
        const char c = console::read();
        switch(c) {
            case '\r':
            case '\n': {
                write('\n');
                return true;
            }
            case 0x7f:  // DEL from a typical terminal
            case '\b': {
                if(line.length != 0) {
                    --line.length;
                    write("\b \b");
                }
                break;
            }
            case 0x03: {  // Ctrl-C: abandon the line, keep the thread alive
                write("^C\n");
                line.length = 0;
                return false;
            }
            default: {
                // Printable ASCII only. The terminal sends raw bytes, so a
                // control character other than the three above is ignored.
                if(c >= ' ' && c <= '~' && line.length + 1 < LINE_CAPACITY) {
                    line.text[line.length++] = c;
                    write(c);
                }
                break;
            }
        }
    }
}

void print_help() noexcept {
    println("commands:");
    println("  help          show this list");
    println("  echo <text>   print <text>");
    println("  info          show the current thread, process and kernel stack");
}

// Show the state the scheduler keeps: the running thread, its process and the
// stack slot it was given. This is the observable half of the per-thread stack
// change, so `info` is part of the shell rather than a debug-only command.
void print_info() noexcept {
    const cpu* hart = mycpu();
    const proc::thread* thread = hart->current;

    println("hart {}: idle {:#018x} current {:#018x}",
            cpuid(),
            to_uint64(vaddr(hart->idle)),
            to_uint64(vaddr(thread)));

    if(thread == nullptr) {
        println("  no current thread");
        return;
    }

    println("  current thread {:#018x} in process {:#018x}, {} thread(s)",
            to_uint64(vaddr(thread)),
            to_uint64(vaddr(thread->proc)),
            thread->proc->thread_count.load());
    println("  kstack [{:#018x}, {:#018x}), {} bytes",
            to_uint64(thread->kstack.base),
            to_uint64(thread->kstack.base) + thread->kstack.size,
            thread->kstack.size);
}

void execute(const input_line& line) noexcept {
    const command_line command = parse(std::string_view{line.text, line.length});
    if(command.command.empty()) {
        return;
    }

    if(command.command == "help") {
        print_help();
    } else if(command.command == "echo") {
        println("{}", command.arguments);
    } else if(command.command == "info") {
        print_info();
    } else {
        println("shell: {}: command not found (type 'help')", command.command);
    }
}

}  // namespace

[[noreturn]] void shell() {
    // A new thread is entered with interrupts disabled. Enable them before
    // using blocking console input and to allow normal interrupt preemption.
    riscv::csrs(riscv::registers::sstatus, riscv::csr::sstatus::SIE);

    // The boot log went through the asynchronous console: it returns once the
    // sink has queued the bytes, not once the UART has shifted them out. Flush
    // before the first prompt so the shell's output cannot overtake the boot
    // lines it follows.
    console::flush();

    println("seele-os shell: type 'help' for commands");

    input_line line;
    while(true) {
        prompt();
        if(!read_line(line)) {
            continue;
        }
        execute(line);
    }
}

}  // namespace proc
