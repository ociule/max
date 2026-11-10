/* Copyright (c) 2026 M.A.X. Port Team
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

#include "gamestats.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(__linux__)
#include <sys/resource.h>
#endif /* defined(__linux__) */

#include "access.hpp"
#include "game_manager.hpp"
#include "hash.hpp"
#include "randomizer.hpp"
#include "resource_manager.hpp"
#include "units_manager.hpp"
#include "winloss.hpp"

using json = nlohmann::ordered_json;

GameStats_Counters GameStats_Turn;
uint64_t GameStats_TickCount;
bool GameStats_InDispatch;
std::atomic<uint64_t> GameStats_WorkerBusyNs;
std::atomic<uint64_t> GameStats_WorkerJobs;

static bool GameStats_Enabled;
static std::ofstream GameStats_File;
static GameStats_Counters GameStats_Game;
static uint64_t GameStats_GameStartNs;
static uint64_t GameStats_FirstTurnNs;
static uint64_t GameStats_TurnStartNs;
static uint64_t GameStats_TeamTurnsNs;
static uint64_t GameStats_TeamTurnCount;
static uint64_t GameStats_WorkerBusyTurnStartNs;
static uint64_t GameStats_WorkerJobsTurnStart;
static uint64_t GameStats_AttacksLanded[PLAYER_TEAM_MAX];

static const char* const GameStats_TeamNames[PLAYER_TEAM_MAX] = {"red", "green", "blue", "gray", "alien"};

static const char* const GameStats_BudgetNames[GAMESTATS_BUDGET_COUNT] = {
    "reminders_start",   "reminders", "path_dispatch", "auto_survey",        "check_assaults", "clear_zone_examine",
    "clear_zone_search", "kill_unit", "retreat",       "search_destination", "update_terrain", "distance_field"};

static const char* const GameStats_TimeNames[GAMESTATS_TIME_COUNT] = {
    "tick_s", "orders_s", "render_s", "dispatch_s", "ai_s", "input_s", "present_s", "poll_s", "animation_s",
    "idle_render_s"};

static void GameStats_Reset(GameStats_Counters& counters) {
    counters = {};
    counters.time_limit_min_ms = UINT64_MAX;
}

static double GameStats_Seconds(uint64_t ns) { return static_cast<double>(ns) / 1e9; }

static void GameStats_Accumulate(GameStats_Counters& total, const GameStats_Counters& turn) {
    for (int32_t i = 0; i < GAMESTATS_BUDGET_COUNT; ++i) {
        total.budget_checks[i] += turn.budget_checks[i];
        total.budget_cutoffs[i] += turn.budget_cutoffs[i];
    }

    for (int32_t i = 0; i < GAMESTATS_TIME_COUNT; ++i) {
        total.time_ns[i] += turn.time_ns[i];
    }

    total.ticks += turn.ticks;
    total.ticks_with_path_jobs += turn.ticks_with_path_jobs;
    total.loop_iterations += turn.loop_iterations;
    total.presents += turn.presents;
    total.time_limit_min_ms = std::min(total.time_limit_min_ms, turn.time_limit_min_ms);
    total.time_limit_max_ms = std::max(total.time_limit_max_ms, turn.time_limit_max_ms);
    total.time_limit_sum_ms += turn.time_limit_sum_ms;
    total.reminders += turn.reminders;
    total.reminders_left_at_cutoffs += turn.reminders_left_at_cutoffs;
    total.paths_requested += turn.paths_requested;
    total.paths_answered_directly += turn.paths_answered_directly;
    total.paths_dispatched += turn.paths_dispatched;
    total.paths_delivered_in_tick += turn.paths_delivered_in_tick;
    total.paths_delivered_between_ticks += turn.paths_delivered_between_ticks;
    total.paths_cancelled += turn.paths_cancelled;
    total.paths_discarded += turn.paths_discarded;
    total.path_latency_ticks_sum += turn.path_latency_ticks_sum;
    total.path_latency_ticks_max = std::max(total.path_latency_ticks_max, turn.path_latency_ticks_max);
    total.path_latency_ns_sum += turn.path_latency_ns_sum;
    total.path_latency_ns_max = std::max(total.path_latency_ns_max, turn.path_latency_ns_max);
    total.end_turn_idle_max = std::max(total.end_turn_idle_max, turn.end_turn_idle_max);
    total.end_turn_idle_resets += turn.end_turn_idle_resets;
    total.end_turn_by_idle_limit += turn.end_turn_by_idle_limit;
    total.end_turn_by_timer += turn.end_turn_by_timer;
    total.end_turn_wait_abandoned += turn.end_turn_wait_abandoned;
    total.assertions += turn.assertions;
}

