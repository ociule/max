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

/// Turns a builder keeps walking to a chosen building site.
static constexpr int32_t JevAi_SiteWalkTurns = 3;

/// Cells around a builder searched for building sites.
static constexpr int32_t JevAi_SiteSearchRadius = 6;

static const char* const JevAi_DirectionNames[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};

static const char* const JevAi_ConstructionInstructions =
    "You manage construction for your team in M.A.X., a turn-based strategy game on a map of square cells. Military "
    "units are commanded separately; your job is the economy and production that feed them. The game ends at a turn "
    "limit: the team with the most points wins, a tie goes to the team with the most valuable units and buildings, "
    "and a team with no Constructor and no factory is eliminated. Each working Eco-sphere scores 1 point every turn, "
    "so Eco-spheres built early score the most. Choose what this unit does now. Rules, most important first: 1. If "
    "your team has no factory, building one comes first; the Heavy "
    "Vehicle Plant is the only factory that makes tanks and other strong combat units, and new Constructors. 2. A "
    "building that needs power does not work while the base has no spare power; Engineers build Power Generators "
    "quickly, so start the factory and let an Engineer add power. 3. Keep builders out of danger. If your team has "
    "only one Constructor and no factory, losing it loses the game. 4. Put mining stations on surveyed resources to "
    "raise income; surveyors find those resources near the base. 5. Builders use the raw materials they carry; refill "
    "at the base when they carry too little. 6. Once a factory is started, build a Habitat for workers and then "
    "Eco-spheres, with Power Generators for them. Buildings that touch your base join it and share its materials, "
    "power and workers.";

static const char* const JevAi_FactoryInstructions =
    "You manage production for your team in M.A.X., a turn-based strategy game. Military units are commanded "
    "separately; you choose what this factory produces next, or nothing. A unit takes the given number of turns and "
    "uses raw materials from your base every turn while it is being built. Rules, most important first: 1. If your "
    "team has only one Constructor, produce another one first when this factory can, so that losing a builder does "
    "not lose the game. 2. Then produce combat units that fight ground units, a mix of strong and long range ones. 3. "
    "Produce nothing only when the base cannot afford anything useful.";

static const char* const JevAi_MilitaryInstructions =
    "You command one military or support unit of your team in M.A.X., a turn-based strategy game on a map of square "
    "cells. Construction is managed separately; your job is to find and destroy the enemy while keeping your base "
    "and your builders safe. Choose this unit's action now. Rules, most important first: 1. Attack enemies you can "
    "damage, above all ones that can be destroyed this turn or cannot shoot back. 2. If enemies are near your base, "
    "your Eco-spheres or your builders, move between them or attack them; the enemy goes for the team with the most "
    "points. 3. Do not move where the danger is higher than this unit's "
    "hits unless it can destroy the enemy there. 4. Otherwise explore to find the enemy: fast units go far, slow "
    "units stay within a few cells of the base until enemies are found.";

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

/// A map cell as the player sees it, counted from 1.
static nlohmann::ordered_json JevAi_PointData(Point point) { return {point.x + 1, point.y + 1}; }

static int32_t JevAi_GetDistance(Point a, Point b) {
    return static_cast<int32_t>(SDL_sqrt(Access_GetSquaredDistance(a, b)));
}

/// Every cell of a building site must be free, not only the top left one.
static bool JevAi_IsSiteFree(uint16_t team, ResourceID unit_type, Point site) {
    const int32_t size = (ResourceManager_GetUnit(unit_type).GetFlags() & BUILDING) ? 2 : 1;

    for (int32_t x = site.x; x < site.x + size; ++x) {
        for (int32_t y = site.y; y < site.y + size; ++y) {
            if (!Access_IsAccessible(unit_type, team, x, y, AccessModifier_EnemySameClassBlocks)) {
                return false;
            }
        }
    }

    return true;
}

/// Turns until the game ends, or -1 if it has no turn limit.
static int32_t JevAi_GetTurnsLeft() {
    if (ini_setting_victory_type != VICTORY_TYPE_DURATION) {
        return -1;
    }

    return std::max(0, ini_setting_victory_limit - static_cast<int32_t>(GameManager_TurnCounter));
}

static bool JevAi_IsFactory(ResourceID unit_type) {
    return unit_type == LIGHTPLT || unit_type == LANDPLT || unit_type == SHIPYARD || unit_type == AIRPLT ||
           unit_type == TRAINHAL;
}

static bool JevAi_IsBuilder(ResourceID unit_type) { return unit_type == CONSTRCT || unit_type == ENGINEER; }

/// Builders and surveyors answer construction questions, all other mobile units military ones.
static bool JevAi_IsConstructionUnit(ResourceID unit_type) {
    return JevAi_IsBuilder(unit_type) || unit_type == SURVEYOR;
}

/// Units that fight ground units. Scouts are left out, they are for seeing.
static bool JevAi_IsGroundCombatUnit(uint16_t team, ResourceID unit_type) {
    return unit_type != SCOUT &&
           UnitsManager_GetCurrentUnitValues(&UnitsManager_TeamInfo[team], unit_type)->GetAttribute(ATTRIB_ATTACK) >
               0 &&
           Access_IsValidAttackTargetType(unit_type, TANK);
}

static int32_t JevAi_GetSize(ResourceID unit_type) {
    return (ResourceManager_GetUnit(unit_type).GetFlags() & BUILDING) ? 2 : 1;
}

