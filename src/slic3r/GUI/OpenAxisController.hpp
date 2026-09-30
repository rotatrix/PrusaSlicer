#pragma once
#include <memory>
#include <optional>
#include <string>
#include "Camera.hpp"
#include "GLModel.hpp"
#include <openaxis/connection_manager.hpp>
#include <openaxis/logging.hpp>
#include <openaxis/navigation.hpp>

namespace Slic3r::GUI {
class GLCanvas3D;
class OpenAxisScheduler;

// One integration per application, owned by the plater. It follows the plater's
// current canvas (3D view or preview); transport and reconciliation remain in OpenAxis.
class OpenAxisController final : public openaxis::NavigationAdapter {
  public:
    explicit OpenAxisController(Camera &);
    ~OpenAxisController();
    static OpenAxisController *instance();
    // Refresh the bound canvas, focus, cursor and context from the plater.
    void poll();
    void canvas_destroyed(const GLCanvas3D &);
    void render_indicator(GLCanvas3D &);
    void render_diagnostics(GLCanvas3D &);
    bool diagnostics_visible() const { return m_diagnostics_visible; }
    void toggle_diagnostics();

  private:
    void schedule_diagnostic_expiry();
    struct QueryCapture;
    openaxis::NavigationContext capture_context() override;
    bool is_current(const openaxis::NavigationContext &) override;
    std::unique_ptr<openaxis::NavigationCapture> begin_query(const openaxis::NavigationContext &) override;
    openaxis::WriteResult apply_pose(const openaxis::NavigationContext &, const openaxis::NavigationPose &,
                                    const openaxis::Value &, std::optional<openaxis::Vec3>) override;
    void show_pivot(const openaxis::NavigationContext &, std::optional<openaxis::Vec3>) override;
    std::string context_key() const;
    openaxis::Value fact(const std::string &);
    openaxis::Value pick(const std::string &name, bool inside);
    std::optional<openaxis::Pose> read_camera();
    bool write_camera(const openaxis::Pose &);
    void pivot(std::optional<openaxis::Vec3>);
    void redraw();
    bool viewport_available() const;
    void render_overlay();
    Camera &m_camera;
    std::shared_ptr<openaxis::DiagnosticLog> m_log;
    std::shared_ptr<OpenAxisScheduler> m_scheduler;
    std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
    std::optional<double> m_diagnostic_deadline;
    bool m_writing_camera = false;
    openaxis::OpenAxisClient m_client;
    openaxis::NavigationDiagnostics m_collector;
    openaxis::NavigationSession m_session;
    openaxis::OpenAxisConnectionManager m_connection;
    GLCanvas3D *m_canvas = nullptr;
    Transform3d m_last_view{Transform3d::Identity()};
    double m_last_zoom = 0;
    Camera::EType m_last_projection = Camera::EType::Unknown;
    int m_width = 0, m_height = 0;
    double m_scale = 1;
    std::string m_context;
    bool m_focused = false;
    int m_x = -1, m_y = -1;
    std::optional<openaxis::Vec3> m_pivot;
    GLModel m_pivot_fill, m_pivot_rim;
    bool m_diagnostics_visible = false;
};
} // namespace Slic3r::GUI