static json GameStats_ToJson(const GameStats_Counters& counters) {
    json j;

    j["ticks"] = counters.ticks;
    j["ticks_with_path_jobs"] = counters.ticks_with_path_jobs;
    j["loop_iterations"] = counters.loop_iterations;
    j["presents"] = counters.presents;

    for (int32_t i = 0; i < GAMESTATS_TIME_COUNT; ++i) {
        j[GameStats_TimeNames[i]] = GameStats_Seconds(counters.time_ns[i]);
    }

    j["time_limit_ms"] = {
        {"min", counters.ticks && counters.time_limit_min_ms != UINT64_MAX ? counters.time_limit_min_ms : 0},
        {"mean", counters.ticks ? static_cast<double>(counters.time_limit_sum_ms) / counters.ticks : 0.0},
        {"max", counters.time_limit_max_ms}};

    json budget;

    for (int32_t i = 0; i < GAMESTATS_BUDGET_COUNT; ++i) {
        budget[GameStats_BudgetNames[i]] = {counters.budget_checks[i], counters.budget_cutoffs[i]};
    }

    j["budget_checks_cutoffs"] = budget;
    j["reminders"] = counters.reminders;
    j["reminders_left_at_cutoffs"] = counters.reminders_left_at_cutoffs;

    const uint64_t delivered = counters.paths_delivered_in_tick + counters.paths_delivered_between_ticks;

    j["paths"] = {{"requested", counters.paths_requested},
                  {"answered_directly", counters.paths_answered_directly},
                  {"dispatched", counters.paths_dispatched},
                  {"delivered_in_tick", counters.paths_delivered_in_tick},
                  {"delivered_between_ticks", counters.paths_delivered_between_ticks},
                  {"cancelled", counters.paths_cancelled},
                  {"discarded", counters.paths_discarded},
                  {"latency_ticks_mean", delivered ? static_cast<double>(counters.path_latency_ticks_sum) / delivered
                                                   : 0.0},
                  {"latency_ticks_max", counters.path_latency_ticks_max},
                  {"latency_ms_mean",
                   delivered ? static_cast<double>(counters.path_latency_ns_sum) / delivered / 1e6 : 0.0},
                  {"latency_ms_max", static_cast<double>(counters.path_latency_ns_max) / 1e6}};

    j["end_turn"] = {{"idle_ticks_max", counters.end_turn_idle_max},
                     {"idle_resets", counters.end_turn_idle_resets},
                     {"by_idle_limit", counters.end_turn_by_idle_limit},
                     {"by_timer", counters.end_turn_by_timer},
                     {"wait_abandoned", counters.end_turn_wait_abandoned}};

    j["assertions"] = counters.assertions;

    return j;
}

static uint64_t GameStats_ConsistencyViolations;

/* The surface a mobile unit stands on by the rules of Access_IsAccessible(): a bridge is land for land units and
 * water for sea units, even on a coast tile, a water platform and rubble are land.
 */
static uint8_t GameStats_SurfaceFor(const UnitInfo* unit) {
    uint8_t surface = Access_GetSurfaceType(unit->grid_x, unit->grid_y);
    const auto cell = Hash_MapHash[Point(unit->grid_x, unit->grid_y)];

    if (cell) {
        for (auto it = cell->Begin(), it_end = cell->End(); it != it_end; ++it) {
            switch ((*it).GetUnitType()) {
                case BRIDGE: {
                    surface = (unit->flags & MOBILE_LAND_UNIT) ? SURFACE_TYPE_LAND : SURFACE_TYPE_WATER;
                } break;

                case WTRPLTFM:
                case LRGRUBLE:
                case SMLRUBLE: {
                    surface = SURFACE_TYPE_LAND;
                } break;

                default: {
                } break;
            }
        }
    }

    return surface;
}

