#include "preview-monitor.h"
namespace lbm {
namespace {
constexpr const char *token = "lbm_preview_restore_monitor_v1";
void mark(obs_source_t *source, bool pending)
{
	auto *settings = obs_source_get_private_settings(source);
	if (pending)
		obs_data_set_bool(settings, token, true);
	else
		obs_data_unset_user_value(settings, token);
	obs_data_release(settings);
}
} // namespace
thread_local PreviewMonitorLease *PreviewMonitorLease::changing_ = nullptr;
void PreviewMonitorLease::changed(void *data, calldata_t *event)
{
	auto &lease = *static_cast<PreviewMonitorLease *>(data);
	if (changing_ == &lease)
		return;
	lease.owned_.store(false);
	mark(static_cast<obs_source_t *>(calldata_ptr(event, "source")), false);
}
PreviewMonitorLease::PreviewMonitorLease(obs_source_t *source) : source_(obs_source_get_weak_source(source))
{
	signal_handler_connect(obs_source_get_signal_handler(source), "monitor", changed, this);
	if (obs_source_get_monitoring_enabled(source)) {
		owned_.store(true);
		mark(source, true);
		changing_ = this;
		obs_source_set_monitoring_enabled(source, false);
		changing_ = nullptr;
	}
}
PreviewMonitorLease::~PreviewMonitorLease()
{
	if (auto *source = obs_weak_source_get_source(source_)) {
		if (owned_.exchange(false)) {
			changing_ = this;
			if (!obs_source_get_monitoring_enabled(source))
				obs_source_set_monitoring_enabled(source, true);
			changing_ = nullptr;
			mark(source, false);
		}
		signal_handler_disconnect(obs_source_get_signal_handler(source), "monitor", changed, this);
		obs_source_release(source);
	}
	obs_weak_source_release(source_);
}
bool PreviewMonitorLease::matches(obs_source_t *source) const
{
	auto *weak = obs_source_get_weak_source(source);
	const bool same = weak == source_;
	obs_weak_source_release(weak);
	return same;
}
void PreviewMonitorLease::recover(obs_source_t *source)
{
	auto *settings = obs_source_get_private_settings(source);
	const bool pending = obs_data_get_bool(settings, token);
	obs_data_release(settings);
	if (pending) {
		if (!obs_source_get_monitoring_enabled(source))
			obs_source_set_monitoring_enabled(source, true);
		mark(source, false);
	}
}
void PreviewMonitors::begin()
{
	active_ = true;
	refresh();
}
void PreviewMonitors::refresh()
{
	if (!active_)
		return;
	obs_enum_sources(
		[](void *data, obs_source_t *source) {
			auto &self = *static_cast<PreviewMonitors *>(data);
			for (const auto &lease : self.leases_)
				if (lease->matches(source))
					return true;
			if (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO)
				self.leases_.push_back(std::make_unique<PreviewMonitorLease>(source));
			return true;
		},
		this);
}
void PreviewMonitors::stop()
{
	active_ = false;
	leases_.clear();
}
void PreviewMonitors::recover()
{
	obs_enum_sources(
		[](void *, obs_source_t *source) {
			PreviewMonitorLease::recover(source);
			return true;
		},
		nullptr);
}
} // namespace lbm
