#pragma once
#include <vector>
#include <string>
#include <optional>
#include <cmath>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include "cli.hpp"

// -----------------------------------------------------------------------
// TruckConfig
// -----------------------------------------------------------------------
struct TruckConfig {
    double speed    = 0.0;   // V_max (m/s)
    double capacity = 0.0;   // M_t   (kg)
};

// -----------------------------------------------------------------------
// DroneConfig – mirrors the Rust enum with precomputed fields
// -----------------------------------------------------------------------
struct DroneConfig {
    enum class Model { Linear, NonLinear, Endurance } model;

    // Shared raw data
    double takeoff_speed = 0;
    double cruise_speed  = 0;
    double landing_speed = 0;
    double altitude      = 0;
    double cap           = 0;
    double battery_val   = 1.0;
    double fixed_time_val= std::numeric_limits<double>::infinity();
    cli::ConfigType speed_type = cli::ConfigType::High;
    cli::ConfigType range_type = cli::ConfigType::High;

    // Linear precomputed
    double beta  = 0;
    double gamma = 0;

    // NonLinear precomputed
    double _vert_k1              = 0;
    double _vert_k2              = 0;
    double _vert_c2              = 0;
    double _vert_half_takeoff    = 0;
    double _vert_half_landing    = 0;
    double _vert_half_takeoff_2  = 0;
    double _vert_half_landing_2  = 0;
    double _hori_c12             = 0;
    double _hori_c4v3            = 0;
    double _hori_c42v4           = 0;
    double _hori_c5              = 0;

    // Endurance / all models
    double _takeoff_time = 0;
    double _landing_time = 0;

    static constexpr double W = 1.5;
    static constexpr double G = 9.8;

    double capacity()    const { return cap; }
    double battery()     const { return battery_val; }
    double fixed_time()  const { return fixed_time_val; }

    double takeoff_time() const { return _takeoff_time; }
    double landing_time() const { return _landing_time; }

    double cruise_time(double distance) const { return distance / cruise_speed; }

    double takeoff_power(double weight) const {
        switch(model) {
            case Model::Linear: return beta * weight + gamma;
            case Model::NonLinear: {
                double w = W + weight;
                return (_vert_k1 * w) * (_vert_half_takeoff + std::sqrt(_vert_half_takeoff_2 + _vert_k2 * w))
                       + _vert_c2 * std::pow(w, 1.5);
            }
            case Model::Endurance: return 0.0;
        }
        return 0.0;
    }

    double landing_power(double weight) const {
        switch(model) {
            case Model::Linear: return beta * weight + gamma;
            case Model::NonLinear: {
                double w = W + weight;
                return (_vert_k1 * w) * (_vert_half_landing + std::sqrt(_vert_half_landing_2 + _vert_k2 * w))
                       + _vert_c2 * std::pow(w, 1.5);
            }
            case Model::Endurance: return 0.0;
        }
        return 0.0;
    }

    double cruise_power(double weight) const {
        switch(model) {
            case Model::Linear: return beta * weight + gamma;
            case Model::NonLinear: {
                double temp = (W + weight) * G - _hori_c5;
                return _hori_c12 * std::pow(temp * temp + _hori_c42v4, 0.75) + _hori_c4v3;
            }
            case Model::Endurance: return 0.0;
        }
        return 0.0;
    }

    // Factory
    static DroneConfig make(const std::string& path,
                            cli::EnergyModel config,
                            cli::ConfigType speed_type,
                            cli::ConfigType range_type);
};

// -----------------------------------------------------------------------
// Config
// -----------------------------------------------------------------------
struct Config {
    std::size_t customers_count = 0;
    std::size_t trucks_count    = 0;
    std::size_t drones_count    = 0;

    std::vector<double> x;
    std::vector<double> y;
    std::vector<double> demands;
    std::vector<bool>   dronable;

    cli::DistanceType truck_distance = cli::DistanceType::Euclidean;
    cli::DistanceType drone_distance = cli::DistanceType::Euclidean;

    std::vector<std::vector<double>> truck_distances;
    std::vector<std::vector<double>> drone_distances;

    TruckConfig truck;
    DroneConfig drone;

    std::string          problem;
    cli::EnergyModel     config                    = cli::EnergyModel::Endurance;
    double               tabu_size_factor          = 0.75;
    std::size_t          adaptive_iterations       = 60;
    bool                 adaptive_fixed_iterations = false;
    std::size_t          adaptive_pull_elite_segments = 1;
    std::size_t          adaptive_segments         = 7;
    bool                 adaptive_fixed_segments   = false;
    std::size_t          ejection_chain_iterations = 0;
    double               destroy_rate              = 0.1;
    cli::ConfigType      speed_type                = cli::ConfigType::High;
    cli::ConfigType      range_type                = cli::ConfigType::High;
    double               waiting_time_limit        = 3600.0;
    cli::Strategy        strategy                  = cli::Strategy::Adaptive;
    cli::ElitePullStrategy elite_pull_strategy     = cli::ElitePullStrategy::Random;
    cli::ElitePullAcceptStrategy elite_pull_accept_strategy = cli::ElitePullAcceptStrategy::Always;
    cli::ElitePushStrategy elite_push_strategy     = cli::ElitePushStrategy::NewBest;
    // Selective accept: an offered elite within this % of the requester's
    // personal-best cost can still be accepted if it's more diverse than
    // average (see pick_for_dispatch in parallel.cpp). E.g. 1.0 = 1%.
    double               elite_pull_quality_tolerance_pct = 1.0;
    std::optional<std::size_t> fix_iteration       = std::nullopt;
    double               reset_after_factor        = 125.0;
    std::size_t          max_elite_size            = 0;
    double               elite_pool_factor         = 0.03;
    std::size_t          max_evaluations           = 0;
    double               penalty_exponent          = 0.5;
    bool                 single_truck_route        = false;
    bool                 single_drone_route        = false;
    bool                 verbose                   = false;
    std::string          outputs                   = "outputs/";
    bool                 disable_logging           = false;
    bool                 dry_run                   = false;
    std::string          extra                     = "";
    std::optional<uint64_t> seed                   = std::nullopt;
    double          gamma_1                        = 0.3;
    double          gamma_2                        = 0.2;
    double          gamma_3                        = 0.1;
    double          gamma_4                        = 0.3;
    bool            randomize_worker_adaptive_hyperparams = false;
    cli::EliteReplaceStrategy elite_replace_strategy = cli::EliteReplaceStrategy::SimilarityAware;
    bool            compact_output                 = false;
    std::string     run_id                         = "";
};

// Distance matrix helper
std::vector<std::vector<double>> distance_matrix(
    cli::DistanceType type,
    const std::vector<double>& x,
    const std::vector<double>& y);

// Build Config from CLI run args (parses problem file, config files)
Config build_config(const cli::RunArgs& args);

// Build Config from a previously serialized config JSON file
Config build_config_from_json(const std::string& json_path);

// Serialize to JSON (for writing -config.json output)
nlohmann::json config_to_json(const Config& cfg);

// Global singleton – initialized once from CLI args
extern Config* g_config_ptr;
inline Config& global_config() noexcept { return *g_config_ptr; }
void    set_global_config(Config cfg);
