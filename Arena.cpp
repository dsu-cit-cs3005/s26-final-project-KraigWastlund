#include "Arena.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <set>
#include <thread>

namespace fs = std::filesystem;

namespace {
// Removes leading and trailing whitespace from a string.
std::string trim(const std::string& text)
{
    std::size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
        ++start;
    }

    std::size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }

    return text.substr(start, end - start);
}

// Parses common truthy/falsey text values and falls back to default_value when unknown.
bool parse_bool(std::string value, bool default_value)
{
    value = trim(value);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (value == "true" || value == "1" || value == "yes" || value == "on") {
        return true;
    }
    if (value == "false" || value == "0" || value == "no" || value == "off") {
        return false;
    }
    return default_value;
}

int signum(int value)
{
    return (value > 0) - (value < 0);
}

void clear_terminal_screen()
{
    std::cout << "\x1b[2J\x1b[H";
}
}  // namespace

Arena::Arena(const std::string& config_file)
    : config_(load_config(config_file)),
      obstacle_board_(static_cast<std::size_t>(config_.rows), std::string(static_cast<std::size_t>(config_.cols), '.')),
      rng_(std::random_device{}())
{
}

Arena::~Arena()
{
    for (auto& entry : robots_) {
        delete entry.robot;
        entry.robot = nullptr;
        if (entry.handle != nullptr) {
            dlclose(entry.handle);
            entry.handle = nullptr;
        }
    }
}

void Arena::set_live_mode(bool live_mode)
{
    config_.game_state_live = live_mode;
}

GameConfig Arena::load_config(const std::string& filename)
{
    GameConfig cfg;
    std::ifstream in(filename);
    if (!in) {
        // Missing config is not fatal; we just run with built-in defaults.
        std::cerr << "Warning: could not open config '" << filename << "', using defaults.\n";
        return cfg;
    }

    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        // Skip empty lines and comments to allow human-readable config files.
        if (line.empty() || line[0] == '#') {
            continue;
        }

        // Accept both "Key:Value" and "Key=Value" formats.
        std::size_t split = line.find(':');
        if (split == std::string::npos) {
            split = line.find('=');
        }
        if (split == std::string::npos) {
            continue;
        }

        const std::string key = trim(line.substr(0, split));
        const std::string value = trim(line.substr(split + 1));
        std::istringstream value_stream(value);

        if (key == "Arena_Size") {
            int rows = cfg.rows;
            int cols = cfg.cols;
            if (value_stream >> rows >> cols) {
                // Keep the board large enough to avoid tiny/degenerate arenas.
                cfg.rows = std::max(5, rows);
                cfg.cols = std::max(5, cols);
            }
        } else if (key == "Max_Rounds") {
            int rounds = cfg.max_rounds;
            if (value_stream >> rounds) {
                // Ensure game loop always has at least one round.
                cfg.max_rounds = std::max(1, rounds);
            }
        } else if (key == "Sleep_interval") {
            double interval = cfg.sleep_interval;
            if (value_stream >> interval) {
                // Prevent negative sleep durations.
                cfg.sleep_interval = std::max(0.0, interval);
            }
        } else if (key == "Game_State_Live") {
            cfg.game_state_live = parse_bool(value, cfg.game_state_live);
        } else if (key == "Flamethrowers") {
            int count = cfg.flamethrowers;
            if (value_stream >> count) {
                // Obstacle counts cannot be negative.
                cfg.flamethrowers = std::max(0, count);
            }
        } else if (key == "Pits") {
            int count = cfg.pits;
            if (value_stream >> count) {
                cfg.pits = std::max(0, count);
            }
        } else if (key == "Mounds") {
            int count = cfg.mounds;
            if (value_stream >> count) {
                cfg.mounds = std::max(0, count);
            }
        }
    }

    return cfg;
}

