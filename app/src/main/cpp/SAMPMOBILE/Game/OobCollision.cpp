#include "OobCollision.h"
#include "Game.h"
#include "Object.h"
#include "RepeatSectorUtil.h"
#include "Util.h"
#include "Vehicle.h"
#include "Engine/Symbols.h"
#include "../Net/NetGame.h"
#include "../Net/VehiclePool.h"
#include "../Client.h"
#include "Core/PtrNode.h"
#include <SA.h>
#include <spdlog/spdlog.h>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

/**
 * Sector coords are floor(pos/50 + 60); CPhysical::Add rejects any bound
 * rect whose coord lands outside [-120,120] so out-of-bounds entities
 * never get records at all. ProcessCollisionSectorList and
 * CRenderer::ScanSectorList hash the RAW coord (&0xF) into the repeat
 * group — only the static-sector lookup clamps to [0,119]. Our injected
 * records must use the same raw coords the scanners hash with, or the
 * entity lands in a group nobody scans.
 */
static int RepeatSectorCoord(float f)
{
	return (int) floorf(f / 50.0f + 60.0f);
}

/** world bound sphere -> overlapped repeat-sector cell range (the same
 *  rect CPhysical::Add derives), with a safety bound so a pathological
 *  bound sphere can't drain the CEntryInfoNode pool. */
static void ComputeRepeatSectorRect(sa::CPhysical* pEntity, int iRect[4])
{
	sa::CVector center = pEntity->GetPosition();
	float fRadius = 5.0f;
	sa::CBaseModelInfo* pModelInfo = GetModelInfoByID(pEntity->m_nModelIndex);
	if (pModelInfo && pModelInfo->m_pColModel) {
		auto& sph = pModelInfo->m_pColModel->m_boundSphere;
		center = pEntity->GetMatrix() * sph.m_vecCenter;
		if (sph.m_fRadius > fRadius) fRadius = sph.m_fRadius;
	}
	iRect[0] = RepeatSectorCoord(center.x - fRadius);
	iRect[1] = RepeatSectorCoord(center.y - fRadius);
	iRect[2] = RepeatSectorCoord(center.x + fRadius);
	iRect[3] = RepeatSectorCoord(center.y + fRadius);
	int iCX = RepeatSectorCoord(center.x), iCY = RepeatSectorCoord(center.y);
	if (iRect[2] - iRect[0] > 15) { iRect[0] = iCX - 7; iRect[2] = iCX + 8; }
	if (iRect[3] - iRect[1] > 15) { iRect[1] = iCY - 7; iRect[3] = iCY + 8; }
}

/** Patches::initBetterPoolsSize replaces the stock pools at startup:
 *  CEntryInfoNode gets 20000 entries (not the stock 500), so full
 *  injection of a ~700-object OOB map (~6800 records) fits comfortably.
 *  The budget is a pure safety cap — engine Add/AddToMovingList never
 *  null-check nw() results, so if a pathological map ever pushed demand
 *  past the pool, the next engine Add would write through a null
 *  pointer. 19000 leaves ~1000 records of engine headroom. */
static const int OOB_RECORD_BUDGET = 120000;
static int s_iOobInjectedTotal = 0;

/** bookkeeping clear for records engine teardown already freed — drop
 *  the budget charge without touching the pointers. */
static void DropDeadInjection(std::vector<CEntryInfoNode*>& infos)
{
	s_iOobInjectedTotal -= (int) infos.size();
	infos.clear();
}

/** link the entity into every repeat-sector group's entity-type sub-list
 *  (vehicles +0, peds +8, objects +16) covered by iRect — one pooled
 *  CPtrNodeDoubleLink plus a CEntryInfoNode on m_pCollisionList per cell,
 *  exactly as CPhysical::Add does, so engine teardown unlinks them the
 *  same way it does regular entities. */
static void InjectIntoRepeatSectors(sa::CPhysical* pEntity, const int iRect[4],
		std::vector<CEntryInfoNode*>& infos)
{
	sa_world::linkEntityToRepeatSectors(
		pEntity, iRect[0], iRect[1], iRect[2], iRect[3],
		&infos, &s_iOobInjectedTotal, OOB_RECORD_BUDGET);
}

/** unlink everything InjectIntoRepeatSectors added. Every record is
 *  re-verified on the entity's m_pCollisionList chain and must still
 *  point back at this entity — pool slots get re-issued, so a stale
 *  record could alias somebody else's live entry. */
