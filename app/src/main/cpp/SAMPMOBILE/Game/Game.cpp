//
// Created by ALYN on 2024/7/30.
//

#include "Game.h"
#include "../Client.h"
#include "../Java/Java.h"
#include "../UI/UISettings.h"
#include "../UI/ImGuiRenderer.h"

void InitScripting();

bool bUsedPlayerSlots[PLAYER_PED_SLOTS];

extern sa::CPlayerInfo* WORLD_PLAYERS;
char* szGameTextMessage = nullptr;

inline int FindFirstFreePlayerPedSlot()
{
	//spdlog::info("FindFirstFreePlayerPedSlot");

	uint8_t x = 2;
	while (x != PLAYER_PED_SLOTS) {
		if (!bUsedPlayerSlots[x]) return x;
		x++;
	}

	return 0;
}

Game::Game()
{
	//spdlog::info("Game::Game");

	m_gameCamera = nullptr;
	m_gamePlayer = nullptr;
	m_checkpointsEnabled = false;
	m_raceCheckpointsEnabled = false;
	m_raceCheckpointHandle = 0;

	m_clockEnabled = false;
	m_gameInput = true;

	memset(bUsedPlayerSlots, 0, sizeof(bUsedPlayerSlots));
	memset(m_preloadedVehicleModels, 0, sizeof(m_preloadedVehicleModels));
}

Game::~Game()
{
	//spdlog::info("Game::Game");
}

void Game::Initialize()
{
	//spdlog::info("Game::Initialize");

	szGameTextMessage = (char*) malloc(512 + 1);
	GameResetRadarColors();

	InitScripting();
}

void Game::StartGame()
{
	//spdlog::info("Game::StartGame");

	// StartGameScreen::OnNewGameCheck
	eng::fn::StartGameScreen_OnNewGameCheck();

	GameAimSyncInit();
}

bool Game::IsModelLoaded(int iModel)
{
	//spdlog::info("Game::IsModelLoaded: {}", iModel);

	if (iModel > 20000 || iModel < 0) {
		return true;
	}
	else {
		return ScriptCommand(&is_model_available, iModel);
	}

	// return sa::CStreaming::HasModelLoaded(iModel);
}

void Game::RequestModel(int iModel)
{
	//spdlog::info("Game::RequestModel: {}", iModel);
	ScriptCommand(&request_model, iModel);
	// sa::CStreaming::RequestModel(iModel, 2);
}

void Game::LoadRequestedModels()
{
	//spdlog::info("Game::LoadRequestedModels");
	ScriptCommand(&load_requested_models);
	// sa::CStreaming::LoadAllRequestedModels(false);
}

void Game::RemoveModel(int iModel, bool bFromStreaming)
{
	//spdlog::info("Game::RemoveModel: {}", iModel);

	if (iModel >= 0 && iModel < 20000) {
		if (bFromStreaming) {
			if (ScriptCommand(&is_model_available, iModel)) {
				// CStreaming::RemoveModel
				eng::fn::CStreaming_RemoveModel( iModel);
			}
		}
		else {
			if (ScriptCommand(&is_model_available, iModel)) {
				ScriptCommand(&release_model, iModel);
			}
		}
	}
}

void Game::ToggleThePassingOfTime(bool bOnOff)
{
	//spdlog::info("Game::ToggleThePassingOfTime: {}", bOnOff);

	if (bOnOff) {
		// CClock::Update(void)
		Memory::restoreRet((uintptr_t) eng::ptr(eng::Sym::CClock_Update));
		m_clockEnabled = true;
	}
	else {
		Memory::ret((uintptr_t) eng::ptr(eng::Sym::CClock_Update));
		m_clockEnabled = false;
	}
}

void Game::EnableClock(bool bEnable)
{
	//spdlog::info("Game::EnableClock: {}", bEnable);

	char byteClockData[] = {'%', '0', '2', 'd', ':', '%', '0', '2', 'd', 0};
	Memory::protectAddr(g_saSym->Abs(addr::ClockFormat)); // %02d:%02d

	if (bEnable) {
		ToggleThePassingOfTime(true);
		memcpy(g_saSym->Abs<void*>(addr::ClockFormat), byteClockData, 10);
	}
	else {
		ToggleThePassingOfTime(false);
		memset(g_saSym->Abs<void*>(addr::ClockFormat), 0, 10);
	}
}