/* Orders whose execution reads the unit's parent while they are in progress: the unit being serviced, carried, built
 * or held (ProcessRepair(), ProcessTransfer(), ProcessLoading(), ProcessUnloading(), ProcessLanding(),
 * UnitsManager_ProcessOrderReload(), UnitsManager_ProcessOrderUpgrade(), UnitsManager_ProcessOrderActivate(),
 * UnitsManager_StartClearing(), a builder's finished product). A unit boarding a container (ORDER_MOVE_TO_UNIT) finds
 * it by position, not by its parent. A finished order (ORDER_STATE_EXECUTING_ORDER) reads nothing any more.
 */
static bool GameStats_OrderUsesParent(const UnitInfo* unit) {
    if (unit->GetOrderState() == ORDER_STATE_EXECUTING_ORDER) {
        return false;
    }

    switch (unit->GetOrder()) {
        case ORDER_BUILD: {
            // a builder links to its product when the product is done, until then only a bulldozer reads its parent
            return unit->GetOrderState() == ORDER_STATE_UNIT_READY ||
                   unit->GetOrderState() == ORDER_STATE_BUILD_CLEARING || unit->GetUnitType() == BULLDOZR;
        }

        case ORDER_ACTIVATE:
        case ORDER_UNLOAD:
        case ORDER_CLEAR:
        case ORDER_LAND:
        case ORDER_LOAD:
        case ORDER_REPAIR:
        case ORDER_RELOAD:
        case ORDER_TRANSFER:
        case ORDER_UPGRADE:
            return true;

        default:
            return false;
    }
}

/* Consistency checks of the game state, run at the end of every team turn, when no unit moves. Only reads. The checks
 * cross the unit lists, the map index (Hash_MapHash), the unit index (Hash_UnitHash), the containers and their stored
 * units, and each unit's limits. A violation is a state the game logic should never leave behind, counted per check
 * with the first few cases described.
 */