static void UnlinkFromRepeatSectors(sa::CPhysical* pEntity,
		std::vector<CEntryInfoNode*>& infos)
{
	constexpr size_t kRepeatSectorSize = 3 * sizeof(void*);
	uintptr_t pSectors = eng::addr(eng::Sym::CWorld_ms_aRepeatSectors);

	for (CEntryInfoNode* pInfo : infos) {
		CEntryInfoNode* pCur = pEntity->m_pCollisionList.m_pNode;
		while (pCur && pCur != pInfo) pCur = pCur->m_pNext;
		if (pCur != pInfo) continue;

		// only records we created may be freed — a re-issued pool slot can
		// alias a live engine record on this entity's own chain, and our
		// records are the only ones pointing back into ms_aRepeatSectors
		uintptr_t uRepeat = reinterpret_cast<uintptr_t>(pInfo->m_pRepeatSector);
		if (!pSectors || uRepeat < pSectors || uRepeat >= pSectors + 256 * kRepeatSectorSize) continue;

		auto* pNode = reinterpret_cast<sa::CPtrNodeDoubleLink*>(pInfo->m_pDoubleLink);
		if (!pNode || pNode->m_item != pEntity) continue;
		if (pNode->m_prev) pNode->m_prev->m_next = pNode->m_next;
		else *reinterpret_cast<sa::CPtrNodeDoubleLink**>(pInfo->m_pDoubleLinkList) = pNode->m_next;
		if (pNode->m_next) pNode->m_next->m_prev = pNode->m_prev;
		eng::fn::CPtrNodeDoubleLink_OpDelete( pNode);

		if (pInfo->m_pPrevious) pInfo->m_pPrevious->m_pNext = pInfo->m_pNext;
		else pEntity->m_pCollisionList.m_pNode = pInfo->m_pNext;
		if (pInfo->m_pNext) pInfo->m_pNext->m_pPrevious = pInfo->m_pPrevious;
		eng::fn::CEntryInfoNode_OpDelete( pInfo);
	}
	s_iOobInjectedTotal -= (int) infos.size();
	infos.clear();
}

static bool SectorRecordAlive(sa::CPhysical* pEntity, CEntryInfoNode* pInfo)
{
	for (CEntryInfoNode* pCur = pEntity->m_pCollisionList.m_pNode;
			pCur; pCur = pCur->m_pNext) {
		if (pCur == pInfo) return true;
	}
	return false;
}

/** CColStore streaming is position-driven: the quadtree only ever
 *  matches in-bounds positions, so a shared colModel on an OOB entity
 *  stays an empty shell — meaning ZERO collision, not a bound-sphere
 *  fallback (ProcessColModels early-outs on null m_pColData). The old
 *  GetBoundingBox gate made it worse: it refused to pin slots with no
 *  quadtree entry, which are exactly the slots this path exists for.
 *  The def array is 255 entries and always present, so AddRef /
 *  RemoveRef are safe on any valid slot id.
 *  The engine's own force-load recipe (LoadAllCollision /
 *  LoadAllBoundingBoxes) is RequestModel + LoadAllRequestedModels —
 *  the async channel starves col requests queued outside position
 *  matching, so we pump it synchronously, at most once a second across
 *  all OOB entities. */
static uint32_t s_dwOobColPumpTick = 0;
static std::unordered_set<int> s_oobColReloaded;

static bool OobColDefLoaded(int iSlot)
{
	auto ppStore = eng::as<uintptr_t*>(eng::Sym::CColStore_ms_pColPool);
	uintptr_t uStore = ppStore ? *ppStore : 0;
	char* pDefs = uStore ? *reinterpret_cast<char**>(uStore) : nullptr;
	return pDefs && pDefs[iSlot * 44 + 40];
}

bool PinOobColSlot(sa::CColModel* pCM, int& iPinnedSlot)
{
	int iSlot = (pCM && !pCM->m_bIsSingleColDataAlloc)
			? (int) pCM->m_nColSlot : -1;
	if (iSlot <= 0 || iSlot >= 255) iSlot = -1;
	if (iPinnedSlot == iSlot) return false;
	if (iPinnedSlot >= 0) {
		eng::fn::CColStore_RemoveRef( iPinnedSlot);
	}
	iPinnedSlot = -1;
	if (iSlot >= 0) {
		eng::fn::CColStore_AddRef( iSlot);
		iPinnedSlot = iSlot;
	}
	return true;
}

