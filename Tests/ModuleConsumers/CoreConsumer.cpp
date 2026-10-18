#include "Core/BoundedInput.h"
#include "Core/PlatformServices.h"
#include "Scene/UUID.h"

#include <sstream>
#include <string_view>

int main()
{
	const auto ExecutablePath = PulseForge::Platform::GetExecutablePath();
	if (!ExecutablePath || ExecutablePath->empty()) return 1;

	const auto UserDataDirectory = PulseForge::Platform::GetUserDataDirectory();
	if (!UserDataDirectory || UserDataDirectory->empty()) return 2;

	std::istringstream Input("core consumer");
	const auto Read = PulseForge::Detail::ReadBoundedInput(Input, 32);
	if (!Read || *Read != "core consumer") return 3;

	constexpr std::string_view KnownUUID = "01234567-89ab-cdef-0123-456789abcdef";
	const auto ParsedUUID = PulseForge::UUID::Parse(KnownUUID);
	if (!ParsedUUID || ParsedUUID->ToString() != KnownUUID) return 4;

	const auto GeneratedUUID = PulseForge::UUID::Generate();
	if (!GeneratedUUID || GeneratedUUID->IsNil()) return 5;

	const auto RoundTripUUID = PulseForge::UUID::Parse(GeneratedUUID->ToString());
	if (!RoundTripUUID || *RoundTripUUID != *GeneratedUUID) return 6;

	return 0;
}
