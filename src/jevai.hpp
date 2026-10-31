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

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "jevbridge.hpp"
#include "noneai.hpp"
#include "unitinfo.hpp"

/**
 * \brief Computer player that asks the Jev decision model what each factory and each mobile land unit does.
 *
 * Each turn the controller first releases finished units from factories and building sites. Then it asks one question
 * per idle factory (which unit to produce, or nothing) and one or more per mobile land unit (where to move, what to
 * build for constructors and engineers, or stay). The game lists only legal options and attaches facts to each one;
 * Jev picks. Jev chooses what to build, the game chooses where. Strategy, starting units and saves come from NoneAi.
 */
class JevAi final : public NoneAi {
    enum class State { SELECT, WAIT_FOR_ANSWER, WAIT_FOR_UNIT };

    enum class Action { STAY, MOVE, BUILD, PRODUCE, ATTACK };

    struct Option {
        std::string key;
        Action action;
        Point destination;
        ResourceID unit_type;
        SmartPointer<UnitInfo> target;
        std::string facts;
        int32_t score;
    };

    State m_state{State::SELECT};
    std::vector<SmartPointer<UnitInfo>> m_units;
    std::vector<SmartPointer<UnitInfo>> m_handled;
    SmartPointer<UnitInfo> m_unit;
    std::vector<Option> m_options;
    std::string m_question;
    Option m_pending_build{};
    bool m_has_pending_build{false};
    Point m_move_start;
    uint64_t m_wait_start{0};
    int32_t m_unit_picks{0};
    int32_t m_request_id{0};
    int32_t m_turn_questions{0};
    int32_t m_turn_moves{0};
    int32_t m_turn_failed_moves{0};
    int32_t m_turn_builds{0};
    int32_t m_turn_attacks{0};
    Action m_last_action{Action::STAY};
    int32_t m_units_lost{0};
    int32_t m_explored_cells{0};
    int32_t m_last_turn{0};
    bool m_turn_done_logged{false};
    std::vector<uint8_t> m_explored;
    JevBridge m_bridge;

    bool IsExplored(Point point) const;
    int32_t CountUnexplored(Point center, int32_t scan) const;
    int32_t GetDistanceToUnexplored(Point point) const;
    int32_t GetDanger(UnitInfo* unit, Point point, int32_t* nearest_enemy) const;
    int32_t CountOwnUnits(ResourceID unit_type) const;
    bool TouchesBase(ResourceID unit_type, Point site) const;
    bool FindBuildSite(UnitInfo* builder, ResourceID unit_type, Point& site) const;
    bool ReleaseFinishedUnit();
    void BalanceEconomy();
    UnitInfo* FindIdleFactory() const;
    void AddMoveOptions(UnitInfo* unit);
    void AddBuildOptions(UnitInfo* builder);
    void AddProduceOptions(UnitInfo* factory);
    void AddAttackOptions(UnitInfo* unit);
    nlohmann::ordered_json GetState(UnitInfo* unit) const;
    bool Ask(UnitInfo* unit, const char* instructions);
    void Apply(const std::string& answer);
    void StartBuilding(UnitInfo* builder, ResourceID unit_type, Point site);
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
