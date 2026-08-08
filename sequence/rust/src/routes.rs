use std::collections::VecDeque;
use std::fmt;
use std::mem::swap;
use std::rc::Rc;

use crate::config::CONFIG;
use crate::neighborhoods::Neighborhood;
use crate::solutions::Solution;

#[derive(Debug)]
struct _RouteDataValues {
    distance: f64,
    weight: f64,
}

#[derive(Debug)]
pub struct _RouteData {
    pub customers: Vec<usize>,
    value: _RouteDataValues,
}

impl _RouteData {
    fn _construct(customers: Vec<usize>, distances: &[Vec<f64>]) -> Self {
        assert_eq!(customers.first(), Some(&0));
        assert_eq!(customers.last(), Some(&0));
        assert!(customers.len() >= 3);

        let mut distance = 0.0;
        let mut weight = 0.0;
        for i in 0..customers.len() - 1 {
            distance += distances[customers[i]][customers[i + 1]];
            weight += CONFIG.demands[customers[i]];
        }

        Self {
            customers,
            value: _RouteDataValues { distance, weight },
        }
    }
}

pub trait Route: Sized {
    fn new(customers: Vec<usize>) -> Rc<Self>;
    fn single(customer: usize) -> Rc<Self> {
        Self::new(vec![0, customer, 0])
    }
    fn get_correct_route<'a>(
        truck_routes: &'a [Vec<Rc<TruckRoute>>],
        drone_routes: &'a [Vec<Rc<DroneRoute>>],
    ) -> &'a [Vec<Rc<Self>>];
    fn get_correct_route_mut<'a>(
        truck_routes: &'a mut Vec<Vec<Rc<TruckRoute>>>,
        drone_routes: &'a mut Vec<Vec<Rc<DroneRoute>>>,
    ) -> &'a mut Vec<Vec<Rc<Self>>>;

    fn single_customer() -> bool;
    fn single_route() -> bool;

    fn data(&self) -> &_RouteData;
    fn working_time(&self) -> f64;
    fn capacity_violation(&self) -> f64;
    fn waiting_time_violation(&self) -> f64;

    fn push(&self, customer: usize) -> Rc<Self> {
        let customers = &self.data().customers;
        let mut new_customers = customers.clone();
        new_customers.insert(customers.len() - 1, customer);
        Self::new(new_customers)
    }

    fn pop(&self) -> Rc<Self> {
        let customers = &self.data().customers;
        let mut new_customers = customers.clone();
        new_customers.remove(customers.len() - 2);
        Self::new(new_customers)
    }

    fn _servable(customer: usize) -> bool;

    /// Extract customer subsegments from this route to form a new route during an inter-route operation.
    ///
    /// Note that if the current route becomes empty after extracting the subsegment, the result set will be
    /// empty.
    fn inter_route_extract<T>(&self, neighborhood: Neighborhood) -> Vec<(Rc<Self>, Rc<T>, Vec<usize>)>
    where
        T: Route,
    {
        let customers = &self.data().customers;
        let mut results = vec![];
        let mut queue = VecDeque::new();
        let size = match neighborhood {
            Neighborhood::Move10 => 1,
            Neighborhood::Move20 => 2,
            _default => 0,
        };

        if size == 0 || customers.len() - 2 <= size {
            return results;
        }

        for i in 1..customers.len() - 1 {
            if T::_servable(customers[i]) {
                queue.push_back(customers[i]);
                if queue.len() > size {
                    queue.pop_front();
                }

                if queue.len() == size {
                    let mut original = customers[0..i - size + 1].to_vec();
                    original.extend(customers[i + 1..].iter().copied());

                    let mut route = vec![0];
                    route.extend(queue.iter().copied());
                    route.push(0);

                    let tabu = customers[i - size + 1..i + 1].to_vec();
                    results.push((Self::new(original), T::new(route), tabu));
                }
            } else {
                queue.clear();
            }
        }

        results
    }

    /// Perform inter-route neighborhood search.
    ///
    /// This function is non-commutative (i.e. `r1.inter_route(r2, n) != r2.inter_route(r1, n)`). For example,
    /// `r1.inter_route(r2, Neighborhood::Move10)` will move 1 customer from `r1` to `r2`, but not from `r2` to `r1`.
    ///
    /// For symmetric neighborhoods (e.g. `Neighborhood::Move11`), this function will be commutative though.
    fn inter_route<T>(
        &self,
        other: Rc<T>,
        neighborhood: Neighborhood,
    ) -> Vec<(Option<Rc<Self>>, Option<Rc<T>>, Vec<usize>)>
    where
        T: Route,
    {
        let customers_i = &self.data().customers;
        let customers_j = &other.data().customers;

        let length_i = customers_i.len();
        let length_j = customers_j.len();

        let mut buffer_i = customers_i.clone();
        let mut buffer_j = customers_j.clone();

        let mut results = vec![];

        match neighborhood {
            Neighborhood::Move10 => {
                for (idx_i, &customer_i) in customers_i.iter().enumerate().take(length_i - 1).skip(1) {
                    if !T::_servable(customer_i) {
                        continue;
                    }

                    let removed = buffer_i.remove(idx_i);
                    let route_i = if length_i == 3 {
                        None
                    } else {
                        Some(Self::new(buffer_i.clone()))
                    };
                    let tabu = vec![removed];

                    buffer_j.insert(1, removed);

                    for idx_j in 1..length_j {
                        let ptr = T::new(buffer_j.clone());
                        results.push((route_i.clone(), Some(ptr), tabu.clone()));

                        buffer_j.swap(idx_j, idx_j + 1);
                    }

                    buffer_i.insert(idx_i, removed);
                    buffer_j.pop();
                }
            }
            Neighborhood::Move11 => {
                for idx_i in 1..length_i - 1 {
                    if !T::_servable(buffer_i[idx_i]) {
                        continue;
                    }

                    for idx_j in 1..length_j - 1 {
                        if !Self::_servable(buffer_j[idx_j]) {
                            continue;
                        }

                        swap(&mut buffer_i[idx_i], &mut buffer_j[idx_j]);

                        let ptr_i = Self::new(buffer_i.clone());
                        let ptr_j = T::new(buffer_j.clone());
                        let tabu = vec![customers_i[idx_i], customers_j[idx_j]];
                        results.push((Some(ptr_i), Some(ptr_j), tabu));

                        swap(&mut buffer_i[idx_i], &mut buffer_j[idx_j]);
                    }
                }
            }
            Neighborhood::Move20 => {
                for idx_i in 1..length_i - 2 {
                    if !T::_servable(buffer_i[idx_i]) || !T::_servable(buffer_i[idx_i + 1]) {
                        continue;
                    }

                    let removed_x = buffer_i.remove(idx_i);
                    let removed_y = buffer_i.remove(idx_i);

                    let route_i = if length_i == 4 {
                        None
                    } else {
                        Some(Self::new(buffer_i.clone()))
                    };
                    let tabu = vec![removed_x, removed_y];

                    buffer_j.insert(1, removed_x);
                    buffer_j.insert(2, removed_y);

                    for idx_j in 1..length_j {
                        let ptr = T::new(buffer_j.clone());
                        results.push((route_i.clone(), Some(ptr), tabu.clone()));

                        buffer_j.swap(idx_j + 1, idx_j + 2);
                        buffer_j.swap(idx_j, idx_j + 1);
                    }

                    buffer_i.insert(idx_i, removed_x);
                    buffer_i.insert(idx_i + 1, removed_y);
                    buffer_j.pop();
                    buffer_j.pop();
                }
            }
            Neighborhood::Move21 => {
                for idx_i in 1..length_i - 2 {
                    if !T::_servable(buffer_i[idx_i]) || !T::_servable(buffer_i[idx_i + 1]) {
                        continue;
                    }

                    swap(&mut buffer_i[idx_i], &mut buffer_j[1]);
                    buffer_j.insert(2, buffer_i.remove(idx_i + 1));

                    for idx_j in 1..length_j - 1 {
                        if Self::_servable(buffer_j[idx_j]) {
                            let ptr_i = Self::new(buffer_i.clone());
                            let ptr_j = T::new(buffer_j.clone());
                            let tabu = vec![buffer_j[idx_j], buffer_j[idx_j + 1], buffer_i[idx_i]];
                            results.push((Some(ptr_i), Some(ptr_j), tabu));
                        }

                        swap(&mut buffer_i[idx_i], &mut buffer_j[idx_j + 2]);
                        buffer_j.swap(idx_j + 1, idx_j + 2);
                        buffer_j.swap(idx_j, idx_j + 1);
                    }

                    swap(&mut buffer_i[idx_i], &mut buffer_j[length_j - 1]);
                    buffer_i.insert(idx_i + 1, buffer_j.pop().unwrap());
                }
            }
            Neighborhood::Move22 => {
                for idx_i in 1..length_i - 2 {
                    if !T::_servable(buffer_i[idx_i]) || !T::_servable(buffer_i[idx_i + 1]) {
                        continue;
                    }

                    for idx_j in 1..length_j - 2 {
                        if !Self::_servable(buffer_j[idx_j]) || !Self::_servable(buffer_j[idx_j + 1]) {
                            continue;
                        }

                        swap(&mut buffer_i[idx_i], &mut buffer_j[idx_j]);
                        swap(&mut buffer_i[idx_i + 1], &mut buffer_j[idx_j + 1]);

                        let ptr_i = Self::new(buffer_i.clone());
                        let ptr_j = T::new(buffer_j.clone());
                        let tabu = vec![
                            buffer_i[idx_i],
                            buffer_i[idx_i + 1],
                            buffer_j[idx_j],
                            buffer_j[idx_j + 1],
                        ];
                        results.push((Some(ptr_i), Some(ptr_j), tabu));

                        swap(&mut buffer_i[idx_i], &mut buffer_j[idx_j]);
                        swap(&mut buffer_i[idx_i + 1], &mut buffer_j[idx_j + 1]);
                    }
                }
            }
            Neighborhood::TwoOpt => {
                let mut offset_i = length_i - 1;
                while offset_i > 1 && T::_servable(buffer_i[offset_i - 1]) {
                    offset_i -= 1;
                }

                let mut offset_j = length_j - 1;
                while offset_j > 1 && Self::_servable(buffer_j[offset_j - 1]) {
                    offset_j -= 1;
                }

                for idx_i in offset_i..length_i - 1 {
                    for idx_j in offset_j..length_j - 1 {
                        // Construct separate buffers from scratch
                        let mut buffer_i = customers_i[..idx_i].to_vec();
                        let mut buffer_j = customers_j[..idx_j].to_vec();

                        buffer_i.extend_from_slice(&customers_j[idx_j..]);
                        buffer_j.extend_from_slice(&customers_i[idx_i..]);

                        let tabu = vec![buffer_i[idx_i], buffer_j[idx_j]];

                        // Move the buffers to the new routes
                        let ptr_i = Self::new(buffer_i);
                        let ptr_j = T::new(buffer_j);
                        results.push((Some(ptr_i), Some(ptr_j), tabu));
                    }
                }
            }
            // Neighborhood::CrossExchange => {
            //     // Inefficient implementation, but i'm just too lazy.
            //     for mut l_i in 1..length_i - 1 {
            //         for r_i in 1 + l_i..length_i - 1 {
            //             for mut l_j in 1..length_j - 1 {
            //                 for r_j in 1 + l_j..length_j - 1 {
            //                     // Swap 2 segments customers_i[l_i..r_i] and customers_j[l_j..r_j]
            //                     let ok = loop {
            //                         if l_i >= r_i || l_j >= r_j {
            //                             break true;
            //                         }

            //                         if !T::_servable(buffer_i[l_i]) || !Self::_servable(buffer_j[l_j]) {
            //                             break false;
            //                         }

            //                         swap(&mut buffer_i[l_i], &mut buffer_j[l_j]);
            //                         l_i += 1;
            //                         l_j += 1;
            //                     };

            //                     if ok {
            //                         for _ in l_i..r_i {
            //                             buffer_j.insert(l_j, buffer_i.remove(l_i));
            //                         }
            //                         for _ in l_j..r_j {
            //                             buffer_i.insert(l_i, buffer_j.remove(l_j));
            //                         }

            //                         let ptr_i = Self::new(buffer_i.clone());
            //                         let ptr_j = T::new(buffer_j.clone());
            //                         let tabu = customers_i[l_i..r_i]
            //                             .iter()
            //                             .chain(customers_j[l_j..r_j].iter())
            //                             .copied()
            //                             .collect();
            //                         results.push((Some(ptr_i), Some(ptr_j), tabu));
            //                     }

            //                     buffer_i.clone_from(customers_i);
            //                     buffer_j.clone_from(customers_j);
            //                 }
            //             }
            //         }
            //     }
            // }
            _ => panic!("inter_route called with invalid neighborhood {neighborhood}"),
        }

        results
    }

    fn inter_route_3<T1, T2>(
        &self,
        other_x: Rc<T1>,
        other_y: Rc<T2>,
        neighborhood: Neighborhood,
    ) -> Vec<(Option<Rc<Self>>, Rc<T1>, Rc<T2>, Vec<usize>)>
    where
        T1: Route,
        T2: Route,
    {
        let customers_i = &self.data().customers;
        let customers_j = &other_x.data().customers;
        let customers_k = &other_y.data().customers;

        let length_i = customers_i.len();
        let length_j = customers_j.len();
        let length_k = customers_k.len();

        let mut buffer_i = customers_i.clone();
        let mut buffer_j = customers_j.clone();
        let mut buffer_k = customers_k.clone();

        let mut results = vec![];

        match neighborhood {
            Neighborhood::EjectionChain => {
                for idx_i in 1..length_i - 1 {
                    if !T1::_servable(buffer_i[idx_i]) {
                        continue;
                    }

                    let remove_x = buffer_i.remove(idx_i);
                    for idx_j in 1..length_j - 1 {
                        if !T2::_servable(buffer_j[idx_j]) {
                            continue;
                        }

                        buffer_k.insert(1, buffer_j[idx_j]);
                        buffer_j[idx_j] = remove_x;

                        for idx_k in 1..length_k {
                            let tabu = vec![remove_x, buffer_k[idx_k]];

                            let ptr_i = if buffer_i.len() == 2 {
                                None
                            } else {
                                Some(Self::new(buffer_i.clone()))
                            };
                            let ptr_j = T1::new(buffer_j.clone());
                            let ptr_k = T2::new(buffer_k.clone());
                            results.push((ptr_i, ptr_j, ptr_k, tabu));

                            buffer_k.swap(idx_k, idx_k + 1);
                        }

                        buffer_j[idx_j] = buffer_k.pop().unwrap();
                    }

                    buffer_i.insert(idx_i, remove_x);
                }
            }
            _ => panic!("inter_route_3 called with invalid neighborhood {neighborhood}"),
        }

        results
    }

    /// Returns a pointer to the underlying cached intra-route neighbors.
    fn intra_route(&self, neighborhood: Neighborhood) -> Vec<(Rc<Self>, Vec<usize>)> {
        let data = self.data();

        let length = data.customers.len();
        let mut results = vec![];
        let mut buffer = data.customers.clone();
        match neighborhood {
            Neighborhood::Move10 => {
                for i in 1..length - 2 {
                    for j in i..length - 2 {
                        buffer.swap(j, j + 1);

                        let ptr = Self::new(buffer.clone());
                        let tabu = vec![data.customers[i]];
                        // println!("buffer = {:?}, tabu = {:?}", buffer, tabu);
                        results.push((ptr, tabu));
                    }

                    buffer[i..length - 1].rotate_right(1);
                }

                for i in 2..length - 1 {
                    for j in (2..i + 1).rev() {
                        buffer.swap(j - 1, j);

                        let ptr = Self::new(buffer.clone());
                        let tabu = vec![data.customers[i]];
                        // println!("buffer = {:?}, tabu = {:?}", buffer, tabu);
                        results.push((ptr, tabu));
                    }

                    buffer[1..i + 1].rotate_left(1);
                }
            }
            Neighborhood::Move11 => {
                for i in 1..length - 2 {
                    for j in i..length - 2 {
                        buffer.swap(j, j + 1);
                        buffer.swap(i, j);

                        let ptr = Self::new(buffer.clone());
                        let tabu = vec![data.customers[i], data.customers[j + 1]];
                        // println!("buffer = {:?}, tabu = {:?}", buffer, tabu);
                        results.push((ptr, tabu));
                    }

                    buffer.swap(i, length - 2);
                }
            }
            Neighborhood::Move20 => {
                for i in 1..length - 3 {
                    for j in i + 1..length - 2 {
                        buffer.swap(j, j + 1);
                        buffer.swap(j - 1, j);

                        let ptr = Self::new(buffer.clone());
                        let tabu = vec![data.customers[i], data.customers[i + 1]];
                        // println!("buffer = {:?}, tabu = {:?}", buffer, tabu);
                        results.push((ptr, tabu));
                    }

                    buffer[i..length - 1].rotate_right(2);
                }

                for i in 2..length - 2 {
                    for j in (1..i).rev() {
                        buffer.swap(j + 1, j + 2);
                        buffer.swap(j, j + 2);

                        let ptr = Self::new(buffer.clone());
                        let tabu = vec![data.customers[i], data.customers[i + 1]];
                        // println!("buffer = {:?}, tabu = {:?}", buffer, tabu);
                        results.push((ptr, tabu));
                    }

                    buffer[1..i + 2].rotate_left(2);
                }
            }
            Neighborhood::Move21 => {
                for i in 1..length - 3 {
                    for j in i..length - 3 {
                        buffer.swap(j + 1, j + 2);
                        buffer.swap(j, j + 1);
                        buffer.swap(i, j);

                        let ptr = Self::new(buffer.clone());
                        let tabu = vec![data.customers[i], data.customers[i + 1], data.customers[j + 2]];
                        // println!("buffer = {:?}, tabu = {:?}", buffer, tabu);
                        results.push((ptr, tabu));
                    }

                    buffer.swap(i, length - 3);
                    buffer[i + 1..length - 1].rotate_right(1);
                }

                for i in 2..length - 2 {
                    for j in (1..i).rev() {
                        buffer.swap(j + 1, j + 2);
                        buffer.swap(j, j + 2);
                        buffer.swap(j + 2, i + 1);

                        let ptr = Self::new(buffer.clone());
                        let tabu = vec![data.customers[i], data.customers[i + 1], data.customers[j]];
                        // println!("buffer = {:?}, tabu = {:?}", buffer, tabu);
                        results.push((ptr, tabu));
                    }

                    buffer.swap(1, i + 1);
                    buffer[2..i + 2].rotate_left(1);
                }
            }
            Neighborhood::Move22 => {
                for i in 1..length.saturating_sub(4) {
                    {
                        buffer.swap(i, i + 2);
                        buffer.swap(i + 1, i + 3);

                        let ptr = Self::new(buffer.clone());
                        let tabu = vec![
                            data.customers[i],
                            data.customers[i + 1],
                            data.customers[i + 2],
                            data.customers[i + 3],
                        ];
                        // println!("buffer = {:?}, tabu = {:?}", buffer, tabu);
                        results.push((ptr, tabu));
                    }

                    for j in i + 3..length - 2 {
                        buffer.swap(i, i + 1);
                        buffer.swap(i + 1, j + 1);
                        buffer.swap(j, j + 1);
                        buffer.swap(j - 1, j);

                        let ptr = Self::new(buffer.clone());
                        let tabu = vec![
                            data.customers[i],
                            data.customers[i + 1],
                            data.customers[j],
                            data.customers[j + 1],
                        ];
                        // println!("buffer = {:?}, tabu = {:?}", buffer, tabu);
                        results.push((ptr, tabu));
                    }

                    buffer.swap(i, length - 3);
                    buffer.swap(i + 1, length - 2);
                }
            }
            Neighborhood::TwoOpt => {
                for i in 1..length - 2 {
                    {
                        buffer.swap(i, i + 1);

                        let ptr = Self::new(buffer.clone());
                        let tabu = vec![data.customers[i], data.customers[i + 1]];
                        // println!("buffer = {:?}, tabu = {:?}", buffer, tabu);
                        results.push((ptr, tabu));
                    }

                    for j in i + 2..length - 1 {
                        buffer[i..j + 1].rotate_right(1);

                        let ptr = Self::new(buffer.clone());
                        let tabu = vec![data.customers[i], data.customers[j]];
                        // println!("buffer = {:?}, tabu = {:?}", buffer, tabu);
                        results.push((ptr, tabu));
                    }

                    buffer[i..length - 1].reverse();
                }
            }
            _ => panic!("intra_route called with invalid neighborhood {neighborhood}"),
        }

        for (_, tabu) in results.iter_mut() {
            tabu.sort();
        }

        results
    }
}

