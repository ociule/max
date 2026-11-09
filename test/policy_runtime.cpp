// Full-observation and command boundary checks against real engine objects.
#include <SDL3/SDL_main.h>
#include "policyai.hpp"
#include "access.hpp"
#include "aicontroller.hpp"
#include "game_manager.hpp"
#include "heatmap.hpp"
#include "missionmanager.hpp"
#include "paths_manager.hpp"
#include "resource_manager.hpp"
#include "teamunits.hpp"
#include "transport.hpp"
#include "units_manager.hpp"
#include <cstdlib>
#include <fstream>

using json = nlohmann::ordered_json;
namespace {
std::ofstream report("policy-checks.txt");
int checks;
void Check(bool okay, const char* message) {
    report << (okay ? "PASS " : "FAIL ") << message << std::endl;
    if (!okay) std::exit(1);
    ++checks;
}
void Ready(UnitInfo* unit) {
    unit->path = nullptr;
    unit->SetOrder(ORDER_AWAIT);
    unit->SetOrderState(ORDER_STATE_EXECUTING_ORDER);
}
SmartPointer<UnitInfo> Add(ResourceID type, uint16_t team, int32_t x, int32_t y) {
    auto unit = UnitsManager_DeployUnit(type, team, nullptr, x, y, 0);
    unit->SpotByTeam(team);
    return unit;
}
Point Site() {
    for (int y = 5; y < ResourceManager_MapSize.y - 16; ++y) {
        for (int x = 5; x < ResourceManager_MapSize.x - 16; ++x) {
            bool clear = true;
            for (int dx = 0; dx < 12 && clear; ++dx) {
                for (int dy = 0; dy < 12 && clear; ++dy) {
                    clear = Access_GetSurfaceType(x + dx, y + dy) == SURFACE_TYPE_LAND &&
                            Access_IsAccessible(TANK, PLAYER_TEAM_RED, x + dx, y + dy, AccessModifier_SameClassBlocks);
                }
            }
            if (clear) return Point(x, y);
        }
    }
    Check(false, "Find a clear fixture site");
    return Point();
}
bool Contains(const json& units, uint16_t id) {
    for (const auto& unit : units) if (unit["id"] == id) return true;
    return false;
}
bool Accept(PolicyAi& ai, const json& command) {
    auto result = ai.ApplyCommand(command);
    report << "Command " << command.dump() << " -> " << result.dump() << std::endl;
    return result["status"] == "accepted";
}
}

