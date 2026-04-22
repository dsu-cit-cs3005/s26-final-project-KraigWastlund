#include "RobotBase.h"

#include <vector>

class Robot_Kraig : public RobotBase
{
private:
    // Clockwise direction sequence: Right, Down-right, Down, Down-left,
    // Left, Up-left, Up, Up-right.
    int direction_index_ = 0;

public:
    Robot_Kraig() : RobotBase(3, 4, hammer)
    {
        m_name = "Robot_Kraig";
    }

    void get_radar_direction(int& radar_direction) override
    {
        static const int kDirections[8] = {3, 4, 5, 6, 7, 8, 1, 2};
        radar_direction = kDirections[direction_index_ % 8];
    }

    void process_radar_results(const std::vector<RadarObj>& radar_results) override
    {
        // This robot ignores radar and follows a fixed movement pattern.
        (void)radar_results;
    }

    bool get_shot_location(int& shot_row, int& shot_col) override
    {
        // This robot never shoots.
        (void)shot_row;
        (void)shot_col;
        return false;
    }

    void get_move_direction(int& move_direction, int& move_distance) override
    {
        static const int kDirections[8] = {3, 4, 5, 6, 7, 8, 1, 2};

        // If trapped in a pit, move speed is 0.
        if (get_move_speed() <= 0) {
            move_direction = 0;
            move_distance = 0;
            return;
        }

        move_direction = kDirections[direction_index_ % 8];
        move_distance = 1;

        // Advance the pattern so next turn continues the clockwise cycle.
        direction_index_ = (direction_index_ + 1) % 8;
    }
};

extern "C" RobotBase* create_robot()
{
    return new Robot_Kraig();
}

extern "C" const char* robot_summary()
{
    return "Moves clockwise in a loop and never shoots.";
}
