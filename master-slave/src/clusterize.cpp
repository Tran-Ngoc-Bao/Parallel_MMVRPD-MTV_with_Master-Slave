#include "clusterize.hpp"
#include "config.hpp"
#include <unordered_map>
#include <cmath>
#include <algorithm>
#include <numbers>
#include <cmath>

std::vector<std::vector<size_t>> clusterize(std::vector<size_t>& customers, size_t k)
{
    std::vector<std::vector<size_t>> clusters(k);
    if (customers.empty()) return clusters;

    const Config& cfg = global_config();
    const auto& x = cfg.x;
    const auto& y = cfg.y;

    std::unordered_map<size_t, double> angles;
    for (size_t c : customers) {
        double angle = std::atan2(y[c] - y[0], x[c] - x[0]);
        if (angle < 0.0) angle += 2.0 * M_PI;
        angles[c] = angle;
    }

    std::sort(customers.begin(), customers.end(),
              [&](size_t a, size_t b){ return angles[a] < angles[b]; });

    // Rotate so that the gap between last and first is maximum
    {
        double max_angle = 0.0;
        size_t max_idx   = 0;
        size_t n = customers.size();
        for (size_t i = 0; i < n; ++i) {
            double angle = angles[customers[i]] - angles[customers[(i+1) % n]];
            if (angle > max_angle) {
                max_angle = angle;
                max_idx   = i;
            }
        }
        size_t rotate_first = (max_idx + 1) % customers.size();
        std::rotate(customers.begin(),
                    customers.begin() + static_cast<ptrdiff_t>(rotate_first),
                    customers.end());
    }

    double first_angle = angles[customers.front()];
    double last_angle  = angles[customers.back()];
    double gap = (last_angle - first_angle) / static_cast<double>(k);

    for (size_t c : customers) {
        size_t idx = static_cast<size_t>((angles[c] - first_angle) / gap);
        if (idx >= k) idx = k - 1;
        clusters[idx].push_back(c);
    }
    return clusters;
}
