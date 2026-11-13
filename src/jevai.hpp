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
#include <map>
#include <vector>

#include "jevbridge.hpp"
#include "noneai.hpp"
#include "unitinfo.hpp"

/**
 * \brief Computer player that asks the Jev decision model what each factory and each mobile land unit does.
 *
 * Each turn the controller first releases finished units from factories and building sites. Construction and
 * military decisions are separate questions, each with its own instructions and state:
 * - production: one per idle factory, which unit to produce, or nothing;
 * - construction: builders choose what to build, to refill, to get out of danger or to stay, and surveyors search
 *   for resources where it is safe; these units act before the military ones;
 * - military: every other mobile land unit chooses a target to attack, where to move, or to stay.
 * The game lists only legal options and attaches facts to each one; Jev picks. Jev chooses what to build, the game
 * chooses where. Strategy, starting units and saves come from NoneAi.
 */
class JevAi final : public NoneAi {
    enum class State { SELECT, WAIT_FOR_ANSWER, WAIT_FOR_UNIT };

    enum class Action { STAY, MOVE, BUILD, PRODUCE, ATTACK, REFILL, COMMAND };

    struct Option {
        std::string key;
        Action action;
        Point destination;
        ResourceID unit_type;
        SmartPointer<UnitInfo> target;
        std::string facts;
        int32_t score;
        /// The facts as data, for the bridge's prompt templates.
        nlohmann::ordered_json data;
        /// Build speed: 1, 2 or 4; 0 lets the controller choose.
        int32_t build_rate{0};
    };

    /// A building site a builder is still walking to.
    struct Site {
        /// UnitInfo::GetId(); UnitInfo::unit_id is only the per type number shown in unit names.
        uint32_t unit_id;
        ResourceID unit_type;
        Point site;
        int32_t turn;
        int32_t walk_turns;
        int32_t build_rate;
    };

    /// The outcome of a charge as EstimateFight sees it; gain and loss in materials (UnitInfo::GetNormalRateBuildCost).
    struct Fight {
        int32_t turns;
        int32_t allies;
        int32_t enemies;
        bool target_destroyed;
        bool unit_destroyed;
        int32_t gain;
        int32_t loss;
    };

    /// Where the team saw an enemy: buildings are kept, units for a few turns.
    struct Sighting {
        Point at;
        int32_t turn;
        bool building;
        /// Weapon range, 0 for unarmed units and buildings.
        int32_t range;
    };

    struct Threat {
        int32_t turn;
        int32_t near_base;
        int32_t outranging;
    };

    /// A Mining Station site away from the base and the connectors that join it to the base.
    struct Link {
        Point mine;
        std::vector<Point> connectors;
    };

    State m_state{State::SELECT};
    std::vector<Link> m_links;
    /// Scouts watching the way in: unit (UnitInfo::GetId()) and its post.
    std::vector<std::pair<uint32_t, Point>> m_pickets;
    std::vector<Sighting> m_sightings;
    /// Enemy combat units seen in reach of the base on the last turns, and how many outrange a Tank.
    std::vector<Threat> m_threats;
    /// Factories that chose to produce nothing, with the choices they had: not asked again until these change.
    std::map<uint32_t, std::string> m_idle_factories;
    /// Enemy combat units seen in the last 15 turns: unit id, the turn last seen and whether it outranges a Tank.
    std::map<uint32_t, std::pair<int32_t, bool>> m_enemy_army;
    std::string m_factory_signature;
    /// Builders that chose to stay, with the kinds of choices they had: not asked again until these change.
    std::map<uint32_t, std::string> m_idle_builders;
    std::string m_builder_signature;
    /// Military task groups. The Defend group holds posts on the threat axis, toward where the enemy comes from; every
    /// ground combat unit that is no Scout belongs to it, the size is what production fills. Scouts are the Scout group.
    /// Jev sets the sizes as commander; the controller moves the members.
    int32_t m_defend_size{4};
    int32_t m_scout_size{1};
    /// Scouts of the Defend group (UnitInfo::GetId()) that chase enemy raiders near the base, once enemy units are seen.
    std::vector<uint32_t> m_interceptors;
    /// Posts of the Defend group: unit (UnitInfo::GetId()) and its post.
    std::vector<std::pair<uint32_t, Point>> m_posts;
    /// Why the commander is asked next, empty when not; the turn it was last asked.
    std::string m_command_trigger;
    int32_t m_last_command_turn{-100};
    std::string m_last_command_reason;
    bool m_enemy_seen{false};
    bool m_enemy_base_seen{false};
    bool m_enemy_air_seen{false};
    int32_t m_defenders_at_command{0};
    bool m_command_enemy_seen{false};
    bool m_command_enemy_base_seen{false};
    bool m_command_enemy_air_seen{false};
    /// The unit's move was the controller's own (a Defend group member going to its post): no question after it.
    bool m_scripted{false};
    std::vector<Site> m_sites;
    std::vector<SmartPointer<UnitInfo>> m_units;
    std::vector<SmartPointer<UnitInfo>> m_handled;
    std::vector<uint32_t> m_lost;
    SmartPointer<UnitInfo> m_unit;
    SmartPointer<UnitInfo> m_refill_source;
    std::vector<Option> m_options;
    // movement points from the asked unit to each map cell along the pathfinder's own costs, -1 where no path
    std::vector<int32_t> m_reach;
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
    int32_t m_turn_exit_clears{0};
    std::vector<uint32_t> m_turn_cleared_units;
    Action m_last_action{Action::STAY};
    int32_t m_units_lost{0};
    int32_t m_explored_cells{0};
    /// Turns the base started with full raw material storage, and the raw materials mined on them, which are lost.
    int32_t m_raw_full_turns{0};
    int32_t m_raw_lost{0};
    int32_t m_last_turn{0};
    bool m_turn_done_logged{false};
    std::vector<uint8_t> m_explored;
    JevBridge m_bridge;

