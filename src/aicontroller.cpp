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
#include "units_manager.hpp"

static std::array<ClassicAi, PLAYER_TEAM_MAX - 1> AiController_ClassicTeams{
    ClassicAi(PLAYER_TEAM_RED), ClassicAi(PLAYER_TEAM_GREEN), ClassicAi(PLAYER_TEAM_BLUE), ClassicAi(PLAYER_TEAM_GRAY)};

AiController* AiController_Get(uint16_t team) {
    AiController* result;

    if (team < AiController_ClassicTeams.size() && UnitsManager_TeamInfo[team].team_type == TEAM_TYPE_COMPUTER) {
        result = &AiController_ClassicTeams[team];

    } else {
        result = nullptr;
    }

    return result;
}
