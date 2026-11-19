#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agenticpp/cooldown.hpp"
#include "agenticpp/executor.hpp"
#include "agenticpp/llm_client.hpp"
#include "agenticpp/memory_store.hpp"
#include "agenticpp/policy.hpp"
#include "agenticpp/skills.hpp"
#include "agenticpp/types.hpp"
#include "agenticpp/verifier.hpp"

namespace agenticpp {

// Stage 1 is a serial, replay-driven phase machine. The worker stage will add
// the queue and per-incident workers behind the same interfaces.
class Agent {
public:
    Agent(AgentConfig config, MemoryStore& memory, LlmClient& llm, Policy& policy,
          CommandRunner& runner, Verifier& verifier, EventSink& sink,
          CooldownTable* cooldown = nullptr, SkillRegistry* skills = nullptr);

    // Copies an actionable line; run_once() then executes one incident, blocking.
    void on_line(std::string_view line);
    void run_once();

    [[nodiscard]] const std::vector<Transition>& trail() const noexcept;

private:
    void run_incident(std::string_view line);
    void transition(Phase from, Phase to, std::string reason);

    AgentConfig config_;
    MemoryStore& memory_;
    LlmClient& llm_;
    Policy& policy_;
    CommandRunner& runner_;
    Verifier& verifier_;
    EventSink& sink_;
    CooldownTable* cooldown_;
    SkillRegistry* skills_;

    std::optional<std::string> pending_line_;
    std::vector<Transition> trail_;
};

}  // namespace agenticpp