pub struct TruckRoute {
    _data: _RouteData,
    _working_time: f64,
    _capacity_violation: f64,
    _waiting_time_violation: f64,
}

impl fmt::Debug for TruckRoute {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:?}", self.data().customers)
    }
}

impl Route for TruckRoute {
    fn new(customers: Vec<usize>) -> Rc<Self> {
        Rc::new(Self::_construct(_RouteData::_construct(
            customers.clone(),
            &CONFIG.truck_distances,
        )))
    }

    fn get_correct_route<'a>(
        truck_routes: &'a [Vec<Rc<TruckRoute>>],
        _: &'a [Vec<Rc<DroneRoute>>],
    ) -> &'a [Vec<Rc<Self>>] {
        truck_routes
    }

    fn get_correct_route_mut<'a>(
        truck_routes: &'a mut Vec<Vec<Rc<TruckRoute>>>,
        _: &'a mut Vec<Vec<Rc<DroneRoute>>>,
    ) -> &'a mut Vec<Vec<Rc<Self>>> {
        truck_routes
    }

    fn single_customer() -> bool {
        false
    }

    fn single_route() -> bool {
        CONFIG.single_truck_route
    }

    fn data(&self) -> &_RouteData {
        &self._data
    }

    fn working_time(&self) -> f64 {
        self._working_time
    }

    fn capacity_violation(&self) -> f64 {
        self._capacity_violation
    }

    fn waiting_time_violation(&self) -> f64 {
        self._waiting_time_violation
    }

    fn _servable(_customer: usize) -> bool {
        true
    }
}

