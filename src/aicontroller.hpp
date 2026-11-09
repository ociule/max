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

#ifndef AICONTROLLER_HPP
#define AICONTROLLER_HPP

#include <cstdint>

#include "point.hpp"

class UnitInfo;
class SmartFileReader;
class SmartFileWriter;

/**
 * \brief Decision maker for one computer team.
 *
 * Ai_* functions in ai.cpp forward to the controller of each team that has one. The shared reaction scheduler, the
 * task manager and the terrain distance field are not part of a controller and still run once for all teams.
 */
class AiController {
public:
    virtual ~AiController() = default;

    virtual bool SetupStrategy() = 0;
    virtual void BeginTurn() = 0;

    /**
     * \brief Advances the team's end of turn processing.
     *
     * \return True if work was done this call. This is not a "turn is over" test. In simultaneous mode the caller
     * stops after the first team that returns true.
     */
    virtual bool CheckEndTurn() = 0;

    virtual void SetInfoMapPoint(Point point) = 0;
    virtual void MarkMineMapPoint(Point point) = 0;
    virtual void UpdateMineMap(Point point) = 0;

    /**
     * \brief Wake up notice. Sent when a unit starts, changes or finishes an action, with a short reason.
     */
    virtual void TasksPending(const char* reason) = 0;

    virtual void UnitSpotted(UnitInfo* unit) = 0;
    virtual void RemoveUnit(UnitInfo* unit) = 0;

    virtual void FileSave(SmartFileWriter& file) = 0;
    virtual void FileLoad(SmartFileReader& file) = 0;

    /**
     * \brief Returns how many map cells the team has explored, or -1 if the controller does not track it.
     */
    virtual int32_t GetExploredCells() const { return -1; }
};

enum AiControllerType : uint8_t {
    /// The original computer player.
    AI_CONTROLLER_CLASSIC,
    /// Does nothing and ends its turns. A baseline for comparisons.
    AI_CONTROLLER_NONE,
    /// Moves the team's mobile land units with the Jev decision model.
    AI_CONTROLLER_JEV,
    /// An external policy receives player observations and issues engine commands.
    AI_CONTROLLER_POLICY,
};

/**
 * \brief Returns the controller of a team, or nullptr if the team is not run by the computer.
 *
 * Follows the team's current type, so a team gains or loses its controller as soon as its type changes.
 */
AiController* AiController_Get(uint16_t team);

/**
 * \brief Selects the controller a team uses while it is a computer team. Every team starts with the classic one.
 *
 * Not saved. A loaded game uses the controllers selected when it is loaded.
 */
void AiController_SetType(uint16_t team, AiControllerType type);

/**
 * \brief Returns true if the classic computer player runs the team.
 *
 * The task manager, reaction checks and other classic AI plumbing only work for these teams.
 */
bool AiController_IsClassic(uint16_t team);

/**
 * \brief Ends a team's turn the way the classic computer player does, once the game allows it.
 *
 * \return True if the turn was ended by this call.
 */
bool AiController_EndTurn(uint16_t team);

#endif /* AICONTROLLER_HPP */
