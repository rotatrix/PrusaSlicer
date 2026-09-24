#include "Slic3r/App/Plater/OpenAxisController.hpp"
#include "Slic3r/App/Plater/OpenAxisOverlay.hpp"
#include "Slic3r/App/Plater/OpenAxisPicking.hpp"
#include "Slic3r/App/Plater/PlaterScenePresenter.hpp"
#include "Slic3r/App/Render/Context.hpp"
#include "Slic3r/App/Render/Device.hpp"
#include "Slic3r/App/Render/GL/commonGL.hpp"
#include "Slic3r/App/Scene/BedNodeTag.hpp"
#include "Slic3r/App/Scene/NodeVisitor.hpp"
#include "Slic3r/App/Scene/SceneNodeTag.hpp"
#include "Slic3r/Biz/ProjectInteractor.hpp"
#include "Slic3r/Log.hpp"
#include <imgui/imgui.h>
#include <numbers>
#include <openaxis/logging.hpp>
#include <sstream>

namespace Slic3r::App::Plater {
using openaxis::Value;
namespace {
openaxis::Vec3 vector(const Domain::Vec3d &v) { return {v.x(), v.y(), v.z()}; }
Domain::Vec3d vector(openaxis::Vec3 v) { return {v.x, v.y, v.z}; }
Value bounds_value(const Eigen::AlignedBox3f &b) {
    if (b.isEmpty())
        return nullptr;
    return {{"min", {b.min().x(), b.min().y(), b.min().z()}},
            {"max", {b.max().x(), b.max().y(), b.max().z()}}};
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
OpenAxisController::OpenAxisController(PlaterScenePresenter &p, Biz::ProjectInteractor &i,
                                       std::shared_ptr<openaxis::Scheduler> scheduler,
                                       std::function<void()> redraw)
    : m_presenter(p), m_interactor(i), m_redraw(std::move(redraw)),
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
    m_presenter.add_listener<Scene::ICameraUpdateListener>(this);
    m_collector.on_changed = [this] {
        if (m_overlay_visible || m_diagnostics_visible)
            m_redraw();
    };
    m_client.on_connection = [this](bool connected) {
        record_diagnostic(connected ? "transport: connected" : "transport: disconnected");
    };
    m_client.on_error = [this](const std::string &e) {
        record_diagnostic("transport error: " + e, "debug");
        SPDLOG_DEBUG("OpenAxis: {}", e);
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
    m_presenter.remove_listener<Scene::ICameraUpdateListener>(this);
    m_lifetime.reset();
    m_connection.stop();
    m_session.close();
}
void OpenAxisController::camera_updated(const Scene::Camera &) {
    if (!m_writing_camera && m_enabled)
        m_session.native_camera_changed();
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
    if (!m_presenter.project_ready())
        return false;
    const auto &vp = m_presenter.scene().camera().viewport();
    return vp.width > 0 && vp.height > 0 && m_screen.physical_width() > 0 &&
           m_screen.physical_height() > 0;
}
std::string OpenAxisController::connection_status() const {
    const auto status = m_connection.status();
    if (status.state == "retrying")
        return fmt::format("Retrying in {:.1f}s{}",
                           std::max(0., status.retry_at.value_or(openaxis::diagnostic_time()) -
                                            openaxis::diagnostic_time()),
                           status.error.empty() ? "" : ": " + status.error);
    return status.state;
}
void OpenAxisController::refresh(bool focused, int x, int y, const Render::ScreenInfo &screen) {
    m_screen = screen;
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
        text = fmt::format("{:8.3f}s  {}", seconds, text);
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
    if (!m_presenter.project_ready())
        return "unavailable";
    const auto &vp = m_presenter.scene().camera().viewport();
    return m_interactor.selected_project().metadata().id + "/" +
           std::to_string(m_interactor.selected_project_id()) + "/plater/" +
           std::to_string(reinterpret_cast<std::uintptr_t>(&m_presenter.scene())) + "/" +
           std::to_string(vp.x) + "/" + std::to_string(vp.y) + "/" + std::to_string(vp.width) +
           "/" + std::to_string(vp.height) + "/" + std::to_string(m_screen.scale());
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
    const auto &c = m_presenter.scene().camera();
    if (c.viewport().width <= 0 || c.viewport().height <= 0)
        return {};
    auto q = openaxis::Quat::from_basis(vector(c.right()), vector(c.up()), vector(-c.forward()));
    openaxis::Pose p{vector(c.position()), q.rotvec()};
    double yy = c.projection()(1, 1);
    if (!std::isfinite(yy) || yy <= 0)
        return {};
    if (c.cam_projection().type() == Scene::CameraProjectionType::Perspective)
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
    auto &scene = m_presenter.scene();
    auto &c = scene.camera();
    bool perspective = c.cam_projection().type() == Scene::CameraProjectionType::Perspective;
    if (perspective != (p.fov > 0))
        c.switch_projection_type();
    double yy = c.projection()(1, 1);
    if (p.fov > 0)
        c.set_zoom(c.zoom() * (2 * std::atan(1 / yy)) / p.fov);
    else
        c.set_zoom(c.zoom() * (2 / yy) / p.ortho_extent);
    auto q = openaxis::Quat::from_rotvec(p.r);
    c.look_at(vector(p.t), vector(p.t + q.rotate({0, 0, -1})), vector(q.rotate({0, 1, 0})));
    scene.camera_trackball().synchronize_from_camera(m_pivot.has_value());
    m_redraw();
    return true;
}
void OpenAxisController::pivot(std::optional<openaxis::Vec3> p) {
    m_pivot = p;
    if (p && m_presenter.project_ready())
        m_presenter.scene().camera_trackball().set_pivot(vector(*p));
    m_redraw();
}
Value OpenAxisController::fact(const std::string &name) {
    if (!m_focused || !viewport_available())
        return nullptr;
    auto &scene = m_presenter.scene();
    auto &c = scene.camera();
    const auto &vp = c.viewport();
    if (name == "document.id")
        return m_interactor.selected_project().metadata().id;
    if (name == "world.orientation")
        return {{"forward", {0, 1, 0}}, {"up", {0, 0, 1}}, {"handedness", "right"}};
    if (name == "camera.view_target")
        return openaxis::vector_value(vector(scene.camera_trackball().target()));
    if (name == "viewport.aspect" && vp.height > 0)
        return double(vp.width) / vp.height;
    const double top = double(m_screen.physical_height()) - vp.y - vp.height;
    bool inside = vp.width > 0 && vp.height > 0 && m_x >= vp.x && m_x < vp.x + vp.width &&
                  m_y >= top && m_y < top + vp.height;
    if (name == "viewport.cursor" && inside)
        return {{"x", 2. * (m_x - vp.x) / vp.width - 1}, {"y", 1 - 2. * (m_y - top) / vp.height}};
    const auto &selection = m_interactor.scene_interactor().object_selection();
    auto selected = [&](const Scene::Node &n) {
        const auto *tag = n.tag_of_type<Scene::SceneNodeTag>();
        return tag &&
               (tag->is_wipe_tower()
                    ? selection.is_selected(Domain::ElementRef{tag->wipe_tower_id})
                    : selection.is_selected({tag->object_id, tag->instance_id, tag->volume_id}));
    };
    if (name == "model.bounds" || name == "selection.bounds") {
        Eigen::AlignedBox3f bounds;
        Scene::visit(scene.root(), [&](const Scene::Node &n) {
            if (n.has_tag_of_type<Scene::SceneNodeTag>() && n.has_raycast_component() &&
                (name == "model.bounds" || selected(n)))
                bounds.extend(
                    n.raycast_component()->world_bounding_box(n.world_transform().matrix()));
        });
        return bounds_value(bounds);
    }
    bool center = name == "pick.viewport_center" || name == "pick.viewport_center.selection";
    bool cursor = name == "pick.cursor" || name == "pick.cursor.selection";
    if ((center || cursor) && (center || inside)) {
        bool only = name.find(".selection") != std::string::npos;
        if (only && selection.empty() && m_interactor.scene_interactor().bed_selection().empty())
            return nullptr;
        Scene::NodePickResults hits;
        Scene::Ray ray;
        const double pixel_x = center ? vp.x + vp.width * .5 : m_x;
        const double pixel_y = center ? top + vp.height * .5 : m_y;
        scene.pick_at(float(pixel_x), float(vp.y + pixel_y - top), hits, &ray);
        const Value marker = {pixel_x / m_screen.scale(), pixel_y / m_screen.scale()};
        for (const auto &hit : hits) {
            const auto &n = *hit.node;
            const auto *bed = n.tag_of_type<Scene::BedNodeTag>();
            const bool bed_selected =
                bed && m_interactor.scene_interactor().bed_selection().is_selected(
                           Domain::BedRef{bed->config_container_id, bed->instance_id});
            const bool eligible =
                bed ? openaxis_bed_pickable(*bed, only, bed_selected)
                    : n.has_tag_of_type<Scene::SceneNodeTag>() && (!only || selected(n));
            if (!eligible)
                continue;
            Value result = {
                {"point", openaxis::vector_value(vector(ray.point_at(hit.cast.distance)))}};
            result["markerPosition"] = marker;
            auto b = bounds_value(
                n.raycast_component()->world_bounding_box(n.world_transform().matrix()));
            if (!b.is_null())
                result["bounds"] = b;
            return result;
        }
        return Value{{"markerPosition", marker}};
    }
    return nullptr;
}
void OpenAxisController::render_indicator(Render::Device &device, Render::CommandBuffer &cmd,
                                          const Render::ScreenInfo &screen) {
    m_screen = screen;
    if (!m_pivot || !viewport_available() || context_key() != m_context)
        return;
    const auto &camera = m_presenter.scene().camera();
    const auto &v = camera.viewport();
    const Domain::Vec4d clip = camera.projection() * camera.view().matrix() *
                               Domain::Vec4d(m_pivot->x, m_pivot->y, m_pivot->z, 1);
    if (!OpenAxisOverlay::visible({clip.x(), clip.y(), clip.z(), clip.w()}))
        return;
    const Domain::Vec3f center = (clip.head<3>() / clip.w()).cast<float>();
    // Keep this small transient draw outside the scene graph and restore all state
    // touched by the native geometry renderer, including its binding cache.
    struct State {
        Render::Device &device;
        GLint viewport[4], scissor[4], depth_func, src_rgb, dst_rgb, src_alpha, dst_alpha,
            equation_rgb, equation_alpha, program, vao, vertex_buffer, index_buffer;
        GLboolean depth, blend, cull, scissor_enabled, depth_write;
        explicit State(Render::Device &d) : device(d) {
            glGetIntegerv(GL_VIEWPORT, viewport);
            glGetIntegerv(GL_SCISSOR_BOX, scissor);
            glGetIntegerv(GL_DEPTH_FUNC, &depth_func);
            glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_write);
            depth = glIsEnabled(GL_DEPTH_TEST);
            blend = glIsEnabled(GL_BLEND);
            cull = glIsEnabled(GL_CULL_FACE);
            scissor_enabled = glIsEnabled(GL_SCISSOR_TEST);
            glGetIntegerv(GL_BLEND_SRC_RGB, &src_rgb);
            glGetIntegerv(GL_BLEND_DST_RGB, &dst_rgb);
            glGetIntegerv(GL_BLEND_SRC_ALPHA, &src_alpha);
            glGetIntegerv(GL_BLEND_DST_ALPHA, &dst_alpha);
            glGetIntegerv(GL_BLEND_EQUATION_RGB, &equation_rgb);
            glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &equation_alpha);
            glGetIntegerv(GL_CURRENT_PROGRAM, &program);
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &vertex_buffer);
            glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &index_buffer);
        }
        ~State() {
            auto enabled = [](GLenum cap, GLboolean value) {
                if (value)
                    glEnable(cap);
                else
                    glDisable(cap);
            };
            enabled(GL_DEPTH_TEST, depth);
            enabled(GL_BLEND, blend);
            enabled(GL_CULL_FACE, cull);
            enabled(GL_SCISSOR_TEST, scissor_enabled);
            glDepthFunc(depth_func);
            glDepthMask(depth_write);
            glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
            glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
            glBlendFuncSeparate(src_rgb, dst_rgb, src_alpha, dst_alpha);
            glBlendEquationSeparate(equation_rgb, equation_alpha);
            glUseProgram(program);
            glBindVertexArray(vao);
            glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, index_buffer);
            device.load_state();
        }
    } restore(device);
    if (!m_marker_geometry)
        m_marker_geometry = std::make_unique<Render::DynamicGeometry<Render::VertexP3>>(device);
    cmd.set_viewport(v);
    cmd.set_scissor(v);
    cmd.set_scissor_enabled(true);
    cmd.set_cull_face_enabled(false);
    cmd.set_depth_write_enabled(false);
    cmd.set_blending_enabled(true);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    cmd.set_depth_test_enabled(true);
    const auto *shader = device.context().shader_manager().shader("flat");
    const Domain::SquareMatrix4f identity = Domain::SquareMatrix4f::Identity();
    auto point = [&](float radius, double angle) -> Domain::Vec3f {
        return {center.x() + float(2 * radius * screen.scale() * std::cos(angle) / v.width),
                center.y() + float(2 * radius * screen.scale() * std::sin(angle) / v.height),
                center.z()};
    };
    for (bool occluded : {true, false}) {
        glDepthFunc(occluded ? GL_GREATER : GL_LEQUAL);
        const float alpha = occluded ? .25f : 1.f;
        Render::Material green(shader), black(shader);
        green.set_uniform("projection_view_model_matrix", identity)
            .set_uniform("uniform_color", Domain::Vec4f(0, 1, 0, alpha));
        black.set_uniform("projection_view_model_matrix", identity)
            .set_uniform("uniform_color", Domain::Vec4f(0, 0, 0, alpha));
        {
            auto fill = m_marker_geometry->set_material(green).build_primitive(
                Render::PrimitiveType::Triangles);
            auto rim = m_marker_geometry->set_material(black).build_primitive(
                Render::PrimitiveType::Triangles);
            for (int k = 0; k < 32; ++k) {
                double a = 2 * std::numbers::pi * k / 32, b = 2 * std::numbers::pi * (k + 1) / 32;
                auto ia = point(4, a), ib = point(4, b), oa = point(5.5f, a), ob = point(5.5f, b);
                fill.vertex(center).vertex(ia).vertex(ib);
                // Annulus avoids blending a translucent green disc over black.
                rim.vertex(ia).vertex(oa).vertex(ob).vertex(ia).vertex(ob).vertex(ib);
            }
        }
        m_marker_geometry->draw(cmd);
    }
}
void OpenAxisController::render_overlay() {
    if (!m_overlay_visible || !viewport_available())
        return;
    const auto frame = m_collector.presentation();
    if (frame.context != context_key())
        return;
    const auto &camera = m_presenter.scene().camera();
    const auto &v = camera.viewport();
    OpenAxisOverlay::Viewport viewport{double(v.x),
                                       double(v.y),
                                       double(v.width),
                                       double(v.height),
                                       double(m_screen.physical_height()),
                                       m_screen.scale()};
    auto pixel = [](const std::array<double, 2> &p) { return ImVec2(float(p[0]), float(p[1])); };
    auto clip = [&](openaxis::Vec3 p) {
        const Domain::Vec4d q =
            camera.projection() * camera.view().matrix() * Domain::Vec4d(p.x, p.y, p.z, 1);
        return OpenAxisOverlay::Clip{q.x(), q.y(), q.z(), q.w()};
    };
    auto color = [](const std::string &tone, double opacity = 1) {
        const auto &c = openaxis::diagnostic_colors().at(tone);
        return IM_COL32(c[0], c[1], c[2], int(255 * opacity));
    };
    auto *draw = ImGui::GetBackgroundDrawList();
    auto lo = pixel(viewport.pixel(v.x, viewport.top())),
         hi = pixel(viewport.pixel(v.x + v.width, viewport.top() + v.height));
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
} // namespace Slic3r::App::Plater
