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

#include "commandline.hpp"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

#include "aicontroller.hpp"
#include "game_manager.hpp"
#include "menu.hpp"
#include "missionmanager.hpp"
#include "resource_manager.hpp"
#include "settings.hpp"
#include "task.hpp"
#include "units_manager.hpp"
#include "winloss.hpp"

struct CommandLine_Options {
    bool is_game{false};
    bool hidden{false};
    bool has_seed{false};
    uint32_t seed{0};
    int32_t world{0};
    int32_t opponent{OPPONENT_TYPE_AVERAGE};
    int32_t play_mode{PLAY_MODE_TURN_BASED};
    int32_t turns{100};
    int32_t team_types[PLAYER_TEAM_MAX - 1]{TEAM_TYPE_COMPUTER, TEAM_TYPE_COMPUTER, TEAM_TYPE_NONE, TEAM_TYPE_NONE};
    AiControllerType controllers[PLAYER_TEAM_MAX - 1]{AI_CONTROLLER_CLASSIC, AI_CONTROLLER_CLASSIC,
                                                      AI_CONTROLLER_CLASSIC, AI_CONTROLLER_CLASSIC};
};

static CommandLine_Options CommandLine_Values;

static const char* const CommandLine_TeamNames[PLAYER_TEAM_MAX - 1] = {"red", "green", "blue", "gray"};

static const char* const CommandLine_ControllerNames[] = {"classic", "none", "jev"};

static const char* const CommandLine_Usage =
    "Usage: max-port [options]\n"
    "Without options the game starts with the menus. With options it starts a custom game right away.\n"
    "\n"
    "  --teams LIST       player type of red, green, blue and gray: computer, player or none.\n"
    "                     Missing teams are none. Default: computer,computer\n"
    "  --controllers LIST computer player of red, green, blue and gray: classic, none (does nothing)\n"
    "                     or jev (moves its land units with the Jev decision model, see\n"
    "                     tools/jev_bridge.py). Missing teams are classic. Default: classic\n"
    "  --world N          planet, 0 to 23, in the order of the planet menu. Default: 0\n"
    "  --opponent N       computer skill, 0 (clueless) to 5 (god). Default: 2 (average)\n"
    "  --play-mode MODE   turn-based or simultaneous. Default: turn-based\n"
    "  --turns N          the game ends after N turns. Default: 100\n"
    "  --seed N           random seed for the map and the computer players. Default: the time. It does\n"
    "                     not make the game repeatable, as computer players think for a fixed time.\n"
    "  --hidden           no window and no sound.\n"
    "  --help             show this text.\n"
    "\n"
    "A command line game logs one line per turn and the final ranking to the game log. It does not\n"
    "change settings.json and does not autosave.\n";

static bool CommandLine_ParseNumber(const std::string& text, int32_t min, int32_t max, int32_t& value) {
    char* end;
    const long number = std::strtol(text.c_str(), &end, 10);

    if (text.empty() || *end != '\0' || number < min || number > max) {
        return false;
    }

    value = static_cast<int32_t>(number);

    return true;
}

static bool CommandLine_ParseTeams(const std::string& text, int32_t* team_types) {
    size_t start = 0;
    int32_t team = 0;
    int32_t active_teams = 0;

    for (; team < PLAYER_TEAM_MAX - 1 && start <= text.size(); ++team) {
        size_t end = text.find(',', start);

        if (end == std::string::npos) {
            end = text.size();
        }

        const std::string type = text.substr(start, end - start);

        if (type == "computer") {
            team_types[team] = TEAM_TYPE_COMPUTER;

        } else if (type == "player") {
            team_types[team] = TEAM_TYPE_PLAYER;

        } else if (type == "none") {
            team_types[team] = TEAM_TYPE_NONE;

        } else {
            return false;
        }

        if (team_types[team] != TEAM_TYPE_NONE) {
            ++active_teams;
        }

        start = end + 1;
    }

    if (start <= text.size()) {
        return false;
    }

    for (; team < PLAYER_TEAM_MAX - 1; ++team) {
        team_types[team] = TEAM_TYPE_NONE;
    }

    return active_teams >= 2;
}

