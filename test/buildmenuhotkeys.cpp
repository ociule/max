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

#include "buildmenuhotkeys.hpp"

#include <gtest/gtest.h>

TEST(BuildMenuHotkeys, Initials) {
    EXPECT_EQ(BuildMenuHotkeys::Initials("Connector"), "c");
    EXPECT_EQ(BuildMenuHotkeys::Initials("Power Generator"), "pg");
    EXPECT_EQ(BuildMenuHotkeys::Initials(" Anti-Aircraft "), "aa");
    EXPECT_EQ(BuildMenuHotkeys::Initials("Eco-Sphere"), "es");
    EXPECT_EQ(BuildMenuHotkeys::Initials("Water  Platform"), "wp");
}

TEST(BuildMenuHotkeys, SelectOnlyAfterCompleteSequence) {
    BuildMenuHotkeys keys;
    keys.Add("Connector");
    keys.Add("Power Generator");
    keys.Add("Power Station");

    EXPECT_EQ(keys.Match('C', 1), 0);
    EXPECT_EQ(keys.Match('p', 0), -1);
    EXPECT_EQ(keys.Match('G', 0), 1);
    EXPECT_EQ(keys.Match('P', 1), -1);
    EXPECT_EQ(keys.Match('s', 1), 2);
}

TEST(BuildMenuHotkeys, NewShortcutRecoversFromIncompleteOrInvalidSequence) {
    BuildMenuHotkeys keys;
    keys.Add("Connector");
    keys.Add("Power Generator");

    EXPECT_EQ(keys.Match('p', 0), -1);
    EXPECT_EQ(keys.Match('c', 1), 0);
    EXPECT_EQ(keys.Match('z', 0), -1);
    EXPECT_EQ(keys.Match('g', 0), -1);
    EXPECT_EQ(keys.Match('p', 0), -1);
    EXPECT_EQ(keys.Match('g', 0), 1);
}

TEST(BuildMenuHotkeys, NavigationAndMouseEventsResetPendingSequence) {
    BuildMenuHotkeys keys;
    keys.Add("Power Generator");

    for (int command : {13, 27, 328, 336, 2002, 10013}) {
        EXPECT_EQ(keys.Match('p', 0), -1);
        EXPECT_EQ(keys.Match(command, 0), -1);
        EXPECT_EQ(keys.Match('g', 0), -1);
    }
    EXPECT_EQ(keys.Match('p', 0), -1);
    EXPECT_EQ(keys.Match('g', 0), 0);
}

TEST(BuildMenuHotkeys, SharedInitialsCycleAndShortNamesRemainReachable) {
    BuildMenuHotkeys keys;
    keys.Add("Radar");
    keys.Add("Repair Depot");
    keys.Add("Research Depot");

    EXPECT_EQ(keys.Match('r', 0), 0);
    EXPECT_EQ(keys.Match('d', 0), 1);
    EXPECT_EQ(keys.Match('r', 1), 0);
    EXPECT_EQ(keys.Match('d', 0), 2);
    EXPECT_EQ(keys.Match('r', 2), 0);
    EXPECT_EQ(keys.Match('d', 0), 1);
}

TEST(BuildMenuHotkeys, RadarAndRoadShareR) {
    BuildMenuHotkeys keys;
    keys.Add("Connector");
    keys.Add("Radar");
    keys.Add("Road");

    EXPECT_EQ(keys.Match('r', 0), 1);
    EXPECT_EQ(keys.Match('r', 1), 2);
    EXPECT_EQ(keys.Match('r', 2), 1);

    EXPECT_EQ(keys.Match('a', 1), 1);
    EXPECT_EQ(keys.Match('d', 1), 1);
    EXPECT_EQ(keys.Match(13, 1), -1);
    EXPECT_EQ(keys.Match('R', 1), 2);
    EXPECT_EQ(keys.Match('O', 2), 2);
    EXPECT_EQ(keys.Match('a', 2), 2);
    EXPECT_EQ(keys.Match('d', 2), 2);
}

TEST(BuildMenuHotkeys, LongerNamePrefixesDisambiguateAndInitialsTakePriority) {
    BuildMenuHotkeys keys;
    keys.Add("Hangar");
    keys.Add("Habitat");
    keys.Add("Heavy Vehicle Plant");
    keys.Add("Gold Refinery");
    keys.Add("Greenhouse");

    EXPECT_EQ(keys.Match('h', 2), 0);
    EXPECT_EQ(keys.Match('a', 0), -1);
    EXPECT_EQ(keys.Match('b', 0), 1);
    EXPECT_EQ(keys.Match('h', 1), 0);
    EXPECT_EQ(keys.Match('a', 0), -1);
    EXPECT_EQ(keys.Match('n', 0), 0);
    EXPECT_EQ(keys.Match('h', 0), 1);
    EXPECT_EQ(keys.Match('v', 1), -1);
    EXPECT_EQ(keys.Match('p', 1), 2);
    EXPECT_EQ(keys.Match('g', 2), 4);
    EXPECT_EQ(keys.Match('r', 4), 3);
}

TEST(BuildMenuHotkeys, EmptyMenuAndReset) {
    BuildMenuHotkeys keys;
    EXPECT_EQ(keys.Match('c', -1), -1);
    keys.Add("Power Generator");
    EXPECT_EQ(keys.Match('p', 0), -1);
    keys.Reset();
    EXPECT_EQ(keys.Match('g', 0), -1);
}
