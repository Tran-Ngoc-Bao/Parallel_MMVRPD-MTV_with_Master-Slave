#pragma once
#include <string>
#include <optional>
#include <cstdint>

namespace cli {

enum class EnergyModel { Linear = 0, NonLinear = 1, Endurance = 2, Unlimited = 3 };
enum class ConfigType  { Low, High };
enum class Strategy    { Random, Cyclic, Vns, Adaptive };
enum class ElitePullStrategy { Random, TopK, Rank, PullCount, Off };
enum class ElitePushStrategy { NewBest, SegmentBest, SignificantBest };
enum class EliteReplaceStrategy { SimilarityOnly, SimilarityQuality, SimilarityPullFirst };
enum class DistanceType{ Manhattan, Euclidean };
enum class WorkerHyperparams { Fixed, Random, Preset };

inline const char* to_str(EnergyModel m){
    switch(m){
        case EnergyModel::Linear:    return "linear";
        case EnergyModel::NonLinear: return "non-linear";
        case EnergyModel::Endurance: return "endurance";
        case EnergyModel::Unlimited: return "unlimited";
    }
    return "";
}
inline const char* to_str(ConfigType c){
    return c == ConfigType::Low ? "low" : "high";
}
inline const char* to_str(Strategy s){
    switch(s){
        case Strategy::Random:   return "random";
        case Strategy::Cyclic:   return "cyclic";
        case Strategy::Vns:      return "vns";
        case Strategy::Adaptive: return "adaptive";
    }
    return "";
}
inline const char* to_str(ElitePullStrategy s){
    switch(s){
        case ElitePullStrategy::Random:    return "random";
        case ElitePullStrategy::TopK:      return "topk";
        case ElitePullStrategy::Rank:      return "rank";
        case ElitePullStrategy::PullCount: return "pullcount";
        case ElitePullStrategy::Off:       return "off";
    }
    return "";
}
inline const char* to_str(ElitePushStrategy s){
    switch(s){
        case ElitePushStrategy::NewBest:        return "new-best";
        case ElitePushStrategy::SegmentBest:    return "segment-best";
        case ElitePushStrategy::SignificantBest:return "significant-best";
    }
    return "";
}
inline const char* to_str(EliteReplaceStrategy s){
    switch(s){
        case EliteReplaceStrategy::SimilarityOnly:      return "similarity-only";
        case EliteReplaceStrategy::SimilarityQuality:   return "similarity-quality";
        case EliteReplaceStrategy::SimilarityPullFirst: return "similarity-pull-first";
    }
    return "";
}
inline const char* to_str(DistanceType d){
    return d == DistanceType::Manhattan ? "manhattan" : "euclidean";
}
inline const char* to_str(WorkerHyperparams w){
    switch(w){
        case WorkerHyperparams::Fixed:  return "fixed";
        case WorkerHyperparams::Random: return "random";
        case WorkerHyperparams::Preset: return "preset";
    }
    return "fixed";
}

// ---------------------------------------------------------------------
// Run sub-command options
// ---------------------------------------------------------------------
struct RunArgs {
    std::string   problem;
    std::string   truck_cfg                  = "problems/config_parameter/truck_config.json";
    std::string   drone_cfg                  = "problems/config_parameter/drone_endurance_config.json";
    EnergyModel   config                     = EnergyModel::Endurance;
    double        tabu_size_factor           = 0.75;
    std::size_t   adaptive_iterations        = 60;
    bool          adaptive_fixed_iterations  = false;
    std::size_t   adaptive_pull_elite_segments = 1;
    bool          adaptive_fixed_segments    = false;
    std::size_t   ejection_chain_iterations  = 0;
    double        destroy_rate               = 0.1;
    double        elite_pool_factor          = 0.03;
    ElitePullStrategy elite_pull_strategy    = ElitePullStrategy::Random;
    ElitePushStrategy elite_push_strategy    = ElitePushStrategy::NewBest;
    ConfigType    speed_type                 = ConfigType::High;
    ConfigType    range_type                 = ConfigType::High;
    DistanceType  truck_distance             = DistanceType::Euclidean;
    DistanceType  drone_distance             = DistanceType::Euclidean;
    std::optional<std::size_t> trucks_count  = std::nullopt;
    std::optional<std::size_t> drones_count  = std::nullopt;
    double        waiting_time_limit         = 3600.0;
    Strategy      strategy                   = Strategy::Adaptive;
    std::optional<std::size_t> fix_iteration = std::nullopt;
    double        reset_after_factor         = 125.0;
    std::size_t   max_elite_size             = 0;
    double        time_limit                 = 0.0;
    std::size_t   max_evaluations            = 0;
    double        penalty_exponent           = 0.5;
    bool          single_truck_route         = false;
    bool          single_drone_route         = false;
    bool          verbose                    = false;
    std::string   outputs                    = "outputs/";
    bool          disable_logging            = false;
    bool          dry_run                    = false;
    std::string   extra                      = "";
    std::optional<uint64_t> seed             = std::nullopt;
    double        gamma_1                    = 0.3;
    double        gamma_2                    = 0.2;
    double        gamma_3                    = 0.1;
    double        gamma_4                    = 0.3;
    double        min_pull_elites_per_worker_factor = 1.0;
    WorkerHyperparams worker_hyperparams     = WorkerHyperparams::Fixed;
    bool          randomize_worker_adaptive_hyperparams = false;
    EliteReplaceStrategy elite_replace_strategy = EliteReplaceStrategy::SimilarityOnly;
    bool          compact_output             = false;
    std::string   run_id                     = "";
};

struct EvaluateArgs {
    std::string solution;
    std::string config;
};

enum class CommandType { Run, Evaluate };

struct Arguments {
    CommandType cmd;
    RunArgs     run;
    EvaluateArgs evaluate;
};

} // namespace cli
