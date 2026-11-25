#include "../Client.h"
#include "Game.h"
#include "Hooks.h"
#include "Hooks/HookDomains.h"
#include "Pickups.h"
#include "RemoveBuilding.h"

void Hooks::install()
{
	spdlog::info("Installing hooks...");

	InstallStreamingHooks();
	InstallLifecycleHooks();
	InstallInputHooks();
	InstallRenderHooks();
	InstallWorldHooks();

	Pickups::installHooks();
	RemoveBuilding::installHooks();

	installPadHooks();
	installWidgetFixHooks();
	installCrosshairFixHooks();
}
