if(BUILD_TESTING)
	add_executable(PulseForgeCoreTests
		Tests/PulseForgeTests.cpp
		Tests/CoreTests.cpp
	)
	target_include_directories(PulseForgeCoreTests PRIVATE Tests)
	target_link_libraries(PulseForgeCoreTests PRIVATE PulseForgeCore)
	if(TARGET PulseForgePublicHeaderChecks)
		add_dependencies(PulseForgeCoreTests PulseForgePublicHeaderChecks)
	endif()
	add_test(NAME PulseForge.Core COMMAND PulseForgeCoreTests)

	add_executable(PulseForgeCoreConsumer Tests/ModuleConsumers/CoreConsumer.cpp)
	set_property(TARGET PulseForgeCoreConsumer PROPERTY DISABLE_PRECOMPILE_HEADERS ON)
	target_link_libraries(PulseForgeCoreConsumer PRIVATE PulseForgeCore)
	add_test(NAME PulseForge.CoreConsumer COMMAND PulseForgeCoreConsumer)

	if(PULSEFORGE_BUILD_CORE_ONLY)
		add_custom_target(PulseForgeTests)
		add_dependencies(PulseForgeTests PulseForgeCoreTests PulseForgeCoreConsumer)
	endif()
endif()
