#pragma once
#include <string>
#include <optional>
#include <cstdint>

namespace cli {

enum class EnergyModel { Linear = 0, NonLinear = 1, Endurance = 2, Unlimited = 3 };
enum class ConfigType  { Low, High };
enum class Strategy    { Random, Cyclic, Vns, Adaptive };
enum class DistanceType{ Manhattan, Euclidean };

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
inline const char* to_str(DistanceType d){
    return d == DistanceType::Manhattan ? "manhattan" : "euclidean";
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
    std::size_t   adaptive_segments          = 7;
    bool          adaptive_fixed_segments    = false;
    std::size_t   ejection_chain_iterations  = 0;
    double        destroy_rate               = 0.1;
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
