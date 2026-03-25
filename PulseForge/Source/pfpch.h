#pragma once

// Target OS // Windows Specific
#ifdef PF_PLATFORM_WINDOWS
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <Windows.h>
#endif


// Utility & Memory
#include <memory>
#include <utility>
#include <algorithm>
#include <functional>

// Data Structures
#include <string>
#include <sstream>
#include <array>
#include <vector>
#include <unordered_map>
#include <unordered_set>

// Engine Core
#include "Core/Log.h"
