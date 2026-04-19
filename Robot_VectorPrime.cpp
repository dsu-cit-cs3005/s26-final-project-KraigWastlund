#include "RobotBase.h"

#include <cmath>
#include <limits>
#include <vector>

class Robot_VectorPrime : public RobotBase
{
private:
    int m_radar_direction = 1;
    int m_target_row = -1;
    int m_target_col = -1;
    bool m_moving_right = true;

    static int direction_from_delta(int dr, int dc)
    {
        if (dr == -1 && dc == 0) return 1;
        if (dr == -1 && dc == 1) return 2;
        if (dr == 0 && dc == 1) return 3;
        if (dr == 1 && dc == 1) return 4;
        if (dr == 1 && dc == 0) return 5;
        if (dr == 1 && dc == -1) return 6;
        if (dr == 0 && dc == -1) return 7;
        if (dr == -1 && dc == -1) return 8;
        return 0;
    }

public:
    Robot_VectorPrime() : RobotBase(4, 3, railgun)
    {
        m_name = "VectorPrime";
    }

    void get_radar_direction(int& radar_direction) override
    {
        radar_direction = m_radar_direction;
        m_radar_direction = (m_radar_direction % 8) + 1;
    }

    void process_radar_results(const std::vector<RadarObj>& radar_results) override
    {
        int current_row = 0;
        int current_col = 0;
        get_current_location(current_row, current_col);

        int best_distance = std::numeric_limits<int>::max();
        m_target_row = -1;
        m_target_col = -1;

        for (const RadarObj& obj : radar_results) {
            if (obj.m_type != 'R') {
                continue;
            }
            int distance = std::abs(obj.m_row - current_row) + std::abs(obj.m_col - current_col);
            if (distance < best_distance) {
                best_distance = distance;
                m_target_row = obj.m_row;
                m_target_col = obj.m_col;
            }
        }
    }

    bool get_shot_location(int& shot_row, int& shot_col) override
    {
        if (m_target_row == -1 || m_target_col == -1) {
            return false;
        }

        shot_row = m_target_row;
        shot_col = m_target_col;
        m_target_row = -1;
        m_target_col = -1;
        return true;
    }

    void get_move_direction(int& move_direction, int& move_distance) override
    {
        if (get_move_speed() == 0) {
            move_direction = 0;
            move_distance = 0;
            return;
        }

        int row = 0;
        int col = 0;
        get_current_location(row, col);

        if (m_target_row != -1 && m_target_col != -1) {
            int dr = (m_target_row > row) - (m_target_row < row);
            int dc = (m_target_col > col) - (m_target_col < col);
            move_direction = direction_from_delta(dr, dc);
            move_distance = (move_direction == 0) ? 0 : 1;
            return;
        }

        if (m_moving_right) {
            if (col < m_board_col_max - 1) {
                move_direction = 3;
                move_distance = std::min(get_move_speed(), (m_board_col_max - 1) - col);
                return;
            }
            m_moving_right = false;
            if (row < m_board_row_max - 1) {
                move_direction = 5;
                move_distance = 1;
                return;
            }
        } else {
            if (col > 0) {
                move_direction = 7;
                move_distance = std::min(get_move_speed(), col);
                return;
            }
            m_moving_right = true;
            if (row > 0) {
                move_direction = 1;
                move_distance = 1;
                return;
            }
        }

        move_direction = 0;
        move_distance = 0;
    }
};

extern "C" RobotBase* create_robot()
{
    return new Robot_VectorPrime();
}

extern "C" const char* robot_summary()
{
    return "Sweeps rows, railguns spotted enemies.";
}
