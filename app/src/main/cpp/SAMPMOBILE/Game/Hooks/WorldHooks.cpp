#include "HookDomains.h"
#include "../Hooks.h"
#include "../Game.h"
#include "../Animation.h"
#include "../../Net/NetGame.h"
#include "../../Net/Proto/RpcOutbound.h"
#include "../../UI/UI.h"
#include "../../Client.h"
#include "Core/Rect.h"
#include <cmath>

// T34: SetObjectNoCameraCollision — let GTA camera LOS & sphere checks ignore flagged SA-MP objects
DECL_HOOK(bool, CWorld_CameraToIgnoreThisObject, sa::CEntity *pEntity)
{
	if (pEntity)
	{
		if (CObject *pObj = CObject::FindByGtaEntity(pEntity); pObj && pObj->m_bNoCameraCollision)
		{
			return true;
		}
	}
	return CWorld_CameraToIgnoreThisObject(pEntity);
}

DECL_HOOK(int, CRadar_SetCoordBlip, uint32_t BlpType, sa::CVector vecPosition, uint32_t nColour, uint32_t DispFlag, char *pScriptName)
{
	if (pScriptName && !strncmp(pScriptName, "CODEWAY", 7))
	{
		// CWorld::FindGroundZForCoord — only snap user-placed pause-map target markers ("CODEWAY"),
		// never server-supplied coord blips (e.g. SetPlayerMapIcon style 1/3).
		vecPosition.z = eng::fn::CWorld_FindGroundZForCoord(vecPosition.x, vecPosition.y);

		if (Client::offlineMode() && pGame)
		{
			CPlayerPed *pLocal = pGame->FindPlayerPed();
			if (pLocal && pLocal->m_ped)
			{
				RwMatrix mat = pLocal->m_ped->GetMatrix().ToRwMatrix();
				mat.pos = sa::CVector(vecPosition.x, vecPosition.y, vecPosition.z + 5.0f);
				if (pLocal->IsInVehicle() && pLocal->m_ped->pVehicle)
				{
					pLocal->m_ped->pVehicle->SetMatrix((sa::CMatrix &)mat);
				}
				pLocal->m_ped->SetMatrix((sa::CMatrix &)mat);

				if (pUI && pUI->chat())
				{
					pUI->chat()->addInfoMessage("-> Teleported to: %.2f %.2f %.2f", vecPosition.x, vecPosition.y, vecPosition.z);
				}
			}
		}

		if (pNetGame && pNetGame->GetRakClient())
		{
			RakNet::BitStream bsSend;
			proto::Writer writer(bsSend);
			proto::MapMarkerReq req{};
			req.position = {vecPosition.x, vecPosition.y, vecPosition.z};
			req.serialize(writer, proto::ProtoVersion::V037);
			pNetGame->GetRakClient()->RPC(&RPC_MapMarker, &bsSend, HIGH_PRIORITY, RELIABLE, 0, false, UNASSIGNED_NETWORK_ID, nullptr);
		}
	}

	return CRadar_SetCoordBlip(BlpType, vecPosition, nColour, DispFlag, pScriptName);
}

DECL_HOOK(void, CRadar_DrawRadarGangOverlay, bool bFullMap)
{
	if (pNetGame)
	{
		CGangZonePool *pGangZonePool = pNetGame->GetGangZonePool();
		if (pGangZonePool)
		{
			pGangZonePool->Draw(bFullMap);
		}
	}
}

DECL_HOOK(uint32_t, CHudColours_GetIntColour, uintptr_t _this, uint32_t index)
{
	return TranslateColorCodeToRGBA(index);
}

static inline bool HasOccupiedVehicleDriver(const sa::CVehicle *v)
{
	return v && (v->pDriver != nullptr || (v->m_pTowingVehicle != nullptr && v->m_pTowingVehicle->pDriver != nullptr));
}

DECL_HOOK(uint32_t, CAutoMobile_ProcessEntityCollision, sa::CVehicle *_this, sa::CEntity *pCollided, uintptr_t a3)
{
	if (!pNetGame || !pNetGame->m_netSet || !pNetGame->m_netSet->disableVehicleCollisions ||
		!_this || !pCollided || !pCollided->IsVehicle() ||
		pCollided->m_nModelIndex < 400 || pCollided->m_nModelIndex > 611 ||
		!HasOccupiedVehicleDriver(_this) || !HasOccupiedVehicleDriver(reinterpret_cast<sa::CVehicle*>(pCollided)))
	{
		return CAutoMobile_ProcessEntityCollision(_this, pCollided, a3);
	}

	return 0;
}

