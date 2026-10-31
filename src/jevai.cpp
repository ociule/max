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

#include "access.hpp"
#include "builder.hpp"
#include "buildmenu.hpp"
#include "cargo.hpp"
#include "game_manager.hpp"
#include "path.hpp"
#include "production_manager.hpp"
#include "remote.hpp"
#include "resource_manager.hpp"
#include "survey.hpp"
#include "task.hpp"
#include "units_manager.hpp"

using json = nlohmann::ordered_json;

/// Jev questions asked for one unit in one turn at most.
static constexpr int32_t JevAi_MaxPicksPerUnit = 4;

/// Jev questions asked in one turn at most, for the whole team.
static constexpr int32_t JevAi_MaxQuestionsPerTurn = 80;

/// Milliseconds to wait for an answer before the unit stays where it is.
static constexpr uint64_t JevAi_AnswerTimeout = 15000;

/// Milliseconds to wait for a move, a build start or a release to finish.
static constexpr uint64_t JevAi_UnitTimeout = 30000;

/// Cells around a builder searched for building sites.
static constexpr int32_t JevAi_SiteSearchRadius = 6;

static const char* const JevAi_DirectionNames[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};

static const char* const JevAi_UnitInstructions =
    "You command one unit of your team in M.A.X., a turn-based strategy game on a map of square cells. To win, build "
    "an economy and an army and destroy the enemy. A usual opening: build a factory next to your base, keep enough "
    "power for your buildings, put mining stations on surveyed resources, then produce combat units and attack. "
    "Constructors and engineers build from the raw materials they carry and refill at the base. Buildings that touch "
    "your base join it and share its materials and power. Explore to find resources and enemies, and avoid danger "
    "above 0, where enemies can shoot. Choose this unit's action now.";

static const char* const JevAi_FactoryInstructions =
    "You run one factory of your team in M.A.X., a turn-based strategy game. To win, build an economy and an army and "
    "destroy the enemy. Choose which unit this factory produces next, or nothing. A unit takes the given number of "
    "turns and uses raw materials from your base every turn while it is being built.";

static bool JevAi_IsInsideMap(Point point) {
    return point.x >= 0 && point.y >= 0 && point.x < ResourceManager_MapSize.x && point.y < ResourceManager_MapSize.y;
}

static std::string JevAi_GetTypeName(ResourceID unit_type) {
    return std::string(ResourceManager_GetUnit(unit_type).GetSingularName());
}

static std::string JevAi_GetUnitName(UnitInfo* unit) { return JevAi_GetTypeName(unit->GetUnitType()); }

static std::string JevAi_FormatPoint(Point point) {
    return "[" + std::to_string(point.x + 1) + "," + std::to_string(point.y + 1) + "]";
}

static bool JevAi_IsFactory(ResourceID unit_type) {
    return unit_type == LIGHTPLT || unit_type == LANDPLT || unit_type == SHIPYARD || unit_type == AIRPLT ||
           unit_type == TRAINHAL;
}

static bool JevAi_IsBuilder(ResourceID unit_type) { return unit_type == CONSTRCT || unit_type == ENGINEER; }

static int32_t JevAi_GetSize(ResourceID unit_type) {
    return (ResourceManager_GetUnit(unit_type).GetFlags() & BUILDING) ? 2 : 1;
}

/// What a unit type is for, in a few words.
static std::string JevAi_DescribePurpose(uint16_t team, ResourceID unit_type) {
    if (JevAi_IsFactory(unit_type)) {
        std::string result = "factory that makes";

        for (const ResourceID product : Builder_GetBuildableUnits(team, unit_type)) {
            result += " " + JevAi_GetTypeName(product) + ",";
        }

        result.back() = ';';

        return result;
    }

    switch (unit_type) {
        case MININGST:
            return "mines raw materials, fuel and gold from the resources under it, the base's only income;";
        case POWGEN:
            return "small power plant, most buildings need power to work;";
        case POWERSTN:
            return "large power plant for a big base;";
        case CNCT_4W:
            return "connector, joins separate buildings into one base;";
        case ADUMP:
            return "extra storage for raw materials, only useful when storage is full;";
        case FDUMP:
            return "extra storage for fuel, only useful when storage is full;";
        case GOLDSM:
            return "extra storage for gold;";
        case HABITAT:
            return "dwelling, gives workers to training halls, research centres and eco-spheres;";
        case RESEARCH:
            return "research centre, slowly upgrades units, needs workers;";
        case GREENHSE:
            return "eco-sphere, scores points, needs workers;";
        case DEPOT:
            return "repairs and reloads land units next to it;";
        case HANGAR:
            return "repairs and reloads aircraft;";
        case DOCK:
            return "repairs and reloads ships;";
        case BARRACKS:
            return "repairs and reloads infantry;";
        case RADAR:
            return "radar, sees enemies far away;";
        case GUNTURRT:
        case ARTYTRRT:
        case ANTIAIR:
        case ANTIMSSL:
            return "fixed defense turret;";
        case ROAD:
            return "road, units move faster on it;";
        case BRIDGE:
            return "bridge, lets land units cross water;";
        case WTRPLTFM:
            return "platform for building on water;";
        case LANDPAD:
            return "landing pad for aircraft;";
        case BLOCK:
            return "concrete wall that blocks movement;";
        case CONSTRCT:
            return "builds large buildings: factories, mining stations, power plants;";
        case ENGINEER:
            return "builds small buildings: power generators, connectors, turrets, roads;";
        case SURVEYOR:
            return "finds resources for mining stations;";
        case SCOUT:
            return "fast scout that sees far;";
        case TANK:
            return "main battle tank;";
        case ARTILLRY:
            return "artillery, long range against ground;";
        case ROCKTLCH:
            return "rocket launcher, long range against ground;";
        case MISSLLCH:
            return "missile launcher, very long range;";
        case SP_FLAK:
            return "mobile anti-aircraft, only shoots aircraft;";
        case MINELAYR:
            return "lays mines;";
        case SCANNER:
            return "mobile radar;";
        case SPLYTRCK:
            return "supply truck, reloads units;";
        case REPAIR:
            return "repair unit;";
        case FUELTRCK:
            return "fuel truck;";
        case GOLDTRCK:
            return "gold truck;";
        case BULLDOZR:
            return "clears rubble;";
        case INFANTRY:
            return "infantry;";
        case COMMANDO:
            return "infiltrator, steals or disables enemy units;";
        default:
            return "";
    }
}