static json GameStats_CheckConsistency() {
    std::map<std::string, uint64_t> counts;
    std::map<std::string, uint64_t> notes;
    std::vector<std::string> examples;
    std::vector<std::string> note_examples;

    auto describe = [](const UnitInfo* unit) {
        return std::string(ResourceManager_GetResourceID(unit->GetUnitType())) + " team " +
               std::to_string(unit->team) + " id " + std::to_string(unit->GetId()) + " at [" +
               std::to_string(unit->grid_x + 1) + "," + std::to_string(unit->grid_y + 1) + "] order " +
               std::to_string(unit->GetOrder()) + "/" + std::to_string(unit->GetOrderState());
    };

    auto report = [&counts, &examples](const char* check, const std::string& text) {
        ++counts[check];

        if (examples.size() < 20) {
            examples.push_back(std::string(check) + ": " + text);
        }
    };

    // known harmless states, counted apart from the violations, with their own examples
    auto note = [&notes, &note_examples](const char* check, const std::string& text) {
        ++notes[check];

        if (note_examples.size() < 5) {
            note_examples.push_back(std::string(check) + ": " + text);
        }
    };

    struct ListInfo {
        SmartList<UnitInfo>* list;
        const char* name;
    };

    const ListInfo lists[] = {{&UnitsManager_GroundCoverUnits, "ground_cover"},
                              {&UnitsManager_MobileLandSeaUnits, "mobile_land_sea"},
                              {&UnitsManager_StationaryUnits, "stationary"},
                              {&UnitsManager_MobileAirUnits, "mobile_air"},
                              {&UnitsManager_ParticleUnits, "particles"}};

    std::unordered_map<const UnitInfo*, int32_t> memberships;
    std::vector<UnitInfo*> units;

    for (const auto& list : lists) {
        for (auto it = list.list->Begin(), it_end = list.list->End(); it != it_end; ++it) {
            if (++memberships[it->Get()] == 1) {
                units.push_back(it->Get());
            }
        }
    }

    if (UnitsManager_ParticleUnits.GetCount() > 0) {
        report("particles_at_rest", std::to_string(UnitsManager_ParticleUnits.GetCount()) + " particles");
    }

    const Point map_size = ResourceManager_MapSize;
    std::unordered_map<const UnitInfo*, int32_t> held_units;
    std::unordered_set<uint32_t> ids;

    for (UnitInfo* unit : units) {
        if (memberships[unit] > 1) {
            report("unit_in_several_lists", describe(unit));
        }

        if (unit->grid_x < 0 || unit->grid_y < 0 || unit->grid_x >= map_size.x || unit->grid_y >= map_size.y) {
            report("unit_outside_map", describe(unit));

            continue;
        }

        if (unit->flags & (EXPLODING | MISSILE_UNIT)) {
            continue;
        }

        UnitValues* values = unit->GetBaseValues();

        if (unit->hits == 0 && values->GetAttribute(ATTRIB_HITS) > 0 && unit->GetOrderState() != ORDER_STATE_DESTROY) {
            report("destroyed_unit_in_lists", describe(unit));
        }

        if (unit->hits > values->GetAttribute(ATTRIB_HITS)) {
            report("hits_above_max", describe(unit) + " hits " + std::to_string(unit->hits));
        }

        if (unit->ammo > values->GetAttribute(ATTRIB_AMMO)) {
            report("ammo_above_max", describe(unit) + " ammo " + std::to_string(unit->ammo));
        }

        if (unit->shots > values->GetAttribute(ATTRIB_ROUNDS)) {
            report("shots_above_max", describe(unit) + " shots " + std::to_string(unit->shots));
        }

        if (unit->speed > values->GetAttribute(ATTRIB_SPEED)) {
            report("speed_above_max", describe(unit) + " speed " + std::to_string(unit->speed));
        }

        /* Rubble keeps the materials a bulldozer salvages from it in its storage and a greenhouse counts the points it
         * produced in it, beyond any storage attribute.
         */
        const bool factory_unit_ready = (unit->flags & STATIONARY) && unit->GetOrder() == ORDER_BUILD &&
                                        unit->GetOrderState() == ORDER_STATE_UNIT_READY && unit->storage == 1;

        if (unit->storage < 0 ||
            (unit->storage > values->GetAttribute(ATTRIB_STORAGE) && unit->GetUnitType() != SMLRUBLE &&
             unit->GetUnitType() != LRGRUBLE && unit->GetUnitType() != GREENHSE && !factory_unit_ready)) {
            report("storage_out_of_range", describe(unit) + " storage " + std::to_string(unit->storage));
        }

        if (unit->GetId() != UnitInfo::NO_ID) {
            if (!ids.insert(unit->GetId()).second) {
                report("duplicate_unit_id", describe(unit));
            }

            if (Hash_UnitHash[unit->GetId()] != unit) {
                report("unit_index_mismatch", describe(unit));
            }
        }

        UnitInfo* parent = unit->GetParent();

        /* A factory holds its finished unit on its own cell until the unit is activated, a mobile builder its finished
         * building: the unit is idle and on the map, a factory counts it as its storage. Harmless, but a unit that
         * waits for turns shows a stuck builder.
         */
        const bool waits_for_builder = unit->GetOrder() == ORDER_IDLE && parent && parent->GetOrder() == ORDER_BUILD &&
                                       parent->GetOrderState() == ORDER_STATE_UNIT_READY;

        if (waits_for_builder) {
            note("unit_waits_for_builder", describe(unit) + " of " + describe(parent));

        } else if (unit->GetOrder() == ORDER_IDLE) {
            // a stored unit: held by a living container, off the map
            if (!parent) {
                report("stored_unit_without_container", describe(unit));

            } else if (!memberships.count(parent) || parent->hits == 0) {
                report("stored_unit_in_destroyed_container", describe(unit) + " in " + describe(parent));

            } else {
                ++held_units[parent];
            }

        } else {
            /* A unit keeps the parent of its last service order (a repair truck the unit it repaired), nothing reads it
             * until an order that uses the parent sets a new one. Only an order that reads a removed parent is wrong.
             */
            if (parent && !memberships.count(parent) && !(parent->flags & (EXPLODING | MISSILE_UNIT))) {
                if (GameStats_OrderUsesParent(unit)) {
                    report("parent_not_in_lists", describe(unit) + " parent " + describe(parent));

                } else {
                    note("stale_parent", describe(unit) + " parent " + describe(parent));
                }
            }

            // every cell of a unit on the map is indexed
            const int32_t size = (unit->flags & BUILDING) ? 2 : 1;

            for (int32_t dx = 0; dx < size; ++dx) {
                for (int32_t dy = 0; dy < size; ++dy) {
                    const auto cell = Hash_MapHash[Point(unit->grid_x + dx, unit->grid_y + dy)];
                    bool found = false;

                    if (cell) {
                        for (auto it = cell->Begin(), it_end = cell->End(); it != it_end; ++it) {
                            if (it->Get() == unit) {
                                found = true;
                                break;
                            }
                        }
                    }

                    if (!found) {
                        report("unit_missing_from_map_index", describe(unit));
                    }
                }
            }

            // mobile units stand on terrain they may enter
            if ((unit->flags & (MOBILE_LAND_UNIT | MOBILE_SEA_UNIT)) && !(unit->flags & MOBILE_AIR_UNIT)) {
                const uint8_t surface = GameStats_SurfaceFor(unit);

                if (!(ResourceManager_GetUnit(unit->GetUnitType()).GetLandType() & surface)) {
                    report("unit_on_forbidden_terrain", describe(unit) + " surface " + std::to_string(surface));
                }
            }
        }
    }

    // containers count exactly the units they hold
    for (UnitInfo* unit : units) {
        const ResourceID type = unit->GetUnitType();
        const auto held = held_units.find(unit);
        const int32_t held_count = held != held_units.end() ? held->second : 0;

        if (type == SEATRANS || type == AIRTRANS || type == CLNTRANS || type == HANGAR || type == DOCK ||
            type == BARRACKS || type == DEPOT) {
            if (unit->hits > 0 && unit->storage != held_count) {
                report("container_storage_mismatch",
                       describe(unit) + " storage " + std::to_string(unit->storage) + " holds " +
                           std::to_string(held_count));
            }

        } else if (held_count > 0) {
            report("units_held_by_non_container", describe(unit) + " holds " + std::to_string(held_count));
        }
    }

    // every map index entry is a listed unit on one of its cells
    for (int32_t x = 0; x < map_size.x; ++x) {
        for (int32_t y = 0; y < map_size.y; ++y) {
            const auto cell = Hash_MapHash[Point(x, y)];

            if (!cell) {
                continue;
            }

            for (auto it = cell->Begin(), it_end = cell->End(); it != it_end; ++it) {
                const UnitInfo* unit = it->Get();

                if (!memberships.count(unit)) {
                    report("map_index_unit_not_in_lists",
                           describe(unit) + " indexed at [" + std::to_string(x + 1) + "," + std::to_string(y + 1) +
                               "]");
                    continue;
                }

                const UnitInfo* holder = unit->GetParent();

                if (unit->GetOrder() == ORDER_IDLE &&
                    !(holder && holder->GetOrder() == ORDER_BUILD &&
                      holder->GetOrderState() == ORDER_STATE_UNIT_READY)) {
                    report("stored_unit_on_map", describe(unit));
                    continue;
                }

                const int32_t size = (unit->flags & BUILDING) ? 2 : 1;
                const bool on_unit = x >= unit->grid_x && x < unit->grid_x + size && y >= unit->grid_y &&
                                     y < unit->grid_y + size;
                const bool on_transit = unit->in_transit && x == unit->last_target.x && y == unit->last_target.y;

                if (!on_unit && !on_transit) {
                    report("map_index_wrong_cell", describe(unit) + " indexed at [" + std::to_string(x + 1) + "," +
                                                       std::to_string(y + 1) + "]");
                }
            }
        }
    }

    uint64_t violations = 0;

    for (const auto& [check, count] : counts) {
        violations += count;
    }

    GameStats_ConsistencyViolations += violations;

    json j;

    j["violations"] = violations;
    j["by_check"] = counts;
    j["notes"] = notes;
    j["examples"] = examples;
    j["note_examples"] = note_examples;

    return j;
}