std::vector<std::string> Arena::discover_robot_sources()
{
    std::vector<std::string> out;
    std::set<std::string> seen;

    auto scan_dir = [&](const fs::path& dir) {
        // Skip directories that don't exist so this works in both layouts.
        if (!fs::exists(dir) || !fs::is_directory(dir)) {
            return;
        }

        for (const auto& entry : fs::directory_iterator(dir)) {
            // Ignore things like symlinks, devices...
            if (!entry.is_regular_file()) {
                continue;
            }

            const fs::path p = entry.path();
            const std::string filename = p.filename().string();
            if (p.extension() != ".cpp") {
                continue;
            }
            if (filename.rfind("Robot_", 0) != 0) {
                continue;
            }

            const std::string abs_path = fs::absolute(p).string();
            // Deduplicate in case files are found through multiple scan roots.
            if (seen.insert(abs_path).second) {
                out.push_back(abs_path);
            }
        }
    };

    // Support either flat repo layout or ./robots subfolder layout.
    scan_dir(".");
    scan_dir("robots");

    // Stable ordering keeps robot load behavior deterministic.
    std::sort(out.begin(), out.end());
    return out;
}

bool Arena::load_robots()
{
    // Clear any old loaded robots first.
    for (auto& entry : robots_) {
        delete entry.robot;
        entry.robot = nullptr;
        if (entry.handle) {
            dlclose(entry.handle);
            entry.handle = nullptr;
        }
    }
    robots_.clear();

    std::vector<std::string> robot_files = discover_robot_sources();
    if (robot_files.empty()) {
        std::cerr << "No Robot_*.cpp files found.\n";
        return false;
    }

    const std::string symbols = "!@#$%^&*ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    int symbol_index = 0;

    std::cout << "Loading Robots...\n";
    for (const std::string& file : robot_files) {
        // file = "/path/Robot_Ratboy.cpp" -> stem = Robot_Ratboy
        std::string stem = fs::path(file).stem().string();
        std::string lib_name = "lib" + stem + ".so";

        // Compile robot source into shared library.
        std::string compile_cmd =
            "g++ -shared -fPIC -o \"" + lib_name + "\" \"" + file + "\" RobotBase.o -I. -std=c++20";
        std::cout << "Compiling " << file << " to " << lib_name << "...\n";
        if (std::system(compile_cmd.c_str()) != 0) {
            std::cerr << "  Compile failed, skipping.\n";
            continue;
        }

        // Open shared library and find required exports.
        void* handle = dlopen(("./" + lib_name).c_str(), RTLD_NOW);
        if (!handle) {
            std::cerr << "  Failed to load " << lib_name << ": " << dlerror() << '\n';
            continue;
        }

        RobotFactory create_robot = (RobotFactory)dlsym(handle, "create_robot");
        const char* (*robot_summary)() = (const char* (*)())dlsym(handle, "robot_summary");
        if (!create_robot || !robot_summary) {
            std::cerr << "  Missing create_robot or robot_summary, skipping.\n";
            dlclose(handle);
            continue;
        }

        // Validate summary string (required by your grading rules).
        const char* summary = robot_summary();
        if (!summary) {
            std::cerr << "  robot_summary returned null, skipping.\n";
            dlclose(handle);
            continue;
        }
        std::size_t summary_len = std::strlen(summary);
        if (summary_len == 0 || summary_len > 50) {
            std::cerr << "  robot_summary length must be 1-50, skipping.\n";
            dlclose(handle);
            continue;
        }

        // Create robot object and store in arena list.
        RobotBase* robot = create_robot();
        if (!robot) {
            std::cerr << "  create_robot failed, skipping.\n";
            dlclose(handle);
            continue;
        }

        robot->set_boundaries(config_.rows, config_.cols);
        robot->m_character = symbols[static_cast<std::size_t>(symbol_index % static_cast<int>(symbols.size()))];
        symbol_index++;

        RobotEntry entry;
        entry.source_file = file;
        entry.shared_lib = lib_name;
        entry.handle = handle;
        entry.robot = robot;
        entry.symbol = robot->m_character;
        robots_.push_back(entry);

        std::cout << "  Loaded " << robot->m_name << " " << robot->m_character << " from " << file << '\n';
    }

    if (robots_.empty()) {
        std::cerr << "No robots loaded successfully.\n";
        return false;
    }

    std::cout << "Loaded " << robots_.size() << " robot(s).\n";
    return true;
}

