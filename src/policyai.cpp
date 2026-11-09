#include "policyai.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <set>
#include <stdexcept>
#include "access.hpp"
#include "builder.hpp"
#include "buildmenu.hpp"
#include "cargo.hpp"
#include "game_manager.hpp"
#include "heatmap.hpp"
#include "resource_manager.hpp"
#include "task.hpp"
#include "teamunits.hpp"
#include "units_manager.hpp"
#include "world.hpp"

using json = nlohmann::ordered_json;

namespace {
constexpr uint64_t ReplyTimeout = 15000;
constexpr uint64_t OrderTimeout = 30000;
constexpr int32_t MaxCommandsPerTurn = 256;

auto UnitLists() {
    return std::array<SmartList<UnitInfo>*, 4>{&UnitsManager_MobileLandSeaUnits, &UnitsManager_MobileAirUnits,
                                            &UnitsManager_StationaryUnits, &UnitsManager_GroundCoverUnits};
}

UnitInfo* FindUnit(int32_t id) {
    for (auto* list : UnitLists()) {
        for (auto& unit : *list) {
            if (unit.GetId() == id && unit.hits > 0) return &unit;
        }
    }
    return nullptr;
}

bool Inside(int32_t x, int32_t y) {
    return x >= 0 && y >= 0 && x < ResourceManager_MapSize.x && y < ResourceManager_MapSize.y;
}

json UnitAttributeData(UnitValues* values) {
    json result = json::array();
    for (char i = 0; i < ATTRIB_COUNT; ++i) result.push_back(values->GetAttribute(i));
    return result;
}

json CargoData(const Cargo& cargo) { return {cargo.raw, cargo.fuel, cargo.gold}; }

bool Settled(UnitInfo* unit) {
    if (!unit || unit->hits == 0) return true;
    if (unit->GetOrderState() == ORDER_STATE_INIT) return false;
    switch (unit->GetOrder()) {
        case ORDER_AWAIT:
        case ORDER_SENTRY:
        case ORDER_POWER_ON:
        case ORDER_POWER_OFF:
        case ORDER_HALT_BUILDING:
        case ORDER_HALT_BUILDING_2:
        case ORDER_IDLE:
        case ORDER_DISABLE: return true;
        case ORDER_BUILD:
            return unit->GetOrderState() == ORDER_STATE_BUILD_IN_PROGRESS ||
                   unit->GetOrderState() == ORDER_STATE_UNIT_READY;
        case ORDER_MOVE:
        case ORDER_MOVE_TO_UNIT:
            return unit->GetOrderState() == ORDER_STATE_EXECUTING_ORDER && !unit->path;
        default: return false;
    }
}

json Rejected(const char* reason) { return {{"status", "rejected"}, {"reason", reason}}; }

// Avoid narrowing/wrapping JSON numbers before validating a command.
int32_t Integer(const json& data, const char* key, int32_t min, int32_t max) {
    const auto& value = data.at(key);
    if (!value.is_number_integer() || value < min || value > max) throw std::out_of_range(key);
    return value.get<int32_t>();
}
}

bool PolicyAi::SetupStrategy() {
    ++m_episode;
    m_bridge.Close();
    m_sightings.clear();
    m_request = 0;
    m_last_turn = 0;
    m_pending = nullptr;
    m_result = nullptr;
    return NoneAi::SetupStrategy();
}

void PolicyAi::FileLoad(SmartFileReader& file) {
    ++m_episode;
    NoneAi::FileLoad(file);
    m_bridge.Close();
    m_sightings.clear();
    m_pending = nullptr;
    m_result = nullptr;
    m_last_turn = 0;
    m_phase = Phase::READY;
}

void PolicyAi::BeginTurn() {
    if (GameManager_TurnCounter <= m_last_turn) m_sightings.clear();
    m_last_turn = GameManager_TurnCounter;
    m_commands = 0;
    m_wait_start = SDL_GetTicks();
    m_pending = nullptr;
    m_phase = Phase::READY;
    m_bridge.Close(); // Discard any stale replies from an earlier turn.
}

