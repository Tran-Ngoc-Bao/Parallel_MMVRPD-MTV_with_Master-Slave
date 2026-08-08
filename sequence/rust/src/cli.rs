use std::fmt;

use clap::{Parser, Subcommand, ValueEnum};
use serde::{Deserialize, Serialize};

#[derive(Copy, Clone, Debug, PartialEq, Eq, PartialOrd, Ord, ValueEnum, Deserialize, Serialize)]
pub enum EnergyModel {
    #[serde(rename = "linear")]
    Linear = 0,
    #[serde(rename = "non-linear")]
    NonLinear = 1,
    #[serde(rename = "endurance")]
    Endurance = 2,
    #[serde(rename = "unlimited")]
    Unlimited = 3,
}

impl fmt::Display for EnergyModel {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{}",
            match self {
                Self::Linear => "linear",
                Self::NonLinear => "non-linear",
                Self::Endurance => "endurance",
                Self::Unlimited => "unlimited",
            }
        )
    }
}

#[derive(Copy, Clone, Debug, PartialEq, Eq, PartialOrd, Ord, ValueEnum, Deserialize, Serialize)]
pub enum ConfigType {
    #[serde(rename = "low")]
    Low,
    #[serde(rename = "high")]
    High,
}

impl fmt::Display for ConfigType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{}",
            match self {
                Self::Low => "low",
                Self::High => "high",
            }
        )
    }
}

#[derive(Copy, Clone, Debug, PartialEq, Eq, PartialOrd, Ord, ValueEnum, Deserialize, Serialize)]
pub enum Strategy {
    #[serde(rename = "random")]
    Random,
    #[serde(rename = "cyclic")]
    Cyclic,
    #[serde(rename = "vns")]
    Vns,
    #[serde(rename = "adaptive")]
    Adaptive,
}

impl fmt::Display for Strategy {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{}",
            match self {
                Self::Random => "random",
                Self::Cyclic => "cyclic",
                Self::Vns => "vns",
                Self::Adaptive => "adaptive",
            }
        )
    }
}

#[derive(Copy, Clone, Debug, PartialEq, Eq, PartialOrd, Ord, ValueEnum, Deserialize, Serialize)]
pub enum DistanceType {
    #[serde(rename = "manhattan")]
    Manhattan,
    #[serde(rename = "euclidean")]
    Euclidean,
}

impl fmt::Display for DistanceType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{}",
            match self {
                Self::Manhattan => "manhattan",
                Self::Euclidean => "euclidean",
            }
        )
    }
}

impl DistanceType {
    pub fn matrix(&self, x: &[f64], y: &[f64]) -> Vec<Vec<f64>> {
        let n = x.len();
        assert_eq!(n, y.len());

        let mut matrix = vec![vec![0.0; n]; n];
        for i in 0..n {
            for j in 0..n {
                let dx = x[i] - x[j];
                let dy = y[i] - y[j];
                matrix[i][j] = match self {
                    Self::Manhattan => dx.abs() + dy.abs(),
                    Self::Euclidean => (dx * dx + dy * dy).sqrt(),
                };
            }
        }

        matrix
    }
}

#[derive(Debug, Parser)]
#[command(
    long_about = "The min-timespan parallel technician-and-drone scheduling in door-to-door sampling service system",
    propagate_version = true,
    version
)]
pub struct Arguments {
    #[command(subcommand)]
    pub command: Commands,
}

#[allow(clippy::large_enum_variant)] // This struct is mostly a singleton
#[derive(Debug, Subcommand)]
pub enum Commands {
    /// Evaluate an existing solution
    Evaluate {
        /// Path to the solution JSON file
        solution: String,

        /// Path to the config JSON file
        config: String,
    },