    bool IsExplored(Point point) const;
    int32_t CountUnexplored(Point center, int32_t scan) const;
    int32_t GetDistanceToUnexplored(Point point) const;
    int32_t GetDanger(UnitInfo* unit, Point point, int32_t* nearest_enemy) const;
    int32_t CountOwnUnits(ResourceID unit_type) const;
    bool GetBasePosition(Point& position) const;
    Complex* GetEconomyComplex() const;
    int32_t GetDistanceToOwnBuilder(Point point) const;
    bool TouchesBase(ResourceID unit_type, Point site) const;
    bool FindBuildSite(UnitInfo* builder, ResourceID unit_type, Point& site) const;
    bool LeavesExits(ResourceID unit_type, Point site) const;
    std::vector<Point> FindDefenseSites(UnitInfo* builder, ResourceID unit_type) const;
    std::vector<Point> FindMineSites(UnitInfo* builder = nullptr, bool fuel_first = false) const;
    bool FindConnectorPath(Point site, int32_t size, std::vector<Point>& path) const;
    std::vector<Point> GetLinkCells(UnitInfo* builder) const;
    void RememberEnemies();
    bool GetBaseCenter(Point& center, int32_t& extent) const;
    bool GetEnemyDirection(Point& origin) const;
    void AddScoutOptions(UnitInfo* scout);
    void AddChargeOptions(UnitInfo* unit);
    Fight EstimateFight(UnitInfo* unit, UnitInfo* target, Point end, int32_t turns_to_range, double unseen) const;
    void AddHoldOption(UnitInfo* unit);
    bool GetApproach(std::vector<Point>& cells, Point& origin) const;
    nlohmann::ordered_json GetDefenseCoverage(ResourceID unit_type, Point site) const;
    bool IsFactoryExit(Point cell) const;
    bool ClearExit(UnitInfo* parent);
    bool ReleaseFinishedUnit();
    void BalanceEconomy();
    UnitInfo* FindIdleFactory() const;
    void AddMoveOptions(UnitInfo* unit);
    void AddBuildOptions(UnitInfo* builder);
    void AddProduceOptions(UnitInfo* factory);
    void AddAttackOptions(UnitInfo* unit);
    void AddRefillOptions(UnitInfo* builder);
    void AddSurveyOptions(UnitInfo* surveyor);
    nlohmann::ordered_json GetState(UnitInfo* unit) const;
    bool Ask(UnitInfo* unit, const char* instructions);
    void Apply(const std::string& answer);
    void StartBuilding(UnitInfo* builder, ResourceID unit_type, Point site, int32_t build_rate = 0);
    void WalkToSite(ResourceID unit_type, Point site, int32_t build_rate);
    bool ContinueToSite();
    void FinishUnit();
    void ComputeReach(UnitInfo* unit);
    int32_t CountDefenders(int32_t* anti_air = nullptr) const;
    int32_t CountInProduction(bool combat, bool anti_air, bool scout, bool surveyor = false,
                              bool long_range = false) const;
    bool GetThreatAnchor(Point& anchor, Point& origin, bool& enemy_known) const;
    bool FindPost(UnitInfo* unit, Point anchor, Point& post) const;
    bool IsDefender(UnitInfo* unit) const;
    int32_t ScriptDefender(UnitInfo* unit);
    int32_t ScriptToPost(UnitInfo* unit, Point anchor, const char* role);
    int32_t ScriptScanner(UnitInfo* unit);
    int32_t ScriptScout(UnitInfo* unit);
    bool IsInterceptor(UnitInfo* unit) const;
    void AssignInterceptors();
    bool FindRaider(UnitInfo* unit, Point& at) const;
    int32_t ScriptInterceptor(UnitInfo* unit);
    int32_t CountAntiAirTurrets() const;
    void ApplyScripted(const Option& option, const char* reason);
    void CheckCommandTriggers();
    bool AskCommand();
    void ApplyCommand(const Option& option);
    nlohmann::ordered_json GetCommandState() const;
    int32_t GetReachCost(Point cell) const;
    const char* GetTeamName() const;

public:
    explicit JevAi(uint16_t team) : NoneAi(team) {}

    bool SetupStrategy() override;
    void BeginTurn() override;
    bool CheckEndTurn() override;
    void SetInfoMapPoint(Point point) override;
    void RemoveUnit(UnitInfo* unit) override;
    void UnitSpotted(UnitInfo* unit) override;
    int32_t GetExploredCells() const override { return m_explored_cells; }
};

#endif /* JEVAI_HPP */