void Arena::place_obstacles(char obstacle_type, int count)
{
    // Randomly place a specific obstacle type on empty cells.
    std::uniform_int_distribution<int> row_dist(0, config_.rows - 1);
    std::uniform_int_distribution<int> col_dist(0, config_.cols - 1);

    int placed = 0;
    int attempts = 0;
    const int max_attempts = std::max(1000, config_.rows * config_.cols * 5);

    while (placed < count && attempts < max_attempts) {
        ++attempts;
        const int r = row_dist(rng_);
        const int c = col_dist(rng_);
        if (obstacle_board_[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] != '.') {
            continue;
        }
        obstacle_board_[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] = obstacle_type;
        ++placed;
    }
}

bool Arena::in_bounds(int row, int col) const
{
    return row >= 0 && row < config_.rows && col >= 0 && col < config_.cols;
}

bool Arena::initialize_board()
{
    // Reset board and place configured obstacles.
    obstacle_board_.assign(static_cast<std::size_t>(config_.rows),
                           std::string(static_cast<std::size_t>(config_.cols), '.'));
    place_obstacles('F', config_.flamethrowers);
    place_obstacles('P', config_.pits);
    place_obstacles('M', config_.mounds);

    // Place each loaded robot on a random empty (non-obstacle, unoccupied) cell.
    std::uniform_int_distribution<int> row_dist(0, config_.rows - 1);
    std::uniform_int_distribution<int> col_dist(0, config_.cols - 1);

    auto robot_at_cell = [&](int row, int col, bool include_dead, int exclude_idx) -> int {
        for (int i = 0; i < static_cast<int>(robots_.size()); ++i) {
            if (i == exclude_idx) {
                continue;
            }
            RobotBase* rb = robots_[static_cast<std::size_t>(i)].robot;
            if (!rb) {
                continue;
            }
            if (!include_dead && rb->get_health() <= 0) {
                continue;
            }
            int rr = 0;
            int cc = 0;
            rb->get_current_location(rr, cc);
            if (rr == row && cc == col) {
                return i;
            }
        }
        return -1;
    };

    for (auto& entry : robots_) {
        if (!entry.robot) {
            continue;
        }

        bool placed = false;
        const int max_attempts = std::max(1000, config_.rows * config_.cols * 5);
        for (int attempt = 0; attempt < max_attempts; ++attempt) {
            const int r = row_dist(rng_);
            const int c = col_dist(rng_);
            if (obstacle_board_[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] != '.') {
                continue;
            }
            if (robot_at_cell(r, c, true, -1) != -1) {
                continue;
            }

            entry.robot->move_to(r, c);
            std::cout << "Loaded robot: " << entry.robot->m_name << " at (" << r << ", " << c << ")\n";
            placed = true;
            break;
        }

        if (!placed) {
            std::cerr << "Could not place robot " << entry.source_file << " on the board.\n";
            return false;
        }
    }

    return true;
}

void Arena::print_board() const
{
    // Header row (column numbers).
    std::cout << "    ";
    for (int c = 0; c < config_.cols; ++c) {
        std::cout << std::setw(2) << c << ' ';
    }
    std::cout << '\n';

    auto robot_at_cell = [&](int row, int col, bool include_dead, int exclude_idx) -> int {
        for (int i = 0; i < static_cast<int>(robots_.size()); ++i) {
            if (i == exclude_idx) {
                continue;
            }
            RobotBase* rb = robots_[static_cast<std::size_t>(i)].robot;
            if (!rb) {
                continue;
            }
            if (!include_dead && rb->get_health() <= 0) {
                continue;
            }
            int rr = 0;
            int cc = 0;
            rb->get_current_location(rr, cc);
            if (rr == row && cc == col) {
                return i;
            }
        }
        return -1;
    };

    // Board body.
    for (int r = 0; r < config_.rows; ++r) {
        std::cout << std::setw(3) << r << ' ';
        for (int c = 0; c < config_.cols; ++c) {
            std::string token = " .";

            const int robot_idx = robot_at_cell(r, c, true, -1);
            if (robot_idx != -1) {
                RobotBase* rb = robots_[static_cast<std::size_t>(robot_idx)].robot;
                const char prefix = (rb->get_health() > 0) ? 'R' : 'X';
                token = std::string(1, prefix) + robots_[static_cast<std::size_t>(robot_idx)].symbol;
            } else {
                const char obstacle = obstacle_board_[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)];
                if (obstacle != '.') {
                    token = std::string(" ") + obstacle;
                }
            }
            std::cout << token << ' ';
        }
        std::cout << '\n';
    }
}