/* A 64 bit FNV-1a hash of the state the network desync check compares (every unit's identity, position, hit points,
 * speed, shots, cargo and ammunition) plus the orders. Equal games have equal checksums at equal points.
 */
static uint64_t GameStats_Checksum() {
    uint64_t hash = 0xcbf29ce484222325ull;

    auto mix = [&hash](int64_t value) {
        for (int32_t i = 0; i < 8; ++i) {
            hash ^= static_cast<uint8_t>(value >> (i * 8));
            hash *= 0x100000001b3ull;
        }
    };

    for (auto* list : {&UnitsManager_GroundCoverUnits, &UnitsManager_MobileLandSeaUnits, &UnitsManager_StationaryUnits,
                       &UnitsManager_MobileAirUnits}) {
        for (auto it = list->Begin(), it_end = list->End(); it != it_end; ++it) {
            const UnitInfo& unit = *it;

            if (unit.GetId() != UnitInfo::NO_ID && !(unit.flags & EXPLODING)) {
                mix(unit.team);
                mix(unit.GetUnitType());
                mix(unit.unit_id);
                mix(unit.grid_x);
                mix(unit.grid_y);
                mix(unit.hits);
                mix(unit.speed);
                mix(unit.shots);
                mix(unit.storage);
                mix(unit.ammo);
                mix(unit.GetOrder());
                mix(unit.GetOrderState());
            }
        }
    }

    return hash;
}

