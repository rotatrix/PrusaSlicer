#pragma once
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <openaxis/diagnostics.hpp>
#include <openaxis/scheduler.hpp>
#include <vector>
#include <wx/app.h>
#include <wx/timer.h>

namespace Slic3r::App::Platform::WX {
// Only enqueueing crosses threads. wx owns dispatch and a single one-shot
// timer for the earliest deadline; there is no periodic integration tick.
class OpenAxisScheduler final : public openaxis::Scheduler, private wxEvtHandler {
    struct Lifetime {
        OpenAxisScheduler *owner;
    };
    std::shared_ptr<Lifetime> m_lifetime = std::make_shared<Lifetime>(Lifetime{this});
    std::multimap<double, Callback> m_deadlines;
    wxTimer m_timer{this};
    void arm() {
        m_timer.Stop();
        if (m_deadlines.empty())
            return;
        double ms = std::ceil((m_deadlines.begin()->first - openaxis::diagnostic_time()) * 1000);
        m_timer.StartOnce(int(std::clamp(ms, 1., double(std::numeric_limits<int>::max()))));
    }

  public:
    std::function<void()> before_dispatch;
    OpenAxisScheduler() {
        Bind(
            wxEVT_TIMER,
            [this](wxTimerEvent &) {
                // Move due callbacks out first: callbacks can enqueue more work.
                std::vector<Callback> due;
                auto end = m_deadlines.upper_bound(openaxis::diagnostic_time());
                for (auto it = m_deadlines.begin(); it != end; ++it)
                    due.push_back(std::move(it->second));
                m_deadlines.erase(m_deadlines.begin(), end);
                for (auto &f : due) {
                    if (before_dispatch)
                        before_dispatch();
                    f();
                }
                arm();
            },
            m_timer.GetId());
    }
    ~OpenAxisScheduler() override {
        m_timer.Stop();
        m_lifetime.reset();
    }
    void post(Callback f) override {
        std::weak_ptr<Lifetime> weak = m_lifetime;
        CallAfter([weak, f = std::move(f)] {
            if (auto life = weak.lock()) {
                if (life->owner->before_dispatch)
                    life->owner->before_dispatch();
                f();
            }
        });
    }
    void post_at(double deadline, Callback f) override {
        std::weak_ptr<Lifetime> weak = m_lifetime;
        CallAfter([weak, deadline, f = std::move(f)]() mutable {
            if (auto life = weak.lock()) {
                life->owner->m_deadlines.emplace(deadline, std::move(f));
                life->owner->arm();
            }
        });
    }
};
} // namespace Slic3r::App::Platform::WX
