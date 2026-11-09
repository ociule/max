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

#include "aicontroller.hpp"

#include <array>

#include "classicai.hpp"
#include "game_manager.hpp"
#include "jevai.hpp"
#include "noneai.hpp"
#include "policyai.hpp"
#include "units_manager.hpp"

static std::array<ClassicAi, PLAYER_TEAM_MAX - 1> AiController_ClassicTeams{
    ClassicAi(PLAYER_TEAM_RED), ClassicAi(PLAYER_TEAM_GREEN), ClassicAi(PLAYER_TEAM_BLUE), ClassicAi(PLAYER_TEAM_GRAY)};

static std::array<NoneAi, PLAYER_TEAM_MAX - 1> AiController_NoneTeams{
    NoneAi(PLAYER_TEAM_RED), NoneAi(PLAYER_TEAM_GREEN), NoneAi(PLAYER_TEAM_BLUE), NoneAi(PLAYER_TEAM_GRAY)};

static std::array<JevAi, PLAYER_TEAM_MAX - 1> AiController_JevTeams{JevAi(PLAYER_TEAM_RED), JevAi(PLAYER_TEAM_GREEN),
                                                                    JevAi(PLAYER_TEAM_BLUE), JevAi(PLAYER_TEAM_GRAY)};

static std::array<AiControllerType, PLAYER_TEAM_MAX - 1> AiController_Types{
    AI_CONTROLLER_CLASSIC, AI_CONTROLLER_CLASSIC, AI_CONTROLLER_CLASSIC, AI_CONTROLLER_CLASSIC};

static std::array<PolicyAi, PLAYER_TEAM_MAX - 1> AiController_PolicyTeams{
    PolicyAi(PLAYER_TEAM_RED), PolicyAi(PLAYER_TEAM_GREEN), PolicyAi(PLAYER_TEAM_BLUE), PolicyAi(PLAYER_TEAM_GRAY)};

AiController* AiController_Get(uint16_t team) {
    AiController* result;

    if (team < AiController_Types.size() && UnitsManager_TeamInfo[team].team_type == TEAM_TYPE_COMPUTER) {
        switch (AiController_Types[team]) {
            case AI_CONTROLLER_POLICY: {
                result = &AiController_PolicyTeams[team];
            } break;
            case AI_CONTROLLER_NONE: {
                result = &AiController_NoneTeams[team];
            } break;

            case AI_CONTROLLER_JEV: {
                result = &AiController_JevTeams[team];
            } break;

            default: {
                result = &AiController_ClassicTeams[team];
            } break;
        }

    } else {
        result = nullptr;
    }

    return result;
}

void AiController_SetType(uint16_t team, AiControllerType type) {
    if (team < AiController_Types.size()) {
        AiController_Types[team] = type;
    }
}

bool AiController_IsClassic(uint16_t team) {
    return team < AiController_Types.size() && UnitsManager_TeamInfo[team].team_type == TEAM_TYPE_COMPUTER &&
           AiController_Types[team] == AI_CONTROLLER_CLASSIC;
}

bool AiController_EndTurn(uint16_t team) {
    bool is_demo = true;

    for (int32_t i = PLAYER_TEAM_RED; i < PLAYER_TEAM_MAX - 1; ++i) {
        if (UnitsManager_TeamInfo[i].team_type == TEAM_TYPE_PLAYER ||
            UnitsManager_TeamInfo[i].team_type == TEAM_TYPE_REMOTE) {
            is_demo = false;
        }
    }

    bool result;

    // same condition as AiPlayer::CheckEndTurn
    if (!UnitsManager_TeamInfo[team].finished_turn &&
        (GameManager_PlayMode == PLAY_MODE_TURN_BASED || GameManager_GameState == GAME_STATE_9_END_TURN || is_demo)) {
        GameManager_RefreshOrders(team, true);

        result = true;

    } else {
        result = false;
    }

    return result;
}
