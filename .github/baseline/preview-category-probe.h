#pragma once

// Isolated feasibility experiment; not used by the product.
#include <atomic>
#include <algorithm>
#include <cstring>
#include <limits>

namespace category_probe {
constexpr size_t packet_limit = 2048;
constexpr size_t source_count = 3;
struct Block {
	uint64_t timestamp[source_count]{};
	uint32_t pending[source_count]{};
	float pcm[source_count][2][AUDIO_OUTPUT_FRAMES]{};
};
struct RawBlock {
	uint64_t timestamp = 0;
	uint32_t frames = 0;
	uint32_t render_index = UINT32_MAX;
	uint32_t pending[source_count]{};
	float pcm[2][AUDIO_OUTPUT_FRAMES]{};
};
struct State {
	obs_source_t *sources[source_count]{};
	std::vector<Block> blocks{packet_limit};
	std::vector<RawBlock> raw{packet_limit};
	size_t count = 0, raw_count = 0;
	bool overflow = false, raw_overflow = false;
	// The high bit closes admission. Only the control thread waits for readers.
	std::atomic<unsigned> readers{0};
	static constexpr unsigned closed = 1u << 31;
	uint32_t rate = 0;
	bool enter()
	{
		unsigned n = readers.load(std::memory_order_acquire);
		while (!(n & closed)) {
			if (readers.compare_exchange_weak(n, n + 1, std::memory_order_acq_rel))
				return true;
		}
		return false;
	}
	void leave() { readers.fetch_sub(1, std::memory_order_release); }
	void close()
	{
		readers.fetch_or(closed, std::memory_order_acq_rel);
		while (readers.load(std::memory_order_acquire) != closed)
			std::this_thread::yield();
	}
	~State()
	{
		for (auto *source : sources)
			if (source)
				obs_source_release(source);
	}
};
using Holder = std::shared_ptr<State>;
inline Holder creating;

inline bool render(void *data, uint64_t *, obs_source_audio_mix *, uint32_t mixers, size_t channels, size_t rate)
{
	auto &s = **static_cast<Holder *>(data);
	if (!s.enter())
		return false;
	if (channels != 2 || rate != s.rate || !(mixers & 1) || s.count == packet_limit) {
		s.overflow = true;
		s.leave();
		return false;
	}
	auto &block = s.blocks[s.count++];
	for (size_t source = 0; source < source_count; ++source) {
		block.pending[source] = obs_source_audio_pending(s.sources[source]);
		block.timestamp[source] = obs_source_get_audio_timestamp(s.sources[source]);
		if (block.pending[source] || !block.timestamp[source])
			continue;
		obs_source_audio_mix mix{};
		obs_source_get_audio_mix(s.sources[source], &mix);
		for (size_t channel = 0; channel < 2; ++channel) {
			if (!mix.output[0].data[channel]) {
				s.overflow = true;
				continue;
			}
			std::memcpy(block.pcm[source][channel], mix.output[0].data[channel],
				    sizeof(block.pcm[source][channel]));
		}
	}
	s.leave();
	// No output timestamp or PCM: this observer never contributes to the mix.
	return false;
}

inline void raw_callback(void *data, size_t, audio_data *packet)
{
	auto &s = *static_cast<State *>(data);
	if (s.raw_count == packet_limit || packet->frames > AUDIO_OUTPUT_FRAMES) {
		s.raw_overflow = true;
		return;
	}
	auto &block = s.raw[s.raw_count++];
	block.timestamp = packet->timestamp;
	block.frames = packet->frames;
	block.render_index = s.count ? uint32_t(s.count - 1) : UINT32_MAX;
	for (size_t source = 0; source < source_count; ++source)
		block.pending[source] = obs_source_audio_pending(s.sources[source]);
	for (size_t channel = 0; channel < 2; ++channel)
		std::memcpy(block.pcm[channel], packet->data[channel], packet->frames * sizeof(float));
}

inline QJsonObject audit(const State &s, const QString &directory)
{
	QJsonObject result{{"render_packets", qint64(s.count)},
			   {"raw_packets", qint64(s.raw_count)},
			   {"rate", int(s.rate)},
			   {"overflow", s.overflow || s.raw_overflow},
			   {"source_count", int(source_count)},
			   {"bgm_sync_offset_ns", QString::number(37000000)},
			   {"bgm2_sync_offset_ns", QString::number(53000000)},
			   {"observer_returns_false", true}};
	if (!s.raw_count) {
		result.insert("passed", false);
		return result;
	}
	const uint64_t epoch = s.raw[0].timestamp;
	std::vector<float> expected, actual, bgm_category;
	expected.reserve(size_t(s.rate) * 16 * 2);
	actual.reserve(size_t(s.rate) * 16 * 2);
	bgm_category.reserve(size_t(s.rate) * 16 * 2);
	double maximum = 0, energy[source_count]{}, bgm_energy = 0, raw_energy = 0;
	size_t matched = 0, missing = 0, rejected[source_count]{};
	uint32_t previous = UINT32_MAX;
	QByteArray packet_csv("render_packet,source,timestamp_ns,pending\n");
	for (size_t p = 0; p < s.count; ++p)
		for (size_t source = 0; source < source_count; ++source)
			packet_csv += QByteArray::number(qulonglong(p)) + ',' + QByteArray::number(qulonglong(source)) +
				      ',' + QByteArray::number(qulonglong(s.blocks[p].timestamp[source])) + ',' +
				      QByteArray::number(s.blocks[p].pending[source]) + '\n';
	QByteArray raw_csv("timestamp_ns,frames\n");
	for (size_t p = 0; p < s.raw_count; ++p) {
		const auto &raw = s.raw[p];
		raw_csv += QByteArray::number(qulonglong(raw.timestamp)) + ',' + QByteArray::number(raw.frames) + '\n';
		const bool fresh = raw.render_index < s.count &&
				   (previous == UINT32_MAX || raw.render_index > previous);
		previous = raw.render_index;
		// Keep the same interior interval as the original experiment.
		const int64_t first = int64_t(std::llround(double(int64_t(raw.timestamp - epoch)) * s.rate / 1e9));
		if (!fresh) {
			missing += raw.frames;
			continue;
		}
		const auto &render = s.blocks[raw.render_index];
		bool known = true;
		std::array<int64_t, source_count> start;
		start.fill(-1);
		for (size_t source = 0; source < source_count; ++source) {
			if (raw.pending[source]) {
				++rejected[source];
				continue; // OBS did not include this source in this render cycle.
			}
			if (render.pending[source] || !render.timestamp[source]) {
				known = false;
				continue;
			}
			const auto ts = render.timestamp[source];
			// Match libobs mix_audio's supported timestamp interval.
			if (ts >= raw.timestamp && ts < raw.timestamp + uint64_t(raw.frames) * 1000000000ULL / s.rate)
				start[source] = int64_t((ts - raw.timestamp) * s.rate / 1000000000ULL);
		}
		for (uint32_t i = 0; i < raw.frames; ++i) {
			const int64_t index = first + i;
			if (index < int64_t(s.rate) || index >= int64_t(s.rate) * 7)
				continue;
			if (!known) {
				++missing;
				continue;
			}
			++matched;
			for (size_t channel = 0; channel < 2; ++channel) {
				float values[source_count]{};
				for (size_t source = 0; source < source_count; ++source)
					if (start[source] >= 0 && int64_t(i) >= start[source])
						values[source] = render.pcm[source][channel][i - size_t(start[source])];
				const float bgm = values[1] + values[2];
				const float sum = values[0] + bgm;
				const float value = raw.pcm[channel][i];
				maximum = std::max(maximum, double(std::abs(sum - value)));
				for (size_t source = 0; source < source_count; ++source)
					energy[source] += double(values[source]) * values[source];
				bgm_energy += double(bgm) * bgm;
				raw_energy += double(value) * value;
				expected.push_back(sum);
				actual.push_back(value);
				bgm_category.push_back(bgm);
			}
		}
	}
	result.insert("voice_pending_blocks", qint64(rejected[0]));
	result.insert("bgm_pending_blocks", qint64(rejected[1]));
	result.insert("bgm2_pending_blocks", qint64(rejected[2]));
	auto save = [&](const QString &name, const void *data, size_t bytes) {
		QFile f(directory + '/' + name);
		return f.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
		       f.write(static_cast<const char *>(data), qint64(bytes)) == qint64(bytes) && f.flush();
	};
	QByteArray states(
		"raw_packet,timestamp_ns,render_index,voice_render_pending,voice_timestamp,bgm_render_pending,bgm_timestamp,bgm2_render_pending,bgm2_timestamp,voice_raw_pending,bgm_raw_pending,bgm2_raw_pending\n");
	for (size_t p = 0; p < s.raw_count; ++p) {
		const auto &r = s.raw[p];
		if (r.render_index >= s.count)
			continue;
		const auto &b = s.blocks[r.render_index];
		states += QByteArray::number(qulonglong(p)) + ',' + QByteArray::number(qulonglong(r.timestamp)) + ',' +
			  QByteArray::number(r.render_index);
		for (size_t source = 0; source < source_count; ++source)
			states += ',' + QByteArray::number(b.pending[source]) + ',' +
				  QByteArray::number(qulonglong(b.timestamp[source]));
		for (size_t source = 0; source < source_count; ++source)
			states += ',' + QByteArray::number(r.pending[source]);
		states += '\n';
	}
	const bool saved = save("category-states.csv", states.constData(), size_t(states.size())) &&
			   save("category-render-blocks.bin", s.blocks.data(), s.count * sizeof(Block)) &&
			   save("category-raw-blocks.bin", s.raw.data(), s.raw_count * sizeof(RawBlock)) &&
			   save("category-render.csv", packet_csv.constData(), size_t(packet_csv.size())) &&
			   save("category-raw.csv", raw_csv.constData(), size_t(raw_csv.size())) &&
			   save("category-bgm.f32", bgm_category.data(), bgm_category.size() * sizeof(float)) &&
			   save("category-sum.f32", expected.data(), expected.size() * sizeof(float)) &&
			   save("category-raw.f32", actual.data(), actual.size() * sizeof(float));
	result.insert("matched_frames", qint64(matched));
	result.insert("missing_frames", qint64(missing));
	result.insert("max_absolute_error", maximum);
	result.insert("voice_energy", energy[0]);
	result.insert("bgm1_energy", energy[1]);
	result.insert("bgm2_energy", energy[2]);
	result.insert("bgm_energy", bgm_energy);
	result.insert("raw_energy", raw_energy);
	result.insert("category_aggregate", "voice=source[0]; bgm=source[1]+source[2]; expected=voice+bgm");
	result.insert("artifacts_saved", saved);
	result.insert("render_block_bytes", qint64(sizeof(Block)));
	result.insert("raw_block_bytes", qint64(sizeof(RawBlock)));
	result.insert("audio_output_frames", int(AUDIO_OUTPUT_FRAMES));
	result.insert(
		"binary_layout",
		"native little-endian: render uint64 timestamp[3], uint32 pending[3], float pcm[3][2][frames], 4 bytes trailing alignment padding; raw uint64 timestamp, uint32 frames, uint32 render_index, uint32 pending[3], float pcm[2][frames], 4 bytes trailing alignment padding");
	result.insert("passed", saved && !s.overflow && !s.raw_overflow && !missing && matched >= size_t(s.rate) * 3 &&
					maximum <= 1e-6 && energy[0] > 0 && energy[1] > 0 && energy[2] > 0 &&
					bgm_energy > 0);
	return result;
}
} // namespace category_probe