void RequestOobColSlot(sa::CColModel* pCM, int iPinnedSlot,
		uint32_t& dwLastReqTick)
{
	if (!pCM || pCM->m_pColData || iPinnedSlot < 0) return;
	uint32_t dwNow = GetTickCount();
	if (dwNow - dwLastReqTick <= 500) return;
	dwLastReqTick = dwNow;
	// a "loaded" col file whose colData never showed up was parsed at a
	// moment when binding skipped this model — binding only runs inside
	// LoadCol, so force exactly one reload cycle to re-run it
	if (OobColDefLoaded(iPinnedSlot)
			&& s_oobColReloaded.insert(iPinnedSlot).second) {
		eng::fn::CStreaming_RemoveModel(
				25000 + iPinnedSlot);
		spdlog::info("OOBCol: force reload slot={}", iPinnedSlot);
	}
	eng::fn::CStreaming_RequestModel(
			25000 + iPinnedSlot, 0x18);
	if (dwNow - s_dwOobColPumpTick > 1000) {
		s_dwOobColPumpTick = dwNow;
		eng::fn::CStreaming_LoadAllRequestedModels( true);
	}
}

/** one-shot state dump when a pin lands on a slot — reads the CColDef
 *  (refcount @+38 / loaded flag @+40) via ms_pColPool's def array, and
 *  whether streaming has a file registered for 25000+slot at all. */
void LogOobColPin(int iModel, int iSlot)
{
	if (iSlot < 0) return;
	int iRef = -1, iLoaded = -1, iMin = -1, iMax = -1;
	auto ppStore = eng::as<uintptr_t*>(eng::Sym::CColStore_ms_pColPool);
	uintptr_t uStore = ppStore ? *ppStore : 0;
	char* pDefs = uStore ? *reinterpret_cast<char**>(uStore) : nullptr;
	if (pDefs) {
		char* pDef = pDefs + iSlot * 44;
		iMin = (int) *reinterpret_cast<int16_t*>(pDef + 34);
		iMax = (int) *reinterpret_cast<int16_t*>(pDef + 36);
		iRef = (int) *reinterpret_cast<int16_t*>(pDef + 38);
		iLoaded = (int) *reinterpret_cast<uint8_t*>(pDef + 40);
	}
	bool bInImg = eng::fn::CStreaming_IsObjectInCdImage( 25000 + iSlot);
	spdlog::info("OOBCol: model={} pin slot={} ref={} loaded={} "
			"range=[{},{}] inImg={}",
			iModel, iSlot, iRef, iLoaded, iMin, iMax, (int) bInImg);
}

/**
 * Insert the entity into CWorld::ms_aRepeatSectors — the 16x16 coarse
 * grid ProcessCollisionSectorList scans — exactly as CPhysical::Add does
 * for in-bounds entities: for every sector cell the entity's bound box
 * overlaps, a pooled CPtrNodeDoubleLink in the entity-type sub-list
 * (vehicles +0, peds +8, objects +16) plus a CEntryInfoNode membership
 * record on m_pCollisionList, so engine teardown unlinks them the same
 * way it does regular entities. Multi-cell coverage matters: large
 * floor pieces are reachable from sectors their center isn't in.
 * Only runs while the object is genuinely out of bounds — in-bounds it
 * must leave sector placement to the engine.
 */