static bool CommandLine_ParseControllers(const std::string& text, AiControllerType* controllers) {
    size_t start = 0;

    for (int32_t team = 0; team < PLAYER_TEAM_MAX - 1 && start <= text.size(); ++team) {
        size_t end = text.find(',', start);

        if (end == std::string::npos) {
            end = text.size();
        }

        const std::string name = text.substr(start, end - start);
        bool found = false;

        for (uint8_t type = AI_CONTROLLER_CLASSIC; type <= AI_CONTROLLER_JEV; ++type) {
            if (name == CommandLine_ControllerNames[type]) {
                controllers[team] = static_cast<AiControllerType>(type);
                found = true;
            }
        }

        if (!found) {
            return false;
        }

        start = end + 1;
    }

    return start > text.size();
}

int32_t CommandLine_Parse(int argc, char* argv[]) {
    auto& options = CommandLine_Values;

    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        const std::string value = (i + 1 < argc) ? argv[i + 1] : "";
        bool valid = true;
        int32_t number{0};

        if (option == "--help") {
            std::fputs(CommandLine_Usage, stdout);

            return EXIT_SUCCESS;

        } else if (option == "--hidden") {
            options.hidden = true;

        } else if (option == "--teams") {
            valid = CommandLine_ParseTeams(value, options.team_types);
            ++i;

        } else if (option == "--controllers") {
            valid = CommandLine_ParseControllers(value, options.controllers);
            ++i;

        } else if (option == "--world") {
            valid = CommandLine_ParseNumber(value, 0, 23, options.world);
            ++i;

        } else if (option == "--opponent") {
            valid = CommandLine_ParseNumber(value, OPPONENT_TYPE_CLUELESS, OPPONENT_TYPE_GOD, options.opponent);
            ++i;

        } else if (option == "--play-mode") {
            if (value == "turn-based") {
                options.play_mode = PLAY_MODE_TURN_BASED;

            } else if (value == "simultaneous") {
                options.play_mode = PLAY_MODE_SIMULTANEOUS_MOVES;

            } else {
                valid = false;
            }

            ++i;

        } else if (option == "--turns") {
            valid = CommandLine_ParseNumber(value, 1, 9999, options.turns);
            ++i;

        } else if (option == "--seed") {
            valid = CommandLine_ParseNumber(value, 0, INT32_MAX, number);
            options.seed = number;
            options.has_seed = true;
            ++i;

        } else {
            std::fprintf(stderr, "Unknown option %s.\n\n%s", option.c_str(), CommandLine_Usage);

            return EXIT_FAILURE;
        }

        if (!valid) {
            std::fprintf(stderr, "Invalid value \"%s\" for %s.\n\n%s", value.c_str(), option.c_str(),
                         CommandLine_Usage);

            return EXIT_FAILURE;
        }

        options.is_game = true;
    }

    return -1;
}

bool CommandLine_IsGame() { return CommandLine_Values.is_game; }

static SDL_AssertState SDLCALL CommandLine_AssertionHandler(const SDL_AssertData* data, void* userdata) {
    // nobody can answer a dialog in a hidden game
    SDL_Log("Command line: assertion failed at %s (%s:%d): %s", data->function, data->filename, data->linenum,
            data->condition);

    return SDL_ASSERTION_ALWAYS_IGNORE;
}

