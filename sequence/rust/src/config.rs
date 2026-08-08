use std::f64::consts;
use std::fs;
use std::sync::LazyLock;

use clap::Parser;
use regex::{Regex, RegexBuilder};
use serde::{Deserialize, Serialize};

use crate::cli;

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct TruckConfig {
    #[serde(rename = "V_max (m/s)")]
    pub speed: f64,

    #[serde(rename = "M_t (kg)")]
    pub capacity: f64,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct LinearJSON {
    #[serde(rename = "takeoffSpeed [m/s]")]
    takeoff_speed: f64,

    #[serde(rename = "cruiseSpeed [m/s]")]
    cruise_speed: f64,

    #[serde(rename = "landingSpeed [m/s]")]
    landing_speed: f64,

    #[serde(rename = "cruiseAlt [m]")]
    altitude: f64,

    #[serde(rename = "capacity [kg]")]
    capacity: f64,

    #[serde(rename = "batteryPower [Joule]")]
    battery: f64,

    speed_type: cli::ConfigType,
    range_type: cli::ConfigType,

    #[serde(rename = "beta(w/kg)")]
    beta: f64,

    #[serde(rename = "gamma(w)")]
    gamma: f64,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct NonLinearJSON {
    #[serde(rename = "takeoffSpeed [m/s]")]
    takeoff_speed: f64,

    #[serde(rename = "cruiseSpeed [m/s]")]
    cruise_speed: f64,

    #[serde(rename = "landingSpeed [m/s]")]
    landing_speed: f64,

    #[serde(rename = "cruiseAlt [m]")]
    altitude: f64,

    #[serde(rename = "capacity [kg]")]
    capacity: f64,

    #[serde(rename = "batteryPower [Joule]")]
    battery: f64,

    speed_type: cli::ConfigType,
    range_type: cli::ConfigType,
}

#[derive(Debug, Deserialize)]
struct _NonLinearFileJSON {
    config: Vec<NonLinearJSON>,
    k1: f64,

    #[serde(rename = "k2 (sqrt(kg/m))")]
    k2: f64,

    #[serde(rename = "c1 (sqrt(m/kg))")]
    c1: f64,

    #[serde(rename = "c2 (sqrt(m/kg))")]
    c2: f64,

    #[serde(rename = "c4 (kg/m)")]
    c4: f64,

    #[serde(rename = "c5 (Ns/m)")]
    c5: f64,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct EnduranceJSON {
    speed_type: cli::ConfigType,
    range_type: cli::ConfigType,

    #[serde(rename = "capacity [kg]")]
    capacity: f64,

    #[serde(rename = "FixedTime (s)")]
    fixed_time: f64,

    #[serde(rename = "V_max (m/s)")]
    speed: f64,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(tag = "config")]
pub enum DroneConfig {
    Linear {
        _data: LinearJSON,
        _takeoff_time: f64,
        _landing_time: f64,
    },
    NonLinear {
        _data: NonLinearJSON,
        _vert_k1: f64,
        _vert_k2: f64,
        _vert_c2: f64,
        _vert_half_takeoff: f64,
        _vert_half_landing: f64,
        _vert_half_takeoff_2: f64,
        _vert_half_landing_2: f64,
        _hori_c12: f64,
        _hori_c4v3: f64,
        _hori_c42v4: f64,
        _hori_c5: f64,
        _takeoff_time: f64,
        _landing_time: f64,
    },
    Endurance {
        _data: EnduranceJSON,
    },
}

impl DroneConfig {
    const W: f64 = 1.5;
    const G: f64 = 9.8;

    fn new(path: &String, config: cli::EnergyModel, speed_type: cli::ConfigType, range_type: cli::ConfigType) -> Self {
        match config {
            cli::EnergyModel::Linear => {
                let data = serde_json::from_str::<Vec<LinearJSON>>(&fs::read_to_string(path).unwrap()).unwrap();

                for config in data {
                    if config.speed_type == speed_type && config.range_type == range_type {
                        let _takeoff_time = config.altitude / config.takeoff_speed;
                        let _landing_time = config.altitude / config.landing_speed;
                        return Self::Linear {
                            _data: config,
                            _takeoff_time,
                            _landing_time,
                        };
                    }
                }

                panic!("No matching linear config")
            }
            cli::EnergyModel::NonLinear => {
                let data = serde_json::from_str::<_NonLinearFileJSON>(&fs::read_to_string(path).unwrap()).unwrap();

                for config in data.config {
                    if config.speed_type == speed_type && config.range_type == range_type {
                        let _vert_k1 = data.k1 * Self::G;
                        let _vert_k2 = Self::G / (data.k2 * data.k2);
                        let _vert_c2 = data.c2 * Self::G.powf(1.5);
                        let _vert_half_takeoff: f64 = config.takeoff_speed / 2.0;
                        let _vert_half_landing = config.landing_speed / 2.0;
                        let _vert_half_takeoff_2 = _vert_half_takeoff * _vert_half_takeoff;
                        let _vert_half_landing_2 = _vert_half_landing * _vert_half_landing;
                        let _hori_c12 = data.c1 + data.c2;
                        let _hori_c4v3 = data.c4 * config.cruise_speed * config.cruise_speed * config.cruise_speed;
                        let _hori_c42v4 = data.c4
                            * data.c4
                            * config.cruise_speed
                            * config.cruise_speed
                            * config.cruise_speed
                            * config.cruise_speed;

                        let deg_10 = consts::PI / 18.0;
                        let _hori_c5 = data.c5 * (config.cruise_speed * deg_10.cos()).powi(2);

                        let _takeoff_time = config.altitude / config.takeoff_speed;
                        let _landing_time = config.altitude / config.landing_speed;

                        return Self::NonLinear {
                            _data: config,
                            _vert_k1,
                            _vert_k2,
                            _vert_c2,
                            _vert_half_takeoff,
                            _vert_half_landing,
                            _vert_half_takeoff_2,
                            _vert_half_landing_2,
                            _hori_c12,
                            _hori_c4v3,
                            _hori_c42v4,
                            _hori_c5,
                            _takeoff_time,
                            _landing_time,
                        };
                    }
                }

                panic!("No matching non-linear config")
            }
            cli::EnergyModel::Endurance => {
                let data = serde_json::from_str::<Vec<EnduranceJSON>>(&fs::read_to_string(path).unwrap()).unwrap();

                for config in data {
                    if config.speed_type == speed_type && config.range_type == range_type {
                        return Self::Endurance { _data: config };
                    }
                }

                panic!("No matching endurance config")
            }
            cli::EnergyModel::Unlimited => Self::Endurance {
                _data: EnduranceJSON {
                    speed_type: cli::ConfigType::High,
                    range_type: cli::ConfigType::High,
                    capacity: f64::INFINITY,
                    fixed_time: f64::INFINITY,
                    speed: 1.0,
                },
            },
        }
    }

    pub fn capacity(&self) -> f64 {
        match self {
            Self::Linear { _data, .. } => _data.capacity,
            Self::NonLinear { _data, .. } => _data.capacity,
            Self::Endurance { _data, .. } => _data.capacity,
        }
    }

    pub fn battery(&self) -> f64 {
        match self {
            Self::Linear { _data, .. } => _data.battery,
            Self::NonLinear { _data, .. } => _data.battery,
            Self::Endurance { .. } => 1.0,
        }
    }

    pub fn fixed_time(&self) -> f64 {
        match self {
            Self::Linear { .. } | Self::NonLinear { .. } => f64::INFINITY,
            Self::Endurance { _data, .. } => _data.fixed_time,
        }
    }

    pub fn takeoff_power(&self, weight: f64) -> f64 {
        match self {
            Self::Linear { _data, .. } => _data.beta.mul_add(weight, _data.gamma),
            Self::NonLinear {
                _vert_k1,
                _vert_k2,
                _vert_c2,
                _vert_half_takeoff,
                _vert_half_takeoff_2,
                ..
            } => {
                let w = Self::W + weight;
                (_vert_k1 * w).mul_add(
                    _vert_half_takeoff + (_vert_half_takeoff_2 + _vert_k2 * w).sqrt(),
                    _vert_c2 * w.powf(1.5),
                )
            }
            Self::Endurance { .. } => 0.0,
        }
    }

    pub fn landing_power(&self, weight: f64) -> f64 {
        match self {
            Self::Linear { _data, .. } => _data.beta.mul_add(weight, _data.gamma),
            Self::NonLinear {
                _vert_k1,
                _vert_k2,
                _vert_c2,
                _vert_half_landing,
                _vert_half_landing_2,
                ..
            } => {
                let w = Self::W + weight;
                (_vert_k1 * w).mul_add(
                    _vert_half_landing + (_vert_half_landing_2 + _vert_k2 * w).sqrt(),
                    _vert_c2 * w.powf(1.5),
                )
            }
            Self::Endurance { .. } => 0.0,
        }
    }

    pub fn cruise_power(&self, weight: f64) -> f64 {
        match self {
            Self::Linear { _data, .. } => _data.beta.mul_add(weight, _data.gamma),
            Self::NonLinear {
                _hori_c12,
                _hori_c4v3,
                _hori_c42v4,
                _hori_c5,
                ..
            } => {
                let temp = (Self::W + weight) * Self::G - _hori_c5;
                _hori_c12 * (temp * temp + _hori_c42v4).powf(0.75) + _hori_c4v3
            }
            Self::Endurance { .. } => 0.0,
        }
    }

    pub fn takeoff_time(&self) -> f64 {
        match self {
            Self::Linear { _takeoff_time, .. } | Self::NonLinear { _takeoff_time, .. } => *_takeoff_time,
            Self::Endurance { .. } => 0.0,
        }
    }

    pub fn landing_time(&self) -> f64 {
        match self {
            Self::Linear { _landing_time, .. } | Self::NonLinear { _landing_time, .. } => *_landing_time,
            Self::Endurance { .. } => 0.0,
        }
    }

    pub fn cruise_time(&self, distance: f64) -> f64 {
        match self {
            Self::Linear { _data, .. } => distance / _data.cruise_speed,
            Self::NonLinear { _data, .. } => distance / _data.cruise_speed,
            Self::Endurance { _data, .. } => distance / _data.speed,
        }
    }
}

#[derive(Debug, Deserialize, Serialize)]
pub struct SerializedConfig {
    customers_count: usize,
    trucks_count: usize,
    drones_count: usize,

    x: Vec<f64>,
    y: Vec<f64>,
    demands: Vec<f64>,
    dronable: Vec<bool>,

    truck_distance: cli::DistanceType,
    drone_distance: cli::DistanceType,

    truck: TruckConfig,
    drone: DroneConfig,

    problem: String,
    config: cli::EnergyModel,
    tabu_size_factor: f64,
    adaptive_iterations: usize,
    adaptive_fixed_iterations: bool,
    adaptive_segments: usize,
    adaptive_fixed_segments: bool,
    ejection_chain_iterations: usize,
    destroy_rate: f64,
    speed_type: cli::ConfigType,
    range_type: cli::ConfigType,
    waiting_time_limit: f64,
    strategy: cli::Strategy,
    fix_iteration: Option<usize>,
    reset_after_factor: f64,
    max_elite_size: usize,
    penalty_exponent: f64,
    single_truck_route: bool,
    single_drone_route: bool,
    verbose: bool,
    outputs: String,
    disable_logging: bool,
    dry_run: bool,
    extra: String,
    compact_output: bool,
    run_id: String,
}

#[derive(Clone, Debug)]
pub struct Config {
    pub customers_count: usize,
    pub trucks_count: usize,
    pub drones_count: usize,

    pub x: Vec<f64>,
    pub y: Vec<f64>,
    pub demands: Vec<f64>,
    pub dronable: Vec<bool>,

    pub truck_distance: cli::DistanceType,
    pub drone_distance: cli::DistanceType,
    pub truck_distances: Vec<Vec<f64>>,
    pub drone_distances: Vec<Vec<f64>>,

    pub truck: TruckConfig,
    pub drone: DroneConfig,

    pub problem: String,
    pub config: cli::EnergyModel,
    pub tabu_size_factor: f64,
    pub adaptive_iterations: usize,
    pub adaptive_fixed_iterations: bool,
    pub adaptive_segments: usize,
    pub adaptive_fixed_segments: bool,
    pub ejection_chain_iterations: usize,
    pub destroy_rate: f64,
    pub speed_type: cli::ConfigType,
    pub range_type: cli::ConfigType,
    pub waiting_time_limit: f64,
    pub strategy: cli::Strategy,
    pub fix_iteration: Option<usize>,
    pub reset_after_factor: f64,
    pub max_elite_size: usize,
    pub penalty_exponent: f64,
    pub single_truck_route: bool,
    pub single_drone_route: bool,
    pub verbose: bool,
    pub outputs: String,
    pub disable_logging: bool,
    pub dry_run: bool,
    pub extra: String,
    pub compact_output: bool,
    pub run_id: String,
}

impl From<SerializedConfig> for Config {
    fn from(config: SerializedConfig) -> Self {
        let truck_distances = config.truck_distance.matrix(&config.x, &config.y);
        let drone_distances = config.drone_distance.matrix(&config.x, &config.y);

        Self {
            customers_count: config.customers_count,
            trucks_count: config.trucks_count,
            drones_count: config.drones_count,
            x: config.x,
            y: config.y,
            demands: config.demands,
            dronable: config.dronable,
            truck_distance: config.truck_distance,
            drone_distance: config.drone_distance,
            truck_distances,
            drone_distances,
            truck: config.truck,
            drone: config.drone,
            problem: config.problem,
            config: config.config,
            tabu_size_factor: config.tabu_size_factor,
            adaptive_iterations: config.adaptive_iterations,
            adaptive_fixed_iterations: config.adaptive_fixed_iterations,
            adaptive_segments: config.adaptive_segments,
            adaptive_fixed_segments: config.adaptive_fixed_segments,
            ejection_chain_iterations: config.ejection_chain_iterations,
            destroy_rate: config.destroy_rate,
            speed_type: config.speed_type,
            range_type: config.range_type,
            waiting_time_limit: config.waiting_time_limit,
            strategy: config.strategy,
            fix_iteration: config.fix_iteration,
            reset_after_factor: config.reset_after_factor,
            max_elite_size: config.max_elite_size,
            penalty_exponent: config.penalty_exponent,
            single_truck_route: config.single_truck_route,
            single_drone_route: config.single_drone_route,
            verbose: config.verbose,
            outputs: config.outputs,
            disable_logging: config.disable_logging,
            dry_run: config.dry_run,
            extra: config.extra,
            compact_output: config.compact_output,
            run_id: config.run_id,
        }
    }
}

impl From<Config> for SerializedConfig {
    fn from(config: Config) -> Self {
        Self {
            customers_count: config.customers_count,
            trucks_count: config.trucks_count,
            drones_count: config.drones_count,
            x: config.x,
            y: config.y,
            demands: config.demands,
            dronable: config.dronable,
            truck_distance: config.truck_distance,
            drone_distance: config.drone_distance,
            truck: config.truck,
            drone: config.drone,
            problem: config.problem,
            config: config.config,
            tabu_size_factor: config.tabu_size_factor,
            adaptive_iterations: config.adaptive_iterations,
            adaptive_fixed_iterations: config.adaptive_fixed_iterations,
            adaptive_segments: config.adaptive_segments,
            adaptive_fixed_segments: config.adaptive_fixed_segments,
            ejection_chain_iterations: config.ejection_chain_iterations,
            destroy_rate: config.destroy_rate,
            speed_type: config.speed_type,
            range_type: config.range_type,
            waiting_time_limit: config.waiting_time_limit,
            strategy: config.strategy,
            fix_iteration: config.fix_iteration,
            reset_after_factor: config.reset_after_factor,
            max_elite_size: config.max_elite_size,
            penalty_exponent: config.penalty_exponent,
            single_truck_route: config.single_truck_route,
            single_drone_route: config.single_drone_route,
            verbose: config.verbose,
            outputs: config.outputs,
            disable_logging: config.disable_logging,
            dry_run: config.dry_run,
            extra: config.extra,
            compact_output: config.compact_output,
            run_id: config.run_id,
        }
    }
}

pub static CONFIG: LazyLock<Config> = LazyLock::new(|| {
    let arguments = cli::Arguments::parse();
    eprintln!("Received {arguments:?}");
    match arguments.command {
        cli::Commands::Evaluate { config, .. } => {
            let data = fs::read_to_string(config).unwrap();
            let deserialized = serde_json::from_str::<SerializedConfig>(&data).unwrap();
            Config::from(deserialized)
        }
        cli::Commands::Run {
            problem,
            truck_cfg,
            drone_cfg,
            config,
            tabu_size_factor,
            adaptive_iterations,
            adaptive_fixed_iterations,
            adaptive_segments,
            adaptive_fixed_segments,
            ejection_chain_iterations,
            destroy_rate,
            speed_type,
            range_type,
            truck_distance,
            drone_distance,
            trucks_count,
            drones_count,
            waiting_time_limit,
            strategy,
            fix_iteration,
            reset_after_factor,
            max_elite_size,
            penalty_exponent,
            single_truck_route,
            single_drone_route,
            verbose,
            outputs,
            disable_logging,
            dry_run,
            extra,
            compact_output,
            run_id,
        } => {
            let trucks_count_regex = Regex::new(r"trucks_count (\d+)").unwrap();
            let drones_count_regex = Regex::new(r"drones_count (\d+)").unwrap();
            let depot_regex = Regex::new(r"depot (-?[\d\.]+)\s+(-?[\d\.]+)").unwrap();
            let customers_regex = RegexBuilder::new(r"^\s*(-?[\d\.]+)\s+(-?[\d\.]+)\s+(0|1)\s+([\d\.]+)\s*$")
                .multi_line(true)
                .build()
                .unwrap();

            let data = fs::read_to_string(&problem).unwrap();

            let trucks_count = trucks_count
                .or_else(|| {
                    trucks_count_regex
                        .captures(&data)
                        .and_then(|caps| caps.get(1))
                        .and_then(|m| m.as_str().parse::<usize>().ok())
                })
                .expect("Missing trucks count");
            let drones_count = drones_count
                .or_else(|| {
                    drones_count_regex
                        .captures(&data)
                        .and_then(|caps| caps.get(1))
                        .and_then(|m| m.as_str().parse::<usize>().ok())
                })
                .expect("Missing drones count");

            let depot = depot_regex
                .captures(&data)
                .and_then(|caps| {
                    let x = caps.get(1)?.as_str().parse::<f64>().ok()?;
                    let y = caps.get(2)?.as_str().parse::<f64>().ok()?;
                    Some((x, y))
                })
                .expect("Missing depot coordinates");

            let mut customers_count = 0;
            let mut x = vec![depot.0];
            let mut y = vec![depot.1];
            let mut demands = vec![0.0];
            let mut dronable = vec![true];
            for c in customers_regex.captures_iter(&data) {
                customers_count += 1;

                let (_, [_x, _y, _dronable, _demand]) = c.extract::<4>();
                x.push(_x.parse::<f64>().unwrap());
                y.push(_y.parse::<f64>().unwrap());
                dronable.push(matches!(_dronable, "1"));
                demands.push(_demand.parse::<f64>().unwrap());
            }

            let truck_distances = truck_distance.matrix(&x, &y);
            let drone_distances = drone_distance.matrix(&x, &y);

            let truck = serde_json::from_str::<TruckConfig>(&fs::read_to_string(truck_cfg).unwrap()).unwrap();
            let drone = DroneConfig::new(&drone_cfg, config, speed_type, range_type);

            let takeoff = drone.takeoff_time();
            let takeoff_from_depot = drone.takeoff_power(0.0);

            let landing = drone.landing_time();
            let landing_from_depot = drone.landing_power(0.0);

            let cruise_from_depot = drone.cruise_power(0.0);
            for i in 1..customers_count + 1 {
                dronable[i] = dronable[i]
                    && demands[i] <= drone.capacity()
                    && takeoff + drone.cruise_time(drone_distances[0][i] + drone_distances[i][0]) + landing
                        <= drone.fixed_time()
                    && (landing_from_depot + drone.landing_power(demands[i])).mul_add(
                        landing,
                        drone.cruise_power(demands[i]).mul_add(
                            drone.cruise_time(drone_distances[i][0]),
                            (takeoff_from_depot + drone.takeoff_power(demands[i]))
                                .mul_add(takeoff, cruise_from_depot * drone.cruise_time(drone_distances[0][i])),
                        ),
                    ) <= drone.battery();
            }

            Config {
                customers_count,
                trucks_count,
                drones_count,
                x,
                y,
                demands,
                dronable,
                truck_distance,
                drone_distance,
                truck_distances,
                drone_distances,
                truck,
                drone,
                problem,
                config,
                tabu_size_factor,
                adaptive_iterations,
                adaptive_fixed_iterations,
                adaptive_segments,
                adaptive_fixed_segments,
                ejection_chain_iterations,
                destroy_rate,
                speed_type,
                range_type,
                waiting_time_limit,
                strategy,
                fix_iteration,
                reset_after_factor,
                max_elite_size,
                penalty_exponent,
                single_truck_route,
                single_drone_route,
                verbose,
                outputs,
                disable_logging,
                dry_run,
                extra,
                compact_output,
                run_id,
            }
        }
    }
});
