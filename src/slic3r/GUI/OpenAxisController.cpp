#include "OpenAxisController.hpp"
#include "OpenAxisOverlay.hpp"
#include "GLCanvas3D.hpp"
#include "MultipleBeds.hpp"
#include <imgui/imgui.h>
#include <openaxis/logging.hpp>
#include <sstream>
#include <cmath>
#include <limits>
#include <iomanip>
#include <boost/log/trivial.hpp>

namespace Slic3r::GUI {
using openaxis::Value;
namespace {
openaxis::Vec3 vector(const Vec3d &v) { return {v.x(), v.y(), v.z()}; }
Vec3d vector(openaxis::Vec3 v) { return {v.x, v.y, v.z}; }
Value bounds_value(const BoundingBoxf3 &b) {
    if (!b.defined)
        return nullptr;
    return {{"min", {b.min.x(), b.min.y(), b.min.z()}},
            {"max", {b.max.x(), b.max.y(), b.max.z()}}};
}
openaxis::OpenAxisClientOptions options(openaxis::Scheduler *scheduler) {
    openaxis::DiagnosticLog::configure("prusaslicer");
    openaxis::OpenAxisClientOptions o;
    o.client_name = "PrusaSlicer";
    o.scheduler = scheduler;
    o.target = {{"pid", openaxis::current_process_id()}};
    return o;
}
} // namespace
OpenAxisController::OpenAxisController(GLCanvas3D &canvas, Camera &camera,
                                       std::shared_ptr<openaxis::Scheduler> scheduler,
                                       std::function<void()> redraw)
    : m_canvas(canvas), m_camera(camera), m_redraw(std::move(redraw)),
      m_scheduler(std::move(scheduler)), m_client(options(m_scheduler.get())), m_collector([this] {
          openaxis::DiagnosticOptions options;
          options.debug = true;
          options.log = [this](const std::string &level, const std::string &message) {
              if (level != "debug" || m_diagnostics_writes)
                  record_diagnostic(level + ": " + message, level);
          };
          return options;
      }()),
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
          metadata.tags = {"app.prusaslicer", "workspace.modeling"};
          metadata.capabilities = {"navigation"};
          metadata.focused = m_focused;
          return metadata;
      }}) {
    m_collector.on_changed = [this] {
        if (m_overlay_visible || m_diagnostics_visible)
            m_redraw();
    };
    m_client.on_connection = [this](bool connected) {
        record_diagnostic(connected ? "transport: connected" : "transport: disconnected");
    };
    m_client.on_error = [this](const std::string &e) {
        record_diagnostic("transport error: " + e, "debug");
        BOOST_LOG_TRIVIAL(debug) << "OpenAxis: " << e;
    };
    m_connection.on_state = [this](const openaxis::ConnectionStatus &status) {
        // Lifecycle file logging is handled by the SDK.
        if (m_diagnostics_visible)
            m_redraw();
    };
    m_session.diagnostics = [this](const openaxis::Diagnostic &d) noexcept {
        if (d.event == "write" && d.target == "camera")
            ++m_camera_writes;
    };
    record_diagnostic("Connecting to ws://127.0.0.1:6607");
    m_connection.start();
}
OpenAxisController::~OpenAxisController() {
    m_lifetime.reset();
    m_connection.stop();
    m_session.close();
}
void OpenAxisController::schedule_diagnostic_expiry() {
    if (!m_overlay_visible && !m_diagnostics_visible)
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
        if (m_overlay_visible || m_diagnostics_visible)
            m_redraw();
    });
}
void OpenAxisController::deactivate() {
    m_session.cancel("viewport_inactive");
    m_focused = false;
    m_connection.refresh_metadata();
    m_connection.stop();
    m_collector.clear();
    m_context.clear();
    m_redraw();
}
bool OpenAxisController::viewport_available() const {
    return m_canvas.is_initialized() && m_canvas.get_model() && m_width > 0 && m_height > 0;
}
std::string OpenAxisController::connection_status() const {
    const auto status = m_connection.status();
    if (status.state == "retrying")
        return "Retrying: " + status.error;
    return status.state;
}
void OpenAxisController::refresh(bool focused, int x, int y, int width, int height, double scale) {
    m_width = width; m_height = height; m_scale = scale;
    const bool native_changed = !m_last_view.matrix().isApprox(m_camera.get_view_matrix().matrix()) || m_last_zoom != m_camera.get_zoom();
    m_last_view = m_camera.get_view_matrix();
    m_last_zoom = m_camera.get_zoom();
    if (!m_enabled)
        return;
    const bool available = viewport_available();
    focused = focused && available;
    const std::string context = available ? context_key() : "";
    if (context != m_context || (!available && m_session.active())) {
        m_session.cancel("viewport_changed");
        m_collector.clear();
        m_context = context;
    }
    m_collector.set_enabled(m_overlay_visible && available);
    m_collector.set_context(context);
    // Panel hover makes cursor picking unavailable, not camera navigation.
    // Rotatrix can use its viewport-center fallback; panel keyboard focus
    // does not interfere with a cursor over the scene.
    m_x = ImGui::GetIO().WantCaptureMouse ? -1 : x;
    m_y = ImGui::GetIO().WantCaptureMouse ? -1 : y;
    if (m_focused && !focused)
        m_session.cancel("focus_lost");
    if (m_focused != focused) {
        m_focused = focused;
        m_connection.refresh_metadata();
    }
    if (native_changed && m_focused && !m_writing_camera)
        m_session.native_camera_changed();
    m_connection.start();
}
void OpenAxisController::record_diagnostic(std::string text, const std::string &level) noexcept {
    openaxis::DiagnosticLog::emit(level, text);
    if (m_diagnostics_paused)
        return;
    try {
        constexpr std::size_t max_entries = 250, max_detail = 4096;
        if (text.size() > max_detail)
            text = text.substr(0, max_detail) + " [truncated]";
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - m_log_start).count();
        std::ostringstream entry;
        entry << std::fixed << std::setprecision(3) << seconds << "s  " << text;
        text = entry.str();
        m_diagnostic_log.push_back(std::move(text));
        if (m_diagnostic_log.size() > max_entries)
            m_diagnostic_log.pop_front();
        m_diagnostics_dirty = true;
    } catch (...) {
        // Logging must remain passive, including under memory pressure.
    }
}
void OpenAxisController::render_diagnostics(bool &visible) {
    m_diagnostics_visible = visible;
    render_overlay();
    schedule_diagnostic_expiry();
    if (!visible)
        return;
    ImGui::SetNextWindowSize(ImVec2(680, 420), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("OpenAxis Diagnostics", &visible)) {
        if (ImGui::Checkbox("Enable OpenAxis navigation", &m_enabled)) {
            if (!m_enabled)
                deactivate();
            m_redraw();
        }
        ImGui::Text("Rotatrix: %s | Navigation focus: %s | Gesture: %s",
                    connection_status().c_str(), m_focused ? "Yes" : "No",
                    m_session.active() ? "Active" : "Idle");
        ImGui::Text("Endpoint: ws://127.0.0.1:6607 | Camera writes: %llu",
                    static_cast<unsigned long long>(m_camera_writes));
        ImGui::TextUnformatted(
            "SDK query evidence, writes, corrections and transport errors. Latest 250 entries.");
        if (ImGui::Checkbox("Viewport diagnostics", &m_overlay_visible)) {
            m_collector.set_enabled(m_overlay_visible && viewport_available());
            m_redraw();
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("Pivot remains visible independently.");
        ImGui::Checkbox("Pause logging", &m_diagnostics_paused);
        ImGui::SameLine();
        ImGui::Checkbox("Include camera writes", &m_diagnostics_writes);
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &m_diagnostics_autoscroll);
        if (ImGui::Button("Copy log")) {
            std::string report =
                "PrusaSlicer OpenAxis diagnostics\nEndpoint: ws://127.0.0.1:6607\n";
            report += connection_status() + "\n";
            for (const auto &line : m_diagnostic_log)
                report += line + "\n";
            ImGui::SetClipboardText(report.c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear log")) {
            m_diagnostic_log.clear();
            m_diagnostics_dirty = true;
        }
        ImGui::Separator();
        ImGui::BeginChild("OpenAxis event log", ImVec2(0, 0), false,
                          ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto &line : m_diagnostic_log)
            ImGui::TextUnformatted(line.c_str());
        if (m_diagnostics_autoscroll && m_diagnostics_dirty)
            ImGui::SetScrollHereY(1.0f);
        m_diagnostics_dirty = false;
        ImGui::EndChild();
    }
    ImGui::End();
    m_diagnostics_visible = visible;
}
std::string OpenAxisController::context_key() const {
    if (!viewport_available()) return "unavailable";
    return std::to_string(m_canvas.get_model()->id().id) + "/" +
        std::to_string(m_canvas.openaxis_scene_revision()) + "/" +
        std::to_string(reinterpret_cast<std::uintptr_t>(&m_canvas)) + "/" +
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
    if (!m_enabled || !m_focused || !viewport_available()) return {};
    return context_key();
}
bool OpenAxisController::is_current(const openaxis::NavigationContext &context) {
    const auto *key = std::any_cast<std::string>(&context);
    return key && m_enabled && m_focused && viewport_available() && *key == context_key();
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
    // Native camera listeners also see adapter writes. Their realized result is
    // already returned to the SDK; do not emit a duplicate input notification.
    struct Writing {
        bool &flag;
        Writing(bool &f) : flag(f) { flag = true; }
        ~Writing() { flag = false; }
    } writing(m_writing_camera);
    auto &c = m_camera;
    c.set_type(p.fov > 0 ? Camera::EType::Perspective : Camera::EType::Ortho);
    auto q = openaxis::Quat::from_rotvec(p.r);
    const Vec3d position = vector(p.t), forward = vector(q.rotate({0, 0, -1}));
    // Preserve the native orbit radius while updating its quaternion and target.
    // Native mouse input then continues from exactly the realized SDK pose.
    const double distance = std::max(1.0, m_pivot ? (position - vector(*m_pivot)).norm() : c.get_distance());
    c.look_at(position, position + distance * forward, vector(q.rotate({0, 1, 0})));
    c.set_zoom(p.fov > 0 ? m_height / (2 * distance * std::tan(p.fov / 2)) : m_height / p.ortho_extent);
    m_canvas.update_openaxis_projection();
    m_last_view = c.get_view_matrix();
    m_last_zoom = c.get_zoom();
    m_redraw();
    return true;
}
void OpenAxisController::pivot(std::optional<openaxis::Vec3> p) {
    m_pivot = p;
    m_redraw();
}
Value OpenAxisController::fact(const std::string &name) {
    if (!m_focused || !viewport_available()) return nullptr;
    const auto &c = m_camera;
    if (name == "document.id") return std::to_string(m_canvas.get_model()->id().id);
    if (name == "world.orientation") return {{"forward", {0, 1, 0}}, {"up", {0, 0, 1}}, {"handedness", "right"}};
    if (name == "camera.view_target") return openaxis::vector_value(vector(c.get_target()));
    if (name == "viewport.aspect") return double(m_width) / m_height;
    const bool inside = m_x >= 0 && m_x < m_width && m_y >= 0 && m_y < m_height;
    if (name == "viewport.cursor" && inside)
        return {{"x", 2. * m_x / m_width - 1}, {"y", 1 - 2. * m_y / m_height}};
    const auto &volumes = m_canvas.get_volumes().volumes;
    const auto &selected = m_canvas.get_selection().get_volume_idxs();
    if (name == "model.bounds" || name == "selection.bounds") {
        BoundingBoxf3 bounds;
        for (unsigned i = 0; i < volumes.size(); ++i)
            if (volumes[i]->is_active && !volumes[i]->disabled && (name == "model.bounds" || selected.count(i)))
                bounds.merge(volumes[i]->transformed_bounding_box());
        return bounds_value(bounds);
    }
    const bool center = name == "pick.viewport_center" || name == "pick.viewport_center.selection";
    const bool cursor = name == "pick.cursor" || name == "pick.cursor.selection";
    if ((center || cursor) && (center || inside)) {
        const bool only = name.find(".selection") != std::string::npos;
        const Vec2d pixel(center ? m_width * .5 : m_x, center ? m_height * .5 : m_y);
        Value result = {{"markerPosition", {pixel.x() / m_scale, pixel.y() / m_scale}}};
        double closest = std::numeric_limits<double>::max();
        auto test = [&](const SceneRaycasterItem &item, const Transform3d &transform, const GLVolume *volume) {
            Vec3f point, normal;
            if (!item.is_active() || !item.get_raycaster()->closest_hit(pixel, transform, c, point, normal, nullptr)) return;
            const Vec3d world = transform * point.cast<double>();
            const Vec3d world_normal = (transform.linear().inverse().transpose() * normal.cast<double>()).normalized();
            if (!item.use_back_faces() && world_normal.dot(c.get_dir_forward()) >= 0) return;
            const double distance = (world - c.get_position()).squaredNorm();
            if (distance >= closest) return;
            closest = distance;
            result = {{"point", openaxis::vector_value(vector(world))}, {"markerPosition", {pixel.x() / m_scale, pixel.y() / m_scale}}};
            if (volume) result["bounds"] = bounds_value(volume->transformed_bounding_box());
        };
        for (const auto &item : *m_canvas.get_raycasters_for_picking(SceneRaycaster::EType::Volume)) {
            const int id = SceneRaycaster::decode_id(SceneRaycaster::EType::Volume, item->get_id());
            if (id >= 0 && size_t(id) < volumes.size() && !volumes[id]->disabled && (!only || selected.count(id)))
                test(*item, item->get_transform(), volumes[id]);
        }
        // 2.9 has no selectable bed objects. Beds participate only in ordinary picks.
        if (!only && c.is_looking_downward())
            for (const auto &item : *m_canvas.get_raycasters_for_picking(SceneRaycaster::EType::Bed))
                for (int bed = 0; bed < s_multiple_beds.get_number_of_beds(); ++bed) {
                    Transform3d transform = item->get_transform();
                    transform.translate(s_multiple_beds.get_bed_translation(bed));
                    test(*item, transform, nullptr);
                }
        return result;
    }
    return nullptr;
}
void OpenAxisController::render_indicator() {
    if (!m_pivot || !viewport_available() || context_key() != m_context) return;
    const Vec4d clip = m_camera.get_projection_matrix().matrix() * m_camera.get_view_matrix().matrix() * Vec4d(m_pivot->x, m_pivot->y, m_pivot->z, 1);
    if (!OpenAxisOverlay::visible({clip.x(), clip.y(), clip.z(), clip.w()})) return;
    const ImVec2 pixel(float((clip.x() / clip.w() + 1) * .5 * m_width / m_scale), float((1 - clip.y() / clip.w()) * .5 * m_height / m_scale));
    float depth = 1.f;
    glReadPixels(std::clamp(int(pixel.x * m_scale), 0, m_width - 1),
                 std::clamp(m_height - 1 - int(pixel.y * m_scale), 0, m_height - 1),
                 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
    const int alpha = (clip.z() / clip.w() + 1) * .5 > depth + 1e-5 ? 64 : 255;
    auto *draw = ImGui::GetBackgroundDrawList();
    draw->AddCircle(pixel, 4.75f, IM_COL32(0, 0, 0, alpha), 32, 1.5f);
    draw->AddCircleFilled(pixel, 4.f, IM_COL32(0, 255, 0, alpha));
}
void OpenAxisController::render_overlay() {
    if (!m_overlay_visible || !viewport_available())
        return;
    const auto frame = m_collector.presentation();
    if (frame.context != context_key())
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
    auto *draw = ImGui::GetBackgroundDrawList();
    auto lo = pixel(viewport.pixel(v[0], viewport.top())),
         hi = pixel(viewport.pixel(v[0] + v[2], viewport.top() + v[3]));
    draw->PushClipRect(lo, hi, true);
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
    // Leave room for the 80-logical-pixel viewcube and its surrounding margin.
    const float text_x = lo.x + 120;
    const float line_height = ImGui::GetTextLineHeightWithSpacing();
    float y = std::max(lo.y + 24, hi.y - 24 - float(frame.lines.size()) * line_height);
    for (const auto &line : frame.lines) {
        draw->AddText({text_x + 1, y + 1}, IM_COL32_BLACK, line.text.c_str());
        draw->AddText({text_x, y}, color(line.tone), line.text.c_str());
        y += line_height;
    }
    draw->PopClipRect();
}
} // namespace Slic3r::GUI
