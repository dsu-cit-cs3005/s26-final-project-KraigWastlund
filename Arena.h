#ifndef ARENA_H
#define ARENA_H

#include "RobotBase.h"

#include <random>
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
        char symbol = '?';
    };

    static GameConfig load_config(const std::string& filename);
    static std::vector<std::string> discover_robot_sources();

    void place_obstacles(char obstacle_type, int count);
    bool in_bounds(int row, int col) const;

    GameConfig config_;
    std::vector<std::string> obstacle_board_;
    std::vector<RobotEntry> robots_;
    std::mt19937 rng_;
};

#endif
