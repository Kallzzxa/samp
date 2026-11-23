#include "Game.h"
#include "../Client.h"
#include "../Settings.h"
#include "VehicleColoursTable.h"

#include <sys/stat.h>

sa::CPlayerInfo* WORLD_PLAYERS = nullptr;

bool Patches::hasFullAudioData()
{
	static int s_cached = -1;
	if (s_cached != -1) {
		return s_cached == 1;
	}
	const std::string dir = Client::gameDir();
	if (dir.empty()) {
		s_cached = 0;
		return false;
	}
	// Check both lowercase (Android) and uppercase (PC-style) audio bank paths.
	// Stripped 1.04GB data packs omit all .osw banks; leaving the audio engine
	// enabled without them crashes in CAEMP3TrackLoader::GetDataStream on spawn.
	const char* probes[] = {
		"audio/sfx/GENRL.osw",
		"audio/SFX/GENRL.osw",
		"audio/sfx/genrl.osw",
		"audio/sfx/FEET.osw",
		"audio/SFX/FEET.osw",
	};
	struct stat st{};
	for (const char* rel : probes) {
		const std::string full = (dir.back() == '/') ? (dir + rel) : (dir + "/" + rel);
		if (stat(full.c_str(), &st) == 0 && st.st_size > 0) {
			s_cached = 1;
			return true;
		}
	}
	s_cached = 0;
	return false;
}

void Patches::preApply()
{
	spdlog::info("Applying FPS patches...");

	// fps limit from Settings (0 = unlimited -> 250 FPS cap in engine)
	Settings::applyFpsLimitPatch();

	// show fps
	//*(bool*) (g_saSym->GetSymbol<uintptr_t>("FrontEndMenuManager") + (VER_x32 ? 0xB5 : 0xC1)) = true;

	// Set max stats
	sa::CCheat::WeaponSkillsCheat();
	sa::CCheat::VehicleSkillsCheat();
	sa::CCheat::StaminaCheat();
	patches::set("CStats::SetStatValue", true);
}

