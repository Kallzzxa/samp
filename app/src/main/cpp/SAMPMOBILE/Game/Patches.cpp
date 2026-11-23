#include "Game.h"
#include "../Client.h"
#include "../Settings.h"
#include "VehicleColoursTable.h"

sa::CPlayerInfo* WORLD_PLAYERS = nullptr;

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
	Memory::ret(eng::addr(eng::Sym::CStats_SetStatValue)); // CStats::SetStatValue
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

	// Menu_SwitchOffToGame call in StartGameScreen::OnNewGameCheck
	Memory::nop(g_saSym->Abs<uintptr_t>(addr::Menu_SwitchOffToGame_StartGameScreen_OnNewGameCheck), 2);

	// Reveal all zones
	if (uintptr_t zonesVisited = eng::addr(eng::Sym::CTheZones_ZonesVisited)) {
		uint8_t visitedBuf[100];
		memset(visitedBuf, 1, sizeof(visitedBuf));
		Memory::writeMemory(zonesVisited, reinterpret_cast<uintptr_t>(visitedBuf), sizeof(visitedBuf));
	}
	if (int* zonesRevealed = eng::as<int*>(eng::Sym::CTheZones_ZonesRevealed)) {
		*zonesRevealed = 100; // CTheZones::ZonesRevealed
	}

	// RwCameraEndUpdate (for Idle hook)
	Memory::nop(g_saSym->Abs<uintptr_t>(addr::RwCameraEndUpdate_patch), 2);

	if (Client::offlineMode()) {
		return;
	}

	// DefaultPCSaveFileName
	Memory::writeMemory(eng::addr(eng::Sym::DefaultPCSaveFileName), (uintptr_t) "GTASAMP", 8);

	// Disable update time
	Memory::ret(eng::addr(eng::Sym::CClock_Update)); // CClock::Update

	// Disable update weather
	Memory::ret(eng::addr(eng::Sym::CWeather_Update)); // CWeather::Update

	// No message print
	Memory::ret(eng::addr(eng::Sym::CHud_SetHelpMessage)); // CHud::SetHelpMessage
	Memory::ret(eng::addr(eng::Sym::CHud_SetHelpMessageStatUpdate)); // CHud::SetHelpMessageStatUpdate
	Memory::ret(eng::addr(eng::Sym::CHud_DrawBustedWastedMessage)); // CHud::DrawBustedWastedMessage

	// Disable cheat
	Memory::ret(eng::addr(eng::Sym::CCheat_ProcessCheatMenu)); // CCheat::ProcessCheatMenu
	Memory::ret(eng::addr(eng::Sym::CCheat_ProcessCheats)); // CCheat::ProcessCheats
	Memory::ret(eng::addr(eng::Sym::CCheat_AddToCheatString)); // CCheat::AddToCheatString
	Memory::ret(eng::addr(eng::Sym::CCheat_DoCheats)); // CCheat::DoCheats

	// fix alpha bug in _rwOpenGLRasterCreate
	Memory::writeMemory(g_saSym->Abs<uintptr_t>(addr::_rwOpenGLRasterCreate_patch), !VER_x32 ? (uintptr_t) "\x22\x00\x80\x52" : (uintptr_t) "\x01\x22", !VER_x32 ? 4 : 2);

	// fix texture animation
	if (auto* pRunUVAnim = eng::as<bool*>(eng::Sym::RunUVAnim)) {
		*pRunUVAnim = true;
	}

	// fix black markers
#if VER_x32
	// CRadar::Draw3dMarkers (translate color)
	Memory::writeMemory(g_saSym->Abs<uintptr_t>(addr::CRadar_Draw3dMarkers_colorBranch), (uintptr_t) "\x2C\xE0", 2);
	Memory::writeMemory(g_saSym->Abs<uintptr_t>(addr::CRadar_Draw3dMarkers_colorMov), (uintptr_t) "\x30\x46", 2);

	// CRadar::DrawEntityBlip (translate color)
	Memory::writeMemory(g_saSym->Abs<uintptr_t>(addr::CRadar_DrawEntityBlip_colorBranch), (uintptr_t) "\x3A\xE0", 2);
	Memory::writeMemory(g_saSym->Abs<uintptr_t>(addr::CRadar_DrawEntityBlip_colorMov), (uintptr_t) "\x30\x46", 2);

	// CRadar::DrawCoordBlip (translate color)
	Memory::writeMemory(g_saSym->Abs<uintptr_t>(addr::CRadar_DrawCoordBlip_colorBranch), (uintptr_t) "\x12\xE0", 2);
	Memory::writeMemory(g_saSym->Abs<uintptr_t>(addr::CRadar_DrawCoordBlip_colorMov), (uintptr_t) "\x48\x46", 2);
