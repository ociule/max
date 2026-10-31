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

#ifndef COMMANDLINE_HPP
#define COMMANDLINE_HPP

#include <cstdint>

struct WinLoss_Status;

/**
 * \brief Reads the program arguments.
 *
 * \return -1 to continue starting the game, otherwise the exit code to quit with (after --help or a bad option).
 */
int32_t CommandLine_Parse(int argc, char* argv[]);

/**
 * \brief Returns true if the arguments asked for a custom game to start without the menus.
 */
bool CommandLine_IsGame();

/**
 * \brief Starts the custom game described by the arguments and returns when it is over.
 */
void CommandLine_RunGame();

/**
 * \brief Returns the --seed value if one was given, otherwise default_seed.
 */
uint32_t CommandLine_GetSeed(uint32_t default_seed);

/**
 * \brief Logs one line with each active team's units, buildings and points. Does nothing in a menu-started game.
 */
void CommandLine_LogTurn(int32_t turn);

/**
 * \brief Logs the final ranking of a command line game.
 *
 * \return True in a command line game, where the caller skips the game over screen.
 */
bool CommandLine_LogResult(const WinLoss_Status& status, int32_t turn);

#endif /* COMMANDLINE_HPP */
