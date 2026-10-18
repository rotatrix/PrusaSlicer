// Copied from oss-forks/shared/prusaslicer-family/OpenAxisCameraMath.hpp
// Keep copies aligned; see that directory's README.md.
#pragma once
#include <algorithm>
#include <cmath>
#include <optional>

namespace Slic3r::GUI::OpenAxisCameraMath {
struct ViewScale { double distance, zoom; };
// pivot_depth is (pivot - eye).dot(forward), not Euclidean pivot distance.
// Native projection is a user preference; mismatched poses preserve its scale.
// Callers retain ownership of native camera setters and their limits.
inline ViewScale view_scale(double old_distance, double old_zoom, double height,
                            bool perspective, std::optional<double> pivot_depth,
                            double fov, double ortho_extent) {
    double distance = std::max(1e-6, old_distance);
    if (pivot_depth && std::isfinite(*pivot_depth) && *pivot_depth > 1e-6)
        distance = *pivot_depth;
    double zoom = old_zoom * (perspective ? old_distance / distance : 1.0);
    if (perspective && fov > 0)
        zoom = height / (2 * distance * std::tan(fov / 2));
    else if (!perspective && ortho_extent > 0)
        zoom = height / ortho_extent;
    return {distance, zoom};
}
} // namespace Slic3r::GUI::OpenAxisCameraMath