void Arena::run_simulation()
{
    if (robots_.empty()) {
        std::cout << "No robots loaded. Nothing to simulate.\n";
        return;
    }

    // Helper: find robot index at a specific cell.
    auto robot_at_cell = [&](int row, int col, bool include_dead, int exclude_idx) -> int {
        for (int i = 0; i < static_cast<int>(robots_.size()); ++i) {
            if (i == exclude_idx) {
                continue;
            }
            RobotBase* rb = robots_[static_cast<std::size_t>(i)].robot;
            if (!rb) {
                continue;
            }
            if (!include_dead && rb->get_health() <= 0) {
                continue;
            }
            int rr = 0;
            int cc = 0;
            rb->get_current_location(rr, cc);
            if (rr == row && cc == col) {
                return i;
            }
        }
        return -1;
    };

    // Helper: count living robots and optionally return winner index.
    auto winner_index = [&]() -> int {
        int alive_count = 0;
        int winner = -1;
        for (int i = 0; i < static_cast<int>(robots_.size()); ++i) {
            RobotBase* rb = robots_[static_cast<std::size_t>(i)].robot;
            if (rb && rb->get_health() > 0) {
                ++alive_count;
                winner = i;
            }
        }
        return (alive_count == 1) ? winner : -1;
    };

    // Helper: convert board content into radar type.
    auto radar_cell_type = [&](int row, int col, int observer_idx) -> char {
        int idx = robot_at_cell(row, col, true, observer_idx);
        if (idx != -1) {
            return (robots_[static_cast<std::size_t>(idx)].robot->get_health() > 0) ? 'R' : 'X';
        }
        return obstacle_board_[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)];
    };

    // Helper: radar scan logic (direction 0 = 8 neighbors, 1-8 = 3-wide ray).
    auto perform_radar_scan = [&](int robot_idx, int direction) {
        std::vector<RadarObj> out;
        int row = 0;
        int col = 0;
        robots_[static_cast<std::size_t>(robot_idx)].robot->get_current_location(row, col);

        if (direction == 0) {
            for (int dr = -1; dr <= 1; ++dr) {
                for (int dc = -1; dc <= 1; ++dc) {
                    if (dr == 0 && dc == 0) {
                        continue;
                    }
                    int rr = row + dr;
                    int cc = col + dc;
                    if (!in_bounds(rr, cc)) {
                        continue;
                    }
                    char type = radar_cell_type(rr, cc, robot_idx);
                    if (type != '.') {
                        out.emplace_back(type, rr, cc);
                    }
                }
            }
            return out;
        }

        if (direction < 1 || direction > 8) {
            return out;
        }

        int fdr = directions[direction].first;
        int fdc = directions[direction].second;
        int pdr = -fdc;
        int pdc = fdr;

        for (int step = 1;; ++step) {
            bool any_in_bounds = false;
            for (int lateral = -1; lateral <= 1; ++lateral) {
                int rr = row + (fdr * step) + (pdr * lateral);
                int cc = col + (fdc * step) + (pdc * lateral);
                if (!in_bounds(rr, cc)) {
                    continue;
                }
                any_in_bounds = true;
                char type = radar_cell_type(rr, cc, robot_idx);
                if (type != '.') {
                    out.emplace_back(type, rr, cc);
                }
            }
            if (!any_in_bounds) {
                break;
            }
        }

        return out;
    };

    // Helper: roll random damage in [low, high].
    auto random_damage = [&](int low, int high) {
        std::uniform_int_distribution<int> dist(low, high);
        return dist(rng_);
    };

    // Helper: apply damage with armor scaling, then reduce armor by 1.
    auto damage_robot = [&](int target_idx, int raw_damage, const std::string& source_name) -> std::string {
        RobotBase* target = robots_[static_cast<std::size_t>(target_idx)].robot;
        if (!target || target->get_health() <= 0) {
            return "";
        }

        int armor = target->get_armor();
        double scale = std::max(0.0, 1.0 - 0.1 * static_cast<double>(armor));
        int reduced = std::max(0, static_cast<int>(std::lround(raw_damage * scale)));
        int health_after = target->take_damage(reduced);
        target->reduce_armor(1);

        std::ostringstream os;
        os << source_name << " hits "
           << target->m_name << " " << robots_[static_cast<std::size_t>(target_idx)].symbol
           << " for " << reduced << " (raw " << raw_damage << "). HP: " << health_after;
        return os.str();
    };

    for (int round = 0; round < config_.max_rounds; ++round) {
        std::vector<std::string> round_events;

        if (!config_.game_state_live) {
            std::cout << "\n=========== starting round " << round << " ===========\n";
            print_board();
        }

        for (int i = 0; i < static_cast<int>(robots_.size()); ++i) {
            int winner = winner_index();
            if (winner != -1) {
                std::cout << "\nWinner: " << robots_[static_cast<std::size_t>(winner)].robot->m_name
                          << " " << robots_[static_cast<std::size_t>(winner)].symbol << '\n';
                return;
            }

            RobotBase* actor = robots_[static_cast<std::size_t>(i)].robot;
            if (!actor || actor->get_health() <= 0) {
                continue;
            }

            if (!config_.game_state_live) {
                std::cout << "\n" << actor->m_name << " " << robots_[static_cast<std::size_t>(i)].symbol
                          << " begins turn.\n";
            }

            const std::string actor_tag =
                actor->m_name + " " + robots_[static_cast<std::size_t>(i)].symbol;

            // 1) Radar direction -> 2) scan -> 3) process radar.
            int radar_dir = 0;
            actor->get_radar_direction(radar_dir);
            if (radar_dir < 0 || radar_dir > 8) {
                radar_dir = 0;
            }
            std::vector<RadarObj> radar = perform_radar_scan(i, radar_dir);
            actor->process_radar_results(radar);

            // 4) Shoot OR move (never both).
            int shot_row = 0;
            int shot_col = 0;
            if (actor->get_shot_location(shot_row, shot_col)) {
                WeaponType weapon = actor->get_weapon();
                int actor_row = 0;
                int actor_col = 0;
                actor->get_current_location(actor_row, actor_col);

                int dr = signum(shot_row - actor_row);
                int dc = signum(shot_col - actor_col);
                std::set<int> hit_targets;
                int raw_damage = 0;

                if (weapon == railgun) {
                    raw_damage = random_damage(10, 20);
                    for (int step = 1;; ++step) {
                        int rr = actor_row + dr * step;
                        int cc = actor_col + dc * step;
                        if (!in_bounds(rr, cc)) {
                            break;
                        }
                        int target = robot_at_cell(rr, cc, false, i);
                        if (target != -1) {
                            hit_targets.insert(target);
                        }
                    }
                } else if (weapon == hammer) {
                    raw_damage = random_damage(50, 60);
                    int rr = actor_row + dr;
                    int cc = actor_col + dc;
                    if (in_bounds(rr, cc)) {
                        int target = robot_at_cell(rr, cc, false, i);
                        if (target != -1) {
                            hit_targets.insert(target);
                        }
                    }
                } else if (weapon == grenade) {
                    if (actor->get_grenades() > 0) {
                        actor->decrement_grenades();
                        raw_damage = random_damage(10, 40);
                        for (int rr = shot_row - 1; rr <= shot_row + 1; ++rr) {
                            for (int cc = shot_col - 1; cc <= shot_col + 1; ++cc) {
                                if (!in_bounds(rr, cc)) {
                                    continue;
                                }
                                int target = robot_at_cell(rr, cc, false, i);
                                if (target != -1) {
                                    hit_targets.insert(target);
                                }
                            }
                        }
                    } else {
                        round_events.push_back(actor_tag + " tried to fire grenade but has none left.");
                    }
                } else {  // flamethrower
                    raw_damage = random_damage(30, 50);
                    int pdr = -dc;
                    int pdc = dr;
                    for (int step = 1; step <= 4; ++step) {
                        for (int lateral = -1; lateral <= 1; ++lateral) {
                            int rr = actor_row + dr * step + pdr * lateral;
                            int cc = actor_col + dc * step + pdc * lateral;
                            if (!in_bounds(rr, cc)) {
                                continue;
                            }
                            int target = robot_at_cell(rr, cc, false, i);
                            if (target != -1) {
                                hit_targets.insert(target);
                            }
                        }
                    }
                }

                if (hit_targets.empty()) {
                    round_events.push_back(actor_tag + " shoots but hits nothing.");
                } else {
                    for (int target : hit_targets) {
                        std::string msg = damage_robot(target, raw_damage, actor_tag);
                        if (!msg.empty()) {
                            round_events.push_back(msg);
                        }
                    }
                }
            } else {
                int move_dir = 0;
                int move_dist = 0;
                actor->get_move_direction(move_dir, move_dist);

                if (move_dir < 1 || move_dir > 8 || move_dist <= 0) {
                    round_events.push_back(actor_tag + " does not move.");
                    continue;
                }

                move_dist = std::min(move_dist, actor->get_move_speed());
                int dr = directions[move_dir].first;
                int dc = directions[move_dir].second;
                int row = 0;
                int col = 0;
                actor->get_current_location(row, col);

                int cur_row = row;
                int cur_col = col;
                for (int step = 0; step < move_dist; ++step) {
                    int next_row = cur_row + dr;
                    int next_col = cur_col + dc;
                    if (!in_bounds(next_row, next_col)) {
                        break;
                    }

                    // Robots (alive or dead) block movement.
                    if (robot_at_cell(next_row, next_col, true, i) != -1) {
                        break;
                    }

                    char obstacle = obstacle_board_[static_cast<std::size_t>(next_row)][static_cast<std::size_t>(next_col)];
                    if (obstacle == 'M') {
                        break;
                    }

                    actor->move_to(next_row, next_col);
                    cur_row = next_row;
                    cur_col = next_col;

                    if (obstacle == 'P') {
                        actor->disable_movement();
                        round_events.push_back(actor_tag + " falls into a pit and is trapped.");
                        break;
                    }

                    if (obstacle == 'F') {
                        std::string msg = damage_robot(i, random_damage(30, 50), "Arena flamethrower");
                        if (!msg.empty()) {
                            round_events.push_back(msg);
                        }
                        // Spec: if robot dies while on F, dead robot occupies tile and F disappears.
                        if (actor->get_health() <= 0) {
                            obstacle_board_[static_cast<std::size_t>(cur_row)][static_cast<std::size_t>(cur_col)] = '.';
                            break;
                        }
                    }
                }

                int final_row = 0;
                int final_col = 0;
                actor->get_current_location(final_row, final_col);
                if (final_row == row && final_col == col) {
                    round_events.push_back(actor_tag + " could not move.");
                } else {
                    std::ostringstream os;
                    os << actor_tag << " moves to (" << final_row << ", " << final_col << ").";
                    round_events.push_back(os.str());
                }
            }
        }

        if (config_.game_state_live) {
            clear_terminal_screen();
            std::cout << "=========== starting round " << round << " ===========\n";
            print_board();
            for (const std::string& event : round_events) {
                std::cout << "- " << event << '\n';
            }
            if (config_.sleep_interval > 0.0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(static_cast<int>(config_.sleep_interval * 1000.0)));
            }
        } else {
            for (const std::string& event : round_events) {
                std::cout << "- " << event << '\n';
            }
        }
    }

    int winner = -1;
    int alive_count = 0;
    for (int i = 0; i < static_cast<int>(robots_.size()); ++i) {
        RobotBase* rb = robots_[static_cast<std::size_t>(i)].robot;
        if (rb && rb->get_health() > 0) {
            ++alive_count;
            winner = i;
        }
    }

    if (alive_count == 1) {
        std::cout << "\nWinner: " << robots_[static_cast<std::size_t>(winner)].robot->m_name
                  << " " << robots_[static_cast<std::size_t>(winner)].symbol << '\n';
    } else {
        std::cout << "\nReached max rounds (" << config_.max_rounds << "). No winner.\n";
    }
}
