#pragma once
#include <vector>
#include <memory>
#include "rc.hpp"
#include <optional>
#include <tuple>
#include <deque>
#include <algorithm>
#include <cassert>
#include <cmath>
#include "neighborhoods.hpp"
#include "config.hpp"

// Forward declarations
class TruckRoute;
class DroneRoute;
struct Solution;

// -----------------------------------------------------------------------
// RouteData
// -----------------------------------------------------------------------
struct RouteData {
    std::vector<size_t> customers;
    double distance = 0.0;
    double weight   = 0.0;

    static RouteData construct(std::vector<size_t> customers,
                               const std::vector<std::vector<double>>& distances)
    {
        assert(customers.front() == 0);
        assert(customers.back()  == 0);
        assert(customers.size()  >= 3);
        RouteData d;
        d.customers = std::move(customers);
        for (size_t i = 0; i + 1 < d.customers.size(); ++i) {
            d.distance += distances[d.customers[i]][d.customers[i+1]];
            d.weight   += global_config().demands[d.customers[i]];
        }
        return d;
    }
};

// -----------------------------------------------------------------------
// Abstract base
// -----------------------------------------------------------------------
class RouteBase {
public:
    virtual const RouteData& data() const = 0;
    virtual double working_time() const = 0;
    virtual double capacity_violation() const = 0;
    virtual double waiting_time_violation() const = 0;
    virtual ~RouteBase() = default;
};

// -----------------------------------------------------------------------
// RouteHelper – static dispatch per type (specialised after class defs)
// -----------------------------------------------------------------------
template<typename T> struct RouteHelper;

// -----------------------------------------------------------------------
// Utility helpers
// -----------------------------------------------------------------------
template<typename T>
T swap_remove_elem(std::vector<T>& v, size_t idx) {
    T elem = std::move(v[idx]);
    if (idx != v.size() - 1) v[idx] = std::move(v.back());
    v.pop_back();
    return elem;
}

template<typename T>
void swap_push(std::vector<T>& v, size_t idx, T elem) {
    size_t l = v.size();
    v.push_back(std::move(elem));
    std::swap(v[idx], v[l]);
}

// -----------------------------------------------------------------------
// CRTP Route base – provides push/pop and all neighborhood operations
// -----------------------------------------------------------------------
template<typename Derived>
class Route : public RouteBase {
public:
    // ---- Shared manipulators ----
    LocalRc<Derived> route_push(size_t customer) const {
        const auto& custs = data().customers;
        auto nc = custs;
        nc.insert(nc.end() - 1, customer);
        return RouteHelper<Derived>::make(std::move(nc));
    }

    LocalRc<Derived> route_pop() const {
        const auto& custs = data().customers;
        auto nc = custs;
        nc.erase(nc.end() - 2);
        return RouteHelper<Derived>::make(std::move(nc));
    }

    // ---- inter_route_extract ----
    template<typename T>
    std::vector<std::tuple<
        LocalRc<Derived>,
        LocalRc<T>,
        std::vector<size_t>>>
    inter_route_extract(Neighborhood n) const
    {
        using Ret = std::tuple<LocalRc<Derived>, LocalRc<T>, std::vector<size_t>>;
        std::vector<Ret> results;
        const auto& customers = data().customers;
        std::deque<size_t> queue;

        size_t sz = 0;
        if (n == Neighborhood::Move10) sz = 1;
        else if (n == Neighborhood::Move20) sz = 2;
        if (sz == 0 || customers.size() - 2 <= sz) return results;

        for (size_t i = 1; i + 1 < customers.size(); ++i) {
            if (RouteHelper<T>::servable(customers[i])) {
                queue.push_back(customers[i]);
                if (queue.size() > sz) queue.pop_front();
                if (queue.size() == sz) {
                    std::vector<size_t> original(customers.begin(), customers.begin() + (i - sz + 1));
                    original.insert(original.end(), customers.begin() + i + 1, customers.end());

                    std::vector<size_t> route = {0};
                    route.insert(route.end(), queue.begin(), queue.end());
                    route.push_back(0);

                    std::vector<size_t> tabu(customers.begin() + (i - sz + 1), customers.begin() + i + 1);
                    results.emplace_back(
                        RouteHelper<Derived>::make(original),
                        RouteHelper<T>::make(route),
                        tabu);
                }
            } else {
                queue.clear();
            }
        }
        return results;
    }

