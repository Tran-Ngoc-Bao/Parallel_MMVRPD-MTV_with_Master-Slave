#include "routes.hpp"
#include "solutions.hpp"
#include <cmath>
#include <stdexcept>

// -----------------------------------------------------------------------
// Route evaluation (shared by make() and the neighbourhoods)
// -----------------------------------------------------------------------
RouteVals evaluate_truck(const std::vector<size_t>& c)
{
    const Config& cfg = global_config();
    const auto& d = cfg.truck_distances;
    RouteVals v;
    for (size_t i = 0; i + 1 < c.size(); ++i) {
        v.distance += d[c[i]][c[i+1]];
        v.weight   += cfg.demands[c[i]];
    }

    double speed = cfg.truck.speed;
    v.working_time       = v.distance / speed;
    v.capacity_violation = std::max(0.0, v.weight - cfg.truck.capacity);

    // Waiting time violation
    double wtv    = 0.0;
    double accum  = 0.0;
    for (size_t i = 1; i + 1 < c.size(); ++i) {
        accum += d[c[i-1]][c[i]] / speed;
        wtv   += std::max(0.0, v.working_time - accum - cfg.waiting_time_limit);
    }
    v.waiting_time_violation = wtv;
    return v;
}

RouteVals evaluate_drone(const std::vector<size_t>& c)
{
    const Config& cfg = global_config();
    const DroneConfig& drone = cfg.drone;
    const auto& dist = cfg.drone_distances;
    RouteVals v;
    for (size_t i = 0; i + 1 < c.size(); ++i) {
        v.distance += dist[c[i]][c[i+1]];
        v.weight   += cfg.demands[c[i]];
    }

    double takeoff  = drone.takeoff_time();
    double landing  = drone.landing_time();

    v.working_time = (takeoff + landing) * static_cast<double>(c.size() - 1)
                     + drone.cruise_time(v.distance);
    v.capacity_violation = std::max(0.0, v.weight - drone.capacity());

    double time   = 0.0;
    double energy = 0.0;
    double weight = 0.0;
    double wtv    = 0.0;

    for (size_t i = 0; i + 1 < c.size(); ++i) {
        double cruise = drone.cruise_time(dist[c[i]][c[i+1]]);
        time   += takeoff + cruise + landing;
        energy += drone.landing_power(weight) * landing
                + drone.takeoff_power(weight) * takeoff
                + drone.cruise_power(weight)  * cruise;
        weight += cfg.demands[c[i]];
        wtv    += std::max(0.0, v.working_time - time - cfg.waiting_time_limit);
    }

    v.energy_violation       = std::max(0.0, energy - drone.battery());
    v.fixed_time_violation   = std::max(0.0, v.working_time - drone.fixed_time());
    v.waiting_time_violation = wtv;
    return v;
}

// -----------------------------------------------------------------------
// TruckRoute::make
// -----------------------------------------------------------------------
LocalRc<TruckRoute> TruckRoute::make(std::vector<size_t> customers)
{
    assert(customers.front() == 0 && customers.back() == 0 && customers.size() >= 3);
    auto r = LocalRc<TruckRoute>::make(TruckRoute::PrivateTag{});
    const RouteVals v = evaluate_truck(customers);
    r->_data.customers           = std::move(customers);
    r->_data.distance            = v.distance;
    r->_data.weight              = v.weight;
    r->_working_time             = v.working_time;
    r->_capacity_violation       = v.capacity_violation;
    r->_waiting_time_violation   = v.waiting_time_violation;
    return r;
}

// -----------------------------------------------------------------------
// DroneRoute::make
// -----------------------------------------------------------------------
LocalRc<DroneRoute> DroneRoute::make(std::vector<size_t> customers)
{
    assert(customers.front() == 0 && customers.back() == 0 && customers.size() >= 3);
    auto r = LocalRc<DroneRoute>::make(DroneRoute::PrivateTag{});
    const RouteVals v = evaluate_drone(customers);
    r->_data.customers           = std::move(customers);
    r->_data.distance            = v.distance;
    r->_data.weight              = v.weight;
    r->_working_time             = v.working_time;
    r->_capacity_violation       = v.capacity_violation;
    r->_waiting_time_violation   = v.waiting_time_violation;
    r->energy_violation          = v.energy_violation;
    r->fixed_time_violation      = v.fixed_time_violation;
    return r;
}

// -----------------------------------------------------------------------
// AnyRoute::from_solution / to_solution
// -----------------------------------------------------------------------
std::pair<std::vector<std::vector<AnyRoute>>,
          std::vector<std::vector<AnyRoute>>>