json PolicyAi::GetObservation() {
    const auto& team = UnitsManager_TeamInfo[m_team];
    const int32_t width = ResourceManager_MapSize.x;
    const int32_t height = ResourceManager_MapSize.y;
    json terrain = json::array(), visibility = json::array(), resources = json::array();
    const auto* world = ResourceManager_GetActiveWorld();
    for (int32_t y = 0; y < height; ++y) {
        for (int32_t x = 0; x < width; ++x) {
            const int32_t index = y * width + x;
            // MAX shows terrain outside scan coverage too (darkened on the minimap).
            terrain.push_back(world ? world->GetSurfaceType(x, y) : SURFACE_TYPE_NONE);
            visibility.push_back(team.heat_map && team.heat_map->IsVisible(x, y));
            const uint16_t cargo = ResourceManager_CargoMap ? ResourceManager_CargoMap[index] : 0;
            if (team.team_units && (cargo & team.team_units->hash_team_id)) {
                resources.push_back({(cargo & CARGO_GOLD) ? 2 : ((cargo & CARGO_FUEL) ? 1 : 0),
                                     std::min<int32_t>(cargo & CARGO_MASK, 16)});
            } else {
                resources.push_back(nullptr);
            }
        }
    }

    json own = json::array(), enemies = json::array(), networks = json::array();
    std::set<uint16_t> exported;
    std::set<Complex*> complexes;
    for (auto* list : UnitLists()) {
        for (auto& unit : *list) {
            if (!exported.insert(unit.GetId()).second) continue;
            if (unit.team != m_team) {
                // IsDetectedByTeam includes historical spotting, so it must not
                // be used to export a hidden unit's live position or health.
                if (!unit.IsVisibleToTeam(m_team) || unit.hits == 0 || unit.GetOrder() == ORDER_IDLE) continue;
                json seen = {{"id", unit.GetId()}, {"team", unit.team}, {"type", unit.GetUnitType()},
                             {"at", {unit.grid_x, unit.grid_y}}, {"hits", unit.hits},
                             {"last_seen_turn", GameManager_TurnCounter}};
                enemies.push_back(seen);
                m_sightings[unit.GetId()] = seen;
                continue;
            }
            if (unit.hits == 0) continue;
            json queue = json::array();
            for (uint32_t i = 0; i < unit.build_list.GetCount(); ++i) queue.push_back(*unit.build_list[i]);
            json contents = json::array();
            if (unit.GetUnitList()) {
                for (auto& child : *unit.GetUnitList()) contents.push_back(child.GetId());
            }
            own.push_back({{"id", unit.GetId()}, {"type", unit.GetUnitType()},
                           {"at", {unit.grid_x, unit.grid_y}}, {"flags", unit.flags},
                           {"attributes", UnitAttributeData(unit.GetBaseValues())}, {"version", unit.GetBaseValues()->GetVersion()},
                           {"hits", unit.hits}, {"moves", unit.speed}, {"shots", unit.shots}, {"ammo", unit.ammo},
                           {"cargo", unit.storage}, {"order", unit.GetOrder()}, {"state", unit.GetOrderState()},
                           {"destination", {unit.move_to_grid_x, unit.move_to_grid_y}},
                           {"build_turns", unit.build_time}, {"build_rate", unit.GetBuildRate()},
                           {"repeat", unit.GetRepeatBuildState()}, {"queue", queue},
                           {"mining", {unit.raw_mining, unit.fuel_mining, unit.gold_mining}},
                           {"mining_max", {unit.raw_mining_max, unit.fuel_mining_max, unit.gold_mining_max}},
                           {"mining_total", unit.total_mining}, {"disabled_turns", unit.disabled_turns_remaining},
                           {"experience", unit.experience}, {"move_and_fire", unit.move_and_fire},
                           {"move_fraction", unit.move_fraction}, {"group_speed", unit.group_speed},
                           {"auto_survey", unit.auto_survey}, {"reaction_fire_disabled", unit.disabled_reaction_fire},
                           {"laying_state", unit.GetLayingState()}, {"connectors", unit.connectors},
                           {"research_topic", unit.research_topic}, {"contents", contents},
                           {"parent", unit.GetParent() ? json(unit.GetParent()->GetId()) : json(nullptr)},
                           {"network", unit.GetComplex() ? json(unit.GetComplex()->GetId()) : json(nullptr)}});
            if (unit.GetComplex()) complexes.insert(unit.GetComplex());
        }
    }
    for (auto* complex : complexes) {
        Cargo inventory, capacity, mining, mining_capacity;
        complex->GetCargoInfo(inventory, capacity);
        complex->GetCargoMining(mining, mining_capacity);
        networks.push_back({{"id", complex->GetId()}, {"stock", CargoData(inventory)},
                            {"capacity", CargoData(capacity)}, {"mining", CargoData(mining)},
                            {"mining_capacity", CargoData(mining_capacity)}, {"power", complex->power},
                            {"workers", complex->workers}});
    }
    json research = json::array(), scores = json::array(), catalog = json::array(), sightings = json::array();
    for (const auto& topic : team.research_topics) {
        research.push_back({topic.research_level, topic.turns_to_complete, topic.allocation});
    }
    for (int32_t t = 0; t < PLAYER_TEAM_MAX - 1; ++t) {
        if (UnitsManager_TeamInfo[t].team_type != TEAM_TYPE_NONE) {
            scores.push_back({{"team", t}, {"points", UnitsManager_TeamInfo[t].team_points}});
        }
    }
    for (int32_t type = 0; type < UNIT_END; ++type) {
        const auto id = static_cast<ResourceID>(type);
        const auto& unit = ResourceManager_GetUnit(id);
        auto* values = team.team_units ? team.team_units->GetCurrentUnitValues(id) : nullptr;
        if (!values) continue;
        catalog.push_back({{"type", type}, {"key", ResourceManager_GetResourceID(id)},
                           {"name", std::string(unit.GetSingularName())}, {"flags", unit.GetFlags()},
                           {"land_type", unit.GetLandType()}, {"cargo_type", unit.GetCargoType()},
                           {"attributes", UnitAttributeData(values)},
                           {"products", Builder_GetBuildableUnits(m_team, id)}});
    }
    for (const auto& entry : m_sightings) sightings.push_back(entry.second);
    return {{"schema", "max-policy-observation-v1"}, {"team", m_team}, {"turn", GameManager_TurnCounter},
            {"play_mode", GameManager_PlayMode}, {"clan", team.team_clan},
            {"victory", {{"type", ini_setting_victory_type}, {"limit", ini_setting_victory_limit}}},
            {"map", {{"width", width}, {"height", height}, {"terrain", terrain},
                     {"visible", visibility}, {"resources", resources}}},
            {"own", own}, {"visible_enemies", enemies}, {"last_seen", sightings},
            {"networks", networks}, {"research", research}, {"scores", scores},
            {"credits", team.team_units ? team.team_units->GetGold() : 0}, {"catalog", catalog},
            {"commands", {"end_turn", "move", "attack", "build", "produce", "sentry"}},
            {"commands_remaining", MaxCommandsPerTurn - m_commands}, {"last_result", m_result}};
}

