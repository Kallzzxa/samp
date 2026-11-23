//
// Created by ALYN on 2024/8/28.
//

#include "Game.h"
#include "../Net/NetGame.h"
#include "../Client.h"
#include "RenderWare/rpuvanim.h"
#include "Core/PtrNode.h"
#include <cmath>
#include <strings.h>
#include <unordered_map>
#include <unordered_set>

extern Game* pGame;
extern NetGame* pNetGame;
extern MaterialTextGenerator* pMaterialTextGenerator;

RwTexture* g_blankTexture;

// O(1) lookup from GTA CEntity* to live CObject* (covers both CObjectPool
// objects and CPlayerPed attached objects, which do not live in CObjectPool).
static std::unordered_map<sa::CEntity*, CObject*> s_liveObjectByEntity;

CObject* CObject::FindByGtaEntity(sa::CEntity* pEntity)
{
	if (!pEntity) {
		return nullptr;
	}
	auto it = s_liveObjectByEntity.find(pEntity);
	if (it == s_liveObjectByEntity.end()) {
		return nullptr;
	}
	CObject* pObj = it->second;
	if (!pObj || !pObj->m_gtaId || GamePool_Object_GetAt(pObj->m_gtaId) != reinterpret_cast<sa::CObject*>(pEntity)) {
		s_liveObjectByEntity.erase(it);
		return nullptr;
	}
	return pObj;
}

CObject::CObject(int iModel, sa::CVector vecPos, sa::CVector vecRot, float fDrawDistance, uint8_t bAttached)
{
	spdlog::info("CObject::CObject({}, [{} {} {}], [{} {} {}], {}, {}).", iModel, vecPos.x, vecPos.y, vecPos.z, vecRot.x, vecRot.y, vecRot.z, fDrawDistance, bAttached);
	m_AttachedVehicleID = INVALID_VEHICLE_ID;
	m_AttachedObjectID = INVALID_OBJECT_ID;
	m_bAttachedToPed = bAttached;

	m_entity = nullptr;
	m_gtaId = 0;

	m_vecAttachedPos.x = 0.0f;
	m_vecAttachedPos.y = 0.0f;
	m_vecAttachedPos.z = 0.0f;
	m_vecAttachedRot.x = 0.0f;
	m_vecAttachedRot.y = 0.0f;
	m_vecAttachedRot.z = 0.0f;
	m_bSyncRotation = true;

	m_byteMoving = 0;
	m_fMoveSpeed = 0.0f;
	m_bNeedRotate = false;
	m_iModel = iModel;
	m_vecRotation = sa::CVector{0.0f, 0.0f, 0.0f};
	m_matTarget = {};
	m_quatStart = {};
	m_quatTarget = {};
	m_vecRotationTarget = sa::CVector{0.0f, 0.0f, 0.0f};
	m_vecSubRotationTarget = sa::CVector{0.0f, 0.0f, 0.0f};
	m_fDistanceToTargetPoint = 0.0f;
	m_dwMoveTick = 0;
	m_bNoCameraCollision = false;

	if (!IsValidModel(iModel)) {
		spdlog::info("Object model {} is invalid, using default model.", iModel);
		iModel = 18631;
	}

	uint32_t dwRetID = 0;
	ScriptCommand(&create_object, iModel, vecPos.x, vecPos.y, vecPos.z, &dwRetID);
	spdlog::info("Object created with model {} at [{} {} {}].", iModel, vecPos.x, vecPos.y, vecPos.z);

	sa::CObject* pEntity = dwRetID ? GamePool_Object_GetAt(dwRetID) : nullptr;

	if (dwRetID && pEntity) {
		m_entity = pEntity;
		m_gtaId = dwRetID;
		s_liveObjectByEntity[m_entity] = this;

		m_iModel = iModel;

		EnsureRwObject();

		RwMatrix mat = m_entity->GetMatrix().ToRwMatrix();
		mat.pos = vecPos;
		m_entity->SetMatrix((sa::CMatrix&) mat);
		SetRotation(&vecRot);

		// out-of-bounds entities never go through the world's
		// frame-sync pipeline — without this they render at the
		// atomic frame's stale transform (origin), not vecPos
		if (vecPos.x > 3000.0f || vecPos.x < -3000.0f ||
				vecPos.y > 3000.0f || vecPos.y < -3000.0f) {
			UpdateRwMatrixAndFrame();
		}
	}
	else {
		spdlog::error("Failed to create object with model {} at [{} {} {}].", iModel, vecPos.x, vecPos.y, vecPos.z);
	}

	for (int i = 0; i < MAX_MATERIALS; i++) {
		memset(&m_Material[i], 0, sizeof(MATERIAL_OBJECT));
		m_pLastObjectMaterialTexture[i] = nullptr;
		m_dwLastObjectMaterialColor[i] = {};
		m_LastObjectMaterialSurface[i] = {1.0f, 0.0f, 1.0f};
	}

	m_bHasCustomObjectMaterial = false;
	m_bAllowChangeObjectMaterial = false;
	m_bObjectMaterialTextureChanged = false;
	m_bObjectMaterialColorChanged = false;
	m_uiLastObjectGeometry = 0;
	m_pLastObjectMaterialRwObject = nullptr;

	m_bForceRender = (vecPos.x > 3000.0f || vecPos.x < -3000.0f ||
			vecPos.y > 3000.0f || vecPos.y < -3000.0f);
	m_pUVAnimRwObject = nullptr;
	m_bUVAnimEnabled = false;
	m_bDiagLogged = false;
	m_iPinnedColSlot = -1;
	m_dwLastColRequestTick = 0;
	m_bColDiagLogged = false;
	m_aiInjRect[0] = m_aiInjRect[1] = m_aiInjRect[2] = m_aiInjRect[3] = -1;

	if (m_bForceRender && m_entity) {
		spdlog::info("OOB object: model={} entity={:p} rwObj={:p} moving={:p}", iModel,
				(void*) m_entity, (void*) m_entity->m_pRwObject,
				(void*) m_entity->m_pMovingList);
	}

	TryEnableUVAnim();
}

CObject::~CObject()
{
	if (m_entity) {
		auto it = s_liveObjectByEntity.find(m_entity);
		if (it != s_liveObjectByEntity.end() && it->second == this) {
			s_liveObjectByEntity.erase(it);
		}
	}

	// release the col-slot pin first — the CColDef pool is static and
	// outlives the entity, and RemoveRef is safe on any valid slot id
	// (the def array is 255 fixed entries, no quadtree entry needed)
	if (m_iPinnedColSlot >= 0) {
		eng::fn::CColStore_RemoveRef( m_iPinnedColSlot);
		m_iPinnedColSlot = -1;
	}

	sa::CObject* pEntity = m_gtaId ? GamePool_Object_GetAt(m_gtaId) : nullptr;

	// always balance the injection bookkeeping — RemoveFromRepeatSector
	// re-verifies entity identity itself and falls back to just dropping
	// the stale vector; skipping it when the slot was recycled would leak
	// s_iOobInjectedTotal until the budget disables OOB injection
	RemoveFromRepeatSector();

	// the engine may already have recycled the slot — unlink our sector
	// records only while the entity is provably still ours; if it's gone,
	// its teardown removed the records too (they're properly linked) and
	// touching the chain would free pool slots that were re-issued
	if (pEntity && pEntity == m_entity && !IsEntityPlaceable(pEntity)) {
		ScriptCommand(&destroy_object, m_gtaId);
		if (GetModelRefCounts(m_iModel) == 0) {
			pGame->RemoveModel(m_iModel, false);
		}
	}

	if (m_bHasCustomObjectMaterial) {
		for (int i = 0; i < MAX_MATERIALS; i++) {
			if (m_Material[i].pMaterialTexture && m_Material[i].pMaterialTexture != g_blankTexture) {
				RwTextureDestroy(m_Material[i].pMaterialTexture);
				m_Material[i].pMaterialTexture = 0;
			}

			memset(&m_Material[i], 0, sizeof(MATERIAL_OBJECT));
			m_pLastObjectMaterialTexture[i] = 0;
			m_dwLastObjectMaterialColor[i] = {};
		}

		m_bHasCustomObjectMaterial = false;
	}
}

/**
 * create_object always produces a real CObject (CDummyObject lives in a
 * separate 96B pool and only serves IPL instances) — but the atomic is
 * only instanced once the model has streamed in, so a fresh object can
 * sit with m_pRwObject == 0. Re-instance it via SetModelIndex once the
 * model arrives — the same retry the reference client's ProcessStreaming
 * uses for streamed-out objects.
 */
