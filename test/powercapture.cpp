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

#include "powercapture.hpp"

#include <gtest/gtest.h>

using PowerCapture::Structure;

static std::vector<uint16_t> PlanOwners(const std::vector<PowerCapture::Structure>& structures,
                                        const std::vector<uint16_t>& completed_units) {
    return PowerCapture::Plan(structures, completed_units).owners;
}

TEST(PowerCapture, ConnectorCapturesAnEntireIsolatedComplex) {
    std::vector<Structure> map{
        {1, 0, 0, 0, 0, 1, true, true},   {2, 0, 0, 1, 0, 1, false, false}, {3, 1, 1, 2, 0, 2, false, false},
        {4, 1, 1, 4, 0, 1, false, false}, {5, 1, 1, 5, 0, 2, false, false},
    };
    EXPECT_EQ(PlanOwners(map, {2}), (std::vector<uint16_t>{0, 0, 0, 0, 0}));
}

TEST(PowerCapture, InactiveIsolatedPowerStationOrGeneratorIsCapturable) {
    for (int32_t size : {1, 2}) {
        std::vector<Structure> map{
            {1, 0, 0, 0, 0, 1, true, true},
            {2, 0, 0, 1, 0, 1, false, false},
            {3, 1, 1, 2, 0, size, true, false},
        };
        EXPECT_EQ(PlanOwners(map, {2}), (std::vector<uint16_t>{0, 0, 0}));
    }
}

TEST(PowerCapture, OnlyActiveEnemySourcesProtectTheirComplex) {
    for (bool on : {false, true}) {
        std::vector<Structure> map{
            {1, 0, 0, 0, 0, 1, true, true},
            {2, 1, 1, 1, 0, 2, false, false},
            {3, 1, 1, 3, 0, 1, true, on},
        };
        const uint16_t owner = on ? 1 : 0;
        EXPECT_EQ(PlanOwners(map, {1}), (std::vector<uint16_t>{0, owner, owner}));
    }
}

TEST(PowerCapture, CapturingTeamNeedsAnOperatingSource) {
    std::vector<Structure> map{
        {1, 0, 0, 0, 0, 1, true, false},
        {2, 1, 1, 1, 0, 2, false, false},
    };
    EXPECT_EQ(PlanOwners(map, {1}), (std::vector<uint16_t>{0, 1}));
    map[0].power_on = true;
    EXPECT_EQ(PlanOwners(map, {1}), (std::vector<uint16_t>{0, 0}));
}

TEST(PowerCapture, BuildingsConnectAlongEveryEdgeButNeverDiagonally) {
    for (auto [x, y] : {std::pair{-1, 0}, {-1, 1}, {0, -1}, {1, -1}, {2, 0}, {2, 1}, {0, 2}, {1, 2}}) {
        std::vector<Structure> map{
            {1, 0, 0, 0, 0, 2, true, true},
            {2, 1, 1, x, y, 1, false, false},
        };
        EXPECT_EQ(PlanOwners(map, {1})[1], 0) << x << ',' << y;
    }
    for (auto [x, y] : {std::pair{-1, -1}, {-1, 2}, {2, -1}, {2, 2}}) {
        std::vector<Structure> map{
            {1, 0, 0, 0, 0, 2, true, true},
            {2, 1, 1, x, y, 1, false, false},
        };
        EXPECT_EQ(PlanOwners(map, {1})[1], 1) << x << ',' << y;
    }
}

TEST(PowerCapture, BuildingToBuildingCaptureWorksForEitherPlayer) {
    for (uint16_t team : {0, 1, 2, 3}) {
        const uint16_t enemy = (team + 1) % 4;
        std::vector<Structure> map{
            {1, team, 0, 0, 0, 2, true, true},
            {2, enemy, 1, 2, 1, 2, false, false},
        };
        EXPECT_EQ(PlanOwners(map, {1}), (std::vector<uint16_t>{team, team}));
    }
}

TEST(PowerCapture, CaptureStopsAtTheContactedEnemyComplex) {
    std::vector<Structure> map{
        {1, 0, 0, 0, 0, 1, true, true},   {2, 1, 1, 1, 0, 1, false, false}, {3, 0, 2, 2, 0, 2, false, false},
        {4, 2, 3, 4, 0, 2, false, false}, {5, 3, 4, 6, 0, 2, true, false},  {6, 2, 5, 8, 0, 2, false, false},
    };
    EXPECT_EQ(PlanOwners(map, {1}), (std::vector<uint16_t>{0, 0, 0, 2, 3, 2}));
}