void Game::EnableZoneNames(bool bEnable)
{
	//spdlog::info("Game::EnableZoneNames: {}", bEnable);
	ScriptCommand(&enable_zone_names, bEnable);
}

void Game::DisableMarker(uint32_t dwMarker)
{
	//spdlog::info("Game::DisableMarker: {}", dwMarker);
	ScriptCommand(&disable_marker, dwMarker);
}

/**
 * @param byteType
 * @param fPosX
 * @param fPosY
 * @param fPosZ
 * @param dwColor
 * @param byteStyle
 * @return
 */
uint32_t Game::CreateRadarMarkerIcon(uint8_t byteType, float fPosX, float fPosY, float fPosZ, uint32_t dwColor, uint8_t byteStyle)
{
	//spdlog::info("Game::CreateRadarMarkerIcon: {}, {}, {}, {}, {}, {}", byteType, fPosX, fPosY, fPosZ, dwColor, byteStyle);

	uint32_t dwMarkerID = 0;

	if (byteStyle == 1) {
		ScriptCommand(&create_marker_icon, fPosX, fPosY, fPosZ, byteType, &dwMarkerID);
	}
	else if (byteStyle == 2) {
		ScriptCommand(&create_radar_marker_icon, fPosX, fPosY, fPosZ, byteType, &dwMarkerID);
	}
	else if (byteStyle == 3) {
		ScriptCommand(&create_icon_marker_sphere, fPosX, fPosY, fPosZ, byteType, &dwMarkerID);
	}
	else {
		ScriptCommand(&create_radar_marker_without_sphere, fPosX, fPosY, fPosZ, byteType, &dwMarkerID);
	}

	if (byteType == 0) {
		if (dwColor >= 1004) {
			ScriptCommand(&set_marker_color, dwMarkerID, dwColor);
			ScriptCommand(&show_on_radar, dwMarkerID, 3);
		}
		else {
			ScriptCommand(&set_marker_color, dwMarkerID, dwColor);
			ScriptCommand(&show_on_radar, dwMarkerID, 2);
		}
	}

	return dwMarkerID;

	// crashes sometimes wtf?
	/*uint32_t dwMapIcon = 0;

	switch (byteStyle) {
		case 0:    // MAPICON_LOCAL
			ScriptCommand(&create_radar_marker_without_sphere, fPosX, fPosY, fPosZ, byteType, &dwMapIcon);
			break;

		case 1:    // MAPICON_GLOBAL
			ScriptCommand(&create_marker_icon, fPosX, fPosY, fPosZ, byteType, &dwMapIcon);
			break;

		case 2:    // MAPICON_LOCAL_CHECKPOINT
			ScriptCommand(&create_radar_marker_icon, fPosX, fPosY, fPosZ, byteType, &dwMapIcon);
			break;

		case 3:    // MAPICON_GLOBAL_CHECKPOINT
			ScriptCommand(&create_icon_marker_sphere, fPosX, fPosY, fPosZ, byteType, &dwMapIcon);
			break;
	}

	if (byteType == 0) {
		if (dwColor < 1004) {
			ScriptCommand(&set_marker_color, dwMapIcon, dwColor);
			ScriptCommand(&show_on_radar, dwMapIcon, 2);
		}
		else {
			ScriptCommand(&set_marker_color, dwMapIcon, dwColor);
			ScriptCommand(&show_on_radar, dwMapIcon, 3);
		}
	}

	return dwMapIcon;*/
}

void Game::UpdateCheckpoints()
{
	//spdlog::info("Game::UpdateCheckpoints");

	if (m_checkpointsEnabled) {
		CPlayerPed* pPlayerPed = FindPlayerPed();
		if (pPlayerPed) {
			ScriptCommand(&is_actor_near_point_3d, pPlayerPed->m_gtaId, m_checkpointPos.x, m_checkpointPos.y, m_checkpointPos.z, m_checkpointExtent.x, m_checkpointExtent.y, m_checkpointExtent.z, 1);

			if (!m_checkpointMarker) {
				m_checkpointMarker = CreateRadarMarkerIcon(0, m_checkpointPos.x, m_checkpointPos.y, m_checkpointPos.z, 1005, 0);
			}
		}
	}
	else if (m_checkpointMarker) {
		DisableMarker(m_checkpointMarker);
		m_checkpointMarker = 0;
	}

	if (m_raceCheckpointsEnabled) {
		CPlayerPed* pPlayerPed = FindPlayerPed();
		if (pPlayerPed) {
			if (!m_raceCheckpointMarker) {
				m_raceCheckpointMarker = CreateRadarMarkerIcon(0, m_raceCheckpointPos.x, m_raceCheckpointPos.y, m_raceCheckpointPos.z, 1005, 0);
			}
		}
	}
	else if (m_raceCheckpointMarker) {
		DisableMarker(m_raceCheckpointMarker);
		DisableRaceCheckpoint();
		m_raceCheckpointMarker = 0;
	}
}