void CObject::EnsureRwObject()
{
	if (!m_entity || IsEntityPlaceable(m_entity) || m_entity->m_pRwObject) {
		return;
	}
	if (!pGame) {
		return;
	}
	if (!pGame->IsModelLoaded(m_iModel)) {
		// the engine created the entity without an atomic because the model
		// wasn't streamed yet — keep requesting like the reference client's
		// ProcessStreaming; retry next tick once it arrives
		pGame->RequestModel(m_iModel);
		return;
	}

	spdlog::info("EnsureRwObject: re-instancing atomic for object {} (model {})", m_gtaId, m_iModel);
	CEntity::SetModelIndex(m_iModel);
	UpdateRwMatrixAndFrame();
}

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
static const int OOB_RECORD_BUDGET = 19000;
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
	constexpr size_t kPtrSize = sizeof(void*);
	constexpr size_t kRepeatSectorSize = 3 * kPtrSize;
	size_t uListOff;
	switch (pEntity->m_nType) {
		case sa::ENTITY_TYPE_VEHICLE: uListOff = 0; break;
		case sa::ENTITY_TYPE_PED:     uListOff = kPtrSize; break;
		case sa::ENTITY_TYPE_OBJECT:  uListOff = 2 * kPtrSize; break;
		default: return;
	}

	auto pSectors = eng::addr(eng::Sym::CWorld_ms_aRepeatSectors);
	if (!pSectors) return;

	// cells 16 apart hash to the same group — dedup
	uint32_t aSeenMask[8] = {};
	for (int cy = iRect[1]; cy <= iRect[3]; ++cy) {
		for (int cx = iRect[0]; cx <= iRect[2]; ++cx) {
			int iGroup = ((cy & 0xF) << 4) | (cx & 0xF);
			uint32_t uBit = 1u << (iGroup & 31);
			if (aSeenMask[iGroup >> 5] & uBit) continue;
			aSeenMask[iGroup >> 5] |= uBit;
			if (s_iOobInjectedTotal >= OOB_RECORD_BUDGET) return;

			auto** ppHead = reinterpret_cast<sa::CPtrNodeDoubleLink**>(
					pSectors + static_cast<size_t>(iGroup) * kRepeatSectorSize + uListOff);
			auto* pNode = eng::fn::CPtrNodeDoubleLink_OpNew<sa::CPtrNodeDoubleLink*>(
					sizeof(sa::CPtrNodeDoubleLink));
			auto* pInfo = eng::fn::CEntryInfoNode_OpNew<CEntryInfoNode*>(
					sizeof(CEntryInfoNode));
			if (!pNode || !pInfo) {
				if (pNode) eng::fn::CPtrNodeDoubleLink_OpDelete( pNode);
				if (pInfo) eng::fn::CEntryInfoNode_OpDelete( pInfo);
				return;
			}

			pNode->m_item = pEntity;
			pNode->m_next = *ppHead;
			pNode->m_prev = nullptr;
			if (*ppHead) (*ppHead)->m_prev = pNode;
			*ppHead = pNode;

			pInfo->m_pDoubleLinkList = reinterpret_cast<CPtrListDoubleLink*>(ppHead);
			pInfo->m_pDoubleLink = reinterpret_cast<CPtrNodeDoubleLink*>(pNode);
			pInfo->m_pRepeatSector = reinterpret_cast<CRepeatSector*>(pSectors + static_cast<size_t>(iGroup) * kRepeatSectorSize);
			pInfo->m_pPrevious = nullptr;
			pInfo->m_pNext = pEntity->m_pCollisionList.m_pNode;
			if (pInfo->m_pNext) pInfo->m_pNext->m_pPrevious = pInfo;
			pEntity->m_pCollisionList.m_pNode = pInfo;

			++s_iOobInjectedTotal;
			infos.push_back(pInfo);
		}
	}
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

static bool PinOobColSlot(sa::CColModel* pCM, int& iPinnedSlot)
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

