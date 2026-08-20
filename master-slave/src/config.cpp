#include "config.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <regex>
#include <stdexcept>
#include <cmath>
#include <numbers>
#include <optional>

// -----------------------------------------------------------------------
// Distance matrix
// -----------------------------------------------------------------------
std::vector<std::vector<double>> distance_matrix(
    cli::DistanceType type,
    const std::vector<double>& x,
    const std::vector<double>& y)
{
    std::size_t n = x.size();
    std::vector<std::vector<double>> m(n, std::vector<double>(n, 0.0));
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            double dx = x[i] - x[j];
            double dy = y[i] - y[j];
            if (type == cli::DistanceType::Manhattan)
                m[i][j] = std::abs(dx) + std::abs(dy);
            else
                m[i][j] = std::sqrt(dx*dx + dy*dy);
        }
    }
    return m;
}

// -----------------------------------------------------------------------
// DroneConfig::make
// -----------------------------------------------------------------------
DroneConfig DroneConfig::make(
    const std::string& path,
    cli::EnergyModel   config,
    cli::ConfigType    speed_type,
    cli::ConfigType    range_type)
{
    auto read_file = [](const std::string& p) -> std::string {
        std::ifstream f(p);
        if (!f) throw std::runtime_error("Cannot open file: " + p);
        return std::string((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
    };

    auto match_cfg = [&](const nlohmann::json& j) -> bool {
        auto s = j.at("speed_type").get<std::string>();
        auto r = j.at("range_type").get<std::string>();
        bool sp = (speed_type == cli::ConfigType::Low) ? (s == "low") : (s == "high");
        bool rp = (range_type == cli::ConfigType::Low) ? (r == "low") : (r == "high");
        return sp && rp;
    };

    DroneConfig dc;

    if (config == cli::EnergyModel::Linear) {
        auto arr = nlohmann::json::parse(read_file(path));
        for (auto& entry : arr) {
            if (!match_cfg(entry)) continue;
            dc.model         = Model::Linear;
            dc.takeoff_speed = entry.at("takeoffSpeed [m/s]").get<double>();
            dc.cruise_speed  = entry.at("cruiseSpeed [m/s]").get<double>();
            dc.landing_speed = entry.at("landingSpeed [m/s]").get<double>();
            dc.altitude      = entry.at("cruiseAlt [m]").get<double>();
            dc.cap           = entry.at("capacity [kg]").get<double>();
            dc.battery_val   = entry.at("batteryPower [Joule]").get<double>();
            dc.speed_type    = speed_type;
            dc.range_type    = range_type;
            dc.beta          = entry.at("beta(w/kg)").get<double>();
            dc.gamma         = entry.at("gamma(w)").get<double>();
            dc._takeoff_time = dc.altitude / dc.takeoff_speed;
            dc._landing_time = dc.altitude / dc.landing_speed;
            dc.fixed_time_val= std::numeric_limits<double>::infinity();
            return dc;
        }
        throw std::runtime_error("No matching linear config");
    }

    if (config == cli::EnergyModel::NonLinear) {
        auto root = nlohmann::json::parse(read_file(path));
        double k1 = root.at("k1").get<double>();
        double k2 = root.at("k2 (sqrt(kg/m))").get<double>();
        double c1 = root.at("c1 (sqrt(m/kg))").get<double>();
        double c2 = root.at("c2 (sqrt(m/kg))").get<double>();
        double c4 = root.at("c4 (kg/m)").get<double>();
        double c5 = root.at("c5 (Ns/m)").get<double>();

        for (auto& entry : root.at("config")) {
            if (!match_cfg(entry)) continue;
            dc.model         = Model::NonLinear;
            dc.takeoff_speed = entry.at("takeoffSpeed [m/s]").get<double>();
            dc.cruise_speed  = entry.at("cruiseSpeed [m/s]").get<double>();
            dc.landing_speed = entry.at("landingSpeed [m/s]").get<double>();
            dc.altitude      = entry.at("cruiseAlt [m]").get<double>();
            dc.cap           = entry.at("capacity [kg]").get<double>();
            dc.battery_val   = entry.at("batteryPower [Joule]").get<double>();
            dc.speed_type    = speed_type;
            dc.range_type    = range_type;

            dc._vert_k1            = k1 * G;
            dc._vert_k2            = G / (k2 * k2);
            dc._vert_c2            = c2 * std::pow(G, 1.5);
            dc._vert_half_takeoff  = dc.takeoff_speed / 2.0;
            dc._vert_half_landing  = dc.landing_speed / 2.0;
            dc._vert_half_takeoff_2= dc._vert_half_takeoff * dc._vert_half_takeoff;
            dc._vert_half_landing_2= dc._vert_half_landing * dc._vert_half_landing;
            dc._hori_c12           = c1 + c2;
            dc._hori_c4v3          = c4 * dc.cruise_speed * dc.cruise_speed * dc.cruise_speed;
            dc._hori_c42v4         = c4*c4 * dc.cruise_speed*dc.cruise_speed
                                            * dc.cruise_speed*dc.cruise_speed;
            {
                double deg10  = M_PI / 18.0;
                double cv     = dc.cruise_speed * std::cos(deg10);
                dc._hori_c5   = c5 * cv * cv;
            }
            dc._takeoff_time = dc.altitude / dc.takeoff_speed;
            dc._landing_time = dc.altitude / dc.landing_speed;
            dc.fixed_time_val= std::numeric_limits<double>::infinity();
            return dc;
        }
        throw std::runtime_error("No matching non-linear config");
    }

    if (config == cli::EnergyModel::Endurance) {
        auto arr = nlohmann::json::parse(read_file(path));
        for (auto& entry : arr) {
            if (!match_cfg(entry)) continue;
            dc.model         = Model::Endurance;
            dc.cap           = entry.at("capacity [kg]").get<double>();
            dc.fixed_time_val= entry.at("FixedTime (s)").get<double>();
            dc.cruise_speed  = entry.at("V_max (m/s)").get<double>();
            dc.speed_type    = speed_type;
            dc.range_type    = range_type;
            dc.battery_val   = 1.0;
            dc._takeoff_time = 0.0;
            dc._landing_time = 0.0;
            return dc;
        }
        throw std::runtime_error("No matching endurance config");
    }

    // Unlimited
    dc.model         = Model::Endurance;
    dc.cap           = std::numeric_limits<double>::infinity();
    dc.fixed_time_val= std::numeric_limits<double>::infinity();
    dc.cruise_speed  = 1.0;
    dc.battery_val   = 1.0;
    dc._takeoff_time = 0.0;
    dc._landing_time = 0.0;
    dc.speed_type    = cli::ConfigType::High;
    dc.range_type    = cli::ConfigType::High;
    return dc;
}

// -----------------------------------------------------------------------
// build_config from RunArgs
// -----------------------------------------------------------------------
Config build_config(const cli::RunArgs& args)
{
    auto read_file = [](const std::string& p) -> std::string {
        std::ifstream f(p);
        if (!f) throw std::runtime_error("Cannot open file: " + p);
        return std::string((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
    };

    std::string data = read_file(args.problem);

    // Parse trucks / drones count
    auto parse_uint = [](const std::string& s) -> std::size_t {
        return static_cast<std::size_t>(std::stoul(s));
    };

    std::optional<std::size_t> trucks_count = args.trucks_count;
    std::optional<std::size_t> drones_count = args.drones_count;

    {
        std::regex re_trucks(R"(trucks_count (\d+))");
        std::smatch m;
        if (!trucks_count && std::regex_search(data, m, re_trucks))
            trucks_count = parse_uint(m[1].str());
    }
    {
        std::regex re_drones(R"(drones_count (\d+))");
        std::smatch m;
        if (!drones_count && std::regex_search(data, m, re_drones))
            drones_count = parse_uint(m[1].str());
    }
    if (!trucks_count) throw std::runtime_error("Missing trucks count");
    if (!drones_count) throw std::runtime_error("Missing drones count");

    // Parse depot
    std::pair<double,double> depot;
    {
        std::regex re_depot(R"(depot (-?[\d\.]+)\s+(-?[\d\.]+))");
        std::smatch m;
        if (!std::regex_search(data, m, re_depot))
            throw std::runtime_error("Missing depot coordinates");
        depot = { std::stod(m[1].str()), std::stod(m[2].str()) };
    }

    // Parse customers (multiline)
    std::size_t customers_count = 0;
    std::vector<double> x   = { depot.first  };
    std::vector<double> y   = { depot.second };
    std::vector<double> demands = { 0.0 };
    std::vector<bool>   dronable = { true };

    {
        std::regex re_cust(
            R"(^\s*(-?[\d\.]+)\s+(-?[\d\.]+)\s+(0|1)\s+([\d\.]+)\s*$)",
            std::regex::multiline);
        auto it  = std::sregex_iterator(data.begin(), data.end(), re_cust);
        auto end = std::sregex_iterator();
        for (; it != end; ++it) {
            ++customers_count;
            const std::smatch& cm = *it;
            x.push_back(std::stod(cm[1].str()));
            y.push_back(std::stod(cm[2].str()));
            dronable.push_back(cm[3].str() == "1");
            demands.push_back(std::stod(cm[4].str()));
        }
    }

    auto truck_distances = distance_matrix(args.truck_distance, x, y);
    auto drone_distances = distance_matrix(args.drone_distance, x, y);

    // Parse truck config
    TruckConfig truck;
    {
        auto j = nlohmann::json::parse(read_file(args.truck_cfg));
        truck.speed    = j.at("V_max (m/s)").get<double>();
        truck.capacity = j.at("M_t (kg)").get<double>();
    }

    DroneConfig drone = DroneConfig::make(args.drone_cfg, args.config,
                                          args.speed_type, args.range_type);

    // Refine dronable based on drone constraints
    {
        double takeoff            = drone.takeoff_time();
        double landing            = drone.landing_time();
        double takeoff_from_depot = drone.takeoff_power(0.0);
        double landing_from_depot = drone.landing_power(0.0);
        double cruise_from_depot  = drone.cruise_power(0.0);

        for (std::size_t i = 1; i <= customers_count; ++i) {
            if (!dronable[i]) continue;
            double dem = demands[i];
            // capacity check
            if (dem > drone.capacity()) { dronable[i] = false; continue; }
            // fixed time check
            double cruise_dist = drone_distances[0][i] + drone_distances[i][0];
            if (takeoff + drone.cruise_time(cruise_dist) + landing > drone.fixed_time()) {
                dronable[i] = false; continue;
            }
            // energy check
            double energy =
                (landing_from_depot + drone.landing_power(dem)) * landing
                + drone.cruise_power(dem) * drone.cruise_time(drone_distances[i][0])
                + (takeoff_from_depot + drone.takeoff_power(dem)) * takeoff
                + cruise_from_depot * drone.cruise_time(drone_distances[0][i]);
            if (energy > drone.battery()) { dronable[i] = false; }
        }
    }

    Config cfg;
    cfg.customers_count           = customers_count;
    cfg.trucks_count              = *trucks_count;
    cfg.drones_count              = *drones_count;
    cfg.x                         = std::move(x);
    cfg.y                         = std::move(y);
    cfg.demands                   = std::move(demands);
    cfg.dronable                  = std::move(dronable);
    cfg.truck_distance            = args.truck_distance;
    cfg.drone_distance            = args.drone_distance;
    cfg.truck_distances           = std::move(truck_distances);
    cfg.drone_distances           = std::move(drone_distances);
    cfg.truck                     = truck;
    cfg.drone                     = drone;
    cfg.problem                   = args.problem;
    cfg.config                    = args.config;
    cfg.tabu_size_factor          = args.tabu_size_factor;
    cfg.adaptive_iterations       = args.adaptive_iterations;
    cfg.adaptive_fixed_iterations = args.adaptive_fixed_iterations;
    cfg.adaptive_pull_elite_segments = args.adaptive_pull_elite_segments;
    cfg.adaptive_segments         = args.adaptive_segments;
    cfg.adaptive_fixed_segments   = args.adaptive_fixed_segments;
    cfg.ejection_chain_iterations = args.ejection_chain_iterations;
    cfg.destroy_rate              = args.destroy_rate;
    cfg.elite_pool_factor         = args.elite_pool_factor;
    cfg.elite_pool_size           = args.elite_pool_size;
    cfg.speed_type                = args.speed_type;
    cfg.range_type                = args.range_type;
    cfg.waiting_time_limit        = args.waiting_time_limit;
    cfg.strategy                  = args.strategy;
    cfg.elite_pull_strategy       = args.elite_pull_strategy;
    cfg.elite_pull_accept_strategy = args.elite_pull_accept_strategy;
    cfg.elite_push_strategy       = args.elite_push_strategy;
    cfg.elite_pull_quality_tolerance_pct = args.elite_pull_quality_tolerance_pct;
    cfg.fix_iteration             = args.fix_iteration;
    cfg.reset_after_factor        = args.reset_after_factor;
    cfg.max_elite_size            = args.max_elite_size;
    cfg.max_evaluations           = args.max_evaluations;
    cfg.penalty_exponent          = args.penalty_exponent;
    cfg.single_truck_route        = args.single_truck_route;
    cfg.single_drone_route        = args.single_drone_route;
    cfg.verbose                   = args.verbose;
    cfg.outputs                   = args.outputs;
    cfg.disable_logging           = args.disable_logging;
    cfg.dry_run                   = args.dry_run;
    cfg.extra                     = args.extra;
    cfg.seed                      = args.seed;
    cfg.gamma_1                   = args.gamma_1;
    cfg.gamma_2                   = args.gamma_2;
    cfg.gamma_3                   = args.gamma_3;
    cfg.gamma_4                   = args.gamma_4;
    cfg.randomize_worker_adaptive_hyperparams = args.randomize_worker_adaptive_hyperparams;
    cfg.elite_replace_strategy    = args.elite_replace_strategy;
    cfg.compact_output            = args.compact_output;
    cfg.run_id                    = args.run_id;
    return cfg;
}

// -----------------------------------------------------------------------
// JSON helpers for config serialization
// -----------------------------------------------------------------------
static std::string energy_model_str(cli::EnergyModel m) {
    switch(m) {
        case cli::EnergyModel::Linear:    return "linear";
        case cli::EnergyModel::NonLinear: return "non-linear";
        case cli::EnergyModel::Endurance: return "endurance";
        case cli::EnergyModel::Unlimited: return "unlimited";
    }
    return "";
}
static std::string config_type_str(cli::ConfigType c) {
    return c == cli::ConfigType::Low ? "low" : "high";
}
static std::string strategy_str(cli::Strategy s) {
    switch(s) {
        case cli::Strategy::Random:   return "random";
        case cli::Strategy::Cyclic:   return "cyclic";
        case cli::Strategy::Vns:      return "vns";
        case cli::Strategy::Adaptive: return "adaptive";
    }
    return "";
}
static std::string elite_pull_strategy_str(cli::ElitePullStrategy s) {
    switch(s) {
        case cli::ElitePullStrategy::Random:    return "random";
        case cli::ElitePullStrategy::TopK:      return "topk";
        case cli::ElitePullStrategy::Rank:      return "rank";
        case cli::ElitePullStrategy::PullCount: return "pullcount";
        case cli::ElitePullStrategy::Off:       return "off";
    }
    return "";
}
static std::string elite_pull_accept_strategy_str(cli::ElitePullAcceptStrategy s) {
    return s == cli::ElitePullAcceptStrategy::Selective ? "selective" : "always";
}
static std::string elite_push_strategy_str(cli::ElitePushStrategy s) {
    switch(s) {
        case cli::ElitePushStrategy::NewBest:         return "new-best";
        case cli::ElitePushStrategy::SegmentBest:     return "segment-best";
        case cli::ElitePushStrategy::SignificantBest: return "significant-best";
    }
    return "";
}
static std::string elite_replace_strategy_str(cli::EliteReplaceStrategy s) {
    switch(s) {
        case cli::EliteReplaceStrategy::TDCrowding:   return "td-crowding";
        case cli::EliteReplaceStrategy::EdgeCrowding: return "edge-crowding";
        case cli::EliteReplaceStrategy::QualityOnly:  return "quality-only";
        case cli::EliteReplaceStrategy::RandomTarget: return "random-target";
    }
    return "";
}
static std::string distance_str(cli::DistanceType d) {
    return d == cli::DistanceType::Manhattan ? "manhattan" : "euclidean";
}

static nlohmann::json drone_config_to_json(const DroneConfig& dc) {
    nlohmann::json j;
    std::string speed = config_type_str(dc.speed_type);
    std::string range = config_type_str(dc.range_type);
    switch (dc.model) {
        case DroneConfig::Model::Linear:
            j["config"] = "Linear";
            j["_data"] = {
                {"takeoffSpeed [m/s]", dc.takeoff_speed},
                {"cruiseSpeed [m/s]",  dc.cruise_speed},
                {"landingSpeed [m/s]", dc.landing_speed},
                {"cruiseAlt [m]",      dc.altitude},
                {"capacity [kg]",      dc.cap},
                {"batteryPower [Joule]", dc.battery_val},
                {"speed_type",         speed},
                {"range_type",         range},
                {"beta(w/kg)",         dc.beta},
                {"gamma(w)",           dc.gamma}
            };
            j["_takeoff_time"] = dc._takeoff_time;
            j["_landing_time"] = dc._landing_time;
            break;
        case DroneConfig::Model::NonLinear:
            j["config"] = "NonLinear";
            j["_data"] = {
                {"takeoffSpeed [m/s]", dc.takeoff_speed},
                {"cruiseSpeed [m/s]",  dc.cruise_speed},
                {"landingSpeed [m/s]", dc.landing_speed},
                {"cruiseAlt [m]",      dc.altitude},
                {"capacity [kg]",      dc.cap},
                {"batteryPower [Joule]", dc.battery_val},
                {"speed_type",         speed},
                {"range_type",         range}
            };
            j["_vert_k1"]             = dc._vert_k1;
            j["_vert_k2"]             = dc._vert_k2;
            j["_vert_c2"]             = dc._vert_c2;
            j["_vert_half_takeoff"]   = dc._vert_half_takeoff;
            j["_vert_half_landing"]   = dc._vert_half_landing;
            j["_vert_half_takeoff_2"] = dc._vert_half_takeoff_2;
            j["_vert_half_landing_2"] = dc._vert_half_landing_2;
            j["_hori_c12"]            = dc._hori_c12;
            j["_hori_c4v3"]           = dc._hori_c4v3;
            j["_hori_c42v4"]          = dc._hori_c42v4;
            j["_hori_c5"]             = dc._hori_c5;
            j["_takeoff_time"]        = dc._takeoff_time;
            j["_landing_time"]        = dc._landing_time;
            break;
        case DroneConfig::Model::Endurance:
            j["config"] = "Endurance";
            j["_data"] = {
                {"speed_type",         speed},
                {"range_type",         range},
                {"capacity [kg]",      dc.cap},
                {"FixedTime (s)",      dc.fixed_time_val},
                {"V_max (m/s)",        dc.cruise_speed}
            };
            break;
    }
    return j;
}

nlohmann::json config_to_json(const Config& cfg) {
    nlohmann::json j;
    j["customers_count"]           = cfg.customers_count;
    j["trucks_count"]              = cfg.trucks_count;
    j["drones_count"]              = cfg.drones_count;
    j["x"]                         = cfg.x;
    j["y"]                         = cfg.y;
    j["demands"]                   = cfg.demands;
    j["dronable"]                  = cfg.dronable;
    j["truck_distance"]            = distance_str(cfg.truck_distance);
    j["drone_distance"]            = distance_str(cfg.drone_distance);
    j["truck"]                     = {{"V_max (m/s)", cfg.truck.speed},
                                      {"M_t (kg)",    cfg.truck.capacity}};
    j["drone"]                     = drone_config_to_json(cfg.drone);
    j["problem"]                   = cfg.problem;
    j["config"]                    = energy_model_str(cfg.config);
    j["tabu_size_factor"]          = cfg.tabu_size_factor;
    j["adaptive_iterations"]       = cfg.adaptive_iterations;
    j["adaptive_fixed_iterations"] = cfg.adaptive_fixed_iterations;
    j["adaptive_pull_elite_segments"] = cfg.adaptive_pull_elite_segments;
    j["adaptive_segments"]         = cfg.adaptive_segments;
    j["adaptive_fixed_segments"]   = cfg.adaptive_fixed_segments;
    j["ejection_chain_iterations"] = cfg.ejection_chain_iterations;
    j["destroy_rate"]              = cfg.destroy_rate;
    j["elite_pool_factor"]         = cfg.elite_pool_factor;
    j["elite_pool_size"]           = cfg.elite_pool_size;
    j["speed_type"]                = config_type_str(cfg.speed_type);
    j["range_type"]                = config_type_str(cfg.range_type);
    j["waiting_time_limit"]        = cfg.waiting_time_limit;
    j["strategy"]                  = strategy_str(cfg.strategy);
    j["elite_pull_strategy"]       = elite_pull_strategy_str(cfg.elite_pull_strategy);
    j["elite_pull_accept_strategy"] = elite_pull_accept_strategy_str(cfg.elite_pull_accept_strategy);
    j["elite_push_strategy"]       = elite_push_strategy_str(cfg.elite_push_strategy);
    j["elite_pull_quality_tolerance_pct"] = cfg.elite_pull_quality_tolerance_pct;
    if (cfg.fix_iteration)
        j["fix_iteration"]         = *cfg.fix_iteration;
    else
        j["fix_iteration"]         = nullptr;
    j["reset_after_factor"]        = cfg.reset_after_factor;
    j["max_elite_size"]            = cfg.max_elite_size;
    j["max_evaluations"]           = cfg.max_evaluations;
    j["penalty_exponent"]          = cfg.penalty_exponent;
    j["single_truck_route"]        = cfg.single_truck_route;
    j["single_drone_route"]        = cfg.single_drone_route;
    j["verbose"]                   = cfg.verbose;
    j["outputs"]                   = cfg.outputs;
    j["disable_logging"]           = cfg.disable_logging;
    j["dry_run"]                   = cfg.dry_run;
    j["extra"]                     = cfg.extra;
    j["gamma_1"]                   = cfg.gamma_1;
    j["gamma_2"]                   = cfg.gamma_2;
    j["gamma_3"]                   = cfg.gamma_3;
    j["gamma_4"]                   = cfg.gamma_4;
    j["randomize_worker_adaptive_hyperparams"] = cfg.randomize_worker_adaptive_hyperparams;
    j["elite_replace_strategy"]    = elite_replace_strategy_str(cfg.elite_replace_strategy);
    j["compact_output"]            = cfg.compact_output;
    j["run_id"]                    = cfg.run_id;
    return j;
}

// -----------------------------------------------------------------------
// build_config from previously serialized JSON
// -----------------------------------------------------------------------
Config build_config_from_json(const std::string& json_path)
{
    std::ifstream f(json_path);
    if (!f) throw std::runtime_error("Cannot open config: " + json_path);
    nlohmann::json j;
    f >> j;

    Config cfg;
    cfg.customers_count           = j.at("customers_count").get<std::size_t>();
    cfg.trucks_count              = j.at("trucks_count").get<std::size_t>();
    cfg.drones_count              = j.at("drones_count").get<std::size_t>();
    cfg.x                         = j.at("x").get<std::vector<double>>();
    cfg.y                         = j.at("y").get<std::vector<double>>();
    cfg.demands                   = j.at("demands").get<std::vector<double>>();
    cfg.dronable                  = j.at("dronable").get<std::vector<bool>>();

    auto dist_from_str = [](const std::string& s) {
        return s == "manhattan" ? cli::DistanceType::Manhattan : cli::DistanceType::Euclidean;
    };
    cfg.truck_distance = dist_from_str(j.at("truck_distance").get<std::string>());
    cfg.drone_distance = dist_from_str(j.at("drone_distance").get<std::string>());
    cfg.truck_distances = distance_matrix(cfg.truck_distance, cfg.x, cfg.y);
    cfg.drone_distances = distance_matrix(cfg.drone_distance, cfg.x, cfg.y);

    auto tj = j.at("truck");
    cfg.truck.speed    = tj.at("V_max (m/s)").get<double>();
    cfg.truck.capacity = tj.at("M_t (kg)").get<double>();

    // Reconstruct DroneConfig from JSON
    {
        auto dj  = j.at("drone");
        std::string model_str = dj.at("config").get<std::string>();
        DroneConfig dc;
        auto st_str = [](const std::string& s) {
            return s == "low" ? cli::ConfigType::Low : cli::ConfigType::High;
        };
        if (model_str == "Linear") {
            dc.model         = DroneConfig::Model::Linear;
            auto d           = dj.at("_data");
            dc.takeoff_speed = d.at("takeoffSpeed [m/s]").get<double>();
            dc.cruise_speed  = d.at("cruiseSpeed [m/s]").get<double>();
            dc.landing_speed = d.at("landingSpeed [m/s]").get<double>();
            dc.altitude      = d.at("cruiseAlt [m]").get<double>();
            dc.cap           = d.at("capacity [kg]").get<double>();
            dc.battery_val   = d.at("batteryPower [Joule]").get<double>();
            dc.beta          = d.at("beta(w/kg)").get<double>();
            dc.gamma         = d.at("gamma(w)").get<double>();
            dc.speed_type    = st_str(d.at("speed_type").get<std::string>());
            dc.range_type    = st_str(d.at("range_type").get<std::string>());
            dc._takeoff_time = dj.at("_takeoff_time").get<double>();
            dc._landing_time = dj.at("_landing_time").get<double>();
            dc.fixed_time_val= std::numeric_limits<double>::infinity();
        } else if (model_str == "NonLinear") {
            dc.model              = DroneConfig::Model::NonLinear;
            auto d                = dj.at("_data");
            dc.takeoff_speed      = d.at("takeoffSpeed [m/s]").get<double>();
            dc.cruise_speed       = d.at("cruiseSpeed [m/s]").get<double>();
            dc.landing_speed      = d.at("landingSpeed [m/s]").get<double>();
            dc.altitude           = d.at("cruiseAlt [m]").get<double>();
            dc.cap                = d.at("capacity [kg]").get<double>();
            dc.battery_val        = d.at("batteryPower [Joule]").get<double>();
            dc.speed_type         = st_str(d.at("speed_type").get<std::string>());
            dc.range_type         = st_str(d.at("range_type").get<std::string>());
            dc._vert_k1           = dj.at("_vert_k1").get<double>();
            dc._vert_k2           = dj.at("_vert_k2").get<double>();
            dc._vert_c2           = dj.at("_vert_c2").get<double>();
            dc._vert_half_takeoff = dj.at("_vert_half_takeoff").get<double>();
            dc._vert_half_landing = dj.at("_vert_half_landing").get<double>();
            dc._vert_half_takeoff_2= dj.at("_vert_half_takeoff_2").get<double>();
            dc._vert_half_landing_2= dj.at("_vert_half_landing_2").get<double>();
            dc._hori_c12          = dj.at("_hori_c12").get<double>();
            dc._hori_c4v3         = dj.at("_hori_c4v3").get<double>();
            dc._hori_c42v4        = dj.at("_hori_c42v4").get<double>();
            dc._hori_c5           = dj.at("_hori_c5").get<double>();
            dc._takeoff_time      = dj.at("_takeoff_time").get<double>();
            dc._landing_time      = dj.at("_landing_time").get<double>();
            dc.fixed_time_val     = std::numeric_limits<double>::infinity();
        } else { // Endurance
            dc.model         = DroneConfig::Model::Endurance;
            auto d           = dj.at("_data");
            dc.cap           = d.at("capacity [kg]").get<double>();
            dc.fixed_time_val= d.at("FixedTime (s)").get<double>();
            dc.cruise_speed  = d.at("V_max (m/s)").get<double>();
            dc.speed_type    = st_str(d.at("speed_type").get<std::string>());
            dc.range_type    = st_str(d.at("range_type").get<std::string>());
            dc.battery_val   = 1.0;
            dc._takeoff_time = 0.0;
            dc._landing_time = 0.0;
        }
        cfg.drone = dc;
    }

    auto em_from_str = [](const std::string& s) {
        if (s == "linear")    return cli::EnergyModel::Linear;
        if (s == "non-linear")return cli::EnergyModel::NonLinear;
        if (s == "endurance") return cli::EnergyModel::Endurance;
        return cli::EnergyModel::Unlimited;
    };
    auto st_from_str = [](const std::string& s) {
        return s == "low" ? cli::ConfigType::Low : cli::ConfigType::High;
    };
    auto strat_from_str = [](const std::string& s) {
        if (s == "random")   return cli::Strategy::Random;
        if (s == "cyclic")   return cli::Strategy::Cyclic;
        if (s == "vns")      return cli::Strategy::Vns;
        return cli::Strategy::Adaptive;
    };
    auto elite_pull_from_str = [](const std::string& s) {
        if (s == "topk")     return cli::ElitePullStrategy::TopK;
        if (s == "rank")     return cli::ElitePullStrategy::Rank;
        if (s == "pullcount")return cli::ElitePullStrategy::PullCount;
        if (s == "off")      return cli::ElitePullStrategy::Off;
        return cli::ElitePullStrategy::Random;
    };
    auto elite_pull_accept_from_str = [](const std::string& s) {
        if (s == "selective") return cli::ElitePullAcceptStrategy::Selective;
        return cli::ElitePullAcceptStrategy::Always;
    };
    auto elite_push_from_str = [](const std::string& s) {
        if (s == "segment-best")     return cli::ElitePushStrategy::SegmentBest;
        if (s == "significant-best") return cli::ElitePushStrategy::SignificantBest;
        return cli::ElitePushStrategy::NewBest;
    };
    auto elite_replace_from_str = [](const std::string& s) {
        if (s == "edge-crowding") return cli::EliteReplaceStrategy::EdgeCrowding;
        if (s == "quality-only")  return cli::EliteReplaceStrategy::QualityOnly;
        if (s == "random-target") return cli::EliteReplaceStrategy::RandomTarget;
        return cli::EliteReplaceStrategy::TDCrowding;
    };

    cfg.problem                   = j.at("problem").get<std::string>();
    cfg.config                    = em_from_str(j.at("config").get<std::string>());
    cfg.tabu_size_factor          = j.at("tabu_size_factor").get<double>();
    cfg.adaptive_iterations       = j.at("adaptive_iterations").get<std::size_t>();
    cfg.adaptive_fixed_iterations = j.at("adaptive_fixed_iterations").get<bool>();
    cfg.adaptive_pull_elite_segments = j.at("adaptive_pull_elite_segments").get<std::size_t>();
    cfg.adaptive_segments         = j.value("adaptive_segments", static_cast<std::size_t>(7));
    cfg.adaptive_fixed_segments   = j.at("adaptive_fixed_segments").get<bool>();
    cfg.ejection_chain_iterations = j.at("ejection_chain_iterations").get<std::size_t>();
    cfg.destroy_rate              = j.at("destroy_rate").get<double>();
    cfg.elite_pool_factor         = j.value("elite_pool_factor", 0.03);
    cfg.elite_pool_size           = j.value("elite_pool_size", static_cast<std::size_t>(0));
    cfg.speed_type                = st_from_str(j.at("speed_type").get<std::string>());
    cfg.range_type                = st_from_str(j.at("range_type").get<std::string>());
    cfg.waiting_time_limit        = j.at("waiting_time_limit").get<double>();
    cfg.strategy                  = strat_from_str(j.at("strategy").get<std::string>());
    cfg.elite_pull_strategy       = j.contains("elite_pull_strategy")
        ? elite_pull_from_str(j.at("elite_pull_strategy").get<std::string>())
        : cli::ElitePullStrategy::Random;
    cfg.elite_pull_accept_strategy = j.contains("elite_pull_accept_strategy")
        ? elite_pull_accept_from_str(j.at("elite_pull_accept_strategy").get<std::string>())
        : cli::ElitePullAcceptStrategy::Always;
    cfg.elite_push_strategy       = j.contains("elite_push_strategy")
        ? elite_push_from_str(j.at("elite_push_strategy").get<std::string>())
        : cli::ElitePushStrategy::NewBest;
    cfg.elite_pull_quality_tolerance_pct = j.value("elite_pull_quality_tolerance_pct", 1.0);
    if (!j.at("fix_iteration").is_null())
        cfg.fix_iteration         = j.at("fix_iteration").get<std::size_t>();
    cfg.reset_after_factor        = j.at("reset_after_factor").get<double>();
    cfg.max_elite_size            = j.at("max_elite_size").get<std::size_t>();
    cfg.max_evaluations           = j.value("max_evaluations", static_cast<std::size_t>(0));
    cfg.penalty_exponent          = j.at("penalty_exponent").get<double>();
    cfg.single_truck_route        = j.at("single_truck_route").get<bool>();
    cfg.single_drone_route        = j.at("single_drone_route").get<bool>();
    cfg.verbose                   = j.at("verbose").get<bool>();
    cfg.outputs                   = j.at("outputs").get<std::string>();
    cfg.disable_logging           = j.at("disable_logging").get<bool>();
    cfg.dry_run                   = j.at("dry_run").get<bool>();
    cfg.extra                     = j.at("extra").get<std::string>();
    cfg.gamma_1                   = j.at("gamma_1").get<double>();
    cfg.gamma_2                   = j.at("gamma_2").get<double>();
    cfg.gamma_3                   = j.at("gamma_3").get<double>();
    cfg.gamma_4                   = j.at("gamma_4").get<double>();
    cfg.randomize_worker_adaptive_hyperparams = j.at("randomize_worker_adaptive_hyperparams").get<bool>();
    cfg.elite_replace_strategy = j.contains("elite_replace_strategy")
        ? elite_replace_from_str(j.at("elite_replace_strategy").get<std::string>())
        : cli::EliteReplaceStrategy::TDCrowding;
    cfg.compact_output            = j.at("compact_output").get<bool>();
    cfg.run_id                    = j.value("run_id", "");
    return cfg;
}

// -----------------------------------------------------------------------
// Global singleton
// -----------------------------------------------------------------------
Config* g_config_ptr = nullptr;

void set_global_config(Config cfg) {
    static Config storage;
    storage      = std::move(cfg);
    g_config_ptr = &storage;
}