static json GameStats_Teams() {
    json teams;
    int32_t units[PLAYER_TEAM_MAX]{};
    int32_t buildings[PLAYER_TEAM_MAX]{};

    for (auto* list : {&UnitsManager_MobileLandSeaUnits, &UnitsManager_MobileAirUnits}) {
        for (auto it = list->Begin(), it_end = list->End(); it != it_end; ++it) {
            ++units[(*it).team];
        }
    }

    for (auto it = UnitsManager_StationaryUnits.Begin(), it_end = UnitsManager_StationaryUnits.End(); it != it_end;
         ++it) {
        ++buildings[(*it).team];
    }

    for (int32_t team = PLAYER_TEAM_RED; team < PLAYER_TEAM_MAX - 1; ++team) {
        const CTInfo& info = UnitsManager_TeamInfo[team];

        if (info.team_type != TEAM_TYPE_NONE) {
            uint64_t lost = 0;

            for (const uint32_t casualty : info.casualties) {
                lost += casualty;
            }

            json remnants = json::array();

            // the last few mobile units of a team, to tell why a beaten team is not eliminated
            if (units[team] > 0 && units[team] <= 10) {
                for (auto* list : {&UnitsManager_MobileLandSeaUnits, &UnitsManager_MobileAirUnits}) {
                    for (auto it = list->Begin(), it_end = list->End(); it != it_end; ++it) {
                        if ((*it).team == team) {
                            std::string seen_by;

                            for (int32_t other = PLAYER_TEAM_RED; other < PLAYER_TEAM_MAX - 1; ++other) {
                                if (other != team && (*it).IsVisibleToTeam(other)) {
                                    seen_by += GameStats_TeamNames[other];
                                }
                            }

                            remnants.push_back(std::string(ResourceManager_GetResourceID((*it).GetUnitType())) +
                                               " at [" + std::to_string((*it).grid_x + 1) + "," +
                                               std::to_string((*it).grid_y + 1) + "] order " +
                                               std::to_string((*it).GetOrder()) + "/" +
                                               std::to_string((*it).GetOrderState()) + " hits " +
                                               std::to_string((*it).hits) + " seen by " +
                                               (seen_by.empty() ? "none" : seen_by));
                        }
                    }
                }
            }

            teams[GameStats_TeamNames[team]] = {{"units", units[team]},
                                                {"remnants", remnants},
                                                {"buildings", buildings[team]},
                                                {"worth", WinLoss_GetTotalWorthOfAssets(team)},
                                                {"points", info.team_points},
                                                {"units_built", info.stats_units_built},
                                                {"buildings_built", info.stats_buildings_built},
                                                {"lost", lost},
                                                {"attacks_landed", GameStats_AttacksLanded[team]}};
        }
    }

    return teams;
}