    // ---- inter_route ----
    template<typename T>
    std::vector<std::tuple<
        std::optional<LocalRc<Derived>>,
        std::optional<LocalRc<T>>,
        std::vector<size_t>>>
    inter_route(LocalRc<T> other, Neighborhood n) const
    {
        using Opt_D = std::optional<LocalRc<Derived>>;
        using Opt_T = std::optional<LocalRc<T>>;
        using Ret   = std::tuple<Opt_D, Opt_T, std::vector<size_t>>;
        std::vector<Ret> results;

        const auto& ci = data().customers;
        const auto& cj = other->data().customers;
        size_t li = ci.size(), lj = cj.size();

        auto bi = ci; // mutable buffer_i
        auto bj = cj; // mutable buffer_j

        switch (n) {
        case Neighborhood::Move10:
            for (size_t idx_i = 1; idx_i + 1 < li; ++idx_i) {
                if (!RouteHelper<T>::servable(ci[idx_i])) continue;
                size_t removed = bi[idx_i];
                bi.erase(bi.begin() + static_cast<ptrdiff_t>(idx_i));
                Opt_D ri = (li == 3) ? Opt_D{} : Opt_D{RouteHelper<Derived>::make(bi)};
                std::vector<size_t> tabu = {removed};
                bj.insert(bj.begin() + 1, removed);
                for (size_t idx_j = 1; idx_j < lj; ++idx_j) {
                    results.emplace_back(ri, Opt_T{RouteHelper<T>::make(bj)}, tabu);
                    std::swap(bj[idx_j], bj[idx_j+1]);
                }
                bi.insert(bi.begin() + static_cast<ptrdiff_t>(idx_i), removed);
                bj.pop_back();
            }
            break;

        case Neighborhood::Move11:
            for (size_t idx_i = 1; idx_i + 1 < li; ++idx_i) {
                if (!RouteHelper<T>::servable(bi[idx_i])) continue;
                for (size_t idx_j = 1; idx_j + 1 < lj; ++idx_j) {
                    if (!RouteHelper<Derived>::servable(bj[idx_j])) continue;
                    std::swap(bi[idx_i], bj[idx_j]);
                    std::vector<size_t> tabu = {ci[idx_i], cj[idx_j]};
                    results.emplace_back(
                        Opt_D{RouteHelper<Derived>::make(bi)},
                        Opt_T{RouteHelper<T>::make(bj)},
                        tabu);
                    std::swap(bi[idx_i], bj[idx_j]);
                }
            }
            break;

        case Neighborhood::Move20:
            for (size_t idx_i = 1; idx_i + 2 < li; ++idx_i) {
                if (!RouteHelper<T>::servable(bi[idx_i]) ||
                    !RouteHelper<T>::servable(bi[idx_i+1])) continue;
                size_t rx = bi[idx_i];
                bi.erase(bi.begin() + static_cast<ptrdiff_t>(idx_i));
                size_t ry = bi[idx_i];
                bi.erase(bi.begin() + static_cast<ptrdiff_t>(idx_i));
                Opt_D ri = (li == 4) ? Opt_D{} : Opt_D{RouteHelper<Derived>::make(bi)};
                std::vector<size_t> tabu = {rx, ry};
                bj.insert(bj.begin()+1, rx);
                bj.insert(bj.begin()+2, ry);
                for (size_t idx_j = 1; idx_j < lj; ++idx_j) {
                    results.emplace_back(ri, Opt_T{RouteHelper<T>::make(bj)}, tabu);
                    std::swap(bj[idx_j+1], bj[idx_j+2]);
                    std::swap(bj[idx_j],   bj[idx_j+1]);
                }
                bi.insert(bi.begin() + static_cast<ptrdiff_t>(idx_i),   rx);
                bi.insert(bi.begin() + static_cast<ptrdiff_t>(idx_i)+1, ry);
                bj.pop_back(); bj.pop_back();
            }
            break;

        case Neighborhood::Move21:
            for (size_t idx_i = 1; idx_i + 2 < li; ++idx_i) {
                if (!RouteHelper<T>::servable(bi[idx_i]) ||
                    !RouteHelper<T>::servable(bi[idx_i+1])) continue;
                std::swap(bi[idx_i], bj[1]);
                bj.insert(bj.begin()+2, bi[idx_i+1]);
                bi.erase(bi.begin() + static_cast<ptrdiff_t>(idx_i+1));
                for (size_t idx_j = 1; idx_j + 1 < lj; ++idx_j) {
                    if (RouteHelper<Derived>::servable(bj[idx_j])) {
                        std::vector<size_t> tabu = {bj[idx_j], bj[idx_j+1], bi[idx_i]};
                        results.emplace_back(
                            Opt_D{RouteHelper<Derived>::make(bi)},
                            Opt_T{RouteHelper<T>::make(bj)},
                            tabu);
                    }
                    std::swap(bi[idx_i], bj[idx_j+2]);
                    std::swap(bj[idx_j+1], bj[idx_j+2]);
                    std::swap(bj[idx_j],   bj[idx_j+1]);
                }
                std::swap(bi[idx_i], bj[lj-1]);
                bi.insert(bi.begin() + static_cast<ptrdiff_t>(idx_i+1), bj.back());
                bj.pop_back();
            }
            break;

        case Neighborhood::Move22:
            for (size_t idx_i = 1; idx_i + 2 < li; ++idx_i) {
                if (!RouteHelper<T>::servable(bi[idx_i]) ||
                    !RouteHelper<T>::servable(bi[idx_i+1])) continue;
                for (size_t idx_j = 1; idx_j + 2 < lj; ++idx_j) {
                    if (!RouteHelper<Derived>::servable(bj[idx_j]) ||
                        !RouteHelper<Derived>::servable(bj[idx_j+1])) continue;
                    std::swap(bi[idx_i],   bj[idx_j]);
                    std::swap(bi[idx_i+1], bj[idx_j+1]);
                    // tabu uses the swapped values: after swap, bi has what was in bj and vice-versa
                    std::vector<size_t> tabu = {bi[idx_i], bi[idx_i+1], bj[idx_j], bj[idx_j+1]};
                    results.emplace_back(
                        Opt_D{RouteHelper<Derived>::make(bi)},
                        Opt_T{RouteHelper<T>::make(bj)},
                        tabu);
                    std::swap(bi[idx_i],   bj[idx_j]);
                    std::swap(bi[idx_i+1], bj[idx_j+1]);
                }
            }
            break;

        case Neighborhood::TwoOpt: {
            size_t off_i = li - 1;
            while (off_i > 1 && RouteHelper<T>::servable(ci[off_i-1])) --off_i;
            size_t off_j = lj - 1;
            while (off_j > 1 && RouteHelper<Derived>::servable(cj[off_j-1])) --off_j;
            for (size_t idx_i = off_i; idx_i + 1 < li; ++idx_i) {
                for (size_t idx_j = off_j; idx_j + 1 < lj; ++idx_j) {
                    std::vector<size_t> ni(ci.begin(), ci.begin()+idx_i);
                    std::vector<size_t> nj(cj.begin(), cj.begin()+idx_j);
                    ni.insert(ni.end(), cj.begin()+idx_j, cj.end());
                    nj.insert(nj.end(), ci.begin()+idx_i, ci.end());
                    std::vector<size_t> tabu = {ni[idx_i], nj[idx_j]};
                    results.emplace_back(
                        Opt_D{RouteHelper<Derived>::make(ni)},
                        Opt_T{RouteHelper<T>::make(nj)},
                        tabu);
                }
            }
            break;
        }

        default:
            throw std::runtime_error("inter_route: invalid neighborhood");
        }
        return results;
    }

