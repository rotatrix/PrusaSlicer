#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "slic3r/GUI/Camera.hpp"

using namespace Slic3r;
using Slic3r::GUI::Camera;

TEST_CASE("External camera views survive projection updates and native presets", "[Camera]")
{
    Camera camera;
    const BoundingBoxf3 scene(Vec3d(-10, -10, 0), Vec3d(10, 10, 20));
    camera.set_scene_box(scene);
    camera.set_viewport(0, 0, 1200, 800);
    const auto type = GENERATE(Camera::EType::Perspective, Camera::EType::Ortho);
    camera.set_type(type);
    const double height = 800.0;
    const double fov = 0.7;
    const double extent = 0.5; // Deliberately exceeds the native mouse zoom limit.
    for (double depth : {200.0, 30.0, 2.0, 0.1}) {
        const Vec3d position(0, 0, 10 + depth);
        const Vec3d pivot(3, 4, 10); // Off-axis pivot, centered at the same depth.
        const Vec3d forward(0, 0, -1);
        const Vec3d target = position + (pivot - position).dot(forward) * forward;
        const double zoom = type == Camera::EType::Perspective ? height / (2 * depth * std::tan(fov / 2)) : height / extent;
        camera.set_external_view(position, target, Vec3d::UnitY(), zoom);
        const auto view = camera.get_view_matrix();
        for (int redraw = 0; redraw < 3; ++redraw) {
            camera.apply_projection(scene);
            REQUIRE(camera.get_view_matrix().matrix().isApprox(view.matrix(), 1e-10));
            const double yy = camera.get_projection_matrix()(1, 1);
            const double actual = type == Camera::EType::Perspective ? 2 * std::atan(1 / yy) : 2 / yy;
            REQUIRE(std::abs(actual - (type == Camera::EType::Perspective ? fov : extent)) < 1e-10);
            REQUIRE(camera.get_near_z() > 0);
            REQUIRE(camera.get_far_z() > camera.get_near_z());
        }
        for (const char *preset : {"iso", "top", "front"}) {
            camera.select_view(preset);
            camera.apply_projection(scene);
            REQUIRE(camera.get_target().isApprox(target, 1e-10));
            REQUIRE(std::abs(camera.get_distance() - depth) < 1e-8);
        }
    }
    camera.zoom_to_box(scene);
    camera.apply_projection(scene);
    REQUIRE(camera.get_near_z() >= Camera::FrustrumMinNearZ);
}