static void GameStats_Write(const json& line) {
    if (GameStats_File) {
        GameStats_File << line.dump() << '\n';
        GameStats_File.flush();
    }
}

uint64_t GameStats_Now() { return SDL_GetTicksNS(); }

void GameStats_Init() {
    GameStats_Enabled = true;
    GameStats_GameStartNs = GameStats_Now();
    GameStats_Reset(GameStats_Turn);
    GameStats_Reset(GameStats_Game);

    GameStats_File.open((ResourceManager_FilePathGamePref / "stats.jsonl").lexically_normal(),
                        std::ios::out | std::ios::trunc);

    if (!GameStats_File) {
        SDL_Log("Statistics: cannot open stats.jsonl.");
    }
}

void GameStats_BeginTeamTurn(int32_t team) {
    if (!GameStats_Enabled) {
        return;
    }

    GameStats_TurnStartNs = GameStats_Now();

    if (!GameStats_FirstTurnNs) {
        GameStats_FirstTurnNs = GameStats_TurnStartNs;
    }

    GameStats_WorkerBusyTurnStartNs = GameStats_WorkerBusyNs.load(std::memory_order_relaxed);
    GameStats_WorkerJobsTurnStart = GameStats_WorkerJobs.load(std::memory_order_relaxed);

    // the work between two team turns (end of turn processing, economy) only counts in the game totals
    GameStats_Accumulate(GameStats_Game, GameStats_Turn);
    GameStats_Reset(GameStats_Turn);
}

void GameStats_EndTeamTurn(int32_t team) {
    if (!GameStats_Enabled) {
        return;
    }

    const uint64_t wall_ns = GameStats_Now() - GameStats_TurnStartNs;

    GameStats_TeamTurnsNs += wall_ns;
    ++GameStats_TeamTurnCount;

    json line;

    line["type"] = "team_turn";
    line["turn"] = GameManager_TurnCounter;
    line["team"] = (team >= 0 && team < PLAYER_TEAM_MAX) ? GameStats_TeamNames[team] : "all";
    line["wall_s"] = GameStats_Seconds(wall_ns);
    line.update(GameStats_ToJson(GameStats_Turn));
    line["worker_busy_s"] =
        GameStats_Seconds(GameStats_WorkerBusyNs.load(std::memory_order_relaxed) - GameStats_WorkerBusyTurnStartNs);
    line["worker_jobs"] = GameStats_WorkerJobs.load(std::memory_order_relaxed) - GameStats_WorkerJobsTurnStart;
    line["teams"] = GameStats_Teams();
    line["rng_draws"] = Randomizer_GetDrawCount();
    line["rng_state"] = Randomizer_GetStateHash();
    line["checksum"] = GameStats_Checksum();
    line["consistency"] = GameStats_CheckConsistency();

    GameStats_Write(line);

    GameStats_Accumulate(GameStats_Game, GameStats_Turn);
    GameStats_Reset(GameStats_Turn);
}

