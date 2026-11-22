#include "../Game/Game.h"
#include "../Client.h"
#include "NetGame.h"
#include "../UI/UI.h"
#include "../Java/Java.h"
#include <RakNet/GetTime.h>
#include <cmath>

static inline bool IsFiniteVec(const sa::CVector& v)
{
	return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

#define INIT_RPC_BS(bs) \
	if (!rpcParams || !rpcParams->input || rpcParams->numberOfBitsOfData <= 0 || !pNetGame) return; \
	auto Data = reinterpret_cast<unsigned char*>(rpcParams->input); \
	int iBitLength = rpcParams->numberOfBitsOfData; \
	RakNet::BitStream bs(Data, BITS_TO_BYTES(iBitLength), false)

extern UI* pUI;
extern Game* pGame;
extern NetGame* pNetGame;

extern int iNetModeNormalOnFootSendRate;
extern int iNetModeNormalInCarSendRate;
extern int iNetModeFiringSendRate;
extern int iNetModeSendMultiplier;

#define REJECT_REASON_BAD_VERSION 1
#define REJECT_REASON_BAD_NICKNAME 2
#define REJECT_REASON_BAD_MOD 3
#define REJECT_REASON_BAD_PLAYERID 4

int g_iLagCompensationMode = 0;

void ProcessIncommingEvent(PLAYERID playerID, int iEventType, uint32_t dwParam1, uint32_t dwParam2, uint32_t dwParam3);

void InitGame(RPCParameters* rpcParams)
{
	spdlog::info("RPC: InitGame");
	INIT_RPC_BS(bsInitGame);

	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	if (!pPlayerPool) return;

	PLAYERID MyPlayerID = 0;
	int iLagCompensation = 0;
	bool bStuntBonus = false;
	bool bLanMode = false;
	//read server settings - bail out on truncated packets so we never apply garbage state
	bool bReadOk = true;
	bReadOk &= bsInitGame.Read(pNetGame->m_netSet->zoneNames);
	bReadOk &= bsInitGame.Read(pNetGame->m_netSet->useCJWalk);
	bReadOk &= bsInitGame.Read(pNetGame->m_netSet->allowWeapons);
	bReadOk &= bsInitGame.Read(pNetGame->m_netSet->limitGlobalChatRadius);
	bReadOk &= bsInitGame.Read(pNetGame->m_netSet->globalChatRadius);
	bReadOk &= bsInitGame.Read(bStuntBonus);
	bReadOk &= bsInitGame.Read(pNetGame->m_netSet->nameTagDrawDistance);
	bReadOk &= bsInitGame.Read(pNetGame->m_netSet->disableInteriorEnterExits);
	bReadOk &= bsInitGame.Read(pNetGame->m_netSet->nameTagLOS);
	//pNetGame->m_netSet->nameTagLOS = true;
	bReadOk &= bsInitGame.Read(pNetGame->m_netSet->manualVehicleEngineAndLight);
	bReadOk &= bsInitGame.Read(pNetGame->m_netSet->spawnsAvailable);
	bReadOk &= bsInitGame.Read(MyPlayerID);
	bReadOk &= bsInitGame.Read(pNetGame->m_netSet->showNameTags);
	bReadOk &= bsInitGame.Read(pNetGame->m_netSet->showPlayerMarkers);
	bReadOk &= bsInitGame.Read(pNetGame->m_netSet->worldTime_Hour);
	bReadOk &= bsInitGame.Read(pNetGame->m_netSet->weather);
	bReadOk &= bsInitGame.Read(pNetGame->m_netSet->gravity);
	bReadOk &= bsInitGame.Read(bLanMode);
	bReadOk &= bsInitGame.Read(pNetGame->m_netSet->deathDropMoney);
	bReadOk &= bsInitGame.Read(pNetGame->m_netSet->instagib);

	bReadOk &= bsInitGame.Read(iNetModeNormalOnFootSendRate);
	bReadOk &= bsInitGame.Read(iNetModeNormalInCarSendRate);
	bReadOk &= bsInitGame.Read(iNetModeFiringSendRate);
	bReadOk &= bsInitGame.Read(iNetModeSendMultiplier);
	bReadOk &= bsInitGame.Read(iLagCompensation);
	if (!bReadOk) {
		spdlog::warn("InitGame RPC truncated, ignoring");
		return;
	}

	switch (iLagCompensation) {
		case 1: g_iLagCompensationMode = 0;
			break;
		case 2: g_iLagCompensationMode = 1;
			break;
		default: g_iLagCompensationMode = 2;
			break;
	}

	if (pGame) {
		pGame->DisableAutoAim();
	}

	uint8_t byteStrLen = 0;
	if (!bsInitGame.Read(byteStrLen)) {
		return;
	}
	memset(pNetGame->m_hostName, 0, sizeof(pNetGame->m_hostName));
	if (byteStrLen) {
		if (!bsInitGame.Read(pNetGame->m_hostName, byteStrLen)) {
			return;
		}
	}
	pNetGame->m_hostName[byteStrLen] = 0;

	uint8_t byteVehicleModels[212];
	if (!bsInitGame.Read((char*) byteVehicleModels, 212)) {
		return;
	}
//	pGame->PreloadVehicleModels(byteVehicleModels);

	bsInitGame.Read(pNetGame->m_netSet->vehicleFriendlyFire);
	pPlayerPool->SetLocalPlayerID(MyPlayerID);
	pGame->EnableStuntBonus(bStuntBonus);

	if (bLanMode) {
		pNetGame->m_lanMode = true;
	}

	pNetGame->InitGameLogic();
	pGame->SetGravity(pNetGame->m_netSet->gravity);

	if (pNetGame->m_netSet->disableInteriorEnterExits) {
		pGame->DisableEnterExits();
	}

	if (pNetGame->m_netSet->vehicleFriendlyFire) {
		//InstallHook_VehicleFriendlyFire();
	}

	pGame->SetWorldWeather(pNetGame->m_netSet->weather);
	pGame->ToggleCJWalk(pNetGame->m_netSet->useCJWalk);

	spdlog::info("szHostName, {}", pNetGame->m_hostName);
	spdlog::info("vehicleFriendlyFire, {}", pNetGame->m_netSet->vehicleFriendlyFire);

	if (pUI) {
		pUI->chat()->addDebugMessage("Connected to {B9C9BF}%s", Encoding::cp2utf(pNetGame->m_hostName).c_str());
		pUI->voicebutton()->setVisible(Settings::voice() && Settings::voiceMicEnable());
	}

	pNetGame->SetGameState(GAMESTATE_CONNECTED);

	pPlayerPool->GetLocalPlayer()->HandleClassSelection();
}

//Prompt box rpc processing
void DialogBox(RPCParameters* rpcParams)
{
	spdlog::info("RPC: DialogBox");
	INIT_RPC_BS(bsData);

	int16_t wDialogID = 0;
	uint8_t byteDialogStyle = 0;
	uint8_t byteLength = 0;
	char szTitle[256 + 1] = {0};
	char szButton1[256 + 1] = {0};
	char szButton2[256 + 1] = {0};
	char szMessage[4096 + 1] = {0};

	if (!bsData.Read(wDialogID) || !bsData.Read(byteDialogStyle)) return;

	if (!bsData.Read(byteLength) || (byteLength > 0 && !bsData.Read(szTitle, byteLength))) return;
	szTitle[byteLength] = '\0';

	if (!bsData.Read(byteLength) || (byteLength > 0 && !bsData.Read(szButton1, byteLength))) return;
	szButton1[byteLength] = '\0';

	if (!bsData.Read(byteLength) || (byteLength > 0 && !bsData.Read(szButton2, byteLength))) return;
	szButton2[byteLength] = '\0';

	stringCompressor->DecodeString(szMessage, 4096, &bsData);

	spdlog::info("wDialogID {}", wDialogID);
	spdlog::info("byteDialogStyle {}", (int) byteDialogStyle);
	spdlog::info("szTitle {}", szTitle);
	spdlog::info("szButton1 {}", szButton1);
	spdlog::info("szButton2 {}", szButton2);
	spdlog::info("szMessage {}", szMessage);

	if (g_java) {
		std::string uTitle = Encoding::cp2utf(szTitle);
		std::string uMessage = Encoding::cp2utf(szMessage);
		std::string uBtn1 = Encoding::cp2utf(szButton1);
		std::string uBtn2 = Encoding::cp2utf(szButton2);
		g_java->showDialog(wDialogID, byteDialogStyle,
			const_cast<char*>(uTitle.c_str()),
			const_cast<char*>(uMessage.c_str()),
			const_cast<char*>(uBtn1.c_str()),
			const_cast<char*>(uBtn2.c_str()));
	}
}

//chat handling
void Chat(RPCParameters* rpcParams)
{
	INIT_RPC_BS(bsData);

	if (pNetGame->GetGameState() != GAMESTATE_CONNECTED) {
		return;
	}

	PLAYERID playerId = INVALID_PLAYER_ID;
	uint8_t byteLen = 0;
	unsigned char szText[256 + 1] = {0};

	if (!bsData.Read(playerId) || !bsData.Read(byteLen)) return;
	if (byteLen > 0 && !bsData.Read((char*) szText, byteLen)) return;
	szText[byteLen] = '\0';

	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	if (!pPlayerPool) return;
	if (playerId == pPlayerPool->GetLocalPlayerID()) {
		if (pUI && pUI->chat() && pPlayerPool->GetLocalPlayer()) {
			pUI->chat()->addChatMessage(Encoding::cp2utf((char*) szText), Encoding::cp2utf(pPlayerPool->GetLocalPlayerName()), pPlayerPool->GetLocalPlayer()->GetPlayerColorAsRGBA());
		}
	}
	else {
		CRemotePlayer* pRemotePlayer = pPlayerPool->GetAt(playerId);
		if (pRemotePlayer) {
			pRemotePlayer->Say((char*) szText);
		}
	}
}

//client message
void ClientMessage(RPCParameters* rpcParams)
{
	spdlog::info("RPC: Client message ");
	INIT_RPC_BS(bsData);

	uint32_t dwStrLen = 0, dwColor = 0;

	if (!bsData.Read(dwColor) || !bsData.Read(dwStrLen)) return;

	if (dwStrLen == 0 || dwStrLen > 4096 || (int) (dwStrLen * 8) > bsData.GetNumberOfUnreadBits()) {
		return;
	}

	char* szMsg = (char*) malloc(dwStrLen + 1);
	if (!szMsg) return;

	if (!bsData.Read(szMsg, dwStrLen)) {
		free(szMsg);
		return;
	}

	szMsg[dwStrLen] = '\0';

	spdlog::info("dwColor {}", dwColor);
	spdlog::info("dwStrLen {}", dwStrLen);
	spdlog::info("szMsg {}", szMsg);

	if (pUI && pUI->chat()) {
		pUI->chat()->addClientMessage(Encoding::cp2utf(szMsg), dwColor);
	}

	free(szMsg);
}

void RequestClass(RPCParameters* rpcParams)
{
	spdlog::info("RPC: Request class");
	INIT_RPC_BS(bsData);

	uint8_t byteRequestOutcome = 0;
	PLAYER_SPAWN_INFO SpawnInfo{};
	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	if (!pPlayerPool || !pPlayerPool->GetLocalPlayer()) return;
	CLocalPlayer* pLocalPlayer = pPlayerPool->GetLocalPlayer();

	if (!bsData.Read(byteRequestOutcome)) return;
	if (Client::isDL()) {
		PLAYER_SPAWN_INFO_DL spawnInfoDl{};
		if (!bsData.Read((char*) &spawnInfoDl, sizeof(spawnInfoDl))) return;
		SpawnInfo.byteTeam = spawnInfoDl.byteTeam;
		SpawnInfo.iSkin = spawnInfoDl.iSkin;
		SpawnInfo.unk = spawnInfoDl.unk;
		SpawnInfo.vecPos = spawnInfoDl.vecPos;
		SpawnInfo.fRotation = spawnInfoDl.fRotation;
		memcpy(SpawnInfo.iSpawnWeapons, spawnInfoDl.iSpawnWeapons, sizeof(SpawnInfo.iSpawnWeapons));
		memcpy(SpawnInfo.iSpawnWeaponsAmmo, spawnInfoDl.iSpawnWeaponsAmmo, sizeof(SpawnInfo.iSpawnWeaponsAmmo));
	}
	else {
		if (!bsData.Read((char*) &SpawnInfo, sizeof(PLAYER_SPAWN_INFO))) return;
	}

	if (byteRequestOutcome) {
		pLocalPlayer->SetSpawnInfo(&SpawnInfo);
		pLocalPlayer->HandleClassSelectionOutcome(true);
	}
	else {
		pLocalPlayer->HandleClassSelectionOutcome(false);
	}
}

//Request spawn players
void RequestSpawn(RPCParameters* rpcParams)
{
	spdlog::info("RPC: Request spawn");
	INIT_RPC_BS(bsData);

	uint8_t byteRequestOutcome = 0;
	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	if (!pPlayerPool || !pPlayerPool->GetLocalPlayer()) return;
	CLocalPlayer* pLocalPlayer = pPlayerPool->GetLocalPlayer();

	if (!bsData.Read(byteRequestOutcome)) return;
	spdlog::info("byteRequestOutcome {}", byteRequestOutcome);

	if (byteRequestOutcome == 2 || (byteRequestOutcome && pLocalPlayer->m_bWaitingForSpawnRequestReply)) {
		pLocalPlayer->Spawn();
	}
	else {
		pLocalPlayer->m_bWaitingForSpawnRequestReply = false;
	}
}

void ToggleClock(RPCParameters* rpcParams)
{
	spdlog::info("RPC: Toggle clock");
	INIT_RPC_BS(bsData);

	uint8_t byteClock = 0;
	if (!bsData.Read(byteClock)) return;
	spdlog::info("byteClock {}", byteClock);

	if (byteClock) {
		pNetGame->m_netSet->holdTime = false;
	}
	else {
		pNetGame->m_netSet->holdTime = true;
		int hour = 0, minute = 0;
		if (pGame) {
			pGame->GetWorldTime(&hour, &minute);
		}
		pNetGame->m_netSet->worldTime_Hour = static_cast<uint8_t>(hour);
		pNetGame->m_netSet->worldTime_Minute = static_cast<uint8_t>(minute);
	}
}

void SetTimeEx(RPCParameters* rpcParams)
{
	spdlog::info("RPC: SetTimeEx");
	INIT_RPC_BS(bsData);

	uint8_t byteHour = 0;
	uint8_t byteMinute = 0;
	if (!bsData.Read(byteHour) || !bsData.Read(byteMinute)) return;

	spdlog::info("byteHour {}", byteHour);
	spdlog::info("byteMinute {}", byteMinute);

	if (pGame) pGame->SetWorldTime(byteHour, byteMinute);
	pNetGame->m_netSet->worldTime_Hour = byteHour;
	pNetGame->m_netSet->worldTime_Minute = byteMinute;
}

void Weather(RPCParameters* rpcParams)
{
	spdlog::info("RPC: Weather");
	INIT_RPC_BS(bsData);

	uint8_t byteWeather = 0;
	if (!bsData.Read(byteWeather)) return;

	spdlog::info("weather {}", byteWeather);

	pNetGame->m_netSet->weather = byteWeather;
	if (pGame) pGame->SetWorldWeather(byteWeather);
}

void WorldTime(RPCParameters* rpcParams)
{
	spdlog::info("RPC: World time");
	INIT_RPC_BS(bsData);

	uint8_t byteWorldTIme = 0;
	if (!bsData.Read(byteWorldTIme)) return;

	spdlog::info("byteWorldTIme {}", byteWorldTIme);

	pNetGame->m_netSet->worldTime_Hour = byteWorldTIme;
}

void ConnectionRejected(RPCParameters* rpcParams)
{
	INIT_RPC_BS(bsData);

	uint8_t byteRejectReason = 0;
	if (!bsData.Read(byteRejectReason)) return;

	switch (byteRejectReason) {
		case REJECT_REASON_BAD_VERSION:
			if (pUI) {
				pUI->chat()->addInfoMessage("CONNECTION REJECTED: Incorrect Version.");
			}

			break;

		case REJECT_REASON_BAD_NICKNAME:
			if (pUI) {
				pUI->chat()->addInfoMessage("CONNECTION REJECTED: Unacceptable NickName");
				pUI->chat()->addInfoMessage("Please choose another nick between and 3-20 characters");
				pUI->chat()->addInfoMessage("Please use only a-z, A-Z, 0-9");
				pUI->chat()->addInfoMessage("Use /quit to exit or press ESC and select Quit Game");
			}
			pNetGame->SetGameState(GAMESTATE_WAIT_CONNECT);
			break;

		case REJECT_REASON_BAD_MOD:
			if (pUI) {
				pUI->chat()->addInfoMessage("CONNECTION REJECTED: Bad mod version.");
			}
			break;
		case REJECT_REASON_BAD_PLAYERID:
			if (pUI) {
				pUI->chat()->addInfoMessage("CONNECTION REJECTED: Unable to allocate a player slot.");
			}
			break;
	}

	pNetGame->GetRakClient()->Disconnect(500);
}

void GameModeRestart(RPCParameters* rpcParams)
{
	pNetGame->ShutdownForGameModeRestart();
}

void ServerJoin(RPCParameters* rpcParams)
{
	INIT_RPC_BS(bsData);
	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	if (!pPlayerPool) return;
	CRemotePlayer* pRemotePlayer = nullptr;
	uint8_t byteNameLen = 0;
	uint8_t byteIsNPC = 0;
	PLAYERID PlayerID = INVALID_PLAYER_ID;
	uint32_t dwMarkerColor = 0;
	char szPlayerName[256 + 1] = {0};

	if (!bsData.Read(PlayerID) || !bsData.Read(dwMarkerColor) || !bsData.Read(byteIsNPC) || !bsData.Read(byteNameLen)) {
		return;
	}
	if (byteNameLen > 0 && !bsData.Read(szPlayerName, byteNameLen)) {
		return;
	}
	szPlayerName[byteNameLen] = '\0';

	spdlog::info("PlayerID {}", PlayerID);
	spdlog::info("dwMarkerColor {}", dwMarkerColor);
	spdlog::info("byteIsNPC {}", byteIsNPC);
	spdlog::info("szPlayerName {}", szPlayerName);

	if (strlen(szPlayerName) > MAX_PLAYER_NAME) {
		return;
	}

	pPlayerPool->New(PlayerID, szPlayerName, byteIsNPC);

	pRemotePlayer = pPlayerPool->GetAt(PlayerID);

	if (dwMarkerColor && pRemotePlayer) {
		pRemotePlayer->SetPlayerColor(dwMarkerColor);
	}
}

void ServerQuit(RPCParameters* rpcParams)
{
	spdlog::info("RPC: Server quit");
	INIT_RPC_BS(bsData);

	PLAYERID playerId = INVALID_PLAYER_ID;
	uint8_t byteReason = 0;
	if (!bsData.Read(playerId) || !bsData.Read(byteReason)) return;

	if (pNetGame->GetPlayerPool()) {
		pNetGame->GetPlayerPool()->Delete(playerId, byteReason);
	}
}

void WorldPlayerAdd(RPCParameters* rpcParams)
{
	spdlog::info("RPC: World player add");
	INIT_RPC_BS(bsData);

	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	CRemotePlayer* pRemotePlayer = nullptr;
	CPlayerPed* pPlayerPed = nullptr;
	PLAYERID playerId = INVALID_PLAYER_ID;
	uint8_t byteTeam = 0;
	int iSkin = 0;
	sa::CVector vecPos{};
	float fRotation = 0.0f;
	uint32_t dwColor = 0;
	uint8_t byteFightingStyle = 0;
	uint16_t wUnkData[11] = {0};

	if (!bsData.Read(playerId) || !bsData.Read(byteTeam) || !bsData.Read(iSkin)) return;
	if (Client::isDL()) {
		int iCustomSkin = 0;
		if (!bsData.Read(iCustomSkin)) return;
	}
	if (!bsData.Read(vecPos.x) || !bsData.Read(vecPos.y) || !bsData.Read(vecPos.z) ||
			!bsData.Read(fRotation) || !bsData.Read(dwColor) || !bsData.Read(byteFightingStyle)) {
		return;
	}
	bsData.Read((char*) wUnkData, sizeof(wUnkData));
	if (!IsFiniteVec(vecPos) || !std::isfinite(fRotation)) return;

	if (pPlayerPool) {
		pRemotePlayer = pPlayerPool->GetAt(playerId);
		if (pRemotePlayer) {
			if (pRemotePlayer->Spawn(byteTeam, iSkin, &vecPos, fRotation, dwColor, byteFightingStyle)) {
				pPlayerPed = pRemotePlayer->GetPlayerPed();
				if (pPlayerPed) {
					for (int i = 0; i < 11; i++) {
						pPlayerPed->SetSkillLevel(i, wUnkData[i]);
					}
				}

				if (byteTeam != 0xFF) {
					pRemotePlayer->SetTeam(byteTeam);
				}

				if (pPlayerPool->IsPlayerNPC(playerId)) {
					if (pPlayerPed) {
						pPlayerPed->SetImmunities(0, 1, 1, 0, 0);
					}
				}
			}
			else {
				if (pUI && pUI->chat()) {
					pUI->chat()->addDebugMessage("Warning: Couldn't spawn player(%u)", playerId);
				}
			}
		}
	}
}

void WorldPlayerRemove(RPCParameters* rpcParams)
{
	spdlog::info("RPC: World player remove");
	INIT_RPC_BS(bsData);

	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	CRemotePlayer* pRemotePlayer = nullptr;
	PLAYERID playerId = INVALID_PLAYER_ID;

	if (!bsData.Read(playerId)) return;
	spdlog::info("playerId {}", playerId);
	if (pPlayerPool) {
		pRemotePlayer = pPlayerPool->GetAt(playerId);
		if (pRemotePlayer) {
			pRemotePlayer->Remove();
		}
	}
}

void WorldPlayerDeath(RPCParameters* rpcParams)
{
	spdlog::info("RPC: Player Death");
	INIT_RPC_BS(bsData);

	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	CRemotePlayer* pRemotePlayer = nullptr;
	PLAYERID playerId = INVALID_PLAYER_ID;

	if (!bsData.Read(playerId)) return;
	if (pPlayerPool) {
		pRemotePlayer = pPlayerPool->GetAt(playerId);
		if (pRemotePlayer) {
			pRemotePlayer->HandleDeath();
		}
	}
}

void WorldVehicleAdd(RPCParameters* rpcParams)
{
	spdlog::info("RPC: World vehicle add");
	INIT_RPC_BS(bsData);

	NEW_VEHICLE VehicleInfo{};
	VEHICLE_MOD VehicleModinfo{};

	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
	if (!pVehiclePool) {
		return;
	}

	if (!bsData.Read((char*) &VehicleInfo, sizeof(NEW_VEHICLE)) ||
			!bsData.Read((char*) &VehicleModinfo, sizeof(VEHICLE_MOD))) {
		return;
	}

	if (VehicleInfo.iVehicleType < 400 || VehicleInfo.iVehicleType > 611 ||
			VehicleInfo.VehicleID >= MAX_VEHICLES ||
			!IsFiniteVec(VehicleInfo.vecPos) || !std::isfinite(VehicleInfo.fRotation)) {
		return;
	}

	pVehiclePool->New(&VehicleInfo);
	CVehicle* pVehicle = pVehiclePool->GetAt(VehicleInfo.VehicleID);
	if (!pVehicle) {
		return;
	}

	bool bIsTrain = false;

	if (VehicleInfo.iVehicleType == 537) {
		VehicleInfo.iVehicleType = 569;
		bIsTrain = true;
	}

	if (VehicleInfo.iVehicleType == 538) {
		VehicleInfo.iVehicleType = 570;
		bIsTrain = true;
	}

	if (bIsTrain) {
		VehicleInfo.VehicleID++;
		pVehiclePool->New(&VehicleInfo);
		VehicleInfo.VehicleID++;
		pVehiclePool->New(&VehicleInfo);
		VehicleInfo.VehicleID++;
		pVehiclePool->New(&VehicleInfo);
	}

	if (pVehicle->GetVehicleSubtype() == VEHICLE_SUBTYPE_CAR) {
		for (int i = 0; i < 14; i++) {
			uint8_t byteMod = VehicleModinfo.byteModSlots[i];
			if (byteMod) {
				pVehicle->AddComponent(byteMod + 999);
			}
		}

		if (VehicleModinfo.bytePaintJob) {
			pVehicle->SetPaintJob(VehicleModinfo.bytePaintJob - 1);
		}
	}

	if (VehicleModinfo.iBodyColor1 != -1 || VehicleModinfo.iBodyColor2 != -1) {
		pVehicle->SetColor(VehicleModinfo.iBodyColor1, VehicleModinfo.iBodyColor2);
	}
}

void WorldVehicleRemove(RPCParameters* rpcParams)
{
	spdlog::info("RPC: World vehicle remove");
	INIT_RPC_BS(bsData);

	VEHICLEID VehicleID = INVALID_VEHICLE_ID;
	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
	if (!pVehiclePool) {
		return;
	}

	if (!bsData.Read(VehicleID)) {
		return;
	}

	spdlog::info("VehicleID {}", VehicleID);

	CVehicle* pVehicle = pVehiclePool->GetAt(VehicleID);
	if (!pVehicle) {
		return;
	}

	int iModel = pVehicle->GetModelIndex();
	pVehiclePool->Delete(VehicleID);

	if (iModel == 537 || iModel == 538) {
		pVehiclePool->Delete(VehicleID + 1);
		pVehiclePool->Delete(VehicleID + 2);
		pVehiclePool->Delete(VehicleID + 3);
	}
}

void TimerUpdate(RPCParameters* rpcParams)
{
	INIT_RPC_BS(bsData);
	uint32_t dwTime = 0;

	if (!bsData.Read(dwTime)) return;
	spdlog::info("dwTime {}", dwTime);

	if (pGame) pGame->UpdateGlobalTimer(dwTime);
}

void ScmEvent(RPCParameters* rpcParams)
{
	spdlog::info("RPC: SCM event");
	INIT_RPC_BS(bsData);

	PLAYERID playerId = INVALID_PLAYER_ID;
	int iEvent = 0;
	uint32_t dwParam1 = 0, dwParam2 = 0, dwParam3 = 0;

	if (!bsData.Read(playerId) || !bsData.Read(iEvent) ||
			!bsData.Read(dwParam1) || !bsData.Read(dwParam2) || !bsData.Read(dwParam3)) {
		return;
	}

	spdlog::info("playerId {}", playerId);
	spdlog::info("iEvent {}", iEvent);
	spdlog::info("dwParam1 {}", dwParam1);
	spdlog::info("dwParam2 {}", dwParam2);
	spdlog::info("dwParam3 {}", dwParam3);

	ProcessIncommingEvent(playerId, iEvent, dwParam1, dwParam2, dwParam3);
}

void UpdateScoresPingsIPs(RPCParameters* rpcParams)
{
	spdlog::info("RPC: Update scores and pings");
	INIT_RPC_BS(bsData);

	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	if (!pPlayerPool) return;

	PLAYERID playerId = INVALID_PLAYER_ID;
	int iPlayerScore = 0;
	int iPlayerPing = 0;

	for (int i = 0; i < (iBitLength / 8) / 10; i++) {
		if (!bsData.Read(playerId) || !bsData.Read(iPlayerScore) || !bsData.Read(iPlayerPing)) {
			break;
		}

		spdlog::info("RPC_PINGIP: ({}): score: {}, ping: {}", playerId, iPlayerScore, iPlayerPing);

		pPlayerPool->UpdatePlayerScore(playerId, iPlayerScore);
		pPlayerPool->UpdatePlayerPing(playerId, iPlayerPing);
	}
}

void Pickup(RPCParameters* rpcParams)
{
	spdlog::info("RPC: Pickup");
	INIT_RPC_BS(bsData);

	int iIndex = -1;
	PICKUP Pickup{};
	if (!bsData.Read(iIndex) || !bsData.Read((char*) &Pickup, sizeof(PICKUP))) return;

	CPickupPool* pPickupPool = pNetGame->GetPickupPool();
	if (pPickupPool) {
		pPickupPool->New(&Pickup, iIndex);
	}
}

//destroy pickup
void DestroyPickup(RPCParameters* rpcParams)
{
	spdlog::info("RPC: Destroy pickup");
	INIT_RPC_BS(bsData);

	int iIndex = -1;
	if (!bsData.Read(iIndex)) return;
	//Get pickup pool
	CPickupPool* pPickupPool = pNetGame->GetPickupPool();
	if (pPickupPool) {
		//destroyed by index
		pPickupPool->Destroy(iIndex);
	}
}

//destroy dropped weapon pickup by owner (0.3.DL, RPC 151)
void DestroyWeaponPickup(RPCParameters* rpcParams)
{
	spdlog::info("RPC: Destroy weapon pickup");
	INIT_RPC_BS(bsData);

	uint8_t byteOwner = 0;
	if (!bsData.Read(byteOwner)) return;

	CPickupPool* pPickupPool = pNetGame->GetPickupPool();
	if (pPickupPool) {
		pPickupPool->DestroyWeaponByOwner(byteOwner);
	}
}

//anti-cheat probe (0.3.DL, RPC 103): we can't checksum PC-side memory
//structures on Android, so we answer well-formed replies - result 0 for
//checksum types, real RakNet time for the 0x48 keep-alive
void ClientCheck(RPCParameters* rpcParams)
{
	INIT_RPC_BS(bsData);

	uint8_t byteType = 0;
	uint32_t dwArgument = 0;
	uint16_t wOffset = 0, wLength = 0;
	if (!bsData.Read(byteType)) return;
	if (!bsData.Read(dwArgument)) return;
	if (!bsData.Read(wOffset)) return;
	if (!bsData.Read(wLength)) return;

	if (wOffset > 256 || wLength > 256 || wLength < 2) return;

	uint8_t byteResult = 0;
	if (byteType == 0x48) {
		dwArgument = (uint32_t) RakNet::GetTime();
	}

	RakNet::BitStream bsReply;
	bsReply.Write(byteType);
	bsReply.Write(dwArgument);
	bsReply.Write(byteResult);
	if (pNetGame->GetRakClient()) {
		pNetGame->GetRakClient()->RPC(&RPC_ClientCheck, &bsReply, HIGH_PRIORITY, RELIABLE_ORDERED, 0, false, UNASSIGNED_NETWORK_ID, nullptr);
	}
}

//Create 3D text
void Create3DTextLabel(RPCParameters* rpcParams)
{
	spdlog::info("RPC: Create 3D text label");
	INIT_RPC_BS(bsData);

	CTextLabelPool* pTextLabelPool = pNetGame->GetTextLabelPool();
	if (pTextLabelPool == nullptr) {
		return;
	}

	uint16_t wLabelId = 0xFFFF;
	TEXT_LABEL label{};
	char szBuffer[2048 + 1] = {0};

	if (!bsData.Read(wLabelId) || !bsData.Read(label.dwColor) ||
			!bsData.Read(label.vecPos.x) || !bsData.Read(label.vecPos.y) || !bsData.Read(label.vecPos.z) ||
			!bsData.Read(label.fDistance) || !bsData.Read(label.bTestLOS) ||
			!bsData.Read(label.playerId) || !bsData.Read(label.vehicleId)) {
		return;
	}
	if (!IsFiniteVec(label.vecPos) || !std::isfinite(label.fDistance)) return;

	stringCompressor->DecodeString(szBuffer, 2048, &bsData);
	strncpy(label.text, szBuffer, sizeof(label.text) - 1);
	label.text[sizeof(label.text) - 1] = '\0';

	if (wLabelId < MAX_TEXT_LABELS) {
		pTextLabelPool->NewLabel(wLabelId, &label);
	}
}

//update 3d text
void Update3DTextLabel(RPCParameters* rpcParams)
{
	spdlog::info("RPC: Update 3D text label");
	INIT_RPC_BS(bsData);

	CTextLabelPool* pTextLabelPool = pNetGame->GetTextLabelPool();
	if (pTextLabelPool == nullptr) {
		return;
	}

	uint16_t wLabelId = 0xFFFF;
	if (!bsData.Read(wLabelId)) return;

	if (pTextLabelPool->GetSlotState(wLabelId)) {
		pTextLabelPool->ClearLabel(wLabelId);
	}
}

void SetCheckpoint(RPCParameters* rpcParams)
{
	spdlog::info("RPC: Set checkpoint");
	INIT_RPC_BS(bsData);

	sa::CVector vecPos{};
	float fSize = 0.0f;
	if (!bsData.Read(vecPos.x) || !bsData.Read(vecPos.y) || !bsData.Read(vecPos.z) || !bsData.Read(fSize)) {
		return;
	}
	if (!IsFiniteVec(vecPos) || !std::isfinite(fSize)) return;

	sa::CVector vecSize;
	vecSize.x = fSize;
	vecSize.y = fSize;
	vecSize.z = fSize;

	if (pGame) {
		pGame->SetCheckpointInformation(&vecPos, &vecSize);
		pGame->EnableCheckpoints(true);
	}
}

void DisableCheckpoint(RPCParameters* rpcParams)
{
	spdlog::info("RPC: Disable checkpoint");
	if (pGame) pGame->EnableCheckpoints(false);
}

void SetRaceCheckpoint(RPCParameters* rpcParams)
{
	spdlog::info("RPC: Set race checkpoint");
	INIT_RPC_BS(bsData);

	uint8_t byteType = 0;
	sa::CVector vecPos{};
	sa::CVector vecNextPos{};
	float fRadius = 0.0f;
	if (!bsData.Read(byteType) ||
			!bsData.Read(vecPos.x) || !bsData.Read(vecPos.y) || !bsData.Read(vecPos.z) ||
			!bsData.Read(vecNextPos.x) || !bsData.Read(vecNextPos.y) || !bsData.Read(vecNextPos.z) ||
			!bsData.Read(fRadius)) {
		return;
	}
	if (!IsFiniteVec(vecPos) || !IsFiniteVec(vecNextPos) || !std::isfinite(fRadius)) return;

	if (pGame) {
		pGame->SetRaceCheckpointInformation(byteType, &vecPos, &vecNextPos, fRadius);
		pGame->EnableRaceCheckpoints(true);
	}
}

void DisableRaceCheckpoint(RPCParameters* rpcParams)
{
	spdlog::info("RPC: Disable race checkpoint");
	if (pGame) pGame->EnableRaceCheckpoints(false);
}

void DamageVehicle(RPCParameters* rpcParams)
{
	spdlog::info("RPC: Damage vehicle");
	INIT_RPC_BS(bsData);

	VEHICLEID VehicleID = INVALID_VEHICLE_ID;
	uint32_t dwPanels = 0;
	uint32_t dwDoors = 0;
	uint8_t byteLight = 0;
	uint8_t byteTires = 0;
	if (!bsData.Read(VehicleID) || !bsData.Read(dwPanels) || !bsData.Read(dwDoors) ||
			!bsData.Read(byteLight) || !bsData.Read(byteTires)) {
		return;
	}

	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
	if (!pVehiclePool) {
		return;
	}
	CVehicle* pVehicle = pVehiclePool->GetAt(VehicleID);
	if (pVehicle) {
		pVehicle->SetDamageStatus(dwPanels, dwDoors, byteLight);
		pVehicle->SetTireDamageStatus(byteTires);
	}
}

void EnterVehicle(RPCParameters* rpcParams)
{
	spdlog::info("RPC: Enter vehicle");
	INIT_RPC_BS(bsData);

	PLAYERID PlayerID = INVALID_PLAYER_ID;
	VEHICLEID VehicleID = INVALID_VEHICLE_ID;
	uint8_t bytePassenger = 0;
	if (!bsData.Read(PlayerID) || !bsData.Read(VehicleID) || !bsData.Read(bytePassenger)) {
		return;
	}

	if (bytePassenger) {
		bytePassenger = 1;
	}

	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	if (!pPlayerPool || VehicleID >= MAX_VEHICLES) {
		return;
	}
	CRemotePlayer* pRemotePlayer = pPlayerPool->GetAt(PlayerID);
	if (pRemotePlayer) {
		if (pRemotePlayer->GetDistanceFromLocalPlayer() < 200.0f) {
			pRemotePlayer->EnterVehicle(VehicleID, bytePassenger);
		}
	}
}

void ExitVehicle(RPCParameters* rpcParams)
{
	spdlog::info("RPC: Exit vehicle");
	INIT_RPC_BS(bsData);

	PLAYERID PlayerID = INVALID_PLAYER_ID;
	VEHICLEID VehicleID = INVALID_VEHICLE_ID;
	if (!bsData.Read(PlayerID) || !bsData.Read(VehicleID)) {
		return;
	}

	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	if (!pPlayerPool) {
		return;
	}
	CRemotePlayer* pRemotePlayer = pPlayerPool->GetAt(PlayerID);
	if (pRemotePlayer) {
		if (pRemotePlayer->GetDistanceFromLocalPlayer() < 200.0f) {
			pRemotePlayer->ExitVehicle();
		}
	}
}

//Vehicle parameters
void VehicleParamsEx(RPCParameters* rpcParams)
{
	spdlog::info("RPC: VehicleParamsEx");
	INIT_RPC_BS(bsData);

	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
	if (!pVehiclePool) {
		return;
	}

	VEHICLEID VehicleID = INVALID_VEHICLE_ID;
	VEHICLE_PARAMS_EX vehParamsEx{};
	if (!bsData.Read(VehicleID) || !bsData.Read((char*) &vehParamsEx, sizeof(VEHICLE_PARAMS_EX))) {
		return;
	}

	CVehicle* pVehicle = pVehiclePool->GetAt(VehicleID);
	if (!pVehicle) {
		return;
	}
	//engine status
	if (vehParamsEx.byteEngine <= 1) {
		pVehicle->SetEngineState(vehParamsEx.byteEngine == 1);
	}

	if (vehParamsEx.byteLight <= 1) {
		pVehicle->SetLightState(vehParamsEx.byteLight == 1);
	}
	//door state
	if (vehParamsEx.byteDoors <= 1) {
		pVehicle->SetDoorState(vehParamsEx.byteDoors == 1);
	}
	//vehicle object
	if (vehParamsEx.byteObjective <= 1) {
		pVehicle->m_byteObjectiveVehicle = vehParamsEx.byteObjective == 1;
	}

	if (vehParamsEx.byteBoot <= 1) {
		pVehicle->SetComponentOpenState(1, 17, vehParamsEx.byteBoot == 1 ? 1.0f : 0.0f);
	}

	if (vehParamsEx.byteBonnet <= 1) {
		pVehicle->SetComponentOpenState(0, 16, vehParamsEx.byteBonnet == 1 ? 1.0f : 0.0f);
	}

	if (vehParamsEx.byteAlarm <= 1) {
		pVehicle->SetSirenOn(vehParamsEx.byteAlarm == 1);
		pVehicle->SetAlarmState(vehParamsEx.byteAlarm == 1 ? 20000 : 0);
	}

	if (vehParamsEx.byteSiren <= 1) {
		pVehicle->SetSirenOn(vehParamsEx.byteSiren == 1);
	}

	if (vehParamsEx.byteDoor1 <= 1) {
		pVehicle->SetComponentOpenState(2, 10, vehParamsEx.byteDoor1 == 1 ? 1.0f : 0.0f);
	}

	if (vehParamsEx.byteDoor2 <= 1) {
		pVehicle->SetComponentOpenState(3, 8, vehParamsEx.byteDoor2 == 1 ? 1.0f : 0.0f);
	}

	if (vehParamsEx.byteDoor3 <= 1) {
		pVehicle->SetComponentOpenState(4, 11, vehParamsEx.byteDoor3 == 1 ? 1.0f : 0.0f);
	}

	if (vehParamsEx.byteDoor4 <= 1) {
		pVehicle->SetComponentOpenState(5, 9, vehParamsEx.byteDoor4 == 1 ? 1.0f : 0.0f);
	}

	if (vehParamsEx.byteWindow1) {
		if (vehParamsEx.byteWindow1 == 1) {
			pVehicle->CloseWindow(10);
		}
	}
	else {
		pVehicle->OpenWindow(10);
	}

	if (vehParamsEx.byteWindow2) {
		if (vehParamsEx.byteWindow2 == 1) {
			pVehicle->CloseWindow(8);
		}
	}
	else {
		pVehicle->OpenWindow(8);
	}

	if (vehParamsEx.byteWindow3) {
		if (vehParamsEx.byteWindow3 == 1) {
			pVehicle->CloseWindow(11);
		}
	}
	else {
		pVehicle->OpenWindow(11);
	}

	if (vehParamsEx.byteWindow4) {
		if (vehParamsEx.byteWindow4 == 1) {
			pVehicle->CloseWindow(9);
		}
	}
	else {
		pVehicle->OpenWindow(9);
	}
}

//show npc
void ShowActor(RPCParameters* rpcParams)
{
	spdlog::info("RPC: Show actor");
	INIT_RPC_BS(bsData);

	CActorPool* pActorPool = pNetGame->GetActorPool();
	if (!pActorPool) {
		return;
	}

	NEW_ACTOR newActor{};
	if (Client::isDL()) {
		int iCustomSkin = 0;
		if (!bsData.Read(newActor.ActorID) || !bsData.Read(newActor.iSkin) ||
				!bsData.Read(iCustomSkin) ||
				!bsData.Read(newActor.vecPos.x) || !bsData.Read(newActor.vecPos.y) || !bsData.Read(newActor.vecPos.z) ||
				!bsData.Read(newActor.fAngle) || !bsData.Read(newActor.fHealth) || !bsData.Read(newActor.bInvulnerable)) {
			return;
		}
	}
	else {
		if (!bsData.Read((char*) &newActor, sizeof(NEW_ACTOR))) return;
	}
	if (!IsFiniteVec(newActor.vecPos) || !std::isfinite(newActor.fAngle)) return;

	pActorPool->New(&newActor);
}

//hide npc
void HideActor(RPCParameters* rpcParams)
{
	spdlog::info("RPC: Hide actor");
	INIT_RPC_BS(bsData);

	CActorPool* pActorPool = pNetGame->GetActorPool();
	if (!pActorPool) {
		return;
	}

	PLAYERID ActorID = INVALID_PLAYER_ID;
	if (!bsData.Read(ActorID)) return;

	pActorPool->Delete(ActorID);
}

//chat bubble
void ChatBubble(RPCParameters* rpcParams)
{
	spdlog::info("RPC: ChatBubble");
	INIT_RPC_BS(bsData);

	CPlayerBubblePool* pPlayerBubblePool = pNetGame->GetPlayerBubblePool();
	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	if (pPlayerBubblePool == nullptr || pPlayerPool == nullptr) {
		return;
	}

	PLAYERID playerId = INVALID_PLAYER_ID;
	uint32_t color = 0;
	float drawDistance = 0.0f;
	uint32_t duration = 0;
	uint8_t byteTextLen = 0;

	char szText[256 + 1] = {0};

	if (!bsData.Read(playerId)) return;

	if (playerId == pPlayerPool->GetLocalPlayerID() || playerId >= MAX_PLAYERS) {
		return;
	}

	if (!bsData.Read(color) || !bsData.Read(drawDistance) || !bsData.Read(duration) || !bsData.Read(byteTextLen)) {
		return;
	}
	if (byteTextLen > 0 && !bsData.Read((char*) szText, byteTextLen)) {
		return;
	}
	szText[byteTextLen] = '\0';

	pPlayerBubblePool->New(playerId, szText, color, drawDistance, duration);
}

//Edit attached object
void EditAttachedObject(RPCParameters* rpcParams)
{
	spdlog::info("RPC: EditAttachedObject");
	INIT_RPC_BS(bsData);

	int attachedObjectIndex = -1;
	if (!bsData.Read(attachedObjectIndex)) return;

	EditObject::startEditAttachedObject(attachedObjectIndex);
}

//edit object
void EditObject(RPCParameters* rpcParams)
{
	spdlog::info("RPC: EditObject");
	INIT_RPC_BS(bsData);

	bool bPlayerObject = false;
	OBJECTID objectId = INVALID_OBJECT_ID;

	if (!bsData.Read(bPlayerObject)) return;
	if (!bsData.Read(objectId)) return;

	EditObject::m_playerObj = bPlayerObject;

	// there is no player-object pool — editing a global object id here
	// would mutate an unrelated object and send bogus results back
	if (bPlayerObject) {
		spdlog::warn("RPC: EditObject - player objects unsupported, ignoring (id {})", objectId);
		if (pUI && pUI->chat()) pUI->chat()->addDebugMessage("PlayerObject editing not supported");
		return;
	}
	EditObject::startEditObject(objectId);
}

//Go to edit object
void EnterEditObject(RPCParameters* rpcParams)
{
	spdlog::info("RPC: EnterEditObject");
}

//cancel editor object
void CancelEditObject(RPCParameters* rpcParams)
{
	spdlog::info("RPC: CancelEditObject");
	EditObject::cancelEdit();
}

void RegisterRPCs(RakClientInterface* pRakClient)
{
	spdlog::info("Registering RPC's..");

	// RPC_SetPlayerSkillLevel
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrCreate3DTextLabel, Create3DTextLabel);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrUpdate3DTextLabel, Update3DTextLabel);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_DialogBox, DialogBox);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_SetCheckpoint, SetCheckpoint);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_DisableCheckpoint, DisableCheckpoint);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_SetRaceCheckpoint, SetRaceCheckpoint);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_DisableRaceCheckpoint, DisableRaceCheckpoint);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_UpdateScoresPingsIPs, UpdateScoresPingsIPs);
	// RPC_SvrStats - useless
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_GameModeRestart, GameModeRestart);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ConnectionRejected, ConnectionRejected);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ClientMessage, ClientMessage);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_WorldTime, WorldTime);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_Pickup, Pickup);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_DestroyPickup, DestroyPickup);
	// RPC_DestroyWeaponPickup(97) - unused in 037; 0.3.DL moved it to 151
	if (Client::isDL()) {
		pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrDestroyWeaponPickup, DestroyWeaponPickup);
		pRakClient->RegisterAsRemoteProcedureCall(&RPC_ClientCheck, ClientCheck);
	}
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScmEvent, ScmEvent);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_Weather, Weather);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_SetTimeEx, SetTimeEx);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ToggleClock, ToggleClock);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_TimerUpdate, TimerUpdate);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_WorldPlayerAdd, WorldPlayerAdd);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_WorldPlayerDeath, WorldPlayerDeath);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_WorldPlayerRemove, WorldPlayerRemove);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_WorldVehicleAdd, WorldVehicleAdd);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_WorldVehicleRemove, WorldVehicleRemove);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_DamageVehicle, DamageVehicle);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_VehicleParamsEx, VehicleParamsEx);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_EnterVehicle, EnterVehicle);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ExitVehicle, ExitVehicle);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ServerJoin, ServerJoin);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ServerQuit, ServerQuit);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_InitGame, InitGame);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_Chat, Chat);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_RequestClass, RequestClass);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_RequestSpawn, RequestSpawn);

	pRakClient->RegisterAsRemoteProcedureCall(&RPC_EditAttachedObject, EditAttachedObject);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_EditObject, EditObject);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_EnterEditObject, EnterEditObject);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_CancelEditObject, CancelEditObject);

	// RPC_SetTargeting
	// RPC_ClientCheckResponse
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ShowActor, ShowActor);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_HideActor, HideActor);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ChatBubble, ChatBubble);
}

