#ifndef POLICYAI_HPP
#define POLICYAI_HPP

#include <map>
#include <nlohmann/json.hpp>
#include "noneai.hpp"
#include "policybridge.hpp"
#include "unitinfo.hpp"

/** Experimental full-observation controller. Its external policy chooses a
 * command directly; this controller supplies no tactical choices or scores.
 * Version 1 supports a documented subset of the player command interface. */
class PolicyAi final : public NoneAi {
    enum class Phase { READY, REPLY, ORDER, END };
    Phase m_phase{Phase::READY};
    PolicyBridge m_bridge;
    uint64_t m_request{0};
    uint64_t m_episode{0};
    uint64_t m_wait_start{0};
    int32_t m_last_turn{0};
    int32_t m_commands{0};
    SmartPointer<UnitInfo> m_pending;
    std::map<uint16_t, nlohmann::ordered_json> m_sightings;
    nlohmann::ordered_json m_result{nullptr};
    bool IsSettled() const;
    void FailTurn(const char* reason);

public:
    explicit PolicyAi(uint16_t team) : NoneAi(team) {}
    bool SetupStrategy() override;
    void BeginTurn() override;
    bool CheckEndTurn() override;
    void FileLoad(SmartFileReader& file) override;

    /// Snapshot updates only observation memory, never game objects or RNG.
    nlohmann::ordered_json GetObservation();
    /// Validate first, then issue a normal engine order. No fallback strategy.
    nlohmann::ordered_json ApplyCommand(const nlohmann::ordered_json& command);
};

#endif
