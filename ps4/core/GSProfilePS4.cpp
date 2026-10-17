// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Called and reset on GS, never read concurrently by the CPU ticker.
#include "GSProfilePS4.h"
#include "GS/GSPerfMon.h"

extern void ProbeLog(const char* format, ...);

void PS4GSProfile::LogInterval(double seconds)
{
	if (!enabled)
		return;
	ProbeLog("GS workload: averages_per_emulated_frame prim=%.0f draws=%.0f draw_calls=%.0f "
			 "readbacks=%.1f uploads=%.1f copies=%.1f barriers=%.1f render_passes=%.1f\n",
		g_perfmon.Get(GSPerfMon::Prim), g_perfmon.Get(GSPerfMon::Draw),
		g_perfmon.Get(GSPerfMon::DrawCalls), g_perfmon.Get(GSPerfMon::Readbacks),
		g_perfmon.Get(GSPerfMon::TextureUploads), g_perfmon.Get(GSPerfMon::TextureCopies),
		g_perfmon.Get(GSPerfMon::Barriers), g_perfmon.Get(GSPerfMon::RenderPasses));
	constexpr const char* names[] = {"reuse_fence", "explicit_wait", "fence_counter", "acquire",
		"submit", "present", "idle", "pipeline", "shader", "hw_draw", "vk_render",
		"texture_update", "readback", "sw_sprite", "texture_source_lookup", "depth_target_lookup",
		"color_target_lookup", "draw_prims"};
	static_assert(std::size(names) == Count);
	for (unsigned i = 0; i < Count; i++)
	{
		auto& c = counters[i];
		if (c.calls)
		{
			// Raw sampled wall time only: do not sum nested scopes or label it CPU usage.
			// Short-call clock overhead and sampling variance need proper console testing.
			ProbeLog("GS timing: interval=%.3f scope=%s calls=%llu samples=%llu sampled_wall_ms=%.3f "
					 "sample_max_ms=%.3f inclusive=yes\n",
				seconds, names[i],
				static_cast<unsigned long long>(c.calls), static_cast<unsigned long long>(c.samples),
				Common::Timer::ConvertValueToMilliseconds(c.elapsed),
				Common::Timer::ConvertValueToMilliseconds(c.maximum));
		}
		c.calls = c.samples = 0;
		c.elapsed = c.maximum = 0;
	}
}
