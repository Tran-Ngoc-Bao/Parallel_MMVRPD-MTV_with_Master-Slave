use std::cmp::min;
use std::collections::HashMap;
use std::f64::consts;

use crate::config::CONFIG;

pub fn clusterize(customers: &mut [usize], k: usize) -> Vec<Vec<usize>> {
    let mut clusters = vec![vec![]; k];
    if customers.is_empty() {
        return clusters;
    }

    let x = &CONFIG.x;
    let y = &CONFIG.y;
    let mut angles = HashMap::<usize, f64>::new();
    for &customer in customers.iter() {
        let mut angle = (y[customer] - y[0]).atan2(x[customer] - x[0]);
        if angle < 0.0 {
            angle += 2.0 * consts::PI;
        }

        angles.insert(customer, angle);
    }

    customers.sort_by(|i, j| angles[i].total_cmp(&angles[j]));

    // Rotate `customers` such that the angle between `customers.last` and `customers.first` is the greatest
    {
        let mut max_angle = 0.0;
        let mut max_angle_idx = 0;
        for i in 0..customers.len() {
            let angle = angles[&customers[i]] - angles[&customers[(i + 1) % customers.len()]];
            if angle > max_angle {
                max_angle = angle;
                max_angle_idx = i;
            }
        }

        let rotate_first = (max_angle_idx + 1) % customers.len();
        customers.rotate_left(rotate_first);
    }

    let first = customers.first().unwrap();
    let last = customers.last().unwrap();
    let gap = (angles[last] - angles[first]) / k as f64;
    for customer in customers.iter() {
        let cluster = min(((angles[customer] - angles[first]) / gap) as usize, k - 1);
        clusters[cluster].push(*customer);
    }

    clusters
}