static void register_category_probe()
{
	obs_source_info info{};
	info.id = "lbm_category_observer";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_AUDIO;
	info.get_name = [](void *) {
		return "category feasibility observer";
	};
	info.create = [](obs_data_t *, obs_source_t *) -> void * {
		return new category_probe::Holder(category_probe::creating);
	};
	info.destroy = [](void *p) {
		delete static_cast<category_probe::Holder *>(p);
	};
	info.audio_render = category_probe::render;
	obs_register_source(&info);
}

static void start_category_probe(const QString &directory, std::function<void(bool, const QString &)> done)
{
	using namespace category_probe;
	auto state = std::make_shared<State>();
	state->rate = audio_output_get_sample_rate(obs_get_audio());
	state->sources[0] = obs_get_source_by_name("LBM baseline voice");
	state->sources[1] = obs_get_source_by_name("LBM baseline bgm");
	if (!state->sources[0] || !state->sources[1] || (state->rate != 44100 && state->rate != 48000)) {
		done(false, "category fixture sources or supported rate missing");
		return;
	}
	struct TreeCheck {
		State *state;
		bool seen[source_count]{};
	} check{state.get()};
	for (uint32_t channel = 0; channel < MAX_CHANNELS; ++channel) {
		auto *root = obs_get_output_source(channel);
		if (!root)
			continue;
		if (obs_source_active(root))
			obs_source_enum_active_tree(
				root,
				[](obs_source_t *, obs_source_t *source, void *p) {
					auto &c = *static_cast<TreeCheck *>(p);
					for (size_t i = 0; i < source_count; ++i)
						if (source == c.state->sources[i])
							c.seen[i] = true;
				},
				&check);
		obs_source_release(root);
	}
	if (!check.seen[0] || !check.seen[1]) {
		done(false, "category sources absent from active root tree");
		return;
	}
	auto *scene_source = obs_frontend_get_current_scene();
	auto *scene = scene_source ? obs_scene_from_source(scene_source) : nullptr;
	if (!scene) {
		if (scene_source)
			obs_source_release(scene_source);
		done(false, "category current scene missing");
		return;
	}
	state->sources[2] = obs_source_create("lbm_baseline_signal", "LBM baseline bgm 2", nullptr, nullptr);
	auto *bgm_item = state->sources[2] ? obs_scene_add(scene, state->sources[2]) : nullptr;
	if (bgm_item)
		obs_sceneitem_addref(bgm_item);
	obs_source_release(scene_source);
	if (!bgm_item) {
		if (state->sources[2])
			obs_source_remove(state->sources[2]);
		done(false, "category second BGM creation or attachment failed");
		return;
	}
	std::array<uint32_t, source_count> old_mixers{};
	std::array<int64_t, source_count> old_offsets{};
	std::array<bool, source_count> old_monitor{};
	constexpr int64_t offsets[source_count]{0, 37000000, 53000000};
	for (size_t i = 0; i < source_count; ++i) {
		old_mixers[i] = obs_source_get_audio_mixers(state->sources[i]);
		old_offsets[i] = obs_source_get_sync_offset(state->sources[i]);
		old_monitor[i] = obs_source_get_monitoring_enabled(state->sources[i]);
		obs_source_set_audio_mixers(state->sources[i], 1);
		obs_source_set_sync_offset(state->sources[i], offsets[i]);
		obs_source_set_monitoring_enabled(state->sources[i], false);
	}
	creating = state;
	auto *observer = obs_source_create_private("lbm_category_observer", "category observer", nullptr);
	creating.reset();
	audio_convert_info conversion{};
	conversion.samples_per_sec = state->rate;
	conversion.format = AUDIO_FORMAT_FLOAT_PLANAR;
	conversion.speakers = SPEAKERS_STEREO;
	conversion.allow_clipping = false;
	obs_add_raw_audio_callback(0, &conversion, raw_callback, state.get());
	auto *window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	auto *timer = new QTimer(window);
	const uint64_t started = os_gettime_ns();
	auto position = std::make_shared<uint64_t>(0);
	QObject::connect(timer, &QTimer::timeout, window, [=] {
		const uint32_t frames = state->rate / 100;
		std::array<float, 480> left{}, right{};
		for (size_t source = 0; source < source_count; ++source) {
			for (uint32_t i = 0; i < frames; ++i) {
				for (size_t channel = 0; channel < 2; ++channel) {
					uint64_t x = *position + i + (source * 2 + channel + 1) * 0x9E3779B97F4A7C15ULL;
					x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
					x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
					x ^= x >> 31;
					float value = float((x & 0x7fff) + 1) / 524288.0f;
					(channel ? right : left)[i] = x >> 63 ? -value : value;
				}
			}
			obs_source_audio data{};
			data.frames = frames;
			data.speakers = SPEAKERS_STEREO;
			data.samples_per_sec = state->rate;
			data.format = AUDIO_FORMAT_FLOAT_PLANAR;
			data.timestamp = started + *position * 1000000000ULL / state->rate;
			data.data[0] = reinterpret_cast<const uint8_t *>(left.data());
			data.data[1] = reinterpret_cast<const uint8_t *>(right.data());
			obs_source_output_audio(state->sources[source], &data);
		}
		*position += frames;
	});
	timer->start(10);
	QTimer::singleShot(9000, window, [=] {
		timer->stop();
		timer->deleteLater();
		state->close();
		std::thread detach([=] { obs_remove_raw_audio_callback(0, raw_callback, state.get()); });
		detach.join();
		if (observer) {
			obs_source_remove(observer);
			obs_source_release(observer);
		}
		for (size_t i = 0; i < source_count; ++i) {
			obs_source_set_audio_mixers(state->sources[i], old_mixers[i]);
			obs_source_set_sync_offset(state->sources[i], old_offsets[i]);
			obs_source_set_monitoring_enabled(state->sources[i], old_monitor[i]);
		}
		obs_sceneitem_remove(bgm_item);
		obs_sceneitem_release(bgm_item);
		obs_source_remove(state->sources[2]);
		auto result = audit(*state, directory);
		if (!observer)
			result.insert("passed", false);
		QFile file(directory + "/category-result.json");
		const auto bytes = QJsonDocument(result).toJson();
		const bool saved = file.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
				   file.write(bytes) == bytes.size() && file.flush();
		done(saved && result.value("passed").toBool(), "category feasibility artifacts: category-result.json");
	});
}
