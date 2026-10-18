#pragma once

#define PULSEFORGE_API

#define BIT(x) (1 << x)

#ifdef PF_DEBUG
	#define PF_ENABLE_ASSERTS
#endif

#ifdef PF_ENABLE_ASSERTS
	namespace PulseForge::Platform
	{
		void DebugBreak() noexcept;
	}

	#define PF_CORE_ASSERT(x, ...)\
	{ \
		if(!(x)) \
		{ \
			PF_CORE_CRITICAL("Assertion failed: {0}", __VA_ARGS__); \
			::PulseForge::Platform::DebugBreak(); \
		} \
	}

	#define PF_ASSERT(x, ...) \
	{ \
		if(!(x)) \
		{ \
			PF_CRITICAL("Assertion failed: {0}", __VA_ARGS__); \
		} \
	}
#else
	#define PF_CORE_ASSERT(x, ...)
	#define PF_ASSERT(x, ...)
#endif