json PolicyAi::ApplyCommand(const json& command) {
    if (!command.is_object()) return Rejected("command must be an object");
    if (!GameManager_IsActiveTurn(m_team) || UnitsManager_TeamInfo[m_team].finished_turn) {
        return Rejected("team is not active");
    }
    try {
        const auto action = command.at("action").get<std::string>();
        if (action == "end_turn") {
            m_phase = Phase::END;
            return {{"status", "accepted"}, {"action", action}};
        }
        if (action != "move" && action != "attack" && action != "build" && action != "produce" && action != "sentry") {
            return Rejected("unsupported action");
        }
        UnitInfo* unit = FindUnit(Integer(command, "unit", 0, UINT16_MAX - 1));
        if (!unit || unit->team != m_team) return Rejected("unit is not controlled by this team");
        if (unit->disabled_turns_remaining || unit->GetOrder() == ORDER_IDLE || unit->in_transit || !Settled(unit)) {
            return Rejected("unit is not ready for a new command");
        }
        int32_t x = 0, y = 0;
        ResourceID product = INVALID_ID;
        int32_t rate = 1;
        UnitInfo* target = nullptr;
        if (action == "move" || action == "build") {
            const auto& at = command.at("at");
            if (!at.is_array() || at.size() != 2) return Rejected("at must be [x,y]");
            json point = {{"x", at[0]}, {"y", at[1]}};
            x = Integer(point, "x", 0, ResourceManager_MapSize.x - 1);
            y = Integer(point, "y", 0, ResourceManager_MapSize.y - 1);
        }
        if (action == "move") {
            if (unit->flags & STATIONARY || !unit->speed) return Rejected("unit cannot move");
            // Do not preflight occupancy against hidden engine objects. Let the
            // normal pathfinder resolve blocked/unknown destinations.
            unit->move_to_grid_x = x;
            unit->move_to_grid_y = y;
            unit->path = nullptr;
            UnitsManager_SetNewOrder(unit, ORDER_MOVE, ORDER_STATE_INIT);
        } else if (action == "attack") {
            target = FindUnit(Integer(command, "target", 0, UINT16_MAX - 1));
            if (!target || target->team == m_team || !target->IsVisibleToTeam(m_team)) {
                return Rejected("target is not a visible enemy");
            }
            if (!unit->shots || !unit->ammo || !Access_IsValidAttackTarget(unit, target)) {
                return Rejected("unit cannot attack this target");
            }
            const auto range = unit->GetBaseValues()->GetAttribute(ATTRIB_RANGE);
            const bool in_range = Access_IsWithinAttackRange(unit, target->grid_x, target->grid_y, range);
            if (!in_range && ((unit->flags & STATIONARY) || !unit->speed)) return Rejected("target is out of range");
            unit->SetEnemy(target);
            unit->path = nullptr;
            unit->move_to_grid_x = target->grid_x;
            unit->move_to_grid_y = target->grid_y;
            UnitsManager_SetNewOrder(unit, ORDER_MOVE_TO_ATTACK,
                                    in_range ? ORDER_STATE_EXECUTING_ORDER : ORDER_STATE_MOVE_INIT);
        } else if (action == "sentry") {
            UnitsManager_SetNewOrder(unit, ORDER_SENTRY, ORDER_STATE_INIT);
        } else {
            product = static_cast<ResourceID>(Integer(command, "type", 0, UNIT_END - 1));
            rate = command.contains("rate") ? Integer(command, "rate", 1, 4) : 1;
            if (rate != 1 && rate != 2 && rate != 4) return Rejected("rate must be 1, 2 or 4");
            const auto products = Builder_GetBuildableUnits(m_team, unit->GetUnitType());
            if (std::find(products.begin(), products.end(), product) == products.end() || !Builder_IsBuildable(m_team, product)) {
                return Rejected("unit cannot build this type");
            }
            if (action == "produce") {
                if (!(unit->flags & STATIONARY)) return Rejected("produce requires a factory");
            } else {
                if (unit->flags & STATIONARY) return Rejected("build requires a mobile builder");
                const auto size = (ResourceManager_GetUnit(product).GetFlags() & BUILDING) ? 2 : 1;
                if (!Inside(x + size - 1, y + size - 1)) return Rejected("building footprint is outside the map");
                // First slice: build where the builder stands. The policy can
                // move it first. Do not invent a site or auto-walk to one.
                if (unit->grid_x < x || unit->grid_x >= x + size || unit->grid_y < y || unit->grid_y >= y + size) {
                    return Rejected("builder must stand in the chosen footprint");
                }
                for (int32_t by = y; by < y + size; ++by) {
                    for (int32_t bx = x; bx < x + size; ++bx) {
                        // Checking unsurveyed enemy occupancy would be an oracle.
                        if (!UnitsManager_TeamInfo[m_team].heat_map ||
                            !UnitsManager_TeamInfo[m_team].heat_map->IsVisible(bx, by)) {
                            return Rejected("construction footprint must be visible");
                        }
                        if (!Access_IsAccessible(product, m_team, bx, by, AccessModifier_EnemySameClassBlocks)) {
                            return Rejected("construction footprint is blocked");
                        }
                    }
                }
            }
            auto queue = unit->GetBuildList();
            queue.Clear();
            queue.PushBack(&product);
            unit->SetBuildRate(rate);
            unit->SetRepeatBuildState(false);
            unit->move_to_grid_x = x;
            unit->move_to_grid_y = y;
            unit->path = nullptr;
            unit->BuildOrder();
        }
        m_pending = unit;
        m_phase = Phase::ORDER;
        m_wait_start = SDL_GetTicks();
        return {{"status", "accepted"}, {"action", action}, {"unit", unit->GetId()}};
    } catch (const std::exception&) {
        return Rejected("missing, malformed or out-of-range command argument");
    }
}

