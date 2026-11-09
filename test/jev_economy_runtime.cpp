// Regression checks for the b18 shutdown and an occupied mine destination, with no model calls.
#include <SDL3/SDL_main.h>
#include <jevai.hpp>
#include "access.hpp"
#include "cargo.hpp"
#include "game_manager.hpp"
#include "missionmanager.hpp"
#include "production_manager.hpp"
#include "resource_manager.hpp"
#include "scripter.hpp"
#include "teamunits.hpp"
#include "transport.hpp"
#include "units_manager.hpp"
#include <cstdlib>
#include <fstream>

namespace {
std::ofstream report("jev-economy-checks.txt");
int checks;
void Check(bool okay, const char* message) {
    report << (okay ? "PASS " : "FAIL ") << message << std::endl;
    if (!okay) std::exit(1);
    ++checks;
}

UnitInfo* Add(ResourceID type, Complex* network, UnitOrderType order) {
    SmartPointer<UnitInfo> unit(new UnitInfo(type, PLAYER_TEAM_RED, UINT16_MAX));
    unit->complex = network;
    unit->storage = 0;
    unit->SetOrder(order);
    unit->SetOrderState(ORDER_STATE_EXECUTING_ORDER);
    UnitsManager_StationaryUnits.PushBack(*unit);
    return unit.Get();
}
}

void RuntimeJevEconomyChecks() {
    GameManager_PlayMode = PLAY_MODE_TURN_BASED;
    GameManager_ActiveTurnTeam = PLAYER_TEAM_RED;
    UnitsManager_TeamInfo[PLAYER_TEAM_RED].team_type = TEAM_TYPE_COMPUTER;
    Scripter::ScriptParameters arguments, results;
    std::string error;
    Check(Scripter::RunScript(ResourceManager_GetMissionManager()->GetGameRulesHandler()->GetInterpreter(),
                             Scripter::LoadDefaultGameRules(), arguments, results, &error),
          "Use unrestricted game production rules instead of the training mission's restricted menu");
    JevAi ai(PLAYER_TEAM_RED);

    SmartPointer<Complex> network(new Complex(30000));
    auto mine = Add(MININGST, network.Get(), ORDER_POWER_OFF);
    mine->raw_mining_max = 12;
    mine->fuel_mining_max = 8;
    mine->raw_mining = mine->fuel_mining = 8;
    mine->total_mining = 16;
    auto generator = Add(POWGEN, network.Get(), ORDER_POWER_OFF);
    auto unused = Add(POWGEN, network.Get(), ORDER_POWER_OFF);
    auto disabled = Add(POWGEN, network.Get(), ORDER_DISABLE);
    auto factory = Add(LANDPLT, network.Get(), ORDER_AWAIT);
    Access_UpdateResourcesTotal(network.Get());
    ai.BalanceEconomy();
    Check(mine->GetOrder() == ORDER_POWER_ON && generator->GetOrder() == ORDER_POWER_ON,
          "Zero-stock fuel mine and generator restart together after the b18 blackout");
    Check(disabled->GetOrder() == ORDER_DISABLE, "Recovery leaves disabled generators alone");
    Access_UpdateResourcesTotal(network.Get());
    auto balance = ProductionManager_PreviewProduction(PLAYER_TEAM_RED, network.Get(), factory);
    Check(balance.raw >= 0 && balance.fuel >= 0 && balance.power >= 0,
          "Recovered network can supply normal production without stored fuel");
    ai.m_options.clear();
    ai.AddProduceOptions(factory);
    bool normal = false, fast = false;
    for (const auto& option : ai.m_options) {
        if (option.action != JevAi::Action::PRODUCE) continue;
        const auto& supply = option.data.at("supply");
        if (option.build_rate == 4) {
            fast = true;
            Check(supply.at("raw_balance").get<int>() < 0,
                  "Each x4 option reports its own shortage rather than normal-rate affordability");
        } else if (option.build_rate == 1) {
            normal = true;
            Check(supply.at("raw_balance").get<int>() >= 0,
                  "Normal-rate option includes mining income despite empty storage");
        }
    }
    Check(normal && fast, "Production menu still offers both normal and faster choices");

    SmartPointer<Complex> dry(new Complex(30001));
    auto dry_mine = Add(MININGST, dry.Get(), ORDER_POWER_OFF);
    dry_mine->raw_mining_max = 12;
    dry_mine->fuel_mining_max = 0;
    auto dry_generator = Add(POWGEN, dry.Get(), ORDER_POWER_OFF);
    ai.BalanceEconomy();
    Check(dry_mine->GetOrder() == ORDER_POWER_OFF && dry_generator->GetOrder() == ORDER_POWER_OFF,
          "A network without fuel deposits or inventory cannot invent a recovery");

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
    Check(false, "Reach economy test hook");
    return 1;
}
