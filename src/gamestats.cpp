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
#include <nlohmann/json.hpp>

#if defined(__linux__)
#include <sys/resource.h>
#endif /* defined(__linux__) */

#include "game_manager.hpp"
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

static const char* const GameStats_TimeNames[GAMESTATS_TIME_COUNT] = {"tick_s", "orders_s", "render_s", "dispatch_s",
                                                                      "ai_s",   "input_s",  "present_s"};

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
                     {"by_timer", counters.end_turn_by_timer}};

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

            teams[GameStats_TeamNames[team]] = {{"units", units[team]},
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

void GameStats_AttackLanded(uint16_t attacker_team) {
    if (attacker_team < PLAYER_TEAM_MAX) {
        ++GameStats_AttacksLanded[attacker_team];
    }
}