DECL_HOOK(uint32_t, CBike_ProcessEntityCollision, sa::CVehicle *_this, sa::CEntity *pCollided, uintptr_t a3)
{
	if (!pNetGame || !pNetGame->m_netSet || !pNetGame->m_netSet->disableVehicleCollisions ||
		!_this || !pCollided || !pCollided->IsVehicle() ||
		pCollided->m_nModelIndex < 400 || pCollided->m_nModelIndex > 611 ||
		!HasOccupiedVehicleDriver(_this) || !HasOccupiedVehicleDriver(reinterpret_cast<sa::CVehicle*>(pCollided)))
	{
		return CBike_ProcessEntityCollision(_this, pCollided, a3);
	}

	return 0;
}

DECL_HOOK(uint32_t, CMonsterTruck_ProcessEntityCollision, sa::CVehicle *_this, sa::CEntity *pCollided, uintptr_t a3)
{
	if (!pNetGame || !pNetGame->m_netSet || !pNetGame->m_netSet->disableVehicleCollisions ||
		!_this || !pCollided || !pCollided->IsVehicle() ||
		pCollided->m_nModelIndex < 400 || pCollided->m_nModelIndex > 611 ||
		!HasOccupiedVehicleDriver(_this) || !HasOccupiedVehicleDriver(reinterpret_cast<sa::CVehicle*>(pCollided)))
	{
		return CMonsterTruck_ProcessEntityCollision(_this, pCollided, a3);
	}

	return 0;
}

DECL_HOOK(uint32_t, CTrailer_ProcessEntityCollision, sa::CVehicle *_this, sa::CEntity *pCollided, uintptr_t a3)
{
	if (_this && pCollided && pCollided->IsVehicle()
		&& _this->m_pTowingVehicle == reinterpret_cast<sa::CVehicle*>(pCollided))
	{
		// skip self-collision between trailer and its own tractor
		return 0;
	}

	if (!pNetGame || !pNetGame->m_netSet || !pNetGame->m_netSet->disableVehicleCollisions ||
		!_this || !pCollided || !pCollided->IsVehicle() ||
		pCollided->m_nModelIndex < 400 || pCollided->m_nModelIndex > 611 ||
		!HasOccupiedVehicleDriver(_this) || !HasOccupiedVehicleDriver(reinterpret_cast<sa::CVehicle*>(pCollided)))
	{
		return CTrailer_ProcessEntityCollision(_this, pCollided, a3);
	}

	return 0;
}

DECL_HOOK(uint32_t, CPhysical_ProcessEntityCollision, sa::CPhysical *_this, sa::CEntity *pCollided, uintptr_t a3)
{
	if (pNetGame && pNetGame->m_netSet && pNetGame->m_netSet->disableVehicleCollisions
		&& _this && pCollided && _this->IsVehicle() && pCollided->IsVehicle()
		&& _this->m_nModelIndex >= 400 && _this->m_nModelIndex <= 611
		&& pCollided->m_nModelIndex >= 400 && pCollided->m_nModelIndex <= 611)
	{
		auto* pVehA = reinterpret_cast<sa::CVehicle*>(_this);
		auto* pVehB = reinterpret_cast<sa::CVehicle*>(pCollided);
		if (HasOccupiedVehicleDriver(pVehA) && HasOccupiedVehicleDriver(pVehB)) {
			return 0;
		}
	}

	return CPhysical_ProcessEntityCollision(_this, pCollided, a3);
}

// PC SA-MP parity (hooks.cpp:1024-1040, GameHookGuardPhysicalProcessCollision):
// SetGravityProcessing(false) sets bit 31 (0x80000000u) on CEntity::m_nFlags.
// Returning before CPhysical::ProcessCollision prevents m_bIsInSafePosition = 1
// when m_bUsesCollision == 0, allowing CWorld::Process -> ProcessShift -> ApplySpeed()
// to integrate velocity smoothly for GhostMode, vehicle-surfing, and entering peds.
DECL_HOOK(void, CPhysical_ProcessCollision, sa::CPhysical *_this)
{
	if (!_this || (_this->m_nFlags & 0x80000000u) != 0)
	{
		return;
	}
	CPhysical_ProcessCollision(_this);
}

