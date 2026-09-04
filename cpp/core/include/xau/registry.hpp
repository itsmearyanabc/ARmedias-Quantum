// The list of strategies the tooling knows how to run.
//
// One list, shared by run_baselines and the terminal. Two lists would drift,
// and the failure mode is quiet: a strategy tuned in one place and evaluated in
// the other, with nothing to indicate they were different.
#pragma once

#include "xau/strategy.hpp"

#include <functional>
#include <memory>
#include <span>

namespace xau {

struct BaselineEntry {
    const char* name;
    const char* description;
    // True for the four rule baselines from PLAN section 7. The null model and
    // buy-and-hold are runnable from the terminal but are not strategies, and
    // letting them satisfy the Phase 3 gate would be nonsense.
    bool gate_candidate;
    // Lots is a parameter rather than baked in because position size belongs to
    // the caller, not the strategy: the risk layer owns it from Phase 7.
    std::function<std::unique_ptr<Strategy>(double lots)> make;
};

// Stable for the life of the program; entries may be referenced freely.
[[nodiscard]] std::span<const BaselineEntry> baseline_registry();

// A nullary factory bound to a lot size, for walk-forward and repeated runs.
// Copies the maker rather than capturing the entry by reference, so the result
// stays valid regardless of what happens to the registry span.
[[nodiscard]] inline StrategyFactory factory_for(const BaselineEntry& entry, double lots) {
    auto make = entry.make;
    return [make, lots] { return make(lots); };
}

// Looks a strategy up by name. Returns nullptr when there is no such entry —
// the caller decides whether that is an error, because the terminal wants to
// fall back and a batch tool wants to stop.
[[nodiscard]] const BaselineEntry* find_baseline(const char* name);

}  // namespace xau