void UnregisterRPCs(RakClientInterface* pRakClient)
{
	spdlog::info("Unregistering RPC's..");

	// RPC_SetPlayerSkillLevel
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrCreate3DTextLabel);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrUpdate3DTextLabel);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_DialogBox);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_SetCheckpoint);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_DisableCheckpoint);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_SetRaceCheckpoint);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_DisableRaceCheckpoint);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_UpdateScoresPingsIPs);
	// RPC_SvrStats - useless
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_GameModeRestart);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ConnectionRejected);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ClientMessage);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_WorldTime);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_Pickup);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_DestroyPickup);
	// RPC_DestroyWeaponPickup(97) - unused in 037; 0.3.DL moved it to 151
	if (Client::isDL()) {
		pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrDestroyWeaponPickup);
		pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ClientCheck);
	}
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScmEvent);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_Weather);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_SetTimeEx);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ToggleClock);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_TimerUpdate);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_WorldPlayerAdd);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_WorldPlayerDeath);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_WorldPlayerRemove);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_WorldVehicleAdd);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_WorldVehicleRemove);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_DamageVehicle);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_VehicleParamsEx);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_EnterVehicle);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ExitVehicle);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ServerJoin);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ServerQuit);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_InitGame);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_Chat);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_RequestClass);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_RequestSpawn);

	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_EditAttachedObject);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_EditObject);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_EnterEditObject);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_CancelEditObject);

	// RPC_SetTargeting
	// RPC_ClientCheckResponse
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ShowActor);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_HideActor);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ChatBubble);
}