    // ---- inter_route_3 (EjectionChain only) ----
    template<typename T1, typename T2>
    std::vector<std::tuple<
        std::optional<LocalRc<Derived>>,
        LocalRc<T1>,
        LocalRc<T2>,
        std::vector<size_t>>>
    inter_route_3(LocalRc<T1> other_x, LocalRc<T2> other_y,
                  Neighborhood /*n – must be EjectionChain*/) const
    {
        using Opt_D = std::optional<LocalRc<Derived>>;
        using Ret   = std::tuple<Opt_D, LocalRc<T1>, LocalRc<T2>, std::vector<size_t>>;
        std::vector<Ret> results;

        const auto& ci = data().customers;
        const auto& cj = other_x->data().customers;
        const auto& ck = other_y->data().customers;
        size_t li = ci.size(), lj = cj.size(), lk = ck.size();

        auto bi = ci;
        auto bj = cj;
        auto bk = ck;

        for (size_t idx_i = 1; idx_i + 1 < li; ++idx_i) {
            if (!RouteHelper<T1>::servable(bi[idx_i])) continue;
            size_t remove_x = bi[idx_i];
            bi.erase(bi.begin() + static_cast<ptrdiff_t>(idx_i));

            for (size_t idx_j = 1; idx_j + 1 < lj; ++idx_j) {
                if (!RouteHelper<T2>::servable(bj[idx_j])) continue;
                bk.insert(bk.begin()+1, bj[idx_j]);
                bj[idx_j] = remove_x;

                for (size_t idx_k = 1; idx_k < lk; ++idx_k) {
                    std::vector<size_t> tabu = {remove_x, bk[idx_k]};
                    Opt_D pi = (bi.size() == 2) ? Opt_D{} : Opt_D{RouteHelper<Derived>::make(bi)};
                    results.emplace_back(pi,
                        RouteHelper<T1>::make(bj),
                        RouteHelper<T2>::make(bk),
                        tabu);
                    std::swap(bk[idx_k], bk[idx_k+1]);
                }
                bj[idx_j] = bk.back();
                bk.pop_back();
            }
            bi.insert(bi.begin() + static_cast<ptrdiff_t>(idx_i), remove_x);
        }
        return results;
    }