// PC SA-MP parity (hooks.cpp:1500-1540): update local & remote surfing attachments
// at the end of CWorld::Process() (after vehicle/object physics step, before RenderScene).
DECL_HOOK(void, CWorld_ProcessAttachedEntities)
{
	CWorld_ProcessAttachedEntities();

	if (!pNetGame)
	{
		return;
	}

	CPlayerPool *pPlayerPool = pNetGame->GetPlayerPool();
	if (!pPlayerPool)
	{
		return;
	}

	if (CLocalPlayer *pLocal = pPlayerPool->GetLocalPlayer())
	{
		if (pLocal->m_bIsActive && pLocal->m_surfSync.bIsActive)
		{
			pLocal->UpdateSurfing();
		}
	}

	for (PLAYERID i = 0; i <= pPlayerPool->m_iLastPlayerID && i < MAX_PLAYERS; ++i)
	{
		if (!pPlayerPool->GetSlotState(i)) continue;
		CRemotePlayer *pRemote = pPlayerPool->GetAt(i);
		if (pRemote && pRemote->IsActive() && pRemote->IsSurfing())
		{
			pRemote->ProcessSurfing();
		}
	}
}

// PC SA-MP parity (hooks.cpp:1195-1207, GameHookOverrideVerticalLineCollisionFlags):
// Force checkObjects = true whenever buildings are queried so custom CreateObject floors
// act as solid ground across FindGroundZ, SetPlayerPosFindZ, ped placement, and shadows.
DECL_HOOK(bool, CWorld_ProcessVerticalLine, const sa::CVector *vecOrigin, float fDistance, sa::CColPoint *pColPoint,
	sa::CEntity **ppEntity, bool bBuildings, bool bVehicles, bool bPeds, bool bObjects, bool bDummies,
	bool bSeeThrough, void *pIgnoreCar)
{
	if (pNetGame && bBuildings)
	{
		bObjects = true;
	}
	return CWorld_ProcessVerticalLine(vecOrigin, fDistance, pColPoint, ppEntity,
		bBuildings, bVehicles, bPeds, bObjects, bDummies, bSeeThrough, pIgnoreCar);
}

// PC SA-MP parity (patches.cpp:183-185): disable 2-player co-op screen-edge tether clamping.
DECL_HOOK(bool, CGameLogic_IsPlayerAllowedToGoInThisDirection, sa::CPed *pPed, sa::CVector vecDir, float fDistance)
{
	if (!Client::offlineMode())
	{
		return true;
	}
	return CGameLogic_IsPlayerAllowedToGoInThisDirection(pPed, vecDir, fDistance);
}

// PC SA-MP parity (hooks.cpp:1275-1344, GameHookPreservePedTransform):
// Preserve invulnerable Actor position and zero XY velocity across CCivilianPed::ProcessControl.
DECL_HOOK(void, CCivilianPed_ProcessControl, sa::CPed *_this)
{
	if (!_this || !pNetGame)
	{
		CCivilianPed_ProcessControl(_this);
		return;
	}

	const bool bInvulnFlags = _this->physicalFlags.bBulletProof
		&& _this->physicalFlags.bExplosionProof
		&& _this->physicalFlags.bMeleeProof;

	CActor *pManagedActor = nullptr;
	if (CActorPool *pActorPool = pNetGame->GetActorPool())
	{
		PLAYERID actorId = pActorPool->FindIDFromGtaPtr(_this);
		if (actorId != INVALID_PLAYER_ID)
		{
			pManagedActor = pActorPool->GetAt(actorId);
		}
	}

	const bool bLockTransform = bInvulnFlags || (pManagedActor && pManagedActor->m_invulnerable);
	sa::CVector savedPos{};
	bool bHasSavedPos = false;

	if (bLockTransform)
	{
		_this->m_vecMoveSpeed.x = 0.0f;
		_this->m_vecMoveSpeed.y = 0.0f;
		if (_this->m_matrix)
		{
			savedPos = _this->m_matrix->m_pos;
			bHasSavedPos = true;
		}
	}

	CCivilianPed_ProcessControl(_this);

	if (bLockTransform)
	{
		if (bHasSavedPos && _this->m_matrix)
		{
			_this->m_matrix->m_pos = savedPos;
		}
		_this->m_vecMoveSpeed.x = 0.0f;
		_this->m_vecMoveSpeed.y = 0.0f;
	}
}

bool g_bCreatingSirenVehicle = false;