void Patches::allocPlayers()
{
	spdlog::info("Allocating world players...");

	// relocate CWorld::Players[] using true ABI sizeof(sa::CPlayerInfo)
	// (404B on arm32, 472B on arm64)
	const size_t totalSize = sizeof(sa::CPlayerInfo) * PLAYER_PED_SLOTS;
	WORLD_PLAYERS = static_cast<sa::CPlayerInfo*>(calloc(PLAYER_PED_SLOTS, sizeof(sa::CPlayerInfo)));
	if (!WORLD_PLAYERS) {
		spdlog::error("Failed to allocate WORLD_PLAYERS ({} bytes)", totalSize);
		return;
	}

	Memory::protectAddr(g_saSym->Abs(addr::_ZN6CWorld7PlayersE_ptr));
	*g_saSym->Abs<sa::CPlayerInfo**>(addr::_ZN6CWorld7PlayersE_ptr) = WORLD_PLAYERS;
	patches::registerData("CWorld::Players", "Relocate CWorld::Players[] for multiplayer slots",
		patches::Scope::Boot, g_saSym->Abs<uintptr_t>(addr::_ZN6CWorld7PlayersE_ptr));

	spdlog::info("CWorld::Players new address: 0x{:X}, old address: 0x{:X}, size: {} ({} x {})",
		(uintptr_t) WORLD_PLAYERS, (uintptr_t) eng::ptr(eng::Sym::CWorld_Players),
		totalSize, PLAYER_PED_SLOTS, sizeof(sa::CPlayerInfo));
}
void Patches::initBetterPoolsSize()
{
	spdlog::info("Initializing better pools size...");

	typedef sa::CPool<void> CPool;
	auto setPool = [](eng::Sym sym, int count, int elemSize) {
		if (auto** p = eng::as<CPool**>(sym)) {
			*p = new CPool(count, elemSize);
		}
	};

#if VER_x32
	setPool(eng::Sym::CPools_ms_pPtrNodeSingleLinkPool, 100000, 8);
	setPool(eng::Sym::CPools_ms_pPtrNodeDoubleLinkPool, 20000, 12);
	setPool(eng::Sym::CPools_ms_pEntryInfoNodePool,     20000, 20);
	setPool(eng::Sym::CPools_ms_pPedPool,               240,   1996);
	setPool(eng::Sym::CPools_ms_pVehiclePool,           2000,  2604);
	setPool(eng::Sym::CPools_ms_pBuildingPool,          20000, 60);
	setPool(eng::Sym::CPools_ms_pObjectPool,            3000,  420);
	setPool(eng::Sym::CPools_ms_pDummyPool,             40000, 60);
	setPool(eng::Sym::CPools_ms_pColModelPool,          50000, 48);
	setPool(eng::Sym::CPools_ms_pTaskPool,              5000,  128);
	setPool(eng::Sym::CPools_ms_pEventPool,             1000,  68);
	setPool(eng::Sym::CPools_ms_pPointRoutePool,        200,   100);
	setPool(eng::Sym::CPools_ms_pPatrolRoutePool,       200,   420);
	setPool(eng::Sym::CPools_ms_pNodeRoutePool,         200,   36);
	setPool(eng::Sym::CPools_ms_pTaskAllocatorPool,     3000,  32);
	setPool(eng::Sym::CPools_ms_pPedIntelligencePool,   240,   664);
	setPool(eng::Sym::CPools_ms_pPedAttractorPool,      200,   236);
#else
	setPool(eng::Sym::CPools_ms_pPtrNodeSingleLinkPool, 100000, 16);   // 1200000/75000 = 16
	setPool(eng::Sym::CPools_ms_pPtrNodeDoubleLinkPool, 20000,  24);   // 144000/6000 = 24
	setPool(eng::Sym::CPools_ms_pEntryInfoNodePool,     20000,  40);   // 20000/500 = 40
	setPool(eng::Sym::CPools_ms_pPedPool,               240,    2504); // 350560/140 = 2504
	setPool(eng::Sym::CPools_ms_pVehiclePool,           2000,   3176); // 349360/110 = 3176
	setPool(eng::Sym::CPools_ms_pBuildingPool,          20000,  96);   // 1344000/14000 = 96
	setPool(eng::Sym::CPools_ms_pObjectPool,            3000,   544);  // 190400/350 = 544
	setPool(eng::Sym::CPools_ms_pDummyPool,             40000,  96);   // 336000/3500 = 96
	setPool(eng::Sym::CPools_ms_pColModelPool,          50000,  56);   // 568400/10150 = 56
	setPool(eng::Sym::CPools_ms_pTaskPool,              5000,   168);  // 84000/500 = 168
	setPool(eng::Sym::CPools_ms_pEventPool,             1000,   88);   // 17600/200 = 88
	setPool(eng::Sym::CPools_ms_pPointRoutePool,        200,    100);  // 6400/64 = 100
	setPool(eng::Sym::CPools_ms_pPatrolRoutePool,       200,    420);  // 13440/32 = 420
	setPool(eng::Sym::CPools_ms_pNodeRoutePool,         200,    36);   // 2304/64 = 36
	setPool(eng::Sym::CPools_ms_pTaskAllocatorPool,     3000,   48);   // 768/16 = 48
	setPool(eng::Sym::CPools_ms_pPedIntelligencePool,   240,    1088); // 152320/140 = 1088
	setPool(eng::Sym::CPools_ms_pPedAttractorPool,      200,    236);  // 15104/64 = 236
#endif
}

