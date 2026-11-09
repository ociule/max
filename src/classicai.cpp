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

#include "classicai.hpp"

#include "ai.hpp"
#include "aiplayer.hpp"
#include "resource_manager.hpp"

bool ClassicAi::SetupStrategy() { return Ai_SetupStrategy(m_team); }

void ClassicAi::BeginTurn() { AiPlayer_Teams[m_team].BeginTurn(); }

bool ClassicAi::CheckEndTurn() { return AiPlayer_Teams[m_team].CheckEndTurn(); }

void ClassicAi::SetInfoMapPoint(Point point) { AiPlayer_Teams[m_team].SetInfoMapPoint(point); }

void ClassicAi::MarkMineMapPoint(Point point) { AiPlayer_Teams[m_team].MarkMineMapPoint(point); }

void ClassicAi::UpdateMineMap(Point point) { AiPlayer_Teams[m_team].UpdateMineMap(point); }

void ClassicAi::TasksPending(const char* reason) { AiPlayer_Teams[m_team].ChangeTasksPendingFlag(true); }

void ClassicAi::UnitSpotted(UnitInfo* unit) { AiPlayer_Teams[m_team].UnitSpotted(unit); }

void ClassicAi::UnitHidden(UnitInfo* unit) { AiPlayer_Teams[m_team].UnitHidden(unit); }

void ClassicAi::RemoveUnit(UnitInfo* unit) { AiPlayer_Teams[m_team].RemoveUnit(unit); }

void ClassicAi::FileSave(SmartFileWriter& file) { AiPlayer_Teams[m_team].FileSave(file); }

void ClassicAi::FileLoad(SmartFileReader& file) { AiPlayer_Teams[m_team].FileLoad(file); }

int32_t ClassicAi::GetExploredCells() const {
    uint8_t** info_map = AiPlayer_Teams[m_team].GetInfoMap();
    int32_t result = 0;

    if (info_map) {
        for (int32_t x = 0; x < ResourceManager_MapSize.x; ++x) {
            for (int32_t y = 0; y < ResourceManager_MapSize.y; ++y) {
                if (info_map[x][y] & INFO_MAP_EXPLORED) {
                    ++result;
                }
            }
        }
    }

    return result;
}