bool PolicyAi::IsSettled() const {
    if (UnitsManager_CombatEffectsActive || UnitsManager_DelayedReactionsPending) return false;
    for (auto* list : UnitLists()) {
        for (auto& unit : *list) {
            if (unit.team == m_team && !Settled(&unit)) return false;
        }
    }
    return true;
}

void PolicyAi::FailTurn(const char* reason) {
    SDL_Log("Policy team %u: %s; ending turn without a substitute policy.", m_team, reason);
    m_result = {{"status", "controller_failure"}, {"reason", reason}};
    m_bridge.Close();
    m_phase = Phase::END;
}

bool PolicyAi::CheckEndTurn() {
    if (UnitsManager_TeamInfo[m_team].finished_turn || !GameManager_IsActiveTurn(m_team)) return false;
    if (m_phase == Phase::END) return AiController_EndTurn(m_team);
    if (m_phase == Phase::ORDER) {
        if (!m_pending || m_pending->hits == 0 || Settled(m_pending.Get())) {
            if (m_result.value("status", "") == "accepted") {
                m_result["status"] = "settled";
                m_result["unit_alive"] = m_pending && m_pending->hits > 0;
                if (m_pending && m_pending->hits > 0) {
                    m_result["at"] = {m_pending->grid_x, m_pending->grid_y};
                    m_result["order"] = m_pending->GetOrder();
                    m_result["state"] = m_pending->GetOrderState();
                }
            }
            m_pending = nullptr;
            m_phase = Phase::READY;
        } else if (SDL_GetTicks() - m_wait_start > OrderTimeout) {
            FailTurn("order did not settle before the deadline");
        }
        return false;
    }
    if (m_phase == Phase::REPLY) {
        std::string line;
        if (m_bridge.Poll(line)) {
            try {
                const auto reply = json::parse(line);
                if (!reply.is_object() || !reply.contains("id") || !reply["id"].is_number_unsigned() ||
                    reply["id"].get<uint64_t>() != m_request) {
                    FailTurn("reply has a stale or malformed observation ID");
                    return true;
                }
                m_phase = Phase::READY;
                m_result = ApplyCommand(reply.at("command"));
                SDL_Log("Policy team %u request %llu: %s", m_team, static_cast<unsigned long long>(m_request),
                        m_result.dump().c_str());
            } catch (const std::exception&) {
                FailTurn("malformed bridge reply");
            }
            return true;
        }
        if (!m_bridge.IsConnected() || SDL_GetTicks() - m_wait_start > ReplyTimeout) {
            FailTurn("bridge disconnected or reply deadline exceeded");
        }
        return false;
    }
    if (!IsSettled()) {
        if (SDL_GetTicks() - m_wait_start > OrderTimeout) FailTurn("simulation did not settle before the deadline");
        return false;
    }
    if (m_commands >= MaxCommandsPerTurn) {
        FailTurn("per-turn command budget exhausted");
        return true;
    }
    ++m_commands;
    const json request = {{"protocol", "max-policy-v1"}, {"episode", m_episode},
                          {"id", ++m_request}, {"state", GetObservation()}};
    if (!m_bridge.Send(request.dump())) {
        FailTurn("bridge unavailable or observation exceeds transport limit");
    } else {
        m_wait_start = SDL_GetTicks();
        m_phase = Phase::REPLY;
    }
    return true;
}