impl TruckRoute {
    fn _calculate_waiting_time_violation(customers: &[usize], working_time: f64) -> f64 {
        let speed = CONFIG.truck.speed;
        let mut waiting_time_violation = 0.0;
        let mut accumulate_time = 0.0;
        for i in 1..customers.len() - 1 {
            accumulate_time += CONFIG.truck_distances[customers[i - 1]][customers[i]] / speed;
            waiting_time_violation += (working_time - accumulate_time - CONFIG.waiting_time_limit).max(0.0);
        }

        waiting_time_violation
    }

    fn _construct(data: _RouteData) -> Self {
        let speed = CONFIG.truck.speed;
        let _working_time = data.value.distance / speed;
        let _capacity_violation = (data.value.weight - CONFIG.truck.capacity).max(0.0);
        let _waiting_time_violation = Self::_calculate_waiting_time_violation(&data.customers, _working_time);

        Self {
            _data: data,
            _working_time,
            _capacity_violation,
            _waiting_time_violation,
        }
    }
}

pub struct DroneRoute {
    _data: _RouteData,
    _working_time: f64,
    _capacity_violation: f64,
    _waiting_time_violation: f64,

    pub energy_violation: f64,
    pub fixed_time_violation: f64,
}

impl fmt::Debug for DroneRoute {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:?}", self.data().customers)
    }
}

