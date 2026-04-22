#include "Arena.h"

#include <iostream>
#include <string>

int main(int argc, char* argv[])
{
    std::string config_file = (argc >= 2) ? argv[1] : "RobotWarz.cfg";

    Arena arena(config_file);
    arena.initialize_board();

    std::cout << "\nRobot source files discovered:\n";
    auto robots = Arena::discover_robot_sources();
    if (robots.empty()) {
        std::cout << "  (none found in ./ or ./robots)\n";
    } else {
        for (const auto& file : robots) {
            std::cout << "  " << file << '\n';
        }
    }

    std::cout << "\nInitial board with obstacles:\n";
    arena.print_board();

    return 0;
}
