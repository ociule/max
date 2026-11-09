// Read-only production forecast checks against real engine objects.
#include <SDL3/SDL_main.h>
#include "cargo.hpp"
#include "complex.hpp"
#include "game_manager.hpp"
#include "missionmanager.hpp"
#include "production_manager.hpp"
#include "resource_manager.hpp"
#include "transport.hpp"
#include "unitinfo.hpp"
#include "units_manager.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

namespace {
std::ofstream report("production-preview-checks.txt");
int checks;
std::vector<SmartPointer<UnitInfo>> fixtures;

void Check(bool okay, const char* message) {
    report << (okay ? "PASS " : "FAIL ") << message << std::endl;
    if (!okay) std::exit(1);
    ++checks;
}

UnitInfo* Add(ResourceID type, Complex* complex, UnitOrderType order) {
    SmartPointer<UnitInfo> unit(new UnitInfo(type, PLAYER_TEAM_RED, UINT16_MAX));
    unit->complex = complex;
    unit->storage = 0;
    unit->SetOrder(order);
    unit->SetOrderState(ORDER_STATE_EXECUTING_ORDER);
    UnitsManager_StationaryUnits.PushBack(*unit);
    fixtures.push_back(unit);
    return unit.Get();
}

std::string Snapshot() {
    std::ostringstream out;
    for (auto it = UnitsManager_StationaryUnits.Begin(); it != UnitsManager_StationaryUnits.End(); ++it) {
        const auto& u = *it;
        out << u.GetOrder() << ',' << u.GetOrderState() << ',' << u.GetPriorOrder() << ','
            << u.GetPriorOrderState() << ',' << u.storage << ',' << u.raw_mining << ',' << u.fuel_mining
            << ',' << u.gold_mining << ',' << u.total_mining << ';';
        if (u.GetComplex()) {
            auto* c = u.GetComplex();
            out << c->material << ',' << c->fuel << ',' << c->power << ',' << c->workers << ',' << c->gold;
        }
    }
    return out.str();
}

Cargo Preview(UnitInfo* factory, int32_t build_rate = 1) {
    const auto before = Snapshot();
    const auto balance = ProductionManager_PreviewProduction(factory->team, factory->GetComplex(), factory, build_rate);
    Check(before == Snapshot(), "Preview leaves all orders, inventories, mining and network balances unchanged");
    return balance;
}
} // namespace

void RuntimeProductionPreviewChecks() {
    SmartPointer<Complex> network(new Complex(30000));
    auto* mine = Add(MININGST, network.Get(), ORDER_POWER_ON);
    mine->raw_mining = 10;
    mine->fuel_mining = 6;
    mine->total_mining = 16;
    mine->raw_mining_max = 12;
    mine->fuel_mining_max = 8;
    mine->storage = 25;
    auto* factory = Add(LIGHTPLT, network.Get(), ORDER_AWAIT);
    auto* generator = Add(POWGEN, network.Get(), ORDER_POWER_ON);
    auto* spare1 = Add(POWGEN, network.Get(), ORDER_POWER_OFF);
    auto* spare2 = Add(POWGEN, network.Get(), ORDER_POWER_OFF);

    auto balance = Preview(factory);
    Check(balance.raw == 32 && balance.fuel == 2 && balance.power == 0 && balance.life == 0,
          "Idle fueled generators make production affordable despite zero current spare power");
    balance = Preview(factory);
    Check(balance.fuel == 2, "Repeated preview does not activate generators or consume fuel");
    balance = Preview(factory, 2);
    Check(balance.raw == 23, "x2 preview charges twelve raw materials rather than the normal three");
    balance = Preview(factory, 4);
    Check(balance.raw == -1, "x4 preview reports the actual shortage at thirty-six raw materials per turn");

    mine->fuel_mining = 0;
    balance = Preview(factory);
    Check(balance.fuel < 0, "Forecast reports fuel shortage instead of promising supplied production");
    mine->fuel_mining = 6;
    spare1->SetOrder(ORDER_DISABLE);
    spare2->SetOrder(ORDER_DISABLE);
    balance = Preview(factory);
    Check(balance.power == -1, "Disabled generators cannot cover the factory's power deficit");

    SmartPointer<Complex> separate(new Complex(30001));
    Add(POWERSTN, separate.Get(), ORDER_POWER_ON);
    balance = Preview(factory);
    Check(balance.power == -1, "A generator on another network cannot power this factory");
    spare1->SetOrder(ORDER_POWER_OFF);
    spare2->SetOrder(ORDER_POWER_OFF);

    mine->storage = 0;
    mine->raw_mining = 0;
    balance = Preview(factory);
    Check(balance.raw == -3, "Factory needs three raw materials per turn even with power available");
    mine->storage = 25;
    mine->raw_mining = 10;

    factory->SetOrder(ORDER_BUILD);
    factory->SetBuildRate(1);
    factory->build_time = 8;
    balance = Preview(factory);
    Check(balance.raw == 32 && balance.fuel == 2 && balance.power == 0,
          "Restarting a factory replaces its current demand rather than counting it twice");
    factory->SetOrder(ORDER_AWAIT);
    auto* training = Add(TRAINHAL, network.Get(), ORDER_AWAIT);
    balance = Preview(training);
    Check(balance.life == -1, "Training hall forecast includes its worker requirement");
    Add(HABITAT, network.Get(), ORDER_POWER_ON);
    balance = Preview(training);
    Check(balance.life == 2, "An operating Habitat supplies workers to production");
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
    Check(false, "Reach forecast test hook");
    return 1;
}
