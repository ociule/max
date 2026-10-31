/* Copyright (c) 2022 M.A.X. Port Team
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef JEVAI_HPP
#define JEVAI_HPP

#include <string>
#include <vector>

#include "jevbridge.hpp"
#include "noneai.hpp"
#include "unitinfo.hpp"

/**
 * \brief Proof of concept computer player that explores with the Jev decision model.
 *
 * Jev moves every mobile land unit of the team, one unit and one move at a time. For each move the game lists up to
 * 17 legal destinations with facts about each (unexplored cells revealed, danger, distance to the nearest enemy) plus
 * "stay", and Jev picks one. Nothing else is played: no building, research or attacks other than automatic return fire.
 * Strategy, starting units and saves come from NoneAi.
 */
class JevAi final : public NoneAi {
    enum class State { SELECT_UNIT, WAIT_FOR_ANSWER, WAIT_FOR_MOVE };

    struct Option {
        std::string key;
        Point destination;
        std::string facts;
        int32_t score;
    };

    State m_state{State::SELECT_UNIT};
    std::vector<SmartPointer<UnitInfo>> m_units;
    SmartPointer<UnitInfo> m_unit;
    std::vector<Option> m_options;
    Point m_move_start;
    uint64_t m_wait_start{0};
    int32_t m_unit_picks{0};
    int32_t m_request_id{0};
    int32_t m_turn_questions{0};
    int32_t m_turn_moves{0};
    int32_t m_turn_failed_moves{0};
    int32_t m_units_lost{0};
    int32_t m_explored_cells{0};
    std::vector<uint8_t> m_explored;
    JevBridge m_bridge;

    bool IsExplored(Point point) const;
    int32_t CountUnexplored(Point center, int32_t scan) const;
    int32_t GetDanger(UnitInfo* unit, Point point, int32_t* nearest_enemy) const;
    void BuildOptions(UnitInfo* unit);
    bool Ask(UnitInfo* unit);
    void Apply(const std::string& answer);
    void FinishUnit();
    const char* GetTeamName() const;

public:
    explicit JevAi(uint16_t team) : NoneAi(team) {}

    void BeginTurn() override;
    bool CheckEndTurn() override;
    void SetInfoMapPoint(Point point) override;
    void RemoveUnit(UnitInfo* unit) override;
    int32_t GetExploredCells() const override { return m_explored_cells; }
};

#endif /* JEVAI_HPP */
