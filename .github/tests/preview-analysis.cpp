#include "preview-analysis.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

constexpr uint32_t kRate = 48000;
constexpr uint32_t kFrames = kRate * 6;

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

bool close(double first, double second)
{
	return first == second || (std::isfinite(first) && std::isfinite(second) && std::abs(first - second) < 1e-6);
}

void feed(lbm::PreviewAnalysis &analysis, const std::vector<float> &voice, const std::vector<float> &bgm,
	  uint32_t frames, uint32_t block)
{
	for (uint32_t offset = 0; offset < frames;) {
		const uint32_t count = std::min(block, frames - offset);
		require(analysis.append(voice.data() + offset * 2, bgm.data() + offset * 2, count), "append failed");
		offset += count;
	}
}

void require_unrated(const lbm::PreviewAnalysis &analysis)
{
	const auto summary = analysis.summary();
	require(summary.valid_points == 0 && !summary.balance_status, "invalid recording received a rating");
	require(!std::isfinite(summary.voice_lufs) && !std::isfinite(summary.bgm_lufs), "invalid mean became finite");
	for (const auto &point : analysis.series())
		require(!point.balance_status, "invalid observation received a rating");
}

} // namespace

int main()
{
	try {
		std::vector<float> voice(kFrames * 2);
		std::vector<float> bgm(kFrames * 2);
		std::vector<float> silence(kFrames * 2, 0.0f);
		for (uint32_t i = 0; i < kFrames; ++i) {
			const float sample =
				static_cast<float>(0.1 * std::sin(2.0 * 3.14159265358979323846 * 1000 * i / kRate));
			// Opposite channels also exercise VAD without mono phase cancellation.
			voice[i * 2] = sample;
			voice[i * 2 + 1] = -sample;
			bgm[i * 2] = sample * 0.5f;
			bgm[i * 2 + 1] = -sample * 0.5f;
		}

		lbm::PreviewAnalysis analysis;
		for (const uint32_t rate : {44100u, 48000u}) {
			const uint32_t boundary = rate * 2 / 5;
			require(analysis.reset(rate, -40.0, 6.0, 0.5), "reset failed");
			feed(analysis, voice, bgm, boundary - 1, 997);
			require_unrated(analysis);
			for (const auto &point : analysis.series())
				require(!std::isfinite(point.voice_lufs) && !std::isfinite(point.bgm_lufs),
					"incomplete window has LUFS");
			require(analysis.append(voice.data() + (boundary - 1) * 2, bgm.data() + (boundary - 1) * 2, 1),
				"boundary append failed");
			require(std::isfinite(analysis.series().back().voice_lufs) &&
					analysis.series().back().balance_status.has_value(),
				"400 ms window did not produce a dynamic value");
			require(std::isfinite(analysis.series().back().mix_lufs) && analysis.series().back().mix_status,
				"400 ms mix window did not produce a value");
			require(analysis.summary().valid_points == 0, "short recording produced a 3 second average");
			require(analysis.series().back().frame_position == boundary,
				"first value has wrong recording position");
			const uint32_t average_boundary = rate * 3;
			require(analysis.append(voice.data() + boundary * 2, bgm.data() + boundary * 2,
						average_boundary - boundary - 1),
				"pre-average append failed");
			require(analysis.summary().valid_points == 0, "incomplete Short-term window entered average");
			require(analysis.append(voice.data() + (average_boundary - 1) * 2,
						bgm.data() + (average_boundary - 1) * 2, 1),
				"average boundary append failed");
			require(analysis.summary().valid_points == 1 && analysis.summary().mix_valid_points == 1,
				"3 second window did not produce first average point");

			// A brief peak and an incomplete final interval must not disappear behind VAD or LUFS gating.
			const uint32_t interval = rate / 10;
			std::vector<float> peak_voice((interval * 2 + 1) * 2, 0.0f);
			std::vector<float> peak_bgm(peak_voice.size(), 0.0f);
			peak_voice[0] = 0.75f;
			peak_bgm[0] = 0.75f;
			peak_voice[peak_voice.size() - 1] = 1.75f;
			require(analysis.reset(rate, 0.0, 6.0, 0.5), "peak reset failed");
			feed(analysis, peak_voice, peak_bgm, interval * 2 + 1, 997);
			const auto peaks = analysis.summary();
			require(peaks.frames == interval * 2 + 1 && peaks.valid_points == 0 &&
					peaks.clip_status == lbm::Status::BAD &&
					close(peaks.voice_peak_dbfs, 20.0 * std::log10(1.75)) &&
					close(peaks.bgm_peak_dbfs, 20.0 * std::log10(0.75)) &&
					close(peaks.mix_peak_dbfs, 20.0 * std::log10(1.75)),
				"short recording lost an ungated tail peak");
			require(analysis.series().size() == 2 && !analysis.series()[0].voice_active &&
					analysis.series()[0].clip_status == lbm::Status::BAD &&
					close(analysis.series()[0].mix_peak_dbfs, 20.0 * std::log10(1.5)) &&
					analysis.series()[1].clip_status == lbm::Status::OK &&
					!std::isfinite(analysis.series()[1].mix_peak_dbfs),
				"interval peaks were clipped, gated, or retained");
		}

		require(analysis.reset(kRate, -40.0, 6.0, 0.5), "silence reset failed");
		feed(analysis, silence, silence, kFrames, 997);
		require_unrated(analysis);
		require(analysis.summary().frames == kFrames && analysis.summary().clip_status == lbm::Status::OK &&
				!std::isfinite(analysis.summary().mix_peak_dbfs),
			"silence peak validity lost");

		const float warning = static_cast<float>(std::pow(10.0, -1.0 / 20.0));
		const float peak_levels[] = {std::nextafter(warning, 0.0f), std::nextafter(warning, 1.0f), 1.0f};
		const lbm::Status peak_ratings[] = {lbm::Status::OK, lbm::Status::WARN, lbm::Status::BAD};
		for (size_t i = 0; i < 3; ++i) {
			std::vector<float> pulse(kRate / 10 * 2, 0.0f);
			pulse[17] = peak_levels[i];
			require(analysis.reset(kRate, 0.0, 6.0, 0.5), "clip boundary reset failed");
			require(analysis.append(pulse.data(), silence.data(), kRate / 10),
				"clip boundary append failed");
			require(analysis.summary().clip_status == peak_ratings[i] &&
					analysis.series().back().clip_status == peak_ratings[i],
				"clip threshold missed a short peak");
		}

		for (bool silent_voice : {false, true}) {
			require(analysis.reset(kRate, -40.0, 6.0, 0.5), "unpaired reset failed");
			feed(analysis, silent_voice ? silence : voice, silent_voice ? bgm : silence, kFrames, 997);
			require_unrated(analysis);
		}
		const double expected_delta = 20.0 * std::log10(2.0);
		const lbm::Status colors[] = {lbm::Status::OK, lbm::Status::WARN, lbm::Status::BAD};
		for (int category = 0; category < 3; ++category) {
			require(analysis.reset(kRate, -40.0, 6.0 + category * 2.0, 0.5), "tone reset failed");
			feed(analysis, voice, bgm, kFrames, kFrames);
			const auto summary = analysis.summary();
			require(summary.valid_points == 31, "unexpected valid sample interval");
			require(summary.balance_status == colors[category], "wrong fixed basic rating");
			require(close(summary.voice_lufs - summary.bgm_lufs, expected_delta),
				"wrong mean LUFS difference");
			require(summary.mix_valid_points == summary.valid_points &&
					close(summary.mix_lufs - summary.voice_lufs, 20.0 * std::log10(1.5)),
				"wrong summed mix average");

			for (const auto &point : analysis.series()) {
				if (!point.balance_status)
					continue;
				require(point.voice_active, "opposite channels lost VAD activity");
				require(point.balance_status == colors[category],
					"wrong initial or sustained dynamic rating");
				require(close(point.voice_lufs - point.bgm_lufs, expected_delta),
					"wrong instantaneous LUFS difference");
			}
		}

		lbm::PreviewAnalysis split;
		require(split.reset(kRate, -40.0, 10.0, 0.5), "split reset failed");
		feed(split, voice, bgm, kFrames, 997);
		require(split.series().size() == analysis.series().size(), "block division changed point count");
		for (size_t i = 0; i < analysis.series().size(); ++i) {
			const auto &a = analysis.series()[i];
			const auto &b = split.series()[i];
			require(a.frame_position == b.frame_position && a.voice_active == b.voice_active &&
					a.balance_status == b.balance_status && a.mix_status == b.mix_status &&
					a.clip_status == b.clip_status && close(a.voice_lufs, b.voice_lufs) &&
					close(a.bgm_lufs, b.bgm_lufs) && close(a.mix_lufs, b.mix_lufs) &&
					close(a.voice_peak_dbfs, b.voice_peak_dbfs) &&
					close(a.bgm_peak_dbfs, b.bgm_peak_dbfs) &&
					close(a.mix_peak_dbfs, b.mix_peak_dbfs),
				"block division changed analysis");
		}

		// Cancelling the mix must not remove the paired voice/BGM average.
		std::vector<float> inverse(voice.size());
		std::transform(voice.begin(), voice.end(), inverse.begin(), [](float value) { return -value; });
		require(analysis.reset(kRate, -40.0, 6.0, 0.5), "inverse reset failed");
		feed(analysis, voice, inverse, kFrames, 997);
		const auto cancelled = analysis.summary();
		require(cancelled.valid_points == 31 && cancelled.balance_status &&
				close(cancelled.voice_lufs, cancelled.bgm_lufs) && cancelled.mix_valid_points == 0 &&
				!std::isfinite(cancelled.mix_lufs) && !cancelled.mix_status &&
				!std::isfinite(cancelled.mix_peak_dbfs),
			"silent mix corrupted paired averages");
		for (const auto &point : analysis.series())
			require(!std::isfinite(point.mix_lufs) && !point.mix_status,
				"cancelled mix became a rated signal");

		// The recorded mix thresholds must affect both the dynamic and whole-recording rating.
		const double mix_lufs = split.summary().mix_lufs;
		for (int category = 0; category < 3; ++category) {
			const double ok = mix_lufs - 1.0 + category * 3.0;
			require(analysis.reset(kRate, -40.0, 6.0, 0.5, ok, ok - 4.0), "mix threshold reset failed");
			feed(analysis, voice, bgm, kFrames, 997);
			require(analysis.summary().mix_status == colors[category] &&
					analysis.series().back().mix_status == colors[category],
				"mix setting did not apply to both ratings");
		}

		require(analysis.reset(kRate, -40.0, 6.0, 0.5), "final reset failed");
		require(analysis.series().empty() && analysis.processed_frames() == 0 && !analysis.point_at(kFrames),
			"reset retained old playback observations");
		require(analysis.summary().frames == 0 && !analysis.summary().clip_status &&
				!analysis.summary().mix_status,
			"reset retained whole-recording peak state");
		require_unrated(analysis);
		feed(analysis, silence, silence, kFrames, 997);
		require_unrated(analysis);
		for (const auto &point : analysis.series())
			require(!point.voice_active && !std::isfinite(point.voice_lufs) &&
					!std::isfinite(point.bgm_lufs),
				"reset retained VAD or loudness state");
		std::cout << "preview-analysis: passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "preview-analysis: " << error.what() << '\n';
		return 1;
	}
}
