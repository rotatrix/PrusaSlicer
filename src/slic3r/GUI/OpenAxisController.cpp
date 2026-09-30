#include "OpenAxisController.hpp"
#include "OpenAxisOverlay.hpp"
#include "OpenAxisScheduler.hpp"
#include "3DScene.hpp"
#include "GLCanvas3D.hpp"
#include "GLShader.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"
#include "libslic3r/MultipleBeds.hpp"
#include "libslic3r/libslic3r.h"
#include <imgui/imgui.h>
#include <wx/dialog.h>
#include <wx/glcanvas.h>
#include <sstream>
#include <cmath>
#include <limits>

namespace Slic3r::GUI {
using openaxis::Value;
namespace {
OpenAxisController *s_instance = nullptr;
openaxis::Vec3 vector(const Vec3d &v) { return {v.x(), v.y(), v.z()}; }
Vec3d vector(openaxis::Vec3 v) { return {v.x, v.y, v.z}; }
Value bounds_value(const BoundingBoxf3 &b) {
    if (!b.defined)
        return nullptr;
    return {{"min", {b.min.x(), b.min.y(), b.min.z()}},
            {"max", {b.max.x(), b.max.y(), b.max.z()}}};
}
openaxis::OpenAxisClientOptions options(openaxis::Scheduler *scheduler) {
    openaxis::OpenAxisClientOptions o;
    o.client_name = "PrusaSlicer";
    o.client_version = SLIC3R_VERSION;
    o.scheduler = scheduler;
    o.target = {{"pid", openaxis::current_process_id()}, {"app_version", SLIC3R_VERSION}};
    return o;
}
// Modal dialogs belong to the active application, so activation alone does not
// mean the viewport is receiving interaction.
bool modal_dialog_open() {
    for (auto *node = wxTopLevelWindows.GetFirst(); node; node = node->GetNext())
        if (auto *dialog = dynamic_cast<wxDialog *>(node->GetData()); dialog && dialog->IsShown() && dialog->IsModal())
            return true;
    return false;
}
// Screen-facing disc geometry in logical pixels around the origin.
void init_disc(GLModel &model, float inner, float outer) {
    constexpr unsigned segments = 32;
    GLModel::Geometry data;
    data.format = {GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3};
    for (unsigned k = 0; k < segments; ++k) {
        const float a = float(2 * M_PI * k / segments);
        data.add_vertex(Vec3f(inner * std::cos(a), inner * std::sin(a), 0.f));
        data.add_vertex(Vec3f(outer * std::cos(a), outer * std::sin(a), 0.f));
    }
    if (inner == 0.f)
        for (unsigned k = 0; k < segments; ++k)
            data.add_triangle(2 * k, 2 * k + 1, 2 * ((k + 1) % segments) + 1);
    else
        // An annulus avoids blending a translucent fill over a second black disc.
        for (unsigned k = 0; k < segments; ++k) {
            const unsigned n = (k + 1) % segments;
            data.add_triangle(2 * k, 2 * k + 1, 2 * n + 1);
            data.add_triangle(2 * k, 2 * n + 1, 2 * n);
        }
    model.init_from(std::move(data));
}
} // namespace
OpenAxisController *OpenAxisController::instance() { return s_instance; }
OpenAxisController::OpenAxisController(Camera &camera)
    : m_camera(camera), m_log(openaxis::DiagnosticLog::configure("prusaslicer", {}, SLIC3R_VERSION)),
      m_scheduler(std::make_shared<OpenAxisScheduler>()), m_client(options(m_scheduler.get())),
      m_session(
          m_client, *this, &m_collector,
          [this] {
              openaxis::NavigationOptions o;
              o.scheduler = m_scheduler.get();
              o.observation = [this](const openaxis::NavigationContext &context) {
                  return is_current(context) ? read_camera() : std::nullopt;
              };
              return o;
          }()),
      m_connection(m_client, {[this] {
          openaxis::ConnectionMetadata metadata;
          metadata.tags = {"app.prusaslicer"};
          metadata.capabilities = {"navigation"};
          metadata.focused = m_focused;
          return metadata;
      }}) {
    s_instance = this;
    m_collector.on_changed = [this] {
        if (m_diagnostics_visible)
            redraw();
    };
    m_connection.on_state = [this](const openaxis::ConnectionStatus &) {
        // Lifecycle file logging is handled by the SDK; status is a diagnostic row.
        if (m_diagnostics_visible)
            redraw();
    };
    m_scheduler->before_dispatch = [this] { poll(); };
    m_connection.start();
}
OpenAxisController::~OpenAxisController() {
    m_scheduler->before_dispatch = {};
    m_lifetime.reset();
    m_connection.stop();
    m_session.close();
    m_log->close();
    s_instance = nullptr;
}
void OpenAxisController::redraw() {
    if (!m_canvas)
        return;
    m_canvas->set_as_dirty();
    if (auto *canvas = m_canvas->get_wxglcanvas())
        canvas->Refresh(false);
}
void OpenAxisController::schedule_diagnostic_expiry() {
    if (!m_diagnostics_visible)
        return;
    auto deadline = m_collector.presentation().expires_at;
    if (!deadline || (m_diagnostic_deadline && *m_diagnostic_deadline <= *deadline))
        return;
    m_diagnostic_deadline = deadline;
    std::weak_ptr<int> alive = m_lifetime;
    m_scheduler->post_at(*deadline, [this, alive, deadline] {
        if (alive.expired() || m_diagnostic_deadline != deadline)
            return;
        m_diagnostic_deadline.reset();
        if (m_diagnostics_visible)
            redraw();
    });
}
void OpenAxisController::toggle_diagnostics() {
    m_diagnostics_visible = !m_diagnostics_visible;
    m_collector.set_enabled(m_diagnostics_visible && viewport_available());
    redraw();
}
void OpenAxisController::canvas_destroyed(const GLCanvas3D &canvas) {
    if (&canvas != m_canvas)
        return;
    m_canvas = nullptr;
    m_session.cancel("viewport_closed");
    m_collector.clear();
    m_context.clear();
}
bool OpenAxisController::viewport_available() const {
    return m_canvas && m_canvas->is_initialized() && m_canvas->get_model() && m_width > 0 && m_height > 0;
}
void OpenAxisController::poll() {
    Plater *plater = wxGetApp().plater();
    GLCanvas3D *canvas = plater ? plater->get_current_canvas3D() : nullptr;
    if (canvas != m_canvas) {
        // The canvas is part of the captured context, so old work cannot reach the new view.
        m_canvas = canvas;
        m_session.cancel("viewport_changed");
        m_collector.clear();
    }
    wxGLCanvas *window = canvas ? canvas->get_wxglcanvas() : nullptr;
    bool focused = false;
    m_x = m_y = -1;
    if (window) {
        const Size size = canvas->get_canvas_size();
        // wx coordinates are logical on Retina; the camera viewport is physical.
#if ENABLE_RETINA_GL
        m_scale = size.get_scale_factor();
#else
        m_scale = 1.0;
#endif
        m_width = size.get_width();
        m_height = size.get_height();
        const wxPoint cursor = window->ScreenToClient(wxGetMousePosition());
        // Panel hover makes cursor picking unavailable, not camera navigation.
        // Rotatrix can use its viewport-center fallback.
        if (!ImGui::GetIO().WantCaptureMouse) {
            m_x = int(cursor.x * m_scale);
            m_y = int(cursor.y * m_scale);
        }
        focused = window->IsShownOnScreen() && window->IsEnabled() && wxTheApp->IsActive() && !modal_dialog_open();
    } else
        m_width = m_height = 0;
    const bool native_changed = !m_last_view.matrix().isApprox(m_camera.get_view_matrix().matrix()) ||
        m_last_zoom != m_camera.get_zoom() || m_last_projection != m_camera.get_type();
    m_last_view = m_camera.get_view_matrix();
    m_last_zoom = m_camera.get_zoom();
    m_last_projection = m_camera.get_type();
    const bool available = viewport_available();
    focused = focused && available;
    const std::string context = available ? context_key() : "";
    if (context != m_context || (!available && m_session.active())) {
        m_session.cancel("viewport_changed");
        m_collector.clear();
        m_context = context;
    }
    m_collector.set_enabled(m_diagnostics_visible && available);
    m_collector.set_context(context);
    if (m_focused && !focused)
        m_session.cancel("focus_lost");
    if (m_focused != focused) {
        m_focused = focused;
        m_connection.refresh_metadata();
    }
    if (native_changed && m_focused && !m_writing_camera)
        m_session.native_camera_changed();
}
void OpenAxisController::render_diagnostics(GLCanvas3D &canvas) {
    if (&canvas != m_canvas)
        return;
    render_overlay();
    schedule_diagnostic_expiry();
}
std::string OpenAxisController::context_key() const {
    if (!viewport_available())
        return "unavailable";
    return std::to_string(m_canvas->get_model()->id().id) + "/" +
        std::to_string(m_canvas->openaxis_scene_revision()) + "/" +
        std::to_string(reinterpret_cast<std::uintptr_t>(m_canvas)) + "/" +
        std::to_string(s_multiple_beds.get_active_bed()) + "/" +
        std::to_string(m_width) + "/" + std::to_string(m_height) + "/" + std::to_string(m_scale);
}
// Queries run synchronously on the UI thread. Pin the view identity and initial
// camera; resolve expensive scene facts only when requested by the server.
struct OpenAxisController::QueryCapture final : openaxis::NavigationCapture {
    OpenAxisController &owner;
    openaxis::NavigationContext context;
    std::optional<openaxis::Pose> initial;
    QueryCapture(OpenAxisController &controller, const openaxis::NavigationContext &captured)
        : owner(controller), context(captured), initial(owner.read_camera()) {}
    std::optional<openaxis::Pose> initial_observation() override { return initial; }
    Value resolve(const std::string &name) override {
        if (!owner.is_current(context)) return nullptr;
        if (name == "camera.pose") return initial ? openaxis::pose_value(*initial) : Value{};
        return owner.fact(name);
    }
};
openaxis::NavigationContext OpenAxisController::capture_context() {
    if (!m_focused || !viewport_available()) return {};
    return context_key();
}
bool OpenAxisController::is_current(const openaxis::NavigationContext &context) {
    const auto *key = std::any_cast<std::string>(&context);
    return key && m_focused && viewport_available() && *key == context_key();
}
std::unique_ptr<openaxis::NavigationCapture>
OpenAxisController::begin_query(const openaxis::NavigationContext &context) {
    if (!is_current(context)) return {};
    return std::make_unique<QueryCapture>(*this, context);
}
openaxis::WriteResult OpenAxisController::apply_pose(
    const openaxis::NavigationContext &context, const openaxis::NavigationPose &pose,
    const Value &, std::optional<openaxis::Vec3>) {
    if (!is_current(context) || !write_camera(pose)) return {};
    return {true, read_camera()};
}
void OpenAxisController::show_pivot(const openaxis::NavigationContext &context,
                                    std::optional<openaxis::Vec3> point) {
    if (!point || is_current(context)) pivot(point);
}
std::optional<openaxis::Pose> OpenAxisController::read_camera() {
    if (!m_focused || !viewport_available())
        return {};
    const auto &c = m_camera;
    if (c.get_viewport()[2] <= 0 || c.get_viewport()[3] <= 0)
        return {};
    auto q = openaxis::Quat::from_basis(vector(c.get_dir_right()), vector(c.get_dir_up()), vector(-c.get_dir_forward()));
    openaxis::Pose p{vector(c.get_position()), q.rotvec()};
    double yy = c.get_projection_matrix()(1, 1);
    if (!std::isfinite(yy) || yy <= 0)
        return {};
    if (c.get_type() == Camera::EType::Perspective)
        p.fov = 2 * std::atan(1 / yy);
    else
        p.ortho_extent = 2 / yy;
    return p;
}
bool OpenAxisController::write_camera(const openaxis::Pose &p) {
    if (!m_focused || !viewport_available())
        return false;
    // Native change polling also sees adapter writes. Their realized result is
    // already returned to the SDK; do not emit a duplicate input notification.
    struct Writing {
        bool &flag;
        Writing(bool &f) : flag(f) { flag = true; }
        ~Writing() { flag = false; }
    } writing(m_writing_camera);
    auto &c = m_camera;
    auto q = openaxis::Quat::from_rotvec(p.r);
    const Vec3d position = vector(p.t), forward = vector(q.rotate({0, 0, -1}));
    // Keep the native orbit radius so mouse orbit continues around the camera's own target.
    const double distance = std::max(1.0, c.get_distance());
    c.look_at(position, position + distance * forward, vector(q.rotate({0, 1, 0})));
    // The projection is the user's preference, persisted by Camera::set_type; a pose
    // never switches it. A mismatched pose keeps the zoom and the readback lets the
    // SDK rebase the server onto the native projection.
    const double height = c.get_viewport()[3];
    if (c.get_type() == Camera::EType::Perspective && p.fov > 0)
        c.set_zoom(height / (2 * distance * std::tan(p.fov / 2)));
    else if (c.get_type() == Camera::EType::Ortho && p.ortho_extent > 0)
        c.set_zoom(height / p.ortho_extent);
    m_canvas->update_openaxis_projection();
    m_last_view = c.get_view_matrix();
    m_last_zoom = c.get_zoom();
    m_last_projection = c.get_type();
    redraw();
    return true;
}
void OpenAxisController::pivot(std::optional<openaxis::Vec3> p) {
    m_pivot = p;
    redraw();
}
Value OpenAxisController::fact(const std::string &name) {
    if (!m_focused || !viewport_available()) return nullptr;
    const auto &c = m_camera;
    if (name == "document.id")
        return wxGetApp().is_gcode_viewer() ? Value{} : Value(std::to_string(m_canvas->get_model()->id().id));
    if (name == "world.orientation") return {{"forward", {0, 1, 0}}, {"up", {0, 0, 1}}, {"handedness", "right"}};
    if (name == "camera.view_target") return openaxis::vector_value(vector(c.get_target()));
    if (name == "viewport.aspect") return double(m_width) / m_height;
    const bool inside = m_x >= 0 && m_x < m_width && m_y >= 0 && m_y < m_height;
    if (name == "viewport.cursor" && inside)
        return {{"x", 2. * m_x / m_width - 1}, {"y", 1 - 2. * m_y / m_height}};
    const auto &volumes = m_canvas->get_volumes().volumes;
    const auto &selected = m_canvas->get_selection().get_volume_idxs();
    if (name == "model.bounds") {
        BoundingBoxf3 bounds;
        for (const auto *volume : volumes)
            if (volume->is_active && !volume->disabled)
                bounds.merge(volume->transformed_bounding_box());
        if (m_canvas->openaxis_is_preview())
            bounds.merge(m_canvas->openaxis_gcode_bounds());
        return bounds_value(bounds);
    }
    if (name == "selection.bounds") {
        // Preview has no editable selection; object selection is the only selection.
        if (m_canvas->openaxis_is_preview())
            return nullptr;
        BoundingBoxf3 bounds;
        for (unsigned i = 0; i < volumes.size(); ++i)
            if (volumes[i]->is_active && !volumes[i]->disabled && selected.count(i))
                bounds.merge(volumes[i]->transformed_bounding_box());
        return bounds_value(bounds);
    }
    if (name.rfind("pick.", 0) == 0)
        return pick(name, inside);
    return nullptr;
}
Value OpenAxisController::pick(const std::string &name, bool inside) {
    const bool center = name == "pick.viewport_center" || name == "pick.viewport_center.selection";
    const bool cursor = name == "pick.cursor" || name == "pick.cursor.selection";
    if (!(center || (cursor && inside)))
        return nullptr;
    const bool only = name.find(".selection") != std::string::npos;
    const auto &volumes = m_canvas->get_volumes().volumes;
    const auto &selected = m_canvas->get_selection().get_volume_idxs();
    // With no selection the test is skipped, not missed: the server tries its next candidate.
    if (only && (selected.empty() || m_canvas->openaxis_is_preview()))
        return nullptr;
    const auto &c = m_camera;
    const Vec2d pixel(center ? m_width * .5 : m_x, center ? m_height * .5 : m_y);
    const Value marker = {pixel.x() / m_scale, pixel.y() / m_scale};
    Value result = {{"markerPosition", marker}};
    double closest = std::numeric_limits<double>::max();
    auto accept = [&](const Vec3d &world, const GLVolume *volume) {
        const double distance = (world - c.get_position()).squaredNorm();
        if (distance >= closest) return;
        closest = distance;
        result = {{"point", openaxis::vector_value(vector(world))}, {"markerPosition", marker}};
        if (volume) result["bounds"] = bounds_value(volume->transformed_bounding_box());
    };
    auto test = [&](const SceneRaycasterItem &item, const Transform3d &transform, const GLVolume *volume,
                    const ClippingPlane *clipping) {
        Vec3f point, normal;
        if (!item.is_active() || !item.get_raycaster()->closest_hit(pixel, transform, c, point, normal, clipping)) return;
        const Vec3d world = transform * point.cast<double>();
        const Vec3d world_normal = (transform.linear().inverse().transpose() * normal.cast<double>()).normalized();
        if (!item.use_back_faces() && world_normal.dot(c.get_dir_forward()) >= 0) return;
        accept(world, volume);
    };
    // Match native hover picking: gizmo clipping applies to volumes only.
    const ClippingPlane clipping = m_canvas->get_gizmos_manager().get_clipping_plane().inverted_normal();
    for (const auto &item : *m_canvas->get_raycasters_for_picking(SceneRaycaster::EType::Volume)) {
        const int id = SceneRaycaster::decode_id(SceneRaycaster::EType::Volume, item->get_id());
        if (id >= 0 && size_t(id) < volumes.size() && !volumes[id]->disabled && (!only || selected.count(id)))
            test(*item, item->get_transform(), volumes[id], &clipping);
    }
    // Beds are never selection candidates; they participate only in ordinary picks.
    if (!only && c.is_looking_downward())
        for (const auto &item : *m_canvas->get_raycasters_for_picking(SceneRaycaster::EType::Bed))
            for (int bed = 0; bed < s_multiple_beds.get_number_of_beds(); ++bed) {
                Transform3d transform = item->get_transform();
                transform.translate(s_multiple_beds.get_bed_translation(bed));
                test(*item, transform, nullptr, nullptr);
            }
    // Toolpaths have no raycasters; use their rendered depth at the same sample.
    if (!only && m_canvas->openaxis_is_preview())
        if (const auto hit = m_canvas->openaxis_gcode_hit(pixel))
            accept(*hit, nullptr);
    return result;
}
void OpenAxisController::render_indicator(GLCanvas3D &canvas) {
    if (&canvas != m_canvas || !m_pivot || !viewport_available() || context_key() != m_context) return;
    const Vec4d clip = m_camera.get_projection_matrix().matrix() * m_camera.get_view_matrix().matrix() *
        Vec4d(m_pivot->x, m_pivot->y, m_pivot->z, 1);
    if (!OpenAxisOverlay::visible({clip.x(), clip.y(), clip.z(), clip.w()})) return;
    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (!shader) return;
    if (!m_pivot_fill.is_initialized()) init_disc(m_pivot_fill, 0.f, 4.f);
    if (!m_pivot_rim.is_initialized()) init_disc(m_pivot_rim, 4.f, 5.5f);
    // Place logical-pixel geometry at the pivot's NDC position and depth, applying DPI once.
    const auto &viewport = m_camera.get_viewport();
    Transform3d transform = Transform3d::Identity();
    transform.translate(clip.head<3>() / clip.w());
    transform.scale(Vec3d(2 * m_scale / viewport[2], 2 * m_scale / viewport[3], 1));
    GLboolean depth_test, depth_write, blend, cull;
    GLint depth_func, blend_src, blend_dst;
    glsafe(::glGetBooleanv(GL_DEPTH_TEST, &depth_test));
    glsafe(::glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_write));
    glsafe(::glGetBooleanv(GL_BLEND, &blend));
    glsafe(::glGetBooleanv(GL_CULL_FACE, &cull));
    glsafe(::glGetIntegerv(GL_DEPTH_FUNC, &depth_func));
    glsafe(::glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src));
    glsafe(::glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst));
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDepthMask(GL_FALSE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    glsafe(::glDisable(GL_CULL_FACE));
    shader->start_using();
    shader->set_uniform("view_model_matrix", transform);
    shader->set_uniform("projection_matrix", Transform3d::Identity());
    // Per-fragment depth: visible fragments opaque, occluded fragments faint.
    for (const bool occluded : {true, false}) {
        glsafe(::glDepthFunc(occluded ? GL_GREATER : GL_LEQUAL));
        const float alpha = occluded ? .25f : 1.f;
        m_pivot_fill.set_color(ColorRGBA(0.f, 1.f, 0.f, alpha));
        m_pivot_fill.render();
        m_pivot_rim.set_color(ColorRGBA(0.f, 0.f, 0.f, alpha));
        m_pivot_rim.render();
    }
    shader->stop_using();
    glsafe(::glDepthFunc(depth_func));
    glsafe(::glDepthMask(depth_write));
    glsafe(::glBlendFunc(blend_src, blend_dst));
    if (!blend) glsafe(::glDisable(GL_BLEND));
    if (!depth_test) glsafe(::glDisable(GL_DEPTH_TEST));
    if (cull) glsafe(::glEnable(GL_CULL_FACE));
}
void OpenAxisController::render_overlay() {
    if (!m_diagnostics_visible || !viewport_available())
        return;
    const auto &camera = m_camera;
    const auto &v = camera.get_viewport();
    OpenAxisOverlay::Viewport viewport{double(v[0]),
                                       double(v[1]),
                                       double(v[2]),
                                       double(v[3]),
                                       double(m_height),
                                       m_scale};
    auto pixel = [](const std::array<double, 2> &p) { return ImVec2(float(p[0]), float(p[1])); };
    auto clip = [&](openaxis::Vec3 p) {
        const Vec4d q =
            camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix() * Vec4d(p.x, p.y, p.z, 1);
        return OpenAxisOverlay::Clip{q.x(), q.y(), q.z(), q.w()};
    };
    auto color = [](const std::string &tone, double opacity = 1) {
        const auto &c = openaxis::diagnostic_colors().at(tone);
        return IM_COL32(c[0], c[1], c[2], int(255 * opacity));
    };
    // Connection status is part of the diagnostic presentation; there is no separate panel.
    const auto status = m_connection.status();
    std::vector<openaxis::DiagnosticLine> lines{{"OpenAxis: " + status.state, status.state == "ready" ? "pass" : "text"}};
    if (status.state == "retrying") {
        const double wait = std::max(0., status.retry_at.value_or(openaxis::diagnostic_time()) - openaxis::diagnostic_time());
        lines[0] = {"OpenAxis: retrying in " + std::to_string(int(std::ceil(wait))) + " s" +
                        (status.error.empty() ? "" : " (" + status.error + ")"), "missing"};
    }
    auto frame = m_collector.presentation();
    const bool evidence = frame.context == context_key();
    if (evidence)
        lines.insert(lines.end(), frame.lines.begin(), frame.lines.end());
    auto *draw = ImGui::GetBackgroundDrawList();
    auto lo = pixel(viewport.pixel(v[0], viewport.top())),
         hi = pixel(viewport.pixel(v[0] + v[2], viewport.top() + v[3]));
    draw->PushClipRect(lo, hi, true);
    if (evidence) {
        for (const auto &segment : frame.segments) {
            auto a = clip(segment.start), b = clip(segment.end);
            if (OpenAxisOverlay::clip_segment(a, b))
                draw->AddLine(pixel(viewport.project(a)), pixel(viewport.project(b)),
                              color(segment.tone, segment.opacity), float(segment.width));
        }
        for (const auto &marker : frame.markers) {
            auto p = pixel(marker.point);
            auto c = color(marker.tone, .65);
            draw->AddLine({p.x - 9, p.y}, {p.x + 9, p.y}, IM_COL32_BLACK, 4);
            draw->AddLine({p.x, p.y - 9}, {p.x, p.y + 9}, IM_COL32_BLACK, 4);
            draw->AddLine({p.x - 9, p.y}, {p.x + 9, p.y}, c, 2);
            draw->AddLine({p.x, p.y - 9}, {p.x, p.y + 9}, c, 2);
            std::istringstream labels(marker.label);
            std::string label;
            float y = p.y - 8;
            while (std::getline(labels, label)) {
                draw->AddText({p.x + 13, y + 1}, IM_COL32_BLACK, label.c_str());
                draw->AddText({p.x + 12, y}, c, label.c_str());
                y += 15;
            }
        }
    }
    // Leave room for the 80-logical-pixel viewcube and its surrounding margin.
    const float text_x = lo.x + 120;
    const float line_height = ImGui::GetTextLineHeightWithSpacing();
    float y = std::max(lo.y + 24, hi.y - 24 - float(lines.size()) * line_height);
    for (const auto &line : lines) {
        draw->AddText({text_x + 1, y + 1}, IM_COL32_BLACK, line.text.c_str());
        draw->AddText({text_x, y}, color(line.tone), line.text.c_str());
        y += line_height;
    }
    draw->PopClipRect();
}
} // namespace Slic3r::GUI