void Game::SetCheckpointInformation(sa::CVector* vecPos, sa::CVector* vecSize)
{
	//spdlog::info("Game::SetCheckpointInformation");

	m_checkpointPos.x = vecPos->x;
	m_checkpointPos.y = vecPos->y;
	m_checkpointPos.z = vecPos->z;

	m_checkpointExtent.x = vecSize->x;
	m_checkpointExtent.y = vecSize->y;
	m_checkpointExtent.z = vecSize->z;

	if (m_checkpointMarker) {
		DisableMarker(m_checkpointMarker);
		m_checkpointMarker = 0;
		m_checkpointMarker = CreateRadarMarkerIcon(0, m_checkpointPos.x, m_checkpointPos.y, m_checkpointPos.z, 1005, 0);
	}
}

void Game::SetRaceCheckpointInformation(uint8_t byteType, sa::CVector* vecPos, sa::CVector* vecNextPos, float fRadius)
{
	//spdlog::info("Game::SetRaceCheckpointInformation");

	m_raceCheckpointPos.x = vecPos->x;
	m_raceCheckpointPos.y = vecPos->y;
	m_raceCheckpointPos.z = vecPos->z;

	m_raceCheckpointNextPos.x = vecNextPos->x;
	m_raceCheckpointNextPos.y = vecNextPos->y;
	m_raceCheckpointNextPos.z = vecNextPos->z;

	m_raceType = byteType;
	m_raceCheckpointRadius = fRadius;

	if (m_raceCheckpointMarker) {
		DisableMarker(m_raceCheckpointMarker);
		m_raceCheckpointMarker = CreateRadarMarkerIcon(0, m_raceCheckpointPos.x, m_raceCheckpointPos.y, m_raceCheckpointPos.z, 1005, 0);
	}

	MakeRaceCheckpoint();
}

void Game::MakeRaceCheckpoint()
{
	//spdlog::info("Game::MakeRaceCheckpoint");

	DisableRaceCheckpoint();

	ScriptCommand(&create_racing_checkpoint, (int) m_raceType,
			m_raceCheckpointPos.x, m_raceCheckpointPos.y, m_raceCheckpointPos.z, m_raceCheckpointNextPos.x, m_raceCheckpointNextPos.y, m_raceCheckpointNextPos.z, m_raceCheckpointRadius, &m_raceCheckpointHandle);

	m_raceCheckpointsEnabled = true;
}

void Game::DisableRaceCheckpoint()
{
	//spdlog::info("Game::DisableRaceCheckpoint");

	if (m_raceCheckpointHandle) {
		ScriptCommand(&destroy_racing_checkpoint, m_raceCheckpointHandle);
		m_raceCheckpointHandle = 0;
	}

	m_raceCheckpointsEnabled = false;
}

/**
 * @return
 */
uint8_t Game::GetPedSlotsUsed()
{
	//spdlog::info("Game::GetPedSlotsUsed");

	uint8_t count = 0;
	for (int i = 2; i < PLAYER_PED_SLOTS; i++) {
		if (bUsedPlayerSlots[i]) {
			count++;
		}
	}

	return count;
}