    // ---- intra_route ----
    std::vector<std::pair<LocalRc<Derived>, std::vector<size_t>>>
    intra_route(Neighborhood n) const
    {
        using Ret = std::pair<LocalRc<Derived>, std::vector<size_t>>;
        std::vector<Ret> results;
        const auto& custs = data().customers;
        size_t len = custs.size();
        auto buf = custs;

        switch (n) {
        case Neighborhood::Move10:
            // Forward pass: move each element rightward
            for (size_t i = 1; i + 2 < len; ++i) {  // i: 1..len-3
                for (size_t j = i; j + 2 < len; ++j) {  // j: i..len-3
                    std::swap(buf[j], buf[j+1]);
                    results.emplace_back(RouteHelper<Derived>::make(buf),
                                         std::vector<size_t>{custs[i]});
                }
                // rotate_right(1) on buf[i..len-1): last element goes to front
                std::rotate(buf.begin()+i, buf.begin()+(len-2), buf.begin()+(len-1));
            }
            // Backward pass: move each element leftward
            for (size_t i = 2; i + 1 < len; ++i) {  // i: 2..len-2
                for (size_t j = i; j > 1; --j) {
                    std::swap(buf[j-1], buf[j]);
                    results.emplace_back(RouteHelper<Derived>::make(buf),
                                         std::vector<size_t>{custs[i]});
                }
                // rotate_left(1) on buf[1..i+1): first element goes to back
                std::rotate(buf.begin()+1, buf.begin()+2, buf.begin()+i+1);
            }
            break;

        case Neighborhood::Move11:
            for (size_t i = 1; i + 2 < len; ++i) {
                for (size_t j = i; j + 2 < len; ++j) {
                    std::swap(buf[j], buf[j+1]);
                    std::swap(buf[i], buf[j]);
                    results.emplace_back(RouteHelper<Derived>::make(buf),
                                         std::vector<size_t>{custs[i], custs[j+1]});
                }
                std::swap(buf[i], buf[len-2]);
            }
            break;

        case Neighborhood::Move20:
            // Forward pass
            for (size_t i = 1; i + 3 < len; ++i) {  // i: 1..len-4 (pair starts)
                for (size_t j = i+1; j + 2 < len; ++j) {  // j: i+1..len-3
                    std::swap(buf[j], buf[j+1]);
                    std::swap(buf[j-1], buf[j]);
                    results.emplace_back(RouteHelper<Derived>::make(buf),
                                         std::vector<size_t>{custs[i], custs[i+1]});
                }
                // rotate_right(2) on buf[i..len-1)
                std::rotate(buf.begin()+i, buf.begin()+(len-3), buf.begin()+(len-1));
            }
            // Backward pass
            for (size_t i = 2; i + 2 < len; ++i) {  // i: 2..len-3
                for (size_t j = i-1; j >= 1 && j < i; --j) {
                    std::swap(buf[j+1], buf[j+2]);
                    std::swap(buf[j],   buf[j+2]);
                    results.emplace_back(RouteHelper<Derived>::make(buf),
                                         std::vector<size_t>{custs[i], custs[i+1]});
                    if (j == 0) break;
                }
                // rotate_left(2) on buf[1..i+2)
                std::rotate(buf.begin()+1, buf.begin()+3, buf.begin()+i+2);
            }
            break;

        case Neighborhood::Move21:
            // Forward pass
            for (size_t i = 1; i + 3 < len; ++i) {  // i: 1..len-4
                for (size_t j = i; j + 3 < len; ++j) {  // j: i..len-4
                    std::swap(buf[j+1], buf[j+2]);
                    std::swap(buf[j],   buf[j+1]);
                    std::swap(buf[i],   buf[j]);
                    results.emplace_back(RouteHelper<Derived>::make(buf),
                                         std::vector<size_t>{custs[i], custs[i+1], custs[j+2]});
                }
                std::swap(buf[i], buf[len-3]);
                // rotate_right(1) on buf[i+1..len-1)
                std::rotate(buf.begin()+i+1, buf.begin()+(len-2), buf.begin()+(len-1));
            }
            // Backward pass
            for (size_t i = 2; i + 2 < len; ++i) {  // i: 2..len-3
                for (size_t j = i-1; j >= 1 && j < i; --j) {
                    std::swap(buf[j+1], buf[j+2]);
                    std::swap(buf[j],   buf[j+2]);
                    std::swap(buf[j+2], buf[i+1]);
                    results.emplace_back(RouteHelper<Derived>::make(buf),
                                         std::vector<size_t>{custs[i], custs[i+1], custs[j]});
                    if (j == 0) break;
                }
                std::swap(buf[1], buf[i+1]);
                // rotate_left(1) on buf[2..i+2)
                std::rotate(buf.begin()+2, buf.begin()+3, buf.begin()+i+2);
            }
            break;

        case Neighborhood::Move22:
            for (size_t i = 1; i + 4 < len; ++i) {  // i: 1..len-5 (saturating_sub(4) exclusive)
                {
                    std::swap(buf[i],   buf[i+2]);
                    std::swap(buf[i+1], buf[i+3]);
                    results.emplace_back(RouteHelper<Derived>::make(buf),
                                         std::vector<size_t>{custs[i],custs[i+1],custs[i+2],custs[i+3]});
                }
                for (size_t j = i+3; j + 2 < len; ++j) {  // j: i+3..len-3
                    std::swap(buf[i],   buf[i+1]);
                    std::swap(buf[i+1], buf[j+1]);
                    std::swap(buf[j],   buf[j+1]);
                    std::swap(buf[j-1], buf[j]);
                    results.emplace_back(RouteHelper<Derived>::make(buf),
                                         std::vector<size_t>{custs[i],custs[i+1],custs[j],custs[j+1]});
                }
                std::swap(buf[i],   buf[len-3]);
                std::swap(buf[i+1], buf[len-2]);
            }
            break;

        case Neighborhood::TwoOpt:
            for (size_t i = 1; i + 2 < len; ++i) {
                {
                    std::swap(buf[i], buf[i+1]);
                    results.emplace_back(RouteHelper<Derived>::make(buf),
                                         std::vector<size_t>{custs[i], custs[i+1]});
                }
                for (size_t j = i+2; j + 1 < len; ++j) {  // j: i+2..len-2
                    // rotate_right(1) on buf[i..j+1)
                    std::rotate(buf.begin()+i, buf.begin()+j, buf.begin()+j+1);
                    results.emplace_back(RouteHelper<Derived>::make(buf),
                                         std::vector<size_t>{custs[i], custs[j]});
                }
                std::reverse(buf.begin()+i, buf.begin()+(len-1));
            }
            break;

        default:
            throw std::runtime_error("intra_route: invalid neighborhood");
        }

        for (auto& [r, tabu] : results) {
            std::sort(tabu.begin(), tabu.end());
        }
        return results;
    }
};