DECL_HOOK(bool, CVehicle_UsesSiren, sa::CVehicle *_this)
{
	if (g_bCreatingSirenVehicle) {
		return true;
	}
	if (CVehicle_UsesSiren(_this)) {
		return true;
	}
	if (_this && pNetGame && pNetGame->GetVehiclePool()) {
		VEHICLEID vid = pNetGame->GetVehiclePool()->FindIDFromGtaPtr(_this);
		if (vid != INVALID_VEHICLE_ID) {
			CVehicle* pVeh = pNetGame->GetVehiclePool()->GetAt(vid);
			if (pVeh && pVeh->m_bSiren) {
				return true;
			}
		}
	}
	return false;
}

// sector coords are floor(pos/50 + 60); CPhysical::Add rejects any bound
// rect whose coords land outside [-120,119] (unsigned c+120 <= 240), so
// out-of-bounds entities never get sector records at all. Repeat-sector
// groups only hash coord & 0xF — remap out-of-range rect edges mod-16 so
// Add accepts the entity and lands its records in the same groups a
// scanner at its true position hashes to.
// Destination band is the dead zone (w0-112 → coords -112..-97,
// x in [-8600,-7850]): remapping into positive cells 0..15 placed the rect
// over real SF-map territory, and since rect-vs-rect tests use the
// REMAPPED rect, OOB entities ghost-collided with real entities sharing
// their hash group — the residual "sticky" feel. No real entity can ever
// reach x < -3000, so dead-zone rects can never overlap real ones.
// Same mechanism libMultiSA ships: it inline-hooks GetBoundRect at +0x53.
DECL_HOOK(sa::CRect, CPhysical_GetBoundRect, sa::CPhysical *_this)
{
	sa::CRect r = CPhysical_GetBoundRect(_this);
	auto fixAxis = [](float &lo, float &hi) {
		int c0 = (int) floorf(lo / 50.0f + 60.0f);
		int c1 = (int) floorf(hi / 50.0f + 60.0f);
		if (c0 >= -120 && c0 <= 120 && c1 >= -120 && c1 <= 120) return;
		if (c1 < c0) { int t = c0; c0 = c1; c1 = t; }
		int span = c1 - c0;
		if (span > 15) span = 15;
		int w0 = c0 & 0xF;
		lo = (w0 - 172) * 50.0f + 25.0f;
		hi = (w0 + span - 172) * 50.0f + 25.0f;
	};
	fixAxis(r.left, r.right);
	fixAxis(r.bottom, r.top);
	return r;
}

// set while a script/opcode-initiated attach or detach is running, so that
// the BreakTowLink hook can tell intentional breaks from GTA-spontaneous ones
bool g_bIntentionalTrailerDetach = false;

DECL_HOOK(void, CTrailer_BreakTowLink, sa::CVehicle *_this)
{
	if (!g_bIntentionalTrailerDetach && pNetGame && _this
			&& _this->m_pTowingVehicle && _this->fHealth > 0.0f)
	{
		CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
		if (pVehiclePool)
		{
			VEHICLEID tractorId = pVehiclePool->FindIDFromGtaPtr(_this->m_pTowingVehicle);
			CVehicle* pTractor = (tractorId != INVALID_VEHICLE_ID) ? pVehiclePool->GetAt(tractorId) : nullptr;
			if (pTractor && pTractor->VerifyInstance() && pTractor->m_pTrailer
					&& !pTractor->IsDriverLocalPlayer())
			{
				// server still owns this attachment: veto GTA-initiated break
				return;
			}
		}
	}

	CTrailer_BreakTowLink(_this);
}

// fix crosshair
DECL_HOOK(int, CPlantMgr_AddLocTris, void* entry, const sa::CVector* camPos, int a3, int startTri, int endTri)
{
	if (!entry) return 0;
	auto* entity = *reinterpret_cast<sa::CEntity**>(entry);
	if (!entity || IsEntityPlaceable(entity)) return 0;

	auto* colModel = eng::fn::CEntity_GetColModel<sa::CColModel*>( entity);
	if (!colModel || !colModel->m_pColData) return 0;

	auto* colData = colModel->m_pColData;
	if (!colData->m_pVertices || !colData->m_pTriangles || colData->m_nNumTriangles == 0) {
		return 0;
	}
	if (startTri < 0 || endTri < startTri || endTri >= static_cast<int>(colData->m_nNumTriangles)) {
		return 0;
	}
	for (int i = startTri; i <= endTri; ++i) {
		const sa::CColTriangle& tri = colData->m_pTriangles[i];
		if (tri.vA < 0 || tri.vB < 0 || tri.vC < 0 ||
			tri.vA > 65535 || tri.vB > 65535 || tri.vC > 65535) {
			return 0;
		}
	}
	return CPlantMgr_AddLocTris(entry, camPos, a3, startTri, endTri);
}