void Game::DisplayGameText(const char* szStr, int iTime, int iSize)
{
	//spdlog::info("Game::DisplayGameText: {}, {}, {}", szStr, iTime, iSize);

	if (!szStr || iSize < 0 || iSize > 6) return;

	std::string utf8 = Encoding::cp2utf(szStr);

	if (StringHasNonAscii(utf8.c_str())) {
		// The engine font cannot draw these characters; show via ImGui instead.
		m_imguiGameText = GxtToImGuiColors(ReplaceKey(utf8), "{FFFFFF}");
		m_imguiGameTextExpire = GetTickCount() + iTime;
		m_imguiGameTextSize = iSize;
		return;
	}

	m_imguiGameText.clear();

	ScriptCommand(&text_clear_all);

	uint16_t m_szAscii[801];
	CFont::StringToAscii(utf8.c_str(), m_szAscii);
	CFont::AsciiToGxtCharU8(m_szAscii, (uint16_t*) szGameTextMessage);

	// CMessages::AddBigMesssage
	eng::fn::CMessages_AddBigMessage( szGameTextMessage, iTime, iSize);
}

void Game::DrawImGuiGameText(ImGuiRenderer* renderer)
{
	if (m_imguiGameText.empty()) return;
	if (GetTickCount() >= m_imguiGameTextExpire) {
		m_imguiGameText.clear();
		return;
	}

	float fontSize = (UISettings::fontSize() / 2.0f) * (1.0f + (float) m_imguiGameTextSize * 0.15f);
	ImVec2 textSize = renderer->calculateTextSize(m_imguiGameText, fontSize);

	ImVec2 pos;
	pos.x = ((float) sa::RsGlobal->screenWidth - textSize.x) * 0.5f;
	pos.y = (float) sa::RsGlobal->screenHeight * 0.72f;

	renderer->drawText(pos, ImColor(1.0f, 1.0f, 1.0f), m_imguiGameText, true, fontSize);
}

/**
 * @return
 */
uint8_t Game::GetActiveInterior()
{
	//spdlog::info("Game::GetActiveInterior");
	auto* pCurrArea = eng::as<uint8_t*>(eng::Sym::CGame_currArea);
	return pCurrArea ? *pCurrArea : 0;
}

/**
 * @return
 */
int Game::GetLocalMoney()
{
	//spdlog::info("Game::GetLocalMoney");
	return WORLD_PLAYERS ? WORLD_PLAYERS->Score : 0;
}

/**
 * @param iAmmount
 */
void Game::AddToLocalMoney(int iAmmount)
{
	//spdlog::info("Game::AddToLocalMoney: {}", iAmmount);
	ScriptCommand(&add_to_player_money, 0, iAmmount);
}

void Game::ResetLocalMoney()
{
	//spdlog::info("Game::ResetLocalMoney");

	int iMoney = GetLocalMoney();
	if (!iMoney) return;

	if (iMoney < 0) {
		AddToLocalMoney(abs(iMoney));
	}
	else {
		AddToLocalMoney(-(iMoney));
	}
}

/**
 * @param iHour
 * @param iMinute
 */
void Game::SetWorldTime(int iHour, int iMinute)
{
	//spdlog::info("Game::SetWorldTime: {}, {}", iHour, iMinute);

	if (auto* pMin = eng::as<uint8_t*>(eng::Sym::CClock_ms_nGameClockMinutes)) {
		*pMin = static_cast<uint8_t>(iMinute);
	}
	if (auto* pHr = eng::as<uint8_t*>(eng::Sym::CClock_ms_nGameClockHours)) {
		*pHr = static_cast<uint8_t>(iHour);
	}
	ScriptCommand(&set_current_time, iHour, iMinute);
}

/**
 * @param iHour
 * @param iMinute
 */
void Game::GetWorldTime(int* iHour, int* iMinute)
{
	//spdlog::info("Game::GetWorldTime");

	if (iMinute) {
		auto* pMin = eng::as<uint8_t*>(eng::Sym::CClock_ms_nGameClockMinutes);
		*iMinute = pMin ? static_cast<int>(*pMin) : 0;
	}
	if (iHour) {
		auto* pHr = eng::as<uint8_t*>(eng::Sym::CClock_ms_nGameClockHours);
		*iHour = pHr ? static_cast<int>(*pHr) : 0;
	}
}

/**
 * @param dwTimer
 */
void Game::UpdateGlobalTimer(uint32_t dwTimer)
{
	//spdlog::info("Game::UpdateGlobalTimer: {}", dwTimer);

	if (!m_clockEnabled) {
		*eng::as<uint32_t*>(eng::Sym::CTimer_m_snTimeInMilliseconds) = dwTimer & 0x3FFFFFFF;
	}
}

