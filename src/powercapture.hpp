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

#ifndef POWERCAPTURE_HPP
#define POWERCAPTURE_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "enums.hpp"

namespace PowerCapture {

// Capture rules (shared by human and AI players):
// - Capture starts only when a new friendly building or Connector completes its
//   connection. Loading, destruction, and power switching do not trigger capture.
// - Only contact made by the newly completed structure starts a capture; unrelated
//   existing enemy adjacency elsewhere in the powered complex is unaffected.
// - Connections use side-adjacent completed buildings and Connectors, never diagonals.
// - The capturing complex must contain an operating Power Generator or Power Station.
// - Capture stops at the enemy physical group directly touched by construction.
//   Existing friendly islands and other enemy groups do not start chain captures.
// - Connected groups follow reciprocal physical connector links. A shared saved
//   Complex record alone never joins disconnected structures for capture.
// - Any operating enemy power source protects its entire connected enemy complex.
// - Switched-off generators and stations are civilian capture targets, including
//   isolated ones. Another active source in their complex still protects them.
// - A Radar or any of the four static weapon types protects its entire complex,
//   even when the military structure is disabled. Capture cannot pass through it.
// - An eligible connected group transfers together into the captor's existing
//   powered complex. Evaluate the whole snapshot before changing any live ownership.
// - Protected enemies may have friendly buildings or Connectors beside them, but
//   no cross-owner connection is created. Mobile units are outside these rules.

// A snapshot of completed structures. Network numbers identify physical groups
// derived by FindNetworks, rather than saved Complex record IDs. Ownership is
// decided for the entire snapshot before any live connections change.
struct Structure {
    uint16_t id;
    uint16_t team;
    size_t network;
    int32_t x;
    int32_t y;
    int32_t size;
    bool power_source;
    bool power_on;
    bool military{false};
    uint16_t connectors{};
};

// Derive physical components instead of trusting saved Complex pointers: old
// records can contain distant, disconnected members, or split a live path across
// several records. Both ends must advertise a link and belong to the same team.
inline std::vector<size_t> FindNetworks(const std::vector<Structure>& structures) {
    std::map<std::pair<int32_t, int32_t>, size_t> cells;
    for (size_t i = 0; i < structures.size(); ++i) {
        const auto& unit = structures[i];
        for (int32_t dx = 0; dx < unit.size; ++dx) {
            for (int32_t dy = 0; dy < unit.size; ++dy) {
                cells.emplace(std::pair{unit.x + dx, unit.y + dy}, i);
            }
        }
    }

    std::vector<std::vector<size_t>> links(structures.size());
    for (size_t i = 0; i < structures.size(); ++i) {
        const auto& unit = structures[i];
        auto connect = [&](int32_t x, int32_t y, uint16_t port, uint16_t opposite, bool horizontal) {
            if (!(unit.connectors & port)) {
                return;
            }
            auto cell = cells.find({x, y});
            if (cell == cells.end()) {
                return;
            }
            const auto j = cell->second;
            const auto& neighbor = structures[j];
            const int32_t offset = horizontal ? x - neighbor.x : y - neighbor.y;
            if (neighbor.team == unit.team && (neighbor.connectors & (opposite << offset))) {
                links[i].push_back(j);
                links[j].push_back(i);
            }
        };
        for (int32_t offset = 0; offset < unit.size; ++offset) {
            connect(unit.x + offset, unit.y - 1, CONNECTOR_NORTH_LEFT << offset, CONNECTOR_SOUTH_LEFT, true);
            connect(unit.x + offset, unit.y + unit.size, CONNECTOR_SOUTH_LEFT << offset, CONNECTOR_NORTH_LEFT, true);
            connect(unit.x - 1, unit.y + offset, CONNECTOR_WEST_TOP << offset, CONNECTOR_EAST_TOP, false);
            connect(unit.x + unit.size, unit.y + offset, CONNECTOR_EAST_TOP << offset, CONNECTOR_WEST_TOP, false);
        }
    }

    std::vector<size_t> networks(structures.size(), SIZE_MAX);
    size_t next_network = 0;
    for (size_t i = 0; i < structures.size(); ++i) {
        if (networks[i] != SIZE_MAX) {
            continue;
        }
        networks[i] = next_network;
        std::vector<size_t> queue{i};
        for (size_t head = 0; head < queue.size(); ++head) {
            for (auto next : links[queue[head]]) {
                if (networks[next] == SIZE_MAX) {
                    networks[next] = next_network;
                    queue.push_back(next);
                }
            }
        }
        ++next_network;
    }
    return networks;
}

struct PlanResult {
    std::vector<uint16_t> owners;
    std::vector<size_t> power_networks;
};

inline PlanResult Plan(const std::vector<Structure>& structures, const std::vector<uint16_t>& completed_units) {
    struct Network {
        uint16_t team{};
        uint16_t first_id{UINT16_MAX};
        bool protected_from_capture{};
        bool powered{};
        std::vector<size_t> contacts;
    };

    size_t network_count = 0;
    for (const auto& structure : structures) {
        network_count = std::max(network_count, structure.network + 1);
    }

    std::vector<Network> networks(network_count);
    std::map<std::pair<int32_t, int32_t>, size_t> cells;
    for (const auto& structure : structures) {
        auto& network = networks[structure.network];
        network.team = structure.team;
        network.first_id = std::min(network.first_id, structure.id);
        network.protected_from_capture |= (structure.power_source && structure.power_on) || structure.military;
        network.powered |= structure.power_source && structure.power_on;
        for (int32_t x = structure.x; x < structure.x + structure.size; ++x) {
            for (int32_t y = structure.y; y < structure.y + structure.size; ++y) {
                cells.emplace(std::pair{x, y}, structure.network);
            }
        }
    }

    std::vector<size_t> seeds;
    for (const auto& structure : structures) {
        const bool completed =
            std::find(completed_units.begin(), completed_units.end(), structure.id) != completed_units.end();
        if (completed) {
            seeds.push_back(structure.network);
        }
        auto connect = [&](int32_t x, int32_t y) {
            auto it = cells.find({x, y});
            if (it != cells.end() && it->second != structure.network) {
                if (completed) {
                    networks[structure.network].contacts.push_back(it->second);
                }
            }
        };
        for (int32_t offset = 0; offset < structure.size; ++offset) {
            connect(structure.x + offset, structure.y - 1);
            connect(structure.x + offset, structure.y + structure.size);
            connect(structure.x - 1, structure.y + offset);
            connect(structure.x + structure.size, structure.y + offset);
        }
    }

    std::sort(seeds.begin(), seeds.end(),
              [&](size_t a, size_t b) { return networks[a].first_id < networks[b].first_id; });
    seeds.erase(std::unique(seeds.begin(), seeds.end()), seeds.end());

    std::vector<size_t> claims(network_count, SIZE_MAX);
    for (auto seed : seeds) {
        if (!networks[seed].powered || claims[seed] != SIZE_MAX) {
            continue;
        }
        const auto team = networks[seed].team;
        claims[seed] = seed;
        // Only this construction's direct enemy contacts are capture candidates.
        // The contact's physical network already contains every structure linked
        // to it. Traversing further would capture other complexes through old
        // friendly connectors or previously blocked cross-owner adjacency.
        for (auto next : networks[seed].contacts) {
            if (claims[next] == SIZE_MAX && networks[next].team != team && !networks[next].protected_from_capture) {
                claims[next] = seed;
            }
        }
    }

    PlanResult result;
    result.owners.reserve(structures.size());
    result.power_networks.reserve(structures.size());
    for (const auto& structure : structures) {
        const auto claim = claims[structure.network];
        result.owners.push_back(claim == SIZE_MAX ? structure.team : networks[claim].team);
        result.power_networks.push_back(claim == SIZE_MAX ? structure.network : claim);
    }
    return result;
}

}  // namespace PowerCapture

#endif /* POWERCAPTURE_HPP */