#else
	// CRadar::Draw3dMarkers (translate color)
	Memory::writeMemory(g_saSym->Abs<uintptr_t>(addr::CRadar_Draw3dMarkers_colorBranch), (uintptr_t) "\x1E\x00\x00\x14", 4);
	Memory::writeMemory(g_saSym->Abs<uintptr_t>(addr::CRadar_Draw3dMarkers_colorMov), (uintptr_t) "\xE1\x03\x14\x2A", 4);

	// CRadar::DrawEntityBlip (translate color)
	Memory::writeMemory(g_saSym->Abs<uintptr_t>(addr::CRadar_DrawEntityBlip_colorBranch), (uintptr_t) "\x22\x00\x00\x14", 4);
	Memory::writeMemory(g_saSym->Abs<uintptr_t>(addr::CRadar_DrawEntityBlip_colorMov), (uintptr_t) "\xE1\x03\x16\x2A", 4);

	// CRadar::DrawCoordBlip (translate color)
	Memory::writeMemory(g_saSym->Abs<uintptr_t>(addr::CRadar_DrawCoordBlip_colorBranch), (uintptr_t) "\xCC\xFF\xFF\x17", 4);
	Memory::writeMemory(g_saSym->Abs<uintptr_t>(addr::CRadar_DrawCoordBlip_colorMov), (uintptr_t) "\xE1\x03\x16\x2A", 4);
#endif

	// crash fix (same canonical address as ToggleCJWalk)
	Memory::nop(g_saSym->Abs<uintptr_t>(addr::ToggleCJWalk), 2); // CPlayerPed::ProcessAnimGroups

	// CRealTimeShadowManager::ReturnRealTimeShadow crash fix
	Memory::nop(g_saSym->Abs<uintptr_t>(addr::CRealTimeShadowManager_ReturnRealTimeShadow_patch1), 2);
	Memory::nop(g_saSym->Abs<uintptr_t>(addr::CRealTimeShadowManager_ReturnRealTimeShadow_patch2), 2);

	// CRealTimeShadowManager::Update crash fix
	Memory::ret(eng::addr(eng::Sym::CRealTimeShadowManager_Update));

	// CPlayerPed::CPlayerPed remove player count restriction (orig: 7)
#if VER_x32
	Memory::writeMemory(g_saSym->Abs<uintptr_t>(addr::CPlayerPed_CPlayerPed_patch), (uintptr_t) "\xE0", 1);
#else
	Memory::writeMemory(g_saSym->Abs<uintptr_t>(addr::CPlayerPed_CPlayerPed_patch), (uintptr_t) "\x34\x00\x80\x52", 4);
#endif

	// Avoid direct teleportation of vehicles
	Memory::ret(eng::addr(eng::Sym::CCarEnterExit_SetPedInCarDirect)); // CCarEnterExit::SetPedInCarDirect

	// add pedestrian (returns CPed*; must return nullptr, not garbage x0=ePedType)
	Memory::ret0(eng::addr(eng::Sym::CPopulation_AddPed)); // CPopulation::AddPed

	// no vehicle audio processing
	Memory::nop(g_saSym->Abs<uintptr_t>(addr::CAEVehicleAudioEntity_Service_patch1), 2); // CAutomobile
	Memory::nop(g_saSym->Abs<uintptr_t>(addr::CAEVehicleAudioEntity_Service_patch2), 2); // CBike
	Memory::nop(g_saSym->Abs<uintptr_t>(addr::CAEVehicleAudioEntity_Service_patch3), 2); // CBoat
	Memory::nop(g_saSym->Abs<uintptr_t>(addr::CAEVehicleAudioEntity_Service_patch4), 2); // CTrain

	// CAudioEngine::StartLoadingTune
	Memory::nop(g_saSym->Abs<uintptr_t>(addr::CAudioEngine_StartLoadingTune_patch), 2);

	// CBike::ProcessAI
#if !VER_x32
	Memory::nop(g_saSym->Abs<uintptr_t>(addr::CBike_ProcessAI_patch), !VER_x32 ? 2 : 1); // crashes on x32