// -----------------------------------------------------------------------
// TruckRoute
// -----------------------------------------------------------------------
class TruckRoute : public Route<TruckRoute> {
public:
    RouteData _data;
    double _working_time          = 0;
    double _capacity_violation    = 0;
    double _waiting_time_violation= 0;

    const RouteData& data()               const override { return _data; }
    double working_time()                 const override { return _working_time; }
    double capacity_violation()           const override { return _capacity_violation; }
    double waiting_time_violation()       const override { return _waiting_time_violation; }

    static LocalRc<TruckRoute> make(std::vector<size_t> customers);
    static LocalRc<TruckRoute> make_single(size_t customer) {
        return make({0, customer, 0});
    }

    static bool servable(size_t /*c*/) { return true; }
    static bool single_customer()      { return false; }
    static bool single_route()         { return global_config().single_truck_route; }

    static std::vector<std::vector<LocalRc<TruckRoute>>>& get_routes_mut(
        std::vector<std::vector<LocalRc<TruckRoute>>>& tr,
        std::vector<std::vector<LocalRc<DroneRoute>>>& /*dr*/) { return tr; }

    static const std::vector<std::vector<LocalRc<TruckRoute>>>& get_routes(
        const std::vector<std::vector<LocalRc<TruckRoute>>>& tr,
        const std::vector<std::vector<LocalRc<DroneRoute>>>& /*dr*/) { return tr; }

