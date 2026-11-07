#pragma once
// Independent reference: OBS mixes selected voice and BGMs onto separate tracks.
// Never reuse the product's category observer or its analysis implementation here.
struct NumericTrack {
	std::vector<float> pcm;
	std::vector<EvidencePacket> packets;
	size_t frames = 0;
	bool overflow = false;
};
static NumericTrack numeric_tracks[2];
static bool numeric_attached = false;
static void numeric_audio(void *opaque, size_t, audio_data *packet)
{
	auto &track = *static_cast<NumericTrack *>(opaque);
	if (track.frames + packet->frames > track.pcm.size() / 2 || track.packets.size() == track.packets.capacity()) {
		track.overflow = true;
		return;
	}
	track.packets.push_back({packet->timestamp, track.frames, packet->frames});
	for (uint32_t i = 0; i < packet->frames; ++i) {
		track.pcm[(track.frames + i) * 2] = reinterpret_cast<const float *>(packet->data[0])[i];
		track.pcm[(track.frames + i) * 2 + 1] = reinterpret_cast<const float *>(packet->data[1])[i];
	}
	track.frames += packet->frames;
}
static void numeric_stop()
{
	if (!numeric_attached)
		return;
	std::thread remover([] {
		for (size_t index = 0; index < 2; ++index)
			obs_remove_raw_audio_callback(index + 1, numeric_audio, &numeric_tracks[index]);
	});
	remover.join();
	numeric_attached = false;
}
static bool numeric_begin()
{
	if (!preview_check(numeric_bgm && evidence_rate == 48000, "numeric fixture unavailable"))
		return false;
	obs_source_set_audio_mixers(voice, 3);
	obs_source_set_audio_mixers(bgm, 5);
	obs_source_set_audio_mixers(numeric_bgm, 5);
	audio_convert_info conversion{};
	conversion.samples_per_sec = evidence_rate;
	conversion.format = AUDIO_FORMAT_FLOAT_PLANAR;
	conversion.speakers = SPEAKERS_STEREO;
	conversion.allow_clipping = false;
	for (size_t index = 0; index < 2; ++index) {
		auto &track = numeric_tracks[index];
		track.pcm.resize(size_t(evidence_rate) * 40 * 2);
		track.packets.reserve(4000);
		obs_add_raw_audio_callback(index + 1, &conversion, numeric_audio, &track);
	}
	numeric_attached = true;
	return true;
}
static bool numeric_save()
{
	numeric_stop();
	auto *session = dock->findChild<QObject *>("previewSession");
	const auto first = session->property("recordedTimestamp").toULongLong();
	const auto count = session->property("recordedFrames").toULongLong();
	const auto snapshot = QJsonObject::fromVariantMap(session->property("recordedAnalysis").toMap());
	if (!preview_check(!snapshot.isEmpty(), "published analysis snapshot unavailable"))
		return false;
	const auto json = QJsonDocument(snapshot).toJson();
	if (!preview_check(save_bytes(result_dir + "/numeric-product.json", json.constData(), json.size()),
			   "analysis snapshot write failed"))
		return false;
	for (size_t index = 0; index < 2; ++index) {
		auto &track = numeric_tracks[index];
		size_t begin = track.frames;
		uint64_t previous = 0;
		uint32_t previous_frames = 0;
		for (const auto &packet : track.packets) {
			if (packet.timestamp == first)
				begin = packet.first;
			if (begin == track.frames || packet.first >= begin + count)
				continue;
			if (previous) {
				const uint64_t expected =
					previous + uint64_t(previous_frames) * 1000000000ULL / evidence_rate;
				const auto delta = expected > packet.timestamp ? expected - packet.timestamp
									       : packet.timestamp - expected;
				if (!preview_check(delta <= 1, "independent category timestamp discontinuity"))
					return false;
			}
			previous = packet.timestamp;
			previous_frames = packet.frames;
		}
		if (!preview_check(!track.overflow && begin < track.frames && count <= track.frames - begin,
				   "independent category interval unavailable"))
			return false;
		const QString name = index ? "bgm" : "voice";
		if (!preview_check(save_bytes(result_dir + "/numeric-" + name + ".f32", track.pcm.data() + begin * 2,
					      count * 8),
				   "independent category PCM write failed"))
			return false;
	}
	observations.insert("numeric_reference", "OBS raw tracks 2/3; pending independent numeric audit");
	return true;
}