#endif

	// CPlayerInfo::KillPlayer -> CMessages::AddBigMessage
	Memory::nop(g_saSym->Abs<uintptr_t>(addr::CMessages_AddBigMessage_patch), 2);

	// CAEGlobalWeaponAudioEntity::ServiceAmbientGunFire
	Memory::ret(eng::addr(eng::Sym::CAEGlobalWeaponAudioEntity_ServiceAmbientGunFire));

	// CPlane::DoPlaneGenerationAndRemoval
	Memory::ret(eng::addr(eng::Sym::CPlane_DoPlaneGenerationAndRemoval));

	// CFileLoader::LoadPickup
	Memory::ret(eng::addr(eng::Sym::CFileLoader_LoadPickup));

	// disable Generate scene npc
	Memory::ret(eng::addr(eng::Sym::CEntryExit_GenerateAmbientPeds)); // CEntryExit::GenerateAmbientPeds

	// fix hospital car
	Memory::ret(eng::addr(eng::Sym::CPlayerInfo_AddHealth)); // CPlayerInfo::AddHealth

	// draw map icon
	Memory::ret(eng::addr(eng::Sym::CRadar_DrawLegend)); // CRadar::DrawLegend

	// CRadar::AddBlipToLegendList
	Memory::ret(eng::addr(eng::Sym::CRadar_AddBlipToLegendList));

	// NOP calling CSprite2d::Draw from CRadar::DrawCoordBlip
	Memory::nop(g_saSym->Abs<uintptr_t>(addr::CRadar_DrawCoordBlip_patch), 2);

	// NOP calling CSprite2d::Draw from CRadar::DrawEntityBlip
	Memory::nop(g_saSym->Abs<uintptr_t>(addr::CRadar_DrawEntityBlip_patch), 2);

	// CVehicleModelInfo::ms_vehicleColourTable
	if (auto* pColourTable = eng::as<uintptr_t*>(eng::Sym::CVehicleModelInfo_ms_vehicleColourTable)) {
		*pColourTable = reinterpret_cast<uintptr_t>(VehicleColoursTableRGBA);
	}

	// stuff
	Memory::ret(eng::addr(eng::Sym::CWanted_ReportCrimeNow));
	Memory::ret(eng::addr(eng::Sym::SaveGameForPause));
	Memory::ret(eng::addr(eng::Sym::C_PcSave_SaveSlot));

	// Remove the FindPlayerInfoForThisPlayerPed
	Memory::nop(g_saSym->Abs<uintptr_t>(addr::FindPlayerInfoForThisPlayerPed_patch), 6);

	// CTaskSimplePlayerOnFoot::PlayIdleAnimations
	Memory::ret(eng::addr(eng::Sym::CTaskSimplePlayerOnFoot_PlayIdleAnimations));

	// DoFade
	Memory::ret(eng::addr(eng::Sym::DoFade));

	// generate random cars
	Memory::ret(eng::addr(eng::Sym::CCarCtrl_GenerateRandomCars));
	Memory::ret(eng::addr(eng::Sym::CTheCarGenerators_Process));

	// vehicle radio
	/*Memory::ret(g_saSym->GetSymbol<uintptr_t>("_ZN21CAEVehicleAudioEntity32PlayerAboutToExitVehicleAsDriverEv"));
	Memory::ret(g_saSym->GetSymbol<uintptr_t>("_ZN21CAEVehicleAudioEntity18JustWreckedVehicleEv"));
	Memory::ret(g_saSym->GetSymbol<uintptr_t>("_ZN21CAEVehicleAudioEntity21TurnOnRadioForVehicleEv"));*/

	// Disable in-game radio & built-in GTA:SA audio servicing (no .osw audio shipped;
	// SA-MP AudioStream/Voice use BASS independently).
	Memory::ret(eng::addr(eng::Sym::CAERadioTrackManager_Service));
	Memory::ret(eng::addr(eng::Sym::CAudioEngine_Service));
	Memory::ret(eng::addr(eng::Sym::CAEStreamThread_Service));
	Memory::ret(eng::addr(eng::Sym::CAEFrontendAudioEntity_AddAudioEvent));
	Memory::ret(eng::addr(eng::Sym::CAudioEngine_ReportFrontendAudioEvent));
}

void InstallVehicleEngineLightPatches()
{
	// ~
}