void RuntimePolicyChecks() {
    Check(ResourceManager_GetMissionManager()->LoadMission(MISSION_CATEGORY_CUSTOM, "MISSION_CATEGORY_CUSTOM"),
          "Use custom-game building rules for the policy command checks");
    GameManager_PlayMode = PLAY_MODE_TURN_BASED;
    GameManager_ActiveTurnTeam = PLAYER_TEAM_RED;
    UnitsManager_TeamInfo[PLAYER_TEAM_RED].team_type = TEAM_TYPE_COMPUTER;
    UnitsManager_TeamInfo[PLAYER_TEAM_RED].finished_turn = false;
    AiController_SetType(PLAYER_TEAM_RED, AI_CONTROLLER_POLICY);
    Check(!AiController_IsClassic(PLAYER_TEAM_RED), "Policy team is excluded from classic AI tasks");
    Check(dynamic_cast<PolicyAi*>(AiController_Get(PLAYER_TEAM_RED)) != nullptr, "Registry selects the policy controller");
    PolicyAi ai(PLAYER_TEAM_RED);
    const auto site = Site();
    auto tank = Add(TANK, PLAYER_TEAM_RED, site.x, site.y);
    auto enemy = Add(SCANNER, PLAYER_TEAM_GREEN, site.x + 6, site.y);
    Ready(tank.Get());
    Ready(enemy.Get());
    enemy->visible_to_team[PLAYER_TEAM_RED] = false;
    enemy->spotted_by_team[PLAYER_TEAM_RED] = true;
    const auto before = ai.GetObservation();
    const auto cell_count = ResourceManager_MapSize.x * ResourceManager_MapSize.y;
    Check(before["map"]["terrain"].size() == size_t(cell_count) &&
          before["map"]["visible"].size() == size_t(cell_count) &&
          before["map"]["resources"].size() == size_t(cell_count), "Every map cell is exported in row-major layers");
    Check(Contains(before["own"], tank->GetId()), "Own entity records use engine unique IDs");
    Check(!Contains(before["visible_enemies"], enemy->GetId()), "Historical spotting does not reveal live enemy state");
    Check(before == ai.GetObservation(), "Repeated observation leaves engine-visible fields unchanged");
    const auto hp = enemy->hits;
    enemy->hits = 1;
    ++enemy->grid_x;
    Check(before == ai.GetObservation(), "Hidden enemy health and movement do not affect the observation");
    enemy->hits = hp;
    --enemy->grid_x;
    const int index = site.y * ResourceManager_MapSize.x + site.x;
    auto& cargo = ResourceManager_CargoMap[index];
    const auto original_cargo = cargo;
    const auto survey_bit = UnitsManager_TeamInfo[PLAYER_TEAM_RED].team_units->hash_team_id;
    cargo = 3;
    auto unknown = ai.GetObservation();
    Check(unknown["map"]["resources"][index].is_null(), "Unknown resources are distinct from surveyed zero");
    cargo = CARGO_GOLD | 12;
    Check(unknown == ai.GetObservation(), "Changing unsurveyed resource type and amount is invisible");
    cargo |= survey_bit;
    Check(ai.GetObservation()["map"]["resources"][index] == json({2, 12}), "Surveyed resources contain only type and visible amount");
    cargo = original_cargo;
    enemy->visible_to_team[PLAYER_TEAM_RED] = true;
    auto seen = ai.GetObservation();
    Check(Contains(seen["visible_enemies"], enemy->GetId()), "Visible enemies are exported");
    enemy->visible_to_team[PLAYER_TEAM_RED] = false;
    auto remembered = ai.GetObservation();
    enemy->hits = 1;
    Check(remembered == ai.GetObservation(), "Remembered sightings do not track hidden changes");
    enemy->hits = hp;
    Check(!Accept(ai, {{"action", "move"}, {"unit", enemy->GetId()}, {"at", {site.x, site.y}}}), "Cannot issue orders to enemy IDs");
    Check(!Accept(ai, {{"action", "attack"}, {"unit", tank->GetId()}, {"target", enemy->GetId()}}), "Cannot target a hidden enemy by ID");
    Check(!Accept(ai, {{"action", "move"}, {"unit", tank->GetId()}, {"at", {-1, site.y}}}), "Out-of-map commands are rejected");
    Check(!Accept(ai, {{"action", "move"}, {"unit", uint64_t(1) << 40}, {"at", {0, 0}}}), "Oversized IDs cannot wrap to an owned unit");
    Check(!Accept(ai, {{"action", "move"}, {"unit", tank->GetId()}, {"at", {1.5, 2}}}), "Fractional coordinates are rejected");
    Check(tank->GetOrder() == ORDER_AWAIT, "Rejected commands do not alter the unit order");
    Check(Accept(ai, {{"action", "move"}, {"unit", tank->GetId()}, {"at", {site.x + 1, site.y}}}), "A policy can choose an arbitrary map destination");
    UnitsManager_ProcessOrders();
    Check(tank->GetOrder() == ORDER_MOVE, "Move enters the normal engine scheduler");
    ResourceManager_GetPathsManager().RemoveRequest(tank.Get());
    Ready(tank.Get());
    enemy->visible_to_team[PLAYER_TEAM_RED] = true;
    Check(Accept(ai, {{"action", "attack"}, {"unit", tank->GetId()}, {"target", enemy->GetId()}}), "Visible valid target accepts an attack");
    UnitsManager_ProcessOrders();
    Check(tank->GetOrder() == ORDER_MOVE_TO_ATTACK && tank->GetOrderState() == ORDER_STATE_MOVE_GETTING_PATH,
          "Out-of-range attack requests movement through the real path scheduler");
    ResourceManager_GetPathsManager().RemoveRequest(tank.Get());
    Ready(tank.Get());
    auto engineer = Add(ENGINEER, PLAYER_TEAM_RED, site.x + 2, site.y + 4);
    Ready(engineer.Get());
    engineer->storage = 40;
    Check(Accept(ai, {{"action", "build"}, {"unit", engineer->GetId()}, {"type", POWGEN},
                     {"at", {engineer->grid_x, engineer->grid_y}}, {"rate", 1}}), "Policy chooses the construction type and exact site");
    UnitsManager_ProcessOrders();
    Check(engineer->GetOrder() == ORDER_BUILD && engineer->build_list.GetCount() == 1 &&
          *engineer->build_list[0] == POWGEN, "Build enters the normal engine construction path");
    auto factory = Add(LANDPLT, PLAYER_TEAM_RED, site.x + 7, site.y + 6);
    Ready(factory.Get());
    Check(Accept(ai, {{"action", "produce"}, {"unit", factory->GetId()}, {"type", TANK}, {"rate", 2}}),
          "Policy chooses a factory product and rate");
    UnitsManager_ProcessOrders();
    Check(factory->build_list.GetCount() == 1 && *factory->build_list[0] == TANK && factory->GetBuildRate() == 2,
          "Factory command reaches the engine with the chosen queue and rate");
    Check(!Accept(ai, {{"action", "produce"}, {"unit", tank->GetId()}, {"type", TANK}}), "Non-builders cannot manufacture arbitrary units");
    Check(Accept(ai, {{"action", "end_turn"}}), "Policy can explicitly finish its turn");
    std::ofstream("policy-observation.json") << seen.dump();
    report << "Observation bytes: " << seen.dump().size() << std::endl;
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
    Check(false, "Reach policy test hook");
    return 1;
}
