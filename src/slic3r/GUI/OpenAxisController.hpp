#pragma once
#include <optional>
#include <chrono>
#include "Camera.hpp"
#include <functional>
#include <openaxis/navigation.hpp>
#include <openaxis/connection_manager.hpp>

namespace Slic3r::GUI {
class GLCanvas3D;

// PrusaSlicer scene adapter; transport and reconciliation remain in OpenAxis.
class OpenAxisController final : public openaxis::NavigationAdapter {
  public:
    OpenAxisController(GLCanvas3D &, Camera &,
                       std::shared_ptr<openaxis::Scheduler>, std::function<void()> redraw);
    ~OpenAxisController();
    void refresh(bool focused, int x, int y, int width, int height, double scale);
    void deactivate();
    void render_indicator();
    void render_diagnostics(bool &visible);

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
    std::optional<openaxis::Pose> read_camera();
    bool write_camera(const openaxis::Pose &);
    void pivot(std::optional<openaxis::Vec3>);
    bool viewport_available() const;
    void render_overlay();
    GLCanvas3D &m_canvas;
    Camera &m_camera;
    Transform3d m_last_view{Transform3d::Identity()};
    double m_last_zoom = 0;
    Camera::EType m_last_projection = Camera::EType::Unknown;
    std::function<void()> m_redraw;
    std::shared_ptr<openaxis::Scheduler> m_scheduler;
    std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
    std::optional<double> m_diagnostic_deadline;
    bool m_writing_camera = false;
    openaxis::OpenAxisClient m_client;
    openaxis::NavigationDiagnostics m_collector;
    openaxis::NavigationSession m_session;
    openaxis::OpenAxisConnectionManager m_connection;
    int m_width = 0, m_height = 0;
    double m_scale = 1;
    std::string m_context;
    bool m_enabled = true;
    bool m_focused = false;
    int m_x = 0, m_y = 0;
    std::optional<openaxis::Vec3> m_pivot;
    bool m_diagnostics_visible = false;
};
} // namespace Slic3r::GUI