/**
 * @param byteWeatherID
 */
void Game::SetWorldWeather(int byteWeatherID)
{
	//spdlog::info("Game::SetWorldWeather: {}", byteWeatherID);

	*eng::as<uint8_t*>(eng::Sym::CWeather_ForcedWeatherType) = byteWeatherID;
	if (!m_clockEnabled) {
		*eng::as<uint8_t*>(eng::Sym::CWeather_NewWeatherType) = byteWeatherID;
		*eng::as<uint8_t*>(eng::Sym::CWeather_OldWeatherType) = byteWeatherID;
	}
}

void Game::DisplayHUD(bool bDisp)
{
	//spdlog::info("Game::DisplayHUD: {}", bDisp);

	if (bDisp) {
		*g_saSym->Abs<uint8_t*>(addr::CTheScripts_bDisplayHud) = 1;
		*g_saSym->Abs<uint8_t*>(addr::CHud_bScriptDontDisplayRadar) = 0;
	}
	else {
		*g_saSym->Abs<uint8_t*>(addr::CTheScripts_bDisplayHud) = 0;
		*g_saSym->Abs<uint8_t*>(addr::CHud_bScriptDontDisplayRadar) = 1;
	}
}

/**
 * @param toggle
 */
void Game::ToggleCJWalk(bool toggle)
{
	//spdlog::info("Game::ToggleCJWalk: {}", toggle);

	if (toggle) {
		Memory::restoreNop(g_saSym->Abs<uintptr_t>(addr::ToggleCJWalk));
	}
	else {
		Memory::nop(g_saSym->Abs<uintptr_t>(addr::ToggleCJWalk), 2);
	}
}

void Game::DisableTrainTraffic()
{
	//spdlog::info("Game::DisableTrainTraffic");
	ScriptCommand(&enable_train_traffic, 0);
}

/**
 * @param x
 * @param y
 */
void Game::RefreshStreamingAt(float x, float y)
{
	//spdlog::info("Game::RefreshStreamingAt: {}, {}", x, y);
	ScriptCommand(&refresh_streaming_at, x, y);
}

void Game::PreloadObjectsAnims()
{
	//spdlog::info("Game::PreloadObjectsAnims");

	// keep the throwable weapon models loaded
	if (!IsModelLoaded(WEAPON_MODEL_TEARGAS)) RequestModel(WEAPON_MODEL_TEARGAS);
	if (!IsModelLoaded(WEAPON_MODEL_GRENADE)) RequestModel(WEAPON_MODEL_GRENADE);
	if (!IsModelLoaded(WEAPON_MODEL_MOLOTOV)) RequestModel(WEAPON_MODEL_MOLOTOV);

	// special action object
	if (!IsModelLoaded(330)) RequestModel(330);
	if (!IsModelLoaded(OBJECT_PARACHUTE)) RequestModel(OBJECT_PARACHUTE);
	if (!IsModelLoaded(OBJECT_CJ_CIGGY)) RequestModel(OBJECT_CJ_CIGGY);
	if (!IsModelLoaded(OBJECT_DYN_BEER_1)) RequestModel(OBJECT_DYN_BEER_1);
	if (!IsModelLoaded(OBJECT_CJ_BEER_B_2)) RequestModel(OBJECT_CJ_BEER_B_2);
	if (!IsModelLoaded(OBJECT_CJ_PINT_GLASS)) RequestModel(OBJECT_CJ_PINT_GLASS);
	if (!IsModelLoaded(18631)) RequestModel(18631);

	// special action anim
	if (IsAnimationLoaded("PARACHUTE") == 0) RequestAnimation("PARACHUTE");
	if (IsAnimationLoaded("PAULNMAC") == 0) RequestAnimation("PAULNMAC");
	if (IsAnimationLoaded("BAR") == 0) RequestAnimation("BAR");
	if (IsAnimationLoaded("SMOKING") == 0) RequestAnimation("SMOKING");
	if (IsAnimationLoaded("DANCING") == 0) RequestAnimation("DANCING");
	if (IsAnimationLoaded("GFUNK") == 0) RequestAnimation("GFUNK");
	if (IsAnimationLoaded("RUNNINGMAN") == 0) RequestAnimation("RUNNINGMAN");
	if (IsAnimationLoaded("STRIP") == 0) RequestAnimation("STRIP");
	if (IsAnimationLoaded("WOP") == 0) RequestAnimation("WOP");
	if (IsAnimationLoaded("CARRY") == 0) RequestAnimation("CARRY");
	if (IsAnimationLoaded("RAPPING") == 0) RequestAnimation("RAPPING");
	if (IsAnimationLoaded("PED") == 0) RequestAnimation("PED");
	if (IsAnimationLoaded("COP_AMBIENT") == 0) RequestAnimation("COP_AMBIENT");
	if (IsAnimationLoaded("SWEET") == 0) RequestAnimation("SWEET");
	if (IsAnimationLoaded("CRACK") == 0) RequestAnimation("CRACK");
}