    // Passkey for LocalRc construction — only TruckRoute can create PrivateTag
    struct PrivateTag { friend class TruckRoute; private: PrivateTag() = default; };
    explicit TruckRoute(PrivateTag) {}

private:
    TruckRoute() = default;
    friend class Route<TruckRoute>;
};

// -----------------------------------------------------------------------
// DroneRoute
// -----------------------------------------------------------------------
class DroneRoute : public Route<DroneRoute> {
public:
    RouteData _data;
    double _working_time          = 0;
    double _capacity_violation    = 0;
    double _waiting_time_violation= 0;
    double energy_violation       = 0;
    double fixed_time_violation   = 0;

    const RouteData& data()               const override { return _data; }
    double working_time()                 const override { return _working_time; }
    double capacity_violation()           const override { return _capacity_violation; }
    double waiting_time_violation()       const override { return _waiting_time_violation; }

    static LocalRc<DroneRoute> make(std::vector<size_t> customers);
    static LocalRc<DroneRoute> make_single(size_t customer) {
        return make({0, customer, 0});
    }

    static bool servable(size_t c)  { return global_config().dronable[c]; }
    static bool single_customer()   { return global_config().single_drone_route; }
    static bool single_route()      { return false; }

    static std::vector<std::vector<LocalRc<DroneRoute>>>& get_routes_mut(
        std::vector<std::vector<LocalRc<TruckRoute>>>& /*tr*/,
        std::vector<std::vector<LocalRc<DroneRoute>>>& dr) { return dr; }