void CommandLine_RunGame() {
    auto& options = CommandLine_Values;
    const auto settings = ResourceManager_GetSettings();

    settings->DisableSaving();

    if (options.hidden) {
        int count;
        SDL_Window** windows = SDL_GetWindows(&count);

        if (windows) {
            for (int i = 0; i < count; ++i) {
                SDL_HideWindow(windows[i]);
            }

            SDL_free(windows);
        }

        SDL_SetAssertionHandler(CommandLine_AssertionHandler, nullptr);

        settings->SetNumericValue("disable_music", 1);
        settings->SetNumericValue("disable_fx", 1);
        settings->SetNumericValue("disable_voice", 1);
    }

    if (!options.has_seed) {
        options.seed = time(nullptr);
        options.has_seed = true;
    }

    if (!ResourceManager_GetMissionManager()->LoadMission(MISSION_CATEGORY_CUSTOM, "MISSION_CATEGORY_CUSTOM")) {
        SDL_Log("Command line: cannot load the custom game mission.");

        return;
    }

    // The same settings the custom game menus set.
    settings->SetNumericValue("world", options.world);
    settings->SetNumericValue("opponent", options.opponent);
    settings->SetNumericValue("play_mode", options.play_mode);
    settings->SetNumericValue("victory_type", VICTORY_TYPE_DURATION);
    settings->SetNumericValue("victory_limit", options.turns);
    settings->SetNumericValue("auto_save", 0);
    menu_update_resource_levels();

    ini_setting_victory_type = VICTORY_TYPE_DURATION;
    ini_setting_victory_limit = options.turns;

    for (int32_t team = PLAYER_TEAM_RED; team < PLAYER_TEAM_MAX - 1; ++team) {
        settings->SetNumericValue(menu_team_player_setting[team], options.team_types[team]);
        settings->SetNumericValue(menu_team_clan_setting[team], settings->GetNumericValue("player_clan"));
        settings->SetStringValue(menu_team_name_setting[team], (options.team_types[team] == TEAM_TYPE_PLAYER)
                                                                   ? settings->GetStringValue("player_name")
                                                                   : CommandLine_TeamNames[team]);
    }

    std::string controllers;

    for (int32_t team = PLAYER_TEAM_RED; team < PLAYER_TEAM_MAX - 1; ++team) {
        AiController_SetType(team, options.controllers[team]);

        if (options.team_types[team] == TEAM_TYPE_COMPUTER) {
            controllers += std::string(" ") + CommandLine_TeamNames[team] + " " +
                           CommandLine_ControllerNames[options.controllers[team]] + ";";
        }
    }

    SDL_Log("Command line: computer players:%s", controllers.c_str());
    SDL_Log("Command line: world %d, opponent %d, %s, %d turns, seed %u.", options.world, options.opponent,
            (options.play_mode == PLAY_MODE_TURN_BASED) ? "turn-based" : "simultaneous", options.turns, options.seed);

    GameManager_GameLoop(GAME_STATE_6_GAME_SETUP);
}

uint32_t CommandLine_GetSeed(uint32_t default_seed) {
    return CommandLine_Values.has_seed ? CommandLine_Values.seed : default_seed;
}

void CommandLine_LogTurn(int32_t turn) {
    if (!CommandLine_Values.is_game) {
        return;
    }

    int32_t units[PLAYER_TEAM_MAX]{};
    int32_t buildings[PLAYER_TEAM_MAX]{};
    int32_t tasked[PLAYER_TEAM_MAX]{};

    for (auto* list : {&UnitsManager_MobileLandSeaUnits, &UnitsManager_MobileAirUnits}) {
        for (auto it = list->Begin(), it_end = list->End(); it != it_end; ++it) {
            ++units[(*it).team];

            if ((*it).GetTask()) {
                ++tasked[(*it).team];
            }
        }
    }

    for (auto it = UnitsManager_StationaryUnits.Begin(), it_end = UnitsManager_StationaryUnits.End(); it != it_end;
         ++it) {
        ++buildings[(*it).team];
    }

    std::string line = "Turn " + std::to_string(turn) + ":";

    for (int32_t team = PLAYER_TEAM_RED; team < PLAYER_TEAM_MAX - 1; ++team) {
        if (UnitsManager_TeamInfo[team].team_type != TEAM_TYPE_NONE) {
            line += std::string(" ") + CommandLine_TeamNames[team] + " units " + std::to_string(units[team]) + " (" +
                    std::to_string(tasked[team]) + " with classic tasks) buildings " + std::to_string(buildings[team]) +
                    " points " + std::to_string(UnitsManager_TeamInfo[team].team_points);

            AiController* controller = AiController_Get(team);

            if (controller && controller->GetExploredCells() >= 0) {
                line += " explored " + std::to_string(controller->GetExploredCells());
            }

            line += ";";
        }
    }

    SDL_Log("Command line: %s", line.c_str());
}

bool CommandLine_LogResult(const WinLoss_Status& status, int32_t turn) {
    if (!CommandLine_Values.is_game) {
        return false;
    }

    std::string line = "Game over on turn " + std::to_string(turn) + ":";

    for (int32_t team = PLAYER_TEAM_RED; team < PLAYER_TEAM_MAX - 1; ++team) {
        if (UnitsManager_TeamInfo[team].team_type != TEAM_TYPE_NONE) {
            line += std::string(" ") + CommandLine_TeamNames[team] + " rank " + std::to_string(status.team_rank[team]) +
                    ";";
        }
    }

    SDL_Log("Command line: %s", line.c_str());

    return true;
}
