#!/usr/bin/env python3
"""Apply a temporary timing instrumentation to the game sources (revert with git checkout).

Per turn it logs: ticks, time inside due ticks split into orders / render / AI, time in ProcessInput (GNW input,
background processes and the SDL present), the remaining time spent between ticks (pacing) and the number of
turn timer expiries (an AI turn cut off after 45 real seconds).
"""

from pathlib import Path

SRC = Path(__file__).resolve().parent.parent.parent / 'src'


def patch(name, old, new, count=1):
    path = SRC / name
    text = path.read_text()
    assert text.count(old) == count, (name, old[:60], text.count(old))
    path.write_text(text.replace(old, new))


patch('game_manager.cpp', '''bool GameManager_RequestMenuExit;
''', '''bool GameManager_RequestMenuExit;

struct GameManager_ProfData {
    uint64_t ticks, busy_ns, orders_ns, render_ns, ai_ns, input_ns, loop_ns, timeouts, idle_calls;
};
GameManager_ProfData GameManager_Prof;
static inline uint64_t GameManager_ProfNow() { return SDL_GetTicksNS(); }
''')

# turn timer expiry: the AI turn is cut off
patch('game_manager.cpp', '''            if (GameManager_TurnTimerValue <= 0) {
                GameManager_UpdateTurnTimer(false, 0);
                GameManager_RequestMenuExit = true;''', '''            if (GameManager_TurnTimerValue <= 0) {
                ++GameManager_Prof.timeouts;
                GameManager_UpdateTurnTimer(false, 0);
                GameManager_RequestMenuExit = true;''')

patch('game_manager.cpp', '''void GameManager_ProgressTurn() {
    do {
        GameManager_Progress2 = true;
        GameManager_ProcessInput();
    } while (GameManager_Progress1 && GameManager_Progress2);

    GameManager_ProcessTick(false);
}''', '''void GameManager_ProgressTurn() {
    const uint64_t loop_start = GameManager_ProfNow();

    do {
        GameManager_Progress2 = true;
        const uint64_t input_start = GameManager_ProfNow();
        GameManager_ProcessInput();
        GameManager_Prof.input_ns += GameManager_ProfNow() - input_start;
    } while (GameManager_Progress1 && GameManager_Progress2);

    GameManager_ProcessTick(false);
    GameManager_Prof.loop_ns += GameManager_ProfNow() - loop_start;
}''')

patch('game_manager.cpp', '''    if (GameManager_IsTickDue()) {
        if (GameManager_IsCheater && GameManager_PlayMode != PLAY_MODE_UNKNOWN) {
            GameManager_PunishCheater();
        }
''', '''    if (GameManager_IsTickDue()) {
        const uint64_t busy_start = GameManager_ProfNow();
        ++GameManager_Prof.ticks;

        if (GameManager_IsCheater && GameManager_PlayMode != PLAY_MODE_UNKNOWN) {
            GameManager_PunishCheater();
        }
''')

patch('game_manager.cpp', '''        if (GameManager_GameState != GAME_STATE_11_TURN_ACTIVE) {
            UnitsManager_ProcessOrders();
        }

        if (GameManager_IsTickRenderDue()) {
            GameManager_RenderMap();
        }

        if (GameManager_GameState != GAME_STATE_11_TURN_ACTIVE && !UnitsManager_OrdersPending) {
            TickTimer_UpdateTimeLimit();

            if (GameManager_UpdateFlag) {
                Ai_CheckReactions();
            }

            GameManager_UpdateFlag = !GameManager_UpdateFlag;

            ResourceManager_GetPathsManager().DispatchJobs();
            Ai_CheckReactions();
            Ai_CheckEndTurn();
        }

        result = true;
''', '''        uint64_t t0 = GameManager_ProfNow();

        if (GameManager_GameState != GAME_STATE_11_TURN_ACTIVE) {
            UnitsManager_ProcessOrders();
        }

        uint64_t t1 = GameManager_ProfNow();
        GameManager_Prof.orders_ns += t1 - t0;

        if (GameManager_IsTickRenderDue()) {
            GameManager_RenderMap();
        }

        uint64_t t2 = GameManager_ProfNow();
        GameManager_Prof.render_ns += t2 - t1;

        if (GameManager_GameState != GAME_STATE_11_TURN_ACTIVE && !UnitsManager_OrdersPending) {
            TickTimer_UpdateTimeLimit();

            if (GameManager_UpdateFlag) {
                Ai_CheckReactions();
            }

            GameManager_UpdateFlag = !GameManager_UpdateFlag;

            ResourceManager_GetPathsManager().DispatchJobs();
            Ai_CheckReactions();
            Ai_CheckEndTurn();
        }

        GameManager_Prof.ai_ns += GameManager_ProfNow() - t2;
        GameManager_Prof.busy_ns += GameManager_ProfNow() - busy_start;

        result = true;
''')

patch('game_manager.cpp', '''    } else {
        if (render_screen || GameManager_RenderEnable || GameManager_GridOffset.x || GameManager_GridOffset.y) {
            GameManager_RenderMap();
        }
''', '''    } else {
        ++GameManager_Prof.idle_calls;

        if (render_screen || GameManager_RenderEnable || GameManager_GridOffset.x || GameManager_GridOffset.y) {
            GameManager_RenderMap();
        }
''')

patch('commandline.cpp', '''static CommandLine_Options CommandLine_Values;
''', '''static CommandLine_Options CommandLine_Values;

struct GameManager_ProfData {
    uint64_t ticks, busy_ns, orders_ns, render_ns, ai_ns, input_ns, loop_ns, timeouts, idle_calls;
};
extern GameManager_ProfData GameManager_Prof;
''')

patch('commandline.cpp', '''    SDL_Log("Command line: %s", line.c_str());
}

bool CommandLine_LogResult''', '''    SDL_Log("Command line: %s", line.c_str());

    {
        static uint64_t last_ns = SDL_GetTicksNS();
        const uint64_t now = SDL_GetTicksNS();
        auto& p = GameManager_Prof;
        auto s = [](uint64_t ns) { return static_cast<double>(ns) / 1e9; };

        SDL_Log("Prof turn %d: wall %.2f ticks %llu busy %.2f (orders %.2f render %.2f ai %.2f) input %.2f "
                "loop %.2f idle_calls %llu timeouts %llu",
                turn, s(now - last_ns), (unsigned long long)p.ticks, s(p.busy_ns), s(p.orders_ns), s(p.render_ns),
                s(p.ai_ns), s(p.input_ns), s(p.loop_ns), (unsigned long long)p.idle_calls,
                (unsigned long long)p.timeouts);

        p = {};
        last_ns = now;
    }
}

bool CommandLine_LogResult''')

print('instrumented')
