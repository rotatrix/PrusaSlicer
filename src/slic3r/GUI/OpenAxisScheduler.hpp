// Copied from oss-forks/shared/prusaslicer-family/OpenAxisScheduler.hpp
// Keep copies aligned; see that directory's README.md.
#pragma once
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <openaxis/diagnostics.hpp>
#include <openaxis/scheduler.hpp>
#include <vector>
#include <wx/app.h>
#include <wx/timer.h>

namespace Slic3r::GUI {
// Only enqueueing crosses threads. Native modal loops may stop servicing
// CallAfter, so a native timer also drains our own queue (not wx's UI queue).
class OpenAxisScheduler final : public openaxis::Scheduler, private wxEvtHandler {
    struct Lifetime {
        OpenAxisScheduler *owner;
    };
    std::shared_ptr<Lifetime> m_lifetime = std::make_shared<Lifetime>(Lifetime{this});
    std::multimap<double, Callback> m_deadlines;
    std::mutex m_mutex;
    bool m_wakeup_pending = false;
    bool m_dispatching = false;
    wxTimer m_timer{this};
    wxTimer m_modal_timer{this};
    void arm() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_timer.Stop();
        if (m_deadlines.empty())
            return;
        double ms = std::ceil((m_deadlines.begin()->first - openaxis::diagnostic_time()) * 1000);
        m_timer.StartOnce(int(std::clamp(ms, 1., double(std::numeric_limits<int>::max()))));
    }

    void dispatch() {
        // A callback can enter a nested modal loop. Do not reenter SDK work.
        if (m_dispatching)
            return;
        m_dispatching = true;
        struct Reset { bool &value; ~Reset() { value = false; } } reset{m_dispatching};
        std::vector<Callback> due;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto end = m_deadlines.upper_bound(openaxis::diagnostic_time());
            for (auto it = m_deadlines.begin(); it != end; ++it)
                due.push_back(std::move(it->second));
            m_deadlines.erase(m_deadlines.begin(), end);
        }
        for (auto &f : due) {
            if (before_dispatch)
                before_dispatch();
            f();
        }
        arm();
    }

  public:
    std::function<void()> before_dispatch;
    OpenAxisScheduler() {
        Bind(wxEVT_TIMER, [this](wxTimerEvent &) { dispatch(); }, m_timer.GetId());
        Bind(wxEVT_TIMER, [this](wxTimerEvent &) { dispatch(); }, m_modal_timer.GetId());
        // Normal dispatch still uses CallAfter and exact one-shot deadlines.
        // This fallback keeps transport/reconnect work alive inside file dialogs.
        m_modal_timer.Start(100);
    }
    ~OpenAxisScheduler() override {
        m_timer.Stop();
        m_modal_timer.Stop();
        m_lifetime.reset();
    }
    void post(Callback f) override {
        post_at(openaxis::diagnostic_time(), std::move(f));
    }
    void post_at(double deadline, Callback f) override {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_deadlines.emplace(deadline, std::move(f));
            if (m_wakeup_pending)
                return;
            m_wakeup_pending = true;
        }
        std::weak_ptr<Lifetime> weak = m_lifetime;
        CallAfter([weak] {
            if (auto life = weak.lock()) {
                {
                    std::lock_guard<std::mutex> lock(life->owner->m_mutex);
                    life->owner->m_wakeup_pending = false;
                }
                life->owner->dispatch();
            }
        });
    }
};
} // namespace Slic3r::GUI
