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
#include <set>
#include <tuple>
#include <climits>
#include <cmath>

#include "access.hpp"
#include "ai.hpp"
#include "aiplayer.hpp"
#include "accessmap.hpp"
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
#include "teamunits.hpp"
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

/// Anti Aircraft turrets built without a question once enemy aircraft are seen (the user, 2026-11-13: an aircraft
/// sighting is the signal for air defense; j7-groups4: offered as one option among many, two were built in 8 games)
static constexpr int32_t JevAi_AntiAirTurrets = 2;

/// Scouts of the Defend group that chase raiders once enemy units are seen (the user, 2026-11-13: the first enemy
/// units are mostly Scouts, own Scouts answer them; j7-groups4: 95 of 226 enemy hits landed on unarmed units)
static constexpr int32_t JevAi_Interceptors = 2;

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

static const char* const JevAi_CommandInstructions =
    "You command the military of your team in M.A.X., a turn-based strategy game. You set the size of your task "
    "groups; the game moves their units, and factories produce combat units only to fill open places in a group. The "
    "Defend group holds posts on the way enemies come to your base, on sentry; the Scout group searches for the enemy "
    "base and watches the way in. Every combat unit costs raw materials that Eco-spheres, mines and power also need, "
    "and Eco-spheres score the points that win. You are asked only when something changed; the plan stands unless "
    "that calls for a different one. Choose one change, or keep the plan.";

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
static bool JevAi_IsSiteFree(uint16_t team, ResourceID unit_type, Point site, UnitInfo* builder = nullptr) {
    const int32_t size = (ResourceManager_GetUnit(unit_type).GetFlags() & BUILDING) ? 2 : 1;

    for (int32_t x = site.x; x < site.x + size; ++x) {
        for (int32_t y = site.y; y < site.y + size; ++y) {
            if (!Access_IsAccessible(unit_type, team, x, y, AccessModifier_EnemySameClassBlocks)) {
                return false;
            }
            // A stationary builder can otherwise be sent to a site occupied by an idle friendly unit forever.
            // Roads and connectors remain passable; the builder itself may stand inside its building footprint.
            if (builder && unit_type != CNCT_4W && unit_type != ROAD) {
                UnitInfo* occupant = Access_GetTeamUnit(x, y, team, MOBILE_LAND_UNIT);
                if (occupant && occupant != builder) {
                    return false;
                }
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

/// A ground combat unit that outranges a Tank: Assault Gun, Rocket Launcher, Missile Crawler.
static bool JevAi_IsLongRange(uint16_t team, ResourceID unit_type) {
    return JevAi_IsGroundCombatUnit(team, unit_type) &&
           UnitsManager_GetCurrentUnitValues(&UnitsManager_TeamInfo[team], unit_type)->GetAttribute(ATTRIB_RANGE) >
               UnitsManager_GetCurrentUnitValues(&UnitsManager_TeamInfo[team], TANK)->GetAttribute(ATTRIB_RANGE);
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

Complex* JevAi::GetEconomyComplex() const {
    // A detached turret can precede the base in the unit list. Prefer the network with the most living mines,
    // then the most raw storage; a stopped mining network still counts so its recovery facts remain visible.
    Complex* best = nullptr;
    std::pair<int32_t, int32_t> best_score{-1, -1};
    std::vector<Complex*> seen;
    for (auto it = UnitsManager_StationaryUnits.Begin(), end = UnitsManager_StationaryUnits.End(); it != end; ++it) {
        Complex* complex = (*it).GetComplex();
        if ((*it).team != m_team || (*it).hits == 0 || !complex ||
            std::find(seen.begin(), seen.end(), complex) != seen.end()) {
            continue;
        }
        seen.push_back(complex);
        int32_t mines = 0;
        for (auto other = UnitsManager_StationaryUnits.Begin(); other != end; ++other) {
            mines += (*other).team == m_team && (*other).hits > 0 && (*other).GetComplex() == complex &&
                     (*other).GetUnitType() == MININGST;
        }
        Cargo materials, capacity;
        complex->GetCargoInfo(materials, capacity);
        const std::pair<int32_t, int32_t> score{mines, capacity.raw};
        if (score > best_score) {
            best = complex;
            best_score = score;
        }
    }
    return best;
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

    // land units drive over connectors and roads
    if (unit_type == CNCT_4W || unit_type == ROAD) {
        return true;
    }
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

        // each unit is moved aside once per turn: a move that failed would only fail again
        if (!blocker || blocker == parent || !Task_IsReadyToTakeOrders(blocker) || blocker->speed == 0 ||
            std::find(m_turn_cleared_units.begin(), m_turn_cleared_units.end(), blocker->GetId()) !=
                m_turn_cleared_units.end()) {
            continue;
        }

        // the cheapest free cell that is no factory exit and that the blocker reaches this turn
        Point best;
        int32_t best_distance = INT32_MAX;

        ComputeReach(blocker);

        for (int32_t x = cell.x - 4; x <= cell.x + 4; ++x) {
            for (int32_t y = cell.y - 4; y <= cell.y + 4; ++y) {
                const Point target(x, y);
                const int32_t distance = GetReachCost(target);

                if (JevAi_IsInsideMap(target) && distance > 0 && distance <= blocker->speed &&
                    distance < best_distance && !IsFactoryExit(target) &&
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
        m_turn_cleared_units.push_back(blocker->GetId());
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

        if (JevAi_IsDefense((*it).GetUnitType()) && ((*it).GetUnitType() == ANTIAIR) == (unit_type == ANTIAIR) &&
            distance <= (*it).GetBaseValues()->GetAttribute(ATTRIB_RANGE)) {
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

    // enemy combat units in reach of the base this turn, and how many outrange a Tank
    Point center;
    int32_t extent = 0;
    const bool has_center = GetBaseCenter(center, extent);
    const int32_t tank_range = UnitsManager_GetCurrentUnitValues(&UnitsManager_TeamInfo[m_team], TANK)->GetAttribute(ATTRIB_RANGE);
    int32_t near_base = 0;
    int32_t outranging = 0;

    // aircraft fly anywhere: they say nothing about the way in, only that air defense is needed
    for (auto it = UnitsManager_MobileAirUnits.Begin(), it_end = UnitsManager_MobileAirUnits.End(); it != it_end; ++it) {
        if ((*it).team != m_team && (*it).team != PLAYER_TEAM_ALIEN && (*it).hits > 0 && (*it).IsVisibleToTeam(m_team)) {
            UnitSpotted(it->Get());
        }
    }

    for (auto* list : {&UnitsManager_MobileLandSeaUnits, &UnitsManager_StationaryUnits}) {
        for (auto it = list->Begin(), it_end = list->End(); it != it_end; ++it) {
            if ((*it).team == m_team || (*it).team == PLAYER_TEAM_ALIEN || (*it).hits == 0 ||
                !(*it).IsVisibleToTeam(m_team) || (*it).GetUnitType() == CNCT_4W || (*it).GetUnitType() == ROAD) {
                continue;
            }

            m_enemy_seen = true;
            m_enemy_base_seen = m_enemy_base_seen || ((*it).flags & STATIONARY);

            // an Air Units Plant announces aircraft: air defense takes 6 turns to build (m-aa1: the first Ground
            // Attack Plane came on turn 90 and killed the Engineer on its way to the Anti Aircraft site)
            if ((*it).GetUnitType() == AIRPLT && !m_enemy_air_seen) {
                m_enemy_air_seen = true;

                SDL_Log("Jev %s: enemy Air Units Plant seen at %s: air defense comes first now.", GetTeamName(),
                        JevAi_FormatPoint(Point((*it).grid_x, (*it).grid_y)).c_str());
            }
            // the Defend group answers the first enemy units, not only the enemy base (the user, 2026-11-13)
            m_defend_size = std::max(m_defend_size, 4);

            const Point at((*it).grid_x, (*it).grid_y);
            const bool building = ((*it).flags & STATIONARY) != 0;
            const int32_t range =
                (*it).GetBaseValues()->GetAttribute(ATTRIB_ATTACK) > 0 ? (*it).GetBaseValues()->GetAttribute(ATTRIB_RANGE)
                                                                        : 0;

            if (!building && range > 0) {
                m_enemy_army[(*it).GetId()] = {turn, range > tank_range};
            }

            if (!building && range > 0 && has_center && JevAi_GetDistance(center, at) <= extent + 15) {
                ++near_base;
                outranging += range > tank_range;
            }
            auto known = std::find_if(m_sightings.begin(), m_sightings.end(),
                                      [at](const Sighting& item) { return item.at == at; });

            if (known != m_sightings.end()) {
                known->turn = turn;
                known->building = known->building || building;
                known->range = std::max(known->range, range);

            } else {
                m_sightings.push_back({at, turn, building, range});
            }
        }
    }
    m_threats.push_back({turn, near_base, outranging});

    for (auto item = m_enemy_army.begin(); item != m_enemy_army.end();) {
        item = (turn - item->second.first > 15) ? m_enemy_army.erase(item) : std::next(item);
    }

    if (m_threats.size() > 10) {
        m_threats.erase(m_threats.begin());
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

bool JevAi::FindConnectorPath(Point site, int32_t size, std::vector<Point>& path) const {
    // the shortest line of connectors, cell to cell along edges, from a site to a building of the base
    constexpr int32_t maximum_length = 6;
    constexpr Point steps[4] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    const auto in_site = [site, size](Point cell) {
        return cell.x >= site.x && cell.x < site.x + size && cell.y >= site.y && cell.y < site.y + size;
    };
    std::vector<std::pair<Point, int32_t>> queue;
    std::vector<Point> seen;

    path.clear();

    for (int32_t x = site.x; x < site.x + size; ++x) {
        for (int32_t y = site.y; y < site.y + size; ++y) {
            for (const Point step : steps) {
                const Point cell(x + step.x, y + step.y);

                if (!in_site(cell) && JevAi_IsInsideMap(cell) &&
                    std::find(seen.begin(), seen.end(), cell) == seen.end()) {
                    seen.push_back(cell);
                    queue.push_back({cell, -1});
                }
            }
        }
    }

    for (size_t index = 0; index < queue.size(); ++index) {
        const Point cell = queue[index].first;
        int32_t length = 0;

        for (int32_t parent = queue[index].second; parent >= 0; parent = queue[parent].second) {
            ++length;
        }

        if (length >= maximum_length ||
            !Access_IsAccessible(CNCT_4W, m_team, cell.x, cell.y, AccessModifier_EnemySameClassBlocks)) {
            continue;
        }

        if (TouchesBase(CNCT_4W, cell)) {
            for (int32_t at = static_cast<int32_t>(index); at >= 0; at = queue[at].second) {
                path.push_back(queue[at].first);
            }

            return true;
        }

        for (const Point step : steps) {
            const Point next(cell.x + step.x, cell.y + step.y);

            if (!in_site(next) && JevAi_IsInsideMap(next) && std::find(seen.begin(), seen.end(), next) == seen.end()) {
                seen.push_back(next);
                queue.push_back({next, static_cast<int32_t>(index)});
            }
        }
    }

    return false;
}

std::vector<Point> JevAi::GetLinkCells(UnitInfo* builder) const {
    // for each mine waiting for its connectors, the next one to lay, nearest to this builder
    std::vector<Point> cells;

    for (const auto& link : m_links) {
        const Point* next = nullptr;
        int32_t nearest = INT32_MAX;

        for (const Point& cell : link.connectors) {
            const int32_t distance = JevAi_GetDistance(cell, Point(builder->grid_x, builder->grid_y));

            if (JevAi_IsSiteFree(m_team, CNCT_4W, cell) && distance < nearest) {
                next = &cell;
                nearest = distance;
            }
        }

        if (next) {
            cells.push_back(*next);
        }
    }

    return cells;
}

std::vector<Point> JevAi::FindMineSites(UnitInfo* builder, bool fuel_first) const {
    // the richest surveyed sites around the base that join it: a mine away from the base has no power
    constexpr int32_t search_radius = 14;
    constexpr int32_t maximum_sites = 3;
    constexpr int32_t minimum_spacing = 3;
    Point base;
    std::vector<std::pair<int32_t, Point>> candidates;

    if (!GetBasePosition(base)) {
        return {};
    }

    for (int32_t x = base.x - search_radius; x <= base.x + search_radius; ++x) {
        for (int32_t y = base.y - search_radius; y <= base.y + search_radius; ++y) {
            const Point site(x, y);
            int16_t raw;
            int16_t gold;
            int16_t fuel;

            if (!JevAi_IsInsideMap(site) || !JevAi_IsInsideMap(Point(x + 1, y + 1))) {
                continue;
            }

            Survey_GetTotalResourcesInArea(x, y, 1, &raw, &gold, &fuel, false, m_team);

            if (raw + fuel + gold == 0 || !JevAi_IsSiteFree(m_team, MININGST, site, builder) ||
                !LeavesExits(MININGST, site)) {
                continue;
            }

            // a site away from the base needs connectors; each one costs a little
            std::vector<Point> path;

            if (!TouchesBase(MININGST, site) && !FindConnectorPath(site, 2, path)) {
                continue;
            }

            // raw materials pay for everything, fuel only runs generators; fuel first when power waits for it
            candidates.push_back(
                {(fuel_first ? raw + fuel * 3 : raw * 2 + fuel) + gold - static_cast<int32_t>(path.size()), site});
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

bool JevAi::GetBaseCenter(Point& center, int32_t& extent) const {
    // the middle of the buildings worth protecting, and how far they reach from it
    int64_t sum_x = 0;
    int64_t sum_y = 0;
    int32_t count = 0;

    for (auto it = UnitsManager_StationaryUnits.Begin(), it_end = UnitsManager_StationaryUnits.End(); it != it_end;
         ++it) {
        if ((*it).team == m_team && (*it).hits > 0 && JevAi_GetProtectionWeight((*it).GetUnitType()) > 0) {
            sum_x += (*it).grid_x;
            sum_y += (*it).grid_y;
            ++count;
        }
    }

    if (count == 0) {
        return false;
    }

    center = Point(static_cast<int32_t>(sum_x / count), static_cast<int32_t>(sum_y / count));
    extent = 1;

    for (auto it = UnitsManager_StationaryUnits.Begin(), it_end = UnitsManager_StationaryUnits.End(); it != it_end;
         ++it) {
        if ((*it).team == m_team && (*it).hits > 0 && JevAi_GetProtectionWeight((*it).GetUnitType()) > 0) {
            extent = std::max(extent, JevAi_GetDistance(center, Point((*it).grid_x, (*it).grid_y)) + 1);
        }
    }

    return true;
}

bool JevAi::GetEnemyDirection(Point& origin) const {
    // where the enemy comes from: its buildings the team has seen, else recent sightings, else the map center,
    // since the enemy usually starts elsewhere on the map; true when it is known
    std::vector<Point> approach;

    if (GetApproach(approach, origin)) {
        return true;
    }

    origin = Point(ResourceManager_MapSize.x / 2, ResourceManager_MapSize.y / 2);

    return false;
}

std::vector<Point> JevAi::FindDefenseSites(UnitInfo* builder, ResourceID unit_type) const {
    // a few distinct legal sites by role, best first. Gun Turrets form a wall on the outside of the base, on the side
    // the enemy comes from; long range defenses stand behind it, inside the base's edge, covering the way in
    constexpr int32_t maximum_sites = 2;
    const bool is_wall = unit_type == GUNTURRT;
    // aircraft come from any side: air defense covers the base's buildings, spread out
    const bool is_air = unit_type == ANTIAIR;
    const int32_t minimum_spacing = is_wall ? 2 : (is_air ? 4 : 3);
    const int32_t range =
        UnitsManager_GetCurrentUnitValues(&UnitsManager_TeamInfo[m_team], unit_type)->GetAttribute(ATTRIB_RANGE);
    const Point position(builder->grid_x, builder->grid_y);
    std::vector<std::pair<int64_t, Point>> candidates;
    std::vector<Point> approach;
    Point origin;
    Point center;
    int32_t extent;

    if (!GetBaseCenter(center, extent)) {
        return {};
    }

    GetApproach(approach, origin);
    GetEnemyDirection(origin);

    const double toward_x = origin.x - center.x;
    const double toward_y = origin.y - center.y;
    const double toward_length = std::max(1.0, std::sqrt(toward_x * toward_x + toward_y * toward_y));

    for (int32_t x = center.x - extent - 4; x <= center.x + extent + 4; ++x) {
        for (int32_t y = center.y - extent - 4; y <= center.y + extent + 4; ++y) {
            const Point site(x, y);
            const int32_t from_center = JevAi_GetDistance(site, center);

            // the wall stands one to three cells outside the base, long range defenses inside its edge
            if ((is_wall && (from_center < extent + 1 || from_center > extent + 3)) ||
                (!is_wall && from_center > extent + 1)) {
                continue;
            }

            if (!JevAi_IsInsideMap(site) || !JevAi_IsSiteFree(m_team, unit_type, site, builder) ||
                !LeavesExits(unit_type, site)) {
                continue;
            }

            // on the side toward the enemy
            const double facing =
                ((x - center.x) * toward_x + (y - center.y) * toward_y) / (std::max(1, from_center) * toward_length);

            if (facing < 0.3 && !is_air) {
                continue;
            }

            int64_t score = is_air ? 0 : static_cast<int64_t>(facing * 20);

            for (auto it = UnitsManager_StationaryUnits.Begin(), it_end = UnitsManager_StationaryUnits.End();
                 it != it_end; ++it) {
                if ((*it).team != m_team || (*it).hits == 0) {
                    continue;
                }

                const int32_t distance = JevAi_GetDistance(site, Point((*it).grid_x, (*it).grid_y));
                const int32_t weight = JevAi_GetProtectionWeight((*it).GetUnitType());

                // what it shields, and the defenses it fights next to
                if (weight > 0 && distance <= range) {
                    score += weight * 5;
                }

                if (JevAi_IsDefense((*it).GetUnitType()) && distance <= 3) {
                    score += 8;
                }
            }

            for (const Point cell : approach) {
                score += (!is_air && JevAi_GetDistance(site, cell) <= range) ? 15 : 0;
            }

            score -= JevAi_GetDistance(site, position);
            candidates.push_back({score, site});
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

    std::vector<Point> sites;

    for (const auto& candidate : candidates) {
        if (std::all_of(sites.begin(), sites.end(), [&candidate, minimum_spacing](Point chosen) {
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

            if (!JevAi_IsSiteFree(m_team, unit_type, candidate, builder) || !LeavesExits(unit_type, candidate)) {
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
                (station.GetOrder() != ORDER_POWER_ON && station.GetOrder() != ORDER_POWER_OFF &&
                 station.GetOrder() != ORDER_AWAIT)) {
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
            if (station.GetOrder() == ORDER_POWER_ON) {
                fuel_mined += station.fuel_mining;
            }
        }

        // Recover the mine and its generators together. Waiting for mined fuel before powering a stopped fuel
        // mine deadlocks an empty network. The normal allocation manager checks the combined next-turn balance
        // and shuts down anything it cannot supply; it does not grant free fuel or power.
        for (auto it = UnitsManager_StationaryUnits.Begin(), it_end = UnitsManager_StationaryUnits.End(); it != it_end;
             ++it) {
            if ((*it).GetComplex() == complex && (*it).GetUnitType() == MININGST && (*it).hits > 0 &&
                (*it).fuel_mining > 0 &&
                ((*it).GetOrder() == ORDER_POWER_OFF || (*it).GetOrder() == ORDER_AWAIT)) {
                UnitsManager_SetNewOrder(it->Get(), ORDER_POWER_ON, ORDER_STATE_INIT);
                ProductionManager_OptimizeProduction(m_team, complex, nullptr, false);
            }
        }

        Access_UpdateResourcesTotal(complex);

        // Recovery may have changed which mines can run.
        fuel_mined = 0;
        for (auto it = UnitsManager_StationaryUnits.Begin(), it_end = UnitsManager_StationaryUnits.End(); it != it_end;
             ++it) {
            if ((*it).GetComplex() == complex && (*it).GetUnitType() == MININGST && (*it).hits > 0 &&
                (*it).GetOrder() == ORDER_POWER_ON) {
                fuel_mined += (*it).fuel_mining;
            }
        }

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

        // a Mining Station is the base's income and its only fuel: it gets power before the rest, as a human player
        // would switch it on; a fuel mine may take the power of an Eco-sphere, since its fuel runs more generators
        int32_t power_left = complex->power;
        int32_t workers_left = complex->workers;

        for (auto it = UnitsManager_StationaryUnits.Begin(), it_end = UnitsManager_StationaryUnits.End(); it != it_end;
             ++it) {
            UnitInfo& mine = *it;

            if (mine.GetComplex() != complex || mine.GetUnitType() != MININGST || mine.hits == 0 ||
                (mine.GetOrder() != ORDER_POWER_OFF && mine.GetOrder() != ORDER_AWAIT)) {
                continue;
            }

            if (power_left < Cargo_GetPowerConsumptionRate(MININGST) && mine.fuel_mining_max > 0) {
                for (auto it2 = UnitsManager_StationaryUnits.Begin(), it2_end = UnitsManager_StationaryUnits.End();
                     it2 != it2_end; ++it2) {
                    if ((*it2).GetComplex() == complex && (*it2).GetUnitType() == GREENHSE &&
                        (*it2).GetOrder() == ORDER_POWER_ON) {
                        SDL_Log("Jev %s: the Eco-sphere at %s gives its power to the Mining Station at %s.",
                                GetTeamName(), JevAi_FormatPoint(Point((*it2).grid_x, (*it2).grid_y)).c_str(),
                                JevAi_FormatPoint(Point(mine.grid_x, mine.grid_y)).c_str());

                        UnitsManager_SetNewOrder(it2->Get(), ORDER_POWER_OFF, ORDER_STATE_INIT);

                        power_left += Cargo_GetPowerConsumptionRate(GREENHSE);
                        workers_left += Cargo_GetLifeConsumptionRate(GREENHSE);
                        break;
                    }
                }
            }

            if (power_left >= Cargo_GetPowerConsumptionRate(MININGST)) {
                UnitsManager_SetNewOrder(it->Get(), ORDER_POWER_ON, ORDER_STATE_INIT);

                power_left -= Cargo_GetPowerConsumptionRate(MININGST);
            }
        }

        // an Eco-sphere only scores while it works

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
            // a factory halted for lack of power or materials is asked again only when it could run: asking it every
            // turn only repeats the same choice (b13 world 3: "produce a Tank" 64 times)
            if ((order == ORDER_HALT_BUILDING || order == ORDER_HALT_BUILDING_2) && unit.GetComplex()) {
                const Cargo balance = ProductionManager_PreviewProduction(m_team, unit.GetComplex(), &unit);

                if (balance.raw < 0 || balance.fuel < 0 || balance.gold < 0 || balance.power < 0 || balance.life < 0) {
                    continue;
                }
            }

            return &unit;
        }
    }

    return nullptr;
}

void JevAi::ComputeReach(UnitInfo* unit) {
    // the costs a move order's path request uses (Paths_RequestPath, PathsManager): cell costs in quarter movement
    // points, diagonal steps 1.5 times, each step rounded up to whole points as Paths_CalculateStep charges them
    const Point size = ResourceManager_MapSize;
    AccessMap map(ResourceManager_GetActiveWorld());
    std::vector<std::pair<int32_t, int32_t>> queue;
    const auto later = [](const std::pair<int32_t, int32_t>& a, const std::pair<int32_t, int32_t>& b) {
        return a.first > b.first;
    };

    map.Init(unit, AccessModifier_SameClassBlocks, CAUTION_LEVEL_NONE);
    m_reach.assign(size.x * size.y, -1);

    if (!JevAi_IsInsideMap(Point(unit->grid_x, unit->grid_y))) {
        return;
    }

    m_reach[unit->grid_x * size.y + unit->grid_y] = 0;
    queue.push_back({0, unit->grid_x * size.y + unit->grid_y});

    while (!queue.empty()) {
        std::pop_heap(queue.begin(), queue.end(), later);
        const auto [cost, index] = queue.back();
        queue.pop_back();

        if (cost > m_reach[index]) {
            continue;
        }

        const Point cell(index / size.y, index % size.y);

        for (int32_t direction = 0; direction < 8; ++direction) {
            const Point next = cell + DIRECTION_OFFSETS[direction];

            if (!JevAi_IsInsideMap(next)) {
                continue;
            }

            int32_t step = map(next.x, next.y) & 0x1F;

            if (step == 0) {
                continue;
            }

            if (DIRECTION_OFFSETS[direction].x != 0 && DIRECTION_OFFSETS[direction].y != 0) {
                step = (step * 3) / 2;
            }

            const int32_t next_cost = cost + (step + 3) / 4;
            const int32_t next_index = next.x * size.y + next.y;

            if (m_reach[next_index] < 0 || next_cost < m_reach[next_index]) {
                m_reach[next_index] = next_cost;
                queue.push_back({next_cost, next_index});
                std::push_heap(queue.begin(), queue.end(), later);
            }
        }
    }
}

int32_t JevAi::GetReachCost(Point cell) const {
    if (!JevAi_IsInsideMap(cell) || m_reach.size() != static_cast<size_t>(ResourceManager_MapSize.x) *
                                                          static_cast<size_t>(ResourceManager_MapSize.y)) {
        return -1;
    }

    return m_reach[cell.x * ResourceManager_MapSize.y + cell.y];
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

            // a straight line can cross buildings, water or rough ground: offer only what the unit reaches this turn
            const int32_t reach = GetReachCost(destination);

            if (reach < 0 || reach > speed) {
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

                // only what the team has surveyed, and how much there is still unsurveyed
                Survey_GetTotalResourcesInArea(destination.x - 1, destination.y - 1, 2, &raw, &gold, &fuel, false,
                                               m_team);

                int32_t unsurveyed = 0;

                for (int32_t x = destination.x - 1; x <= destination.x + 1; ++x) {
                    for (int32_t y = destination.y - 1; y <= destination.y + 1; ++y) {
                        unsurveyed += JevAi_IsInsideMap(Point(x, y)) &&
                                      !(ResourceManager_CargoMap[ResourceManager_MapSize.x * y + x] &
                                        UnitsManager_TeamInfo[m_team].team_units->hash_team_id);
                    }
                }

                facts += ", surveyed resources there raw " + std::to_string(raw) + " fuel " + std::to_string(fuel) +
                         " gold " + std::to_string(gold) + ", " + std::to_string(unsurveyed) +
                         " of 9 cells not surveyed";
                data["resources"] = {{"raw", raw}, {"fuel", fuel}, {"gold", gold}};
                data["unsurveyed_cells"] = unsurveyed;
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
    Cargo base_stock;
    Cargo base_capacity;
    bool enemies_seen = false;

    if (Complex* complex = GetEconomyComplex()) {
        spare_power = complex->power;
        complex->GetCargoInfo(base_stock, base_capacity);
    }

    // as the classic AI does: power needed by all buildings, also those being built, against all generators
    int32_t power_needed = 0;
    int32_t power_generated = 0;
    int32_t generators = 0;
    // the same for workers: Eco-spheres need them, Habitats give them
    int32_t workers_needed = 0;
    int32_t workers_given = 0;
    int32_t factories = 0;
    int32_t idle_mine_fuel = 0;
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

                // a switched off mine mines fuel again once it has power
                if (power_unit == MININGST && (*it).GetOrder() != ORDER_POWER_ON) {
                    idle_mine_fuel += (*it).fuel_mining_max;
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

    // an Eco-sphere is wanted when a factory stands and a worker is free; without power it scores nothing, so the
    // power chain comes first: a Power Generator when the fuel runs one more, else a mine on fuel (the user: Jev
    // builds the power and the Eco-spheres)
    const int32_t eco_power = Cargo_GetPowerConsumptionRate(GREENHSE);
    const bool eco_needs_power =
        factories > 0 && workers_given - workers_needed >= 1 && power_generated - power_needed < eco_power;
    // power planned: the buildings and those being built, the first factory, a wanted Eco-sphere; when the fuel mined
    // cannot run the generators that takes, a mine on fuel comes first (the user: a fuel mine when fuel and power are
    // needed or planned to be needed)
    const int32_t first_factory_power =
        (CountOwnUnits(LANDPLT) + CountOwnUnits(LIGHTPLT) == 0) ? Cargo_GetPowerConsumptionRate(LANDPLT) : 0;
    const int32_t power_planned = power_needed + first_factory_power + (eco_needs_power ? eco_power : 0);
    const int32_t generators_planned =
        generators + std::max(0, (power_planned - power_generated + -Cargo_GetPowerConsumptionRate(POWGEN) - 1) /
                                     std::max(1, -Cargo_GetPowerConsumptionRate(POWGEN)));
    const bool eco_needs_fuel = power_planned > power_generated &&
                                fuel_mining_capacity.fuel + idle_mine_fuel <
                                    generators_planned * Cargo_GetFuelConsumptionRate(POWGEN);

    for (const ResourceID unit_type : Builder_GetBuildableUnits(m_team, builder->GetUnitType())) {
        Point found_site;

        // the game only offers buildings that make sense now
        if (unit_type == POWGEN) {
            // a factory needs power too, plan for the first one
            const int32_t factory_power =
                (CountOwnUnits(LANDPLT) + CountOwnUnits(LIGHTPLT) == 0) ? Cargo_GetPowerConsumptionRate(LANDPLT) : 0;

            if (power_needed + factory_power + (eco_needs_power ? eco_power : 0) <= power_generated ||
                fuel_mining_capacity.fuel + idle_mine_fuel < (generators + 1) * Cargo_GetFuelConsumptionRate(POWGEN)) {
                continue;
            }

        } else if (JevAi_IsFactory(unit_type)) {
            if ((unit_type != LIGHTPLT && unit_type != LANDPLT) || CountOwnUnits(unit_type) >= 4) {
                continue;
            }

        } else if (unit_type == HABITAT || unit_type == GREENHSE) {
            // points come after the first factory; an Eco-sphere needs a free worker, a Habitat gives three
            if (factories == 0 || (unit_type == HABITAT && workers_given - workers_needed >= 1) ||
                (unit_type == GREENHSE && workers_given - workers_needed < 1)) {
                continue;
            }

            // an Eco-sphere without power scores nothing: only while the generators have power to spare or the fuel
            // mined runs one more generator (j5-groups world 6: 8 fuel ran 4 generators, the fifth Eco-sphere idled)
            if (unit_type == GREENHSE &&
                power_generated - power_needed < Cargo_GetPowerConsumptionRate(GREENHSE) &&
                fuel_mining_capacity.fuel + idle_mine_fuel < (generators + 1) * Cargo_GetFuelConsumptionRate(POWGEN)) {
                continue;
            }

        } else if (unit_type == RADAR) {
            // vision around the base: the first Radar once a factory stands, a second one once the enemy is seen
            if (factories == 0 || CountOwnUnits(RADAR) >= (enemies_seen ? 2 : 1)) {
                continue;
            }

        } else if (unit_type == CNCT_4W) {
            if (m_links.empty()) {
                continue;
            }

        } else if (unit_type == ADUMP) {
            // raw materials mined while the base storage is full are lost
            if (base_capacity.raw == 0 || base_stock.raw * 5 < base_capacity.raw * 4 ||
                !FindBuildSite(builder, unit_type, found_site)) {
                continue;
            }

        } else if (unit_type == ANTIAIR) {
            if (!m_enemy_air_seen) {
                continue;
            }

        } else if (unit_type != MININGST && unit_type != GUNTURRT && unit_type != ANTIMSSL && unit_type != ARTYTRRT) {
            continue;
        }

        std::vector<Point> sites;

        if (unit_type == GUNTURRT || unit_type == ANTIMSSL || unit_type == ARTYTRRT || unit_type == ANTIAIR) {
            sites = FindDefenseSites(builder, unit_type);

        } else if (unit_type == MININGST) {
            sites = FindMineSites(builder, eco_needs_fuel);

        } else if (unit_type == CNCT_4W) {
            sites = GetLinkCells(builder);

        } else if (unit_type == ADUMP) {
            sites.push_back(found_site);

        } else if (FindBuildSite(builder, unit_type, found_site)) {
            sites.push_back(found_site);
        }

        for (const Point site : sites) {
            const int32_t turns = BuildMenu_GetTurnsToBuild(unit_type, m_team);
            const int32_t cost = turns * rate;
            if (cost > builder->storage) {
                continue;
            }

            if (JevAi_IsDefense(unit_type)) {
                // a defense next to a factory exit walls in the units it releases (j3-allup world 0: 23 Gun Turrets,
                // a Tank stuck on the exit); a spot that three defenses reach already gains little from a fourth
                bool walls_exit = false;

                for (int32_t dx = -1; dx <= 1 && !walls_exit; ++dx) {
                    for (int32_t dy = -1; dy <= 1 && !walls_exit; ++dy) {
                        walls_exit = IsFactoryExit(Point(site.x + dx, site.y + dy));
                    }
                }

                if (walls_exit || GetDefenseCoverage(unit_type, site)["defenses_covering_site"].get<int32_t>() >= 3) {
                    continue;
                }
            }
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

                if (eco_needs_fuel && fuel > 0) {
                    facts += ", its fuel runs more Power Generators: the base needs more power than its fuel can make";
                    data["fuel_for_eco_spheres"] = true;
                }
                data["resources"] = {{"raw", raw}, {"fuel", fuel}, {"gold", gold}};

                // what the base mines now, to compare with
                Cargo mining;
                Cargo mining_capacity;
                int32_t mines = 0;

                for (auto it = UnitsManager_StationaryUnits.Begin(), it_end = UnitsManager_StationaryUnits.End();
                     it != it_end; ++it) {
                    if ((*it).team == m_team && (*it).hits > 0 && (*it).GetUnitType() == MININGST) {
                        ++mines;

                        if ((*it).GetComplex()) {
                            (*it).GetComplex()->GetCargoMining(mining, mining_capacity);
                        }
                    }
                }

                std::vector<Point> connectors;

                if (TouchesBase(MININGST, site)) {
                    facts += ", joins your base";
                    data["joins_base"] = true;

                } else if (FindConnectorPath(site, 2, connectors)) {
                    facts += ", not joined to your base: it works once " + std::to_string(connectors.size()) +
                             " connectors join it, which Engineers lay";
                    data["joins_base"] = false;
                    data["connectors_needed"] = static_cast<int32_t>(connectors.size());
                }

                facts += ", a station mines up to 16 per turn in all; your base mines raw " +
                         std::to_string(mining.raw) + " fuel " + std::to_string(mining.fuel) + " gold " +
                         std::to_string(mining.gold) + " per turn from " + std::to_string(mines) + " stations";
                data["base_mining"] = {
                    {"raw", mining.raw}, {"fuel", mining.fuel}, {"gold", mining.gold}, {"stations", mines}};
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
                    if (eco_needs_power) {
                        facts += ", its power runs an Eco-sphere, which the base has no power for yet";
                        data["for_eco_sphere"] = true;
                    }

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

            if (unit_type == CNCT_4W) {
                for (const auto& link : m_links) {
                    if (std::find(link.connectors.begin(), link.connectors.end(), site) != link.connectors.end()) {
                        const auto left =
                            std::count_if(link.connectors.begin(), link.connectors.end(),
                                          [this](Point cell) { return JevAi_IsSiteFree(m_team, CNCT_4W, cell); });

                        facts += ", one of " + std::to_string(left) +
                                 " connectors still needed to join the Mining Station at " +
                                 JevAi_FormatPoint(link.mine) + " to your base; it works once joined";
                        data["link"] = {{"mine_at", JevAi_PointData(link.mine)}, {"left", left}};
                        score = 35;
                        break;
                    }
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

                // the site's role, and whether the enemy's side is known or only guessed
                Point origin;
                const bool direction_known = GetEnemyDirection(origin);
                const std::string role = unit_type == GUNTURRT ? "wall" : "behind the wall";

                facts += unit_type == GUNTURRT ? ", part of a wall on the outside of your base"
                                               : ", long range defense behind the wall";
                facts += direction_known ? ", on the side the enemy comes from"
                                         : ", on the side toward the map center: the enemy base is not found yet";
                data["coverage"] = coverage;
                data["coverage"]["role"] = role;
                data["coverage"]["enemy_side_known"] = direction_known;
                score += 10;
            }

            if (unit_type == ADUMP) {
                const int32_t adds = UnitsManager_GetCurrentUnitValues(&UnitsManager_TeamInfo[m_team], ADUMP)
                                         ->GetAttribute(ATTRIB_STORAGE);

                facts += ", stores raw materials: your base holds " + std::to_string(base_stock.raw) + " of " +
                         std::to_string(base_capacity.raw) + " and loses what it mines while full; this adds " +
                         std::to_string(adds);
                data["storage"] = {{"base_raw", base_stock.raw}, {"capacity", base_capacity.raw}, {"adds", adds}};
                score = 40;
            }

            m_options.push_back(
                {"b" + std::to_string(m_options.size()), Action::BUILD, site, unit_type, nullptr, facts, score, data});
            m_options.back().build_rate = 1;

            // the faster rows of the build menu, when the cargo pays for them: the engine's own turns and cost
            int32_t normal_turns;

            builder->GetTurnsToBuild(unit_type, 1, &normal_turns);

            int32_t fastest_turns = normal_turns;
            const Option normal = m_options.back();

            // one faster row, the fastest the cargo pays for: fewer near copies, fewer tokens
            for (const int32_t rate : {4, 2}) {
                int32_t speed_turns;
                const int32_t speed_cost = builder->GetTurnsToBuild(unit_type, rate, &speed_turns);

                if (speed_turns >= fastest_turns || speed_cost > builder->storage) {
                    continue;
                }

                Option fast = normal;

                fast.key = "b" + std::to_string(m_options.size());
                fast.build_rate = rate;
                fast.facts += ", built at x" + std::to_string(rate) + " speed: " + std::to_string(speed_turns) +
                              (speed_turns == 1 ? " turn" : " turns") + " and " + std::to_string(speed_cost) +
                              " raw materials instead";
                fast.data["speed"] = {{"rate", rate}, {"turns", speed_turns}, {"cost", speed_cost}};
                fastest_turns = speed_turns;
                m_options.push_back(fast);
                break;
            }
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
    Cargo mining;
    Cargo mining_capacity;

    if (factory->GetComplex()) {
        factory->GetComplex()->GetCargoMining(mining, mining_capacity);
    }

    const auto add_supply = [this, factory](Option& option, int32_t speed) {
        if (!factory->GetComplex()) {
            return;
        }
        const Cargo balance = ProductionManager_PreviewProduction(m_team, factory->GetComplex(), factory, speed);
        option.data["supply"] = {{"raw_balance", balance.raw}, {"fuel_balance", balance.fuel},
                                 {"power_balance", balance.power}, {"workers_balance", balance.life},
                                 {"gold_balance", balance.gold}};
        option.facts += "; next turn at this speed: raw " + std::to_string(balance.raw) + ", fuel " +
                        std::to_string(balance.fuel) + ", power " + std::to_string(balance.power) + ", workers " +
                        std::to_string(balance.life);
    };

    int32_t anti_air = 0;
    const int32_t combat = CountDefenders(&anti_air) - anti_air;
    // half of the Defend group outranges a Tank, so that it can answer artillery; the other half holds the front line
    int32_t long_range = 0;

    for (auto it = UnitsManager_MobileLandSeaUnits.Begin(), it_end = UnitsManager_MobileLandSeaUnits.End();
         it != it_end; ++it) {
        long_range += IsDefender(it->Get()) && JevAi_IsLongRange(m_team, (*it).GetUnitType());
    }

    const int32_t long_range_size = (m_defend_size + 1) / 2;
    const int32_t long_range_open = long_range_size - long_range - CountInProduction(false, false, false, false, true);
    const int32_t front_open =
        (m_defend_size - long_range_size) - (combat - long_range) - (CountInProduction(true, false, false) -
                                                                     CountInProduction(false, false, false, false, true));
    // one Mobile Anti Aircraft per Defend group of up to 7, two for a larger one
    const int32_t anti_air_size = m_enemy_air_seen ? 1 + m_defend_size / 8 : 0;
    const int32_t anti_air_open = anti_air_size - anti_air - CountInProduction(false, true, false);
    const int32_t scouts = CountOwnUnits(SCOUT);
    const int32_t interceptor_size = m_enemy_seen ? JevAi_Interceptors : 0;
    const int32_t interceptors = static_cast<int32_t>(m_interceptors.size());
    const int32_t scouts_open = m_scout_size + interceptor_size - scouts - CountInProduction(false, false, true);


    for (const ResourceID unit_type : Builder_GetBuildableUnits(m_team, factory->GetUnitType())) {
        // units serve a group or the economy; a combat unit only fills an open place in the Defend group
        std::string place;
        json place_data;

        if (JevAi_IsGroundCombatUnit(m_team, unit_type)) {
            const bool is_long_range = JevAi_IsLongRange(m_team, unit_type);

            if ((is_long_range ? long_range_open : front_open) <= 0) {
                continue;
            }

            place = ", fills a " + std::string(is_long_range ? "long range" : "front line") +
                    " place in the Defend group, which has " +
                    std::to_string(is_long_range ? long_range : combat - long_range) + " of " +
                    std::to_string(is_long_range ? long_range_size : m_defend_size - long_range_size);
            place_data = {{"group", is_long_range ? "defend_long_range" : "defend_front"},
                          {"has", is_long_range ? long_range : combat - long_range},
                          {"size", is_long_range ? long_range_size : m_defend_size - long_range_size}};

        } else if (unit_type == SP_FLAK) {
            if (anti_air_open <= 0) {
                continue;
            }

            place = ", the Defend group's air defense: enemy aircraft were seen, it has " + std::to_string(anti_air) +
                    " of " + std::to_string(anti_air_size);
            place_data = {{"group", "defend_air"}, {"has", anti_air}, {"size", anti_air_size}};

        } else if (unit_type == SCOUT) {
            if (scouts_open <= 0) {
                continue;
            }

            if (interceptors < interceptor_size) {
                place = ", fills an interceptor place in the Defend group: it chases enemy raiders such as Scouts "
                        "near the base and the builders, the group has " + std::to_string(interceptors) + " of " +
                        std::to_string(interceptor_size);
                place_data = {{"group", "defend_interceptors"}, {"has", interceptors}, {"size", interceptor_size}};

            } else {
                place = ", fills a place in the Scout group, which has " + std::to_string(scouts - interceptors) +
                        " of " + std::to_string(m_scout_size);
                place_data = {{"group", "scouts"}, {"has", scouts - interceptors}, {"size", m_scout_size}};
            }

        } else if (unit_type == SURVEYOR) {
            // Surveyors survey by themselves; five cover the ground the base can use
            if (CountOwnUnits(SURVEYOR) + CountInProduction(false, false, false, true) >= 5) {
                continue;
            }

        } else if (unit_type != CONSTRCT && unit_type != ENGINEER) {
            continue;
        }

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
                              {"have", CountOwnUnits(unit_type)},
                              {"per_turn", rate},
                              {"base_mines", mining.raw}}});

        // the stock can be empty while the income pays for production: say both
        m_options.back().facts += ", uses " + std::to_string(rate) +
                                  " raw materials per turn from the base, which mines " + std::to_string(mining.raw) +
                                  " per turn" + place;

        if (!place_data.is_null()) {
            m_options.back().data["group_place"] = place_data;
        }

        // the faster rows: a factory draws more raw materials from the base each turn, which spends a surplus
        const Option normal = m_options.back();
        m_options.back().build_rate = 1;
        add_supply(m_options.back(), 1);
        int32_t fastest_turns;

        factory->GetTurnsToBuild(unit_type, 1, &fastest_turns);

        for (const int32_t speed : {4, 2}) {
            int32_t speed_turns;
            const int32_t speed_cost = factory->GetTurnsToBuild(unit_type, speed, &speed_turns);
            const int32_t per_turn = Cargo_GetRawConsumptionRate(factory->GetUnitType(), speed);

            if (speed_turns >= fastest_turns) {
                continue;
            }

            Option fast = normal;

            fast.key = "p" + std::to_string(m_options.size());
            fast.build_rate = speed;
            fast.facts += ", produced at x" + std::to_string(speed) + " speed: " + std::to_string(speed_turns) +
                          (speed_turns == 1 ? " turn" : " turns") + " and " + std::to_string(speed_cost) +
                          " raw materials instead, " + std::to_string(per_turn) +
                          " per turn from the base, which mines " + std::to_string(mining.raw) + " per turn";
            fast.data["speed"] = {{"rate", speed},
                                  {"turns", speed_turns},
                                  {"cost", speed_cost},
                                  {"per_turn", per_turn},
                                  {"base_mines", mining.raw}};
            add_supply(fast, speed);
            fastest_turns = speed_turns;
            m_options.push_back(fast);
            break;
        }
    }

    m_options.push_back({"nothing",
                         Action::STAY,
                         Point(),
                         INVALID_ID,
                         nullptr,
                         "produce nothing this turn; the base has " + std::to_string(materials.raw) + " of " +
                             std::to_string(capacity.raw) + " raw materials stored and mines " +
                             std::to_string(mining.raw) + " per turn",
                         0,
                         {{"action", "produce_nothing"},
                          {"base_raw", materials.raw},
                          {"base_raw_capacity", capacity.raw},
                          {"base_mines", mining.raw}}});
}

void JevAi::AddScoutOptions(UnitInfo* scout) {
    // Scouts find the enemy base, then watch the way in from a post on it
    const Point position(scout->grid_x, scout->grid_y);
    Point base;

    if (scout->speed == 0 || !GetBasePosition(base)) {
        return;
    }

    const bool base_found =
        std::any_of(m_sightings.begin(), m_sightings.end(), [](const Sighting& sighting) { return sighting.building; });

    if (!base_found) {
        // the most promising unexplored land: far from the own base, not too far from the scout
        Point best;
        int32_t best_score = INT32_MIN;

        for (int32_t x = 0; x < ResourceManager_MapSize.x; x += 3) {
            for (int32_t y = 0; y < ResourceManager_MapSize.y; y += 3) {
                const Point cell(x, y);

                if (IsExplored(cell) || Access_GetSurfaceType(x, y) != SURFACE_TYPE_LAND) {
                    continue;
                }

                const int32_t score = JevAi_GetDistance(base, cell) - JevAi_GetDistance(position, cell) / 2;

                if (score > best_score) {
                    best = cell;
                    best_score = score;
                }
            }
        }

        if (best_score == INT32_MIN) {
            return;
        }

        const int32_t distance = JevAi_GetDistance(position, best);

        m_options.push_back({"e" + std::to_string(m_options.size()),
                             Action::MOVE,
                             best,
                             INVALID_ID,
                             nullptr,
                             "search for the enemy base: head for unexplored land at " + JevAi_FormatPoint(best) +
                                 ", " + std::to_string(distance) + " cells away and " +
                                 std::to_string(JevAi_GetDistance(base, best)) +
                                 " cells from your base; the enemy base is not found yet, it is likely far from yours",
                             25,
                             {{"action", "search_enemy"},
                              {"to", JevAi_PointData(best)},
                              {"distance", distance},
                              {"from_base", JevAi_GetDistance(base, best)}}});
        return;
    }

    // a post on the way in, about twelve cells out, that no other picket holds
    constexpr int32_t post_distance = 12;
    Point origin;

    GetEnemyDirection(origin);

    const int32_t length = std::max(1, JevAi_GetDistance(base, origin));
    const int32_t step = std::min(post_distance, length - 1);
    const Point target(base.x + (origin.x - base.x) * step / length, base.y + (origin.y - base.y) * step / length);
    Point post;
    int32_t nearest = INT32_MAX;

    for (int32_t x = target.x - 2; x <= target.x + 2; ++x) {
        for (int32_t y = target.y - 2; y <= target.y + 2; ++y) {
            if (JevAi_IsInsideMap(Point(x, y)) &&
                Access_IsAccessible(scout->GetUnitType(), m_team, x, y, AccessModifier_SameClassBlocks) &&
                JevAi_GetDistance(target, Point(x, y)) < nearest) {
                post = Point(x, y);
                nearest = JevAi_GetDistance(target, post);
            }
        }
    }

    if (nearest == INT32_MAX) {
        return;
    }

    for (const auto& picket : m_pickets) {
        if (picket.first != scout->GetId() && JevAi_GetDistance(picket.second, post) < 4) {
            return;
        }
    }

    const int32_t distance = JevAi_GetDistance(position, post);

    m_options.push_back({"w" + std::to_string(m_options.size()),
                         Action::MOVE,
                         post,
                         INVALID_ID,
                         nullptr,
                         "watch the way in: go to " + JevAi_FormatPoint(post) + ", " + std::to_string(distance) +
                             " cells away, on the way from the enemy base near " + JevAi_FormatPoint(origin) +
                             " to yours, and stay there on sentry as early warning and as a spotter for your long "
                             "range defenses",
                         20,
                         {{"action", "picket"},
                          {"to", JevAi_PointData(post)},
                          {"distance", distance},
                          {"enemy_at", JevAi_PointData(origin)}}});
}

bool JevAi::IsDefender(UnitInfo* unit) const {
    // every ground combat unit and every Mobile Anti Aircraft belongs to the Defend group; Scouts and Scanners scout
    return unit->team == m_team && (unit->flags & MOBILE_LAND_UNIT) && unit->hits > 0 &&
           (JevAi_IsGroundCombatUnit(m_team, unit->GetUnitType()) || unit->GetUnitType() == SP_FLAK);
}

int32_t JevAi::CountDefenders(int32_t* anti_air) const {
    int32_t count = 0;

    if (anti_air) {
        *anti_air = 0;
    }

    for (auto it = UnitsManager_MobileLandSeaUnits.Begin(), it_end = UnitsManager_MobileLandSeaUnits.End();
         it != it_end; ++it) {
        if (IsDefender(it->Get())) {
            ++count;

            if (anti_air && (*it).GetUnitType() == SP_FLAK) {
                ++*anti_air;
            }
        }
    }

    return count;
}

int32_t JevAi::CountInProduction(bool combat, bool anti_air, bool scout, bool surveyor, bool long_range) const {
    int32_t count = 0;

    for (auto it = UnitsManager_StationaryUnits.Begin(), it_end = UnitsManager_StationaryUnits.End(); it != it_end;
         ++it) {
        if ((*it).team != m_team || (*it).hits == 0 || !JevAi_IsFactory((*it).GetUnitType()) ||
            (*it).GetOrder() != ORDER_BUILD || (*it).GetOrderState() == ORDER_STATE_UNIT_READY ||
            (*it).GetBuildList().GetCount() == 0) {
            continue;
        }

        const ResourceID product = (*it).GetConstructedUnitType();

        if (product == INVALID_ID) {
            continue;
        }

        count += (combat && JevAi_IsGroundCombatUnit(m_team, product)) || (anti_air && product == SP_FLAK) ||
                 (scout && product == SCOUT) || (surveyor && product == SURVEYOR) ||
                 (long_range && JevAi_IsLongRange(m_team, product));
    }

    return count;
}

bool JevAi::GetThreatAnchor(Point& anchor, Point& origin, bool& enemy_known) const {
    // the middle of the Defend group's posts: a few cells out from the base toward where the enemy comes from, the
    // map center before the enemy is known
    Point center;
    int32_t extent;

    if (!GetBaseCenter(center, extent)) {
        return false;
    }

    enemy_known = GetEnemyDirection(origin);

    const int32_t length = std::max(1, JevAi_GetDistance(center, origin));
    // at the edge of the base, between the buildings and the way in: defenders stay where the base's own turrets help
    // (j5-groups world 3: posts 3 cells beyond the edge stood within reach of the enemy base 15 cells away)
    const int32_t out = std::min(length - 1, std::max(2, extent + 1));

    anchor = Point(center.x + (origin.x - center.x) * out / length, center.y + (origin.y - center.y) * out / length);

    return true;
}

bool JevAi::FindPost(UnitInfo* unit, Point anchor, Point& post) const {
    // the free cell nearest the anchor that the unit can reach, apart from the other posts; needs ComputeReach(unit)
    int64_t best = INT64_MAX;

    for (int32_t radius_x = -5; radius_x <= 5; ++radius_x) {
        for (int32_t radius_y = -5; radius_y <= 5; ++radius_y) {
            const Point cell(anchor.x + radius_x, anchor.y + radius_y);

            if (!JevAi_IsInsideMap(cell) || GetReachCost(cell) < 0 || IsFactoryExit(cell) ||
                !(cell == Point(unit->grid_x, unit->grid_y) ||
                  Access_IsAccessible(unit->GetUnitType(), m_team, cell.x, cell.y, AccessModifier_SameClassBlocks))) {
                continue;
            }

            // never within reach of a known enemy defense, with a margin of two cells
            if (std::any_of(m_sightings.begin(), m_sightings.end(), [&](const Sighting& sighting) {
                    return sighting.building && sighting.range > 0 &&
                           JevAi_GetDistance(sighting.at, cell) <= sighting.range + 2;
                })) {
                continue;
            }

            if (std::any_of(m_posts.begin(), m_posts.end(),
                            [&](const auto& item) {
                                return item.first != unit->GetId() && JevAi_GetDistance(item.second, cell) < 2;
                            }) ||
                std::any_of(m_pickets.begin(), m_pickets.end(),
                            [&](const auto& item) { return JevAi_GetDistance(item.second, cell) < 2; })) {
                continue;
            }

            const int64_t score = static_cast<int64_t>(JevAi_GetDistance(anchor, cell)) * 1000 + GetReachCost(cell);

            if (score < best) {
                best = score;
                post = cell;
            }
        }
    }

    return best != INT64_MAX;
}

int32_t JevAi::ScriptDefender(UnitInfo* unit) {
    // -1: an enemy is near, Jev decides; 0: nothing to do now; 1: the controller moved the unit toward its post
    const Point position(unit->grid_x, unit->grid_y);
    int32_t nearest_enemy = -1;
    const int32_t danger = GetDanger(unit, position, &nearest_enemy);

    if (danger > 0 ||
        (nearest_enemy >= 0 && nearest_enemy <= unit->GetBaseValues()->GetAttribute(ATTRIB_SCAN))) {
        return -1;
    }

    Point anchor;
    Point origin;
    bool enemy_known;

    if (!GetThreatAnchor(anchor, origin, enemy_known)) {
        return 0;
    }

    return ScriptToPost(unit, anchor, "the Defend group");
}

int32_t JevAi::ScriptToPost(UnitInfo* unit, Point anchor, const char* role) {
    // 0: at its post or no post; 1: the controller moved the unit toward its post
    const Point position(unit->grid_x, unit->grid_y);

    ComputeReach(unit);

    auto item = std::find_if(m_posts.begin(), m_posts.end(),
                             [unit](const auto& entry) { return entry.first == unit->GetId(); });
    Point post;

    // a post stays while the axis stays and the unit can still get there
    if (item != m_posts.end() && JevAi_GetDistance(item->second, anchor) <= 5 &&
        (item->second == position || GetReachCost(item->second) >= 0)) {
        post = item->second;

    } else if (FindPost(unit, anchor, post)) {
        if (item != m_posts.end()) {
            item->second = post;

        } else {
            m_posts.push_back({unit->GetId(), post});
        }

    } else {
        return 0;
    }

    if (JevAi_GetDistance(position, post) <= 1) {
        if (unit->GetOrder() == ORDER_AWAIT) {
            UnitsManager_SetNewOrder(unit, ORDER_SENTRY, ORDER_STATE_EXECUTING_ORDER);
        }

        return 0;
    }

    if (unit->speed == 0) {
        return 0;
    }

    SDL_Log("Jev %s: %s of %s goes to its post at %s.", GetTeamName(), JevAi_GetUnitName(unit).c_str(), role,
            JevAi_FormatPoint(post).c_str());

    m_unit = unit;
    m_scripted = true;
    m_last_action = Action::MOVE;
    m_move_start = position;
    unit->move_to_grid_x = post.x;
    unit->move_to_grid_y = post.y;

    UnitsManager_SetNewOrder(unit, ORDER_MOVE, ORDER_STATE_INIT);

    ++m_turn_moves;
    m_wait_start = SDL_GetTicks();
    m_state = State::WAIT_FOR_UNIT;

    return 1;
}

int32_t JevAi::ScriptScanner(UnitInfo* unit) {
    // Scanners watch the base from its edge on sentry: the first toward the way enemies come, more on the other sides
    // (the user: more than one is only useful spread around the base; Radar does this once it can be spared)
    const Point position(unit->grid_x, unit->grid_y);
    int32_t nearest_enemy = -1;

    if (GetDanger(unit, position, &nearest_enemy) > 0) {
        return -1;
    }

    Point center;
    int32_t extent;
    Point origin;

    if (!GetBaseCenter(center, extent)) {
        return 0;
    }

    GetEnemyDirection(origin);

    int32_t index = 0;

    for (auto it = UnitsManager_MobileLandSeaUnits.Begin(), it_end = UnitsManager_MobileLandSeaUnits.End();
         it != it_end; ++it) {
        index += (*it).team == m_team && (*it).hits > 0 && (*it).GetUnitType() == SCANNER &&
                 (*it).GetId() < unit->GetId();
    }

    // the direction to the enemy, turned by a quarter for each further Scanner
    Point toward(origin.x - center.x, origin.y - center.y);

    for (int32_t turn = 0; turn < index % 4; ++turn) {
        toward = Point(-toward.y, toward.x);
    }

    const int32_t length = std::max(1, std::max(std::abs(toward.x), std::abs(toward.y)));
    const int32_t out = extent + 1;
    const Point anchor(center.x + toward.x * out / length, center.y + toward.y * out / length);

    return ScriptToPost(unit, anchor, "the base watch");
}

int32_t JevAi::ScriptScout(UnitInfo* unit) {
    // Scouts search for the enemy base, then watch the way in, as Jev chose them before; Jev is asked on contact
    const Point position(unit->grid_x, unit->grid_y);
    int32_t nearest_enemy = -1;

    if (GetDanger(unit, position, &nearest_enemy) > 0 ||
        (nearest_enemy >= 0 && nearest_enemy <= unit->GetBaseValues()->GetAttribute(ATTRIB_SCAN))) {
        return -1;
    }

    ComputeReach(unit);
    m_options.clear();
    AddScoutOptions(unit);

    if (m_options.empty()) {
        return 0;
    }

    const Option option = m_options.front();
    const bool picket = option.data.value("action", "") == "picket";

    m_options.clear();

    if (JevAi_GetDistance(position, option.destination) <= 1 || GetReachCost(option.destination) < 0 ||
        unit->speed == 0) {
        return 0;
    }

    if (picket) {
        const uint32_t id = unit->GetId();

        m_pickets.erase(std::remove_if(m_pickets.begin(), m_pickets.end(),
                                       [id](const auto& item) { return item.first == id; }),
                        m_pickets.end());
        m_pickets.push_back({id, option.destination});
    }

    SDL_Log("Jev %s: Scout at %s %s %s.", GetTeamName(), JevAi_FormatPoint(position).c_str(),
            picket ? "goes to its picket post at" : "searches for the enemy base toward",
            JevAi_FormatPoint(option.destination).c_str());

    m_unit = unit;
    m_scripted = true;
    m_last_action = Action::MOVE;
    m_move_start = position;
    unit->move_to_grid_x = option.destination.x;
    unit->move_to_grid_y = option.destination.y;

    UnitsManager_SetNewOrder(unit, ORDER_MOVE, ORDER_STATE_INIT);

    ++m_turn_moves;
    m_wait_start = SDL_GetTicks();
    m_state = State::WAIT_FOR_UNIT;

    return 1;
}

bool JevAi::IsInterceptor(UnitInfo* unit) const {
    return std::find(m_interceptors.begin(), m_interceptors.end(), unit->GetId()) != m_interceptors.end();
}

void JevAi::AssignInterceptors() {
    // lost Scouts leave; once enemy units are seen, Scouts beyond the Scout group join, the nearest to the base first
    std::vector<UnitInfo*> scouts;

    for (auto it = UnitsManager_MobileLandSeaUnits.Begin(), it_end = UnitsManager_MobileLandSeaUnits.End();
         it != it_end; ++it) {
        if ((*it).team == m_team && (*it).hits > 0 && (*it).GetUnitType() == SCOUT) {
            scouts.push_back(it->Get());
        }
    }

    m_interceptors.erase(std::remove_if(m_interceptors.begin(), m_interceptors.end(),
                                        [&scouts](uint32_t id) {
                                            return std::none_of(scouts.begin(), scouts.end(), [id](UnitInfo* scout) {
                                                return scout->GetId() == id;
                                            });
                                        }),
                         m_interceptors.end());

    Point center;
    int32_t extent;

    if (!m_enemy_seen || !GetBaseCenter(center, extent)) {
        return;
    }

    std::sort(scouts.begin(), scouts.end(), [center](UnitInfo* a, UnitInfo* b) {
        return JevAi_GetDistance(center, Point(a->grid_x, a->grid_y)) <
               JevAi_GetDistance(center, Point(b->grid_x, b->grid_y));
    });

    for (UnitInfo* scout : scouts) {
        const int32_t interceptors = static_cast<int32_t>(m_interceptors.size());

        if (interceptors >= JevAi_Interceptors ||
            static_cast<int32_t>(scouts.size()) - interceptors <= m_scout_size) {
            break;
        }

        if (IsInterceptor(scout)) {
            continue;
        }

        const uint32_t id = scout->GetId();

        m_interceptors.push_back(id);
        m_pickets.erase(std::remove_if(m_pickets.begin(), m_pickets.end(),
                                       [id](const auto& item) { return item.first == id; }),
                        m_pickets.end());

        SDL_Log("Jev %s: %s joins the Defend group as an interceptor.", GetTeamName(), JevAi_GetUnitName(scout).c_str());
    }
}

bool JevAi::FindRaider(UnitInfo* unit, Point& at) const {
    // the nearest enemy unit seen this turn or the last one near the base or near a builder
    Point center;
    int32_t extent;

    if (!GetBaseCenter(center, extent)) {
        return false;
    }

    const Point position(unit->grid_x, unit->grid_y);
    int32_t best = -1;

    for (const Sighting& sighting : m_sightings) {
        if (sighting.building || GameManager_TurnCounter - sighting.turn > 1) {
            continue;
        }

        const int32_t to_builder = GetDistanceToOwnBuilder(sighting.at);

        if (JevAi_GetDistance(center, sighting.at) > extent + 12 && (to_builder < 0 || to_builder > 6)) {
            continue;
        }

        const int32_t distance = JevAi_GetDistance(position, sighting.at);

        if (best < 0 || distance < best) {
            best = distance;
            at = sighting.at;
        }
    }

    return best >= 0;
}

int32_t JevAi::ScriptInterceptor(UnitInfo* unit) {
    // -1: an enemy is in sight, Jev decides (attack, charge, hold); 0: at its post; 1: the controller moved it
    const Point position(unit->grid_x, unit->grid_y);
    int32_t nearest_enemy = -1;

    if (GetDanger(unit, position, &nearest_enemy) > 0 ||
        (nearest_enemy >= 0 && nearest_enemy <= unit->GetBaseValues()->GetAttribute(ATTRIB_SCAN))) {
        return -1;
    }

    Point raider;

    if (FindRaider(unit, raider) && unit->speed > 0) {
        Point cell;

        ComputeReach(unit);

        if (FindPost(unit, raider, cell) && JevAi_GetDistance(position, cell) > 1) {
            SDL_Log("Jev %s: %s of the Defend group chases the enemy seen at %s.", GetTeamName(),
                    JevAi_GetUnitName(unit).c_str(), JevAi_FormatPoint(raider).c_str());

            m_unit = unit;
            m_scripted = true;
            m_last_action = Action::MOVE;
            m_move_start = position;
            unit->move_to_grid_x = cell.x;
            unit->move_to_grid_y = cell.y;

            UnitsManager_SetNewOrder(unit, ORDER_MOVE, ORDER_STATE_INIT);

            ++m_turn_moves;
            m_wait_start = SDL_GetTicks();
            m_state = State::WAIT_FOR_UNIT;

            return 1;
        }
    }

    Point center;
    int32_t extent;

    if (!GetBaseCenter(center, extent)) {
        return 0;
    }

    // between raids the interceptors wait in the base, where they reach every side
    return ScriptToPost(unit, center, "the Defend group's interceptors");
}

int32_t JevAi::CountAntiAirTurrets() const {
    // standing, being built and walked to
    int32_t count = 0;

    for (auto it = UnitsManager_StationaryUnits.Begin(), it_end = UnitsManager_StationaryUnits.End(); it != it_end;
         ++it) {
        count += (*it).team == m_team && (*it).hits > 0 && (*it).GetUnitType() == ANTIAIR;
    }

    for (auto it = UnitsManager_MobileLandSeaUnits.Begin(), it_end = UnitsManager_MobileLandSeaUnits.End();
         it != it_end; ++it) {
        // a builder leaving a finished building still has the build order, with an empty build list (m-aa2 crash)
        count += (*it).team == m_team && (*it).hits > 0 && JevAi_IsBuilder((*it).GetUnitType()) &&
                 (*it).GetOrder() == ORDER_BUILD && (*it).GetBuildList().GetCount() > 0 &&
                 (*it).GetConstructedUnitType() == ANTIAIR;
    }

    for (const Site& site : m_sites) {
        count += site.unit_type == ANTIAIR;
    }

    return count;
}

void JevAi::ApplyScripted(const Option& option, const char* reason) {
    // the controller's own choice among the options, applied as an answer would be; no question is sent
    SDL_Log("Jev %s: %s, no question: %s.", GetTeamName(), reason, option.facts.c_str());

    Apply(json{{"id", m_request_id}, {"choice", option.key}, {"model", "script"}, {"confidence", 1.0}}.dump());
}

void JevAi::UnitSpotted(UnitInfo* unit) {
    // aircraft strike during the enemy's turn and are often gone by Jev's own: any sighting counts
    if (unit->team != m_team && unit->team != PLAYER_TEAM_ALIEN && (unit->flags & MOBILE_AIR_UNIT) &&
        !m_enemy_air_seen) {
        m_enemy_air_seen = true;

        SDL_Log("Jev %s: enemy aircraft seen, %s at %s: air defense comes first now.", GetTeamName(),
                JevAi_GetUnitName(unit).c_str(), JevAi_FormatPoint(Point(unit->grid_x, unit->grid_y)).c_str());
    }
}

JevAi::Fight JevAi::EstimateFight(UnitInfo* unit, UnitInfo* target, Point end, int32_t turns_to_range,
                                  double unseen) const {
    // a short simulation of the local fight after a charge to `end`: the unit, the team's armed units and defenses
    // that can fire at the target this turn or next, against the target and the known enemies that can fire at
    // `end` (its cover). Every unit fires its rounds each turn, one shot at a time, attack minus armor per shot (at
    // least 1), at the side's focus (the target, or the charging unit) while it stands, else at the weakest enemy it
    // can hit. While the unit closes in, the cover fires first. Ground not in sight makes the enemy's fire count more.
    struct Fighter {
        UnitInfo* unit;
        Point at;
        int32_t hits;
        int32_t attack;
        int32_t armor;
        int32_t rounds;
        int32_t value;
    };

    auto make = [](UnitInfo* item, Point at) {
        UnitValues* values = item->GetBaseValues();

        return Fighter{item,
                       at,
                       static_cast<int32_t>(item->hits),
                       values->GetAttribute(ATTRIB_ATTACK),
                       values->GetAttribute(ATTRIB_ARMOR),
                       std::max(1, values->GetAttribute(ATTRIB_ROUNDS)),
                       static_cast<int32_t>(item->GetNormalRateBuildCost())};
    };

    std::vector<Fighter> ours{make(unit, end)};
    std::vector<Fighter> theirs{make(target, Point(target->grid_x, target->grid_y))};
    const Point target_at(target->grid_x, target->grid_y);

    for (auto* list : {&UnitsManager_MobileLandSeaUnits, &UnitsManager_StationaryUnits}) {
        for (auto it = list->Begin(), it_end = list->End(); it != it_end; ++it) {
            UnitInfo& other = *it;
            UnitValues* values = other.GetBaseValues();
            const Point at(other.grid_x, other.grid_y);
            const int32_t speed = (other.flags & STATIONARY) ? 0 : values->GetAttribute(ATTRIB_SPEED);

            if (&other == unit || &other == target || other.hits == 0 || values->GetAttribute(ATTRIB_ATTACK) == 0 ||
                other.ammo == 0) {
                continue;
            }

            if (other.team == m_team) {
                if (Access_IsValidAttackTargetType(other.GetUnitType(), target->GetUnitType()) &&
                    JevAi_GetDistance(at, target_at) <= values->GetAttribute(ATTRIB_RANGE) + speed) {
                    ours.push_back(make(&other, at));
                }

            } else if (other.team != PLAYER_TEAM_ALIEN && other.IsVisibleToTeam(m_team) &&
                       Access_IsValidAttackTargetType(other.GetUnitType(), unit->GetUnitType()) &&
                       JevAi_GetDistance(at, end) <= values->GetAttribute(ATTRIB_RANGE) + speed) {
                theirs.push_back(make(&other, at));
            }
        }
    }

    // one side's volley: each fighter fires its rounds at the focus, else at the weakest it can hit
    auto volley = [](std::vector<Fighter>& shooters, std::vector<Fighter>& targets, double scale) {
        for (const Fighter& shooter : shooters) {
            if (shooter.hits <= 0) {
                continue;
            }

            for (int32_t shot = 0; shot < shooter.rounds; ++shot) {
                Fighter* aim = nullptr;

                for (Fighter& candidate : targets) {
                    if (candidate.hits <= 0 ||
                        !Access_IsValidAttackTargetType(shooter.unit->GetUnitType(), candidate.unit->GetUnitType())) {
                        continue;
                    }

                    if (&candidate == &targets.front()) {
                        aim = &candidate;
                        break;
                    }

                    if (!aim || candidate.hits < aim->hits) {
                        aim = &candidate;
                    }
                }

                if (!aim) {
                    break;
                }

                aim->hits -= static_cast<int32_t>(std::max(1, shooter.attack - aim->armor) * scale + 0.5);
            }
        }
    };

    auto standing = [](const std::vector<Fighter>& side) {
        return std::any_of(side.begin(), side.end(), [](const Fighter& item) { return item.hits > 0; });
    };

    const double enemy_scale = 1.0 + unseen;
    int32_t turn = 0;

    for (int32_t step = 0; step < turns_to_range && standing(ours); ++step) {
        // the cover fires at the unit closing in
        volley(theirs, ours, enemy_scale);
    }

    for (turn = 0; turn < 3 && standing(ours) && theirs.front().hits > 0; ++turn) {
        volley(ours, theirs, 1.0);

        if (standing(theirs)) {
            volley(theirs, ours, enemy_scale);
        }
    }

    Fight fight{};

    fight.turns = turn + turns_to_range;
    fight.allies = static_cast<int32_t>(ours.size()) - 1;
    fight.enemies = static_cast<int32_t>(theirs.size()) - 1;
    fight.target_destroyed = theirs.front().hits <= 0;
    fight.unit_destroyed = ours.front().hits <= 0;

    for (const Fighter& item : theirs) {
        fight.gain += item.hits <= 0 ? item.value : 0;
    }

    for (const Fighter& item : ours) {
        fight.loss += item.hits <= 0 ? item.value : 0;
    }

    return fight;
}

void JevAi::AddChargeOptions(UnitInfo* unit) {
    // close in on an enemy the unit cannot fire at this turn. A charge is a desperation move (the user, 2026-11-13:
    // only when out of choices, with a long range enemy near the base; without vision the unit can be wiped out) or
    // one the task group wins (the user: a Tank against an uncovered Scout, absolutely; against cover only if it
    // looks like a win). Both into ground the team sees; a fight estimate (EstimateFight) says who wins.
    UnitValues* values = unit->GetBaseValues();
    const Point position(unit->grid_x, unit->grid_y);
    const int32_t range = values->GetAttribute(ATTRIB_RANGE);
    // distance, target, last resort
    std::vector<std::tuple<int32_t, UnitInfo*, bool>> targets;
    Point center;
    int32_t extent;

    if (values->GetAttribute(ATTRIB_ATTACK) == 0 || unit->ammo == 0 || unit->speed == 0 ||
        !GetBaseCenter(center, extent)) {
        return;
    }

    for (auto* list : {&UnitsManager_MobileLandSeaUnits, &UnitsManager_StationaryUnits}) {
        for (auto it = list->Begin(), it_end = list->End(); it != it_end; ++it) {
            UnitInfo& target = *it;
            UnitValues* target_values = target.GetBaseValues();

            if (target.team == m_team || target.team == PLAYER_TEAM_ALIEN || target.hits == 0 ||
                !target.IsVisibleToTeam(m_team) || target.GetUnitType() == CNCT_4W ||
                target.GetUnitType() == ROAD || ((target.flags & STATIONARY) && target_values->GetAttribute(ATTRIB_ATTACK) == 0) ||
                !Access_IsValidAttackTarget(unit, &target)) {
                continue;
            }

            // already attackable this turn: the attack option covers it
            if (std::any_of(m_options.begin(), m_options.end(),
                            [&target](const Option& option) { return option.target == &target; })) {
                continue;
            }

            const Point at(target.grid_x, target.grid_y);
            const int32_t target_range = target_values->GetAttribute(ATTRIB_RANGE);
            const int32_t distance = JevAi_GetDistance(position, at);

            if (distance > range + unit->speed * 3) {
                continue;
            }

            // a last resort: it outranges the unit and shells the base or is about to, and no other unit or defense
            // of the team outranges or matches it within a turn's reach
            bool last_resort = target_values->GetAttribute(ATTRIB_ATTACK) > 0 && target_range > range &&
                               Access_IsValidAttackTargetType(target.GetUnitType(), unit->GetUnitType()) &&
                               JevAi_GetDistance(center, at) <= extent + target_range + 2;

            for (auto* own_list : {&UnitsManager_MobileLandSeaUnits, &UnitsManager_StationaryUnits}) {
                for (auto own = own_list->Begin(), own_end = own_list->End(); own != own_end && last_resort; ++own) {
                    UnitValues* own_values = (*own).GetBaseValues();

                    last_resort =
                        !((*own).team == m_team && own->Get() != unit && (*own).hits > 0 && (*own).ammo > 0 &&
                          own_values->GetAttribute(ATTRIB_ATTACK) > 0 &&
                          own_values->GetAttribute(ATTRIB_RANGE) >= target_range &&
                          Access_IsValidAttackTargetType((*own).GetUnitType(), target.GetUnitType()) &&
                          JevAi_GetDistance(Point((*own).grid_x, (*own).grid_y), at) <=
                              own_values->GetAttribute(ATTRIB_RANGE) + own_values->GetAttribute(ATTRIB_SPEED));
                }
            }

            targets.push_back({distance, &target, last_resort});
        }
    }

    std::sort(targets.begin(), targets.end(),
              [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });

    int32_t offered = 0;

    for (size_t i = 0; i < targets.size() && offered < 2; ++i) {
        UnitInfo* target = std::get<1>(targets[i]);
        const bool last_resort = std::get<2>(targets[i]);
        const Point at(target->grid_x, target->grid_y);
        Point best;
        int32_t best_distance = INT32_MAX;

        for (int32_t x = position.x - unit->speed; x <= position.x + unit->speed; ++x) {
            for (int32_t y = position.y - unit->speed; y <= position.y + unit->speed; ++y) {
                const Point cell(x, y);
                const int32_t cost = GetReachCost(cell);

                if (cost <= 0 || cost > unit->speed || IsFactoryExit(cell) ||
                    !Access_IsAccessible(unit->GetUnitType(), m_team, x, y, AccessModifier_SameClassBlocks)) {
                    continue;
                }

                const int32_t distance = Access_GetSquaredDistance(cell, at);

                if (distance < best_distance) {
                    best = cell;
                    best_distance = distance;
                }
            }
        }

        if (best_distance == INT32_MAX || JevAi_GetDistance(best, at) >= JevAi_GetDistance(position, at)) {
            continue;
        }

        const int32_t left = std::max(0, JevAi_GetDistance(best, at) - range);
        const int32_t turns =
            left == 0 ? 0 : (left + values->GetAttribute(ATTRIB_SPEED) - 1) / std::max(1, values->GetAttribute(ATTRIB_SPEED));

        if (turns > (last_resort ? 2 : 1)) {
            continue;
        }

        // the ground around the end of the charge, as far as the target fires and at least 4 cells, must be mostly
        // in sight: enemies unseen there could wipe the unit out
        const int32_t radius = std::max(4, target->GetBaseValues()->GetAttribute(ATTRIB_RANGE));
        const HeatMap* vision = UnitsManager_TeamInfo[m_team].heat_map.get();
        int32_t cells = 0;
        int32_t seen = 0;

        for (int32_t x = best.x - radius; x <= best.x + radius && vision; ++x) {
            for (int32_t y = best.y - radius; y <= best.y + radius; ++y) {
                if (JevAi_IsInsideMap(Point(x, y)) && JevAi_GetDistance(best, Point(x, y)) <= radius) {
                    ++cells;
                    seen += vision->GetComplete(x, y) > 0;
                }
            }
        }

        if (!vision || cells == 0 || seen * 5 < cells * 4) {
            continue;
        }

        const double unseen = 1.0 - static_cast<double>(seen) / cells;
        const Fight fight = EstimateFight(unit, target, best, turns, unseen);
        const bool wins = fight.target_destroyed && !fight.unit_destroyed && fight.gain > fight.loss;

        if (!wins && !last_resort) {
            continue;
        }

        const int32_t closer = JevAi_GetDistance(position, at) - JevAi_GetDistance(best, at);
        const std::string target_name = JevAi_GetUnitName(target);
        // the mock's score: a win without losses and without cover first
        const int32_t score = wins ? 40 + std::min(40, (fight.gain - fight.loss) / 2) - fight.enemies * 5 : 0;
        std::string facts = "charge the enemy " + target_name + " at " + JevAi_FormatPoint(at) + ", it has " +
                            std::to_string(target->hits) + " hits: move " + std::to_string(closer) +
                            " cells toward it to " + JevAi_FormatPoint(best) + ", " +
                            (turns == 0 ? std::string("in range to fire")
                                        : "in range in " + std::to_string(turns) + (turns == 1 ? " more turn" : " more turns")) +
                            "; fight estimate over " + std::to_string(fight.turns) + (fight.turns == 1 ? " turn with " : " turns with ") +
                            std::to_string(fight.allies) + " more of your units against it and " +
                            std::to_string(fight.enemies) + " enemies covering it: " +
                            (fight.target_destroyed ? "it is destroyed" : "it survives") + ", you destroy " +
                            std::to_string(fight.gain) + " and lose " + std::to_string(fight.loss) +
                            " in materials" + (fight.unit_destroyed ? ", this unit is destroyed" : "");

        if (last_resort && !wins) {
            facts += "; a last resort: it outranges you near your base and nothing else of yours can answer it";
        }

        m_options.push_back({"c" + std::to_string(m_options.size()),
                             Action::MOVE,
                             best,
                             INVALID_ID,
                             nullptr,
                             facts,
                             score,
                             {{"action", "charge"},
                              {"target", target_name},
                              {"at", JevAi_PointData(at)},
                              {"hits", target->hits},
                              {"closer", closer},
                              {"to", JevAi_PointData(best)},
                              {"turns_to_range", turns},
                              {"fight", {{"turns", fight.turns},
                                         {"allies", fight.allies},
                                         {"cover", fight.enemies},
                                         {"target_destroyed", fight.target_destroyed},
                                         {"gain", fight.gain},
                                         {"loss", fight.loss},
                                         {"unit_destroyed", fight.unit_destroyed}}},
                              {"reason", wins ? "winnable" : "last_resort"}}});
        ++offered;
    }
}

void JevAi::AddHoldOption(UnitInfo* unit) {
    // a Defend group member in contact can go back to its post instead of chasing
    auto item = std::find_if(m_posts.begin(), m_posts.end(),
                             [unit](const auto& entry) { return entry.first == unit->GetId(); });

    if (item == m_posts.end() || JevAi_GetDistance(Point(unit->grid_x, unit->grid_y), item->second) <= 1 ||
        GetReachCost(item->second) < 0) {
        return;
    }

    const int32_t distance = JevAi_GetDistance(Point(unit->grid_x, unit->grid_y), item->second);

    m_options.push_back({"h" + std::to_string(m_options.size()),
                         Action::MOVE,
                         item->second,
                         INVALID_ID,
                         nullptr,
                         "hold: go back to your post in the Defend group at " + JevAi_FormatPoint(item->second) + ", " +
                             std::to_string(distance) + " cells away, and wait there on sentry; it fires at enemies "
                             "that come in range",
                         5,
                         {{"action", "hold"}, {"to", JevAi_PointData(item->second)}, {"distance", distance}}});
}

void JevAi::CheckCommandTriggers() {
    // the commander is asked when the situation changes, not every turn
    const int32_t turn = GameManager_TurnCounter;
    const int32_t defenders = CountDefenders();
    Cargo stock;
    Cargo capacity;

    if (Complex* complex = GetEconomyComplex()) {
        complex->GetCargoInfo(stock, capacity);
    }

    std::string trigger;

    if (m_last_command_turn < 0) {
        trigger = turn >= 2 ? "start of the game" : "";

    } else if (m_enemy_seen && !m_command_enemy_seen) {
        trigger = "first enemy units seen";

    } else if (m_enemy_base_seen && !m_command_enemy_base_seen) {
        trigger = "enemy base found";

    } else if (m_enemy_air_seen && !m_command_enemy_air_seen) {
        trigger = "enemy aircraft seen";

    } else if (m_defenders_at_command >= 2 && defenders * 2 <= m_defenders_at_command) {
        trigger = "the Defend group lost half of its units";

    } else if (defenders >= m_defend_size && capacity.raw > 0 && stock.raw * 5 >= capacity.raw * 4 &&
               turn - m_last_command_turn >= 3) {
        trigger = "the Defend group is full and raw materials pile up in the base";
    }

    if (!trigger.empty()) {
        m_command_trigger = trigger;
    }
}

nlohmann::ordered_json JevAi::GetCommandState() const {
    json state;
    Cargo stock;
    Cargo capacity;
    Cargo mining;
    Cargo mining_capacity;
    int32_t anti_air = 0;
    const int32_t defenders = CountDefenders(&anti_air);
    Point anchor;
    Point origin;
    bool enemy_known = false;
    uint32_t best_enemy_points = 0;

    if (Complex* complex = GetEconomyComplex()) {
        complex->GetCargoInfo(stock, capacity);
        complex->GetCargoMining(mining, mining_capacity);
    }

    for (int32_t team = PLAYER_TEAM_RED; team < PLAYER_TEAM_MAX - 1; ++team) {
        if (team != m_team && UnitsManager_TeamInfo[team].team_type != TEAM_TYPE_NONE) {
            best_enemy_points = std::max(best_enemy_points, UnitsManager_TeamInfo[team].team_points);
        }
    }

    GetThreatAnchor(anchor, origin, enemy_known);

    int32_t enemy_buildings = 0;
    int32_t recent_near_base = 0;
    Point center;
    int32_t extent = 0;
    const bool has_center = GetBaseCenter(center, extent);

    for (const auto& sighting : m_sightings) {
        enemy_buildings += sighting.building;
        recent_near_base += !sighting.building && GameManager_TurnCounter - sighting.turn <= 5 && has_center &&
                            JevAi_GetDistance(center, sighting.at) <= extent + 15;
    }

    state["game"] = "M.A.X., turn-based strategy";
    state["turn"] = GameManager_TurnCounter;
    state["why_asked"] = m_command_trigger;

    if (m_last_command_turn >= 0) {
        state["current_plan"] = {{"defend_group_size", m_defend_size},
                                 {"scout_group_size", m_scout_size},
                                 {"set_on_turn", m_last_command_turn},
                                 {"set_because", m_last_command_reason}};
    }
    state["score"] = {{"your_points", UnitsManager_TeamInfo[m_team].team_points},
                      {"best_enemy_points", best_enemy_points}};

    if (JevAi_GetTurnsLeft() >= 0) {
        state["score"]["turns_left"] = JevAi_GetTurnsLeft();
    }

    state["economy"] = {{"raw", stock.raw},
                        {"raw_capacity", capacity.raw},
                        {"raw_mined_per_turn", mining.raw},
                        {"eco_spheres", CountOwnUnits(GREENHSE)},
                        {"factories", CountOwnUnits(LANDPLT) + CountOwnUnits(LIGHTPLT)}};
    state["groups"] = {
        {"defend",
         {{"size", m_defend_size},
          {"combat_units", defenders - anti_air},
          {"anti_air_units", anti_air},
          {"combat_units_in_production", CountInProduction(true, false, false)},
          {"posts_toward", JevAi_PointData(origin)},
          {"enemy_direction_known", enemy_known}}},
        {"scouts", {{"size", m_scout_size}, {"units", CountOwnUnits(SCOUT)}}}};
    int32_t threat = 0;
    int32_t threat_outranging = 0;

    for (const auto& item : m_threats) {
        if (GameManager_TurnCounter - item.turn <= 5) {
            threat = std::max(threat, item.near_base);
            threat_outranging = std::max(threat_outranging, item.outranging);
        }
    }

    int32_t enemy_base_distance = -1;

    for (const auto& sighting : m_sightings) {
        if (sighting.building && has_center) {
            const int32_t distance = JevAi_GetDistance(center, sighting.at);

            enemy_base_distance = enemy_base_distance < 0 ? distance : std::min(enemy_base_distance, distance);
        }
    }

    int32_t army_outranging = 0;

    for (const auto& item : m_enemy_army) {
        army_outranging += item.second.second;
    }

    state["enemy"] = {{"units_seen", m_enemy_seen},
                      {"combat_units_seen_last_15_turns", static_cast<int32_t>(m_enemy_army.size())},
                      {"of_them_outranging_your_tanks_all", army_outranging},
                      {"combat_units_near_base_last_5_turns", threat},
                      {"of_them_outranging_your_tanks", threat_outranging},
                      {"base_found", m_enemy_base_seen},
                      {"buildings_known", enemy_buildings},
                      {"aircraft_seen", m_enemy_air_seen},
                      {"unit_sightings_near_base_last_5_turns", recent_near_base}};

    if (enemy_base_distance >= 0) {
        state["enemy"]["nearest_enemy_building_distance"] = enemy_base_distance;
    }
    state["units_lost"] = m_units_lost;

    return state;
}

bool JevAi::AskCommand() {
    const int32_t defenders = CountDefenders();

    m_options.clear();

    // once enemy units are seen the Defend group keeps at least 4 (j6 world 9: shrunk to 2 on turn 50 while the enemy
    // grew from 12 to 38 units; the user, 2026-11-13: defend as soon as enemy units are spotted, not once their base is)
    const int32_t minimum = m_enemy_seen ? 4 : 2;

    if (m_defend_size < minimum) {
        m_defend_size = minimum;
    }

    for (const int32_t size : {2, 4, 6, 8, 10, 12}) {
        if (size == m_defend_size || size < minimum || std::abs(size - m_defend_size) > 4) {
            continue;
        }

        m_options.push_back({"d" + std::to_string(m_options.size()),
                             Action::COMMAND,
                             Point(),
                             INVALID_ID,
                             nullptr,
                             "Defend group of " + std::to_string(size) + " combat units instead of " +
                                 std::to_string(m_defend_size) + " (it has " + std::to_string(defenders) +
                                 "): factories produce combat units until it is full",
                             size == 6 ? 20 : 0,
                             {{"action", "set_defend"}, {"size", size}, {"was", m_defend_size}, {"has", defenders}}});
    }

    for (const int32_t size : {0, 1, 2}) {
        if (size == m_scout_size) {
            continue;
        }

        m_options.push_back({"s" + std::to_string(m_options.size()),
                             Action::COMMAND,
                             Point(),
                             INVALID_ID,
                             nullptr,
                             "Scout group of " + std::to_string(size) + " instead of " + std::to_string(m_scout_size) +
                                 ": Scouts search for the enemy base and watch the way in",
                             0,
                             {{"action", "set_scouts"}, {"size", size}, {"was", m_scout_size}}});
    }

    m_options.push_back({"keep",
                         Action::COMMAND,
                         Point(),
                         INVALID_ID,
                         nullptr,
                         "keep the plan: Defend group of " + std::to_string(m_defend_size) + ", Scout group of " +
                             std::to_string(m_scout_size),
                         10,
                         {{"action", "keep_plan"}, {"defend", m_defend_size}, {"scouts", m_scout_size}}});

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
    request["state"] = GetCommandState();
    request["questions"] = {
        {"command", {{"type", "choice"}, {"instructions", JevAi_CommandInstructions}, {"criteria", criteria}}}};
    request["mock_scores"] = scores;
    request["options"] = options;

    m_question = "command";
    m_last_command_turn = GameManager_TurnCounter;
    m_last_command_reason = m_command_trigger;
    m_command_enemy_seen = m_enemy_seen;
    m_command_enemy_base_seen = m_enemy_base_seen;
    m_command_enemy_air_seen = m_enemy_air_seen;
    m_defenders_at_command = defenders;
    m_command_trigger.clear();
    ++m_turn_questions;

    return m_bridge.Send(request.dump());
}

void JevAi::ApplyCommand(const Option& option) {
    const std::string action = option.data.value("action", "");

    if (action == "set_defend") {
        m_defend_size = option.data.value("size", m_defend_size);

    } else if (action == "set_scouts") {
        m_scout_size = option.data.value("size", m_scout_size);
    }

    SDL_Log("Jev %s: commander plan: Defend group %d, Scout group %d.", GetTeamName(), m_defend_size, m_scout_size);
}

void JevAi::AddSurveyOptions(UnitInfo* surveyor) {
    // new mines can only be joined to the base close to it: survey that ground, nearest first, as a human player
    // sends a Surveyor to survey an area
    constexpr int32_t mining_range = 14;
    const uint16_t surveyed_bit = UnitsManager_TeamInfo[m_team].team_units->hash_team_id;
    const auto is_surveyed = [surveyed_bit](int32_t x, int32_t y) {
        return (ResourceManager_CargoMap[ResourceManager_MapSize.x * y + x] & surveyed_bit) != 0;
    };
    const Point position(surveyor->grid_x, surveyor->grid_y);
    Point base;
    Point best;
    int32_t best_distance = INT32_MAX;
    int32_t remaining = 0;

    if (surveyor->speed == 0 || !GetBasePosition(base)) {
        return;
    }

    for (int32_t x = base.x - mining_range; x <= base.x + mining_range; ++x) {
        for (int32_t y = base.y - mining_range; y <= base.y + mining_range; ++y) {
            if (!JevAi_IsInsideMap(Point(x, y)) || is_surveyed(x, y) ||
                Access_GetSurfaceType(x, y) != SURFACE_TYPE_LAND) {
                continue;
            }

            ++remaining;

            const int32_t distance = JevAi_GetDistance(position, Point(x, y));

            if (distance < best_distance &&
                Access_IsAccessible(SURVEYOR, m_team, x, y, AccessModifier_SameClassBlocks) &&
                GetReachCost(Point(x, y)) >= 0) {
                best = Point(x, y);
                best_distance = distance;
            }
        }
    }

    if (best_distance == INT32_MAX) {
        return;
    }

    const int32_t danger = GetDanger(surveyor, best, nullptr);

    if (danger > 0) {
        return;
    }

    std::string facts = "survey the ground around your base: go to " + JevAi_FormatPoint(best) + ", " +
                        std::to_string(best_distance) + " cells away, " + std::to_string(remaining) +
                        " land cells within " + std::to_string(mining_range) +
                        " cells of your base are not surveyed yet; only deposits that close can be mined and joined "
                        "to the base";

    m_options.push_back({"s" + std::to_string(m_options.size()),
                         Action::MOVE,
                         best,
                         INVALID_ID,
                         nullptr,
                         facts,
                         remaining > 0 ? 30 : 0,
                         {{"action", "survey_base"},
                          {"to", JevAi_PointData(best)},
                          {"distance", best_distance},
                          {"unsurveyed_near_base", remaining},
                          {"mining_range", mining_range}}});
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
                    Access_IsAccessible(builder->GetUnitType(), m_team, x, y, AccessModifier_SameClassBlocks) &&
                    GetReachCost(cell) >= 0) {
                    const int32_t distance = GetReachCost(cell);

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
                                 ", " + std::to_string(static_cast<int32_t>(SDL_sqrt(Access_GetSquaredDistance(nearest_cell, Point(builder->grid_x, builder->grid_y))))) +
                                 " cells away, to take raw materials there; you carry " +
                                 std::to_string(builder->storage) + " of " + std::to_string(capacity) +
                                 ", the base stores " + std::to_string(nearest_materials.raw),
                             builder->storage < capacity / 2 ? 35 : 0,
                             {{"action", "go_refill"},
                              {"to_building", JevAi_GetUnitName(nearest)},
                              {"to", JevAi_PointData(nearest_cell)},
                              {"distance", static_cast<int32_t>(SDL_sqrt(Access_GetSquaredDistance(nearest_cell, Point(builder->grid_x, builder->grid_y))))},
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

            // out of range: only when a firing cell is reachable this turn with a shot left, the cheapest one; moving
            // costs shots to units that cannot move and fire (Paths_CalculateStep)
            const int32_t range = values->GetAttribute(ATTRIB_RANGE);
            int32_t moves_to_fire = 0;

            if (distance > range * range) {
                moves_to_fire = -1;

                for (int32_t x = target.grid_x - range; x <= target.grid_x + range; ++x) {
                    for (int32_t y = target.grid_y - range; y <= target.grid_y + range; ++y) {
                        const int32_t cost = GetReachCost(Point(x, y));
                        const int32_t dx = x - target.grid_x;
                        const int32_t dy = y - target.grid_y;

                        if (cost < 0 || cost > unit->speed || dx * dx + dy * dy > range * range ||
                            (moves_to_fire >= 0 && cost >= moves_to_fire)) {
                            continue;
                        }

                        if (!values->GetAttribute(ATTRIB_MOVE_AND_FIRE) &&
                            std::min<int32_t>(unit->shots, values->GetAttribute(ATTRIB_ROUNDS) * (unit->speed - cost) /
                                                               std::max(1, values->GetAttribute(ATTRIB_SPEED))) < 1) {
                            continue;
                        }

                        moves_to_fire = cost;
                    }
                }

                if (moves_to_fire < 0) {
                    continue;
                }
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

            facts += in_range ? ", in range now" : ", you move " + std::to_string(moves_to_fire) + " cells closer first";
            facts += shoots_back ? ", it can shoot back with attack " +
                                       std::to_string(target_values->GetAttribute(ATTRIB_ATTACK))
                                 : ", it cannot shoot back";

            if (target.hits <= damage * unit->shots) {
                facts += ", it can be destroyed this turn";
            }

            // counter-battery: long range units fire at the enemy's long range units first, which outrange the front
            // line and shell it from where it cannot answer (j7-groups4: 96 of 226 enemy hits came from such units)
            const bool counter_battery = JevAi_IsLongRange(m_team, unit->GetUnitType()) && shoots_back &&
                                         target_values->GetAttribute(ATTRIB_RANGE) >
                                             UnitsManager_GetCurrentUnitValues(&UnitsManager_TeamInfo[m_team], TANK)
                                                 ->GetAttribute(ATTRIB_RANGE);

            if (counter_battery) {
                facts += ", counter-battery: it outranges your front line, your long range units silence it first";
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

            if (counter_battery) {
                data["counter_battery"] = true;
            }

            if (!in_range) {
                data["moves_to_fire"] = moves_to_fire;
            }

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

        if (unit->GetBaseValues()->GetAttribute(ATTRIB_ATTACK) > 0) {
            state["you"]["ammo"] = unit->ammo;
            state["you"]["max_ammo"] = unit->GetBaseValues()->GetAttribute(ATTRIB_AMMO);
        }
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
    Complex* local_complex = is_factory ? unit->GetComplex() : GetEconomyComplex();

    for (auto it = UnitsManager_StationaryUnits.Begin(), it_end = UnitsManager_StationaryUnits.End(); it != it_end;
         ++it) {
        if ((*it).team == m_team && (*it).hits > 0 && local_complex && (*it).GetComplex() == local_complex) {
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
                             {"workers_left", (*it).GetComplex()->workers},
                             {"storage_full_turns", m_raw_full_turns},
                             {"raw_lost_so_far", m_raw_lost}};
            break;
        }
    }

    if (is_factory && local_complex) {
        const Cargo balance = ProductionManager_PreviewProduction(m_team, local_complex, unit);
        state["production"] = {
            {"build_rate", 1},
            {"scope", "normal speed only; each production option has its own supply forecast"},
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

void JevAi::StartBuilding(UnitInfo* builder, ResourceID unit_type, Point site, int32_t build_rate) {
    build_rate = build_rate > 0 ? build_rate : 1;
    if (!(builder->flags & STATIONARY)) {
        int32_t turns;
        const int32_t cost = builder->GetTurnsToBuild(unit_type, build_rate, &turns);
        if (cost > builder->storage) {
            SDL_Log("Jev %s: %s cannot start %s: needs %d materials, carries %d.", GetTeamName(),
                    JevAi_GetUnitName(builder).c_str(), JevAi_GetTypeName(unit_type).c_str(), cost, builder->storage);
            return;
        }
    }
    // same sequence as the classic TaskCreateBuilding and TaskCreateUnit
    SmartObjectArray<ResourceID> build_list = builder->GetBuildList();

    build_list.Clear();
    build_list.PushBack(&unit_type);

    // Execute the advertised row, including normal speed when the builder happens to carry a full cargo.
    builder->SetBuildRate(build_rate);

    builder->move_to_grid_x = site.x;
    builder->move_to_grid_y = site.y;
    builder->path = nullptr;

    if (Remote_IsNetworkGame) {
        Remote_SendNetPacket_38(builder);
    }

    builder->BuildOrder();

    ++m_turn_builds;
}

void JevAi::WalkToSite(ResourceID unit_type, Point site, int32_t build_rate) {
    m_pending_build = {"", Action::BUILD, site, unit_type, nullptr, "", 0};
    m_pending_build.build_rate = build_rate;
    m_has_pending_build = true;
    m_move_start = Point(m_unit->grid_x, m_unit->grid_y);
    m_unit->move_to_grid_x = site.x;
    m_unit->move_to_grid_y = site.y;

    UnitsManager_SetNewOrder(&*m_unit, ORDER_MOVE, ORDER_STATE_INIT);

    m_wait_start = SDL_GetTicks();
    m_state = State::WAIT_FOR_UNIT;
}

bool JevAi::ContinueToSite() {
    // sites belong to builders; ids are the engine's unique ids, not the per type numbers in unit names
    if (!JevAi_IsBuilder(m_unit->GetUnitType())) {
        return false;
    }

    auto entry = std::find_if(m_sites.begin(), m_sites.end(),
                              [this](const Site& item) { return item.unit_id == m_unit->GetId(); });

    if (entry == m_sites.end()) {
        return false;
    }

    const Site site = *entry;
    const int32_t size = JevAi_GetSize(site.unit_type);
    const bool on_site = m_unit->grid_x >= site.site.x && m_unit->grid_x < site.site.x + size &&
                         m_unit->grid_y >= site.site.y && m_unit->grid_y < site.site.y + size;

    m_sites.erase(entry);

    if (!JevAi_IsSiteFree(m_team, site.unit_type, site.site, &*m_unit) ||
        GameManager_TurnCounter - site.turn > site.walk_turns) {
        SDL_Log("Jev %s: %s gives up the %s site at %s.", GetTeamName(), JevAi_GetUnitName(&*m_unit).c_str(),
                JevAi_GetTypeName(site.unit_type).c_str(), JevAi_FormatPoint(site.site).c_str());

        return false;
    }

    if (on_site) {
        StartBuilding(&*m_unit, site.unit_type, site.site, site.build_rate);
        FinishUnit();

    } else {
        m_sites.push_back(site);

        WalkToSite(site.unit_type, site.site, site.build_rate);
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

    if (m_question == "command") {
        SDL_Log("Jev %s: commander chose %s (%s), model %s, confidence %.2f.", GetTeamName(), option->key.c_str(),
                option->facts.c_str(), model.c_str(), confidence);

        ApplyCommand(*option);
        m_state = State::SELECT;

        return;
    }

    SDL_Log("Jev %s: %s at %s chose %s (%s), model %s, confidence %.2f.", GetTeamName(),
            JevAi_GetUnitName(&*m_unit).c_str(), JevAi_FormatPoint(Point(m_unit->grid_x, m_unit->grid_y)).c_str(),
            option->key.c_str(), option->facts.c_str(), model.c_str(), confidence);

    m_last_action = option->action;

    if (m_question == "production") {
        if (option->action == Action::STAY) {
            m_idle_factories[m_unit->GetId()] = m_factory_signature;

        } else {
            m_idle_factories.erase(m_unit->GetId());
        }
    }

    if (m_question == "construction") {
        if (option->action == Action::STAY) {
            m_idle_builders[m_unit->GetId()] = m_builder_signature;

        } else {
            m_idle_builders.erase(m_unit->GetId());
        }
    }

    switch (option->action) {
        case Action::COMMAND:
        case Action::STAY: {
            FinishUnit();
        } break;

        case Action::PRODUCE: {
            BalanceEconomy();
            StartBuilding(&*m_unit, option->unit_type, Point(0, 0), option->build_rate);

            m_wait_start = SDL_GetTicks();
            m_state = State::WAIT_FOR_UNIT;
        } break;

        case Action::BUILD: {
            const int32_t size = JevAi_GetSize(option->unit_type);

            // a mine away from the base: remember the connectors it needs
            if (option->unit_type == MININGST && !TouchesBase(MININGST, option->destination)) {
                Link link{option->destination, {}};

                if (FindConnectorPath(option->destination, 2, link.connectors)) {
                    m_links.push_back(link);

                    SDL_Log("Jev %s: the Mining Station at %s needs %d connectors.", GetTeamName(),
                            JevAi_FormatPoint(option->destination).c_str(),
                            static_cast<int32_t>(link.connectors.size()));
                }
            }

            if (m_unit->grid_x >= option->destination.x && m_unit->grid_x < option->destination.x + size &&
                m_unit->grid_y >= option->destination.y && m_unit->grid_y < option->destination.y + size) {
                StartBuilding(&*m_unit, option->unit_type, option->destination, option->build_rate);
                FinishUnit();

            } else {
                const int32_t walk_turns =
                    JevAi_GetDistance(Point(m_unit->grid_x, m_unit->grid_y), option->destination) /
                        std::max<int32_t>(1, m_unit->GetBaseValues()->GetAttribute(ATTRIB_SPEED)) +
                    JevAi_SiteWalkTurns;

                m_sites.push_back({m_unit->GetId(), option->unit_type, option->destination, GameManager_TurnCounter,
                                   walk_turns, option->build_rate});

                WalkToSite(option->unit_type, option->destination, option->build_rate);
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

            // a computer team's attack in the firing state is cancelled when the target is out of range: out of range,
            // request the approach path first; it stops within weapon range and then fires as a classic attack does
            m_unit->SetEnemy(&*option->target);
            m_unit->path = nullptr;
            m_unit->move_to_grid_x = option->target->grid_x;
            m_unit->move_to_grid_y = option->target->grid_y;

            const bool in_range = Access_IsWithinAttackRange(&*m_unit, option->target->grid_x, option->target->grid_y,
                                                             m_unit->GetBaseValues()->GetAttribute(ATTRIB_RANGE));

            UnitsManager_SetNewOrder(&*m_unit, ORDER_MOVE_TO_ATTACK,
                                     in_range ? ORDER_STATE_EXECUTING_ORDER : ORDER_STATE_MOVE_INIT);

            ++m_turn_attacks;
            m_wait_start = SDL_GetTicks();
            m_state = State::WAIT_FOR_UNIT;
        } break;

        case Action::MOVE: {
            m_scripted = option->data.value("action", "") == "hold";

            if (option->data.value("action", "") == "picket") {
                const uint32_t id = m_unit->GetId();

                m_pickets.erase(std::remove_if(m_pickets.begin(), m_pickets.end(),
                                               [id](const auto& picket) { return picket.first == id; }),
                                m_pickets.end());
                m_pickets.push_back({id, option->destination});
            }

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
        m_threats.clear();
        m_idle_factories.clear();
        m_idle_builders.clear();
        m_enemy_army.clear();
        m_links.clear();
        m_pickets.clear();
        m_raw_full_turns = 0;
        m_raw_lost = 0;
        m_defend_size = 4;
        m_scout_size = 1;
        m_posts.clear();
        m_interceptors.clear();
        m_command_trigger.clear();
        m_last_command_turn = -100;
        m_last_command_reason.clear();
        m_enemy_seen = false;
        m_enemy_base_seen = false;
        m_enemy_air_seen = false;
        m_command_enemy_seen = false;
        m_command_enemy_base_seen = false;
        m_command_enemy_air_seen = false;
        m_defenders_at_command = 0;
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
    m_turn_cleared_units.clear();
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
    AssignInterceptors();
    CheckCommandTriggers();

    // posts of lost units are free again
    m_posts.erase(std::remove_if(m_posts.begin(), m_posts.end(),
                                 [this](const auto& post) {
                                     for (auto it = UnitsManager_MobileLandSeaUnits.Begin(),
                                               it_end = UnitsManager_MobileLandSeaUnits.End();
                                          it != it_end; ++it) {
                                         if ((*it).GetId() == post.first && IsDefender(it->Get())) {
                                             return false;
                                         }
                                     }

                                     return true;
                                 }),
                  m_posts.end());

    // raw materials mined while the base storage is full are lost
    if (Complex* complex = GetEconomyComplex()) {
        Cargo materials;
        Cargo capacity;
        Cargo mining;
        Cargo mining_capacity;

        complex->GetCargoInfo(materials, capacity);
        complex->GetCargoMining(mining, mining_capacity);

        if (capacity.raw > 0 && materials.raw >= capacity.raw) {
            ++m_raw_full_turns;
            m_raw_lost += mining.raw;
        }
    }

    // pickets that reached their post watch it on sentry; lost pickets are forgotten
    m_pickets.erase(
        std::remove_if(m_pickets.begin(), m_pickets.end(),
                       [this](const auto& picket) {
                           for (auto it = UnitsManager_MobileLandSeaUnits.Begin(),
                                     it_end = UnitsManager_MobileLandSeaUnits.End();
                                it != it_end; ++it) {
                               if ((*it).GetId() == picket.first && (*it).team == m_team && (*it).hits > 0) {
                                   if (JevAi_GetDistance(Point((*it).grid_x, (*it).grid_y), picket.second) <= 1 &&
                                       (*it).GetOrder() == ORDER_AWAIT) {
                                       // as the game's own sentry (units_manager.cpp): a sentry left in the init state
                                       // counts as a pending order, and the end of turn waits for it for good
                                       UnitsManager_SetNewOrder(it->Get(), ORDER_SENTRY, ORDER_STATE_EXECUTING_ORDER);
                                   }

                                   return false;
                               }
                           }

                           return true;
                       }),
        m_pickets.end());

    // a link is done when all its connectors stand
    m_links.erase(std::remove_if(m_links.begin(), m_links.end(),
                                 [this](const Link& link) {
                                     return std::none_of(
                                         link.connectors.begin(), link.connectors.end(),
                                         [this](Point cell) { return JevAi_IsSiteFree(m_team, CNCT_4W, cell); });
                                 }),
                  m_links.end());

    std::string buildings;
    Complex* complex = GetEconomyComplex();

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

bool JevAi::SetupStrategy() {
    // The classic AI picks the strategy, landing site and clan and spends the starting credits; log what it bought.
    const bool result = NoneAi::SetupStrategy();

    if (result) {
        static const char* const strategy_names[AI_STRATEGY_MAX] = {
            "random", "defensive", "missiles", "air", "sea", "scout horde", "tank horde", "fast attack",
            "combined arms", "espionage"};
        const TeamMissionSupplies& supplies = UnitsManager_TeamMissionSupplies[m_team];
        const int32_t strategy = AiPlayer_Teams[m_team].GetStrategy();
        std::string loadout;
        std::string bought;

        for (uint32_t i = 0; i < supplies.units.GetCount(); ++i) {
            std::string& list = (i < supplies.unit_count) ? loadout : bought;

            list += (list.empty() ? " " : ", ") + JevAi_GetTypeName(*supplies.units[i]);

            if (*supplies.cargos[i]) {
                list += " (" + std::to_string(*supplies.cargos[i]) + " raw materials)";
            }
        }

        SDL_Log("Jev %s: setup by the classic AI: strategy %s, clan %s, credits %d, %d left; mission units:%s; bought:%s",
                GetTeamName(),
                (strategy >= 0 && strategy < AI_STRATEGY_MAX) ? strategy_names[strategy] : "unknown",
                ResourceManager_GetClanID(static_cast<TeamClanType>(UnitsManager_TeamInfo[m_team].team_clan)).c_str(),
                supplies.start_gold, supplies.team_gold, loadout.empty() ? " none" : loadout.c_str(),
                bought.empty() ? " nothing" : bought.c_str());
    }

    return result;
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
                    JevAi_IsSiteFree(m_team, m_pending_build.unit_type, site, &*m_unit)) {
                    const uint32_t unit_id = m_unit->GetId();

                    m_sites.erase(std::remove_if(m_sites.begin(), m_sites.end(),
                                                 [unit_id](const Site& item) { return item.unit_id == unit_id; }),
                                  m_sites.end());

                    StartBuilding(&*m_unit, m_pending_build.unit_type, site, m_pending_build.build_rate);

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

            if (m_scripted) {
                m_scripted = false;
                FinishUnit();

                return false;
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
                if (!m_unit && !m_command_trigger.empty()) {
                    if (AskCommand()) {
                        m_wait_start = SDL_GetTicks();
                        m_state = State::WAIT_FOR_ANSWER;
                    }

                    return true;
                }

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

                        // air defense once enemy aircraft are seen, without a question
                        auto flak = std::find_if(m_options.begin(), m_options.end(), [](const Option& option) {
                            return option.action == Action::PRODUCE && option.unit_type == SP_FLAK;
                        });

                        if (flak != m_options.end()) {
                            ApplyScripted(*flak, "enemy aircraft were seen");

                            return true;
                        }

                        // after "produce nothing" a factory is asked again only when its choices change: other unit
                        // types offered, or the base storage filling up (j5/j6: 85% of factory questions were
                        // nothing, 78% the same answer as before)
                        Cargo stock;
                        Cargo capacity;
                        std::string signature;

                        if (factory->GetComplex()) {
                            factory->GetComplex()->GetCargoInfo(stock, capacity);
                        }

                        for (const auto& option : m_options) {
                            const std::string type = option.data.value("type", "");

                            if (!type.empty() && signature.find("|" + type + "|") == std::string::npos) {
                                signature += "|" + type + "|";
                            }
                        }

                        signature += (capacity.raw > 0 && stock.raw * 5 >= capacity.raw * 4) ? "full" : "";
                        signature += CountOwnUnits(CONSTRCT) < 2 ? "|few constructors" : "";
                        signature += CountOwnUnits(ENGINEER) < 2 ? "|few engineers" : "";
                        m_factory_signature = signature;

                        auto idle = m_idle_factories.find(factory->GetId());

                        if (idle != m_idle_factories.end() && idle->second == signature) {
                            FinishUnit();

                            return true;
                        }

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

                        // a picket on sentry is asked again only when an enemy comes in sight
                        int32_t nearest_enemy = -1;
                        Point raider;
                        const bool quiet_sentry =
                            unit->GetOrder() == ORDER_SENTRY &&
                            GetDanger(&*unit, Point(unit->grid_x, unit->grid_y), &nearest_enemy) == 0 &&
                            (nearest_enemy < 0 || nearest_enemy > unit->GetBaseValues()->GetAttribute(ATTRIB_SCAN)) &&
                            !(IsInterceptor(&*unit) && FindRaider(&*unit, raider));

                        if (unit->hits > 0 && unit->GetUnitType() == SURVEYOR) {
                            if (!unit->auto_survey && Task_IsReadyToTakeOrders(&*unit)) {
                                SDL_Log("Jev %s: Surveyor at %s surveys by itself.", GetTeamName(),
                                        JevAi_FormatPoint(Point(unit->grid_x, unit->grid_y)).c_str());

                                if (unit->GetOrder() == ORDER_SENTRY) {
                                    UnitsManager_SetNewOrder(&*unit, ORDER_AWAIT, ORDER_STATE_EXECUTING_ORDER);
                                }

                                unit->auto_survey = true;
                                Ai_EnableAutoSurvey(&*unit);
                            }

                            m_units.erase(m_units.begin());
                            continue;
                        }

                        if (unit->hits > 0 && Task_IsReadyToTakeOrders(&*unit) && !quiet_sentry &&
                            (unit->GetUnitType() == SCOUT || unit->GetUnitType() == SCANNER)) {
                            const int32_t scripted = unit->GetUnitType() == SCANNER ? ScriptScanner(&*unit)
                                                     : IsInterceptor(&*unit)        ? ScriptInterceptor(&*unit)
                                                                                    : ScriptScout(&*unit);

                            if (scripted > 0) {
                                return true;
                            }

                            if (scripted == 0) {
                                m_units.erase(m_units.begin());
                                continue;
                            }
                        }

                        if (unit->hits > 0 && Task_IsReadyToTakeOrders(&*unit) && !quiet_sentry &&
                            IsDefender(&*unit)) {
                            const int32_t scripted = ScriptDefender(&*unit);

                            if (scripted > 0) {
                                return true;
                            }

                            if (scripted == 0) {
                                m_units.erase(m_units.begin());
                                continue;
                            }
                        }

                        if (unit->hits > 0 && Task_IsReadyToTakeOrders(&*unit) && !quiet_sentry &&
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

                    int32_t surveyed = 0;

                    for (int32_t cell = 0; cell < ResourceManager_MapSize.x * ResourceManager_MapSize.y; ++cell) {
                        surveyed += (ResourceManager_CargoMap[cell] & UnitsManager_TeamInfo[m_team].team_units->hash_team_id) != 0;
                    }

                    SDL_Log(
                        "Jev %s: turn %d done: %d questions, %d moves, %d did not move, %d builds started, %d "
                        "attacks, %d cells explored, %d cells surveyed, %d units lost.",
                        GetTeamName(), GameManager_TurnCounter, m_turn_questions, m_turn_moves, m_turn_failed_moves,
                        m_turn_builds, m_turn_attacks, m_explored_cells, surveyed, m_units_lost);
                }

                return AiController_EndTurn(m_team);
            }

            const int32_t danger_here = GetDanger(&*m_unit, Point(m_unit->grid_x, m_unit->grid_y), nullptr);

            if (ContinueToSite()) {
                return true;
            }

            const bool is_construction = JevAi_IsConstructionUnit(m_unit->GetUnitType());

            ComputeReach(&*m_unit);
            m_options.clear();

            const bool defender = IsDefender(&*m_unit);

            if (!is_construction) {
                AddAttackOptions(&*m_unit);
                AddChargeOptions(&*m_unit);
            }

            if (!defender || danger_here > 0) {
                AddMoveOptions(&*m_unit);
            }

            if (JevAi_IsBuilder(m_unit->GetUnitType())) {
                AddBuildOptions(&*m_unit);
                AddRefillOptions(&*m_unit);
            }

            if (m_unit->GetUnitType() == SURVEYOR) {
                AddSurveyOptions(&*m_unit);
            }

            if ((m_unit->GetUnitType() == SCOUT && !IsInterceptor(&*m_unit)) || m_unit->GetUnitType() == SCANNER) {
                AddScoutOptions(&*m_unit);

            } else if (defender || IsInterceptor(&*m_unit)) {
                AddHoldOption(&*m_unit);
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

            // counter-battery without a question: a long range unit with an outranging enemy in range fires at it, the
            // one nearest to destruction first
            {
                const Option* battery = nullptr;

                for (const Option& option : m_options) {
                    if (option.action == Action::ATTACK && option.data.value("counter_battery", false) &&
                        option.data.value("in_range", false) &&
                        (!battery || option.data.value("hits", 0) < battery->data.value("hits", 0))) {
                        battery = &option;
                    }
                }

                if (battery) {
                    const Option chosen = *battery;

                    ApplyScripted(chosen, "counter-battery");

                    return true;
                }
            }

            // a builder that can only refill or stay refills: Jev chose the refill 357 times of 358 (j7-groups4)
            if (is_construction && std::all_of(m_options.begin(), m_options.end(), [](const Option& option) {
                    return option.action == Action::STAY || option.data.value("action", "") == "refill";
                }) && m_options.size() == 2) {
                const Option refill = m_options.front();

                ApplyScripted(refill, "only a refill");

                return true;
            }

            // a builder that chose to stay is asked again only when its choices change, as factories are (j7-groups4:
            // 200 of 1124 construction questions answered stay)
            if (is_construction) {
                std::set<std::string> kinds;

                for (const Option& option : m_options) {
                    kinds.insert(option.data.value("action", "") + ":" + option.data.value("type", "") + ":" +
                                 option.data.value("from", ""));
                }

                std::string signature = std::to_string(m_unit->storage / 10);

                for (const std::string& kind : kinds) {
                    signature += "|" + kind;
                }

                m_builder_signature = signature;

                auto idle = m_idle_builders.find(m_unit->GetId());

                if (idle != m_idle_builders.end() && idle->second == signature) {
                    FinishUnit();

                    return true;
                }
            }

            if (m_enemy_air_seen && JevAi_IsBuilder(m_unit->GetUnitType()) &&
                CountAntiAirTurrets() < JevAi_AntiAirTurrets) {
                auto turret = std::find_if(m_options.begin(), m_options.end(), [](const Option& option) {
                    return option.action == Action::BUILD && option.unit_type == ANTIAIR;
                });

                if (turret != m_options.end()) {
                    ApplyScripted(*turret, "enemy aircraft were seen");

                    return true;
                }
            }

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
        std::find(m_lost.begin(), m_lost.end(), unit->GetId()) == m_lost.end()) {
        m_lost.push_back(unit->GetId());
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