impl Route for DroneRoute {
    fn new(customers: Vec<usize>) -> Rc<Self> {
        Rc::new(Self::_construct(_RouteData::_construct(
            customers.clone(),
            &CONFIG.drone_distances,
        )))
    }

    fn get_correct_route<'a>(
        _: &'a [Vec<Rc<TruckRoute>>],
        drone_routes: &'a [Vec<Rc<DroneRoute>>],
    ) -> &'a [Vec<Rc<Self>>] {
        drone_routes
    }

    fn get_correct_route_mut<'a>(
        _: &'a mut Vec<Vec<Rc<TruckRoute>>>,
        drone_routes: &'a mut Vec<Vec<Rc<DroneRoute>>>,
    ) -> &'a mut Vec<Vec<Rc<Self>>> {
        drone_routes
    }

    fn single_customer() -> bool {
        CONFIG.single_drone_route
    }

    fn single_route() -> bool {
        false
    }

    fn data(&self) -> &_RouteData {
        &self._data
    }

    fn working_time(&self) -> f64 {
        self._working_time
    }

    fn capacity_violation(&self) -> f64 {
        self._capacity_violation
    }

    fn waiting_time_violation(&self) -> f64 {
        self._waiting_time_violation
    }

    fn _servable(customer: usize) -> bool {
        CONFIG.dronable[customer]
    }
}

