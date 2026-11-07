#include "preview-category-capture.h"
#include "spsc-queue.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>

namespace lbm {

struct PreviewCategoryCapture::State {
	struct Source {
		obs_source_t *source = nullptr;
		uint64_t timestamp = 0;
		bool pending = true;
		bool seen = false;
		std::array<std::array<float, AUDIO_OUTPUT_FRAMES>, 2> pcm{};
	};
	using Holder = std::shared_ptr<State>;
	static thread_local Holder creating;
	static constexpr unsigned closed = 1u << 31;
	std::atomic<unsigned> readers{0};
	std::atomic<const char *> failure{nullptr};
	std::vector<Source> sources;
	SPSCQueue<CategoryBlock, 128> queue;
	CategoryBlock block;
	unsigned mix = 0;
	uint32_t rate = 0;
	uint64_t sequence = 0, consumed = 0;

	~State()
	{
		for (auto &s : sources)
			if (s.source)
				obs_source_release(s.source);
	}
	void fail(const char *message) noexcept
	{
		const char *expected = nullptr;
		failure.compare_exchange_strong(expected, message, std::memory_order_release);
	}
	bool enter() noexcept
	{
		unsigned n = readers.load(std::memory_order_acquire);
		while (!(n & closed)) {
			if (readers.compare_exchange_weak(n, n + 1, std::memory_order_acq_rel))
				return true;
		}
		return false;
	}
	void leave() noexcept { readers.fetch_sub(1, std::memory_order_release); }
	void close() noexcept
	{
		readers.fetch_or(closed, std::memory_order_acq_rel);
		while (readers.load(std::memory_order_acquire) != closed)
			std::this_thread::yield();
	}
	static void mark(obs_source_t *, obs_source_t *source, void *data)
	{
		for (auto &s : static_cast<State *>(data)->sources)
			if (s.source == source)
				s.seen = true;
	}
	bool in_main_tree()
	{
		for (auto &s : sources)
			s.seen = false;
		for (unsigned channel = 0; channel < MAX_CHANNELS; ++channel) {
			auto *root = obs_get_output_source(channel);
			if (!root)
				continue;
			if (obs_source_active(root) && !obs_source_removed(root)) {
				mark(nullptr, root, this);
				obs_source_enum_active_tree(root, mark, this);
			}
			obs_source_release(root);
		}
		for (const auto &s : sources)
			if (!s.seen || obs_source_removed(s.source))
				return false;
		return true;
	}
	static bool render(void *data, uint64_t *, obs_source_audio_mix *, uint32_t mixers, size_t channels,
			   size_t sample_rate)
	{
		auto &s = **static_cast<Holder *>(data);
		if (!s.enter())
			return false;
		if (!s.failure.load(std::memory_order_acquire))
			s.snapshot(mixers, channels, sample_rate);
		s.leave();
		// This private observer is never added to a scene or output root.
		return false;
	}
	void snapshot(uint32_t mixers, size_t channels, size_t sample_rate)
	{
		if (channels != 2 || sample_rate != rate) {
			fail("Category capture audio format or mix changed");
			return;
		}
		if (!(mixers & (1u << mix)))
			return; // No raw subscriber yet.
		if (!in_main_tree()) {
			fail("Category source left the active main-view tree; other views are unsupported");
			return;
		}
		++sequence;
		for (auto &s : sources) {
			s.pending = obs_source_audio_pending(s.source);
			s.timestamp = obs_source_get_audio_timestamp(s.source);
			if (s.pending || !s.timestamp)
				continue;
			obs_source_audio_mix audio{};
			obs_source_get_audio_mix(s.source, &audio);
			for (size_t channel = 0; channel < 2; ++channel) {
				if (!audio.output[mix].data[channel]) {
					fail("Category source rendered without PCM");
					return;
				}
				std::memcpy(s.pcm[channel].data(), audio.output[mix].data[channel],
					    AUDIO_OUTPUT_FRAMES * sizeof(float));
			}
		}
	}
};

thread_local PreviewCategoryCapture::State::Holder PreviewCategoryCapture::State::creating;

void register_preview_category_capture()
{
	using State = PreviewCategoryCapture::State;
	obs_source_info info{};
	info.id = "lbm_preview_category_observer";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_AUDIO;
	info.get_name = [](void *) {
		return "LBM preview category observer";
	};
	info.create = [](obs_data_t *, obs_source_t *) -> void * {
		return State::creating ? new State::Holder(State::creating) : nullptr;
	};
	info.destroy = [](void *data) {
		delete static_cast<State::Holder *>(data);
	};
	info.audio_render = State::render;
	obs_register_source(&info);
}

PreviewCategoryCapture::PreviewCategoryCapture() = default;
PreviewCategoryCapture::~PreviewCategoryCapture()
{
	stop();
}

bool PreviewCategoryCapture::start(const std::string &voice, const std::vector<std::string> &bgm, unsigned mix_index,
				   uint32_t rate)
{
	stop();
	state_ = std::make_shared<State>();
	auto &s = *state_;
	s.mix = mix_index;
	s.rate = rate;
	auto *audio = obs_get_audio();
	if (!audio || mix_index >= MAX_AUDIO_MIXES || (rate != 44100 && rate != 48000) ||
	    audio_output_get_sample_rate(audio) != rate || audio_output_get_channels(audio) != 2) {
		s.fail("Category capture requires the selected mix at 44.1 or 48 kHz stereo");
		return false;
	}
	if (voice.empty() || bgm.empty()) {
		s.fail("Select one voice source and at least one BGM source");
		return false;
	}
	s.sources.resize(bgm.size() + 1);
	for (size_t i = 0; i < s.sources.size(); ++i) {
		const auto &name = i ? bgm[i - 1] : voice;
		auto *source = obs_get_source_by_name(name.c_str());
		s.sources[i].source = source;
		if (!source || obs_source_removed(source) ||
		    !(obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO)) {
			s.fail("Selected category audio source is missing or removed");
			return false;
		}
		for (size_t j = 0; j < i; ++j) {
			if (s.sources[j].source == source) {
				s.fail("Voice and BGM selections must contain distinct sources");
				return false;
			}
		}
	}
	if (!s.in_main_tree()) {
		s.fail("Category sources must belong to the active main-view tree; other views are unsupported");
		return false;
	}
	State::creating = state_;
	observer_ = obs_source_create_private("lbm_preview_category_observer", "LBM preview categories", nullptr);
	State::creating.reset();
	if (!observer_)
		s.fail("Failed to create the category audio observer");
	return observer_ != nullptr;
}

void PreviewCategoryCapture::consume_raw(const audio_data &packet) noexcept
{
	if (!state_)
		return;
	auto &s = *state_;
	if (s.failure.load(std::memory_order_acquire))
		return;
	if (!packet.frames || packet.frames > AUDIO_OUTPUT_FRAMES || !packet.timestamp) {
		s.fail("Invalid raw audio packet for category capture");
		return;
	}
	if (!s.sequence || s.sequence == s.consumed) {
		s.fail("Category snapshot is missing for this raw audio cycle");
		return;
	}
	s.consumed = s.sequence;
	auto &block = s.block;
	block.timestamp = packet.timestamp;
	block.frames = packet.frames;
	block.voice.fill(0);
	block.bgm.fill(0);
	const uint64_t end = packet.timestamp + uint64_t(packet.frames) * 1000000000ULL / s.rate;
	for (size_t index = 0; index < s.sources.size(); ++index) {
		const auto &source = s.sources[index];
		if (obs_source_removed(source.source)) {
			s.fail("Category source was removed during capture");
			return;
		}
		// OBS can reject a rendered source during buffering. Only the final
		// pending value at the raw callback determines inclusion in this cycle.
		if (obs_source_audio_pending(source.source))
			continue;
		if (source.pending || !source.timestamp) {
			s.fail("Category source timing is unavailable for this raw audio cycle");
			return;
		}
		// Same timestamp interval and integer frame conversion as libobs mix_audio.
		if (source.timestamp < packet.timestamp || source.timestamp >= end)
			continue;
		const uint32_t offset = uint32_t((source.timestamp - packet.timestamp) * s.rate / 1000000000ULL);
		auto &pcm = index ? block.bgm : block.voice;
		for (uint32_t frame = offset; frame < packet.frames; ++frame) {
			pcm[frame * 2] += source.pcm[0][frame - offset];
			pcm[frame * 2 + 1] += source.pcm[1][frame - offset];
		}
	}
	if (!s.queue.try_push(block))
		s.fail("Category analysis queue overflowed");
}

bool PreviewCategoryCapture::pop(CategoryBlock &block) noexcept
{
	return state_ && state_->queue.try_pop(block);
}

const char *PreviewCategoryCapture::error() const noexcept
{
	const char *message = state_ ? state_->failure.load(std::memory_order_acquire) : nullptr;
	return message ? message : "";
}

void PreviewCategoryCapture::stop()
{
	if (state_)
		state_->close();
	if (observer_) {
		obs_source_remove(observer_);
		obs_source_release(observer_);
		observer_ = nullptr;
	}
	state_.reset();
}

} // namespace lbm
