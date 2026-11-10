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

#ifndef GAMESTATS_HPP
#define GAMESTATS_HPP

/* Run time statistics of command line games (see commandline.hpp), written to stats.jsonl in the preferences folder.
 *
 * One JSON line per team turn records where the wall clock time went, how often the computer players ran out of
 * thinking time, how the path finding worker kept up, how the turn ended, what happened in the game and a checksum
 * of the game state. A last line records the process totals. The counters are plain increments and clock reads, the
 * file is written once per team turn, so the statistics do not disturb the timing they measure.
 *
 * The statistics compare the speed and the computer player behavior of different builds and settings, see
 * benchmarks/README.md.
 */

#include <atomic>
#include <cstdint>

/// The places where a computer player stops working when the tick's thinking time is spent.
enum GameStatsBudgetSite : uint8_t {
    GAMESTATS_BUDGET_REMINDERS_START,
    GAMESTATS_BUDGET_REMINDERS,
    GAMESTATS_BUDGET_PATH_DISPATCH,
    GAMESTATS_BUDGET_AUTO_SURVEY,
    GAMESTATS_BUDGET_CHECK_ASSAULTS,
    GAMESTATS_BUDGET_CLEAR_ZONE_EXAMINE,
    GAMESTATS_BUDGET_CLEAR_ZONE_SEARCH,
    GAMESTATS_BUDGET_KILL_UNIT,
    GAMESTATS_BUDGET_RETREAT,
    GAMESTATS_BUDGET_SEARCH_DESTINATION,
    GAMESTATS_BUDGET_UPDATE_TERRAIN,
    GAMESTATS_BUDGET_DISTANCE_FIELD,
    GAMESTATS_BUDGET_COUNT
};

/// Timed parts of the main loop.
enum GameStatsTime : uint8_t {
    GAMESTATS_TIME_TICK,         ///< whole tick, from the tick start to the end of the computer player work
    GAMESTATS_TIME_ORDERS,       ///< unit orders of the tick
    GAMESTATS_TIME_RENDER,       ///< map rendering of the tick
    GAMESTATS_TIME_DISPATCH,     ///< path request preparation and dispatch of the tick
    GAMESTATS_TIME_AI,           ///< computer player reactions, reminders and end turn checks of the tick
    GAMESTATS_TIME_INPUT,        ///< input processing between ticks, includes the background processes
    GAMESTATS_TIME_PRESENT,      ///< presenting the window, wherever it happens
    GAMESTATS_TIME_POLL,         ///< collecting path results at the top of every loop pass, runs their callbacks
    GAMESTATS_TIME_ANIMATION,    ///< turn timer display, corner animation and palette color animation of every pass
    GAMESTATS_TIME_IDLE_RENDER,  ///< map rendering between ticks
    GAMESTATS_TIME_COUNT
};

struct GameStats_Counters {
    uint64_t budget_checks[GAMESTATS_BUDGET_COUNT];
    uint64_t budget_cutoffs[GAMESTATS_BUDGET_COUNT];
    uint64_t time_ns[GAMESTATS_TIME_COUNT];

    uint64_t ticks;
    uint64_t ticks_with_path_jobs;
    uint64_t loop_iterations;
    uint64_t presents;

    uint64_t time_limit_min_ms;
    uint64_t time_limit_max_ms;
    uint64_t time_limit_sum_ms;

    uint64_t reminders;
    uint64_t reminders_left_at_cutoffs;

    uint64_t paths_requested;
    uint64_t paths_answered_directly;
    uint64_t paths_dispatched;
    uint64_t paths_delivered_in_tick;
    uint64_t paths_delivered_between_ticks;
    uint64_t paths_cancelled;
    uint64_t paths_discarded;
    uint64_t path_latency_ticks_sum;
    uint64_t path_latency_ticks_max;
    uint64_t path_latency_ns_sum;
    uint64_t path_latency_ns_max;

    uint64_t end_turn_idle_max;
    uint64_t end_turn_idle_resets;
    uint64_t end_turn_by_idle_limit;
    uint64_t end_turn_by_timer;

    uint64_t assertions;
};

extern GameStats_Counters GameStats_Turn;
extern uint64_t GameStats_TickCount;
extern bool GameStats_InDispatch;
extern std::atomic<uint64_t> GameStats_WorkerBusyNs;
extern std::atomic<uint64_t> GameStats_WorkerJobs;

/// Counts a thinking time check and returns its outcome unchanged. A false outcome stopped work that was left.
inline bool GameStats_Budget(GameStatsBudgetSite site, bool allowed) {
    ++GameStats_Turn.budget_checks[site];

    if (!allowed) {
        ++GameStats_Turn.budget_cutoffs[site];
    }

    return allowed;
}

/// Nanosecond clock for the timed parts.
uint64_t GameStats_Now();

inline void GameStats_AddTime(GameStatsTime part, uint64_t start_ns) {
    GameStats_Turn.time_ns[part] += GameStats_Now() - start_ns;
}

/// Starts the statistics of a command line game, truncates stats.jsonl.
void GameStats_Init();

/// Team turn boundaries, team -1 for the concurrent turn of all teams.
void GameStats_BeginTeamTurn(int32_t team);
void GameStats_EndTeamTurn(int32_t team);

/// Writes the process totals, called when the game is over.
void GameStats_EndGame();

void GameStats_Tick(bool path_jobs_outstanding);
void GameStats_TimeLimit(uint64_t time_limit_ms);
void GameStats_PathDelivered(uint64_t dispatch_tick, uint64_t dispatch_ns);
void GameStats_EndTurnIdle(uint32_t idle_ticks);
void GameStats_AttackLanded(uint16_t attacker_team);

/// Counts a failed assertion (hidden games log and ignore them).
void GameStats_Assertion();

#endif /* GAMESTATS_HPP */