impl DroneRoute {
    fn _construct(data: _RouteData) -> Self {
        let customers = &data.customers;
        let distances = &CONFIG.drone_distances;
        let drone = &CONFIG.drone;

        let _working_time = (CONFIG.drone.takeoff_time() + CONFIG.drone.landing_time()).mul_add(
            customers.len() as f64 - 1.0,
            CONFIG.drone.cruise_time(data.value.distance),
        );
        let _capacity_violation = (data.value.weight - CONFIG.drone.capacity()).max(0.0);

        let mut time = 0.0;
        let mut energy = 0.0;
        let mut weight = 0.0;
        let mut _waiting_time_violation = 0.0;

        let takeoff = drone.takeoff_time();
        let landing = drone.landing_time();
        for i in 0..customers.len() - 1 {
            let cruise = drone.cruise_time(distances[customers[i]][customers[i + 1]]);

            time += takeoff + cruise + landing;
            energy += drone.landing_power(weight).mul_add(
                landing,
                drone
                    .takeoff_power(weight)
                    .mul_add(takeoff, drone.cruise_power(weight) * cruise),
            );
            weight += CONFIG.demands[customers[i]];
            _waiting_time_violation += (_working_time - time - CONFIG.waiting_time_limit).max(0.0);
        }

        let energy_violation = (energy - CONFIG.drone.battery()).max(0.0);
        let fixed_time_violation = (_working_time - CONFIG.drone.fixed_time()).max(0.0);

        Self {
            _data: data,
            _working_time,
            _capacity_violation,
            _waiting_time_violation,
            energy_violation,
            fixed_time_violation,
        }
    }
}

