// Check Jev mine site planning and connector lines in the real engine, without API calls.
#include <SDL3/SDL_main.h>
#include <jevai.hpp> // The test runner supplies a generated header with test access.
#include "access.hpp"
#include "game_manager.hpp"
#include "missionmanager.hpp"
#include "resource_manager.hpp"
#include "survey.hpp"
#include "teamunits.hpp"
#include "transport.hpp"
#include "units_manager.hpp"
#include <algorithm>
#include <cstdlib>
#include <fstream>

namespace {
std::ofstream report("jev-links-checks.txt");
int checks;
void Check(bool okay, const char* message) {
    report << (okay ? "PASS " : "FAIL ") << message << std::endl;
    if (!okay) std::exit(1);
    ++checks;
}

// A clear 16 by 16 land square.
Point Site() {
    for (int y = 5; y < ResourceManager_MapSize.y - 21; ++y) {
        for (int x = 5; x < ResourceManager_MapSize.x - 21; ++x) {
            bool clear = true;
            for (int dx = 0; dx < 16 && clear; ++dx) {
                for (int dy = 0; dy < 16 && clear; ++dy) {
                    clear = Access_GetSurfaceType(x + dx, y + dy) == SURFACE_TYPE_LAND &&
                            Access_IsAccessible(MININGST, PLAYER_TEAM_RED, x + dx, y + dy,
                                                AccessModifier_SameClassBlocks);
                }
            }
            if (clear) return Point(x, y);
        }
    }
    Check(false, "Find a clear test site");
    return Point();
}

void SetDeposit(Point cell, uint16_t amount, bool surveyed) {
    uint16_t& value = ResourceManager_CargoMap[ResourceManager_MapSize.x * cell.y + cell.x];
    value = amount;
    if (surveyed) value |= UnitsManager_TeamInfo[PLAYER_TEAM_RED].team_units->hash_team_id;
}
} // namespace

void RuntimeJevLinksChecks() {
    GameManager_PlayMode = PLAY_MODE_TURN_BASED;
    GameManager_ActiveTurnTeam = PLAYER_TEAM_RED;
    UnitsManager_TeamInfo[PLAYER_TEAM_RED].team_type = TEAM_TYPE_COMPUTER;
    const Point origin = Site();

    const Point plant(origin.x + 6, origin.y + 6);

    // the training mission gives red a base elsewhere; only the test buildings count here
    for (auto it = UnitsManager_StationaryUnits.Begin(), it_end = UnitsManager_StationaryUnits.End(); it != it_end;
         ++it) {
        if ((*it).team == PLAYER_TEAM_RED) (*it).hits = 0;
    }

    // no deposits anywhere the mine site search looks (14 cells around the base)
    for (int x = std::max(0, plant.x - 16); x < std::min<int>(ResourceManager_MapSize.x, plant.x + 18); ++x) {
        for (int y = std::max(0, plant.y - 16); y < std::min<int>(ResourceManager_MapSize.y, plant.y + 18); ++y) {
            SetDeposit(Point(x, y), 0, true);
        }
    }

    auto factory = UnitsManager_SpawnUnit(LANDPLT, PLAYER_TEAM_RED, plant.x, plant.y, nullptr);
    factory->AttachToPrimaryComplex();
    JevAi ai(PLAYER_TEAM_RED);
    Point base;
    Check(ai.GetBasePosition(base), "The factory makes a base");
    Check(ai.FindMineSites().empty(), "No mine site without surveyed deposits");

    // a rich surveyed deposit four cells east of the factory, not touching it
    const Point rich(plant.x + 5, plant.y);
    for (int dx = 0; dx < 2; ++dx) {
        for (int dy = 0; dy < 2; ++dy) SetDeposit(Point(rich.x + dx, rich.y + dy), 10, true);
    }
    Check(!ai.TouchesBase(MININGST, rich), "The deposit does not touch the base");

    std::vector<Point> path;
    Check(ai.FindConnectorPath(rich, 2, path), "A connector line joins the deposit to the base");
    Check(path.size() == 3, "The line is the three cells between them");
    Check(std::all_of(path.begin(), path.end(), [&](Point cell) { return cell.y == plant.y || cell.y == plant.y + 1; }),
          "The line runs straight between the deposit and the factory");

    auto sites = ai.FindMineSites();
    Check(std::find(sites.begin(), sites.end(), rich) != sites.end(), "The rich deposit is offered as a mine site");

    auto constructor = UnitsManager_DeployUnit(CONSTRCT, PLAYER_TEAM_RED, nullptr, plant.x + 2, plant.y + 4, 0);
    auto blocker = UnitsManager_DeployUnit(ENGINEER, PLAYER_TEAM_RED, nullptr, rich.x, rich.y, 0);
    blocker->SetOrder(ORDER_AWAIT);
    blocker->SetOrderState(ORDER_STATE_EXECUTING_ORDER);
    Check(Access_GetTeamUnit(rich.x, rich.y, PLAYER_TEAM_RED, MOBILE_LAND_UNIT) == blocker.Get(),
          "The parked Engineer occupies the mine destination in the engine map");
    sites = ai.FindMineSites(constructor.Get());
    Check(std::none_of(sites.begin(), sites.end(), [&](Point cell) {
              return rich.x >= cell.x && rich.x < cell.x + 2 && rich.y >= cell.y && rich.y < cell.y + 2;
          }), "An idle Engineer inside a mine footprint prevents the b18 repeated blocked destination");
    sites = ai.FindMineSites(blocker.Get());
    Check(std::find(sites.begin(), sites.end(), rich) != sites.end(),
          "The builder itself may occupy a building footprint");
    blocker->hits = 0;

    // an unsurveyed deposit is not known to the team
    const Point hidden(plant.x - 5, plant.y);
    for (int dx = 0; dx < 2; ++dx) {
        for (int dy = 0; dy < 2; ++dy) SetDeposit(Point(hidden.x + dx, hidden.y + dy), 16, false);
    }
    sites = ai.FindMineSites();
    Check(std::find(sites.begin(), sites.end(), hidden) == sites.end(), "An unsurveyed deposit is not offered");

    // the planned line is offered to an Engineer cell by cell, nearest first
    ai.m_links.push_back({rich, path});
    auto engineer = UnitsManager_SpawnUnit(ENGINEER, PLAYER_TEAM_RED, plant.x + 3, plant.y + 3, nullptr);
    auto cells = ai.GetLinkCells(engineer.Get());
    Check(cells.size() == 1, "One connector to lay for the one link");
    Check(std::find(path.begin(), path.end(), cells[0]) != path.end(), "It is a cell of the line");
    Check(ai.LeavesExits(CNCT_4W, cells[0]), "Connectors never count as blocking exits");

    for (const Point cell : path) {
        UnitsManager_SpawnUnit(CNCT_4W, PLAYER_TEAM_RED, cell.x, cell.y, nullptr)->AttachToPrimaryComplex();
    }
    Check(ai.GetLinkCells(engineer.Get()).empty(), "Nothing left to lay once the line stands");
    Check(ai.TouchesBase(MININGST, rich), "The deposit now touches the base through the connectors");
    Check(Access_IsAccessible(TANK, PLAYER_TEAM_RED, path[0].x, path[0].y, AccessModifier_EnemySameClassBlocks),
          "Land units drive over the connectors");
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
    Check(false, "Reach links test hook");
    return 1;
}
