set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
add_subdirectory(PulseForge/Vendor/spdlog)
target_compile_options(spdlog PRIVATE
	$<$<CXX_COMPILER_ID:MSVC>:/EHsc>
)