void GameStats_EndGame() {
    if (!GameStats_Enabled) {
        return;
    }

    GameStats_Accumulate(GameStats_Game, GameStats_Turn);
    GameStats_Reset(GameStats_Turn);

    const uint64_t now = GameStats_Now();
    json line;

    line["type"] = "game";
    line["turns"] = GameManager_TurnCounter;
    line["team_turns"] = GameStats_TeamTurnCount;
    line["wall_s"] = GameStats_Seconds(now - GameStats_GameStartNs);
    line["setup_s"] = GameStats_Seconds((GameStats_FirstTurnNs ? GameStats_FirstTurnNs : now) - GameStats_GameStartNs);
    line["team_turns_s"] = GameStats_Seconds(GameStats_TeamTurnsNs);
    line["since_sdl_init_s"] = GameStats_Seconds(now);
    line.update(GameStats_ToJson(GameStats_Game));
    line["worker_busy_s"] = GameStats_Seconds(GameStats_WorkerBusyNs.load(std::memory_order_relaxed));
    line["worker_jobs"] = GameStats_WorkerJobs.load(std::memory_order_relaxed);
    line["teams"] = GameStats_Teams();
    line["rng_draws"] = Randomizer_GetDrawCount();
    line["rng_state"] = Randomizer_GetStateHash();
    line["checksum"] = GameStats_Checksum();
    line["consistency"] = GameStats_CheckConsistency();
    line["consistency_violations_total"] = GameStats_ConsistencyViolations;

#if defined(__linux__)
    struct rusage process_usage {};
    struct rusage thread_usage {};

    auto seconds = [](const struct timeval& time) { return time.tv_sec + time.tv_usec / 1e6; };

    if (getrusage(RUSAGE_SELF, &process_usage) == 0 && getrusage(RUSAGE_THREAD, &thread_usage) == 0) {
        line["process"] = {{"user_s", seconds(process_usage.ru_utime)},
                           {"system_s", seconds(process_usage.ru_stime)},
                           {"main_thread_user_s", seconds(thread_usage.ru_utime)},
                           {"main_thread_system_s", seconds(thread_usage.ru_stime)},
                           {"peak_rss_kb", process_usage.ru_maxrss}};
    }
#endif /* defined(__linux__) */

    GameStats_Write(line);
    GameStats_File.close();
    GameStats_Enabled = false;
}

void GameStats_Tick(bool path_jobs_outstanding) {
    ++GameStats_TickCount;
    ++GameStats_Turn.ticks;

    if (path_jobs_outstanding) {
        ++GameStats_Turn.ticks_with_path_jobs;
    }
}

void GameStats_TimeLimit(uint64_t time_limit_ms) {
    GameStats_Turn.time_limit_min_ms = std::min(GameStats_Turn.time_limit_min_ms, time_limit_ms);
    GameStats_Turn.time_limit_max_ms = std::max(GameStats_Turn.time_limit_max_ms, time_limit_ms);
    GameStats_Turn.time_limit_sum_ms += time_limit_ms;
}

void GameStats_PathDelivered(uint64_t dispatch_tick, uint64_t dispatch_ns) {
    const uint64_t latency_ticks = GameStats_TickCount - dispatch_tick;
    const uint64_t latency_ns = GameStats_Now() - dispatch_ns;

    if (GameStats_InDispatch) {
        ++GameStats_Turn.paths_delivered_in_tick;

    } else {
        ++GameStats_Turn.paths_delivered_between_ticks;
    }

    GameStats_Turn.path_latency_ticks_sum += latency_ticks;
    GameStats_Turn.path_latency_ticks_max = std::max(GameStats_Turn.path_latency_ticks_max, latency_ticks);
    GameStats_Turn.path_latency_ns_sum += latency_ns;
    GameStats_Turn.path_latency_ns_max = std::max(GameStats_Turn.path_latency_ns_max, latency_ns);
}

void GameStats_EndTurnIdle(uint32_t idle_ticks) {
    GameStats_Turn.end_turn_idle_max = std::max<uint64_t>(GameStats_Turn.end_turn_idle_max, idle_ticks);
}

void GameStats_Assertion() { ++GameStats_Turn.assertions; }

void GameStats_AttackLanded(uint16_t attacker_team) {
    if (attacker_team < PLAYER_TEAM_MAX) {
        ++GameStats_AttacksLanded[attacker_team];
    }
}
