#ifndef ARENA_H
#define ARENA_H

#include "RobotBase.h"

#include <filesystem>
#include <optional>
#include <random>
#include <string_view>
#include <string>
#include <vector>

struct GameConfig {
    int rows = 20;
    int cols = 20;
    int max_rounds = 200;
    double sleep_interval = 0.0;
    bool game_state_live = true;
    int flamethrowers = 3;
    int pits = 3;
    int mounds = 3;
};

class Arena {
public:
    explicit Arena(const std::string& config_file);
    explicit Arena(GameConfig cfg);
    ~Arena();

    void set_live_mode(bool live_mode);

    bool load_robots();
    bool initialize_board();
    void print_board() const;
    void run_simulation();

private:
    struct RobotEntry {
        std::string source_file;
        std::string shared_lib;
        void* handle = nullptr;
        RobotBase* robot = nullptr;
        std::string summary;
        std::string death_reason;
        char symbol = '?';
        int grenades_fired = 0;
    };

    GameConfig config_;
    std::vector<std::string> obstacle_board_;
    std::vector<RobotEntry> robots_;
    std::mt19937 rng_;

    static GameConfig load_config(const std::string& filename);

    std::vector<std::filesystem::path> discover_robot_sources() const;
    std::optional<std::pair<int, int>> random_empty_cell();
    void place_obstacles(char obstacle_type, int count);

    bool in_bounds(int row, int col) const;
    int robot_at(int row, int col, bool include_dead, int exclude_idx) const;
    char radar_cell_type(int row, int col, int observer_idx) const;

    std::vector<RadarObj> perform_radar_scan(int robot_idx, int direction) const;

    int random_damage(int low, int high);
    int apply_armor(int raw_damage, int armor) const;
    std::string damage_robot(int target_idx, int raw_damage, const std::string& source_name);

    std::vector<std::string> handle_shot(int shooter_idx, int shot_row, int shot_col);
    std::vector<std::string> handle_movement(int robot_idx, int direction, int distance);

    void print_robot_stats(const RobotEntry& entry) const;
    void log_radar_results(const std::vector<RadarObj>& radar) const;

    void print_live_hud(int round) const;
    void print_frame_events(const std::vector<std::string>& frame_events) const;
    std::string colorize_token(const std::string& token) const;

    std::optional<int> winner_index() const;
    void announce_winner() const;
};

#endif