TEST(PowerCapture, AnotherActiveSourceProtectsAnInactiveStationInTheSameComplex) {
    std::vector<Structure> map{
        {1, 0, 0, 0, 0, 1, true, true},
        {2, 1, 1, 1, 0, 2, true, false},
        {3, 1, 1, 3, 0, 1, true, true},
    };
    EXPECT_EQ(PlanOwners(map, {1}), (std::vector<uint16_t>{0, 1, 1}));
    map[2].power_on = false;
    EXPECT_EQ(PlanOwners(map, {1}), (std::vector<uint16_t>{0, 0, 0}));
}

TEST(PowerCapture, MilitaryStructureStillProtectsAnInactivePowerStation) {
    std::vector<Structure> map{
        {1, 0, 0, 0, 0, 1, true, true},
        {2, 1, 1, 1, 0, 2, true, false},
        {3, 1, 1, 3, 0, 1, false, false, true},
    };
    EXPECT_EQ(PlanOwners(map, {1}), (std::vector<uint16_t>{0, 1, 1}));
}

TEST(PowerCapture, RemovingEnemyPowerConnectionExposesOnlyTheDetachedGroup) {
    std::vector<Structure> map{
        {1, 0, 0, 0, 0, 1, true, true},
        {2, 1, 1, 1, 0, 2, false, false},
        {3, 1, 1, 3, 0, 1, false, false},
        {4, 1, 1, 4, 0, 1, true, true},
    };
    EXPECT_EQ(PlanOwners(map, {1}), (std::vector<uint16_t>{0, 1, 1, 1}));
    map.erase(map.begin() + 2);
    map[2].network = 2;
    EXPECT_EQ(PlanOwners(map, {1}), (std::vector<uint16_t>{0, 0, 1}));
}

TEST(PowerCapture, ConnectingPlayerWinsAContestedIsland) {
    std::vector<Structure> map{
        {1, 0, 0, 0, 0, 1, true, true},
        {2, 2, 1, 1, 0, 2, false, false},
        {3, 1, 2, 3, 0, 1, true, true},
    };
    EXPECT_EQ(PlanOwners(map, {3}), (std::vector<uint16_t>{0, 1, 1}));
    EXPECT_EQ(PlanOwners(map, {1}), (std::vector<uint16_t>{0, 0, 1}));
    EXPECT_EQ(PlanOwners(map, {1, 3}), PlanOwners(map, {3, 1}));
    auto expected = PlanOwners(map, {1});
    std::reverse(map.begin(), map.end());
    auto actual = PlanOwners(map, {1});
    std::reverse(actual.begin(), actual.end());
    EXPECT_EQ(expected, actual);
}

TEST(PowerCapture, EmptyMapAndDisconnectedStructuresDoNotCapture) {
    EXPECT_TRUE(PlanOwners({}, {}).empty());
    EXPECT_EQ(PlanOwners({{1, 0, 0, 0, 0, 1, true, true}, {2, 1, 1, 9, 9, 2, false, false}}, {1}),
              (std::vector<uint16_t>{0, 1}));
}

TEST(PowerCapture, CapturesJoinTheExistingPoweredComplex) {
    std::vector<Structure> map{
        {1, 1, 0, 1, 0, 2, false, false},
        {2, 0, 1, 0, 0, 1, true, true},
    };
    const auto plan = PowerCapture::Plan(map, {2});
    EXPECT_EQ(plan.owners, (std::vector<uint16_t>{0, 0}));
    EXPECT_EQ(plan.power_networks, (std::vector<size_t>{1, 1}));
}

TEST(PowerCapture, OneMilitaryStructureProtectsItsEntireComplex) {
    std::vector<Structure> map{
        {1, 0, 0, 0, 0, 1, true, true},
        {2, 1, 1, 1, 0, 2, false, false},
        {3, 1, 1, 3, 0, 1, false, false},
        {4, 1, 1, 4, 0, 1, false, false, true},
    };
    EXPECT_EQ(PlanOwners(map, {1}), (std::vector<uint16_t>{0, 1, 1, 1}));

    // After the military structure is disconnected, only the civilian branch falls.
    map.erase(map.begin() + 2);
    map[2].network = 2;
    EXPECT_EQ(PlanOwners(map, {1}), (std::vector<uint16_t>{0, 0, 1}));
}

TEST(PowerCapture, MilitaryComplexCannotBeCapturedOrUsedToReachAnotherComplex) {
    std::vector<Structure> map{
        {1, 0, 0, 0, 0, 1, true, true},
        {2, 1, 1, 1, 0, 1, false, false, true},
        {3, 2, 2, 2, 0, 2, false, false},
    };
    EXPECT_EQ(PlanOwners(map, {1}), (std::vector<uint16_t>{0, 1, 2}));
}