DECL_HOOK(void*, CAnimManager_AddAnimation, void* pClump, int groupId, int animId)
{
	void* pRes = CAnimManager_AddAnimation(pClump, groupId, animId);
	if (pRes && pClump) {
		sa::CPed* pLocal = GamePool_FindPlayerPed();
		if (pLocal && pLocal->m_pRwObject == pClump) {
			RecordLocalPedQueuedAnim(groupId, animId, 4.0f);
		}
	}
	return pRes;
}

DECL_HOOK(void*, CAnimManager_BlendAnimation, void* pClump, int groupId, int animId, float blendDelta)
{
	void* pRes = CAnimManager_BlendAnimation(pClump, groupId, animId, blendDelta);
	if (pRes && pClump) {
		sa::CPed* pLocal = GamePool_FindPlayerPed();
		if (pLocal && pLocal->m_pRwObject == pClump) {
			RecordLocalPedQueuedAnim(groupId, animId, blendDelta);
		}
	}
	return pRes;
}

void InstallWorldHooks()
{
	REG_HOOK(Boot, CRadar_SetCoordBlip, CRadar_SetCoordBlip, "Map click teleport/marker RPC");
	REG_HOOK(Boot, CRadar_DrawRadarGangOverlay, CRadar_DrawRadarGangOverlay, "SA-MP gang zone radar overlay");
	REG_HOOK(Boot, CHudColours_GetIntColour, CHudColours_GetIntColour, "SA-MP radar blip RGBA colors");

	REG_HOOK(Boot, CAnimManager_AddAnimation, CAnimManager_AddAnimation, "Capture local ped AddAnimation for OnFootSync");
	REG_HOOK(Boot, CAnimManager_BlendAnimation, CAnimManager_BlendAnimation, "Capture local ped BlendAnimation for OnFootSync");

	REG_HOOK(Boot, CAutomobile_ProcessEntityCollision, CAutoMobile_ProcessEntityCollision, "DisableRemoteVehicleCollisions (automobile)");
	REG_HOOK(Boot, CBike_ProcessEntityCollision, CBike_ProcessEntityCollision, "DisableRemoteVehicleCollisions (bike)");
	REG_HOOK(Boot, CMonsterTruck_ProcessEntityCollision, CMonsterTruck_ProcessEntityCollision, "DisableRemoteVehicleCollisions (monster)");
	REG_HOOK(Boot, CTrailer_ProcessEntityCollision, CTrailer_ProcessEntityCollision, "DisableRemoteVehicleCollisions (trailer)");
	REG_HOOK(Boot, CPhysical_ProcessEntityCollision, CPhysical_ProcessEntityCollision, "DisableRemoteVehicleCollisions (boats/other)");
	REG_HOOK(Boot, CPhysical_ProcessCollision, CPhysical_ProcessCollision, "Guard CPhysical::ProcessCollision for 0x80000000 gravity-disabled peds");
	REG_HOOK(Boot, CWorld_ProcessAttachedEntities, CWorld_ProcessAttachedEntities, "Pre-render local & remote surfing matrix sync");
	REG_HOOK(Boot, CWorld_ProcessVerticalLine, CWorld_ProcessVerticalLine, "Include dynamic CObject floors in vertical ground queries");
	REG_HOOK(Boot, CGameLogic_IsPlayerAllowedToGoInThisDirection, CGameLogic_IsPlayerAllowedToGoInThisDirection, "Disable 2P co-op screen tether clamping");
	REG_HOOK(Boot, CCivilianPed_ProcessControl, CCivilianPed_ProcessControl, "Preserve invulnerable Actor transform in ProcessControl");
	REG_HOOK(Boot, CTrailer_BreakTowLink, CTrailer_BreakTowLink, "Prevent spurious trailer detach");
	REG_HOOK(Boot, CVehicle_UsesSiren, CVehicle_UsesSiren, "Enable horn-siren on non-emergency vehicles with addSiren");
	REG_HOOK(Boot, CPhysical_GetBoundRect, CPhysical_GetBoundRect, "OOB (|x|>3000) sector bound remapping");
	REG_HOOK(Boot, CWorld_CameraToIgnoreThisObject, CWorld_CameraToIgnoreThisObject, "SetObjectNoCameraCollision camera ignore");
	REG_HOOK(Boot, CPlantMgr_ProcessEntryCollisionDataSections_AddLocTris, CPlantMgr_AddLocTris, "Guard bad col triangle indices");
}
