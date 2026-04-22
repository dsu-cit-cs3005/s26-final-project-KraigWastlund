#ifndef INCLASS_ARENA_H
#define INCLASS_ARENA_H

#include <random>
#include <string>
#include <vector>

struct GameConfig {
    int rows = 20;
    int cols = 20;
    int max_rounds = 200;
    double sleep_interval = 0.5;
    bool game_state_live = true;
    int flamethrowers = 3;
    int pits = 3;
    int mounds = 3;
};

class Arena {
public:
    explicit Arena(const std::string& config_file);

    static GameConfig load_config(const std::string& filename);
    static std::vector<std::string> discover_robot_sources();

    void place_obstacles(char obstacle_type, int count);
    bool initialize_board();
    void print_board() const;

private:
    GameConfig config_;
    std::vector<std::string> board_;
    std::mt19937 rng_;
};

#endif