void Game::DisableAutoAim()
{
	//spdlog::info("Game::DisableAutoAim");
	Memory::ret(eng::addr(eng::Sym::CPlayerPed_FindWeaponLockOnTarget)); // CPed::FindWeaponLockOnTarget
	Memory::ret(eng::addr(eng::Sym::CPlayerPed_FindNextWeaponLockOnTarget)); // CPed::FindNextWeaponLockOnTarget
	Memory::ret(eng::addr(eng::Sym::CPed_SetWeaponLockOnTarget)); // CPed::SetWeaponLockOnTarget
}

void Game::SetWantedLevel(uint8_t level)
{
	//spdlog::info("Game::SetWantedLevel: {}", level);

	if (g_java) {
		g_java->setWantedLevel(level);
	}
}

void Game::EnableStuntBonus(bool bEnable)
{
	//spdlog::info("Game::EnableStuntBonus: {}", bEnable);
	// todo
}

void Game::SetGravity(float fGravity)
{
	//spdlog::info("Game::SetGravity: {}", fGravity);
	Memory::protectAddr(g_saSym->Abs(addr::SetGravity));
	*g_saSym->Abs<float*>(addr::SetGravity) = -fGravity;
}

float Game::FindGroundZForCoord(float fX, float fY, float fZ)
{
	//spdlog::info("Game::FindGroundZForCoord: {}, {}, {}", fX, fY, fZ);

	float fPosZ;
	ScriptCommand(&get_ground_z, fX, fY, fZ, &fPosZ);
	return fPosZ;
}

void Game::DrawGangZone(float fPos[], uint32_t dwColor, bool bFullMap)
{
	//spdlog::info("Game::DrawGangZone: {}, {}, {}", fPos[0], fPos[1], fPos[2]);

	// CRadar::DrawAreaOnRadar
	eng::fn::CRadar_DrawAreaOnRadar( fPos, &dwColor, bFullMap);
}

// Disable scene entry and exit
void Game::DisableEnterExits()
{
	//spdlog::info("Game::DisableEnterExits");

	typedef sa::CPool<sa::CEntryExit, sa::CEntryExit> CEntryExitPool;
	auto ppPool = eng::as<CEntryExitPool**>(eng::Sym::CEntryExitManager_mp_poolEntryExits);
	if (!ppPool || !*ppPool) return;
	auto poolEntryExit = *ppPool;

	if (!poolEntryExit->m_aStorage || poolEntryExit->m_nSize <= 0) {
		return;
	}

	for (int i = 0; i < poolEntryExit->m_nSize; i++) {
		sa::CEntryExit* pEntry = &poolEntryExit->m_aStorage[i];
		pEntry->flags = 0;
	}
}

bool Game::IsAnimationLoaded(const char* szAnimLib)
{
	//spdlog::info("Game::IsAnimationLoaded: {}", szAnimLib);

	// CAnimManager::GetAnimationBlock
	auto pAnimBlock = eng::fn::CAnimManager_GetAnimationBlock<CAnimBlock*>( szAnimLib);
	if (pAnimBlock == nullptr) return false;
	return pAnimBlock->m_loaded;

	//return ScriptCommand(&is_animation_loaded, szAnimLib);
}

void Game::RequestAnimation(const char* szAnimLib)
{
	//spdlog::info("Game::RequestAnimation: {}", szAnimLib);
	ScriptCommand(&request_animation, szAnimLib);
}

/**
 * @param iSound
 * @param fX
 * @param fY
 * @param fZ
 */
