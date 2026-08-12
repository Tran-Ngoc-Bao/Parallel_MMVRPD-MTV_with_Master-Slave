#pragma once
#include <string>
#include <vector>
#include <cstddef>
#include <unordered_set>
#include <algorithm>
#include <utility>

enum class Neighborhood {
    Move10,
    Move11,
    Move20,
    Move21,
    Move22,
    TwoOpt,
    EjectionChain
};

inline std::string neighborhood_to_str(Neighborhood n) {
    switch(n) {
        case Neighborhood::Move10:        return "Move (1, 0)";
        case Neighborhood::Move11:        return "Move (1, 1)";
        case Neighborhood::Move20:        return "Move (2, 0)";
        case Neighborhood::Move21:        return "Move (2, 1)";
        case Neighborhood::Move22:        return "Move (2, 2)";
        case Neighborhood::TwoOpt:        return "2-opt";
        case Neighborhood::EjectionChain: return "Ejection-chain";
    }
    return "";
}

// -----------------------------------------------------------------------
// TabuSet
// -----------------------------------------------------------------------
struct VectorSizeTHash {
    std::size_t operator()(const std::vector<size_t>& v) const noexcept {
        std::size_t h = v.size();
        for (size_t x : v) {
            h ^= std::hash<size_t>{}(x) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        }
        return h;
    }
};

class TabuSet {
public:
    bool contains(const std::vector<size_t>& v) const {
        return set_.find(v) != set_.end();
    }

    // Mirrors the original logic: rotate an existing entry to the back
    // (extends its life), otherwise insert at the back and evict the
    // oldest entry if over capacity.
    void touch(std::vector<size_t> v, std::size_t max_size) {
        auto it = std::find(order_.begin(), order_.end(), v);
        if (it != order_.end()) {
            std::rotate(it, it + 1, order_.end());
            return;
        }
        set_.insert(v);
        order_.push_back(std::move(v));
        if (order_.size() > max_size) {
            set_.erase(order_.front());
            order_.erase(order_.begin());
        }
    }

    void clear() { order_.clear(); set_.clear(); }

    // Insertion-ordered view, used only for CSV debug logging (cold path).
    const std::vector<std::vector<size_t>>& entries() const { return order_; }

private:
    std::vector<std::vector<size_t>> order_;
    std::unordered_set<std::vector<size_t>, VectorSizeTHash> set_;
};

// Forward declare Solution to break circular dependency
struct Solution;

namespace neighborhoods {
    // Returns {best_solution, tabu_entry}. `evaluations` is set to the
    // number of candidate neighbor solutions assessed during this call.
    std::pair<Solution, std::vector<size_t>> inter_route(
        Neighborhood n,
        const Solution& solution,
        const TabuSet& tabu_list,
        double aspiration_cost,
        std::size_t& evaluations);

    std::pair<Solution, std::vector<size_t>> intra_route(
        Neighborhood n,
        const Solution& solution,
        const TabuSet& tabu_list,
        double aspiration_cost,
        std::size_t& evaluations);

    // Returns nullopt if no improvement found (both neighborhoods empty)
    // Otherwise returns the best neighbor and updates tabu_list.
    // `evaluations` is set to the total number of candidate neighbor
    // solutions assessed during this call (intra + inter route).
    bool search(
        Neighborhood n,
        const Solution& solution,
        TabuSet& tabu_list,
        size_t tabu_size,
        double aspiration_cost,
        Solution& out_result,
        std::size_t& evaluations);
} // namespace neighborhoods
