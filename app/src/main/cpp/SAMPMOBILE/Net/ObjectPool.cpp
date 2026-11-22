#include "../Game/Game.h"
#include "../Java/EditObject.h"
#include "NetGame.h"

extern Game* pGame;

CObjectPool::CObjectPool()
{
	for (OBJECTID ObjectID = 0; ObjectID < MAX_OBJECTS; ObjectID++) {
		m_bObjectSlotState[ObjectID] = false;
		m_pObjects[ObjectID] = nullptr;
	}
	m_iObjectCount = 0;
}

CObjectPool::~CObjectPool()
{
	for (OBJECTID ObjectID = 0; ObjectID < MAX_OBJECTS; ObjectID++) {
		Delete(ObjectID);
	}
}

/**
 * @param ObjectID
 * @param iModel
 * @param vecPos
 * @param vecRot
 * @param fDrawDistance
 * @return
 */
bool CObjectPool::New(OBJECTID ObjectID, int iModel, sa::CVector vecPos, sa::CVector vecRot, float fDrawDistance)
{
	spdlog::info("obj id: {} modelid: {}", ObjectID, iModel);

	if (ObjectID >= MAX_OBJECTS) return false;

	if (m_pObjects[ObjectID] != nullptr) {
		Delete(ObjectID);
	}

	m_pObjects[ObjectID] = pGame->NewObject(iModel, vecPos, vecRot, fDrawDistance);
	if (!m_pObjects[ObjectID]) return false;

	// a null m_entity means the engine never created the object (object
	// pool exhausted by IPL placement objects, or the opcode failed) — an
	// active slot with no entity is a zombie every later RPC silently
	// no-ops on, so fail the New instead of marking it live
	if (!m_pObjects[ObjectID]->m_entity) {
		spdlog::warn("CObjectPool::New: engine created no entity for object {} (model {})", ObjectID, iModel);
		delete m_pObjects[ObjectID];
		m_pObjects[ObjectID] = nullptr;
		return false;
	}

	m_mapGtaPtrToID[m_pObjects[ObjectID]->m_entity] = ObjectID;
	m_bObjectSlotState[ObjectID] = true;
	m_iObjectCount++;
	return true;
}

bool CObjectPool::Delete(OBJECTID ObjectID)
{
	if (ObjectID >= MAX_OBJECTS || !m_bObjectSlotState[ObjectID]) {
		return false;
	}

	CObject* pObject = m_pObjects[ObjectID];
	if (pObject) {
		if (EditObject::m_visible && EditObject::m_type == EditObject::TYPE_OBJECT
				&& EditObject::m_id == (int) ObjectID) {
			EditObject::cancelEdit();
		}
		if (pGame && pGame->GetCamera() && pGame->GetCamera()->m_pAttachedTo == pObject) {
			pGame->GetCamera()->Attach(nullptr);
		}
		if (pObject->m_entity) {
			m_mapGtaPtrToID.erase(pObject->m_entity);
		}
		delete pObject;
		m_pObjects[ObjectID] = nullptr;
	}
	m_bObjectSlotState[ObjectID] = false;
	m_iObjectCount--;

	return true;
}

OBJECTID CObjectPool::FindIDFromGtaPtr(sa::CEntity* pGtaObject)
{
	auto it = m_mapGtaPtrToID.find(pGtaObject);
	if (it != m_mapGtaPtrToID.end()) {
		OBJECTID id = it->second;
		// engine pool slots get recycled — the same GTA object pool serves
		// IPL placement objects, so a stale wrapper's m_entity can alias a
		// recycled entity. Re-validate the mapping and drop it when the
		// slot's owner moved on.
		if (id < MAX_OBJECTS && m_pObjects[id] && m_pObjects[id]->m_entity == pGtaObject) {
			return id;
		}
		m_mapGtaPtrToID.erase(it);
	}

	return INVALID_OBJECT_ID;
}

/**
 * @param pGtaObject
 * @return
 */
CObject* CObjectPool::FindObjectFromGtaPtr(sa::CEntity* pGtaObject)
{
	OBJECTID ObjectID = FindIDFromGtaPtr(pGtaObject);
	return GetAt(ObjectID);
}

void CObjectPool::Process()
{
	uint32_t dwThisTick = GetTickCount();
	if (m_dwLastTick == 0) {
		m_dwLastTick = dwThisTick;
	}

	float fElapsedTime = (dwThisTick - m_dwLastTick) / 1000.0f;
	if (!std::isfinite(fElapsedTime) || fElapsedTime < 0.0f) {
		fElapsedTime = 0.0f;
	}
	else if (fElapsedTime > 0.25f) {
		fElapsedTime = 0.25f;
	}

	for (OBJECTID i = 0; i < MAX_OBJECTS; i++) {
		if (m_bObjectSlotState[i] && m_pObjects[i]) {
			m_pObjects[i]->Process(fElapsedTime);
		}
	}

	// OOB vehicles need injected sector records to collide — this used to
	// hang off a single object's Process, so maps without OOB objects
	// never maintained OOB vehicle collision at all
	ProcessOobVehicleSectors();

	m_dwLastTick = dwThisTick;
}

void CObjectPool::ProcessMaterialText()
{
	for (OBJECTID ObjectID = 0; ObjectID < MAX_OBJECTS; ObjectID++) {
		if (m_pObjects[ObjectID] && m_bObjectSlotState[ObjectID]) {
			m_pObjects[ObjectID]->ProcessMaterialText();
		}
	}
}
