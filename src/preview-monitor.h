#pragma once
#include <obs.h>
#include <atomic>
#include <memory>
#include <vector>

namespace lbm {
// Own only changes made by this preview; private settings survive scene saves.
class PreviewMonitorLease {
public:
    explicit PreviewMonitorLease(obs_source_t *source);
    ~PreviewMonitorLease();
    PreviewMonitorLease(const PreviewMonitorLease &) = delete;
    PreviewMonitorLease &operator=(const PreviewMonitorLease &) = delete;
    bool matches(obs_source_t *source) const;
    static void recover(obs_source_t *source);
private:
    static void changed(void *data, calldata_t *event);
    obs_weak_source_t *source_;
    std::atomic<bool> owned_{false};
    static thread_local PreviewMonitorLease *changing_;
};
class PreviewMonitors {
public:
    void begin();
    void refresh();
    void stop();
    static void recover();
    ~PreviewMonitors() { stop(); }
private:
    bool active_ = false;
    std::vector<std::unique_ptr<PreviewMonitorLease>> leases_;
};
} // namespace lbm
