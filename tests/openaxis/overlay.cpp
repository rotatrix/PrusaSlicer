#include "Slic3r/App/Plater/OpenAxisOverlay.hpp"
#include "Slic3r/App/Plater/OpenAxisPicking.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace Slic3r::App::Plater::OpenAxisOverlay;
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
int main() try {
    using Slic3r::App::Plater::openaxis_bed_pickable;
    using Slic3r::App::Scene::BedElementType;
    using Slic3r::App::Scene::BedNodeTag;
    for (auto type : {BedElementType::PlateDefault, BedElementType::PlateTextured, BedElementType::Model}) {
        BedNodeTag bed{1, 2, type, false};
        check(openaxis_bed_pickable(bed, true, true), "selected bed must participate in selection picks");
        check(!openaxis_bed_pickable(bed, true, false), "unselected bed excluded from selection picks");
        check(openaxis_bed_pickable(bed, false, false), "ordinary picks include unselected beds");
        check(!openaxis_bed_pickable({1, 2, type, true}, false, true), "virtual bed preview excluded");
    }
    for (auto type : {BedElementType::Axis, BedElementType::Label, BedElementType::Grid,
                      BedElementType::Contour, BedElementType::PrintVolume, BedElementType::SelectionOutline})
        check(!openaxis_bed_pickable({1, 2, type, false}, false, true), "bed decoration excluded");
    // Offset viewport in a 2x framebuffer: scale positions once, preserve logical sizes.
    Viewport v{200, 100, 800, 600, 1000, 2};
    check(v.valid(), "viewport validity");
    check(v.project({0, 0, 0, 1}) == std::array<double, 2>{300, 300}, "offset viewport center");
    check(v.project({-1, 1, 0, 1}) == std::array<double, 2>{100, 150}, "top-left pixel origin");
    check(v.project({1, -1, 0, 1}) == std::array<double, 2>{500, 450}, "bottom-right pixel origin");
    check(v.project({1, -1, 0, 2}) == std::array<double, 2>{400, 375},
          "perspective division before viewport mapping");
    check(!visible({0, 0, 0, -1}), "behind-eye marker");
    check(!visible({0, 0, 2, 1}), "far-clipped marker");
    check(!visible({0, 0, -2, 1}), "near-clipped marker");
    check(!visible({2, 0, 0, 1}), "offscreen marker");
    check(!visible({std::numeric_limits<double>::quiet_NaN(), 0, 0, 1}), "nonfinite marker");
    Clip a{-2, 0, 0, 1}, b{2, 0, 0, 1};
    check(clip_segment(a, b) && a[0] == -1 && b[0] == 1, "side-plane segment clipping");
    a = {0, 0, -2, 1};
    b = {0, 0, 0, 1};
    check(clip_segment(a, b) && a[2] == -1, "near-plane crossing");
    a = {0, 0, 2, 1};
    b = {0, 0, 3, 1};
    check(!clip_segment(a, b), "fully clipped segment");
    a = {0, 0, -2, -1};
    b = {0, 0, 0, 1};
    check(clip_segment(a, b) && a[3] > 0 && std::isfinite(v.project(a)[0]), "eye-plane crossing");
    v = {0, 0, 600, 1200, 1200, 1.5};
    check(v.project({0, 0, 0, 1}) == std::array<double, 2>{200, 400}, "portrait fractional DPI");
    std::cout << "Overlay clipping, offset viewports, portrait and DPI checks passed\n";
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
