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

#include "jevai.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <climits>
#include <nlohmann/json.hpp>

#include "access.hpp"
#include "game_manager.hpp"
#include "resource_manager.hpp"
#include "task.hpp"
#include "units_manager.hpp"

using json = nlohmann::ordered_json;

/// Jev questions asked for one unit in one turn at most.
static constexpr int32_t JevAi_MaxPicksPerUnit = 4;

/// Jev questions asked in one turn at most, for the whole team.
static constexpr int32_t JevAi_MaxQuestionsPerTurn = 60;

/// Milliseconds to wait for an answer before the unit stays where it is.
static constexpr uint64_t JevAi_AnswerTimeout = 15000;

/// Milliseconds to wait for a move to finish.
static constexpr uint64_t JevAi_MoveTimeout = 30000;

static const char* const JevAi_DirectionNames[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
static const Point JevAi_Directions[8] = {{0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}};

static const char* const JevAi_Instructions =
    "You command one unit of your team in a turn-based strategy game on a map of square cells. Your goal is to "
    "explore as much of the map as possible while keeping your units alive. Choose where this unit moves now. "
    "Prefer destinations that reveal many unexplored cells. Avoid destinations with danger above 0, where enemy "
    "units can shoot. Choose stay when every move is dangerous or reveals nothing.";

static bool JevAi_IsInsideMap(Point point) {
    return point.x >= 0 && point.y >= 0 && point.x < ResourceManager_MapSize.x && point.y < ResourceManager_MapSize.y;
}

static std::string JevAi_GetUnitName(UnitInfo* unit) {
    return std::string(ResourceManager_GetUnit(unit->GetUnitType()).GetSingularName());
}

const char* JevAi::GetTeamName() const {
    static const char* const team_names[PLAYER_TEAM_MAX - 1] = {"red", "green", "blue", "gray"};

    return team_names[m_team];
}

bool JevAi::IsExplored(Point point) const {
    return !m_explored.empty() && m_explored[point.x * ResourceManager_MapSize.y + point.y];
}

int32_t JevAi::CountUnexplored(Point center, int32_t scan) const {
    int32_t result = 0;
    Point point;

    for (point.x = center.x - scan; point.x <= center.x + scan; ++point.x) {
        for (point.y = center.y - scan; point.y <= center.y + scan; ++point.y) {
            if (JevAi_IsInsideMap(point) && Access_GetSquaredDistance(point, center) <= scan * scan &&
                !IsExplored(point)) {
                ++result;
            }
        }
    }

    return result;
}

int32_t JevAi::GetDanger(UnitInfo* unit, Point point, int32_t* nearest_enemy) const {
    int32_t danger = 0;
    int32_t nearest = INT32_MAX;

    for (auto* list : {&UnitsManager_MobileLandSeaUnits, &UnitsManager_MobileAirUnits, &UnitsManager_StationaryUnits}) {
        for (auto it = list->Begin(), it_end = list->End(); it != it_end; ++it) {
            UnitInfo& enemy = *it;

            if (enemy.team != m_team && enemy.hits > 0 && enemy.IsVisibleToTeam(m_team) &&
                enemy.GetBaseValues()->GetAttribute(ATTRIB_ATTACK) > 0) {
                const int32_t distance = Access_GetSquaredDistance(point, Point(enemy.grid_x, enemy.grid_y));

                nearest = std::min(nearest, distance);

                if (Access_IsValidAttackTargetType(enemy.GetUnitType(), unit->GetUnitType())) {
                    const int32_t range = enemy.GetBaseValues()->GetAttribute(ATTRIB_RANGE);

                    if (distance <= range * range) {
                        danger += enemy.GetBaseValues()->GetAttribute(ATTRIB_ATTACK);
                    }
                }
            }
        }
    }

    if (nearest_enemy) {
        *nearest_enemy = (nearest == INT32_MAX) ? -1 : static_cast<int32_t>(SDL_sqrt(nearest));
    }

    return danger;
}

void JevAi::BuildOptions(UnitInfo* unit) {
    const Point position(unit->grid_x, unit->grid_y);
    const int32_t scan = unit->GetBaseValues()->GetAttribute(ATTRIB_SCAN);
    const int32_t speed = unit->speed;
    int32_t nearest_enemy;

    m_options.clear();

    for (int32_t direction = 0; direction < 8; ++direction) {
        const bool is_diagonal = JevAi_Directions[direction].x != 0 && JevAi_Directions[direction].y != 0;
        const int32_t full = is_diagonal ? std::max(1, speed * 2 / 3) : speed;
        const int32_t distances[2] = {std::max(1, full / 2), full};

        for (int32_t i = 0; i < 2; ++i) {
            if (i == 1 && distances[1] == distances[0]) {
                continue;
            }

            Point destination = position;

            destination.x += JevAi_Directions[direction].x * distances[i];
            destination.y += JevAi_Directions[direction].y * distances[i];

            if (!JevAi_IsInsideMap(destination) ||
                !Access_IsAccessible(unit->GetUnitType(), m_team, destination.x, destination.y,
                                     AccessModifier_SameClassBlocks)) {
                continue;
            }

            const int32_t revealed = CountUnexplored(destination, scan);
            const int32_t danger = GetDanger(unit, destination, &nearest_enemy);
            std::string facts = std::string("move ") + JevAi_DirectionNames[direction] + " " +
                                std::to_string(distances[i]) + " cells to [" + std::to_string(destination.x + 1) + "," +
                                std::to_string(destination.y + 1) + "]: reveals " + std::to_string(revealed) +
                                " unexplored cells, danger " + std::to_string(danger);

            if (nearest_enemy >= 0) {
                facts += ", nearest known enemy " + std::to_string(nearest_enemy) + " cells away";
            }

            m_options.push_back({"m" + std::to_string(m_options.size()), destination, facts, revealed - danger * 10});
        }
    }

    const int32_t danger = GetDanger(unit, position, &nearest_enemy);

    m_options.push_back(
        {"stay", position,
         "stay where it is for the rest of this turn: reveals 0 unexplored cells, danger " + std::to_string(danger),
         -danger * 10});
}

bool JevAi::Ask(UnitInfo* unit) {
    int32_t nearest_enemy;
    json state;
    json criteria;
    json scores;

    BuildOptions(unit);

    state["game"] = "M.A.X., turn-based strategy. You only move units to explore.";
    state["turn"] = GameManager_TurnCounter;
    state["unit"] = {{"type", JevAi_GetUnitName(unit)},
                     {"position", {unit->grid_x + 1, unit->grid_y + 1}},
                     {"hits", unit->hits},
                     {"max_hits", unit->GetBaseValues()->GetAttribute(ATTRIB_HITS)},
                     {"moves_left", unit->speed},
                     {"scan_range", unit->GetBaseValues()->GetAttribute(ATTRIB_SCAN)},
                     {"danger_here", GetDanger(unit, Point(unit->grid_x, unit->grid_y), &nearest_enemy)}};
    state["team"] = {{"explored_cells", m_explored_cells},
                     {"map_cells", ResourceManager_MapSize.x * ResourceManager_MapSize.y},
                     {"units_lost", m_units_lost}};

    for (const auto& option : m_options) {
        criteria[option.key] = option.facts;
        scores[option.key] = option.score;
    }

    json request;

    request["id"] = ++m_request_id;
    request["team"] = GetTeamName();
    request["state"] = state;
    request["questions"] = {
        {"move", {{"type", "choice"}, {"instructions", JevAi_Instructions}, {"criteria", criteria}}}};
    request["mock_scores"] = scores;

    ++m_turn_questions;
    ++m_unit_picks;

    return m_bridge.Send(request.dump());
}

void JevAi::Apply(const std::string& answer) {
    std::string choice = "stay";
    std::string model = "?";
    double confidence = 0.0;

    try {
        const json reply = json::parse(answer);

        if (reply.value("id", 0) != m_request_id) {
            SDL_Log("Jev %s: ignoring an answer to an old question.", GetTeamName());

            return;
        }

        if (reply.contains("error")) {
            SDL_Log("Jev %s: bridge error: %s", GetTeamName(), reply["error"].dump().c_str());

        } else {
            choice = reply.value("choice", "stay");
            model = reply.value("model", "?");
            confidence = reply.value("confidence", 0.0);
        }

    } catch (const json::exception& exception) {
        SDL_Log("Jev %s: unreadable answer: %s", GetTeamName(), exception.what());
    }

    auto option =
        std::find_if(m_options.begin(), m_options.end(), [&choice](const Option& item) { return item.key == choice; });

    if (option == m_options.end()) {
        option = m_options.end() - 1;
    }

    SDL_Log("Jev %s: %s at [%d,%d] chose %s (%s), model %s, confidence %.2f.", GetTeamName(),
            JevAi_GetUnitName(&*m_unit).c_str(), m_unit->grid_x + 1, m_unit->grid_y + 1, option->key.c_str(),
            option->facts.c_str(), model.c_str(), confidence);

    if (option->key == "stay") {
        FinishUnit();

    } else {
        m_move_start = Point(m_unit->grid_x, m_unit->grid_y);
        m_unit->move_to_grid_x = option->destination.x;
        m_unit->move_to_grid_y = option->destination.y;

        UnitsManager_SetNewOrder(&*m_unit, ORDER_MOVE, ORDER_STATE_INIT);

        ++m_turn_moves;
        m_wait_start = SDL_GetTicks();
        m_state = State::WAIT_FOR_MOVE;
    }
}

void JevAi::FinishUnit() {
    if (m_unit) {
        m_units.erase(std::remove(m_units.begin(), m_units.end(), m_unit), m_units.end());
        m_unit = nullptr;
    }

    m_unit_picks = 0;
    m_state = State::SELECT_UNIT;
}

void JevAi::BeginTurn() {
    // a new game; the controllers outlive games
    if (GameManager_TurnCounter <= m_last_turn) {
        m_explored.clear();
        m_explored_cells = 0;
        m_units_lost = 0;
    }

    m_last_turn = GameManager_TurnCounter;
    m_turn_done_logged = false;
    m_units.clear();
    m_unit = nullptr;
    m_unit_picks = 0;
    m_turn_questions = 0;
    m_turn_moves = 0;
    m_turn_failed_moves = 0;
    m_state = State::SELECT_UNIT;

    for (auto it = UnitsManager_MobileLandSeaUnits.Begin(), it_end = UnitsManager_MobileLandSeaUnits.End();
         it != it_end; ++it) {
        if ((*it).team == m_team && (*it).hits > 0 && ((*it).flags & MOBILE_LAND_UNIT)) {
            m_units.push_back(SmartPointer<UnitInfo>(it->Get()));
        }
    }

    SDL_Log("Jev %s: turn %d, %d units to move.", GetTeamName(), GameManager_TurnCounter,
            static_cast<int32_t>(m_units.size()));
}

bool JevAi::CheckEndTurn() {
    if (UnitsManager_TeamInfo[m_team].finished_turn || !GameManager_IsActiveTurn(m_team)) {
        return false;
    }

    switch (m_state) {
        case State::WAIT_FOR_ANSWER: {
            std::string answer;

            if (m_bridge.Poll(answer)) {
                Apply(answer);

                return true;
            }

            if (SDL_GetTicks() - m_wait_start > JevAi_AnswerTimeout) {
                SDL_Log("Jev %s: no answer within %u ms, the unit stays.", GetTeamName(),
                        static_cast<uint32_t>(JevAi_AnswerTimeout));

                m_bridge.Close();
                FinishUnit();
            }

            return false;
        }

        case State::WAIT_FOR_MOVE: {
            if (!m_unit || m_unit->hits == 0) {
                FinishUnit();

                return false;
            }

            const bool is_idle = Task_IsReadyToTakeOrders(&*m_unit) && !UnitsManager_CombatEffectsActive;

            if (!is_idle && SDL_GetTicks() - m_wait_start < JevAi_MoveTimeout) {
                return false;
            }

            if (Point(m_unit->grid_x, m_unit->grid_y) == m_move_start) {
                ++m_turn_failed_moves;

                SDL_Log("Jev %s: %s did not move.", GetTeamName(), JevAi_GetUnitName(&*m_unit).c_str());
            }

            if (!is_idle || m_unit->speed == 0 || m_unit_picks >= JevAi_MaxPicksPerUnit) {
                FinishUnit();

            } else {
                m_state = State::SELECT_UNIT;
            }

            return false;
        }

        case State::SELECT_UNIT: {
            if (!m_unit) {
                // the next unit that can still act
                while (!m_units.empty() && m_turn_questions < JevAi_MaxQuestionsPerTurn) {
                    auto& unit = m_units.front();

                    if (unit->hits > 0 && unit->speed > 0 && Task_IsReadyToTakeOrders(&*unit)) {
                        m_unit = unit;
                        break;
                    }

                    m_units.erase(m_units.begin());
                }
            }

            if (!m_unit || m_turn_questions >= JevAi_MaxQuestionsPerTurn) {
                if (UnitsManager_CombatEffectsActive) {
                    return false;
                }

                if (!m_turn_done_logged) {
                    m_turn_done_logged = true;

                    SDL_Log(
                        "Jev %s: turn %d done: %d questions, %d moves, %d did not move, %d cells explored, %d "
                        "units lost.",
                        GetTeamName(), GameManager_TurnCounter, m_turn_questions, m_turn_moves, m_turn_failed_moves,
                        m_explored_cells, m_units_lost);
                }

                return AiController_EndTurn(m_team);
            }

            if (Ask(&*m_unit)) {
                m_wait_start = SDL_GetTicks();
                m_state = State::WAIT_FOR_ANSWER;

            } else {
                // without the bridge the unit stays where it is
                FinishUnit();
            }

            return true;
        }
    }

    return false;
}

void JevAi::SetInfoMapPoint(Point point) {
    if (m_explored.size() != static_cast<size_t>(ResourceManager_MapSize.x * ResourceManager_MapSize.y)) {
        m_explored.assign(ResourceManager_MapSize.x * ResourceManager_MapSize.y, 0);
        m_explored_cells = 0;
    }

    if (JevAi_IsInsideMap(point)) {
        uint8_t& cell = m_explored[point.x * ResourceManager_MapSize.y + point.y];

        if (!cell) {
            cell = 1;
            ++m_explored_cells;
        }
    }
}

void JevAi::RemoveUnit(UnitInfo* unit) {
    if (unit->team == m_team && unit->hits == 0 && (unit->flags & MOBILE_LAND_UNIT)) {
        ++m_units_lost;

        SDL_Log("Jev %s: lost %s at [%d,%d].", GetTeamName(), JevAi_GetUnitName(unit).c_str(), unit->grid_x + 1,
                unit->grid_y + 1);
    }

    if (m_unit == unit) {
        FinishUnit();

    } else {
        m_units.erase(std::remove(m_units.begin(), m_units.end(), unit), m_units.end());
    }
}
