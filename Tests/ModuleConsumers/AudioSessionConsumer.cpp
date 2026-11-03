#include "Audio/AudioSession.h"
#include "Runtime/PlayerSession.h"

int main()
{
	PulseForge::AudioSessionDesc Description;
	Description.Engine.OutputBackend = PulseForge::AudioOutputBackend::Offline;
	const auto Session = PulseForge::AudioSession::CreateForCooked({}, {}, Description);
	if (Session)
		return 1;
	return 0;
}