void Patches::apply()
{
	spdlog::info("Applying patches...");

	allocPlayers();

	// ---- Boot-scope patches (active in both online & offline modes) ----
	patches::registerNop("StartGameScreen.SkipMenuSwitchOff", "NOP Menu_SwitchOffToGame in OnNewGameCheck",
		patches::Scope::Boot, addr::Menu_SwitchOffToGame_StartGameScreen_OnNewGameCheck, 2, false);
	patches::registerNop("Idle.RwCameraEndUpdate", "NOP RwCameraEndUpdate for Idle hook",
		patches::Scope::Boot, addr::RwCameraEndUpdate_patch, 2, false);

	// Reveal all zones
	if (uintptr_t zonesVisited = eng::addr(eng::Sym::CTheZones_ZonesVisited)) {
		uint8_t visitedBuf[100];
		memset(visitedBuf, 1, sizeof(visitedBuf));
		patches::registerBytes("CTheZones::ZonesVisited", "Reveal all map zones",
			patches::Scope::Boot, zonesVisited, visitedBuf, sizeof(visitedBuf), false);
	}
	if (int* zonesRevealed = eng::as<int*>(eng::Sym::CTheZones_ZonesRevealed)) {
		*zonesRevealed = 100;
		patches::registerData("CTheZones::ZonesRevealed", "Mark 100 map zones revealed",
			patches::Scope::Boot, eng::addr(eng::Sym::CTheZones_ZonesRevealed));
	}

	// ---- Session-scope patches (registered unconditionally; toggled on demand) ----
	patches::registerRet("CStats::SetStatValue", "Lock max stats after preApply", patches::Scope::Session, eng::Sym::CStats_SetStatValue, true);
	patches::registerRet("CClock::Update", "Freeze/unfreeze GTA world clock", patches::Scope::Session, eng::Sym::CClock_Update, true);
	patches::registerNop("ToggleCJWalk", "Toggle CJ walk anim group (shared with ProcessAnimGroups)", patches::Scope::Session, addr::ToggleCJWalk, 2, true);
	patches::registerRet("DisableAutoAim.FindTarget", "Disable auto-aim lock-on search", patches::Scope::Session, eng::Sym::CPlayerPed_FindWeaponLockOnTarget, true);
	patches::registerRet("DisableAutoAim.FindNextTarget", "Disable auto-aim next-target search", patches::Scope::Session, eng::Sym::CPlayerPed_FindNextWeaponLockOnTarget, true);
	patches::registerRet("DisableAutoAim.SetTarget", "Disable auto-aim set target", patches::Scope::Session, eng::Sym::CPed_SetWeaponLockOnTarget, true);
	patches::registerRet("DisableAutoAim.TapToTarget", "Disable tap-to-target auto-aim & aim reset", patches::Scope::Session, eng::Sym::CPlayerPed_HandleTapToTarget, true);
	patches::registerRet("CClothes::RebuildPlayer", "Skip CJ clothes rebuild on model change", patches::Scope::Session, eng::Sym::CClothes_RebuildPlayer, true);

	patches::apply(patches::Scope::Boot);

	if (Client::offlineMode()) {
		return;
	}

	// ---- Online-scope patches ----
	patches::registerBytes("DefaultPCSaveFileName", "Rename save file prefix to GTASAMP",
		patches::Scope::Online, eng::addr(eng::Sym::DefaultPCSaveFileName), "GTASAMP", 8, false);

	patches::registerRet("CWeather::Update", "Disable local weather cycling", patches::Scope::Online, eng::Sym::CWeather_Update);

	patches::registerRet("CHud::SetHelpMessage", "Suppress singleplayer help popups", patches::Scope::Online, eng::Sym::CHud_SetHelpMessage);
	patches::registerRet("CHud::SetHelpMessageStatUpdate", "Suppress stat update popups", patches::Scope::Online, eng::Sym::CHud_SetHelpMessageStatUpdate);
	patches::registerRet("CHud::DrawBustedWastedMessage", "Suppress Busted/Wasted banner", patches::Scope::Online, eng::Sym::CHud_DrawBustedWastedMessage);

	patches::registerRet("CCheat::ProcessCheatMenu", "Disable cheat menu", patches::Scope::Online, eng::Sym::CCheat_ProcessCheatMenu);
	patches::registerRet("CCheat::ProcessCheats", "Disable cheat processing", patches::Scope::Online, eng::Sym::CCheat_ProcessCheats);
	patches::registerRet("CCheat::AddToCheatString", "Disable cheat string input", patches::Scope::Online, eng::Sym::CCheat_AddToCheatString);
	patches::registerRet("CCheat::DoCheats", "Disable cheats", patches::Scope::Online, eng::Sym::CCheat_DoCheats);

	// fix alpha bug in _rwOpenGLRasterCreate
	patches::registerBytes("_rwOpenGLRasterCreate.alpha", "Fix OpenGL raster alpha flag",
		patches::Scope::Online, g_saSym->Abs<uintptr_t>(addr::_rwOpenGLRasterCreate_patch),
		!VER_x32 ? "\x22\x00\x80\x52" : "\x01\x22", !VER_x32 ? 4 : 2);

	// fix texture animation
	if (auto* pRunUVAnim = eng::as<bool*>(eng::Sym::RunUVAnim)) {
		*pRunUVAnim = true;
		patches::registerData("RunUVAnim", "Enable RenderWare UV texture animations",
			patches::Scope::Online, eng::addr(eng::Sym::RunUVAnim));
	}

	// fix black markers (CRadar color translation)
#if VER_x32
	patches::registerBytes("CRadar.Draw3dMarkers.branch", "Radar 3D marker color branch", patches::Scope::Online, g_saSym->Abs<uintptr_t>(addr::CRadar_Draw3dMarkers_colorBranch), "\x2C\xE0", 2);
	patches::registerBytes("CRadar.Draw3dMarkers.mov", "Radar 3D marker color mov", patches::Scope::Online, g_saSym->Abs<uintptr_t>(addr::CRadar_Draw3dMarkers_colorMov), "\x30\x46", 2);
	patches::registerBytes("CRadar.DrawEntityBlip.branch", "Radar entity blip color branch", patches::Scope::Online, g_saSym->Abs<uintptr_t>(addr::CRadar_DrawEntityBlip_colorBranch), "\x3A\xE0", 2);
	patches::registerBytes("CRadar.DrawEntityBlip.mov", "Radar entity blip color mov", patches::Scope::Online, g_saSym->Abs<uintptr_t>(addr::CRadar_DrawEntityBlip_colorMov), "\x30\x46", 2);
	patches::registerBytes("CRadar.DrawCoordBlip.branch", "Radar coord blip color branch", patches::Scope::Online, g_saSym->Abs<uintptr_t>(addr::CRadar_DrawCoordBlip_colorBranch), "\x12\xE0", 2);
	patches::registerBytes("CRadar.DrawCoordBlip.mov", "Radar coord blip color mov", patches::Scope::Online, g_saSym->Abs<uintptr_t>(addr::CRadar_DrawCoordBlip_colorMov), "\x48\x46", 2);
#else
	patches::registerBytes("CRadar.Draw3dMarkers.branch", "Radar 3D marker color branch", patches::Scope::Online, g_saSym->Abs<uintptr_t>(addr::CRadar_Draw3dMarkers_colorBranch), "\x1E\x00\x00\x14", 4);
	patches::registerBytes("CRadar.Draw3dMarkers.mov", "Radar 3D marker color mov", patches::Scope::Online, g_saSym->Abs<uintptr_t>(addr::CRadar_Draw3dMarkers_colorMov), "\xE1\x03\x14\x2A", 4);
	patches::registerBytes("CRadar.DrawEntityBlip.branch", "Radar entity blip color branch", patches::Scope::Online, g_saSym->Abs<uintptr_t>(addr::CRadar_DrawEntityBlip_colorBranch), "\x22\x00\x00\x14", 4);
	patches::registerBytes("CRadar.DrawEntityBlip.mov", "Radar entity blip color mov", patches::Scope::Online, g_saSym->Abs<uintptr_t>(addr::CRadar_DrawEntityBlip_colorMov), "\xE1\x03\x16\x2A", 4);
	patches::registerBytes("CRadar.DrawCoordBlip.branch", "Radar coord blip color branch", patches::Scope::Online, g_saSym->Abs<uintptr_t>(addr::CRadar_DrawCoordBlip_colorBranch), "\xCC\xFF\xFF\x17", 4);
	patches::registerBytes("CRadar.DrawCoordBlip.mov", "Radar coord blip color mov", patches::Scope::Online, g_saSym->Abs<uintptr_t>(addr::CRadar_DrawCoordBlip_colorMov), "\xE1\x03\x16\x2A", 4);
#endif

	// CRealTimeShadowManager crash fixes
	patches::registerNop("CRealTimeShadowManager.Return1", "RealTimeShadow crash fix 1", patches::Scope::Online, addr::CRealTimeShadowManager_ReturnRealTimeShadow_patch1, 2);
	patches::registerNop("CRealTimeShadowManager.Return2", "RealTimeShadow crash fix 2", patches::Scope::Online, addr::CRealTimeShadowManager_ReturnRealTimeShadow_patch2, 2);
	patches::registerRet("CRealTimeShadowManager::Update", "RealTimeShadow update crash fix", patches::Scope::Online, eng::Sym::CRealTimeShadowManager_Update);

	// CPlayerPed::CPlayerPed remove player count restriction (orig: 7)
#if VER_x32
	patches::registerBytes("CPlayerPed.PlayerCountLimit", "Lift 7-player cap in CPlayerPed ctor", patches::Scope::Online, g_saSym->Abs<uintptr_t>(addr::CPlayerPed_CPlayerPed_patch), "\xE0", 1);
#else
	patches::registerBytes("CPlayerPed.PlayerCountLimit", "Lift 7-player cap in CPlayerPed ctor", patches::Scope::Online, g_saSym->Abs<uintptr_t>(addr::CPlayerPed_CPlayerPed_patch), "\x34\x00\x80\x52", 4);
#endif

	patches::registerRet("CCarEnterExit::SetPedInCarDirect", "Avoid direct teleport into vehicles", patches::Scope::Online, eng::Sym::CCarEnterExit_SetPedInCarDirect);
	patches::registerRet0("CPopulation::AddPed", "Disable ambient pedestrian spawns (return nullptr)", patches::Scope::Online, eng::Sym::CPopulation_AddPed);

	// Vehicle audio processing: only disable when .osw audio data is absent (T06)
	const bool hasAudio = hasFullAudioData();
	if (!hasAudio) {
		patches::registerNop("CAEVehicleAudio.CAutomobile", "Disable automobile audio service (no .osw)", patches::Scope::Online, addr::CAEVehicleAudioEntity_Service_patch1, 2);
		patches::registerNop("CAEVehicleAudio.CBike", "Disable bike audio service (no .osw)", patches::Scope::Online, addr::CAEVehicleAudioEntity_Service_patch2, 2);
		patches::registerNop("CAEVehicleAudio.CBoat", "Disable boat audio service (no .osw)", patches::Scope::Online, addr::CAEVehicleAudioEntity_Service_patch3, 2);
		patches::registerNop("CAEVehicleAudio.CTrain", "Disable train audio service (no .osw)", patches::Scope::Online, addr::CAEVehicleAudioEntity_Service_patch4, 2);
	}
	patches::registerNop("CAudioEngine.StartLoadingTune", "Disable loading tune audio", patches::Scope::Online, addr::CAudioEngine_StartLoadingTune_patch, 2);

#if !VER_x32
	patches::registerNop("CBike::ProcessAI", "Disable bike AI (arm64 only)", patches::Scope::Online, addr::CBike_ProcessAI_patch, 2);
#endif

	patches::registerNop("CPlayerInfo.KillPlayer.BigMsg", "Suppress wasted big message", patches::Scope::Online, addr::CMessages_AddBigMessage_patch, 2);
	patches::registerRet("CAEGlobalWeaponAudio.AmbientGunFire", "Disable ambient gunfire audio", patches::Scope::Online, eng::Sym::CAEGlobalWeaponAudioEntity_ServiceAmbientGunFire);
	patches::registerRet("CPlane::DoPlaneGenerationAndRemoval", "Disable ambient plane spawns", patches::Scope::Online, eng::Sym::CPlane_DoPlaneGenerationAndRemoval);
	patches::registerRet("CFileLoader::LoadPickup", "Disable singleplayer map pickups", patches::Scope::Online, eng::Sym::CFileLoader_LoadPickup);
	patches::registerRet("CEntryExit::GenerateAmbientPeds", "Disable interior ambient peds", patches::Scope::Online, eng::Sym::CEntryExit_GenerateAmbientPeds);
	patches::registerRet("CPlayerInfo::AddHealth", "Disable hospital/ambulance auto-heal", patches::Scope::Online, eng::Sym::CPlayerInfo_AddHealth);
	patches::registerRet("CRadar::DrawLegend", "Disable radar legend draw", patches::Scope::Online, eng::Sym::CRadar_DrawLegend);
	patches::registerRet("CRadar::AddBlipToLegendList", "Disable radar legend list", patches::Scope::Online, eng::Sym::CRadar_AddBlipToLegendList);
	patches::registerNop("CRadar.DrawCoordBlip.SpriteDraw", "Suppress vanilla coord blip sprite", patches::Scope::Online, addr::CRadar_DrawCoordBlip_patch, 2);
	patches::registerNop("CRadar.DrawEntityBlip.SpriteDraw", "Suppress vanilla entity blip sprite", patches::Scope::Online, addr::CRadar_DrawEntityBlip_patch, 2);

	// CVehicleModelInfo::ms_vehicleColourTable
	if (auto* pColourTable = eng::as<uintptr_t*>(eng::Sym::CVehicleModelInfo_ms_vehicleColourTable)) {
		*pColourTable = reinterpret_cast<uintptr_t>(VehicleColoursTableRGBA);
		patches::registerData("CVehicleModelInfo::ms_vehicleColourTable", "Install SA-MP 256-color vehicle palette",
			patches::Scope::Online, eng::addr(eng::Sym::CVehicleModelInfo_ms_vehicleColourTable));
	}

	patches::registerRet("CWanted::ReportCrimeNow", "Disable singleplayer police crime reports", patches::Scope::Online, eng::Sym::CWanted_ReportCrimeNow);
	patches::registerRet("SaveGameForPause", "Disable pause menu auto-save", patches::Scope::Online, eng::Sym::SaveGameForPause);
	patches::registerRet("C_PcSave::SaveSlot", "Disable PC save slot writes", patches::Scope::Online, eng::Sym::C_PcSave_SaveSlot);
	patches::registerNop("FindPlayerInfoForThisPlayerPed.patch", "Bypass singleplayer playerinfo lookup", patches::Scope::Online, addr::FindPlayerInfoForThisPlayerPed_patch, 6);
	patches::registerRet("CTaskSimplePlayerOnFoot::PlayIdleAnimations", "Disable singleplayer idle anims", patches::Scope::Online, eng::Sym::CTaskSimplePlayerOnFoot_PlayIdleAnimations);
	patches::registerRet("DoFade", "Disable singleplayer screen fades", patches::Scope::Online, eng::Sym::DoFade);
	patches::registerRet("CCarCtrl::GenerateRandomCars", "Disable ambient traffic cars", patches::Scope::Online, eng::Sym::CCarCtrl_GenerateRandomCars);
	patches::registerRet("CTheCarGenerators::Process", "Disable parked car generators", patches::Scope::Online, eng::Sym::CTheCarGenerators_Process);

	// Disable built-in GTA:SA audio servicing only when .osw audio files are absent (T06);
	// when full audio banks exist, let the engine play SFX, vehicle sounds, and PlayerPlaySound.
	if (!hasAudio) {
		spdlog::info("Patches: no .osw audio banks detected -> keeping GTA audio engine patched");
		patches::registerRet("CAERadioTrackManager::Service", "Disable GTA radio service (no .osw)", patches::Scope::Online, eng::Sym::CAERadioTrackManager_Service);
		patches::registerRet("CAudioEngine::Service", "Disable GTA audio engine service (no .osw)", patches::Scope::Online, eng::Sym::CAudioEngine_Service);
		patches::registerRet("CAEStreamThread::Service", "Disable GTA audio stream thread (no .osw)", patches::Scope::Online, eng::Sym::CAEStreamThread_Service);
		patches::registerRet("CAEFrontendAudioEntity::AddAudioEvent", "Disable GTA frontend audio events (no .osw)", patches::Scope::Online, eng::Sym::CAEFrontendAudioEntity_AddAudioEvent);
		patches::registerRet("CAudioEngine::ReportFrontendAudioEvent", "Disable GTA frontend audio report (no .osw)", patches::Scope::Online, eng::Sym::CAudioEngine_ReportFrontendAudioEvent);
	}
	else {
		spdlog::info("Patches: full .osw audio banks detected -> GTA audio engine enabled");
	}

	patches::apply(patches::Scope::Online);

	// Initial online state for Session patches (exact parity with pre-F04 startup)
	patches::set("CClock::Update", true);
	patches::set("ToggleCJWalk", true);
}

void InstallVehicleEngineLightPatches()
{
	// ~
}