AnyRoute::from_solution(const Solution& sol)
{
    std::vector<std::vector<AnyRoute>> tr, dr;
    for (const auto& routes : sol.truck_routes) {
        tr.push_back({});
        for (const auto& rt : routes) tr.back().emplace_back(rt);
    }
    for (const auto& routes : sol.drone_routes) {
        dr.push_back({});
        for (const auto& rt : routes) dr.back().emplace_back(rt);
    }
    return {tr, dr};
}

Solution AnyRoute::to_solution(
    std::vector<std::vector<AnyRoute>> truck_routes,
    std::vector<std::vector<AnyRoute>> drone_routes)
{
    std::vector<std::vector<LocalRc<TruckRoute>>> tr;
    std::vector<std::vector<LocalRc<DroneRoute>>> dr;

    for (auto& routes : truck_routes) {
        tr.push_back({});
        for (auto& rt : routes) {
            if (rt.type != AnyRoute::Type::Truck) throw std::logic_error("Expected truck");
            tr.back().push_back(rt.truck);
        }
    }
    for (auto& routes : drone_routes) {
        dr.push_back({});
        for (auto& rt : routes) {
            if (rt.type != AnyRoute::Type::Drone) throw std::logic_error("Expected drone");
            dr.back().push_back(rt.drone);
        }
    }
    return Solution::make(std::move(tr), std::move(dr));
}

// -----------------------------------------------------------------------
// AnyRoute::inter_route_3_any – dispatches all 8 type combinations
// -----------------------------------------------------------------------
std::vector<std::tuple<
    std::optional<AnyRoute>, AnyRoute, AnyRoute, std::vector<size_t>>>
AnyRoute::inter_route_3_any(const AnyRoute& ox, const AnyRoute& oy,
                             Neighborhood n) const
{
    using Opt = std::optional<AnyRoute>;
    std::vector<std::tuple<Opt, AnyRoute, AnyRoute, std::vector<size_t>>> result;

    auto pack_truck = [](LocalRc<TruckRoute> p) -> AnyRoute { return AnyRoute(p); };
    auto pack_drone = [](LocalRc<DroneRoute> p) -> AnyRoute { return AnyRoute(p); };

#define DO_COMBO(R1, R2, R3, WRAP_I, WRAP_J, WRAP_K) \
    { \
        auto packed = r1->inter_route_3(r2, r3, n); \
        for (auto& [p1, p2, p3, tabu] : packed) { \
            Opt opt_i = p1 ? Opt{WRAP_I(*p1)} : Opt{}; \
            result.emplace_back(opt_i, WRAP_J(p2), WRAP_K(p3), std::move(tabu)); \
        } \
    }

    if (type == Type::Truck && ox.type == Type::Truck && oy.type == Type::Truck) {
        auto r1=truck; auto r2=ox.truck; auto r3=oy.truck;
        DO_COMBO(Truck,Truck,Truck, pack_truck, pack_truck, pack_truck)
    } else if (type == Type::Truck && ox.type == Type::Truck && oy.type == Type::Drone) {
        auto r1=truck; auto r2=ox.truck; auto r3=oy.drone;
        DO_COMBO(Truck,Truck,Drone, pack_truck, pack_truck, pack_drone)
    } else if (type == Type::Truck && ox.type == Type::Drone && oy.type == Type::Truck) {
        auto r1=truck; auto r2=ox.drone; auto r3=oy.truck;
        DO_COMBO(Truck,Drone,Truck, pack_truck, pack_drone, pack_truck)
    } else if (type == Type::Truck && ox.type == Type::Drone && oy.type == Type::Drone) {
        auto r1=truck; auto r2=ox.drone; auto r3=oy.drone;
        DO_COMBO(Truck,Drone,Drone, pack_truck, pack_drone, pack_drone)
    } else if (type == Type::Drone && ox.type == Type::Truck && oy.type == Type::Truck) {
        auto r1=drone; auto r2=ox.truck; auto r3=oy.truck;
        DO_COMBO(Drone,Truck,Truck, pack_drone, pack_truck, pack_truck)
    } else if (type == Type::Drone && ox.type == Type::Truck && oy.type == Type::Drone) {
        auto r1=drone; auto r2=ox.truck; auto r3=oy.drone;
        DO_COMBO(Drone,Truck,Drone, pack_drone, pack_truck, pack_drone)
    } else if (type == Type::Drone && ox.type == Type::Drone && oy.type == Type::Truck) {
        auto r1=drone; auto r2=ox.drone; auto r3=oy.truck;
        DO_COMBO(Drone,Drone,Truck, pack_drone, pack_drone, pack_truck)
    } else {
        auto r1=drone; auto r2=ox.drone; auto r3=oy.drone;
        DO_COMBO(Drone,Drone,Drone, pack_drone, pack_drone, pack_drone)
    }
#undef DO_COMBO
    return result;
}
