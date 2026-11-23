#pragma once

#include <RakNet/RakClientInterface.h>
#include <RakNet/RakNetworkFactory.h>
#include <RakNet/PacketEnumerations.h>
#include <RakNet/StringCompressor.h>

#include <RakNet/SAMP/SAMPRPC.h>

typedef unsigned short PLAYERID;
typedef unsigned short VEHICLEID;
typedef unsigned short OBJECTID;

#define GAMESTATE_WAIT_CONNECT        1
#define GAMESTATE_CONNECTING          2
#define GAMESTATE_CONNECTED           5
#define GAMESTATE_AWAIT_JOIN          6
#define GAMESTATE_RESTARTING          11

#define MAX_PLAYER_NAME               24
#define MAX_PLAYERS                   1004
#define MAX_VEHICLES                  2000
#define MAX_OBJECTS                   2100
#define MAX_ACTORS                    1000
#define MAX_PICKUPS                   4096
#define MAX_TEXT_LABELS               2048
#define MAX_TEXT_DRAWS                2304
#define MAX_GANG_ZONES                1024
#define MAX_MAP_ICONS                 100

#include "LocalPlayer.h"
#include "RemotePlayer.h"
#include "PlayerPool.h"
#include "VehiclePool.h"
#include "ObjectPool.h"
#include "ActorPool.h"
#include "PickupPool.h"
#include "TextLabelPool.h"
#include "TextDrawPool.h"
#include "PlayerBubblePool.h"
#include "GangZonePool.h"
#include "MenuPool.h"
#include "Session.h"

class NetGame {
public:
	NetGame(const char* host, int port, const char* playerName, const char* password);
	~NetGame();

	int GetGameState() { return m_gameState; }
	void SetGameState(int gameState) { m_gameState = gameState; }

	RakClientInterface* GetRakClient() { return m_rakClient; }

	CPlayerPool* GetPlayerPool() { return m_pools->playerPool; }
	CVehiclePool* GetVehiclePool() { return m_pools->vehiclePool; }
	CObjectPool* GetObjectPool() { return m_pools->objectPool; }
	CActorPool* GetActorPool() { return m_pools->actorPool; }
	CPickupPool* GetPickupPool() { return m_pools->pickupPool; }
	CTextLabelPool* GetTextLabelPool() { return m_pools->textLabelPool; }
	CTextDrawPool* GetTextDrawPool() { return m_pools->textDrawPool; }
	CPlayerBubblePool* GetPlayerBubblePool() { return m_pools->playerBubblePool; }
	CGangZonePool* GetGangZonePool() { return m_pools->gangZonePool; }
	CMenuPool* GetMenuPool() { return m_pools->menuPool; }

	void Process();

	SessionState& session() { return m_session; }
	const SessionState& session() const { return m_session; }

	void ShutdownForGameModeRestart(SessionState::ResetReason reason = SessionState::ResetReason::Restart);
	void UpdatePlayerScoresAndPings();
	void InitGameLogic();

	void SetMapIcon(uint8_t byteIconID, float fPosX, float fPosY, float fPosZ, uint8_t byteType, uint32_t dwColor, uint8_t byteStyle);
	void DisableMapIcon(uint8_t byteIconID);

	void SendDialogResponse(uint16_t wDialogID, uint8_t byteButtonID, uint16_t wListBoxItem, const char* szInput);

	void SendPrevClass();
	void SendSpawn();
	void SendNextClass();

	void SendChatMessage(const char* szMessage);
	void SendChatCommand(const char* szCommand);

private:
	void InitializePools();
	void UninitializePools();
	void UpdateNetwork();
	void ProcessPools();
	void ProcessLoadingScreen();
	void ProcessConnecting();

	void Packet_AuthKey(Packet* pkt);
	void Packet_ConnectAttemptFailed(Packet* pkt);
	void Packet_NoFreeIncomingConnections(Packet* pkt);
	void Packet_DisconnectionNotification(Packet* pkt);
	void Packet_ConnectionSucceeded(Packet* pkt);
	void Packet_FailedInitializeEncription(Packet* pkt);
	void Packet_ConnectionBanned(Packet* pkt);
	void Packet_InvalidPassword(Packet* pkt);
	void Packet_ConnectionLost(Packet* pkt);
	void Packet_PlayerSync(Packet* pkt);
	void Packet_VehicleSync(Packet* pkt);
	void Packet_PassengerSync(Packet* pkt);
	void Packet_AimSync(Packet* pkt);
	void Packet_BulletSync(Packet* pkt);
	void Packet_MarkerSync(Packet* pkt);
	void Packet_UnoccupiedSync(Packet* pkt);
	void Packet_TrailerSync(Packet* pkt);

	void ResetVehiclePool();
	void ResetObjectPool();
	void ResetActorPool();
	void ResetPickupPool();
	void ResetTextLabelPool();
	void ResetTextDrawPool();
	void ResetPlayerBubblePool();
	void ResetGangZonePool();
	void ResetMapIcons();
	void ResetMenuPool();

	void ProcessDlArtworkHandshake();

public:
	void NotifyDlVirtualWorld(int32_t virtualWorld);
	void ResetDlArtworkState();

	using NET_SETTINGS = ServerSettings;

	char m_host[256 + 1];
	int m_port;

	NET_SETTINGS* m_netSet;

private:
	SessionState m_session;
	RakClientInterface* m_rakClient;

	int m_gameState;
	int m_lastConnectAttempt;
	uint32_t m_mapIcon[MAX_MAP_ICONS];
	int32_t m_dlVirtualWorld = 0;
	bool m_dlVirtualWorldKnown = false;
	bool m_dlFinishDownloadPending = false;

	struct NET_POOLS {
		CPlayerPool* playerPool;
		CVehiclePool* vehiclePool;
		CObjectPool* objectPool;
		CActorPool* actorPool;
		CPickupPool* pickupPool;
		CTextLabelPool* textLabelPool;
		CTextDrawPool* textDrawPool;
		CPlayerBubblePool* playerBubblePool;
		CGangZonePool* gangZonePool;
		CMenuPool* menuPool;
	} * m_pools;
};