#[derive(Clone, Debug)]
pub enum AnyRoute {
    Truck(Rc<TruckRoute>),
    Drone(Rc<DroneRoute>),
}

impl AnyRoute {
    pub fn from_solution(solution: &Solution) -> (Vec<Vec<Self>>, Vec<Vec<Self>>) {
        (
            solution
                .truck_routes
                .iter()
                .map(|r| r.iter().map(|x| Self::Truck(x.clone())).collect())
                .collect(),
            solution
                .drone_routes
                .iter()
                .map(|r| r.iter().map(|x| Self::Drone(x.clone())).collect())
                .collect(),
        )
    }

    pub fn to_solution(truck_routes: Vec<Vec<Self>>, drone_routes: Vec<Vec<Self>>) -> Solution {
        Solution::new(
            truck_routes
                .into_iter()
                .map(|routes| {
                    routes
                        .into_iter()
                        .map(|route| match route {
                            Self::Truck(truck_route) => truck_route,
                            Self::Drone(_) => panic!("Invalid argument"),
                        })
                        .collect()
                })
                .collect(),
            drone_routes
                .into_iter()
                .map(|routes| {
                    routes
                        .into_iter()
                        .map(|route| match route {
                            Self::Truck(_) => panic!("Invalid argument"),
                            Self::Drone(drone_route) => drone_route,
                        })
                        .collect()
                })
                .collect(),
        )
    }