void CObject::EnsureRepeatSector()
{
	bool bAlive = m_entity && GamePool_Object_GetAt(m_gtaId) == m_entity
			&& !IsEntityPlaceable(m_entity);
	bool bOob = false;
	if (bAlive) {
		const sa::CVector& p = m_entity->GetPosition();
		bOob = p.x > 3000.0f || p.x < -3000.0f
				|| p.y > 3000.0f || p.y < -3000.0f;
	}
	bool bWant = bOob && m_entity->m_pRwObject
			&& m_AttachedVehicleID == INVALID_VEHICLE_ID
			&& m_AttachedObjectID == INVALID_OBJECT_ID
			&& m_AttachedPlayerID == INVALID_PLAYER_ID;
	// engine passes (Remove/RemoveAndAdd/streaming) can strip our records
	// at any time — if the first one is gone from m_pCollisionList, the
	// whole set is stale and must be re-injected; never touch the stale
	// records themselves (pool slots get re-issued to other entities)
	bool bStale = bAlive && !m_aInjectedInfo.empty()
			&& !SectorRecordAlive(m_entity, m_aInjectedInfo.front());
	if (bStale) DropDeadInjection(m_aInjectedInfo);

	// the GetBoundRect hook lets the engine's own Add succeed OOB — once
	// the entity carries live records we didn't write, the engine owns
	// this entity and manual injection would only duplicate them
	bool bEngineOwns = bAlive && m_aInjectedInfo.empty()
			&& m_entity->m_pCollisionList.m_pNode != nullptr;

	int iRect[4] = {-1, -1, -1, -1};
	if (bWant) ComputeRepeatSectorRect(m_entity, iRect);

	if (!bStale && !memcmp(iRect, m_aiInjRect, sizeof(iRect))) return;
	if (bAlive) UnlinkFromRepeatSectors(m_entity, m_aInjectedInfo);
	else DropDeadInjection(m_aInjectedInfo);
	if (bWant && !bEngineOwns) InjectIntoRepeatSectors(m_entity, iRect, m_aInjectedInfo);
	memcpy(m_aiInjRect, iRect, sizeof(iRect));

	if (!m_bDiagLogged && bWant) {
		m_bDiagLogged = true;
		sa::CBaseModelInfo* pModelInfo = GetModelInfoByID(m_iModel);
		sa::CColModel* pColModel = pModelInfo ? pModelInfo->m_pColModel : nullptr;
		sa::CCollisionData* pData = pColModel ? pColModel->m_pColData : nullptr;
		const char* pEnt = reinterpret_cast<const char*>(m_entity);
		uint32_t uPhysFlags = *reinterpret_cast<const uint32_t*>(pEnt + 100);
		uint32_t uEntFlags = *reinterpret_cast<const uint32_t*>(pEnt + 40);
		float fMass = *reinterpret_cast<const float*>(pEnt + 176);
		uint32_t uMiKey = pModelInfo
				? *reinterpret_cast<const uint32_t*>(
						reinterpret_cast<const char*>(pModelInfo) + 8) : 0;
		spdlog::info("OOBCol: model={} cells={} type={} usesCol={} mFlags={:#06x} miKey={:#010x} "
				"colSlot={} singleAlloc={} colData={} vol={} "
				"sph={} box={} ln={} tris={} boundR={:.2f} "
				"eFlags={:#010x} pFlags={:#010x} mass={:.1f} injectedTotal={}",
				m_iModel, (int) m_aInjectedInfo.size(), (int) m_entity->m_nType,
				(int) m_entity->m_bUsesCollision,
				pModelInfo ? (unsigned) pModelInfo->m_flags : 0u,
				uMiKey,
				pColModel ? (int) pColModel->m_nColSlot : -1,
				pColModel ? (int) pColModel->m_bIsSingleColDataAlloc : -1,
				pData ? 1 : 0,
				pColModel ? (int) pColModel->m_bHasCollisionVolumes : -1,
				pData ? (int) pData->m_nNumSpheres : 0,
				pData ? (int) pData->m_nNumBoxes : 0,
				pData ? (int) pData->m_nNumLines : 0,
				pData ? (int) pData->m_nNumTriangles : 0,
				pColModel ? pColModel->m_boundSphere.m_fRadius : 0.0f,
				uEntFlags, uPhysFlags, fMass,
				s_iOobInjectedTotal);
	}
}

void CObject::RemoveFromRepeatSector()
{
	if (m_entity && !IsEntityPlaceable(m_entity)
			&& GamePool_Object_GetAt(m_gtaId) == m_entity) {
		UnlinkFromRepeatSectors(m_entity, m_aInjectedInfo);
	} else {
		DropDeadInjection(m_aInjectedInfo);
	}
	m_aiInjRect[0] = m_aiInjRect[1] = m_aiInjRect[2] = m_aiInjRect[3] = -1;
}

