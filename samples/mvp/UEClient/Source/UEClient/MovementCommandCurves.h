#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <unordered_map>

namespace atlas::mvp::movement_curves
{
inline constexpr std::size_t kMaxSamples = 64;

struct Curve
{
	uint16_t sample_count = 0;
	std::array<float, kMaxSamples> samples{};
};

inline std::unordered_map<uint16_t, Curve>& Curves()
{
	static std::unordered_map<uint16_t, Curve> curves;
	return curves;
}

inline bool Register(uint16_t curve_id, std::initializer_list<float> samples)
{
	if (samples.size() == 0 || samples.size() > kMaxSamples) return false;

	Curve curve;
	curve.sample_count = static_cast<uint16_t>(samples.size());
	std::size_t index = 0;
	for (float sample : samples)
	{
		if (!std::isfinite(sample)) return false;
		curve.samples[index++] = sample;
	}

	Curves()[curve_id] = curve;
	return true;
}

inline void EnsureDefaults()
{
	static const bool registered = [] {
		Register(0, {0.0f, 1.0f});
		Register(1, {0.0f, 1.0f});
		return true;
	}();
	(void)registered;
}

inline float Linear(float normalized_time)
{
	if (!std::isfinite(normalized_time)) return 0.0f;
	return std::clamp(normalized_time, 0.0f, 1.0f);
}

inline float Sample(const Curve& curve, float normalized_time)
{
	if (!std::isfinite(normalized_time)) return 0.0f;
	if (curve.sample_count == 1) return curve.samples[0];

	const float time = std::clamp(normalized_time, 0.0f, 1.0f);
	const float scaled = time * static_cast<float>(curve.sample_count - 1);
	const std::size_t index = static_cast<std::size_t>(std::floor(scaled));
	if (index + 1 >= curve.sample_count) return curve.samples[curve.sample_count - 1];

	const float fraction = scaled - static_cast<float>(index);
	return curve.samples[index] * (1.0f - fraction) + curve.samples[index + 1] * fraction;
}

inline float Sample(uint16_t curve_id, float normalized_time)
{
	EnsureDefaults();
	const auto& curves = Curves();
	const auto it = curves.find(curve_id);
	return it == curves.end() ? Linear(normalized_time) : Sample(it->second, normalized_time);
}

inline float TimeAtProgress(uint16_t curve_id, float progress)
{
	if (!std::isfinite(progress)) return 0.0f;
	progress = std::clamp(progress, 0.0f, 1.0f);
	EnsureDefaults();
	const auto& curves = Curves();
	const auto it = curves.find(curve_id);
	if (it == curves.end() || it->second.sample_count <= 1) return progress;

	const Curve& curve = it->second;
	float best_time = 0.0f;
	float best_error = std::abs(curve.samples[0] - progress);
	for (std::size_t index = 0; index + 1 < curve.sample_count; ++index)
	{
		const float a = curve.samples[index];
		const float b = curve.samples[index + 1];
		const float segment_min = std::min(a, b);
		const float segment_max = std::max(a, b);
		if (progress >= segment_min && progress <= segment_max && std::abs(b - a) > 0.0001f)
		{
			const float fraction = (progress - a) / (b - a);
			return (static_cast<float>(index) + fraction) /
				static_cast<float>(curve.sample_count - 1);
		}

		const float error = std::abs(b - progress);
		if (error < best_error)
		{
			best_error = error;
			best_time = static_cast<float>(index + 1) /
				static_cast<float>(curve.sample_count - 1);
		}
	}
	return std::clamp(best_time, 0.0f, 1.0f);
}
}