    pub fn customers(&self) -> &[usize] {
        match self {
            Self::Truck(route) => &route.data().customers,
            Self::Drone(route) => &route.data().customers,
        }
    }

    pub fn inter_route_3(
        &self,
        other_x: &Self,
        other_y: &Self,
        neighborhood: Neighborhood,
    ) -> Vec<(Option<Self>, Self, Self, Vec<usize>)> {
        let mut result = vec![];
        match (self, other_x, other_y) {
            (Self::Truck(r1), Self::Truck(r2), Self::Truck(r3)) => {
                let packed = r1.inter_route_3(r2.clone(), r3.clone(), neighborhood);
                for (ptr1, ptr2, ptr3, tabu) in packed {
                    result.push((ptr1.map(Self::Truck), Self::Truck(ptr2), Self::Truck(ptr3), tabu));
                }
            }
            (Self::Truck(r1), Self::Truck(r2), Self::Drone(r3)) => {
                let packed = r1.inter_route_3(r2.clone(), r3.clone(), neighborhood);
                for (ptr1, ptr2, ptr3, tabu) in packed {
                    result.push((ptr1.map(Self::Truck), Self::Truck(ptr2), Self::Drone(ptr3), tabu));
                }
            }
            (Self::Truck(r1), Self::Drone(r2), Self::Truck(r3)) => {
                let packed = r1.inter_route_3(r2.clone(), r3.clone(), neighborhood);
                for (ptr1, ptr2, ptr3, tabu) in packed {
                    result.push((ptr1.map(Self::Truck), Self::Drone(ptr2), Self::Truck(ptr3), tabu));
                }
            }
            (Self::Truck(r1), Self::Drone(r2), Self::Drone(r3)) => {
                let packed = r1.inter_route_3(r2.clone(), r3.clone(), neighborhood);
                for (ptr1, ptr2, ptr3, tabu) in packed {
                    result.push((ptr1.map(Self::Truck), Self::Drone(ptr2), Self::Drone(ptr3), tabu));
                }
            }
            (Self::Drone(r1), Self::Truck(r2), Self::Truck(r3)) => {
                let packed = r1.inter_route_3(r2.clone(), r3.clone(), neighborhood);
                for (ptr1, ptr2, ptr3, tabu) in packed {
                    result.push((ptr1.map(Self::Drone), Self::Truck(ptr2), Self::Truck(ptr3), tabu));
                }
            }
            (Self::Drone(r1), Self::Truck(r2), Self::Drone(r3)) => {
                let packed = r1.inter_route_3(r2.clone(), r3.clone(), neighborhood);
                for (ptr1, ptr2, ptr3, tabu) in packed {
                    result.push((ptr1.map(Self::Drone), Self::Truck(ptr2), Self::Drone(ptr3), tabu));
                }
            }
            (Self::Drone(r1), Self::Drone(r2), Self::Truck(r3)) => {
                let packed = r1.inter_route_3(r2.clone(), r3.clone(), neighborhood);
                for (ptr1, ptr2, ptr3, tabu) in packed {
                    result.push((ptr1.map(Self::Drone), Self::Drone(ptr2), Self::Truck(ptr3), tabu));
                }
            }
            (Self::Drone(r1), Self::Drone(r2), Self::Drone(r3)) => {
                let packed = r1.inter_route_3(r2.clone(), r3.clone(), neighborhood);
                for (ptr1, ptr2, ptr3, tabu) in packed {
                    result.push((ptr1.map(Self::Drone), Self::Drone(ptr2), Self::Drone(ptr3), tabu));
                }
            }
        }

        result
    }
}