/// What a unit type is for, in a few words.
static std::string JevAi_DescribePurpose(uint16_t team, ResourceID unit_type) {
    if (JevAi_IsFactory(unit_type)) {
        std::string combat;
        std::string other;
        bool makes_constructors = false;

        for (const ResourceID product : Builder_GetBuildableUnits(team, unit_type)) {
            std::string& names = JevAi_IsGroundCombatUnit(team, product) ? combat : other;

            names += (names.empty() ? "" : ", ") + JevAi_GetTypeName(product);
            makes_constructors |= product == CONSTRCT;
        }

        std::string result = "factory; ";

        result += combat.empty() ? "it makes no combat units that fight ground units"
                                 : "combat units against ground: " + combat;

        if (!other.empty()) {
            result += "; other units: " + other;
        }

        if (makes_constructors) {
            result += "; the only factory that makes new Constructors";
        }

        return result + ";";
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
            return "Eco-sphere, scores 1 point every turn while it works, points decide the game;";
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

/// The engine name of a unit type, a stable key for the prompt templates.
static const char* JevAi_GetTypeKey(ResourceID unit_type) {
    switch (unit_type) {
        case MININGST:
            return "MININGST";
        case POWGEN:
            return "POWGEN";
        case POWERSTN:
            return "POWERSTN";
        case CNCT_4W:
            return "CNCT_4W";
        case ADUMP:
            return "ADUMP";
        case FDUMP:
            return "FDUMP";
        case GOLDSM:
            return "GOLDSM";
        case HABITAT:
            return "HABITAT";
        case RESEARCH:
            return "RESEARCH";
        case GREENHSE:
            return "GREENHSE";
        case DEPOT:
            return "DEPOT";
        case HANGAR:
            return "HANGAR";
        case DOCK:
            return "DOCK";
        case BARRACKS:
            return "BARRACKS";
        case RADAR:
            return "RADAR";
        case GUNTURRT:
            return "GUNTURRT";
        case ARTYTRRT:
            return "ARTYTRRT";
        case ANTIAIR:
            return "ANTIAIR";
        case ANTIMSSL:
            return "ANTIMSSL";
        case ROAD:
            return "ROAD";
        case BRIDGE:
            return "BRIDGE";
        case WTRPLTFM:
            return "WTRPLTFM";
        case LANDPAD:
            return "LANDPAD";
        case BLOCK:
            return "BLOCK";
        case CONSTRCT:
            return "CONSTRCT";
        case ENGINEER:
            return "ENGINEER";
        case SURVEYOR:
            return "SURVEYOR";
        case SCOUT:
            return "SCOUT";
        case TANK:
            return "TANK";
        case ARTILLRY:
            return "ARTILLRY";
        case ROCKTLCH:
            return "ROCKTLCH";
        case MISSLLCH:
            return "MISSLLCH";
        case SP_FLAK:
            return "SP_FLAK";
        case MINELAYR:
            return "MINELAYR";
        case SCANNER:
            return "SCANNER";
        case SPLYTRCK:
            return "SPLYTRCK";
        case REPAIR:
            return "REPAIR";
        case FUELTRCK:
            return "FUELTRCK";
        case GOLDTRCK:
            return "GOLDTRCK";
        case BULLDOZR:
            return "BULLDOZR";
        case INFANTRY:
            return "INFANTRY";
        case COMMANDO:
            return "COMMANDO";
        case LIGHTPLT:
            return "LIGHTPLT";
        case LANDPLT:
            return "LANDPLT";
        case SHIPYARD:
            return "SHIPYARD";
        case AIRPLT:
            return "AIRPLT";
        case TRAINHAL:
            return "TRAINHAL";
        default:
            return "";
    }
}

/// What a unit type is for, as data: its name, and for a factory what it makes.
static nlohmann::ordered_json JevAi_PurposeData(uint16_t team, ResourceID unit_type) {
    nlohmann::ordered_json result = {{"type", JevAi_GetTypeName(unit_type)}, {"key", JevAi_GetTypeKey(unit_type)}};

    if (JevAi_IsFactory(unit_type)) {
        nlohmann::ordered_json combat = nlohmann::ordered_json::array();
        nlohmann::ordered_json other = nlohmann::ordered_json::array();

        for (const ResourceID product : Builder_GetBuildableUnits(team, unit_type)) {
            (JevAi_IsGroundCombatUnit(team, product) ? combat : other).push_back(JevAi_GetTypeName(product));
        }

        result["factory"] = {{"combat", combat}, {"other", other}};
    }

    return result;
}

/// A unit type's attack, range, hits, armor and speed.
static nlohmann::ordered_json JevAi_StatsData(uint16_t team, ResourceID unit_type) {
    UnitValues* values = UnitsManager_GetCurrentUnitValues(&UnitsManager_TeamInfo[team], unit_type);
    nlohmann::ordered_json result = nlohmann::ordered_json::object();

    if (values->GetAttribute(ATTRIB_ATTACK) > 0) {
        result["attack"] = values->GetAttribute(ATTRIB_ATTACK);
        result["range"] = values->GetAttribute(ATTRIB_RANGE);
    }

    result["hits"] = values->GetAttribute(ATTRIB_HITS);
    result["armor"] = values->GetAttribute(ATTRIB_ARMOR);

    if (values->GetAttribute(ATTRIB_SPEED) > 0) {
        result["speed"] = values->GetAttribute(ATTRIB_SPEED);
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

bool JevAi::GetBasePosition(Point& position) const {
    for (auto it = UnitsManager_StationaryUnits.Begin(), it_end = UnitsManager_StationaryUnits.End(); it != it_end;
         ++it) {
        if ((*it).team == m_team && (*it).hits > 0 && (*it).GetComplex() && ((*it).flags & BUILDING)) {
            position = Point((*it).grid_x, (*it).grid_y);

            return true;
        }
    }

    return false;
}

int32_t JevAi::GetDistanceToOwnBuilder(Point point) const {
    int32_t nearest = -1;

    for (auto it = UnitsManager_MobileLandSeaUnits.Begin(), it_end = UnitsManager_MobileLandSeaUnits.End();
         it != it_end; ++it) {
        if ((*it).team == m_team && (*it).hits > 0 && JevAi_IsBuilder((*it).GetUnitType())) {
            const int32_t distance = JevAi_GetDistance(point, Point((*it).grid_x, (*it).grid_y));

            if (nearest < 0 || distance < nearest) {
                nearest = distance;
            }
        }
    }

    return nearest;
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

/// The cells around a building or site, where units leave it.
static std::vector<Point> JevAi_GetOutline(Point corner, int32_t size) {
    std::vector<Point> cells;

    for (int32_t x = corner.x - 1; x <= corner.x + size; ++x) {
        for (int32_t y = corner.y - 1; y <= corner.y + size; ++y) {
            const bool inside = x >= corner.x && x < corner.x + size && y >= corner.y && y < corner.y + size;

            if (!inside && JevAi_IsInsideMap(Point(x, y))) {
                cells.push_back(Point(x, y));
            }
        }
    }

    return cells;
}

/// Free for a land unit to stand on, counting own units as able to move away.
static bool JevAi_IsOpenCell(uint16_t team, Point cell) {
    return Access_IsAccessible(TANK, team, cell.x, cell.y, AccessModifier_EnemySameClassBlocks);
}

bool JevAi::IsFactoryExit(Point cell) const {
    for (auto it = UnitsManager_StationaryUnits.Begin(), it_end = UnitsManager_StationaryUnits.End(); it != it_end;
         ++it) {
        if ((*it).team == m_team && (*it).hits > 0 && JevAi_IsFactory((*it).GetUnitType())) {
            const Point corner((*it).grid_x, (*it).grid_y);

            if (cell.x >= corner.x - 1 && cell.x <= corner.x + 2 && cell.y >= corner.y - 1 && cell.y <= corner.y + 2) {
                return true;
            }
        }
    }

    return false;
}

bool JevAi::LeavesExits(ResourceID unit_type, Point site) const {
    // a finished unit leaves to a free cell next to its factory or building site; never take the last one
    const int32_t size = JevAi_GetSize(unit_type);
    const auto in_site = [site, size](Point cell) {
        return cell.x >= site.x && cell.x < site.x + size && cell.y >= site.y && cell.y < site.y + size;
    };
    // an exit counts when it is free and leads further out, not into a closed pocket
    const auto has_exit = [this, &in_site](Point corner, int32_t width) {
        const auto in_parent = [corner, width](Point cell) {
            return cell.x >= corner.x && cell.x < corner.x + width && cell.y >= corner.y && cell.y < corner.y + width;
        };

        for (const Point cell : JevAi_GetOutline(corner, width)) {
            if (in_site(cell) || !JevAi_IsOpenCell(m_team, cell)) {
                continue;
            }

            for (const Point next : JevAi_GetOutline(cell, 1)) {
                if (!in_site(next) && !in_parent(next) && JevAi_IsOpenCell(m_team, next)) {
                    return true;
                }
            }
        }

        return false;
    };

    // the builder steps out of the finished building
    if (!has_exit(site, size)) {
        return false;
    }

    // factories, and buildings being built, whose exits this site would take
    for (auto* list : {&UnitsManager_StationaryUnits, &UnitsManager_MobileLandSeaUnits}) {
        for (auto it = list->Begin(), it_end = list->End(); it != it_end; ++it) {
            const bool is_factory = JevAi_IsFactory((*it).GetUnitType());
            const bool is_building = (*it).GetOrder() == ORDER_BUILD && !((*it).flags & STATIONARY);

            if ((*it).team != m_team || (*it).hits == 0 || (!is_factory && !is_building)) {
                continue;
            }

            Rect bounds;

            (*it).GetBounds(&bounds);

            const Point corner(bounds.ulx, bounds.uly);
            const int32_t width = std::max(1, bounds.lrx - bounds.ulx);
            const auto outline = JevAi_GetOutline(corner, width);

            if (std::any_of(outline.begin(), outline.end(), in_site) && !has_exit(corner, width)) {
                return false;
            }
        }
    }

    return true;
}

bool JevAi::ClearExit(UnitInfo* parent) {
    // the exits are only taken by own units: move one of them out of the way, as a human player would
    constexpr int32_t maximum_clears_per_turn = 6;
    const int32_t size = (parent->flags & BUILDING) ? 2 : 1;

    if (m_turn_exit_clears >= maximum_clears_per_turn) {
        return false;
    }

    for (const Point cell : JevAi_GetOutline(Point(parent->grid_x, parent->grid_y), size)) {
        UnitInfo* blocker = Access_GetTeamUnit(cell.x, cell.y, m_team, MOBILE_LAND_UNIT);

        if (!blocker || blocker == parent || !Task_IsReadyToTakeOrders(blocker) || blocker->speed == 0) {
            continue;
        }

        // the nearest free cell that is no factory exit
        Point best;
        int32_t best_distance = INT32_MAX;

        for (int32_t x = cell.x - 4; x <= cell.x + 4; ++x) {
            for (int32_t y = cell.y - 4; y <= cell.y + 4; ++y) {
                const Point target(x, y);
                const int32_t distance = Access_GetSquaredDistance(target, cell);

                if (JevAi_IsInsideMap(target) && distance > 1 && distance < best_distance && !IsFactoryExit(target) &&
                    Access_IsAccessible(blocker->GetUnitType(), m_team, x, y, AccessModifier_SameClassBlocks)) {
                    best = target;
                    best_distance = distance;
                }
            }
        }

        if (best_distance == INT32_MAX) {
            continue;
        }

        SDL_Log("Jev %s: %s moves out of the %s's exit to %s.", GetTeamName(), JevAi_GetUnitName(blocker).c_str(),
                JevAi_GetUnitName(parent).c_str(), JevAi_FormatPoint(best).c_str());

        m_unit = blocker;
        m_last_action = Action::MOVE;
        m_move_start = Point(blocker->grid_x, blocker->grid_y);
        blocker->move_to_grid_x = best.x;
        blocker->move_to_grid_y = best.y;

        UnitsManager_SetNewOrder(blocker, ORDER_MOVE, ORDER_STATE_INIT);

        ++m_turn_exit_clears;
        m_wait_start = SDL_GetTicks();
        m_state = State::WAIT_FOR_UNIT;

        return true;
    }

    return false;
}

/// How much a defense is worth near an own building: what the team would lose with it.
static int32_t JevAi_GetProtectionWeight(ResourceID unit_type) {
    switch (unit_type) {
        case GREENHSE:
            return 4;
        case HABITAT:
        case LANDPLT:
        case LIGHTPLT:
            return 3;
        case MININGST:
            return 2;
        case POWGEN:
        case POWERSTN:
            return 1;
        default:
            return 0;
    }
}

static bool JevAi_IsDefense(ResourceID unit_type) {
    return unit_type == GUNTURRT || unit_type == ARTYTRRT || unit_type == ANTIMSSL || unit_type == ANTIAIR;
}

nlohmann::ordered_json JevAi::GetDefenseCoverage(ResourceID unit_type, Point site) const {
    const int32_t range =
        UnitsManager_GetCurrentUnitValues(&UnitsManager_TeamInfo[m_team], unit_type)->GetAttribute(ATTRIB_RANGE);
    nlohmann::ordered_json covers = nlohmann::ordered_json::object();
    int32_t defenses = 0;
    int32_t nearest_enemy = -1;

    for (auto it = UnitsManager_StationaryUnits.Begin(), it_end = UnitsManager_StationaryUnits.End(); it != it_end;
         ++it) {
        if ((*it).team != m_team || (*it).hits == 0) {
            continue;
        }

        const int32_t distance = JevAi_GetDistance(site, Point((*it).grid_x, (*it).grid_y));

        if (JevAi_GetProtectionWeight((*it).GetUnitType()) > 0 && distance <= range) {
            const std::string name = JevAi_GetUnitName(it->Get());

            covers[name] = covers.value(name, 0) + 1;
        }

        if (JevAi_IsDefense((*it).GetUnitType()) && distance <= (*it).GetBaseValues()->GetAttribute(ATTRIB_RANGE)) {
            ++defenses;
        }
    }

    for (auto* list : {&UnitsManager_MobileLandSeaUnits, &UnitsManager_StationaryUnits}) {
        for (auto it = list->Begin(), it_end = list->End(); it != it_end; ++it) {
            if ((*it).team != m_team && (*it).team != PLAYER_TEAM_ALIEN && (*it).hits > 0 &&
                (*it).IsVisibleToTeam(m_team)) {
                const int32_t distance = JevAi_GetDistance(site, Point((*it).grid_x, (*it).grid_y));

                if (nearest_enemy < 0 || distance < nearest_enemy) {
                    nearest_enemy = distance;
                }
            }
        }
    }

    nlohmann::ordered_json result = {{"range", range}, {"covers", covers}, {"defenses_covering_site", defenses}};
    std::vector<Point> approach;
    Point origin;

    if (GetApproach(approach, origin)) {
        int32_t covered = 0;

        for (const Point cell : approach) {
            covered += JevAi_GetDistance(site, cell) <= range;
        }

        result["approach"] = {
            {"from", JevAi_PointData(origin)}, {"cells", static_cast<int32_t>(approach.size())}, {"covered", covered}};
    }

    if (nearest_enemy >= 0) {
        result["nearest_known_enemy"] = nearest_enemy;
    }

    return result;
}

void JevAi::RememberEnemies() {
    // only what the team sees; units are forgotten after a while, buildings are kept
    constexpr int32_t unit_memory_turns = 15;
    const int32_t turn = GameManager_TurnCounter;

    m_sightings.erase(
        std::remove_if(m_sightings.begin(), m_sightings.end(),
                       [turn](const Sighting& item) { return !item.building && turn - item.turn > unit_memory_turns; }),
        m_sightings.end());

    for (auto* list : {&UnitsManager_MobileLandSeaUnits, &UnitsManager_StationaryUnits}) {
        for (auto it = list->Begin(), it_end = list->End(); it != it_end; ++it) {
            if ((*it).team == m_team || (*it).team == PLAYER_TEAM_ALIEN || (*it).hits == 0 ||
                !(*it).IsVisibleToTeam(m_team) || (*it).GetUnitType() == CNCT_4W || (*it).GetUnitType() == ROAD) {
                continue;
            }

            const Point at((*it).grid_x, (*it).grid_y);
            const bool building = ((*it).flags & STATIONARY) != 0;
            auto known = std::find_if(m_sightings.begin(), m_sightings.end(),
                                      [at](const Sighting& item) { return item.at == at; });

            if (known != m_sightings.end()) {
                known->turn = turn;
                known->building = known->building || building;

            } else {
                m_sightings.push_back({at, turn, building});
            }
        }
    }
}

bool JevAi::GetApproach(std::vector<Point>& cells, Point& origin) const {
    // the way enemies come: from their known base, or else from recent sightings, to this base
    constexpr int32_t approach_length = 10;
    Point base;
    int64_t sum_x = 0;
    int64_t sum_y = 0;
    int32_t count = 0;

    if (!GetBasePosition(base)) {
        return false;
    }

    for (const bool buildings : {true, false}) {
        for (const auto& sighting : m_sightings) {
            if (sighting.building == buildings) {
                sum_x += sighting.at.x;
                sum_y += sighting.at.y;
                ++count;
            }
        }

        if (count) {
            break;
        }
    }

    if (count == 0) {
        return false;
    }

    origin = Point(static_cast<int32_t>(sum_x / count), static_cast<int32_t>(sum_y / count));

    const int32_t length = std::max(1, JevAi_GetDistance(base, origin));

    cells.clear();

    for (int32_t step = 1; step <= std::min(length, approach_length); ++step) {
        cells.push_back(
            Point(base.x + (origin.x - base.x) * step / length, base.y + (origin.y - base.y) * step / length));
    }

    return !cells.empty();
}

std::vector<Point> JevAi::FindDefenseSites(UnitInfo* builder, ResourceID unit_type) const {
    // a few distinct legal sites, best first: what they protect, less what other defenses already protect
    constexpr int32_t search_radius = 8;
    constexpr int32_t maximum_sites = 3;
    constexpr int32_t minimum_spacing = 3;
    const int32_t range =
        UnitsManager_GetCurrentUnitValues(&UnitsManager_TeamInfo[m_team], unit_type)->GetAttribute(ATTRIB_RANGE);
    const Point position(builder->grid_x, builder->grid_y);
    std::vector<std::pair<int64_t, Point>> candidates;
    std::vector<Point> approach;
    Point origin;

    GetApproach(approach, origin);

    for (int32_t x = position.x - search_radius; x <= position.x + search_radius; ++x) {
        for (int32_t y = position.y - search_radius; y <= position.y + search_radius; ++y) {
            const Point site(x, y);

            if (!JevAi_IsInsideMap(site) || !JevAi_IsSiteFree(m_team, unit_type, site) ||
                !LeavesExits(unit_type, site)) {
                continue;
            }

            int64_t score = 0;

            for (auto it = UnitsManager_StationaryUnits.Begin(), it_end = UnitsManager_StationaryUnits.End();
                 it != it_end; ++it) {
                const int32_t weight = JevAi_GetProtectionWeight((*it).GetUnitType());

                if ((*it).team != m_team || (*it).hits == 0 || weight == 0 ||
                    JevAi_GetDistance(site, Point((*it).grid_x, (*it).grid_y)) > range) {
                    continue;
                }

                bool covered = false;

                for (auto it2 = UnitsManager_StationaryUnits.Begin(), it2_end = UnitsManager_StationaryUnits.End();
                     it2 != it2_end && !covered; ++it2) {
                    covered =
                        (*it2).team == m_team && (*it2).hits > 0 && JevAi_IsDefense((*it2).GetUnitType()) &&
                        JevAi_GetDistance(Point((*it2).grid_x, (*it2).grid_y), Point((*it).grid_x, (*it).grid_y)) <=
                            (*it2).GetBaseValues()->GetAttribute(ATTRIB_RANGE);
                }

                score += covered ? weight * 5 : weight * 10;
            }

            if (score == 0) {
                continue;
            }

            const nlohmann::ordered_json coverage = GetDefenseCoverage(unit_type, site);

            for (const Point cell : approach) {
                score += JevAi_GetDistance(site, cell) <= range ? 15 : 0;
            }

            score -= JevAi_GetDistance(site, position) * 2;
            candidates.push_back({score, site});
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

    std::vector<Point> sites;

    for (const auto& candidate : candidates) {
        if (std::all_of(sites.begin(), sites.end(), [&candidate](Point chosen) {
                return JevAi_GetDistance(chosen, candidate.second) >= minimum_spacing;
            })) {
            sites.push_back(candidate.second);
        }

        if (static_cast<int32_t>(sites.size()) == maximum_sites) {
            break;
        }
    }

    return sites;
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

            if (!JevAi_IsSiteFree(m_team, unit_type, candidate) || !LeavesExits(unit_type, candidate)) {
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

            if (ClearExit(parent)) {
                return true;
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

        // an Eco-sphere only scores while it works
        int32_t power_left = complex->power;
        int32_t workers_left = complex->workers;

        for (auto it = UnitsManager_StationaryUnits.Begin(), it_end = UnitsManager_StationaryUnits.End(); it != it_end;
             ++it) {
            if ((*it).GetComplex() == complex && (*it).GetUnitType() == GREENHSE && (*it).hits > 0 &&
                ((*it).GetOrder() == ORDER_POWER_OFF || (*it).GetOrder() == ORDER_AWAIT) &&
                power_left >= Cargo_GetPowerConsumptionRate(GREENHSE) &&
                workers_left >= Cargo_GetLifeConsumptionRate(GREENHSE)) {
                UnitsManager_SetNewOrder(it->Get(), ORDER_POWER_ON, ORDER_STATE_INIT);

                power_left -= Cargo_GetPowerConsumptionRate(GREENHSE);
                workers_left -= Cargo_GetLifeConsumptionRate(GREENHSE);
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
    const bool is_builder = JevAi_IsBuilder(unit->GetUnitType());
    const bool is_military = !JevAi_IsConstructionUnit(unit->GetUnitType());
    const int32_t danger_here = GetDanger(unit, position, nullptr);
    Point base;
    const bool has_base = GetBasePosition(base);
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

            // factory exits stay free for the units they release
            if (IsFactoryExit(destination)) {
                continue;
            }

            const int32_t revealed = CountUnexplored(destination, scan);
            const int32_t danger = GetDanger(unit, destination, &nearest_enemy);

            // builders only move to build, to refill or to get out of danger; surveyors only move where it is safe
            if ((is_builder && danger >= danger_here) || (!is_military && danger > 0 && danger >= danger_here)) {
                continue;
            }

            std::string facts = std::string(is_builder ? "move away from danger " : "move ") +
                                JevAi_DirectionNames[direction] + " " + std::to_string(distances[i]) + " cells to " +
                                JevAi_FormatPoint(destination) + ": reveals " + std::to_string(revealed) +
                                " unexplored cells, danger " + std::to_string(danger);
            json data = {{"action", "move"},         {"direction", JevAi_DirectionNames[direction]},
                         {"distance", distances[i]}, {"to", JevAi_PointData(destination)},
                         {"revealed", revealed},     {"danger", danger}};

            if (is_builder) {
                data["away_from_danger"] = true;
            }

            if (is_military && has_base) {
                facts += ", " + std::to_string(JevAi_GetDistance(destination, base)) + " cells from your base";
                data["from_base"] = JevAi_GetDistance(destination, base);

                const int32_t builder_distance = GetDistanceToOwnBuilder(destination);

                if (builder_distance >= 0) {
                    facts += ", " + std::to_string(builder_distance) + " cells from your nearest builder";
                    data["from_builder"] = builder_distance;
                }
            }

            if (revealed == 0) {
                facts +=
                    ", nearest unexplored cell " + std::to_string(GetDistanceToUnexplored(destination)) + " cells away";
                data["nearest_unexplored"] = GetDistanceToUnexplored(destination);
            }

            if (nearest_enemy >= 0) {
                facts += ", nearest known enemy " + std::to_string(nearest_enemy) + " cells away";
                data["nearest_enemy"] = nearest_enemy;
            }

            if (unit->GetUnitType() == SURVEYOR) {
                int16_t raw;
                int16_t gold;
                int16_t fuel;

                Survey_GetTotalResourcesInArea(destination.x - 1, destination.y - 1, 2, &raw, &gold, &fuel, true,
                                               m_team);

                facts += ", resources there raw " + std::to_string(raw) + " fuel " + std::to_string(fuel) + " gold " +
                         std::to_string(gold);
                data["resources"] = {{"raw", raw}, {"fuel", fuel}, {"gold", gold}};
            }

            m_options.push_back({"m" + std::to_string(m_options.size()), Action::MOVE, destination, INVALID_ID, nullptr,
                                 facts, revealed - danger * 10, data});
        }
    }
}

void JevAi::AddBuildOptions(UnitInfo* builder) {
    if (!Task_IsReadyToTakeOrders(builder)) {
        return;
    }

    const int32_t rate = Cargo_GetRawConsumptionRate(builder->GetUnitType(), 1);
    int32_t spare_power = 0;
    bool enemies_seen = false;

    for (auto it = UnitsManager_StationaryUnits.Begin(), it_end = UnitsManager_StationaryUnits.End(); it != it_end;
         ++it) {
        if ((*it).team == m_team && (*it).GetComplex()) {
            spare_power = (*it).GetComplex()->power;
            break;
        }
    }

    // as the classic AI does: power needed by all buildings, also those being built, against all generators
    int32_t power_needed = 0;
    int32_t power_generated = 0;
    int32_t generators = 0;
    // the same for workers: Eco-spheres need them, Habitats give them
    int32_t workers_needed = 0;
    int32_t workers_given = 0;
    int32_t factories = 0;
    Cargo fuel_mining;
    Cargo fuel_mining_capacity;

    for (auto* list : {&UnitsManager_MobileLandSeaUnits, &UnitsManager_StationaryUnits}) {
        for (auto it = list->Begin(), it_end = list->End(); it != it_end; ++it) {
            if ((*it).team != m_team && (*it).team != PLAYER_TEAM_ALIEN && (*it).hits > 0 &&
                (*it).IsVisibleToTeam(m_team)) {
                enemies_seen = true;
            }

            if ((*it).team != m_team || (*it).hits == 0) {
                continue;
            }

            ResourceID power_unit = INVALID_ID;

            if ((*it).flags & STATIONARY) {
                power_unit = (*it).GetUnitType();

                if (!fuel_mining_capacity.fuel && (*it).GetComplex()) {
                    (*it).GetComplex()->GetCargoMining(fuel_mining, fuel_mining_capacity);
                }

            } else if ((*it).GetOrder() == ORDER_BUILD && (*it).build_list.GetCount() > 0) {
                power_unit = (*it).GetConstructedUnitType();
            }

            if (power_unit != INVALID_ID) {
                const int32_t power = Cargo_GetPowerConsumptionRate(power_unit);
                const int32_t workers = Cargo_GetLifeConsumptionRate(power_unit);

                if (power > 0) {
                    power_needed += power;

                } else {
                    power_generated -= power;
                    generators += power_unit == POWGEN;
                }

                if (workers > 0) {
                    workers_needed += workers;

                } else {
                    workers_given -= workers;
                }

                factories += JevAi_IsFactory(power_unit);
            }
        }
    }

    for (const ResourceID unit_type : Builder_GetBuildableUnits(m_team, builder->GetUnitType())) {
        Point found_site;

        // the game only offers buildings that make sense now
        if (unit_type == POWGEN) {
            // a factory needs power too, plan for the first one
            const int32_t factory_power =
                (CountOwnUnits(LANDPLT) + CountOwnUnits(LIGHTPLT) == 0) ? Cargo_GetPowerConsumptionRate(LANDPLT) : 0;

            if (power_needed + factory_power <= power_generated ||
                fuel_mining_capacity.fuel < (generators + 1) * Cargo_GetFuelConsumptionRate(POWGEN)) {
                continue;
            }

        } else if (JevAi_IsFactory(unit_type)) {
            if ((unit_type != LIGHTPLT && unit_type != LANDPLT) || CountOwnUnits(unit_type) >= 2) {
                continue;
            }

        } else if (unit_type == HABITAT || unit_type == GREENHSE) {
            // points come after the first factory; an Eco-sphere needs a free worker, a Habitat gives three
            if (factories == 0 || (unit_type == HABITAT && workers_given - workers_needed >= 1) ||
                (unit_type == GREENHSE && workers_given - workers_needed < 1)) {
                continue;
            }

        } else if (unit_type == ARTYTRRT || unit_type == ANTIMSSL || unit_type == RADAR) {
            // Gun Turrets are offered before the first enemy is seen; the others wait for it
            if (!enemies_seen) {
                continue;
            }

        } else if (unit_type != MININGST && unit_type != GUNTURRT) {
            continue;
        }

        std::vector<Point> sites;

        if (unit_type == GUNTURRT) {
            sites = FindDefenseSites(builder, unit_type);

        } else if (FindBuildSite(builder, unit_type, found_site)) {
            sites.push_back(found_site);
        }

        for (const Point site : sites) {
            const int32_t turns = BuildMenu_GetTurnsToBuild(unit_type, m_team);
            const int32_t cost = turns * rate;
            const bool covers_builder = (site.x == builder->grid_x || site.x + 1 == builder->grid_x) &&
                                        (site.y == builder->grid_y || site.y + 1 == builder->grid_y);
            std::string facts = "build a " + JevAi_GetTypeName(unit_type) + " at " + JevAi_FormatPoint(site) + ": " +
                                JevAi_DescribePurpose(m_team, unit_type) + " " + std::to_string(turns) + " turns, " +
                                std::to_string(cost) + " raw materials, you carry " + std::to_string(builder->storage) +
                                ", you have " + std::to_string(CountOwnUnits(unit_type)) + " already";
            json data = {{"action", "build"},
                         {"type", JevAi_GetTypeName(unit_type)},
                         {"at", JevAi_PointData(site)},
                         {"purpose", JevAi_PurposeData(m_team, unit_type)},
                         {"turns", turns},
                         {"cost", cost},
                         {"carried", builder->storage},
                         {"have", CountOwnUnits(unit_type)}};
            int32_t score = 0;

            if (!covers_builder) {
                facts += ", the builder walks there first";
                data["walks_there"] = true;
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
                data["resources"] = {{"raw", raw}, {"fuel", fuel}, {"gold", gold}};
                score = (raw + fuel + gold) > 0 ? 40 + raw + fuel + gold : -10;

            } else {
                facts += TouchesBase(unit_type, site) ? ", joins your base" : ", not connected to your base";
                data["joins_base"] = TouchesBase(unit_type, site);

                if (JevAi_IsFactory(unit_type)) {
                    score = (CountOwnUnits(unit_type) == 0) ? 60 : 5;

                } else if (unit_type == POWGEN) {
                    score = 20;

                } else if (unit_type == GREENHSE) {
                    score = 30;
                    facts += ", " + std::to_string(workers_given - workers_needed) + " workers spare";
                    data["workers_spare"] = workers_given - workers_needed;

                    if (JevAi_GetTurnsLeft() > 0) {
                        facts += ", " + std::to_string(JevAi_GetTurnsLeft()) + " turns left in the game";
                        data["turns_left"] = JevAi_GetTurnsLeft();
                    }

                } else if (unit_type == HABITAT) {
                    score = 25;
                    facts += ", gives " + std::to_string(-Cargo_GetLifeConsumptionRate(HABITAT)) +
                             " workers, each Eco-sphere needs 1, you have " +
                             std::to_string(std::max(0, workers_given - workers_needed)) + " spare";
                    data["gives_workers"] = -Cargo_GetLifeConsumptionRate(HABITAT);
                    data["workers_spare"] = std::max(0, workers_given - workers_needed);
                }

                if (unit_type == POWGEN) {
                    facts += ", burns " + std::to_string(Cargo_GetFuelConsumptionRate(POWGEN)) + " fuel per turn for " +
                             std::to_string(-Cargo_GetPowerConsumptionRate(POWGEN)) +
                             " power; your buildings, with those being built, need " + std::to_string(power_needed) +
                             " power and your generators give " + std::to_string(power_generated) +
                             ", the base mines " + std::to_string(fuel_mining_capacity.fuel) + " fuel per turn at most";
                    data["generator"] = {{"burns_fuel", Cargo_GetFuelConsumptionRate(POWGEN)},
                                         {"gives_power", -Cargo_GetPowerConsumptionRate(POWGEN)},
                                         {"power_needed", power_needed},
                                         {"power_generated", power_generated},
                                         {"fuel_mined_max", fuel_mining_capacity.fuel}};

                } else if (Cargo_GetPowerConsumptionRate(unit_type) > 0) {
                    facts += ", needs " + std::to_string(Cargo_GetPowerConsumptionRate(unit_type)) + " power";
                    data["needs_power"] = Cargo_GetPowerConsumptionRate(unit_type);
                    data["power_spare"] = spare_power;

                    if (spare_power < Cargo_GetPowerConsumptionRate(unit_type)) {
                        facts += " and the base has none spare yet";
                    }
                }
            }

            if (cost > builder->storage) {
                facts += ", not enough materials carried";
                data["not_enough_materials"] = true;
                score = -50;
            }

            if (JevAi_GetSize(unit_type) == 1 && unit_type != MININGST) {
                if (UnitsManager_GetCurrentUnitValues(&UnitsManager_TeamInfo[m_team], unit_type)
                        ->GetAttribute(ATTRIB_ATTACK) > 0) {
                    facts += ", a defense: " + JevAi_DescribeType(m_team, unit_type);
                    data["defense"] = JevAi_StatsData(m_team, unit_type);
                }
            }

            if (JevAi_IsDefense(unit_type)) {
                const json coverage = GetDefenseCoverage(unit_type, site);
                std::string covered;

                for (const auto& [name, count] : coverage["covers"].items()) {
                    covered += (covered.empty() ? "" : ", ") + std::to_string(count.get<int32_t>()) + " " + name;
                }

                facts += covered.empty() ? ", covers none of your buildings" : ", covers " + covered;
                facts += ", " + std::to_string(coverage["defenses_covering_site"].get<int32_t>()) +
                         " of your defenses reach this spot";

                if (coverage.contains("approach")) {
                    const auto& approach = coverage["approach"];

                    facts += ", covers " + std::to_string(approach["covered"].get<int32_t>()) + " of " +
                             std::to_string(approach["cells"].get<int32_t>()) +
                             " cells of the way from the enemy seen near [" +
                             std::to_string(approach["from"][0].get<int32_t>()) + "," +
                             std::to_string(approach["from"][1].get<int32_t>()) + "] to your base";
                }

                if (coverage.contains("nearest_known_enemy")) {
                    facts += ", nearest known enemy " + std::to_string(coverage["nearest_known_enemy"].get<int32_t>()) +
                             " cells away";
                }

                data["coverage"] = coverage;
                score += 10;
            }

            m_options.push_back(
                {"b" + std::to_string(m_options.size()), Action::BUILD, site, unit_type, nullptr, facts, score, data});
        }
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

        m_options.push_back({"p" + std::to_string(m_options.size()),
                             Action::PRODUCE,
                             Point(),
                             unit_type,
                             nullptr,
                             facts,
                             score,
                             {{"action", "produce"},
                              {"type", JevAi_GetTypeName(unit_type)},
                              {"purpose", JevAi_PurposeData(m_team, unit_type)},
                              {"turns", turns},
                              {"cost", turns * rate},
                              {"stats", JevAi_StatsData(m_team, unit_type)},
                              {"have", CountOwnUnits(unit_type)}}});
    }

    m_options.push_back(
        {"nothing",
         Action::STAY,
         Point(),
         INVALID_ID,
         nullptr,
         "produce nothing this turn; the base has " + std::to_string(materials.raw) + " of " +
             std::to_string(capacity.raw) + " raw materials stored",
         0,
         {{"action", "produce_nothing"}, {"base_raw", materials.raw}, {"base_raw_capacity", capacity.raw}}});
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

            m_options.push_back({"r" + std::to_string(m_options.size()),
                                 Action::REFILL,
                                 Point(),
                                 INVALID_ID,
                                 SmartPointer<UnitInfo>(source),
                                 "take " + std::to_string(amount) + " raw materials from the " +
                                     JevAi_GetUnitName(&building) + " next to you, you carry " +
                                     std::to_string(builder->storage) + " of " + std::to_string(capacity) +
                                     ", the base stores " + std::to_string(materials.raw),
                                 builder->storage < capacity / 2 ? 45 : 5,
                                 {{"action", "refill"},
                                  {"amount", amount},
                                  {"from", JevAi_GetUnitName(&building)},
                                  {"carried", builder->storage},
                                  {"capacity", capacity},
                                  {"base_raw", materials.raw}}});

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
        m_options.push_back({"g" + std::to_string(m_options.size()),
                             Action::MOVE,
                             nearest_cell,
                             INVALID_ID,
                             nullptr,
                             "go next to the " + JevAi_GetUnitName(nearest) + " at " + JevAi_FormatPoint(nearest_cell) +
                                 ", " + std::to_string(static_cast<int32_t>(SDL_sqrt(nearest_distance))) +
                                 " cells away, to take raw materials there; you carry " +
                                 std::to_string(builder->storage) + " of " + std::to_string(capacity) +
                                 ", the base stores " + std::to_string(nearest_materials.raw),
                             builder->storage < capacity / 2 ? 35 : 0,
                             {{"action", "go_refill"},
                              {"to_building", JevAi_GetUnitName(nearest)},
                              {"to", JevAi_PointData(nearest_cell)},
                              {"distance", static_cast<int32_t>(SDL_sqrt(nearest_distance))},
                              {"carried", builder->storage},
                              {"capacity", capacity},
                              {"base_raw", nearest_materials.raw}}});
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

            if (target.team == m_team || target.team == PLAYER_TEAM_ALIEN || target.hits == 0 ||
                !target.IsVisibleToTeam(m_team) || target.GetUnitType() == CNCT_4W || target.GetUnitType() == ROAD ||
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

            json data = {{"action", "attack"},
                         {"target", JevAi_GetUnitName(&target)},
                         {"at", JevAi_PointData(Point(target.grid_x, target.grid_y))},
                         {"hits", target.hits},
                         {"max_hits", target_values->GetAttribute(ATTRIB_HITS)},
                         {"damage_per_shot", damage},
                         {"shots", unit->shots},
                         {"in_range", in_range},
                         {"destroyable", target.hits <= damage * unit->shots}};

            if (shoots_back) {
                data["shoots_back_attack"] = target_values->GetAttribute(ATTRIB_ATTACK);
            }

            m_options.push_back({"a" + std::to_string(m_options.size()), Action::ATTACK,
                                 Point(target.grid_x, target.grid_y), INVALID_ID, SmartPointer<UnitInfo>(&target),
                                 facts, 50 + damage * 5 - (shoots_back ? 10 : 0), data});
        }
    }
}

json JevAi::GetState(UnitInfo* unit) const {
    json state;
    json buildings = json::object();
    json units = json::object();
    json enemies = json::array();
    int32_t nearest_enemy;
    int32_t enemies_seen = 0;
    int32_t factories = 0;
    int32_t combat_units = 0;
    std::vector<std::pair<int32_t, UnitInfo*>> visible_enemies;
    const Point position(unit->grid_x, unit->grid_y);
    const bool is_military = !(unit->flags & STATIONARY) && !JevAi_IsConstructionUnit(unit->GetUnitType());

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

                factories += JevAi_IsFactory((*it).GetUnitType());
                combat_units += !((*it).flags & STATIONARY) && JevAi_IsGroundCombatUnit(m_team, (*it).GetUnitType());

            } else if ((*it).team != PLAYER_TEAM_ALIEN && (*it).IsVisibleToTeam(m_team)) {
                ++enemies_seen;

                if ((*it).GetUnitType() != CNCT_4W && (*it).GetUnitType() != ROAD) {
                    visible_enemies.push_back(
                        {JevAi_GetDistance(position, Point((*it).grid_x, (*it).grid_y)), it->Get()});
                }
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
        state["you"]["danger_here"] = GetDanger(unit, position, &nearest_enemy);

        if (JevAi_IsBuilder(unit->GetUnitType())) {
            state["you"]["materials_carried"] = unit->storage;
            state["you"]["materials_capacity"] = unit->GetBaseValues()->GetAttribute(ATTRIB_STORAGE);
        }
    }

    if (is_military) {
        Point base;

        std::sort(visible_enemies.begin(), visible_enemies.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

        for (size_t i = 0; i < visible_enemies.size() && i < 5; ++i) {
            UnitInfo* enemy = visible_enemies[i].second;

            enemies.push_back({{"type", JevAi_GetUnitName(enemy)},
                               {"position", {enemy->grid_x + 1, enemy->grid_y + 1}},
                               {"hits", enemy->hits},
                               {"distance", visible_enemies[i].first}});
        }

        if (GetBasePosition(base)) {
            state["you"]["distance_to_base"] = JevAi_GetDistance(position, base);
            state["base_position"] = {base.x + 1, base.y + 1};
        }

        const int32_t builder_distance = GetDistanceToOwnBuilder(position);

        if (builder_distance >= 0) {
            state["you"]["distance_to_nearest_builder"] = builder_distance;
        }

        state["army"] = {{"combat_units", combat_units},
                         {"builders", CountOwnUnits(CONSTRCT) + CountOwnUnits(ENGINEER)}};
        state["nearest_enemies"] = enemies;
        state["enemies_seen"] = enemies_seen;
        state["explored_cells"] = m_explored_cells;
        state["map_cells"] = ResourceManager_MapSize.x * ResourceManager_MapSize.y;
        state["units_lost"] = m_units_lost;

        return state;
    }

    uint32_t best_enemy_points = 0;

    for (int32_t team = PLAYER_TEAM_RED; team < PLAYER_TEAM_MAX - 1; ++team) {
        if (team != m_team && UnitsManager_TeamInfo[team].team_type != TEAM_TYPE_NONE) {
            best_enemy_points = std::max(best_enemy_points, UnitsManager_TeamInfo[team].team_points);
        }
    }

    state["score"] = {{"your_points", UnitsManager_TeamInfo[m_team].team_points},
                      {"best_enemy_points", best_enemy_points}};

    if (JevAi_GetTurnsLeft() >= 0) {
        state["score"]["turns_left"] = JevAi_GetTurnsLeft();
    }

    state["team"] = {{"factories", factories},
                     {"constructors", CountOwnUnits(CONSTRCT)},
                     {"engineers", CountOwnUnits(ENGINEER)},
                     {"combat_units", combat_units}};

    // Production questions describe this factory's network, not an arbitrary base.
    const bool is_factory = JevAi_IsFactory(unit->GetUnitType());
    Complex* local_complex = is_factory ? unit->GetComplex() : nullptr;

    for (auto it = UnitsManager_StationaryUnits.Begin(), it_end = UnitsManager_StationaryUnits.End(); it != it_end;
         ++it) {
        if ((*it).team == m_team && (*it).GetComplex() && (!is_factory || (*it).GetComplex() == local_complex)) {
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

    if (is_factory && local_complex) {
        const Cargo balance = ProductionManager_PreviewProduction(m_team, local_complex, unit);
        state["production"] = {
            {"raw_per_turn", Cargo_GetRawConsumptionRate(unit->GetUnitType(), 1)},
            {"power_needed", Cargo_GetPowerConsumptionRate(unit->GetUnitType())},
            {"workers_needed", Cargo_GetLifeConsumptionRate(unit->GetUnitType())},
            {"can_run_next_turn_without_reallocating",
             balance.raw >= 0 && balance.fuel >= 0 && balance.gold >= 0 && balance.power >= 0 && balance.life >= 0},
            {"balance_after_generator_switching",
             {{"raw", balance.raw},
              {"fuel", balance.fuel},
              {"gold", balance.gold},
              {"power", balance.power},
              {"workers", balance.life}}}};
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

    json options = json::object();

    for (const auto& option : m_options) {
        criteria[option.key] = option.facts;
        scores[option.key] = option.score;
        options[option.key] = option.data;
    }

    json request;

    request["id"] = ++m_request_id;
    request["team"] = GetTeamName();
    request["state"] = GetState(unit);
    request["questions"] = {{m_question, {{"type", "choice"}, {"instructions", instructions}, {"criteria", criteria}}}};
    request["mock_scores"] = scores;
    // for the bridge's prompt templates: the same options as data
    request["options"] = options;

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

void JevAi::WalkToSite(ResourceID unit_type, Point site) {
    m_pending_build = {"", Action::BUILD, site, unit_type, nullptr, "", 0};
    m_has_pending_build = true;
    m_move_start = Point(m_unit->grid_x, m_unit->grid_y);
    m_unit->move_to_grid_x = site.x;
    m_unit->move_to_grid_y = site.y;

    UnitsManager_SetNewOrder(&*m_unit, ORDER_MOVE, ORDER_STATE_INIT);

    m_wait_start = SDL_GetTicks();
    m_state = State::WAIT_FOR_UNIT;
}

bool JevAi::ContinueToSite() {
    auto entry = std::find_if(m_sites.begin(), m_sites.end(),
                              [this](const Site& item) { return item.unit_id == m_unit->unit_id; });

    if (entry == m_sites.end()) {
        return false;
    }

    const Site site = *entry;
    const int32_t size = JevAi_GetSize(site.unit_type);
    const bool on_site = m_unit->grid_x >= site.site.x && m_unit->grid_x < site.site.x + size &&
                         m_unit->grid_y >= site.site.y && m_unit->grid_y < site.site.y + size;

    m_sites.erase(entry);

    if (!JevAi_IsSiteFree(m_team, site.unit_type, site.site) ||
        GameManager_TurnCounter - site.turn > JevAi_SiteWalkTurns) {
        SDL_Log("Jev %s: %s gives up the %s site at %s.", GetTeamName(), JevAi_GetUnitName(&*m_unit).c_str(),
                JevAi_GetTypeName(site.unit_type).c_str(), JevAi_FormatPoint(site.site).c_str());

        return false;
    }

    if (on_site) {
        StartBuilding(&*m_unit, site.unit_type, site.site);
        FinishUnit();

    } else {
        m_sites.push_back(site);

        WalkToSite(site.unit_type, site.site);
    }

    return true;
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
                m_sites.push_back({m_unit->unit_id, option->unit_type, option->destination, GameManager_TurnCounter});

                WalkToSite(option->unit_type, option->destination);
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

            // same order as the classic AiAttack_EvaluateAttack and the human attack click (GameManager_SetUnitOrder):
            // the unit moves toward the target cell until it is in range, then fires
            m_unit->SetEnemy(&*option->target);
            m_unit->path = nullptr;
            m_unit->move_to_grid_x = option->target->grid_x;
            m_unit->move_to_grid_y = option->target->grid_y;

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
        m_sites.clear();
        m_sightings.clear();
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
    m_turn_exit_clears = 0;
    m_state = State::SELECT;

    for (auto it = UnitsManager_MobileLandSeaUnits.Begin(), it_end = UnitsManager_MobileLandSeaUnits.End();
         it != it_end; ++it) {
        if ((*it).team == m_team && (*it).hits > 0 && ((*it).flags & MOBILE_LAND_UNIT)) {
            m_units.push_back(SmartPointer<UnitInfo>(it->Get()));
        }
    }

    // construction first, so that military units see this turn's new sites and builders
    std::stable_partition(m_units.begin(), m_units.end(),
                          [](const auto& unit) { return JevAi_IsConstructionUnit(unit->GetUnitType()); });

    BalanceEconomy();
    RememberEnemies();

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
                    m_unit->grid_y < site.y + size && JevAi_IsSiteFree(m_team, m_pending_build.unit_type, site)) {
                    const uint16_t unit_id = m_unit->unit_id;

                    m_sites.erase(std::remove_if(m_sites.begin(), m_sites.end(),
                                                 [unit_id](const Site& item) { return item.unit_id == unit_id; }),
                                  m_sites.end());

                    StartBuilding(&*m_unit, m_pending_build.unit_type, site);

                } else {
                    // moves can wait for a path or for a unit in the way; keep going next turn
                    SDL_Log("Jev %s: %s is on its way to the %s site.", GetTeamName(),
                            JevAi_GetUnitName(&*m_unit).c_str(), JevAi_GetTypeName(m_pending_build.unit_type).c_str());
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
                        m_question = "production";

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

            if (ContinueToSite()) {
                return true;
            }

            const bool is_construction = JevAi_IsConstructionUnit(m_unit->GetUnitType());

            m_options.clear();

            if (!is_construction) {
                AddAttackOptions(&*m_unit);
            }

            AddMoveOptions(&*m_unit);

            if (JevAi_IsBuilder(m_unit->GetUnitType())) {
                AddBuildOptions(&*m_unit);
                AddRefillOptions(&*m_unit);
            }

            m_options.push_back({"stay",
                                 Action::STAY,
                                 Point(m_unit->grid_x, m_unit->grid_y),
                                 INVALID_ID,
                                 nullptr,
                                 "stay where it is for the rest of this turn, danger " + std::to_string(danger_here),
                                 -danger_here * 10,
                                 {{"action", "stay"}, {"danger", danger_here}}});
            m_question = is_construction ? "construction" : "military";
            ++m_unit_picks;

            if (m_options.size() > 1 &&
                Ask(&*m_unit, is_construction ? JevAi_ConstructionInstructions : JevAi_MilitaryInstructions)) {
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
