#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace Slic3r::GUI::OpenAxisOverlay {
using Clip = std::array<double, 4>;
struct Viewport {
    double x, y, width, height, framebuffer_height, scale;
    bool valid() const { return width > 0 && height > 0 && scale > 0; }
    double top() const { return framebuffer_height - y - height; }
    std::array<double, 2> pixel(double px, double py) const { return {px / scale, py / scale}; }
    std::array<double, 2> project(const Clip &p) const {
        return pixel(x + (p[0] / p[3] + 1) * width / 2, top() + (1 - p[1] / p[3]) * height / 2);
    }
};
inline bool finite(const Clip &p) {
    return std::all_of(p.begin(), p.end(), [](double v) { return std::isfinite(v); });
}
inline bool visible(const Clip &p) {
    return finite(p) && p[3] > 1e-12 && std::abs(p[0]) <= p[3] && std::abs(p[1]) <= p[3] &&
           std::abs(p[2]) <= p[3];
}
// Clip before perspective division, including segments crossing the eye plane.
inline bool clip_segment(Clip &a, Clip &b) {
    if (!finite(a) || !finite(b))
        return false;
    double low = 0, high = 1;
    for (int axis = 0; axis < 3; ++axis)
        for (double sign : {-1., 1.}) {
            double pa = a[3] + sign * a[axis], pb = b[3] + sign * b[axis];
            if (pa < 0 && pb < 0)
                return false;
            if (pa < 0)
                low = std::max(low, pa / (pa - pb));
            if (pb < 0)
                high = std::min(high, pa / (pa - pb));
        }
    if (low > high)
        return false;
    auto original = a;
    for (int i = 0; i < 4; ++i) {
        double d = b[i] - original[i];
        a[i] = original[i] + low * d;
        b[i] = original[i] + high * d;
    }
    return a[3] > 1e-12 && b[3] > 1e-12;
}
} // namespace Slic3r::GUI::OpenAxisOverlay