/// A short description of what a unit type is for.
static std::string JevAi_DescribeType(uint16_t team, ResourceID unit_type) {
    UnitValues* values = UnitsManager_GetCurrentUnitValues(&UnitsManager_TeamInfo[team], unit_type);
    std::string result;

    if (values->GetAttribute(ATTRIB_ATTACK) > 0) {
        result += "attack " + std::to_string(values->GetAttribute(ATTRIB_ATTACK)) + ", range " +
                  std::to_string(values->GetAttribute(ATTRIB_RANGE)) + ", ";
    }

    result += "hits " + std::to_string(values->GetAttribute(ATTRIB_HITS)) + ", armor " +
              std::to_string(values->GetAttribute(ATTRIB_ARMOR));

    if (values->GetAttribute(ATTRIB_SPEED) > 0) {
        result += ", speed " + std::to_string(values->GetAttribute(ATTRIB_SPEED));
    }

    return result;
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

int32_t JevAi::GetDistanceToUnexplored(Point point) const {
    int32_t nearest = INT32_MAX;
    Point cell;

    for (cell.x = 0; cell.x < ResourceManager_MapSize.x; ++cell.x) {
        for (cell.y = 0; cell.y < ResourceManager_MapSize.y; ++cell.y) {
            if (!IsExplored(cell)) {
                nearest = std::min(nearest, Access_GetSquaredDistance(point, cell));
            }
        }
    }

    return (nearest == INT32_MAX) ? -1 : static_cast<int32_t>(SDL_sqrt(nearest));
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

int32_t JevAi::CountOwnUnits(ResourceID unit_type) const {
    int32_t result = 0;

    for (auto* list : {&UnitsManager_MobileLandSeaUnits, &UnitsManager_MobileAirUnits, &UnitsManager_StationaryUnits}) {
        for (auto it = list->Begin(), it_end = list->End(); it != it_end; ++it) {
            if ((*it).team == m_team && (*it).GetUnitType() == unit_type && (*it).hits > 0) {
                ++result;
            }
        }
    }

    return result;
}

bool JevAi::TouchesBase(ResourceID unit_type, Point site) const {
    const int32_t size = JevAi_GetSize(unit_type);

    for (auto it = UnitsManager_StationaryUnits.Begin(), it_end = UnitsManager_StationaryUnits.End(); it != it_end;
         ++it) {
        if ((*it).team == m_team && (*it).GetComplex() && (*it).hits > 0) {
            Rect bounds;

            (*it).GetBounds(&bounds);

            // buildings connect along an edge, touching corners do not connect
            const bool overlaps_x = site.x < bounds.lrx && site.x + size > bounds.ulx;
            const bool overlaps_y = site.y < bounds.lry && site.y + size > bounds.uly;
            const bool touches_x = site.x + size == bounds.ulx || site.x == bounds.lrx;
            const bool touches_y = site.y + size == bounds.uly || site.y == bounds.lry;

            if ((touches_x && overlaps_y) || (touches_y && overlaps_x)) {
                return true;
            }
        }
    }

    return false;
}

bool JevAi::FindBuildSite(UnitInfo* builder, ResourceID unit_type, Point& site) const {
    const int32_t size = JevAi_GetSize(unit_type);
    const Point position(builder->grid_x, builder->grid_y);
    int64_t best_score = INT64_MIN;
    Point candidate;

    for (candidate.x = position.x - JevAi_SiteSearchRadius; candidate.x <= position.x + JevAi_SiteSearchRadius;
         ++candidate.x) {
        for (candidate.y = position.y - JevAi_SiteSearchRadius; candidate.y <= position.y + JevAi_SiteSearchRadius;
             ++candidate.y) {
            if (!JevAi_IsInsideMap(candidate) ||
                !JevAi_IsInsideMap(Point(candidate.x + size - 1, candidate.y + size - 1))) {
                continue;
            }

            if (!Access_IsAccessible(unit_type, m_team, candidate.x, candidate.y,
                                     AccessModifier_EnemySameClassBlocks)) {
                continue;
            }

            // the builder can start on any cell of the site, otherwise it walks to the top left cell
            const bool covers_builder = position.x >= candidate.x && position.x < candidate.x + size &&
                                        position.y >= candidate.y && position.y < candidate.y + size;
            const int32_t distance =
                covers_builder ? 0 : static_cast<int32_t>(SDL_sqrt(Access_GetSquaredDistance(position, candidate)));
            int64_t score = -distance;

            if (distance > builder->speed + 1) {
                continue;
            }

            if (unit_type == MININGST) {
                int16_t raw;
                int16_t gold;
                int16_t fuel;

                Survey_GetTotalResourcesInArea(candidate.x, candidate.y, 1, &raw, &gold, &fuel, false, m_team);

                score += (raw + fuel + gold * 2) * 10;

            } else if (TouchesBase(unit_type, candidate)) {
                score += 100;
            }

            if (score > best_score) {
                best_score = score;
                site = candidate;
            }
        }
    }

    return best_score != INT64_MIN;
}

bool JevAi::ReleaseFinishedUnit() {
    for (auto* list : {&UnitsManager_StationaryUnits, &UnitsManager_MobileLandSeaUnits}) {
        for (auto it = list->Begin(), it_end = list->End(); it != it_end; ++it) {
            UnitInfo& unit = *it;

            if (unit.team != m_team || unit.hits == 0 || unit.GetOrder() != ORDER_BUILD ||
                unit.GetOrderState() != ORDER_STATE_UNIT_READY || !unit.GetParent()) {
                continue;
            }

            // same exit search as the classic TaskActivate: walk around the parent's outline
            const bool is_factory = (unit.flags & STATIONARY) != 0;
            UnitInfo* parent = is_factory ? &unit : unit.GetParent();
            UnitInfo* leaving = is_factory ? unit.GetParent() : &unit;
            const int32_t size = (parent->flags & BUILDING) ? 2 : 1;
            Point position(parent->grid_x - 1, parent->grid_y + size);

            for (int32_t direction = 0; direction < 8; direction += 2) {
                for (int32_t range = 0; range < size + 1; ++range) {
                    position += DIRECTION_OFFSETS[direction];

                    if (Access_IsAccessible(leaving->GetUnitType(), m_team, position.x, position.y,
                                            AccessModifier_SameClassBlocks)) {
                        SDL_Log("Jev %s: %s leaves %s to %s.", GetTeamName(), JevAi_GetUnitName(leaving).c_str(),
                                JevAi_GetUnitName(parent).c_str(), JevAi_FormatPoint(position).c_str());

                        if (is_factory) {
                            unit.move_to_grid_x = position.x;
                            unit.move_to_grid_y = position.y;
                            unit.SetParent(leaving);

                            UnitsManager_SetNewOrder(&unit, ORDER_ACTIVATE, ORDER_STATE_EXECUTING_ORDER);

                            if (leaving->flags & MOBILE_LAND_UNIT) {
                                m_units.push_back(SmartPointer<UnitInfo>(leaving));
                            }

                        } else {
                            unit.move_to_grid_x = position.x;
                            unit.move_to_grid_y = position.y;

                            UnitsManager_SetNewOrder(&unit, ORDER_ACTIVATE, ORDER_STATE_IN_TRANSITION);
                        }

                        m_unit = &unit;
                        m_last_action = Action::STAY;
                        m_move_start = Point(unit.grid_x, unit.grid_y);
                        m_wait_start = SDL_GetTicks();
                        m_state = State::WAIT_FOR_UNIT;

                        return true;
                    }
                }
            }

            SDL_Log("Jev %s: no free cell around %s for %s.", GetTeamName(), JevAi_GetUnitName(parent).c_str(),
                    JevAi_GetUnitName(leaving).c_str());
        }
    }

    return false;
}

void JevAi::BalanceEconomy() {
    // what a human does in the allocation menu: mine enough fuel for every power generator, then raw materials,
    // then gold, and switch on the generators the fuel can run
    std::vector<Complex*> complexes;

    for (auto it = UnitsManager_StationaryUnits.Begin(), it_end = UnitsManager_StationaryUnits.End(); it != it_end;
         ++it) {
        if ((*it).team == m_team && (*it).hits > 0 && (*it).GetComplex() &&
            std::find(complexes.begin(), complexes.end(), (*it).GetComplex()) == complexes.end()) {
            complexes.push_back((*it).GetComplex());
        }
    }

    const int32_t generator_fuel = Cargo_GetFuelConsumptionRate(POWGEN);

    for (Complex* complex : complexes) {
        int32_t generators = 0;

        for (auto it = UnitsManager_StationaryUnits.Begin(), it_end = UnitsManager_StationaryUnits.End(); it != it_end;
             ++it) {
            if ((*it).GetComplex() == complex && (*it).GetUnitType() == POWGEN && (*it).hits > 0 &&
                (*it).GetOrder() != ORDER_IDLE) {
                ++generators;
            }
        }

        int32_t fuel_needed = generators * generator_fuel;
        int32_t fuel_mined = 0;

        for (auto it = UnitsManager_StationaryUnits.Begin(), it_end = UnitsManager_StationaryUnits.End(); it != it_end;
             ++it) {
            UnitInfo& station = *it;

            if (station.GetComplex() != complex || station.GetUnitType() != MININGST || station.hits == 0 ||
                station.GetOrder() != ORDER_POWER_ON) {
                continue;
            }

            int32_t capacity = 16;

            station.fuel_mining = std::min<int32_t>({station.fuel_mining_max, std::max(0, fuel_needed), capacity});
            capacity -= station.fuel_mining;
            fuel_needed -= station.fuel_mining;
            station.raw_mining = std::min<int32_t>(station.raw_mining_max, capacity);
            capacity -= station.raw_mining;
            station.gold_mining = std::min<int32_t>(station.gold_mining_max, capacity);
            capacity -= station.gold_mining;

            // spare capacity goes to more fuel
            const int32_t extra_fuel = std::min<int32_t>(station.fuel_mining_max - station.fuel_mining, capacity);

            station.fuel_mining += extra_fuel;
            station.total_mining = station.raw_mining + station.fuel_mining + station.gold_mining;
            fuel_mined += station.fuel_mining;
        }

        Access_UpdateResourcesTotal(complex);

        int32_t generators_on = 0;

        for (auto it = UnitsManager_StationaryUnits.Begin(), it_end = UnitsManager_StationaryUnits.End(); it != it_end;
             ++it) {
            if ((*it).GetComplex() == complex && (*it).GetUnitType() == POWGEN && (*it).hits > 0 &&
                (*it).GetOrder() == ORDER_POWER_ON) {
                ++generators_on;
            }
        }

        for (auto it = UnitsManager_StationaryUnits.Begin(), it_end = UnitsManager_StationaryUnits.End(); it != it_end;
             ++it) {
            if ((*it).GetComplex() == complex && (*it).GetUnitType() == POWGEN && (*it).hits > 0 &&
                (*it).GetOrder() == ORDER_POWER_OFF && fuel_mined >= (generators_on + 1) * generator_fuel) {
                UnitsManager_SetNewOrder(it->Get(), ORDER_POWER_ON, ORDER_STATE_INIT);

                ++generators_on;
            }
        }
    }
}

UnitInfo* JevAi::FindIdleFactory() const {
    for (auto it = UnitsManager_StationaryUnits.Begin(), it_end = UnitsManager_StationaryUnits.End(); it != it_end;
         ++it) {
        UnitInfo& unit = *it;

        // a factory that is still expanding after construction is not connected to the base yet
        const auto order = unit.GetOrder();
        const bool is_settled = (order == ORDER_AWAIT || order == ORDER_POWER_ON || order == ORDER_POWER_OFF ||
                                 order == ORDER_HALT_BUILDING || order == ORDER_HALT_BUILDING_2) &&
                                unit.GetOrderState() == ORDER_STATE_EXECUTING_ORDER;

        if (unit.team == m_team && unit.hits > 0 && JevAi_IsFactory(unit.GetUnitType()) && is_settled &&
            std::find(m_handled.begin(), m_handled.end(), SmartPointer<UnitInfo>(&unit)) == m_handled.end()) {
            return &unit;
        }
    }

    return nullptr;
}

void JevAi::AddMoveOptions(UnitInfo* unit) {
    const Point position(unit->grid_x, unit->grid_y);
    const int32_t scan = unit->GetBaseValues()->GetAttribute(ATTRIB_SCAN);
    const int32_t speed = unit->speed;
    int32_t nearest_enemy;

    if (speed == 0) {
        return;
    }

    for (int32_t direction = 0; direction < 8; ++direction) {
        const bool is_diagonal = DIRECTION_OFFSETS[direction].x != 0 && DIRECTION_OFFSETS[direction].y != 0;
        const int32_t full = is_diagonal ? std::max(1, speed * 2 / 3) : speed;
        const int32_t distances[2] = {std::max(1, full / 2), full};

        for (int32_t i = 0; i < 2; ++i) {
            if (i == 1 && distances[1] == distances[0]) {
                continue;
            }

            Point destination = position;

            destination.x += DIRECTION_OFFSETS[direction].x * distances[i];
            destination.y += DIRECTION_OFFSETS[direction].y * distances[i];

            if (!JevAi_IsInsideMap(destination) ||
                !Access_IsAccessible(unit->GetUnitType(), m_team, destination.x, destination.y,
                                     AccessModifier_SameClassBlocks)) {
                continue;
            }

            const int32_t revealed = CountUnexplored(destination, scan);
            const int32_t danger = GetDanger(unit, destination, &nearest_enemy);
            std::string facts = std::string("move ") + JevAi_DirectionNames[direction] + " " +
                                std::to_string(distances[i]) + " cells to " + JevAi_FormatPoint(destination) +
                                ": reveals " + std::to_string(revealed) + " unexplored cells, danger " +
                                std::to_string(danger);

            if (revealed == 0) {
                facts +=
                    ", nearest unexplored cell " + std::to_string(GetDistanceToUnexplored(destination)) + " cells away";
            }

            if (nearest_enemy >= 0) {
                facts += ", nearest known enemy " + std::to_string(nearest_enemy) + " cells away";
            }

            if (unit->GetUnitType() == SURVEYOR) {
                int16_t raw;
                int16_t gold;
                int16_t fuel;

                Survey_GetTotalResourcesInArea(destination.x - 1, destination.y - 1, 2, &raw, &gold, &fuel, true,
                                               m_team);

                facts += ", resources there raw " + std::to_string(raw) + " fuel " + std::to_string(fuel) + " gold " +
                         std::to_string(gold);
            }

            m_options.push_back({"m" + std::to_string(m_options.size()), Action::MOVE, destination, INVALID_ID, nullptr,
                                 facts, revealed - danger * 10});
        }
    }
}

void JevAi::AddBuildOptions(UnitInfo* builder) {
    if (!Task_IsReadyToTakeOrders(builder)) {
        return;
    }

    const int32_t rate = Cargo_GetRawConsumptionRate(builder->GetUnitType(), 1);
    int32_t spare_power = 0;
    int32_t spare_fuel = 0;
    bool enemies_seen = false;

    for (auto it = UnitsManager_StationaryUnits.Begin(), it_end = UnitsManager_StationaryUnits.End(); it != it_end;
         ++it) {
        if ((*it).team == m_team && (*it).GetComplex()) {
            Cargo mining;
            Cargo mining_capacity;
            int32_t burned = 0;

            (*it).GetComplex()->GetCargoMining(mining, mining_capacity);

            for (auto it2 = UnitsManager_StationaryUnits.Begin(), it2_end = UnitsManager_StationaryUnits.End();
                 it2 != it2_end; ++it2) {
                if ((*it2).GetComplex() == (*it).GetComplex() && (*it2).GetUnitType() == POWGEN &&
                    (*it2).GetOrder() == ORDER_POWER_ON) {
                    burned += Cargo_GetFuelConsumptionRate(POWGEN);
                }
            }

            spare_power = (*it).GetComplex()->power;
            spare_fuel = mining_capacity.fuel - burned;
            break;
        }
    }

    for (auto* list : {&UnitsManager_MobileLandSeaUnits, &UnitsManager_StationaryUnits}) {
        for (auto it = list->Begin(), it_end = list->End(); it != it_end; ++it) {
            if ((*it).team != m_team && (*it).team != PLAYER_TEAM_ALIEN && (*it).hits > 0 &&
                (*it).IsVisibleToTeam(m_team)) {
                enemies_seen = true;
            }
        }
    }

    for (const ResourceID unit_type : Builder_GetBuildableUnits(m_team, builder->GetUnitType())) {
        Point site;

        // the game only offers buildings that make sense now
        if (unit_type == POWGEN) {
            if (spare_power > 0 || spare_fuel < Cargo_GetFuelConsumptionRate(POWGEN)) {
                continue;
            }

        } else if (JevAi_IsFactory(unit_type)) {
            if ((unit_type != LIGHTPLT && unit_type != LANDPLT) || CountOwnUnits(unit_type) >= 2) {
                continue;
            }

        } else if (unit_type == GUNTURRT || unit_type == ARTYTRRT || unit_type == ANTIMSSL || unit_type == RADAR) {
            if (!enemies_seen) {
                continue;
            }

        } else if (unit_type != MININGST) {
            continue;
        }

        if (!FindBuildSite(builder, unit_type, site)) {
            continue;
        }

        const int32_t turns = BuildMenu_GetTurnsToBuild(unit_type, m_team);
        const int32_t cost = turns * rate;
        const bool covers_builder = (site.x == builder->grid_x || site.x + 1 == builder->grid_x) &&
                                    (site.y == builder->grid_y || site.y + 1 == builder->grid_y);
        std::string facts = "build a " + JevAi_GetTypeName(unit_type) + " at " + JevAi_FormatPoint(site) + ": " +
                            JevAi_DescribePurpose(m_team, unit_type) + " " + std::to_string(turns) + " turns, " +
                            std::to_string(cost) + " raw materials, you carry " + std::to_string(builder->storage) +
                            ", you have " + std::to_string(CountOwnUnits(unit_type)) + " already";
        int32_t score = 0;

        if (!covers_builder) {
            facts += ", the builder walks there first";
        }

        if (unit_type == MININGST) {
            int16_t raw;
            int16_t gold;
            int16_t fuel;

            Survey_GetTotalResourcesInArea(site.x, site.y, 1, &raw, &gold, &fuel, false, m_team);

            if (raw + fuel + gold == 0) {
                continue;
            }

            facts += ", surveyed resources under it raw " + std::to_string(raw) + " fuel " + std::to_string(fuel) +
                     " gold " + std::to_string(gold);
            score = (raw + fuel + gold) > 0 ? 40 + raw + fuel + gold : -10;

        } else {
            facts += TouchesBase(unit_type, site) ? ", joins your base" : ", not connected to your base";

            if (JevAi_IsFactory(unit_type)) {
                score = (CountOwnUnits(unit_type) == 0) ? 60 : 5;

            } else if (unit_type == POWGEN) {
                score = 20;
            }

            if (unit_type == POWGEN) {
                facts += ", burns " + std::to_string(Cargo_GetFuelConsumptionRate(POWGEN)) + " fuel per turn for " +
                         std::to_string(-Cargo_GetPowerConsumptionRate(POWGEN)) +
                         " power; the base has no spare power and mines " + std::to_string(spare_fuel) +
                         " spare fuel per turn";

            } else if (Cargo_GetPowerConsumptionRate(unit_type) > 0) {
                facts += ", needs " + std::to_string(Cargo_GetPowerConsumptionRate(unit_type)) + " power";
            }
        }

        if (cost > builder->storage) {
            facts += ", not enough materials carried";
            score = -50;
        }

        if (JevAi_GetSize(unit_type) == 1 && unit_type != MININGST) {
            if (UnitsManager_GetCurrentUnitValues(&UnitsManager_TeamInfo[m_team], unit_type)
                    ->GetAttribute(ATTRIB_ATTACK) > 0) {
                facts += ", a defense: " + JevAi_DescribeType(m_team, unit_type);
            }
        }

        m_options.push_back(
            {"b" + std::to_string(m_options.size()), Action::BUILD, site, unit_type, nullptr, facts, score});
    }
}

void JevAi::AddProduceOptions(UnitInfo* factory) {
    Cargo materials;
    Cargo capacity;

    if (factory->GetComplex()) {
        factory->GetComplex()->GetCargoInfo(materials, capacity);
    }

    const int32_t rate = Cargo_GetRawConsumptionRate(factory->GetUnitType(), 1);

    for (const ResourceID unit_type : Builder_GetBuildableUnits(m_team, factory->GetUnitType())) {
        const int32_t turns = BuildMenu_GetTurnsToBuild(unit_type, m_team);
        std::string facts = "produce a " + JevAi_GetTypeName(unit_type) + ": " +
                            JevAi_DescribePurpose(m_team, unit_type) + " " + std::to_string(turns) + " turns, " +
                            std::to_string(turns * rate) + " raw materials, " + JevAi_DescribeType(m_team, unit_type) +
                            ", you have " + std::to_string(CountOwnUnits(unit_type));
        UnitValues* values = UnitsManager_GetCurrentUnitValues(&UnitsManager_TeamInfo[m_team], unit_type);
        int32_t score = values->GetAttribute(ATTRIB_ATTACK) - CountOwnUnits(unit_type) * 5;

        if (unit_type == CONSTRCT || unit_type == ENGINEER || unit_type == SURVEYOR) {
            score = (CountOwnUnits(unit_type) < 2) ? 30 : -5;
        }

        m_options.push_back(
            {"p" + std::to_string(m_options.size()), Action::PRODUCE, Point(), unit_type, nullptr, facts, score});
    }

    m_options.push_back({"nothing", Action::STAY, Point(), INVALID_ID, nullptr,
                         "produce nothing this turn; the base has " + std::to_string(materials.raw) + " of " +
                             std::to_string(capacity.raw) + " raw materials stored",
                         0});
}

void JevAi::AddRefillOptions(UnitInfo* builder) {
    const int32_t capacity = builder->GetBaseValues()->GetAttribute(ATTRIB_STORAGE);

    if (builder->storage >= capacity || !Task_IsReadyToTakeOrders(builder)) {
        return;
    }

    UnitInfo* nearest = nullptr;
    Point nearest_cell;
    int32_t nearest_distance = INT32_MAX;
    Cargo nearest_materials;

    for (auto it = UnitsManager_StationaryUnits.Begin(), it_end = UnitsManager_StationaryUnits.End(); it != it_end;
         ++it) {
        UnitInfo& building = *it;

        if (building.team != m_team || building.hits == 0 || !building.GetComplex() || !(building.flags & BUILDING) ||
            building.GetOrder() == ORDER_TRANSFER) {
            continue;
        }

        Cargo materials;
        Cargo storage_capacity;

        building.GetComplex()->GetCargoInfo(materials, storage_capacity);

        if (materials.raw <= 0) {
            continue;
        }

        Rect bounds;

        building.GetBounds(&bounds);

        const int32_t dx = std::max({bounds.ulx - builder->grid_x, 0, builder->grid_x - (bounds.lrx - 1)});
        const int32_t dy = std::max({bounds.uly - builder->grid_y, 0, builder->grid_y - (bounds.lry - 1)});

        if (std::max(dx, dy) <= 1) {
            const int32_t amount = std::min<int32_t>(capacity - builder->storage, materials.raw);
            UnitInfo* source = nullptr;

            // the base shares its materials, but they leave from a building that stores raw materials
            for (auto it2 = UnitsManager_StationaryUnits.Begin(), it2_end = UnitsManager_StationaryUnits.End();
                 it2 != it2_end; ++it2) {
                if ((*it2).GetComplex() == building.GetComplex() && (*it2).hits > 0 &&
                    (*it2).GetOrder() != ORDER_TRANSFER &&
                    ResourceManager_GetUnit((*it2).GetUnitType()).GetCargoType() == Unit::CargoType::CARGO_TYPE_RAW &&
                    (!source || (*it2).storage > source->storage)) {
                    source = it2->Get();
                }
            }

            if (!source) {
                continue;
            }

            m_options.push_back({"r" + std::to_string(m_options.size()), Action::REFILL, Point(), INVALID_ID,
                                 SmartPointer<UnitInfo>(source),
                                 "take " + std::to_string(amount) + " raw materials from the " +
                                     JevAi_GetUnitName(&building) + " next to you, you carry " +
                                     std::to_string(builder->storage) + " of " + std::to_string(capacity) +
                                     ", the base stores " + std::to_string(materials.raw),
                                 builder->storage < capacity / 2 ? 45 : 5});

            return;
        }

        // the closest free cell around the building
        for (int32_t x = bounds.ulx - 1; x <= bounds.lrx; ++x) {
            for (int32_t y = bounds.uly - 1; y <= bounds.lry; ++y) {
                const Point cell(x, y);

                if (JevAi_IsInsideMap(cell) &&
                    Access_IsAccessible(builder->GetUnitType(), m_team, x, y, AccessModifier_SameClassBlocks)) {
                    const int32_t distance = Access_GetSquaredDistance(cell, Point(builder->grid_x, builder->grid_y));

                    if (distance < nearest_distance) {
                        nearest = &building;
                        nearest_cell = cell;
                        nearest_distance = distance;
                        nearest_materials = materials;
                    }
                }
            }
        }
    }

    if (nearest && builder->speed > 0) {
        m_options.push_back({"g" + std::to_string(m_options.size()), Action::MOVE, nearest_cell, INVALID_ID, nullptr,
                             "go next to the " + JevAi_GetUnitName(nearest) + " at " + JevAi_FormatPoint(nearest_cell) +
                                 ", " + std::to_string(static_cast<int32_t>(SDL_sqrt(nearest_distance))) +
                                 " cells away, to take raw materials there; you carry " +
                                 std::to_string(builder->storage) + " of " + std::to_string(capacity) +
                                 ", the base stores " + std::to_string(nearest_materials.raw),
                             builder->storage < capacity / 2 ? 35 : 0});
    }
}

void JevAi::AddAttackOptions(UnitInfo* unit) {
    UnitValues* values = unit->GetBaseValues();

    if (values->GetAttribute(ATTRIB_ATTACK) == 0 || unit->ammo == 0 || unit->shots == 0) {
        return;
    }

    const int32_t reach = values->GetAttribute(ATTRIB_RANGE) + unit->speed;

    for (auto* list : {&UnitsManager_MobileLandSeaUnits, &UnitsManager_MobileAirUnits, &UnitsManager_StationaryUnits}) {
        for (auto it = list->Begin(), it_end = list->End(); it != it_end; ++it) {
            UnitInfo& target = *it;

            if (target.team == m_team || target.hits == 0 || !target.IsVisibleToTeam(m_team) ||
                target.GetUnitType() == CNCT_4W || target.GetUnitType() == ROAD ||
                !Access_IsValidAttackTarget(unit, &target)) {
                continue;
            }

            const int32_t distance = Access_GetSquaredDistance(unit, &target);

            if (distance > reach * reach) {
                continue;
            }

            UnitValues* target_values = target.GetBaseValues();
            const int32_t damage =
                std::max(1, values->GetAttribute(ATTRIB_ATTACK) - target_values->GetAttribute(ATTRIB_ARMOR));
            const bool shoots_back = target_values->GetAttribute(ATTRIB_ATTACK) > 0 &&
                                     Access_IsValidAttackTargetType(target.GetUnitType(), unit->GetUnitType());
            const bool in_range = distance <= values->GetAttribute(ATTRIB_RANGE) * values->GetAttribute(ATTRIB_RANGE);
            std::string facts =
                "attack the enemy " + JevAi_GetUnitName(&target) + " at " +
                JevAi_FormatPoint(Point(target.grid_x, target.grid_y)) + ": it has " + std::to_string(target.hits) +
                " of " + std::to_string(target_values->GetAttribute(ATTRIB_HITS)) + " hits, each shot does about " +
                std::to_string(damage) + " damage, you have " + std::to_string(unit->shots) + " shots this turn";

            facts += in_range ? ", in range now" : ", you move closer first";
            facts += shoots_back ? ", it can shoot back with attack " +
                                       std::to_string(target_values->GetAttribute(ATTRIB_ATTACK))
                                 : ", it cannot shoot back";

            if (target.hits <= damage * unit->shots) {
                facts += ", it can be destroyed this turn";
            }

            m_options.push_back({"a" + std::to_string(m_options.size()), Action::ATTACK,
                                 Point(target.grid_x, target.grid_y), INVALID_ID, SmartPointer<UnitInfo>(&target),
                                 facts, 50 + damage * 5 - (shoots_back ? 10 : 0)});
        }
    }
}

json JevAi::GetState(UnitInfo* unit) const {
    json state;
    json buildings = json::object();
    json units = json::object();
    int32_t nearest_enemy;
    int32_t enemies_seen = 0;

    for (auto* list : {&UnitsManager_MobileLandSeaUnits, &UnitsManager_MobileAirUnits, &UnitsManager_StationaryUnits}) {
        for (auto it = list->Begin(), it_end = list->End(); it != it_end; ++it) {
            if ((*it).hits == 0) {
                continue;
            }

            if ((*it).team == m_team) {
                const std::string name = JevAi_GetUnitName(it->Get());
                json& counts = ((*it).flags & STATIONARY) ? buildings : units;

                if ((*it).GetUnitType() != CNCT_4W && (*it).GetUnitType() != ROAD) {
                    counts[name] = counts.value(name, 0) + 1;
                }

            } else if ((*it).IsVisibleToTeam(m_team)) {
                ++enemies_seen;
            }
        }
    }

    state["game"] = "M.A.X., turn-based strategy";
    state["turn"] = GameManager_TurnCounter;
    state["you"] = {{"type", JevAi_GetUnitName(unit)},
                    {"position", {unit->grid_x + 1, unit->grid_y + 1}},
                    {"hits", unit->hits},
                    {"max_hits", unit->GetBaseValues()->GetAttribute(ATTRIB_HITS)}};

    if (!(unit->flags & STATIONARY)) {
        state["you"]["moves_left"] = unit->speed;
        state["you"]["danger_here"] = GetDanger(unit, Point(unit->grid_x, unit->grid_y), &nearest_enemy);

        if (JevAi_IsBuilder(unit->GetUnitType())) {
            state["you"]["materials_carried"] = unit->storage;
            state["you"]["materials_capacity"] = unit->GetBaseValues()->GetAttribute(ATTRIB_STORAGE);
        }
    }

    for (auto it = UnitsManager_StationaryUnits.Begin(), it_end = UnitsManager_StationaryUnits.End(); it != it_end;
         ++it) {
        if ((*it).team == m_team && (*it).GetComplex()) {
            Cargo materials;
            Cargo capacity;

            (*it).GetComplex()->GetCargoInfo(materials, capacity);

            Cargo mining;
            Cargo mining_capacity;

            (*it).GetComplex()->GetCargoMining(mining, mining_capacity);

            state["mining_per_turn"] = {{"raw", mining.raw}, {"fuel", mining.fuel}, {"gold", mining.gold}};
            state["base"] = {{"raw", materials.raw},
                             {"raw_capacity", capacity.raw},
                             {"fuel", materials.fuel},
                             {"fuel_capacity", capacity.fuel},
                             {"gold", materials.gold},
                             {"power_left", (*it).GetComplex()->power},
                             {"workers_left", (*it).GetComplex()->workers}};
            break;
        }
    }

    state["buildings"] = buildings;
    state["units"] = units;
    state["explored_cells"] = m_explored_cells;
    state["map_cells"] = ResourceManager_MapSize.x * ResourceManager_MapSize.y;
    state["enemies_seen"] = enemies_seen;
    state["units_lost"] = m_units_lost;

    return state;
}

bool JevAi::Ask(UnitInfo* unit, const char* instructions) {
    json criteria;
    json scores;

    for (const auto& option : m_options) {
        criteria[option.key] = option.facts;
        scores[option.key] = option.score;
    }

    json request;

    request["id"] = ++m_request_id;
    request["team"] = GetTeamName();
    request["state"] = GetState(unit);
    request["questions"] = {{m_question, {{"type", "choice"}, {"instructions", instructions}, {"criteria", criteria}}}};
    request["mock_scores"] = scores;

    ++m_turn_questions;

    return m_bridge.Send(request.dump());
}

void JevAi::StartBuilding(UnitInfo* builder, ResourceID unit_type, Point site) {
    // same sequence as the classic TaskCreateBuilding and TaskCreateUnit
    SmartObjectArray<ResourceID> build_list = builder->GetBuildList();

    build_list.Clear();
    build_list.PushBack(&unit_type);

    // the classic rule: a builder with a full cargo builds at double speed
    if (!(builder->flags & STATIONARY) &&
        builder->storage >= builder->GetBaseValues()->GetAttribute(ATTRIB_STORAGE) - 2) {
        builder->SetBuildRate(2);

    } else {
        builder->SetBuildRate(1);
    }

    builder->move_to_grid_x = site.x;
    builder->move_to_grid_y = site.y;
    builder->path = nullptr;

    if (Remote_IsNetworkGame) {
        Remote_SendNetPacket_38(builder);
    }

    builder->BuildOrder();

    ++m_turn_builds;
}

void JevAi::Apply(const std::string& answer) {
    std::string choice;
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
            choice = reply.value("choice", "");
            model = reply.value("model", "?");
            confidence = reply.value("confidence", 0.0);
        }

    } catch (const json::exception& exception) {
        SDL_Log("Jev %s: unreadable answer: %s", GetTeamName(), exception.what());
    }

    auto option =
        std::find_if(m_options.begin(), m_options.end(), [&choice](const Option& item) { return item.key == choice; });

    if (option == m_options.end()) {
        // the last option is always stay or nothing
        option = m_options.end() - 1;
    }

    SDL_Log("Jev %s: %s at %s chose %s (%s), model %s, confidence %.2f.", GetTeamName(),
            JevAi_GetUnitName(&*m_unit).c_str(), JevAi_FormatPoint(Point(m_unit->grid_x, m_unit->grid_y)).c_str(),
            option->key.c_str(), option->facts.c_str(), model.c_str(), confidence);

    m_last_action = option->action;

    switch (option->action) {
        case Action::STAY: {
            FinishUnit();
        } break;

        case Action::PRODUCE: {
            BalanceEconomy();
            StartBuilding(&*m_unit, option->unit_type, Point(0, 0));

            m_wait_start = SDL_GetTicks();
            m_state = State::WAIT_FOR_UNIT;
        } break;

        case Action::BUILD: {
            const int32_t size = JevAi_GetSize(option->unit_type);

            if (m_unit->grid_x >= option->destination.x && m_unit->grid_x < option->destination.x + size &&
                m_unit->grid_y >= option->destination.y && m_unit->grid_y < option->destination.y + size) {
                StartBuilding(&*m_unit, option->unit_type, option->destination);
                FinishUnit();

            } else {
                m_pending_build = *option;
                m_has_pending_build = true;
                m_move_start = Point(m_unit->grid_x, m_unit->grid_y);
                m_unit->move_to_grid_x = option->destination.x;
                m_unit->move_to_grid_y = option->destination.y;

                UnitsManager_SetNewOrder(&*m_unit, ORDER_MOVE, ORDER_STATE_INIT);

                m_wait_start = SDL_GetTicks();
                m_state = State::WAIT_FOR_UNIT;
            }
        } break;

        case Action::REFILL: {
            // the classic TaskGetMaterials sequence
            UnitInfo* source = &*option->target;
            Cargo materials;
            Cargo capacity;

            source->GetComplex()->GetCargoInfo(materials, capacity);

            source->transfer_cargo = std::min<int32_t>(
                m_unit->GetBaseValues()->GetAttribute(ATTRIB_STORAGE) - m_unit->storage, materials.raw);
            source->SetParent(&*m_unit);

            UnitsManager_SetNewOrder(source, ORDER_TRANSFER, ORDER_STATE_INIT);

            m_refill_source = source;

            m_wait_start = SDL_GetTicks();
            m_state = State::WAIT_FOR_UNIT;
        } break;

        case Action::ATTACK: {
            m_move_start = Point(m_unit->grid_x, m_unit->grid_y);

            // same order as the classic AiAttack_EvaluateAttack and the human attack click
            m_unit->SetEnemy(&*option->target);
            m_unit->path = nullptr;

            UnitsManager_SetNewOrder(&*m_unit, ORDER_MOVE_TO_ATTACK, ORDER_STATE_EXECUTING_ORDER);

            ++m_turn_attacks;
            m_wait_start = SDL_GetTicks();
            m_state = State::WAIT_FOR_UNIT;
        } break;

        case Action::MOVE: {
            m_move_start = Point(m_unit->grid_x, m_unit->grid_y);
            m_unit->move_to_grid_x = option->destination.x;
            m_unit->move_to_grid_y = option->destination.y;

            UnitsManager_SetNewOrder(&*m_unit, ORDER_MOVE, ORDER_STATE_INIT);

            ++m_turn_moves;
            m_wait_start = SDL_GetTicks();
            m_state = State::WAIT_FOR_UNIT;
        } break;
    }
}

