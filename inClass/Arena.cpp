#include "Arena.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

Arena::Arena(const std::string& config_file)
    : config_(load_config(config_file)),
      board_(static_cast<std::size_t>(config_.rows), std::string(static_cast<std::size_t>(config_.cols), '.')),
      rng_(std::random_device{}())
{
}

GameConfig Arena::load_config(const std::string& filename)
{
    GameConfig cfg;
    std::ifstream in(filename);
    if (!in) {
        std::cerr << "Could not open " << filename << ", using defaults.\n";
        return cfg;
    }

    // Very simple parser for class demo:
    // each line should look like Key:Value
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') { // allow comments
            continue;
        }

        std::size_t split = line.find(':');
        if (split == std::string::npos) {
            // Future improvement: warn on malformed lines.
            continue;
        }

        std::string key = line.substr(0, split);
        std::string value = line.substr(split + 1);
        std::istringstream vs(value);

        if (key == "Arena_Size") {
            vs >> cfg.rows >> cfg.cols;
        } else if (key == "Max_Rounds") {
            vs >> cfg.max_rounds;
        } else if (key == "Sleep_interval") {
            vs >> cfg.sleep_interval;
        } else if (key == "Game_State_Live") {
            std::string live;
            vs >> live;
            cfg.game_state_live = (live == "true");
        } else if (key == "Flamethrowers") {
            vs >> cfg.flamethrowers;
        } else if (key == "Pits") {
            vs >> cfg.pits;
        } else if (key == "Mounds") {
            vs >> cfg.mounds;
        }
    }

    // Future guards:
    // - clamp rows/cols to a minimum
    // - reject negative obstacle counts
    // - accept boolean variants like 1/0, yes/no
    return cfg;
}

std::vector<std::string> Arena::discover_robot_sources()
{
    // Barebones version: just scan current directory.
    // Future improvement: also scan ./robots and deduplicate.
    std::vector<std::string> out;
    if (!fs::exists(".") || !fs::is_directory(".")) {
        return out;
    }

    for (const auto& entry : fs::directory_iterator(".")) {
        if (!entry.is_regular_file()) {
            continue;
        }
        fs::path p = entry.path();
        std::string filename = p.filename().string();
        if (p.extension() == ".cpp" && filename.rfind("Robot_", 0) == 0) {
            out.push_back(p.string());
        }
    }

    std::sort(out.begin(), out.end());
    return out;
}

void Arena::place_obstacles(char obstacle_type, int count)
{
    // Place random obstacles on empty cells.
    // Simple on purpose for teaching.
    std::uniform_int_distribution<int> row_dist(0, config_.rows - 1);
    std::uniform_int_distribution<int> col_dist(0, config_.cols - 1);

    int placed = 0;
    while (placed < count) {
        int r = row_dist(rng_);
        int c = col_dist(rng_);
        if (board_[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] == '.') {
            board_[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] = obstacle_type;
            ++placed;
        }
    }

    // Future guard:
    // add an attempt limit to prevent an infinite loop when count > empty cells.
}

bool Arena::initialize_board()
{
    // Reset board to empty.
    board_.assign(static_cast<std::size_t>(config_.rows), std::string(static_cast<std::size_t>(config_.cols), '.'));

    // Add configured obstacles.
    place_obstacles('F', config_.flamethrowers);
    place_obstacles('P', config_.pits);
    place_obstacles('M', config_.mounds);

    return true;
}

void Arena::print_board() const
{
    std::cout << "    ";
    for (int c = 0; c < config_.cols; ++c) {
        std::cout << std::setw(2) << c << ' ';
    }
    std::cout << '\n';

    for (int r = 0; r < config_.rows; ++r) {
        std::cout << std::setw(3) << r << ' ';
        for (int c = 0; c < config_.cols; ++c) {
            std::cout << ' ' << board_[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] << ' ';
        }
        std::cout << '\n';
    }
}
