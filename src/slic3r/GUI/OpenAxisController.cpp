// Copied from oss-forks/shared/prusaslicer-family/OpenAxisController.cpp
// Keep copies aligned; see that directory's README.md.
#include "OpenAxisController.hpp"
#include "OpenAxisScheduler.hpp"
#include "3DScene.hpp"
#include "GLCanvas3D.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"
#include "libslic3r/libslic3r.h"
#include <imgui/imgui.h>
#include <wx/dialog.h>
#include <wx/glcanvas.h>
#include <sstream>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <boost/log/trivial.hpp>

namespace Slic3r::GUI {
using openaxis::Value;
namespace {
OpenAxisController *s_instance = nullptr;
openaxis::Vec3 vector(const Vec3d &v) { return {v.x(), v.y(), v.z()}; }
Vec3d vector(openaxis::Vec3 v) { return {v.x, v.y, v.z}; }
openaxis::OpenAxisClientOptions options(openaxis::Scheduler *scheduler, const char *app_name) {
    openaxis::OpenAxisClientOptions o;
    o.client_name = app_name;
    o.client_version = SLIC3R_VERSION;
    o.scheduler = scheduler;
    o.target = {{"pid", openaxis::current_process_id()}, {"app_version", SLIC3R_VERSION}};
    return o;
}
} // namespace
OpenAxisController *OpenAxisController::instance() { return s_instance; }
OpenAxisController::OpenAxisController(Camera &camera, const char *app_name,
                                       const char *log_name, std::vector<std::string> tags)
    : m_camera(camera), m_tags(std::move(tags)),
      m_log(openaxis::DiagnosticLog::configure(log_name, {}, SLIC3R_VERSION)),
      m_scheduler(std::make_shared<OpenAxisScheduler>()), m_client(options(m_scheduler.get(), app_name)),
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
          metadata.tags = m_tags;
          metadata.capabilities = {"navigation"};
          metadata.focused = m_focused;
          return metadata;
      }}) {
    if (s_instance)
        throw std::logic_error("Only one OpenAxis controller may own the application connection");
    initialize_native();
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
    m_pivot.reset();
    context_changed();
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
        m_pivot.reset();
        context_changed();
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
    const bool native_changed = !m_last_view.matrix().isApprox(active_camera().get_view_matrix().matrix()) ||
        m_last_zoom != active_camera().get_zoom() || m_last_projection != active_camera().get_type();
    m_last_view = active_camera().get_view_matrix();
    m_last_zoom = active_camera().get_zoom();
    m_last_projection = active_camera().get_type();
    const bool available = viewport_available();
    focused = focused && available;
    const std::string context = available ? context_key() : "";
    if (context != m_context || (!available && m_session.active())) {
        m_session.cancel("viewport_changed");
        m_collector.clear();
        m_context = context;
        m_pivot.reset();
        context_changed();
    }
    m_collector.set_enabled(m_diagnostics_visible && available);
    m_collector.set_context(context);
    if (m_focused && !focused)
        m_session.cancel("focus_lost");
    if (m_focused != focused) {
        m_focused = focused;
        try {
            m_connection.refresh_metadata();
        } catch (const std::exception &e) {
            // A transport can retire during a focus change. Keep dispatching
            // so the connection manager can process the loss and reconnect.
            BOOST_LOG_TRIVIAL(warning) << "OpenAxis metadata refresh: " << e.what();
        }
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
    const auto &c = active_camera();
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
void OpenAxisController::pivot(std::optional<openaxis::Vec3> p) {
    m_pivot = p;
    redraw();
}
} // namespace Slic3r::GUI