void JevAi::FinishUnit() {
    if (m_unit) {
        m_units.erase(std::remove(m_units.begin(), m_units.end(), m_unit), m_units.end());
        m_handled.push_back(m_unit);
        m_unit = nullptr;
    }

    m_unit_picks = 0;
    m_has_pending_build = false;
    m_state = State::SELECT;
}

void JevAi::BeginTurn() {
    // a new game; the controllers outlive games
    if (GameManager_TurnCounter <= m_last_turn) {
        m_explored.clear();
        m_explored_cells = 0;
        m_units_lost = 0;
        m_lost.clear();
    }

    m_last_turn = GameManager_TurnCounter;
    m_turn_done_logged = false;
    m_units.clear();
    m_handled.clear();
    m_unit = nullptr;
    m_unit_picks = 0;
    m_has_pending_build = false;
    m_turn_questions = 0;
    m_turn_moves = 0;
    m_turn_failed_moves = 0;
    m_turn_builds = 0;
    m_turn_attacks = 0;
    m_state = State::SELECT;

    for (auto it = UnitsManager_MobileLandSeaUnits.Begin(), it_end = UnitsManager_MobileLandSeaUnits.End();
         it != it_end; ++it) {
        if ((*it).team == m_team && (*it).hits > 0 && ((*it).flags & MOBILE_LAND_UNIT)) {
            m_units.push_back(SmartPointer<UnitInfo>(it->Get()));
        }
    }

    BalanceEconomy();

    std::string buildings;
    Complex* complex = nullptr;

    for (auto it = UnitsManager_StationaryUnits.Begin(), it_end = UnitsManager_StationaryUnits.End(); it != it_end;
         ++it) {
        if ((*it).team == m_team && (*it).GetUnitType() != CNCT_4W && (*it).GetUnitType() != ROAD) {
            buildings += " " + JevAi_GetUnitName(it->Get()) + " (" + UnitsManager_Orders[(*it).GetOrder()] + " " +
                         std::to_string((*it).GetOrderState()) + " at " +
                         JevAi_FormatPoint(Point((*it).grid_x, (*it).grid_y)) + " complex " +
                         std::to_string((*it).GetComplex() ? (*it).GetComplex()->GetId() : -1) + ")";

            if ((*it).GetUnitType() == MININGST) {
                buildings += "[mining raw " + std::to_string((*it).raw_mining) + "/" +
                             std::to_string((*it).raw_mining_max) + " fuel " + std::to_string((*it).fuel_mining) + "/" +
                             std::to_string((*it).fuel_mining_max) + " gold " + std::to_string((*it).gold_mining) +
                             "/" + std::to_string((*it).gold_mining_max) + "]";
            }

            if (!complex) {
                complex = (*it).GetComplex();
            }
        }
    }

    if (complex) {
        Cargo materials;
        Cargo capacity;

        complex->GetCargoInfo(materials, capacity);

        buildings += "; base raw " + std::to_string(materials.raw) + " fuel " + std::to_string(materials.fuel) +
                     " gold " + std::to_string(materials.gold) + " power left " + std::to_string(complex->power);
    }

    SDL_Log("Jev %s: turn %d, %d units, buildings:%s", GetTeamName(), GameManager_TurnCounter,
            static_cast<int32_t>(m_units.size()), buildings.c_str());
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

        case State::WAIT_FOR_UNIT: {
            if (!m_unit || m_unit->hits == 0) {
                FinishUnit();

                return false;
            }

            if (m_last_action == Action::REFILL) {
                if (m_refill_source && m_refill_source->GetOrder() == ORDER_TRANSFER &&
                    SDL_GetTicks() - m_wait_start < JevAi_UnitTimeout) {
                    return false;
                }

                SDL_Log("Jev %s: %s now carries %d raw materials.", GetTeamName(), JevAi_GetUnitName(&*m_unit).c_str(),
                        m_unit->storage);

                m_refill_source = nullptr;
                m_state = State::SELECT;

                return false;
            }

            if (m_last_action == Action::PRODUCE) {
                if (m_unit->GetOrder() == ORDER_BUILD && m_unit->GetOrderState() == ORDER_STATE_INIT &&
                    SDL_GetTicks() - m_wait_start < JevAi_UnitTimeout) {
                    return false;
                }

                // as for a human player: rebalance mining and power so that the new production keeps running
                if (m_unit->GetComplex()) {
                    ProductionManager_OptimizeProduction(m_team, m_unit->GetComplex(), &*m_unit, false);
                }

                SDL_Log("Jev %s: %s is %s, base balance raw %d fuel %d power %d.", GetTeamName(),
                        JevAi_GetUnitName(&*m_unit).c_str(), UnitsManager_Orders[m_unit->GetOrder()],
                        m_unit->GetComplex() ? m_unit->GetComplex()->material : 0,
                        m_unit->GetComplex() ? m_unit->GetComplex()->fuel : 0,
                        m_unit->GetComplex() ? m_unit->GetComplex()->power : 0);

                FinishUnit();

                return false;
            }

            const bool is_idle = (Task_IsReadyToTakeOrders(&*m_unit) || (m_unit->flags & STATIONARY)) &&
                                 m_unit->GetOrder() != ORDER_ACTIVATE && !UnitsManager_CombatEffectsActive;

            if (!is_idle && SDL_GetTicks() - m_wait_start < JevAi_UnitTimeout) {
                return false;
            }

            if (m_has_pending_build) {
                const int32_t size = JevAi_GetSize(m_pending_build.unit_type);
                const Point site = m_pending_build.destination;

                if (is_idle && m_unit->grid_x >= site.x && m_unit->grid_x < site.x + size && m_unit->grid_y >= site.y &&
                    m_unit->grid_y < site.y + size &&
                    Access_IsAccessible(m_pending_build.unit_type, m_team, site.x, site.y,
                                        AccessModifier_EnemySameClassBlocks)) {
                    StartBuilding(&*m_unit, m_pending_build.unit_type, site);

                } else {
                    SDL_Log("Jev %s: %s did not reach the %s site.", GetTeamName(), JevAi_GetUnitName(&*m_unit).c_str(),
                            JevAi_GetTypeName(m_pending_build.unit_type).c_str());
                }

                FinishUnit();

                return false;
            }

            if (m_unit->flags & STATIONARY || m_unit->GetOrder() == ORDER_BUILD ||
                !(m_unit->flags & MOBILE_LAND_UNIT)) {
                // a release finished
                m_unit = nullptr;
                m_state = State::SELECT;

                return false;
            }

            if (m_last_action == Action::MOVE && Point(m_unit->grid_x, m_unit->grid_y) == m_move_start) {
                ++m_turn_failed_moves;

                SDL_Log("Jev %s: %s did not move.", GetTeamName(), JevAi_GetUnitName(&*m_unit).c_str());
            }

            if (!is_idle || m_unit->speed == 0 || m_unit_picks >= JevAi_MaxPicksPerUnit) {
                FinishUnit();

            } else {
                m_state = State::SELECT;
            }

            return false;
        }

        case State::SELECT: {
            if (UnitsManager_CombatEffectsActive) {
                return false;
            }

            if (m_turn_questions < JevAi_MaxQuestionsPerTurn) {
                if (!m_unit && ReleaseFinishedUnit()) {
                    return true;
                }

                if (!m_unit) {
                    UnitInfo* factory = FindIdleFactory();

                    if (factory) {
                        m_unit = factory;
                        m_options.clear();
                        AddProduceOptions(factory);
                        m_question = "produce";

                        if (m_options.size() > 1 && Ask(factory, JevAi_FactoryInstructions)) {
                            m_wait_start = SDL_GetTicks();
                            m_state = State::WAIT_FOR_ANSWER;

                        } else {
                            FinishUnit();
                        }

                        return true;
                    }
                }

                if (!m_unit) {
                    // the next unit that can still act
                    while (!m_units.empty()) {
                        auto& unit = m_units.front();

                        if (unit->hits > 0 && Task_IsReadyToTakeOrders(&*unit) &&
                            (unit->speed > 0 || JevAi_IsBuilder(unit->GetUnitType()))) {
                            m_unit = unit;
                            break;
                        }

                        m_units.erase(m_units.begin());
                    }
                }
            }

            if (!m_unit) {
                if (!m_turn_done_logged) {
                    m_turn_done_logged = true;

                    SDL_Log(
                        "Jev %s: turn %d done: %d questions, %d moves, %d did not move, %d builds started, %d "
                        "attacks, %d cells explored, %d units lost.",
                        GetTeamName(), GameManager_TurnCounter, m_turn_questions, m_turn_moves, m_turn_failed_moves,
                        m_turn_builds, m_turn_attacks, m_explored_cells, m_units_lost);
                }

                return AiController_EndTurn(m_team);
            }

            const int32_t danger_here = GetDanger(&*m_unit, Point(m_unit->grid_x, m_unit->grid_y), nullptr);

            m_options.clear();
            AddAttackOptions(&*m_unit);
            AddMoveOptions(&*m_unit);

            if (JevAi_IsBuilder(m_unit->GetUnitType())) {
                AddBuildOptions(&*m_unit);
                AddRefillOptions(&*m_unit);
            }

            m_options.push_back({"stay", Action::STAY, Point(m_unit->grid_x, m_unit->grid_y), INVALID_ID, nullptr,
                                 "stay where it is for the rest of this turn, danger " + std::to_string(danger_here),
                                 -danger_here * 10});
            m_question = "action";
            ++m_unit_picks;

            if (m_options.size() > 1 && Ask(&*m_unit, JevAi_UnitInstructions)) {
                m_wait_start = SDL_GetTicks();
                m_state = State::WAIT_FOR_ANSWER;

            } else {
                // without the bridge or without options the unit stays where it is
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
    // the removal notice can come more than once for the same unit
    if (unit->team == m_team && unit->hits == 0 && (unit->flags & MOBILE_LAND_UNIT) &&
        std::find(m_lost.begin(), m_lost.end(), unit->unit_id) == m_lost.end()) {
        m_lost.push_back(unit->unit_id);
        ++m_units_lost;

        SDL_Log("Jev %s: lost %s at %s.", GetTeamName(), JevAi_GetUnitName(unit).c_str(),
                JevAi_FormatPoint(Point(unit->grid_x, unit->grid_y)).c_str());
    }

    if (m_unit == unit) {
        FinishUnit();

    } else {
        m_units.erase(std::remove(m_units.begin(), m_units.end(), unit), m_units.end());
    }
}
