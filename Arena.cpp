#include "Arena.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <cstring>
#include <thread>

namespace fs = std::filesystem;

namespace {
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

bool parse_bool(std::string value, bool default_value)
{
    value = trim(value);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (value == "true" || value == "1" || value == "yes" || value == "live" || value == "on") {
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

constexpr std::size_t kMaxRobotSummaryChars = 50;
}  // namespace

Arena::Arena(const std::string& config_file)
    : Arena(load_config(config_file))
{
}

Arena::Arena(GameConfig cfg)
    : config_(cfg),
      obstacle_board_(cfg.rows, std::string(cfg.cols, '.')),
      rng_(std::random_device{}())
{
}

Arena::~Arena()
{
    for (auto& entry : robots_) {
        delete entry.robot;
        entry.robot = nullptr;
        if (entry.handle) {
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
        std::cerr << "Warning: could not open config '" << filename << "', using defaults.\n";
        return cfg;
    }

    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        std::size_t split_pos = line.find(':');
        if (split_pos == std::string::npos) {
            split_pos = line.find('=');
        }
        if (split_pos == std::string::npos) {
            continue;
        }

        const std::string key = trim(line.substr(0, split_pos));
        const std::string value = trim(line.substr(split_pos + 1));
        std::istringstream value_stream(value);

        if (key == "Arena_Size") {
            int h = cfg.rows;
            int w = cfg.cols;
            if (value_stream >> h >> w) {
                cfg.rows = std::max(5, h);
                cfg.cols = std::max(5, w);
            }
        } else if (key == "ArenaSize") {
            int h = cfg.rows;
            int w = cfg.cols;
            char comma = ',';
            if (value_stream >> h >> comma >> w) {
                cfg.rows = std::max(5, h);
                cfg.cols = std::max(5, w);
            }
        } else if (key == "Max_Rounds" || key == "MaxRounds") {
            int rounds = cfg.max_rounds;
            if (value_stream >> rounds) {
                cfg.max_rounds = std::max(1, rounds);
            }
        } else if (key == "Sleep_interval") {
            double interval = cfg.sleep_interval;
            if (value_stream >> interval) {
                cfg.sleep_interval = std::max(0.0, interval);
            }
        } else if (key == "Game_State_Live" || key == "GameMode") {
            cfg.game_state_live = parse_bool(value, cfg.game_state_live);
        } else if (key == "Flamethrowers") {
            int count = cfg.flamethrowers;
            if (value_stream >> count) {
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
        } else if (key == "ObstacleDensity") {
            std::string density = value;
            std::transform(density.begin(), density.end(), density.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (density == "low") {
                cfg.flamethrowers = 2;
                cfg.pits = 2;
                cfg.mounds = 2;
            } else if (density == "high") {
                cfg.flamethrowers = 6;
                cfg.pits = 6;
                cfg.mounds = 6;
            } else {
                cfg.flamethrowers = 4;
                cfg.pits = 4;
                cfg.mounds = 4;
            }
        }
    }

    return cfg;
}

std::vector<fs::path> Arena::discover_robot_sources() const
{
    std::vector<fs::path> out;
    std::set<std::string> seen;

    auto collect_from_dir = [&](const fs::path& dir) {
        if (!fs::exists(dir) || !fs::is_directory(dir)) {
            return;
        }
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const fs::path p = entry.path();
            const std::string filename = p.filename().string();
            if (p.extension() == ".cpp" && filename.rfind("Robot_", 0) == 0) {
                const std::string key = fs::absolute(p).string();
                if (seen.insert(key).second) {
                    out.push_back(p);
                }
            }
        }
    };

    collect_from_dir("robots");
    collect_from_dir(".");
    std::sort(out.begin(), out.end());
    return out;
}

bool Arena::load_robots()
{
    using RobotSummaryFn = const char* (*)();

    const std::vector<fs::path> sources = discover_robot_sources();
    if (sources.empty()) {
        std::cerr << "No robot sources found (expected Robot_*.cpp in ./robots or current directory).\n";
        return false;
    }

    const std::string symbols = "!@#$%^&*ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    int symbol_idx = 0;

    std::cout << "Loading Robots...\n";
    for (const auto& src : sources) {
        const std::string source_name = src.string();
        const std::string stem = src.stem().string();
        const std::string lib_name = "lib" + stem + ".so";
        const std::string compile_cmd =
            "g++ -shared -fPIC -o \"" + lib_name + "\" \"" + source_name + "\" RobotBase.o -I. -std=c++20";

        std::cout << "Compiling " << source_name << " to " << lib_name << "...\n";
        if (std::system(compile_cmd.c_str()) != 0) {
            std::cerr << "Skipping " << source_name << " due to compile failure.\n";
            continue;
        }

        void* handle = dlopen(("./" + lib_name).c_str(), RTLD_NOW);
        if (!handle) {
            std::cerr << "Failed to load " << lib_name << ": " << dlerror() << '\n';
            continue;
        }

        RobotFactory create_robot = reinterpret_cast<RobotFactory>(dlsym(handle, "create_robot"));
        if (!create_robot) {
            std::cerr << "Failed to find create_robot in " << lib_name << ": " << dlerror() << '\n';
            dlclose(handle);
            continue;
        }

        RobotSummaryFn robot_summary = reinterpret_cast<RobotSummaryFn>(dlsym(handle, "robot_summary"));
        if (!robot_summary) {
            std::cerr << "Failed to find required robot_summary in " << lib_name << ": " << dlerror() << '\n';
            dlclose(handle);
            continue;
        }
        const char* summary_cstr = robot_summary();
        if (!summary_cstr) {
            std::cerr << "robot_summary returned null for " << source_name << '\n';
            dlclose(handle);
            continue;
        }
        const std::size_t summary_len = std::strlen(summary_cstr);
        if (summary_len == 0 || summary_len > kMaxRobotSummaryChars) {
            std::cerr << "Invalid robot_summary length (" << summary_len << ") for " << source_name
                      << ". Required: 1-" << kMaxRobotSummaryChars << " chars.\n";
            dlclose(handle);
            continue;
        }

        RobotBase* bot = create_robot();
        if (!bot) {
            std::cerr << "create_robot returned null for " << source_name << '\n';
            dlclose(handle);
            continue;
        }

        bot->set_boundaries(config_.rows, config_.cols);
        bot->m_character = symbols[symbol_idx % static_cast<int>(symbols.size())];
        ++symbol_idx;
        if (bot->m_name == "Blank_Robot") {
            bot->m_name = stem;
        }

        RobotEntry entry;
        entry.source_file = source_name;
        entry.shared_lib = lib_name;
        entry.handle = handle;
        entry.robot = bot;
        entry.summary = std::string(summary_cstr);
        entry.symbol = bot->m_character;
        robots_.push_back(entry);
    }

    if (robots_.empty()) {
        std::cerr << "No robots were loaded successfully.\n";
        return false;
    }

    return true;
}

std::optional<std::pair<int, int>> Arena::random_empty_cell()
{
    std::uniform_int_distribution<int> row_dist(0, config_.rows - 1);
    std::uniform_int_distribution<int> col_dist(0, config_.cols - 1);

    const int attempts = std::max(1000, config_.rows * config_.cols * 3);
    for (int i = 0; i < attempts; ++i) {
        const int r = row_dist(rng_);
        const int c = col_dist(rng_);
        if (obstacle_board_[r][c] != '.') {
            continue;
        }
        if (robot_at(r, c, true, -1) != -1) {
            continue;
        }
        return std::make_pair(r, c);
    }

    for (int r = 0; r < config_.rows; ++r) {
        for (int c = 0; c < config_.cols; ++c) {
            if (obstacle_board_[r][c] == '.' && robot_at(r, c, true, -1) == -1) {
                return std::make_pair(r, c);
            }
        }
    }
    return std::nullopt;
}

void Arena::place_obstacles(char obstacle_type, int count)
{
    for (int i = 0; i < count; ++i) {
        auto pos = random_empty_cell();
        if (!pos.has_value()) {
            return;
        }
        obstacle_board_[pos->first][pos->second] = obstacle_type;
    }
}

bool Arena::initialize_board()
{
    place_obstacles('F', config_.flamethrowers);
    place_obstacles('P', config_.pits);
    place_obstacles('M', config_.mounds);

    for (auto& entry : robots_) {
        auto pos = random_empty_cell();
        if (!pos.has_value()) {
            std::cerr << "Board is full; could not place all robots.\n";
            return false;
        }
        entry.robot->move_to(pos->first, pos->second);
        std::cout << "Loaded robot: " << entry.robot->m_name << " at (" << pos->first << ", " << pos->second
                  << ")\n";
    }
    return true;
}

bool Arena::in_bounds(int row, int col) const
{
    return row >= 0 && row < config_.rows && col >= 0 && col < config_.cols;
}

int Arena::robot_at(int row, int col, bool include_dead, int exclude_idx) const
{
    for (int idx = 0; idx < static_cast<int>(robots_.size()); ++idx) {
        if (idx == exclude_idx) {
            continue;
        }
        const RobotEntry& entry = robots_[idx];
        if (!entry.robot) {
            continue;
        }
        if (!include_dead && entry.robot->get_health() <= 0) {
            continue;
        }
        int rr = -1;
        int cc = -1;
        entry.robot->get_current_location(rr, cc);
        if (rr == row && cc == col) {
            return idx;
        }
    }
    return -1;
}

char Arena::radar_cell_type(int row, int col, int observer_idx) const
{
    const int robot_idx = robot_at(row, col, true, observer_idx);
    if (robot_idx != -1) {
        return robots_[robot_idx].robot->get_health() > 0 ? 'R' : 'X';
    }
    return obstacle_board_[row][col];
}

std::vector<RadarObj> Arena::perform_radar_scan(int robot_idx, int direction) const
{
    std::vector<RadarObj> out;
    int row = 0;
    int col = 0;
    robots_[robot_idx].robot->get_current_location(row, col);

    if (direction == 0) {
        for (int dr = -1; dr <= 1; ++dr) {
            for (int dc = -1; dc <= 1; ++dc) {
                if (dr == 0 && dc == 0) {
                    continue;
                }
                const int rr = row + dr;
                const int cc = col + dc;
                if (!in_bounds(rr, cc)) {
                    continue;
                }
                const char type = radar_cell_type(rr, cc, robot_idx);
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

    const int fdr = directions[direction].first;
    const int fdc = directions[direction].second;
    const int pdr = -fdc;
    const int pdc = fdr;

    for (int step = 1;; ++step) {
        bool saw_in_bounds = false;
        for (int lateral = -1; lateral <= 1; ++lateral) {
            const int rr = row + (fdr * step) + (pdr * lateral);
            const int cc = col + (fdc * step) + (pdc * lateral);
            if (!in_bounds(rr, cc)) {
                continue;
            }
            saw_in_bounds = true;
            const char type = radar_cell_type(rr, cc, robot_idx);
            if (type != '.') {
                out.emplace_back(type, rr, cc);
            }
        }
        if (!saw_in_bounds) {
            break;
        }
    }

    return out;
}

int Arena::random_damage(int low, int high)
{
    std::uniform_int_distribution<int> dist(low, high);
    return dist(rng_);
}

int Arena::apply_armor(int raw_damage, int armor) const
{
    const double scale = std::max(0.0, 1.0 - (0.1 * static_cast<double>(armor)));
    return std::max(0, static_cast<int>(std::lround(static_cast<double>(raw_damage) * scale)));
}

std::string Arena::damage_robot(int target_idx, int raw_damage, const std::string& source_name)
{
    RobotBase* target = robots_[target_idx].robot;
    if (!target || target->get_health() <= 0) {
        return "";
    }

    const int armor = target->get_armor();
    const int reduced_damage = apply_armor(raw_damage, armor);
    const int health_after = target->take_damage(reduced_damage);
    target->reduce_armor(1);
    if (health_after <= 0 && robots_[target_idx].death_reason.empty()) {
        robots_[target_idx].death_reason = "Destroyed by " + source_name;
    }

    std::ostringstream os;
    os << source_name << " hits " << target->m_name << " for " << reduced_damage
       << " (raw " << raw_damage << "). HP: " << health_after;
    return os.str();
}

std::vector<std::string> Arena::handle_shot(int shooter_idx, int shot_row, int shot_col)
{
    std::vector<std::string> events;
    RobotEntry& shooter = robots_[shooter_idx];
    RobotBase* bot = shooter.robot;
    const WeaponType weapon = bot->get_weapon();

    int shooter_row = 0;
    int shooter_col = 0;
    bot->get_current_location(shooter_row, shooter_col);

    std::vector<std::pair<int, int>> affected_cells;
    int raw_damage = 0;

    const int dr = signum(shot_row - shooter_row);
    const int dc = signum(shot_col - shooter_col);

    if (weapon == railgun) {
        if (dr == 0 && dc == 0) {
            events.push_back(bot->m_name + " railgun shot ignored (same-cell target).");
            return events;
        }
        raw_damage = random_damage(10, 20);
        for (int step = 1;; ++step) {
            const int rr = shooter_row + (dr * step);
            const int cc = shooter_col + (dc * step);
            if (!in_bounds(rr, cc)) {
                break;
            }
            affected_cells.emplace_back(rr, cc);
        }
        events.push_back(bot->m_name + " fires railgun toward (" + std::to_string(shot_row) + "," +
                         std::to_string(shot_col) + ").");
    } else if (weapon == hammer) {
        if (dr == 0 && dc == 0) {
            events.push_back(bot->m_name + " hammer shot ignored (same-cell target).");
            return events;
        }
        raw_damage = random_damage(50, 60);
        const int rr = shooter_row + dr;
        const int cc = shooter_col + dc;
        if (in_bounds(rr, cc)) {
            affected_cells.emplace_back(rr, cc);
        }
        events.push_back(bot->m_name + " swings hammer at (" + std::to_string(rr) + "," + std::to_string(cc) +
                         ").");
    } else if (weapon == grenade) {
        if (shooter.grenades_fired >= 10 || bot->get_grenades() <= 0) {
            events.push_back(bot->m_name + " tried grenade but has none left.");
            return events;
        }
        raw_damage = random_damage(10, 40);
        for (int rr = shot_row - 1; rr <= shot_row + 1; ++rr) {
            for (int cc = shot_col - 1; cc <= shot_col + 1; ++cc) {
                if (in_bounds(rr, cc)) {
                    affected_cells.emplace_back(rr, cc);
                }
            }
        }
        bot->decrement_grenades();
        shooter.grenades_fired++;
        events.push_back(bot->m_name + " launches grenade at (" + std::to_string(shot_row) + "," +
                         std::to_string(shot_col) + ").");
    } else {
        if (dr == 0 && dc == 0) {
            events.push_back(bot->m_name + " flamethrower shot ignored (same-cell target).");
            return events;
        }
        raw_damage = random_damage(30, 50);
        const int pdr = -dc;
        const int pdc = dr;
        for (int step = 1; step <= 4; ++step) {
            for (int lateral = -1; lateral <= 1; ++lateral) {
                const int rr = shooter_row + (dr * step) + (pdr * lateral);
                const int cc = shooter_col + (dc * step) + (pdc * lateral);
                if (in_bounds(rr, cc)) {
                    affected_cells.emplace_back(rr, cc);
                }
            }
        }
        events.push_back(bot->m_name + " sprays flamethrower toward (" + std::to_string(shot_row) + "," +
                         std::to_string(shot_col) + ").");
    }

    std::set<int> hit_targets;
    for (const auto& cell : affected_cells) {
        const int target_idx = robot_at(cell.first, cell.second, false, shooter_idx);
        if (target_idx != -1) {
            hit_targets.insert(target_idx);
        }
    }

    if (hit_targets.empty()) {
        events.push_back("No robots were hit.");
        return events;
    }

    for (const int target_idx : hit_targets) {
        const std::string msg = damage_robot(target_idx, raw_damage, bot->m_name);
        if (!msg.empty()) {
            events.push_back(msg);
        }
    }

    return events;
}

std::vector<std::string> Arena::handle_movement(int robot_idx, int direction, int distance)
{
    std::vector<std::string> events;
    RobotEntry& mover = robots_[robot_idx];
    RobotBase* bot = mover.robot;

    if (distance <= 0 || direction < 1 || direction > 8) {
        events.push_back(bot->m_name + " does not move.");
        return events;
    }

    distance = std::min(distance, bot->get_move_speed());
    if (distance <= 0) {
        events.push_back(bot->m_name + " cannot move (speed is zero).");
        return events;
    }

    int row = 0;
    int col = 0;
    bot->get_current_location(row, col);
    int cur_row = row;
    int cur_col = col;

    const int dr = directions[direction].first;
    const int dc = directions[direction].second;

    bool changed = false;
    for (int step = 0; step < distance; ++step) {
        const int next_row = cur_row + dr;
        const int next_col = cur_col + dc;

        if (!in_bounds(next_row, next_col)) {
            break;
        }

        if (robot_at(next_row, next_col, true, robot_idx) != -1) {
            break;
        }

        const char obstacle = obstacle_board_[next_row][next_col];
        if (obstacle == 'M') {
            break;
        }

        cur_row = next_row;
        cur_col = next_col;
        bot->move_to(cur_row, cur_col);
        changed = true;

        if (obstacle == 'P') {
            bot->disable_movement();
            events.push_back(bot->m_name + " falls into pit at (" + std::to_string(cur_row) + "," +
                             std::to_string(cur_col) + ") and is trapped.");
            return events;
        }

        if (obstacle == 'F') {
            const std::string msg = damage_robot(robot_idx, random_damage(30, 50), "Arena flamethrower");
            if (!msg.empty()) {
                events.push_back(msg);
            }
            if (bot->get_health() <= 0) {
                obstacle_board_[cur_row][cur_col] = '.';
                events.push_back(bot->m_name + " was destroyed on flamethrower tile.");
                return events;
            }
        }
    }

    if (changed) {
        events.push_back(bot->m_name + " moves to (" + std::to_string(cur_row) + "," + std::to_string(cur_col) +
                         ").");
    } else {
        events.push_back(bot->m_name + " cannot move from (" + std::to_string(row) + "," + std::to_string(col) +
                         ").");
    }

    return events;
}

void Arena::print_robot_stats(const RobotEntry& entry) const
{
    int row = 0;
    int col = 0;
    entry.robot->get_current_location(row, col);

    std::cout << entry.robot->m_name << ":\n";
    std::cout << "  Health: " << entry.robot->get_health() << '\n';
    std::cout << "  Weapon: " << entry.robot->get_weapon() << '\n';
    std::cout << "  Armor: " << entry.robot->get_armor() << '\n';
    std::cout << "  Move Speed: " << entry.robot->get_move_speed() << '\n';
    std::cout << "  Location: (" << row << "," << col << ")\n";
}

void Arena::log_radar_results(const std::vector<RadarObj>& radar) const
{
    if (radar.empty()) {
        std::cout << "  checking radar ... found nothing.\n";
        return;
    }

    std::cout << "  checking radar ... found " << radar.size() << " object(s)";
    const std::size_t preview_count = std::min<std::size_t>(radar.size(), 3);
    for (std::size_t i = 0; i < preview_count; ++i) {
        std::cout << " [" << radar[i].m_type << " at (" << radar[i].m_row << "," << radar[i].m_col << ")]";
    }
    if (radar.size() > preview_count) {
        std::cout << " ...";
    }
    std::cout << '\n';
}

void Arena::print_live_hud(int round) const
{
    static constexpr std::string_view kReset = "\x1b[0m";
    static constexpr std::string_view kDeadRed = "\x1b[31m";

    int alive = 0;
    int leader_idx = -1;
    int leader_hp = -1;

    for (int i = 0; i < static_cast<int>(robots_.size()); ++i) {
        const int hp = robots_[i].robot->get_health();
        if (hp > 0) {
            alive++;
            if (hp > leader_hp) {
                leader_hp = hp;
                leader_idx = i;
            }
        }
    }

    std::cout << "RobotWarz Live  |  Round: " << round << "  |  Alive: " << alive << "/" << robots_.size()
              << "  |  Delay: " << config_.sleep_interval << "s\n";
    if (leader_idx >= 0) {
        std::cout << "Leader: " << robots_[leader_idx].robot->m_name << " (" << leader_hp << " HP)\n";
    }

    std::vector<const RobotEntry*> alive_entries;
    std::vector<const RobotEntry*> dead_entries;
    alive_entries.reserve(robots_.size());
    dead_entries.reserve(robots_.size());
    for (const auto& entry : robots_) {
        if (entry.robot->get_health() > 0) {
            alive_entries.push_back(&entry);
        } else {
            dead_entries.push_back(&entry);
        }
    }

    auto fit_cell = [](const std::string& text, std::size_t width) {
        if (width == 0) {
            return std::string{};
        }
        if (text.size() > width) {
            if (width == 1) {
                return std::string(1, '~');
            }
            return text.substr(0, width - 1) + "~";
        }
        return text + std::string(width - text.size(), ' ');
    };

    auto print_section_header = [&fit_cell](const std::string& title) {
        std::cout << title << '\n';
        std::cout << fit_cell("ID", 4) << " | "
                  << fit_cell("Name", 15) << " | "
                  << fit_cell("HP", 4) << " | "
                  << fit_cell("A", 3) << " | "
                  << fit_cell("M", 3) << " | "
                  << fit_cell("Pos", 9) << " | "
                  << "Details\n";
        std::cout << std::string(74, '-') << '\n';
    };

    auto print_empty_row = [&fit_cell]() {
        std::cout << fit_cell("--", 4) << " | "
                  << fit_cell("--", 15) << " | "
                  << fit_cell("--", 4) << " | "
                  << fit_cell("--", 3) << " | "
                  << fit_cell("--", 3) << " | "
                  << fit_cell("--", 9) << " | "
                  << "--\n";
    };

    print_section_header("Alive");
    for (std::size_t i = 0; i < robots_.size(); ++i) {
        if (i >= alive_entries.size()) {
            print_empty_row();
            continue;
        }
        const RobotEntry& entry = *alive_entries[i];
        int row = 0;
        int col = 0;
        entry.robot->get_current_location(row, col);
        const std::string id = std::string("R") + entry.symbol;
        std::ostringstream pos;
        pos << "(" << row << "," << col << ")";
        std::cout << colorize_token(fit_cell(id, 4)) << " | "
                  << fit_cell(entry.robot->m_name, 15) << " | ";
        std::cout << fit_cell(std::to_string(entry.robot->get_health()), 4) << " | "
                  << fit_cell(std::to_string(entry.robot->get_armor()), 3) << " | "
                  << fit_cell(std::to_string(entry.robot->get_move_speed()), 3) << " | "
                  << fit_cell(pos.str(), 9) << " | "
                  << entry.summary << '\n';
    }

    std::cout << "\n";
    print_section_header("Dead");
    for (std::size_t i = 0; i < robots_.size(); ++i) {
        if (i >= dead_entries.size()) {
            print_empty_row();
            continue;
        }
        const RobotEntry& entry = *dead_entries[i];
        int row = 0;
        int col = 0;
        entry.robot->get_current_location(row, col);
        const std::string id = std::string("X") + entry.symbol;
        std::ostringstream pos;
        pos << "(" << row << "," << col << ")";
        std::cout << kDeadRed
                  << fit_cell(id, 4) << " | "
                  << fit_cell(entry.robot->m_name, 15) << " | "
                  << fit_cell(std::to_string(entry.robot->get_health()), 4) << " | "
                  << fit_cell(std::to_string(entry.robot->get_armor()), 3) << " | "
                  << fit_cell(std::to_string(entry.robot->get_move_speed()), 3) << " | "
                  << fit_cell(pos.str(), 9) << " | "
                  << (entry.death_reason.empty() ? std::string("Destroyed") : entry.death_reason)
                  << kReset << '\n';
    }
    std::cout << '\n';
}

void Arena::print_frame_events(const std::vector<std::string>& frame_events) const
{
    std::cout << "\nFrame Events\n";
    std::cout << "------------\n";
    if (frame_events.empty()) {
        std::cout << "No actions this frame.\n";
        return;
    }
    for (const auto& raw_event : frame_events) {
        std::string event = raw_event;

        auto replace_all = [](std::string& text, const std::string& from, const std::string& to) {
            if (from.empty()) {
                return;
            }
            std::size_t start_pos = 0;
            while ((start_pos = text.find(from, start_pos)) != std::string::npos) {
                text.replace(start_pos, from.length(), to);
                start_pos += to.length();
            }
        };

        // Render robot names in the event log as the same colored symbol + name shown in HUD/board.
        for (const auto& entry : robots_) {
            const std::string token = std::string((entry.robot->get_health() > 0) ? "R" : "X") + entry.symbol;
            const std::string decorated = colorize_token(token) + " " + entry.robot->m_name;
            replace_all(event, entry.robot->m_name, decorated);
        }

        std::cout << "- " << event << "\n";
    }
}

std::string Arena::colorize_token(const std::string& token) const
{
    static constexpr std::string_view kReset = "\x1b[0m";
    static constexpr std::string_view kDim = "\x1b[2m";
    static constexpr std::string_view kDeadRed = "\x1b[31m";
    static constexpr std::string_view kYellow = "\x1b[33m";
    static constexpr std::string_view kRed = "\x1b[31m";
    static constexpr std::string_view kMagenta = "\x1b[35m";
    static constexpr std::string_view kRobotColors[] = {
        // High-contrast, non-green robot palette (truecolor).
        "\x1b[38;2;0;191;255m",   // Deep sky blue
        "\x1b[38;2;255;0;255m",   // Magenta
        "\x1b[38;2;255;69;0m",    // Orange red
        "\x1b[38;2;147;112;219m", // Medium purple
        "\x1b[38;2;255;215;0m",   // Gold
        "\x1b[38;2;255;20;147m",  // Deep pink
        "\x1b[38;2;30;144;255m",  // Dodger blue
        "\x1b[38;2;255;99;71m",   // Tomato
        "\x1b[38;2;186;85;211m",  // Orchid
        "\x1b[38;2;240;248;255m", // Alice blue
        "\x1b[38;2;255;140;0m",   // Dark orange
        "\x1b[38;2;138;43;226m"   // Blue violet
    };

    if (token == ".") {
        return std::string(kDim) + token + std::string(kReset);
    }
    if (token == "M") {
        return std::string(kYellow) + token + std::string(kReset);
    }
    if (token == "P") {
        return std::string(kMagenta) + token + std::string(kReset);
    }
    if (token == "F") {
        return std::string(kRed) + token + std::string(kReset);
    }
    if (token.size() >= 2 && token[0] == 'X') {
        return std::string(kDeadRed) + token + std::string(kReset);
    }
    if (token.size() >= 2 && token[0] == 'R') {
        const unsigned int idx = static_cast<unsigned int>(static_cast<unsigned char>(token[1])) %
                                 static_cast<unsigned int>(std::size(kRobotColors));
        return std::string(kRobotColors[idx]) + token + std::string(kReset);
    }
    return token;
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
            const int robot_idx = robot_at(r, c, true, -1);
            std::string token = ".";
            if (robot_idx != -1) {
                RobotBase* rb = robots_[robot_idx].robot;
                token = rb->get_health() > 0 ? "R" : "X";
                token.push_back(robots_[robot_idx].symbol);
            } else if (obstacle_board_[r][c] != '.') {
                token = std::string(1, obstacle_board_[r][c]);
            }

            std::string display = token;
            if (display.size() == 1) {
                display = " " + display;
            }

            if (config_.game_state_live) {
                std::cout << colorize_token(display) << ' ';
            } else {
                std::cout << display << ' ';
            }
        }
        std::cout << '\n';
    }
}

std::optional<int> Arena::winner_index() const
{
    int alive_count = 0;
    int winner = -1;
    for (int idx = 0; idx < static_cast<int>(robots_.size()); ++idx) {
        if (robots_[idx].robot->get_health() > 0) {
            alive_count++;
            winner = idx;
        }
    }
    if (alive_count == 1) {
        return winner;
    }
    return std::nullopt;
}

void Arena::announce_winner() const
{
    auto winner = winner_index();
    if (winner.has_value()) {
        std::cout << "\nWinner: " << robots_[*winner].robot->m_name << " " << robots_[*winner].symbol << '\n';
    }
}

void Arena::run_simulation()
{
    for (int round = 0; round < config_.max_rounds; ++round) {
        std::vector<std::string> frame_events;
        const std::string round_title =
            "=========== starting round " + std::to_string(round) + " ===========";
        const int board_width = 4 + (config_.cols * 3);
        const int banner_padding = std::max(0, (board_width - static_cast<int>(round_title.size())) / 2);

        if (!config_.game_state_live) {
            std::cout << '\n' << std::string(static_cast<std::size_t>(banner_padding), ' ') << round_title << '\n';
            print_board();
        }

        for (int idx = 0; idx < static_cast<int>(robots_.size()); ++idx) {
            if (winner_index().has_value()) {
                if (config_.game_state_live) {
                    clear_terminal_screen();
                    print_live_hud(round);
                    std::cout << '\n'
                              << std::string(static_cast<std::size_t>(banner_padding), ' ')
                              << round_title << '\n';
                    print_board();
                    print_frame_events(frame_events);
                }
                announce_winner();
                return;
            }

            RobotEntry& actor = robots_[idx];
            if (actor.robot->get_health() <= 0) {
                continue;
            }

            if (!config_.game_state_live) {
                std::cout << "\n" << actor.robot->m_name << " " << actor.symbol << " begins turn.\n";
                print_robot_stats(actor);
            }

            int radar_direction = 0;
            actor.robot->get_radar_direction(radar_direction);
            if (radar_direction < 0 || radar_direction > 8) {
                radar_direction = 0;
            }

            std::vector<RadarObj> radar = perform_radar_scan(idx, radar_direction);
            if (config_.game_state_live) {
                std::ostringstream radar_os;
                radar_os << actor.robot->m_name << " radar d" << radar_direction << " found " << radar.size();
                if (!radar.empty()) {
                    radar_os << " first:" << radar.front().m_type << "@(" << radar.front().m_row << ","
                             << radar.front().m_col << ")";
                }
                frame_events.push_back(radar_os.str());
            } else {
                log_radar_results(radar);
            }
            actor.robot->process_radar_results(radar);

            int shot_row = 0;
            int shot_col = 0;
            if (actor.robot->get_shot_location(shot_row, shot_col)) {
                const std::vector<std::string> events = handle_shot(idx, shot_row, shot_col);
                for (const auto& event : events) {
                    if (config_.game_state_live) {
                        frame_events.push_back(event);
                    } else {
                        std::cout << "Shooting: " << event << "\n";
                    }
                }
            } else {
                int move_direction = 0;
                int move_distance = 0;
                actor.robot->get_move_direction(move_direction, move_distance);
                const std::vector<std::string> events = handle_movement(idx, move_direction, move_distance);
                for (const auto& event : events) {
                    if (config_.game_state_live) {
                        frame_events.push_back(event);
                    } else {
                        std::cout << "Moving: " << event << "\n";
                    }
                }
            }
        }

        if (config_.game_state_live) {
            clear_terminal_screen();
            print_live_hud(round);
            std::cout << '\n' << std::string(static_cast<std::size_t>(banner_padding), ' ') << round_title << '\n';
            print_board();
            print_frame_events(frame_events);
            if (config_.sleep_interval > 0.0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(static_cast<int>(config_.sleep_interval * 1000.0)));
            }
        }
    }

    std::cout << "\nReached max rounds (" << config_.max_rounds << ").\n";
    if (winner_index().has_value()) {
        announce_winner();
    } else {
        std::cout << "No winner (draw).\n";
    }
}
