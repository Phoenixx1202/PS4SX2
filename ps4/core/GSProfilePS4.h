// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) GS-owned wall-time measurements; nested scopes are inclusive.
#pragma once

#if defined(PCSX2_PS4_VM_BOOT)
#include "common/Pcsx2Defs.h"
#include "common/Timer.h"

namespace PS4GSProfile
{
	// First nine values match VKOrbisTiming's existing Vulkan call sites.
	enum Kind
	{
		ReuseFence,
		ExplicitWait,
		FenceCounter,
		Acquire,
		Submit,
		Present,
		Idle,
		Pipeline,
		Shader,
		HWDraw,
		VKRender,
		TextureUpdate,
		Readback,
		SWSprite,
		TextureSourceLookup,
		DepthTargetLookup,
		ColorTargetLookup,
		DrawPrims,
		Count
	};
	struct Counter
	{
		u64 calls = 0, samples = 0;
		Common::Timer::Value elapsed = 0, maximum = 0;
		u32 random = 0x47535035;
	};
	inline Counter counters[Count];
	inline bool enabled = false; // Set before opening GS; thereafter GS is sole owner.
	class Scope
	{
		Counter* m_counter = nullptr;
		Common::Timer::Value m_start = 0;

	public:
		explicit Scope(Kind kind)
		{
			if (!enabled)
				return;
			auto& c = counters[kind];
			++c.calls;
			if (kind == HWDraw || kind == VKRender || kind == TextureUpdate || kind == SWSprite ||
				kind == TextureSourceLookup || kind == DepthTargetLookup || kind == ColorTargetLookup || kind == DrawPrims)
			{
				c.random = c.random * 1664525U + 1013904223U;
				if ((c.random >> 26) != 0) // Time roughly 1/64 hot calls.
					return;
			}
			m_counter = &c;
			m_start = Common::Timer::GetCurrentValue();
		}
		~Scope()
		{
			if (!m_counter)
				return;
			const auto duration = Common::Timer::GetCurrentValue() - m_start;
			++m_counter->samples;
			m_counter->elapsed += duration;
			if (duration > m_counter->maximum)
				m_counter->maximum = duration;
		}
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;
	};
	void LogInterval(double seconds);
} // namespace PS4GSProfile
#define PS4_GS_SCOPE(kind) PS4GSProfile::Scope ps4_gs_scope_(PS4GSProfile::kind)
#else
#define PS4_GS_SCOPE(kind) \
	do \
	{ \
	} while (0)
#endif
