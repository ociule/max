// Check that the Jev site search keeps factory and builder exits free, in the real engine, without API calls.
#include <SDL3/SDL_main.h>
#include <jevai.hpp> // The test runner supplies a generated header with test access.
#include "access.hpp"
#include "game_manager.hpp"
#include "missionmanager.hpp"
#include "resource_manager.hpp"
#include "transport.hpp"
#include "units_manager.hpp"
#include <cstdlib>
#include <fstream>
#include <vector>

namespace {
std::ofstream report("jev-exits-checks.txt");
int checks;
void Check(bool okay, const char* message) {
    report << (okay ? "PASS " : "FAIL ") << message << std::endl;
    if (!okay) std::exit(1);
    ++checks;
}

// A clear 10 by 10 land square.
Point Site() {
    for (int y = 5; y < ResourceManager_MapSize.y - 15; ++y) {
        for (int x = 5; x < ResourceManager_MapSize.x - 15; ++x) {
            bool clear = true;
            for (int dx = 0; dx < 10 && clear; ++dx) {
                for (int dy = 0; dy < 10 && clear; ++dy) {
                    clear = Access_GetSurfaceType(x + dx, y + dy) == SURFACE_TYPE_LAND &&
                            Access_IsAccessible(TANK, PLAYER_TEAM_RED, x + dx, y + dy, AccessModifier_SameClassBlocks);
                }
            }
            if (clear) return Point(x, y);
        }
    }
    Check(false, "Find a clear test site");
    return Point();
}

std::vector<Point> Outline(Point corner) {
    std::vector<Point> cells;
    for (int x = corner.x - 1; x <= corner.x + 2; ++x) {
        for (int y = corner.y - 1; y <= corner.y + 2; ++y) {
            if (x < corner.x || x > corner.x + 1 || y < corner.y || y > corner.y + 1) cells.push_back(Point(x, y));
        }
    }
    return cells;
}
} // namespace

void RuntimeJevExitsChecks() {
    GameManager_PlayMode = PLAY_MODE_TURN_BASED;
    GameManager_ActiveTurnTeam = PLAYER_TEAM_RED;
    UnitsManager_TeamInfo[PLAYER_TEAM_RED].team_type = TEAM_TYPE_COMPUTER;
    const Point origin = Site();
    const Point plant(origin.x + 4, origin.y + 4);
    UnitsManager_SpawnUnit(LANDPLT, PLAYER_TEAM_RED, plant.x, plant.y, nullptr);
    JevAi ai(PLAYER_TEAM_RED);
    const auto outline = Outline(plant);
    Check(outline.size() == 12, "A 2x2 factory has 12 exit cells");
    for (const Point cell : outline) {
        Check(ai.IsFactoryExit(cell), "Every cell around the factory is an exit");
    }
    Check(!ai.IsFactoryExit(Point(plant.x - 2, plant.y)), "A cell two away is no exit");

    // fill all exits but three with power generators
    for (size_t i = 3; i < outline.size(); ++i) {
        UnitsManager_SpawnUnit(POWGEN, PLAYER_TEAM_RED, outline[i].x, outline[i].y, nullptr);
    }
    Check(ai.LeavesExits(POWGEN, outline[0]), "A building may take an exit while two others stay free");

    UnitsManager_SpawnUnit(POWGEN, PLAYER_TEAM_RED, outline[2].x, outline[2].y, nullptr);
    Check(!ai.LeavesExits(POWGEN, outline[0]), "A building may not take one of the last two exits");
    Check(!ai.LeavesExits(POWGEN, outline[1]), "Neither of the last two exits");
    Check(ai.LeavesExits(POWGEN, Point(plant.x - 3, plant.y)), "A building away from the factory is allowed");

    // own units on an exit can move away, so they do not count as blocking it
    auto tank = UnitsManager_SpawnUnit(TANK, PLAYER_TEAM_RED, outline[0].x, outline[0].y, nullptr);
    tank->SetOrder(ORDER_AWAIT);
    tank->SetOrderState(ORDER_STATE_EXECUTING_ORDER);
    Check(!ai.LeavesExits(POWGEN, outline[1]), "An own unit on the other exit does not make room");

    // an Engineer next to the factory never picks the last exits
    auto engineer = UnitsManager_SpawnUnit(ENGINEER, PLAYER_TEAM_RED, plant.x - 2, plant.y, nullptr);
    engineer->storage = 40;
    engineer->SetOrder(ORDER_AWAIT);
    engineer->SetOrderState(ORDER_STATE_EXECUTING_ORDER);
    Point site;
    Check(ai.FindBuildSite(engineer.Get(), POWGEN, site), "The Engineer finds a Power Generator site");
    Check(site != outline[0] && site != outline[1], "The site leaves the last two factory exits free");

    // a site boxed in on all sides would trap its builder
    const Point box(origin.x, origin.y);
    for (const Point cell : std::vector<Point>{{box.x - 1, box.y}, {box.x + 1, box.y}, {box.x, box.y - 1},
                                               {box.x, box.y + 1}, {box.x - 1, box.y - 1}, {box.x + 1, box.y + 1},
                                               {box.x - 1, box.y + 1}}) {
        UnitsManager_SpawnUnit(POWGEN, PLAYER_TEAM_RED, cell.x, cell.y, nullptr);
    }
    Check(!ai.LeavesExits(GUNTURRT, box), "A site with one free neighbor would trap its builder");
    report << "PASS all " << checks << " checks" << std::endl;
    std::exit(0);
}

int main(int, char**) {
    ResourceManager_InitResources();
    Check(Transport_Init(), "Initialize transport");
    auto missions = ResourceManager_GetMissionManager();
    Check(missions->LoadMission(MISSION_CATEGORY_TRAINING, uint32_t(0)), "Load training mission");
    missions->SetMissionStart(true);
    GameManager_GameLoop(GAME_STATE_10_LOAD_GAME);
    Check(false, "Reach exits test hook");
    return 1;
}