    static const std::vector<std::vector<LocalRc<DroneRoute>>>& get_routes(
        const std::vector<std::vector<LocalRc<TruckRoute>>>& /*tr*/,
        const std::vector<std::vector<LocalRc<DroneRoute>>>& dr) { return dr; }

    // Passkey for LocalRc construction — only DroneRoute can create PrivateTag
    struct PrivateTag { friend class DroneRoute; private: PrivateTag() = default; };
    explicit DroneRoute(PrivateTag) {}

private:
    DroneRoute() = default;
    friend class Route<DroneRoute>;
};

// -----------------------------------------------------------------------
// RouteHelper specialisations
// -----------------------------------------------------------------------
template<> struct RouteHelper<TruckRoute> {
    static bool servable(size_t c)  { return TruckRoute::servable(c); }
    static bool single_customer()   { return TruckRoute::single_customer(); }
    static bool single_route()      { return TruckRoute::single_route(); }
    static LocalRc<TruckRoute> make(std::vector<size_t> c) {
        return TruckRoute::make(std::move(c));
    }
    static std::vector<std::vector<LocalRc<TruckRoute>>>& get_routes_mut(
        std::vector<std::vector<LocalRc<TruckRoute>>>& tr,
        std::vector<std::vector<LocalRc<DroneRoute>>>& dr) {
        return TruckRoute::get_routes_mut(tr, dr);
    }
    static const std::vector<std::vector<LocalRc<TruckRoute>>>& get_routes(
        const std::vector<std::vector<LocalRc<TruckRoute>>>& tr,
        const std::vector<std::vector<LocalRc<DroneRoute>>>& dr) {
        return TruckRoute::get_routes(tr, dr);
    }
};

template<> struct RouteHelper<DroneRoute> {
    static bool servable(size_t c)  { return DroneRoute::servable(c); }
    static bool single_customer()   { return DroneRoute::single_customer(); }
    static bool single_route()      { return DroneRoute::single_route(); }
    static LocalRc<DroneRoute> make(std::vector<size_t> c) {
        return DroneRoute::make(std::move(c));
    }
    static std::vector<std::vector<LocalRc<DroneRoute>>>& get_routes_mut(
        std::vector<std::vector<LocalRc<TruckRoute>>>& tr,
        std::vector<std::vector<LocalRc<DroneRoute>>>& dr) {
        return DroneRoute::get_routes_mut(tr, dr);
    }
    static const std::vector<std::vector<LocalRc<DroneRoute>>>& get_routes(
        const std::vector<std::vector<LocalRc<TruckRoute>>>& tr,
        const std::vector<std::vector<LocalRc<DroneRoute>>>& dr) {
        return DroneRoute::get_routes(tr, dr);
    }
};

// -----------------------------------------------------------------------
// AnyRoute
// -----------------------------------------------------------------------
struct AnyRoute {
    enum class Type { Truck, Drone } type;
    LocalRc<TruckRoute> truck;
    LocalRc<DroneRoute> drone;

    explicit AnyRoute(LocalRc<TruckRoute> t) : type(Type::Truck), truck(std::move(t)) {}
    explicit AnyRoute(LocalRc<DroneRoute> d) : type(Type::Drone), drone(std::move(d)) {}

    const std::vector<size_t>& customers() const {
        return type == Type::Truck ? truck->data().customers : drone->data().customers;
    }

    static std::pair<std::vector<std::vector<AnyRoute>>,
                     std::vector<std::vector<AnyRoute>>>
    from_solution(const Solution& sol);

    static Solution to_solution(
        std::vector<std::vector<AnyRoute>> truck_routes,
        std::vector<std::vector<AnyRoute>> drone_routes);

    std::vector<std::tuple<
        std::optional<AnyRoute>,
        AnyRoute,
        AnyRoute,
        std::vector<size_t>>>
    inter_route_3_any(const AnyRoute& other_x, const AnyRoute& other_y,
                      Neighborhood n) const;
};