TEST(PowerCapture, MilitaryStructuresInCapturingComplexDoNotPreventCivilianCapture) {
    std::vector<Structure> map{
        {1, 0, 0, 0, 0, 1, true, true},
        {2, 0, 0, 1, 0, 1, false, false, true},
        {3, 1, 1, 2, 0, 2, false, false},
    };
    EXPECT_EQ(PlanOwners(map, {2}), (std::vector<uint16_t>{0, 0, 0}));
}

TEST(PowerCapture, NoConstructionEventPreservesSavedOwnership) {
    std::vector<Structure> map{
        {1, 0, 0, 0, 0, 1, true, true},
        {2, 1, 1, 1, 0, 1, false, false},
    };
    EXPECT_EQ(PlanOwners(map, {}), (std::vector<uint16_t>{0, 1}));
    EXPECT_EQ(PlanOwners(map, {999}), (std::vector<uint16_t>{0, 1}));
    EXPECT_EQ(PlanOwners(map, {2}), (std::vector<uint16_t>{0, 1}));
    EXPECT_EQ(PlanOwners(map, {1}), (std::vector<uint16_t>{0, 0}));
}

TEST(PowerCapture, ConstructionCapturesOnlyAtItsOwnContactWithTheEnemy) {
    std::vector<Structure> map{
        {1, 0, 0, 0, 0, 1, true, true},   {2, 0, 0, 1, 0, 1, false, false}, {3, 1, 1, 2, 0, 1, false, false},
        {4, 0, 0, 0, 1, 1, false, false}, {5, 1, 2, 0, 2, 1, false, false},
    };
    EXPECT_EQ(PlanOwners(map, {2}), (std::vector<uint16_t>{0, 0, 0, 0, 1}));
    EXPECT_EQ(PlanOwners(map, {4}), (std::vector<uint16_t>{0, 0, 1, 0, 0}));
    EXPECT_EQ(PlanOwners(map, {2, 4}), (std::vector<uint16_t>{0, 0, 0, 0, 0}));
}

TEST(PowerCapture, CapturedLoopCannotReenterTheBaseAndCaptureAnUnrelatedNeighbor) {
    std::vector<Structure> map{
        {1, 0, 0, 0, 0, 1, true, true},   {2, 0, 0, 1, 0, 1, false, false}, {3, 1, 1, 2, 0, 1, false, false},
        {4, 1, 1, 2, 1, 1, false, false}, {5, 1, 1, 1, 1, 1, false, false}, {6, 0, 0, 0, 1, 1, false, false},
        {7, 2, 2, 0, 2, 1, false, false},
    };
    EXPECT_EQ(PlanOwners(map, {2}), (std::vector<uint16_t>{0, 0, 0, 0, 0, 0, 2}));
}

TEST(PowerCapture, NewStructureBetweenUnpoweredOwnersKeepsBothOwners) {
    for (int32_t size : {1, 2}) {
        std::vector<Structure> map{
            {1, 0, 0, 0, 0, 1, true, false},
            {2, 0, 0, 1, 0, size, false, false},
            {3, 1, 1, 1 + size, 0, 1, true, false},
        };
        EXPECT_EQ(PlanOwners(map, {2}), (std::vector<uint16_t>{0, 0, 1}));
        EXPECT_EQ(PowerCapture::Plan(map, {2}).power_networks, (std::vector<size_t>{0, 0, 1}));
    }
}

static PowerCapture::PlanResult PlanPhysicalNetworks(std::vector<Structure> structures,
                                                     const std::vector<uint16_t>& completed_units) {
    const auto networks = PowerCapture::FindNetworks(structures);
    for (size_t i = 0; i < structures.size(); ++i) {
        structures[i].network = networks[i];
    }
    return PowerCapture::Plan(structures, completed_units);
}