/**
 * Vehicles drive their own collision differently from peds: a ped's
 * CheckCollision scans the repeat-sector groups under its own position,
 * but a vehicle's ProcessCollision iterates its OWN m_pCollisionList
 * records and runs ProcessCollisionSectorList_SimpleCar on each recorded
 * sector. An OOB vehicle has no records (CPhysical::Add rejected it), so
 * it never collides — wrecks sink straight through injected floors.
 * Inject every OOB pool vehicle into the repeat sectors its bound sphere
 * covers; its own native physics step does the rest, exactly as for an
 * in-bounds vehicle.
 */
struct OobVehicleSector {
	int rect[4] = {-1, -1, -1, -1};
	std::vector<CEntryInfoNode*> infos;
	int pinnedColSlot = -1;
	uint32_t lastColReqTick = 0;
};
static std::unordered_map<sa::CVehicle*, OobVehicleSector> s_oobVehSectors;

void ProcessOobVehicleSectors()
{
	CVehiclePool* pVehiclePool = pNetGame ? pNetGame->GetVehiclePool() : nullptr;
	if (!pVehiclePool) return;

	std::unordered_map<sa::CVehicle*, bool> live;
	for (int i = 0; i < MAX_VEHICLES; i++) {
		CVehicle* pVeh = pVehiclePool->GetAt((VEHICLEID) i);
		if (!pVeh || !pVeh->m_pVehicle || !pVeh->VerifyInstance()) continue;
		sa::CVehicle* pEnt = pVeh->m_pVehicle;
		live[pEnt] = true;

		const sa::CVector& vp = pEnt->GetPosition();
		// same strict bounds the objects use — the engine still owns
		// sector placement in the 3000..3050 margin
		bool bOob = vp.x < -3000.0f || vp.x > 3000.0f
				|| vp.y < -3000.0f || vp.y > 3000.0f;
		if (!bOob) {
			// in-bounds vehicles belong to the engine's own sector
			// machinery — our records would just fight its RemoveAndAdd
			auto it = s_oobVehSectors.find(pEnt);
			if (it != s_oobVehSectors.end()) {
				UnlinkFromRepeatSectors(pEnt, it->second.infos);
				if (it->second.pinnedColSlot >= 0) {
					eng::fn::CColStore_RemoveRef(
							it->second.pinnedColSlot);
				}
				s_oobVehSectors.erase(it);
			}
			continue;
		}

		auto& inj = s_oobVehSectors[pEnt];
		int iRect[4];
		ComputeRepeatSectorRect(pEnt, iRect);
		// engine teardown can strip our records at any time — a dead
		// record vector must not be unlinked, just re-injected
		if (!inj.infos.empty() && !SectorRecordAlive(pEnt, inj.infos.front())) {
			DropDeadInjection(inj.infos);
		}
		// engine-written records mean the GetBoundRect hook already let
		// Add succeed for this vehicle — don't double-inject over them
		bool bEngineOwns = inj.infos.empty()
				&& pEnt->m_pCollisionList.m_pNode != nullptr;
		if (!bEngineOwns
				&& (inj.infos.empty() || memcmp(iRect, inj.rect, sizeof(iRect)) != 0)) {
			UnlinkFromRepeatSectors(pEnt, inj.infos);
			InjectIntoRepeatSectors(pEnt, iRect, inj.infos);
			memcpy(inj.rect, iRect, sizeof(iRect));
		}

		// same position-streamed col problem as OOB objects: an unpinned
		// vehicle colModel stays an empty shell — zero collision, not a
		// bound-sphere fallback — so the car wades through geometry it
		// should mesh-collide with
		sa::CBaseModelInfo* pMI = GetModelInfoByID(pEnt->m_nModelIndex);
		sa::CColModel* pCM = pMI ? pMI->m_pColModel : nullptr;
		if (PinOobColSlot(pCM, inj.pinnedColSlot)) {
			LogOobColPin(pEnt->m_nModelIndex, inj.pinnedColSlot);
		}
		RequestOobColSlot(pCM, inj.pinnedColSlot, inj.lastColReqTick);
	}

	// entities that died while injected had their records freed by engine
	// teardown — drop the bookkeeping without touching the pointers
	for (auto it = s_oobVehSectors.begin(); it != s_oobVehSectors.end();) {
		if (live.count(it->first)) ++it;
		else {
			s_iOobInjectedTotal -= (int) it->second.infos.size();
			if (it->second.pinnedColSlot >= 0) {
				eng::fn::CColStore_RemoveRef(
						it->second.pinnedColSlot);
			}
			it = s_oobVehSectors.erase(it);
		}
	}
}