    /// Run the algorithm
    Run {
        /// Path to the coordinate file
        problem: String,

        /// Path to truck config file
        #[arg(long, default_value_t = String::from("problems/config_parameter/truck_config.json"))]
        truck_cfg: String,

        /// Path to drone config file
        #[arg(long, default_value_t = String::from("problems/config_parameter/drone_endurance_config.json"))]
        drone_cfg: String,

        /// The energy consumption model to use.
        #[arg(short, long, default_value_t = EnergyModel::Endurance)]
        config: EnergyModel,

        /// Tabu size of each neighborhood, final value = [--tabu-size-factor] * [Base]
        #[arg(long, default_value_t = 0.75)]
        tabu_size_factor: f64,

        /// Number of non-improved iterations per adaptive segment = [--adaptive-iterations] * [Base]
        #[arg(long, default_value_t = 60)]
        adaptive_iterations: usize,

        /// Fixed number of iterations per adaptive segment = [--adaptive-iterations] * [Base]
        #[arg(long)]
        adaptive_fixed_iterations: bool,

        /// Number of non-improved segments before resetting the current solution = [--adaptive-segments]
        /// (note: in "adaptive" strategy, "--reset-after-factor" is ignored)
        #[arg(long, default_value_t = 7)]
        adaptive_segments: usize,

        /// Infer --adaptive-segments as a fixed number of segments per reset.
        #[arg(long)]
        adaptive_fixed_segments: bool,

        /// The number of ejection chain iterations to run when the elite set is popped
        #[arg(long, default_value_t = 0)]
        ejection_chain_iterations: usize,

        /// The destroy rate during destroy-and-repair procedure when the elite set is popped,
        /// but before ejection-chain is executed (set to 0 to disable destroy-and-repair)
        #[arg(long, default_value_t = 0.1)]
        destroy_rate: f64,

        /// Speed type of drones.
        #[arg(long, default_value_t = ConfigType::High)]
        speed_type: ConfigType,

        /// Range type of drones.
        #[arg(long, default_value_t = ConfigType::High)]
        range_type: ConfigType,

        /// Distance type to use for trucks.
        #[arg(long, default_value_t = DistanceType::Euclidean)]
        truck_distance: DistanceType,

        /// Distance type to use for drones.
        #[arg(long, default_value_t = DistanceType::Euclidean)]
        drone_distance: DistanceType,

        /// The number of trucks to override. Otherwise, use the default value.
        #[arg(long)]
        trucks_count: Option<usize>,

        /// The number of drones to override. Otherwise, use the default value.
        #[arg(long)]
        drones_count: Option<usize>,

        /// The waiting time limit for each customer (in seconds).
        #[arg(long, default_value_t = 3600.0)]
        waiting_time_limit: f64,

        /// Tabu search neighborhood selection strategy.
        #[arg(long, default_value_t = Strategy::Adaptive)]
        strategy: Strategy,

        /// Fix the number of iterations and disable elite set extraction. Otherwise, run until the elite set is exhausted.
        #[arg(long)]
        fix_iteration: Option<usize>,

        /// The number of non-improved iterations before resetting the current solution = [--reset-after-factor] * [Base]
        #[arg(long, default_value_t = 125.0)]
        reset_after_factor: f64,

        /// The maximum size of the elite set
        #[arg(long, default_value_t = 0)]
        max_elite_size: usize,

        /// Exponent value E attached to the cost function:
        ///
        /// Cost(S) = [working time] * (1 + [weighted penalty values]).powf(E)
        #[arg(long, default_value_t = 0.5)]
        penalty_exponent: f64,

        /// Allow one route per truck only (this route can still serve multiple customers)
        #[arg(long)]
        single_truck_route: bool,

        /// Allow one customer per drone route only (each drone can still perform multiple routes)
        #[arg(long)]
        single_drone_route: bool,

        /// The verbose mode
        #[arg(short, long)]
        verbose: bool,

        /// The directory to store results
        #[arg(long, default_value_t = String::from("outputs/"))]
        outputs: String,

        /// Disable CSV logging per iteration (this can significantly reduce the running time)
        #[arg(long)]
        disable_logging: bool,

        /// Do not run the algorithm, only generate the config file
        #[arg(long)]
        dry_run: bool,

        /// Extra data to store in the output JSON
        #[arg(long, default_value_t = String::new())]
        extra: String,

        /// Only write the main run JSON; skip iteration CSV, solution and config JSON files
        #[arg(long)]
        compact_output: bool,

        /// Fixed run ID to use for output filenames (default: random 8-char alphanumeric)
        #[arg(long, default_value_t = String::new())]
        run_id: String,
    },
}