TEST(PowerCapture, SharedSavedRecordCannotCaptureOrProtectDisconnectedStructures) {
    for (bool active : {false, true}) {
        std::vector<Structure> map{
            {1, 0, 0, 0, 0, 1, true, true, false, CONNECTOR_EAST_TOP},
            {2, 0, 0, 1, 0, 1, false, false, false, CONNECTOR_WEST_TOP},
            {3, 1, 1, 2, 0, 2, true, false, false, CONNECTOR_SOUTH_RIGHT},
            {4, 1, 1, 3, 2, 1, false, false, false, CONNECTOR_NORTH_LEFT},
            {5, 1, 1, 20, 0, 1, false, false, false, CONNECTOR_EAST_TOP},
            {6, 1, 2, 21, 0, 2, true, active, false, CONNECTOR_WEST_TOP},
        };
        EXPECT_EQ(PowerCapture::FindNetworks(map), (std::vector<size_t>{0, 0, 1, 1, 2, 2}));
        EXPECT_EQ(PlanPhysicalNetworks(map, {2}).owners, (std::vector<uint16_t>{0, 0, 0, 0, 1, 1}));
    }
}

TEST(PowerCapture, LiveMilitaryConnectionProtectsAcrossDifferentSavedRecords) {
    std::vector<Structure> map{
        {1, 0, 0, 0, 0, 1, true, true, false, CONNECTOR_EAST_TOP},
        {2, 0, 0, 1, 0, 1, false, false, false, CONNECTOR_WEST_TOP},
        {3, 1, 1, 2, 0, 2, false, false, false, CONNECTOR_EAST_TOP},
        {4, 1, 2, 4, 0, 1, false, false, false, CONNECTOR_WEST_TOP | CONNECTOR_EAST_TOP},
        {5, 1, 3, 5, 0, 1, false, false, true, CONNECTOR_WEST_TOP},
    };
    EXPECT_EQ(PowerCapture::FindNetworks(map), (std::vector<size_t>{0, 0, 1, 1, 1}));
    EXPECT_EQ(PlanPhysicalNetworks(map, {2}).owners, (std::vector<uint16_t>{0, 0, 1, 1, 1}));
}

TEST(PowerCapture, DisconnectedSourceInSharedSavedRecordCannotPowerCapture) {
    std::vector<Structure> map{
        {1, 0, 0, 0, 0, 1, true, true},
        {2, 0, 0, 10, 0, 1, false, false},
        {3, 1, 1, 11, 0, 2, false, false},
    };
    EXPECT_EQ(PlanPhysicalNetworks(map, {2}).owners, (std::vector<uint16_t>{0, 0, 1}));
}

TEST(PowerCapture, PhysicalNetworksRequireReciprocalFriendlyLinksOnEveryBuildingEdge) {
    struct Contact {
        int32_t x, y;
        uint16_t port, opposite;
    };
    for (const auto& edge : std::vector<Contact>{{2, 1, CONNECTOR_NORTH_LEFT, CONNECTOR_SOUTH_LEFT},
                                                 {3, 1, CONNECTOR_NORTH_RIGHT, CONNECTOR_SOUTH_LEFT},
                                                 {4, 2, CONNECTOR_EAST_TOP, CONNECTOR_WEST_TOP},
                                                 {4, 3, CONNECTOR_EAST_BOTTOM, CONNECTOR_WEST_TOP},
                                                 {2, 4, CONNECTOR_SOUTH_LEFT, CONNECTOR_NORTH_LEFT},
                                                 {3, 4, CONNECTOR_SOUTH_RIGHT, CONNECTOR_NORTH_LEFT},
                                                 {1, 2, CONNECTOR_WEST_TOP, CONNECTOR_EAST_TOP},
                                                 {1, 3, CONNECTOR_WEST_BOTTOM, CONNECTOR_EAST_TOP}}) {
        std::vector<Structure> map{
            {1, 0, 0, 2, 2, 2, true, true, false, edge.port},
            {2, 0, 0, edge.x, edge.y, 1, false, false, false, edge.opposite},
        };
        EXPECT_EQ(PowerCapture::FindNetworks(map), (std::vector<size_t>{0, 0}));
        map[1].connectors = 0;
        EXPECT_EQ(PowerCapture::FindNetworks(map), (std::vector<size_t>{0, 1}));
        map[1].connectors = edge.opposite;
        map[1].team = 1;
        EXPECT_EQ(PowerCapture::FindNetworks(map), (std::vector<size_t>{0, 1}));
    }
}

TEST(PowerCapture, CaptureDoesNotSpreadThroughAnExistingFriendlyConnector) {
    std::vector<Structure> map{
        {1, 0, 0, 0, 0, 1, true, true},    {2, 0, 0, 1, 0, 1, false, false},  {3, 1, 1, 2, 0, 2, false, false},
        {4, 0, 2, 2, -1, 1, false, false}, {5, 1, 3, 2, -2, 1, false, false},
    };
    EXPECT_EQ(PlanOwners(map, {2}), (std::vector<uint16_t>{0, 0, 0, 0, 1}));
}
