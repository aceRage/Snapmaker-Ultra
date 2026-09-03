#include "ObjColorMatch.hpp"
#include "ObjColorUtils.hpp"      // QuantKMeans - the very clustering ObjColorPanel::deal_algo uses

#include <algorithm>
#include <cmath>
#include <numeric>

namespace Slic3r {

namespace {

double pivot_rgb(double n)   // sRGB companding, n in 0..1
{
    return (n > 0.04045 ? std::pow((n + 0.055) / 1.055, 2.4) : n / 12.92) * 100.0;
}

double pivot_xyz(double n) { return n > 0.008856 ? std::cbrt(n) : 7.787 * n + 16.0 / 116.0; }

void rgb_to_lab(const RGBA &c, double lab[3])
{
    const double R = pivot_rgb(std::min(std::max((double) c[0], 0.0), 1.0));
    const double G = pivot_rgb(std::min(std::max((double) c[1], 0.0), 1.0));
    const double B = pivot_rgb(std::min(std::max((double) c[2], 0.0), 1.0));
    const double X = 0.412453 * R + 0.357580 * G + 0.180423 * B;
    const double Y = 0.212671 * R + 0.715160 * G + 0.072169 * B;
    const double Z = 0.019334 * R + 0.119193 * G + 0.950227 * B;
    const double x = pivot_xyz(X / 95.047);
    const double y = pivot_xyz(Y / 100.000);
    const double z = pivot_xyz(Z / 108.883);
    lab[0] = 116.0 * y - 16.0;
    lab[1] = 500.0 * (x - y);
    lab[2] = 200.0 * (y - z);
}

} // namespace

float obj_color_distance(const RGBA &a, const RGBA &b)
{
    double la[3], lb[3];
    rgb_to_lab(a, la);
    rgb_to_lab(b, lb);
    return (float) std::sqrt((la[0] - lb[0]) * (la[0] - lb[0]) + (la[1] - lb[1]) * (la[1] - lb[1]) +
                             (la[2] - lb[2]) * (la[2] - lb[2]));
}

bool obj_color_auto_match(const std::vector<RGBA> &input_colors,
                          bool                     is_single_color,
                          const std::vector<RGBA> &existing_colors,
                          ObjColorMatchResult &    out,
                          size_t                   max_slots,
                          float                    tolerance,
                          ObjColorDistanceFn       distance)
{
    out = ObjColorMatchResult();
    if (input_colors.empty() || max_slots == 0)
        return false;
    if (!distance)
        distance = &obj_color_distance;
    out.input = input_colors.size();

    // 1. Cluster, exactly as ObjColorPanel's constructor does: a single-colour import is one
    //    cluster by definition, everything else goes through the k-means with an automatic count.
    std::vector<RGBA> cluster_colors;
    std::vector<int>  labels;
    if (is_single_color) {
        cluster_colors.push_back(input_colors.front());
        labels.assign(input_colors.size(), 0);
    } else {
        QuantKMeans quant(10);
        quant.apply(input_colors, cluster_colors, labels, -1);
    }
    if (cluster_colors.empty() || labels.size() != input_colors.size())
        return false;
    for (int l : labels)
        if (l < 0 || l >= (int) cluster_colors.size())
            return false;
    out.clusters = cluster_colors.size();

    // 2. Give the busiest clusters first claim on a slot of their own; whatever is left over when
    //    the 16 run out is merged into its nearest neighbour, where it does the least harm.
    std::vector<size_t> usage(cluster_colors.size(), 0);
    for (int l : labels)
        ++usage[(size_t) l];
    std::vector<size_t> order(cluster_colors.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::stable_sort(order.begin(), order.end(),
                     [&usage](size_t a, size_t b) { return usage[a] > usage[b]; });

    std::vector<RGBA>          slots(existing_colors.begin(), existing_colors.end());
    std::vector<unsigned char> slot_of_cluster(cluster_colors.size(), 1);

    auto nearest = [&](const RGBA &c, float &best) {
        int best_i = -1;
        best       = 0.f;
        for (size_t i = 0; i < slots.size(); ++i) {
            const float d = distance(c, slots[i]);
            if (best_i < 0 || d < best) {
                best_i = (int) i;
                best   = d;
            }
        }
        return best_i;
    };

    for (size_t c : order) {
        const RGBA &colour = cluster_colors[c];
        float       best   = 0.f;
        const int   near_i = nearest(colour, best);
        if (near_i >= 0 && best <= tolerance) {
            slot_of_cluster[c] = (unsigned char) (near_i + 1);   // the spool is already loaded
            ++out.reused;
        } else if (slots.size() < max_slots) {
            slots.push_back(colour);
            out.added_colors.push_back(colour);
            slot_of_cluster[c] = (unsigned char) slots.size();
            ++out.added;
        } else if (near_i >= 0) {
            slot_of_cluster[c] = (unsigned char) (near_i + 1);   // no room left; nearest it is
            ++out.merged;
        }
    }

    // 3. One id per input colour, and the first cluster's slot as the object's extruder - the same
    //    two lines ObjColorPanel::update_filament_ids ends with.
    out.filament_ids.resize(input_colors.size());
    for (size_t i = 0; i < input_colors.size(); ++i)
        out.filament_ids[i] = slot_of_cluster[(size_t) labels[i]];
    out.first_extruder_id = slot_of_cluster[0];
    return true;
}

} // namespace Slic3r