static void RequestOobColSlot(sa::CColModel* pCM, int iPinnedSlot,
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
static void LogOobColPin(int iModel, int iSlot)
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
			&& m_AttachedObjectID == INVALID_OBJECT_ID;
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

/**
 * @param pVehicle
 */
void CObject::AttachToVehicle(CVehicle* pVehicle)
{
	if (!m_entity || !m_gtaId || !pVehicle || !pVehicle->m_gtaId || !pVehicle->VerifyInstance()) {
		return;
	}

	if (!ScriptCommand(&is_object_attached, m_gtaId)) {
		ScriptCommand(&attach_object_to_car,
				m_gtaId,
				pVehicle->m_gtaId,
				m_vecAttachedPos.x,
				m_vecAttachedPos.y,
				m_vecAttachedPos.z,
				m_vecAttachedRot.x,
				m_vecAttachedRot.y,
				m_vecAttachedRot.z);
	}
}

/**
 * @param pObject
 */
void CObject::AttachToObject(CObject* pObject)
{
	if (!m_entity || !m_gtaId || !pObject || !pObject->m_entity || !pObject->m_gtaId
			|| GamePool_Object_GetAt(pObject->m_gtaId) != reinterpret_cast<sa::CObject*>(pObject->m_entity)) {
		return;
	}

	if (!ScriptCommand(&is_object_attached, m_gtaId)) {
		ScriptCommand(&attach_object_to_object,
				m_gtaId,
				pObject->m_gtaId,
				m_vecAttachedPos.x,
				m_vecAttachedPos.y,
				m_vecAttachedPos.z,
				m_vecAttachedRot.x,
				m_vecAttachedRot.y,
				m_vecAttachedRot.z);
	}

	// When syncRotation is disabled, GTA's attach_object_to_object still inherits
	// the parent's rotation every tick — restore fixed world orientation using
	// m_vecAttachedRot to match PC SA-MP UpdateAttachedObjectRotation.
	if (!m_bSyncRotation) {
		RwMatrix mat = m_entity->GetMatrix().ToRwMatrix();
		sa::CVector pos = mat.pos;
		RotateMatrix(m_vecAttachedRot);
		mat = m_matTarget;
		mat.pos = pos;
		m_entity->SetMatrix((sa::CMatrix&) mat);
		UpdateRwMatrixAndFrame();
	}
}

/**
 * @param matPos
 * @return
 */
float CObject::DistanceRemaining(RwMatrix* matPos)
{
	float fSX, fSY, fSZ;
	fSX = (matPos->pos.x - m_matTarget.pos.x) * (matPos->pos.x - m_matTarget.pos.x);
	fSY = (matPos->pos.y - m_matTarget.pos.y) * (matPos->pos.y - m_matTarget.pos.y);
	fSZ = (matPos->pos.z - m_matTarget.pos.z) * (matPos->pos.z - m_matTarget.pos.z);
	return (float) sqrt(fSX + fSY + fSZ);
}

void CObject::ApplyMoveSpeed()
{
	if (m_entity) {
		float* pTimeStep = eng::as<float*>(eng::Sym::CTimer_ms_fTimeStep);
		if (!pTimeStep) {
			return;
		}
		float fTimeStep = *pTimeStep;

		RwMatrix mat = m_entity->GetMatrix().ToRwMatrix();
		mat.pos.x += fTimeStep * m_entity->GetMoveSpeed().x;
		mat.pos.y += fTimeStep * m_entity->GetMoveSpeed().y;
		mat.pos.z += fTimeStep * m_entity->GetMoveSpeed().z;
		m_entity->SetMatrix((sa::CMatrix&) mat);
	}
}

static void ForEachObjectAtomic(RwObject* rwObj, RpAtomic* (*cb)(RpAtomic*, void*), void* data);
RpAtomic* DriveUVAnimAtomicCB(RpAtomic* atomic, void* data);

/**
 * @param fElapsedTime
 */
void CObject::Process(float fElapsedTime)
{
	if (!m_entity || !m_gtaId || GamePool_Object_GetAt(m_gtaId) != reinterpret_cast<sa::CObject*>(m_entity) || IsEntityPlaceable(m_entity)) {
		return;
	}

	if (!m_entity->m_pRwObject) {
		EnsureRwObject();
	}

	UpdateUVAnim();

	// keep the flag tracking the entity's actual position — previously only
	// the constructor and TeleportTo set it, so an object carried across
	// the boundary by MoveTo neither gained OOB collision nor released its
	// injected records when it came back in bounds
	if (m_entity) {
		const sa::CVector& pos = m_entity->GetPosition();
		m_bForceRender = pos.x > 3000.0f || pos.x < -3000.0f
				|| pos.y > 3000.0f || pos.y < -3000.0f;
	}

	if (m_bForceRender && m_entity) {
		// Col data is position-streamed: CColStore's quadtree matches
		// col files to in-bounds positions only, so an OOB object's
		// shared colModel stays an empty shell — and an empty shell is
		// ZERO collision (ProcessColModels early-outs on null
		// m_pColData; there is no bound-sphere fallback). Pin the slot
		// so LoadCollision can't evict it, then force-load the file
		// synchronously — the async streaming channel starves requests
		// queued outside position matching. Single-alloc colModels own
		// their data outright and need no pin.
		sa::CBaseModelInfo* pModelInfo = GetModelInfoByID(m_iModel);
		sa::CColModel* pColModel = pModelInfo ? pModelInfo->m_pColModel : nullptr;
		if (PinOobColSlot(pColModel, m_iPinnedColSlot)) {
			LogOobColPin(m_iModel, m_iPinnedColSlot);
		}
		RequestOobColSlot(pColModel, m_iPinnedColSlot, m_dwLastColRequestTick);
		if (pColModel && pColModel->m_pColData && m_iPinnedColSlot >= 0
				&& !m_bColDiagLogged) {
			m_bColDiagLogged = true;
			sa::CCollisionData* pData = pColModel->m_pColData;
			spdlog::info("OOBCol: model={} col arrived slot={} "
					"sph={} box={} ln={} tris={}",
					m_iModel, m_iPinnedColSlot,
					(int) pData->m_nNumSpheres, (int) pData->m_nNumBoxes,
					(int) pData->m_nNumLines, (int) pData->m_nNumTriangles);
		}
	}

	if (m_AttachedVehicleID != INVALID_VEHICLE_ID) {
		if (pNetGame) {
			CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
			if (pVehiclePool) {
				CVehicle* pVehicle = pVehiclePool->GetAt(m_AttachedVehicleID);
				if (pVehicle) {
					if (pVehicle->IsAdded()) {
						AttachToVehicle(pVehicle);
					}
				}
			}
		}
	}
	else if (m_AttachedObjectID != INVALID_OBJECT_ID) {
		if (pNetGame) {
			CObjectPool* pObjectPool = pNetGame->GetObjectPool();
			if (pObjectPool) {
				CObject* pObject = pObjectPool->GetAt(m_AttachedObjectID);
				if (pObject) {
					AttachToObject(pObject);
				}
			}
		}
	}
	else if (m_entity && (m_byteMoving & 1)) {
		sa::CVector vecSpeed = {0.0f, 0.0f, 0.0f};

		RwMatrix matEnt = m_entity->GetMatrix().ToRwMatrix();

		float distance = fElapsedTime * m_fMoveSpeed;
		float remaining = DistanceRemaining(&matEnt);
		uint32_t dwThisTick = GetTickCount();

		float posX = matEnt.pos.x;
		float posY = matEnt.pos.y;
		float posZ = matEnt.pos.z;

		float f1 = ((float) (dwThisTick - m_dwMoveTick)) * 0.001f * m_fMoveSpeed;
		float f2 = m_fDistanceToTargetPoint - remaining;

		if (distance >= remaining) {
			m_entity->SetVelocity(vecSpeed);
			m_entity->SetTurnSpeed(vecSpeed);
			matEnt.pos.x = m_matTarget.pos.x;
			matEnt.pos.y = m_matTarget.pos.y;
			matEnt.pos.z = m_matTarget.pos.z;
			if (m_bNeedRotate) {
				m_quatTarget.GetMatrix(&matEnt);
			}
			UpdateMatrix(matEnt);
			StopMoving();
		}
		else if (fElapsedTime > 0.0f) {
			float delta = 1.0f / (remaining / distance);
			matEnt.pos.x += ((m_matTarget.pos.x - matEnt.pos.x) * delta);
			matEnt.pos.y += ((m_matTarget.pos.y - matEnt.pos.y) * delta);
			matEnt.pos.z += ((m_matTarget.pos.z - matEnt.pos.z) * delta);

			float progressRemain = (m_fDistanceToTargetPoint > 0.0001f)
					? (remaining / m_fDistanceToTargetPoint)
					: 0.0f;
			if (progressRemain < 0.0f) progressRemain = 0.0f;
			if (progressRemain > 1.0f) progressRemain = 1.0f;
			float slerpDelta = 1.0f - progressRemain;

			delta = 1.0f / fElapsedTime;
			vecSpeed.x = (matEnt.pos.x - posX) * delta * 0.02f;
			vecSpeed.y = (matEnt.pos.y - posY) * delta * 0.02f;
			vecSpeed.z = (matEnt.pos.z - posZ) * delta * 0.02f;

			if (FloatOffset(f1, f2) > 0.1f) {
				if (f1 > f2) {
					delta = (f1 - f2) * 0.1f + 1.0f;
					vecSpeed.x *= delta;
					vecSpeed.y *= delta;
					vecSpeed.z *= delta;
				}

				if (f2 > f1) {
					delta = 1.0f - (f2 - f1) * 0.1f;
					vecSpeed.x *= delta;
					vecSpeed.y *= delta;
					vecSpeed.z *= delta;
				}
			}

			m_entity->SetVelocity(vecSpeed);
			ApplyMoveSpeed();

			if (m_bNeedRotate) {
				float fx, fy, fz;
				GetRotation(&fx, &fy, &fz);
				float desiredZ = m_vecRotationTarget.z - progressRemain * m_vecSubRotationTarget.z;
				vecSpeed.x = 0.0f;
				vecSpeed.y = 0.0f;
				vecSpeed.z = subAngle(fz, desiredZ) * 0.01f;
				if (vecSpeed.z <= 0.001f) {
					if (vecSpeed.z < -0.001f) {
						vecSpeed.z = -0.001f;
					}
				}
				else {
					vecSpeed.z = 0.001f;
				}

				m_entity->SetTurnSpeed(vecSpeed);
				matEnt = m_entity->GetMatrix().ToRwMatrix();
				sa::CQuaternion quat;
				quat.Slerp(&m_quatStart, &m_quatTarget, slerpDelta);
				quat.Normalize();
				quat.GetMatrix(&matEnt);
			}
			else {
				matEnt = m_entity->GetMatrix().ToRwMatrix();
			}

			UpdateMatrix(matEnt);
		}
	}

	// Re-sync OOB state after moving/attaching so crossing +-3000 takes effect immediately
	if (m_entity) {
		const sa::CVector& pos = m_entity->GetPosition();
		m_bForceRender = pos.x > 3000.0f || pos.x < -3000.0f
				|| pos.y > 3000.0f || pos.y < -3000.0f;
	}

	// inject LAST — the moving path above calls UpdateMatrix
	// (CWorld::Remove/Add), which strips our sector records mid-tick, and
	// CPhysical::Add rejects the OOB re-add; running the injection after it
	// keeps the entity collidable through the next engine collision pass.
	// Reached by attached objects too, so a fresh attach still unlinks
	// stale records.
	if (m_entity && m_entity->m_pRwObject
			&& (m_bForceRender || !m_aInjectedInfo.empty())) {
		EnsureRepeatSector();
	}
}

/**
 * @param ObjectID
 * @param vecPos
 * @param vecRot
 * @param bSyncRotation
 */
void CObject::SetAttachedObject(uint16_t ObjectID, sa::CVector* vecPos, sa::CVector* vecRot, bool bSyncRotation)
{
	if (ObjectID == INVALID_OBJECT_ID) {
		m_AttachedObjectID = INVALID_OBJECT_ID;
		m_vecAttachedPos.x = 0.0f;
		m_vecAttachedPos.y = 0.0f;
		m_vecAttachedPos.z = 0.0f;
		m_vecAttachedRot.x = 0.0f;
		m_vecAttachedRot.y = 0.0f;
		m_vecAttachedRot.z = 0.0f;
		m_bSyncRotation = false;
	}
	else {
		m_AttachedObjectID = ObjectID;
		m_vecAttachedPos.x = vecPos->x;
		m_vecAttachedPos.y = vecPos->y;
		m_vecAttachedPos.z = vecPos->z;
		m_vecAttachedRot.x = vecRot->x;
		m_vecAttachedRot.y = vecRot->y;
		m_vecAttachedRot.z = vecRot->z;
		m_bSyncRotation = bSyncRotation;
	}
}

void CObject::SetScale(sa::CVector vecScale)
{
	// todo: GTA objects have no scale; scaling applies to attached-to-ped objects only
}

sa::CVector CObject::GetScale()
{
	// todo: see SetScale
	return sa::CVector{1.0f, 1.0f, 1.0f};
}

/**
 * @param vecRotation
 */
void CObject::SetRotation(sa::CVector* vecRotation)
{
	if (m_entity && GamePool_Object_GetAt(m_gtaId) == reinterpret_cast<sa::CObject*>(m_entity)) {
		ScriptCommand(&set_object_rotation, m_gtaId, vecRotation->x, vecRotation->y, vecRotation->z);
		m_vecRotation.x = vecRotation->x;
		m_vecRotation.y = vecRotation->y;
		m_vecRotation.z = vecRotation->z;
		if (m_bForceRender) {
			UpdateRwMatrixAndFrame();
		}
	}
}

/**
 * @param pfX
 * @param pfY
 * @param pfZ
 */
void CObject::GetRotation(float* pfX, float* pfY, float* pfZ)
{
	*pfX = 0.0f;
	*pfY = 0.0f;
	*pfZ = 0.0f;

	if (m_entity) {
		sa::CMatrix* mat = m_entity->m_matrix;

		if (mat) {
			// CMatrix::ConvertToEulerAngles
			eng::fn::CMatrix_ConvertToEulerAngles( mat, pfX, pfY, pfZ, 21);
		}

		*pfX = *pfX * 57.295776 * -1.0;
		*pfY = *pfY * 57.295776 * -1.0;
		*pfZ = *pfZ * 57.295776 * -1.0;
	}
}

/**
 * @param fX
 * @param fY
 * @param fZ
 */
void CObject::TeleportTo(float fX, float fY, float fZ)
{
	if (fX > 3000.0f || fX < -3000.0f || fY > 3000.0f || fY < -3000.0f) {
		m_bForceRender = true;
	}
	else {
		m_bForceRender = false;
	}

	CEntity::TeleportTo(fX, fY, fZ);
}

/**
 * @param VehicleID
 * @param vecPos
 * @param vecRot
 */
void CObject::SetAttachedVehicle(uint16_t VehicleID, sa::CVector* vecPos, sa::CVector* vecRot)
{
	if (VehicleID == INVALID_VEHICLE_ID) {
		m_AttachedVehicleID = INVALID_VEHICLE_ID;
		m_vecAttachedPos.x = 0.0f;
		m_vecAttachedPos.y = 0.0f;
		m_vecAttachedPos.z = 0.0f;
		m_vecAttachedRot.x = 0.0f;
		m_vecAttachedRot.y = 0.0f;
		m_vecAttachedRot.z = 0.0f;
	}
	else {
		m_AttachedVehicleID = VehicleID;
		m_vecAttachedPos.x = vecPos->x;
		m_vecAttachedPos.y = vecPos->y;
		m_vecAttachedPos.z = vecPos->z;
		m_vecAttachedRot.x = vecRot->x;
		m_vecAttachedRot.y = vecRot->y;
		m_vecAttachedRot.z = vecRot->z;
	}
}

/**
 * @param fX
 * @param fY
 * @param fZ
 * @param fSpeed
 * @param fRotX
 * @param fRotY
 * @param fRotZ
 */
void CObject::MoveTo(float fX, float fY, float fZ, float fSpeed, float fRotX, float fRotY, float fRotZ)
{
	if (!m_entity) {
		return;
	}

	// malformed packet coords must not propagate NaN into the engine
	// matrix — it spreads to every entity sharing the transform
	if (!std::isfinite(fX) || !std::isfinite(fY) || !std::isfinite(fZ)) {
		spdlog::warn("CObject::MoveTo: rejected non-finite target [{}, {}, {}]", fX, fY, fZ);
		return;
	}

	// a non-positive speed can never arrive — distance>=remaining is never
	// reached so the object creeps forever (the !fSpeed>0 test also catches
	// NaN speeds). Snap to the target instead.
	if (!(fSpeed > 0.0f)) {
		TeleportTo(fX, fY, fZ);
		return;
	}

	RwMatrix mat = m_entity->GetMatrix().ToRwMatrix();

	if (m_byteMoving & 1) {
		StopMoving();
		mat.pos.x = m_matTarget.pos.x;
		mat.pos.y = m_matTarget.pos.y;
		mat.pos.z = m_matTarget.pos.z;

		if (m_bNeedRotate) {
			m_quatTarget.GetMatrix(&mat);
		}

		UpdateMatrix(mat);
	}

	m_dwMoveTick = GetTickCount();
	m_fMoveSpeed = fSpeed;
	m_matTarget.pos.x = fX;
	m_matTarget.pos.y = fY;
	m_matTarget.pos.z = fZ;
	m_byteMoving |= 1;

	if (fRotX <= -999.0f || fRotY <= -999.0f || fRotZ <= -999.0f) {
		m_bNeedRotate = false;
	}
	else {
		m_bNeedRotate = true;

		sa::CVector vecRot;
		RwMatrix matrix;
		GetRotation(&vecRot.x, &vecRot.y, &vecRot.z);
		m_vecRotationTarget.x = fixAngle(fRotX);
		m_vecRotationTarget.y = fixAngle(fRotY);
		m_vecRotationTarget.z = fixAngle(fRotZ);

		m_vecSubRotationTarget.x = subAngle(vecRot.x, m_vecRotationTarget.x);
		m_vecSubRotationTarget.y = subAngle(vecRot.y, m_vecRotationTarget.y);
		m_vecSubRotationTarget.z = subAngle(vecRot.z, m_vecRotationTarget.z);

		RotateMatrix(sa::CVector{fRotX, fRotY, fRotZ});
		matrix = m_entity->GetMatrix().ToRwMatrix();
		m_quatStart.SetFromMatrix(&matrix);
		m_quatTarget.SetFromMatrix(&m_matTarget);
		m_quatStart.Normalize();
		m_quatTarget.Normalize();
	}

	m_fDistanceToTargetPoint = GetDistanceFromPoint(m_matTarget.pos);

	if (pNetGame) {
		CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
		if (pPlayerPool && pPlayerPool->GetLocalPlayer()) {
			pPlayerPool->GetLocalPlayer()->UpdateSurfing();
		}
	}

	// sub_1009F070
	m_entity->m_nFlags &= 0xFFFFFFF7;
}

/**
 * @param vecRot
 */
void CObject::RotateMatrix(sa::CVector vecRot)
{
	m_vecRotation = vecRot;

	vecRot.x *= 0.017453292f; // x * pi/180
	vecRot.y *= 0.017453292f; // y * pi/180
	vecRot.z *= 0.017453292f; // z * pi/180

	float cosx = cos(vecRot.x);
	float sinx = sin(vecRot.x);

	float cosy = cos(vecRot.y);
	float siny = sin(vecRot.y);

	float cosz = cos(vecRot.z);
	float sinz = sin(vecRot.z);

	float sinzx = sinz * sinx;
	float coszx = cosz * sinx;

	m_matTarget.right.x = cosz * cosy - sinzx * siny;
	m_matTarget.right.y = coszx * siny + sinz * cosy;
	m_matTarget.right.z = -(siny * cosx);
	m_matTarget.up.x = -(sinz * cosx);
	m_matTarget.up.y = cosz * cosx;
	m_matTarget.up.z = sinx;
	m_matTarget.at.x = sinzx * cosy + cosz * siny;
	m_matTarget.at.y = sinz * siny - coszx * cosy;
	m_matTarget.at.z = cosy * cosx;
}

void CObject::StopMoving()
{
	sa::CVector vec = {0.0f, 0.0f, 0.0f};
	if (m_entity) {
		m_entity->SetVelocity(vec);
		m_entity->SetTurnSpeed(vec);
	}

	m_byteMoving &= ~1;
}

/**
 * @return
 */
bool CObject::AttachedToMovingEntity()
{
	if (m_AttachedObjectID == INVALID_OBJECT_ID) {
		if (m_AttachedVehicleID != INVALID_VEHICLE_ID) {
			return true;
		}

		return (m_byteMoving & 1);
	}
	else {
		if (m_AttachedObjectID >= 0 && m_AttachedObjectID < MAX_OBJECTS) {
			if (pNetGame) {
				CObjectPool* pObjectPool = pNetGame->GetObjectPool();
				if (pObjectPool) {
					CObject* pObject = pObjectPool->GetAt(m_AttachedObjectID);
					if (pObject) return (pObject->m_byteMoving & 1);
				}
			}
		}
	}

	return false;
}

void CObject::SetMaterial(int iModel, uint8_t byteMaterialIndex, char* txdname, char* texturename, uint32_t dwColor)
{
	spdlog::info("CObject::SetMaterial({}, {}, {}, {}, 0x{:X}).", iModel, byteMaterialIndex, txdname ? txdname : "", texturename ? texturename : "", dwColor);

	if (byteMaterialIndex >= MAX_MATERIALS) {
		return;
	}

	if (iModel > 0 && (!IsValidModel(iModel) || !txdname || !txdname[0] || !texturename || !texturename[0])) {
		return;
	}

	if (m_Material[byteMaterialIndex].bTextureWasCreated && m_Material[byteMaterialIndex].pMaterialTexture
		&& m_Material[byteMaterialIndex].pMaterialTexture != g_blankTexture) {
		RwTextureDestroy(m_Material[byteMaterialIndex].pMaterialTexture);
	}
	m_Material[byteMaterialIndex].pMaterialTexture = nullptr;
	m_Material[byteMaterialIndex].bTextGenAttempts = 0;

	bool bIsNoneTex = (txdname && strcasecmp(txdname, "none") == 0)
			|| (texturename && strcasecmp(texturename, "none") == 0);

	if (iModel > 0) {
		if (!bIsNoneTex) {
			if (!pGame->IsModelLoaded(iModel)) {
				pGame->RequestModel(iModel);
				pGame->LoadRequestedModels();
			}

			if (pGame->IsModelLoaded(iModel)) {
				m_Material[byteMaterialIndex].pMaterialTexture = LoadTextureFromTxd(txdname, texturename);

				if (!ModelInfoGetReferenceCount(iModel)) {
					pGame->RemoveModel(iModel, false);
				}
			}
		}
		// When txd/texture is "none", leave pMaterialTexture null (matches PC SA-MP:
		// keeps the object's original texture while applying dwColor if non-zero).
	}
	else if (iModel == 0) {
		if (!g_blankTexture) {
			g_blankTexture = LoadTextureFromTxd("*", "blanktex");
		}

		m_Material[byteMaterialIndex].pMaterialTexture = g_blankTexture;
	}
	// iModel < 0: no texture, color only

	m_Material[byteMaterialIndex].iType = MATERIAL_TYPE_TEXTURE;
	m_Material[byteMaterialIndex].dwMaterialColor = dwColor;
	m_Material[byteMaterialIndex].bTextureWasCreated = true;
	m_bHasCustomObjectMaterial = true;
}

void CObject::SetMaterialText(uint8_t byteMaterialIndex, uint8_t byteMaterialSize, const char* szFontName, uint8_t byteFontSize, uint8_t byteFontBold, uint32_t dwFontColor, uint32_t dwBackgroundColor, uint8_t byteAlign, const char* szText)
{
	spdlog::info("CObject::SetMaterialText({}, {}, {}, {}, {}, 0x{:X}, 0x{:X}, {}, {}).", byteMaterialIndex, byteMaterialSize, szFontName ? szFontName : "", byteFontSize, byteFontBold, dwFontColor, dwBackgroundColor, byteAlign, szText ? szText : "");

	if (byteMaterialIndex < MAX_MATERIALS) {
		if (!szText || !strlen(szText)) {
			return;
		}

		if (m_Material[byteMaterialIndex].bTextureWasCreated && m_Material[byteMaterialIndex].pMaterialTexture
			&& m_Material[byteMaterialIndex].pMaterialTexture != g_blankTexture) {
			RwTextureDestroy(m_Material[byteMaterialIndex].pMaterialTexture);
		}
		m_Material[byteMaterialIndex].pMaterialTexture = nullptr;

		m_Material[byteMaterialIndex].TextInfo.byteMaterialSize = byteMaterialSize;

		memset(m_Material[byteMaterialIndex].TextInfo.szFontName, 0, 32);
		strncpy(m_Material[byteMaterialIndex].TextInfo.szFontName, szFontName ? szFontName : "", 31);

		m_Material[byteMaterialIndex].TextInfo.byteFontSize = byteFontSize;
		m_Material[byteMaterialIndex].TextInfo.byteFontBold = byteFontBold;
		m_Material[byteMaterialIndex].TextInfo.dwFontColor = dwFontColor;
		m_Material[byteMaterialIndex].TextInfo.dwBackgroundColor = dwBackgroundColor;
		m_Material[byteMaterialIndex].TextInfo.byteAlign = byteAlign;

		memset(m_Material[byteMaterialIndex].TextInfo.szText, 0, 2048);
		strncpy(m_Material[byteMaterialIndex].TextInfo.szText, szText, 2047);

		m_Material[byteMaterialIndex].iType = MATERIAL_TYPE_TEXT;
		m_Material[byteMaterialIndex].dwMaterialColor = 0;
		m_Material[byteMaterialIndex].bTextureWasCreated = false;
		m_Material[byteMaterialIndex].bTextGenAttempts = 0;

		if (!m_bHasCustomObjectMaterial) m_bHasCustomObjectMaterial = true;
	}
}

void CObject::GetMaterialTextSize(int iMatSize, int* iSizeX, int* iSizeY)
{
	int sizes[14][2] = {
			{32, 32}, {64, 32}, {64, 64}, {128, 32}, {128, 64}, {128, 128}, {256, 32},
			{256, 64}, {256, 128}, {256, 256}, {512, 64}, {512, 128}, {512, 256}, {512, 512}
	};

	int del = (iMatSize / 10) - 1;
	if (del < 0) del = 0;
	if (del > 13) del = 13;
	*iSizeX = sizes[del][0];
	*iSizeY = sizes[del][1];
}

void CObject::ProcessMaterialText()
{
	if (!pMaterialTextGenerator) {
		return;
	}

	for (int i = 0; i < MAX_MATERIALS; i++) {
		if (m_Material[i].bTextureWasCreated || m_Material[i].iType != MATERIAL_TYPE_TEXT) {
			continue;
		}

		if (!strlen(m_Material[i].TextInfo.szText)) {
			continue;
		}

		if (m_Material[i].TextInfo.byteMaterialSize < MATERIAL_SIZE_32X32) {
			m_Material[i].TextInfo.byteMaterialSize = MATERIAL_SIZE_32X32;
		}
		else if (m_Material[i].TextInfo.byteMaterialSize > MATERIAL_SIZE_512X512) {
			m_Material[i].TextInfo.byteMaterialSize = MATERIAL_SIZE_512X512;
		}

		int iSizeX, iSizeY;
		GetMaterialTextSize(m_Material[i].TextInfo.byteMaterialSize, &iSizeX, &iSizeY);

		if (m_Material[i].TextInfo.byteAlign < MATERIAL_ALIGN_LEFT || m_Material[i].TextInfo.byteAlign > MATERIAL_ALIGN_RIGHT) {
			m_Material[i].TextInfo.byteAlign = MATERIAL_ALIGN_CENTER;
		}

		// Match PC SA-MP (D3DXCreateFontA passes fontSize directly as pixel height).
		// Clamp 0 -> 1 so ImGuiRenderer::drawText does not fall back to m_font->FontSize.
		uint8_t byteFontSize = m_Material[i].TextInfo.byteFontSize;
		if (byteFontSize == 0) {
			byteFontSize = 1;
		}

		m_Material[i].pMaterialTexture = pMaterialTextGenerator->Create(iSizeX, iSizeY, m_Material[i].TextInfo.szFontName, byteFontSize, m_Material[i].TextInfo.byteFontBold, m_Material[i].TextInfo.dwFontColor, m_Material[i].TextInfo.dwBackgroundColor, m_Material[i].TextInfo.byteAlign, m_Material[i].TextInfo.szText);

		if (m_Material[i].pMaterialTexture) {
			m_Material[i].bTextureWasCreated = true;
			spdlog::info("ProcessMaterialText: texture created {}x{} for slot {}", iSizeX, iSizeY, i);
		}
		else {
			// a failed first attempt used to latch bTextureWasCreated and
			// render the material textless forever — retry a few times
			if (++m_Material[i].bTextGenAttempts >= 5) {
				spdlog::error("ProcessMaterialText: giving up on slot {} after {} attempts", i, (int) m_Material[i].bTextGenAttempts);
				m_Material[i].bTextureWasCreated = true;
			}
		}
	}
}

/**
 * Mobile libGTASA only renders UV-animated textures through the MatFX
 * pipeline: CEntity::PreRender advances the animation (MaterialUpdateUVAnimCB)
 * only when RpMatFXAtomicQueryEffects(atomic) != 0, and the animated UV
 * matrix is applied exclusively by _rpMatFXOpenGLAllInOneRenderCB via
 * _rpMatFXOpenGLUVAnimRender (rpMATFXEFFECTUVTRANSFORM = 5). The default
 * all-in-one render callback contains no UV-anim code at all, so models
 * carrying plain RpUVAnim material data (e.g. water planes 19603/19604)
 * render static. The exported RunUVAnim flag is dead — nothing reads it.
 * Fix: enable the MatFX UV transform effect on materials that have UVAnim
 * data, link the matrix slots to the UVAnim extension matrices, and switch
 * the atomic to the MatFX pipeline so the engine drives the animation.
 */
static std::unordered_set<RpMaterial*> s_uvAnimUpdatedMaterialsThisFrame;
static float s_fUVAnimFrameDt = 0.016f;
static uint32_t s_dwUVAnimLastTick = 0;

void BeginUVAnimFrame()
{
	s_uvAnimUpdatedMaterialsThisFrame.clear();
	uint32_t dwNow = GetTickCount();
	if (s_dwUVAnimLastTick == 0) {
		s_dwUVAnimLastTick = dwNow;
		s_fUVAnimFrameDt = 0.016f;
		return;
	}
	float dt = (dwNow - s_dwUVAnimLastTick) / 1000.0f;
	s_dwUVAnimLastTick = dwNow;
	if (std::isfinite(dt) && dt > 0.0f) {
		s_fUVAnimFrameDt = dt > 0.1f ? 0.1f : dt;
	}
	else {
		s_fUVAnimFrameDt = 0.016f;
	}
}

static void ForEachObjectAtomic(RwObject* rwObj, RpAtomic* (*cb)(RpAtomic*, void*), void* data)
{
	if (!rwObj || !cb) {
		return;
	}

	uint8_t type = *(uint8_t*) rwObj;
	if (type == 1 /*rpATOMIC*/) {
		cb(reinterpret_cast<RpAtomic*>(rwObj), data);
	}
	else if (type == 2 /*rpCLUMP*/) {
		eng::fn::RpClumpForAllAtomics<RpClump*>(
				reinterpret_cast<RpClump*>(rwObj), cb, data);
	}
	else if (rwObj->parent) {
		auto parent = reinterpret_cast<RwFrame*>(rwObj->parent);
		RwFrameForAllObjects(parent, reinterpret_cast<RpAtomicCallBack*>(cb), data);
	}
}

RpMaterial* EnableUVAnimMaterialCB(RpMaterial* material, void* data)
{
	if (!material) {
		return material;
	}

	bool bHasUVAnim = eng::fn::RpMaterialUVAnimExists( material);
	if (!bHasUVAnim) {
		return material;
	}

	if (data) {
		*(bool*) data = true;
	}

	// Ensure material texture uses WRAP addressing so scrolling UVs tile seamlessly.
	if (material->texture) {
		RwTextureSetAddressingMacro(material->texture, rwTEXTUREADDRESSWRAP);
	}

	// If the material uses MatFX UV transform (5 = UVTRANSFORM, 6 = DUALUVTRANSFORM),
	// link its matrix slots to ext->uv[0..1] updated by DriveUVAnimMaterialCB.
	int32_t iEffects = eng::fn::RpMatFXMaterialGetEffects( material);
	if (iEffects == 5 /*rpMATFXEFFECTUVTRANSFORM*/ || iEffects == 6 /*rpMATFXEFFECTDUALUVTRANSFORM*/) {
		auto* pUVAnimGlobals = eng::as<RpUVAnimMaterialGlobalVars*>(eng::Sym::RpUVAnimMaterialGlobals);
		if (pUVAnimGlobals) {
			auto* ext = (RpUVAnimMaterialExtension*) ((char*) material + pUVAnimGlobals->engineOffset);
			if (ext->uv[0] || ext->uv[1]) {
				eng::fn::RpMatFXMaterialSetUVTransformMatrices( material, ext->uv[0], ext->uv[1]);
			}
		}
	}

	return material;
}

RpAtomic* EnableUVAnimAtomicCB(RpAtomic* atomic, void* data)
{
	if (atomic && *(uint8_t*) atomic == 1 /*rpATOMIC*/ && atomic->geometry) {
		bool bHasUVAnimMaterial = false;
		eng::fn::RpGeometryForAllMaterials<RpGeometry*>( atomic->geometry, (RpMaterial* (*)(RpMaterial*, void*)) EnableUVAnimMaterialCB, &bHasUVAnimMaterial);
		if (!bHasUVAnimMaterial) {
			return atomic;
		}

		if (data) {
			auto* pObj = reinterpret_cast<CObject*>(data);
			pObj->m_bUVAnimEnabled = true;

			static std::unordered_set<int> s_loggedModels;
			if (s_loggedModels.size() < 24 && s_loggedModels.insert(pObj->GetModelIndex()).second) {
				spdlog::info("UVAnim: enabled for object model {} (atomic={:p}, pipeline={:p})",
						pObj->GetModelIndex(), (void*) atomic, (void*) atomic->pipeline);
			}
		}
		// Do NOT switch atomic->pipeline here: switching an already-instanced
		// CustomBuilding atomic to MatFX triggers vertex re-instancing on a
		// geometry whose morphTarget->verts has been freed (SIGSEGV at +0x3d4).
		// Instead, CustomPipeRenderCB / rxOpenGLDefaultAllInOneRenderCB hooks
		// apply ext->uv[0] via emu_SetTextureMatrixEnabled directly.
	}

	return atomic;
}

RpMaterial* DriveUVAnimMaterialCB(RpMaterial* material, void* data)
{
	if (!material) {
		return material;
	}

	// Multiple object instances of the same model share one RpMaterial —
	// advance each material at most once per game frame.
	if (!s_uvAnimUpdatedMaterialsThisFrame.insert(material).second) {
		return material;
	}

	if (!eng::fn::RpMaterialUVAnimExists( material)) {
		return material;
	}

	auto* pUVAnimGlobals = eng::as<RpUVAnimMaterialGlobalVars*>(eng::Sym::RpUVAnimMaterialGlobals);
	if (!pUVAnimGlobals) {
		return material;
	}

	auto* ext = (RpUVAnimMaterialExtension*) ((char*) material + pUVAnimGlobals->engineOffset);
	if (!ext->uv[0] && !ext->uv[1]) {
		return material;
	}

	// War Drum's mobile port gates RtAnimInterpolatorAddAnimTime behind
	// Set360UVAnimHack (default false), and enabling it hits an infinite loop
	// in RtAnimInterpolatorAddAnimTime on multi-keyframe DFFs (e.g. 19620/19797)
	// plus RpUVAnimParamKeyFrameApply has a 64-bit struct offset bug (+12 vs +16).
	// Step and evaluate UV keyframes directly in a bounded, crash-proof loop.
	//
	// RenderWare interpolator typeIDs (verified from libGTASA.so):
	//   0x1c0 (448) = _rpUVAnimLinearInterpolatorInfo (uv[6])
	//   0x1c1 (449) = _rpUVAnimParamInterpolatorInfo  (theta, s0, s1, skew, x, y)
	constexpr int32_t kUVAnimTypeLinear = 0x1c0;
	constexpr int32_t kUVAnimTypeParam  = 0x1c1;
	constexpr float kPi    = 3.14159265358979323846f;
	constexpr float kTwoPi = 6.28318530717958647692f;

	bool bChanWritten[RP_UVANIM_APPLIEDCHANNELS] = {false, false};

	for (int slot = 0; slot < RP_UVANIM_MAXSLOTS; ++slot) {
		RtAnimInterpolator* interp = ext->interp[slot];
		if (!interp || !interp->pCurrentAnim) {
			continue;
		}
		RtAnimAnimation* anim = interp->pCurrentAnim;
		if (!anim->pFrames || anim->numFrames < 2 || anim->duration <= 0.0001f || !std::isfinite(anim->duration)) {
			continue;
		}

		// Advance and wrap currentTime strictly inside [0, duration)
		float curTime = interp->currentTime;
		if (!std::isfinite(curTime) || curTime < 0.0f) {
			curTime = 0.0f;
		}
		curTime = std::fmod(curTime + s_fUVAnimFrameDt, anim->duration);
		if (curTime < 0.0f || !std::isfinite(curTime)) {
			curTime = 0.0f;
		}
		interp->currentTime = curTime;

		int32_t typeID = anim->interpInfo ? anim->interpInfo->typeID : kUVAnimTypeParam;
		if (typeID != kUVAnimTypeLinear && typeID != kUVAnimTypeParam) {
			continue;
		}

		const auto* kfs = reinterpret_cast<const RpUVAnimKeyFrame*>(anim->pFrames);
		int count = anim->numFrames;
		if (count > 1024) {
			count = 1024;
		}

		int numNodes = interp->numNodes;
		if (numNodes < 1) {
			numNodes = 1;
		}
		if (numNodes > RP_UVANIM_MAXSLOTS) {
			numNodes = RP_UVANIM_MAXSLOTS;
		}
		if (numNodes > count) {
			numNodes = 1;
		}

		// For multi-node animations (e.g. dual-channel waterfalls), classify
		// each keyframe's owning node via validated prevFrame back-links.
		int8_t nodeOf[1024];
		if (numNodes > 1) {
			const uintptr_t baseAddr = reinterpret_cast<uintptr_t>(kfs);
			const size_t stride = sizeof(RpUVAnimKeyFrame);
			for (int k = 0; k < count; ++k) {
				if (k < numNodes) {
					nodeOf[k] = static_cast<int8_t>(k);
				}
				else {
					int8_t owner = 0;
					const uintptr_t prevAddr = reinterpret_cast<uintptr_t>(kfs[k].prevFrame);
					if (prevAddr >= baseAddr && prevAddr < baseAddr + static_cast<size_t>(k) * stride
							&& ((prevAddr - baseAddr) % stride) == 0) {
						int prevIdx = static_cast<int>((prevAddr - baseAddr) / stride);
						owner = nodeOf[prevIdx];
					}
					nodeOf[k] = owner;
				}
			}
		}

		const auto* customData = reinterpret_cast<const _rpUVAnimCustomData*>(anim->customData);

		for (int node = 0; node < numNodes; ++node) {
			uint32_t chan = customData ? customData->nodeToUVChannelMap[node] : 0u;
			if (chan >= RP_UVANIM_APPLIEDCHANNELS) {
				continue;
			}
			RwMatrix* dst = ext->uv[chan];
			if (!dst) {
				continue;
			}

			// Find bracketing keyframe pair for this node using strict upper bound
			// so step/duplicate timestamps (e.g. LightBar1 0.500/0.500) switch cleanly.
			int idx1 = -1;
			int idx2 = -1;
			int prevNodeIdx = -1;
			for (int k = 0; k < count; ++k) {
				if (numNodes > 1 && nodeOf[k] != node) {
					continue;
				}
				if (prevNodeIdx >= 0) {
					float t1 = kfs[prevNodeIdx].time;
					float t2 = kfs[k].time;
					if (curTime >= t1 && curTime < t2) {
						idx1 = prevNodeIdx;
						idx2 = k;
						break;
					}
				}
				prevNodeIdx = k;
			}
			if (idx1 < 0) {
				// curTime is either before first keyframe or at/past last keyframe
				int firstNodeIdx = -1;
				for (int k = 0; k < count; ++k) {
					if (numNodes == 1 || nodeOf[k] == node) {
						firstNodeIdx = k;
						break;
					}
				}
				if (firstNodeIdx < 0) {
					continue;
				}
				if (curTime < kfs[firstNodeIdx].time) {
					idx1 = firstNodeIdx;
					idx2 = firstNodeIdx;
				}
				else {
					idx1 = prevNodeIdx;
					idx2 = prevNodeIdx;
				}
			}

			const RpUVAnimKeyFrame& kf1 = kfs[idx1];
			const RpUVAnimKeyFrame& kf2 = kfs[idx2];

			float span = kf2.time - kf1.time;
			float alpha = (span > 0.00001f) ? ((curTime - kf1.time) / span) : 0.0f;
			if (alpha < 0.0f) alpha = 0.0f;
			if (alpha > 1.0f) alpha = 1.0f;

			float rx = 1.0f, ry = 0.0f;
			float ux = 0.0f, uy = 1.0f;
			float tx = 0.0f, ty = 0.0f;

			if (typeID == kUVAnimTypeLinear) {
				float uv[6];
				for (int j = 0; j < 6; ++j) {
					uv[j] = kf1.data.linear.uv[j] + (kf2.data.linear.uv[j] - kf1.data.linear.uv[j]) * alpha;
				}
				rx = uv[0]; ry = uv[1];
				ux = uv[2]; uy = uv[3];
				tx = uv[4]; ty = uv[5];
			}
			else {
				// Parametric keyframe (matches RpUVAnimParamKeyFrameInterpolate + Apply):
				//   M = Base(s0, skew, s1, x, y) * T(-0.5, -0.5) * R_z(theta) * T(+0.5, +0.5)
				float dTheta = kf2.data.param.theta - kf1.data.param.theta;
				if (dTheta > kPi) {
					dTheta -= kTwoPi;
				}
				else if (dTheta < -kPi) {
					dTheta += kTwoPi;
				}
				float theta = kf1.data.param.theta + dTheta * alpha;
				float s0   = kf1.data.param.s0   + (kf2.data.param.s0   - kf1.data.param.s0)   * alpha;
				float s1   = kf1.data.param.s1   + (kf2.data.param.s1   - kf1.data.param.s1)   * alpha;
				float skew = kf1.data.param.skew + (kf2.data.param.skew - kf1.data.param.skew) * alpha;
				float px   = kf1.data.param.x    + (kf2.data.param.x    - kf1.data.param.x)    * alpha;
				float py   = kf1.data.param.y    + (kf2.data.param.y    - kf1.data.param.y)    * alpha;

				float c = std::cos(theta);
				float s = std::sin(theta);
				rx = s0 * c - skew * s;
				ry = s0 * s + skew * c;
				ux = -s1 * s;
				uy =  s1 * c;
				tx = (px - 0.5f) * c - (py - 0.5f) * s + 0.5f;
				ty = (px - 0.5f) * s + (py - 0.5f) * c + 0.5f;
			}

			if (!bChanWritten[chan]) {
				dst->right.x = rx;
				dst->right.y = ry;
				dst->right.z = 0.0f;
				dst->up.x = ux;
				dst->up.y = uy;
				dst->up.z = 0.0f;
				dst->at.x = 0.0f;
				dst->at.y = 0.0f;
				dst->at.z = 1.0f;
				dst->pos.x = tx;
				dst->pos.y = ty;
				dst->pos.z = 0.0f;
				// Clear rwMATRIXINTERNALIDENTITY (0x20000) so _rpMatFXOpenGLUVAnimRender uploads it
				dst->flags = 0;
				bChanWritten[chan] = true;
			}
			else {
				// Compose multiple slots targeting the same UV channel: dst = dst * slotMat
				float arx = dst->right.x, ary = dst->right.y;
				float aux = dst->up.x,    auy = dst->up.y;
				float atx = dst->pos.x,   aty = dst->pos.y;
				dst->right.x = arx * rx + ary * ux;
				dst->right.y = arx * ry + ary * uy;
				dst->up.x    = aux * rx + auy * ux;
				dst->up.y    = aux * ry + auy * uy;
				dst->pos.x   = atx * rx + aty * ux + tx;
				dst->pos.y   = atx * ry + aty * uy + ty;
				dst->flags = 0;
			}
		}
	}

	if (bChanWritten[0] || bChanWritten[1]) {
		// Guard against SIGSEGV: RpMatFXMaterialSetUVTransformMatrices dereferences
		// material's MatFX extension without a null check — only call when MatFX UV
		// transform effect (5 or 6) is present.
		int32_t iEffects = eng::fn::RpMatFXMaterialGetEffects( material);
		if (iEffects == 5 /*rpMATFXEFFECTUVTRANSFORM*/ || iEffects == 6 /*rpMATFXEFFECTDUALUVTRANSFORM*/) {
			eng::fn::RpMatFXMaterialSetUVTransformMatrices(
					material, ext->uv[0], ext->uv[1]);
		}
	}
	return material;
}

/*
 * CObject::PreRender is self-contained and does not run CEntity::PreRender's
 * MaterialUpdateUVAnimCB driver, so nothing advances the animation for game
 * objects. Drive UV-animated materials per frame from CObject::Process and
 * CPlayerPed::ProcessAttachedObjects.
 */
RpAtomic* DriveUVAnimAtomicCB(RpAtomic* atomic, void* data)
{
	if (atomic && *(uint8_t*) atomic == 1 /*rpATOMIC*/ && atomic->geometry) {
		eng::fn::RpGeometryForAllMaterials<RpGeometry*>(
				atomic->geometry, (RpMaterial* (*)(RpMaterial*, void*)) DriveUVAnimMaterialCB, nullptr);
	}

	return atomic;
}

RwMatrix* FindAtomicUVAnimMatrix(RpAtomic* atomic, bool* outHasDualUV)
{
	if (outHasDualUV) {
		*outHasDualUV = false;
	}
	if (!atomic || *(uint8_t*) atomic != 1 /*rpATOMIC*/ || !atomic->geometry) {
		return nullptr;
	}
	RpGeometry* geo = atomic->geometry;
	if (!geo->matList.materials || geo->matList.numMaterials <= 0) {
		return nullptr;
	}
	auto* pUVAnimGlobals = eng::as<RpUVAnimMaterialGlobalVars*>(eng::Sym::RpUVAnimMaterialGlobals);
	if (!pUVAnimGlobals) {
		return nullptr;
	}
	int count = geo->matList.numMaterials;
	if (count > 64) count = 64;
	RwMatrix* firstUv = nullptr;
	for (int i = 0; i < count; ++i) {
		RpMaterial* mat = geo->matList.materials[i];
		if (!mat) continue;
		if (!eng::fn::RpMaterialUVAnimExists( mat)) {
			continue;
		}
		auto* ext = (RpUVAnimMaterialExtension*) ((char*) mat + pUVAnimGlobals->engineOffset);
		RwMatrix* uv = ext->uv[0] ? ext->uv[0] : ext->uv[1];
		if (uv && (uv->flags & 0x20000) == 0) {
			if (!firstUv) {
				firstUv = uv;
			}
			if (outHasDualUV) {
				int32_t iEffects = eng::fn::RpMatFXMaterialGetEffects( mat);
				if (iEffects == 6 /*rpMATFXEFFECTDUALUVTRANSFORM*/) {
					*outHasDualUV = true;
				}
			}
			else {
				return firstUv;
			}
		}
	}
	return firstUv;
}

void CObject::TryEnableUVAnim()
{
	if (!m_entity || !m_entity->m_pRwObject) {
		m_pUVAnimRwObject = nullptr;
		m_bUVAnimEnabled = false;
		return;
	}

	if (m_pUVAnimRwObject == m_entity->m_pRwObject) {
		return;
	}

	m_pUVAnimRwObject = m_entity->m_pRwObject;
	m_bUVAnimEnabled = false;
	ForEachObjectAtomic(m_entity->m_pRwObject, EnableUVAnimAtomicCB, this);
}

void CObject::UpdateUVAnim()
{
	TryEnableUVAnim();

	if (m_bUVAnimEnabled && m_entity && m_entity->m_pRwObject) {
		ForEachObjectAtomic(m_entity->m_pRwObject, DriveUVAnimAtomicCB, this);
	}
}

void CObject::TryChangeToCustomObjectMaterial()
{
	if (m_bHasCustomObjectMaterial) {
		ChangeToCustomObjectMaterial();
	}
}

RpAtomic* ChangeCustomMaterialObjectCB(RpAtomic* result, void* data)
{
	auto* pObject = reinterpret_cast<CObject*>(data);
	if (result && pObject && *(uint8_t*) result == 1) {
		RpGeometry* geometry = result->geometry;
		if (geometry) {
			int count = geometry->matList.numMaterials;
			RpMaterial** materials = geometry->matList.materials;
			if (count >= 1 && materials) {
				for (int i = 0; i < count; i++) {
					if (i >= MAX_MATERIALS) break;
					if (!materials[i]) continue;

					if (pObject->m_Material[i].pMaterialTexture) {
						materials[i]->texture = pObject->m_Material[i].pMaterialTexture;
						pObject->m_bObjectMaterialTextureChanged = true;
					}

					if (pObject->m_Material[i].dwMaterialColor) {
						geometry->flags |= rpGEOMETRYMODULATEMATERIALCOLOR;
						geometry->flags &= 0xFFFFFFF7;
						materials[i]->color = *(RwRGBA*) &pObject->m_Material[i].dwMaterialColor;
						materials[i]->surfaceProps.ambient = 1.0f;
						materials[i]->surfaceProps.specular = 0.0f;
						materials[i]->surfaceProps.diffuse = 1.0f;
						pObject->m_bObjectMaterialColorChanged = true;
					}
				}
			}
		}
	}

	return result;
}

RpAtomic* StoreOriginalMaterialObjectCB(RpAtomic* result, void* data)
{
	auto* pObject = reinterpret_cast<CObject*>(data);
	if (result && pObject && *(uint8_t*) result == 1) {
		RpGeometry* geometry = result->geometry;
		if (geometry) {
			pObject->m_uiLastObjectGeometry = geometry->flags;

			int count = geometry->matList.numMaterials;
			RpMaterial** materials = geometry->matList.materials;
			if (count >= 1 && materials) {
				for (int i = 0; i < count; i++) {
					if (i >= MAX_MATERIALS) break;
					if (!materials[i]) continue;
					pObject->m_pLastObjectMaterialTexture[i] = materials[i]->texture;
					pObject->m_dwLastObjectMaterialColor[i] = *(RwRGBA*) &materials[i]->color;
					pObject->m_LastObjectMaterialSurface[i] = materials[i]->surfaceProps;
				}
			}
		}
	}

	return result;
}

RpAtomic* ChangeOriginalMaterialObjectCB(RpAtomic* result, void* data)
{
	auto* pObject = reinterpret_cast<CObject*>(data);
	if (result && pObject && *(uint8_t*) result == 1) {
		RpGeometry* geometry = result->geometry;
		if (geometry) {
			int count = geometry->matList.numMaterials;
			RpMaterial** materials = geometry->matList.materials;
			if (count >= 1 && materials) {
				geometry->flags = pObject->m_uiLastObjectGeometry;

				for (int i = 0; i < count; i++) {
					if (i >= MAX_MATERIALS) break;
					if (!materials[i]) continue;
					if (pObject->m_Material[i].pMaterialTexture && materials[i]->texture != pObject->m_pLastObjectMaterialTexture[i]) {
						materials[i]->texture = pObject->m_pLastObjectMaterialTexture[i];
					}
					if (pObject->m_bObjectMaterialColorChanged && pObject->m_Material[i].dwMaterialColor) {
						materials[i]->color = *(RwRGBA*) &pObject->m_dwLastObjectMaterialColor[i];
						materials[i]->surfaceProps = pObject->m_LastObjectMaterialSurface[i];
					}
				}

				pObject->m_bObjectMaterialTextureChanged = false;
				pObject->m_bObjectMaterialColorChanged = false;
			}
		}
	}

	return result;
}

void CObject::ChangeToCustomObjectMaterial()
{
	if (!m_bHasCustomObjectMaterial || !m_entity || !m_entity->m_pRwObject) {
		return;
	}

	if (m_pLastObjectMaterialRwObject != m_entity->m_pRwObject) {
		ForEachObjectAtomic(m_entity->m_pRwObject, StoreOriginalMaterialObjectCB, this);
		m_pLastObjectMaterialRwObject = m_entity->m_pRwObject;
	}

	ForEachObjectAtomic(m_entity->m_pRwObject, ChangeCustomMaterialObjectCB, this);
}

void CObject::ChangeToOriginalObjectMaterial()
{
	if (m_bHasCustomObjectMaterial) {
		if (m_bObjectMaterialTextureChanged || m_bObjectMaterialColorChanged) {
			if (m_entity && m_entity->m_pRwObject && m_pLastObjectMaterialRwObject == m_entity->m_pRwObject) {
				ForEachObjectAtomic(m_entity->m_pRwObject, ChangeOriginalMaterialObjectCB, this);
			}
		}
	}
}
