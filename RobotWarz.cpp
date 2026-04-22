#include "Arena.h"

#include <ctime>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {
void clear_terminal_screen()
{
    std::cout << "\x1b[2J\x1b[H";
}
}

int main(int argc, char* argv[])
{
    std::srand(static_cast<unsigned>(std::time(nullptr)));

    const std::string config_file = (argc >= 2) ? argv[1] : "RobotWarz.cfg";
    bool force_non_live = false;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--off" || arg == "--no-live" || arg == "--fast") {
            force_non_live = true;
        }
    }

    Arena arena(config_file);
    // Default to live mode unless explicitly disabled by a CLI flag.
    arena.set_live_mode(!force_non_live);
    if (!arena.load_robots()) {
        return 1;
    }
    clear_terminal_screen();
    if (!arena.initialize_board()) {
        return 1;
    }

    arena.print_board();
    std::cout << "Press enter key to begin.";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

    arena.run_simulation();
    return 0;
}