void Game::PlaySound(int iSound, float fX, float fY, float fZ)
{
	//spdlog::info("Game::PlaySound: {}, {}, {}, {}", iSound, fX, fY, fZ);
	ScriptCommand(&play_sound, fX, fY, fZ, iSound);
}

/**
 * @return
 */
bool Game::IsGamePaused()
{
	auto* pPaused = eng::as<bool*>(eng::Sym::CTimer_m_UserPause);
	return pPaused ? *pPaused : false;
}

/**
 * @param pPed
 * @return
 */
bool Game::RemovePlayer(CPlayerPed* pPed)
{
	//spdlog::info("Game::RemovePlayer");

	if (!pPed) return false;
	bUsedPlayerSlots[pPed->m_bytePlayerNumber] = false;
	delete pPed;
	return true;
}

/**
 * @param iModel
 * @param iType
 * @param x
 * @param y
 * @param z
 * @param pdwIndex
 * @return
 */
uint32_t Game::CreatePickup(int iModel, int iType, float x, float y, float z, int* pdwIndex)
{
	//spdlog::info("Game::CreatePickup: {}, {}, {}, {}, {}, {}", iModel, iType, x, y, z, *pdwIndex);

	if (!IsValidModel(iModel)) {
		iModel = 18631;
	}

	if (!IsModelLoaded(iModel)) {
		RequestModel(iModel);
		LoadRequestedModels();

		int iWaitLoad = 0;
		while (!IsModelLoaded(iModel)) {
			usleep(5000);
			if (++iWaitLoad > 2000) {
				spdlog::error("Game::CreatePickup: model {} wouldn't load in time", iModel);
				if (pdwIndex) *pdwIndex = 0;
				return 0;
			}
		}
	}

	uint32_t hnd = 0;
	ScriptCommand(&create_pickup, iModel, iType, x, y, z, &hnd);
	int offset = 32 * (hnd & 0xFFFF);
	if (offset) offset /= 32;
	if (pdwIndex) {
		*pdwIndex = offset;
	}

	return hnd;
}

/**
 * @param iModel
 * @param vecPos
 * @param vecRot
 * @param fDrawDistance
 * @return
 */
CObject* Game::NewObject(int iModel, sa::CVector vecPos, sa::CVector vecRot, float fDrawDistance)
{
	//spdlog::info("Game::NewObject: {}, {}, {}, {}, {}", iModel, vecPos.x, vecPos.y, vecPos.z, fDrawDistance);
	auto pObject = new CObject(iModel, vecPos, vecRot, fDrawDistance, 0);
	return pObject;
}

/**
 * @param iSkin
 * @param fX
 * @param fY
 * @param fZ
 * @param fRotation
 * @param unk
 * @param bIsNPC
 * @return
 */
CPlayerPed* Game::NewPlayer(int iSkin, float fX, float fY, float fZ, float fRotation, bool unk, bool bIsNPC)
{
	//spdlog::info("Game::NewPlayer: {}, {}, {}, {}, {}, {}, {}", iSkin, fX, fY, fZ, fRotation, unk, bIsNPC);
	uint8_t bytePedSlot = FindFirstFreePlayerPedSlot();
	if (!bytePedSlot) return nullptr;
	auto pPed = new CPlayerPed(bytePedSlot, iSkin, fX, fY, fZ, fRotation);
	if (!pPed->m_ped) {
		delete pPed;
		return nullptr;
	}
	bUsedPlayerSlots[bytePedSlot] = true;
	return pPed;
}

/**
 * @param iVehicleType
 * @param fX
 * @param fY
 * @param fZ
 * @param fRotation
 * @param bAddSiren
 * @return
 */
CVehicle* Game::NewVehicle(int iVehicleType, float fX, float fY, float fZ, float fRotation, bool bAddSiren)
{
	//spdlog::info("Game::NewVehicle: {}, {}, {}, {}, {}, {}", iVehicleType, fX, fY, fZ, fRotation, bAddSiren);
	bool bPreloaded = false;
	if (iVehicleType >= 400 && iVehicleType <= 611 && m_preloadedVehicleModels[iVehicleType - 400]) {
		bPreloaded = true;
	}

	auto pVehicle = new CVehicle(iVehicleType, fX, fY, fZ, fRotation, bPreloaded, bAddSiren);
	return pVehicle;
}
