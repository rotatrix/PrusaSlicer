#pragma once
#include <optional>
#include "Slic3r/App/Render/DynamicGeometry.hpp"
#include "Slic3r/App/Render/ScreenInfo.hpp"
#include "Slic3r/App/Scene/Camera.hpp"
#include <chrono>
#include <deque>
#include <functional>
#include <openaxis/navigation.hpp>
#include <openaxis/connection_manager.hpp>

namespace Slic3r::Biz {
class ProjectInteractor;
}
namespace Slic3r::App::Plater {
class PlaterScenePresenter;

// PrusaSlicer scene adapter; transport and reconciliation remain in OpenAxis.
class OpenAxisController final : public openaxis::NavigationAdapter,
                                 private Scene::ICameraUpdateListener {
  public:
    OpenAxisController(PlaterScenePresenter &, Biz::ProjectInteractor &,
                       std::shared_ptr<openaxis::Scheduler>, std::function<void()> redraw);
    ~OpenAxisController();
    void refresh(bool focused, int x, int y, const Render::ScreenInfo &);
    void deactivate();
    void render_indicator(Render::Device &, Render::CommandBuffer &, const Render::ScreenInfo &);
    void render_diagnostics(bool &visible);

  private:
    void camera_updated(const Scene::Camera &) override;
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
    void record_diagnostic(std::string text, const std::string &level = "info") noexcept;
    bool viewport_available() const;
    void render_overlay();
    std::string connection_status() const;
    PlaterScenePresenter &m_presenter;
    Biz::ProjectInteractor &m_interactor;
    std::function<void()> m_redraw;
    std::shared_ptr<openaxis::Scheduler> m_scheduler;
    std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
    std::optional<double> m_diagnostic_deadline;
    bool m_writing_camera = false;
    openaxis::OpenAxisClient m_client;
    openaxis::NavigationDiagnostics m_collector;
    openaxis::NavigationSession m_session;
    openaxis::OpenAxisConnectionManager m_connection;
    Render::ScreenInfo m_screen{0, 0, 1};
    std::unique_ptr<Render::DynamicGeometry<Render::VertexP3>> m_marker_geometry;
    std::string m_context;
    bool m_overlay_visible = false;
    bool m_enabled = true;
    bool m_focused = false;
    int m_x = 0, m_y = 0;
    std::optional<openaxis::Vec3> m_pivot;
    std::deque<std::string> m_diagnostic_log;
    const std::chrono::steady_clock::time_point m_log_start = std::chrono::steady_clock::now();
    bool m_diagnostics_visible = false;
    bool m_diagnostics_paused = false;
    bool m_diagnostics_writes = false;
    bool m_diagnostics_autoscroll = true;
    bool m_diagnostics_dirty = false;
    std::uint64_t m_camera_writes = 0;
};
} // namespace Slic3r::App::Plater
