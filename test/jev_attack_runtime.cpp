// Exercise the actual Jev answer executor and engine path scheduler, without API calls.
#include <SDL3/SDL_main.h>
#include <jevai.hpp> // The test runner supplies a generated header with test access.
#include "access.hpp"
#include "game_manager.hpp"
#include "missionmanager.hpp"
#include "paths_manager.hpp"
#include "resource_manager.hpp"
#include "transport.hpp"
#include "units_manager.hpp"
#include <algorithm>
#include <cstdlib>
#include <fstream>

namespace {
std::ofstream report("jev-attack-checks.txt");
int checks;
void Check(bool okay, const char* message) {
    report << (okay ? "PASS " : "FAIL ") << message << std::endl;
    if (!okay) std::exit(1);
    ++checks;
}

Point Site() {
    for (int y = 5; y < ResourceManager_MapSize.y - 6; ++y) {
        for (int x = 5; x < ResourceManager_MapSize.x - 14; ++x) {
            bool clear = true;
            for (int dx = 0; dx < 12 && clear; ++dx) {
                for (int dy = 0; dy < 3 && clear; ++dy) {
                    clear = Access_GetSurfaceType(x + dx, y + dy) == SURFACE_TYPE_LAND &&
                            Access_IsAccessible(TANK, PLAYER_TEAM_RED, x + dx, y + dy,
                                                AccessModifier_SameClassBlocks);
                }
            }
            if (clear) return Point(x, y);
        }
    }
    Check(false, "Find a clear attack test site");
    return Point();
}

void ChooseAttack(JevAi& ai, UnitInfo* attacker, UnitInfo* target) {
    ai.m_unit = attacker;
    ai.m_request_id = 1;
    ai.m_options.clear();
    ai.AddAttackOptions(attacker);
    auto found = std::find_if(ai.m_options.begin(), ai.m_options.end(),
                              [target](const auto& o) { return o.target.Get() == target; });
    Check(found != ai.m_options.end(), "A visible reachable enemy is offered as an attack option");
    const auto key = found->key;
    ai.Apply(nlohmann::ordered_json({{"id", 1}, {"choice", key}, {"model", "test"}}).dump());
}
} // namespace

void RuntimeJevAttackChecks() {
    GameManager_PlayMode = PLAY_MODE_TURN_BASED;
    GameManager_ActiveTurnTeam = PLAYER_TEAM_RED;
    UnitsManager_TeamInfo[PLAYER_TEAM_RED].team_type = TEAM_TYPE_COMPUTER;
    UnitsManager_TeamInfo[PLAYER_TEAM_RED].finished_turn = false;
    const auto site = Site();
    auto attacker = UnitsManager_SpawnUnit(TANK, PLAYER_TEAM_RED, site.x, site.y, nullptr);
    auto target = UnitsManager_SpawnUnit(SCANNER, PLAYER_TEAM_GREEN, site.x + 6, site.y, nullptr);
    target->visible_to_team[PLAYER_TEAM_RED] = true;
    attacker->SetOrder(ORDER_AWAIT);
    attacker->SetOrderState(ORDER_STATE_EXECUTING_ORDER);
    JevAi ai(PLAYER_TEAM_RED);
    const int range = attacker->GetBaseValues()->GetAttribute(ATTRIB_RANGE);
    Check(!Access_IsWithinAttackRange(attacker.Get(), target->grid_x, target->grid_y, range),
          "Target starts outside firing range");
    ChooseAttack(ai, attacker.Get(), target.Get());
    UnitsManager_ProcessOrders();
    Check(attacker->GetOrder() == ORDER_MOVE_TO_ATTACK &&
              attacker->GetOrderState() == ORDER_STATE_MOVE_GETTING_PATH,
          "Out-of-range attack requests a path instead of being cancelled by computer pursuit rules");
    Check(attacker->move_to_grid_x == target->grid_x && attacker->move_to_grid_y == target->grid_y,
          "The path targets the chosen enemy");
    ResourceManager_GetPathsManager().RemoveRequest(attacker.Get());

    auto close = UnitsManager_SpawnUnit(SCANNER, PLAYER_TEAM_GREEN, site.x + 2, site.y, nullptr);
    close->visible_to_team[PLAYER_TEAM_RED] = true;
    attacker->path = nullptr;
    attacker->SetOrder(ORDER_AWAIT);
    attacker->SetOrderState(ORDER_STATE_EXECUTING_ORDER);
    ChooseAttack(ai, attacker.Get(), close.Get());
    Check(attacker->GetOrder() == ORDER_MOVE_TO_ATTACK &&
              attacker->GetOrderState() == ORDER_STATE_EXECUTING_ORDER,
          "An enemy already in range enters the firing scheduler directly");
    Check(attacker->GetEnemy() == close.Get(), "In-range attack preserves the selected target");
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
    Check(false, "Reach attack test hook");
    return 1;
}
