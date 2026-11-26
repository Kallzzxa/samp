#pragma once

#include "Engine/Symbols.h"
#include "Core/PtrNode.h"
#include <SA.h>
#include <cstdint>
#include <vector>

namespace sa_world {

// Shared canonical helper for linking a CPhysical entity into GTA:SA's 16x16
// CWorld::ms_aRepeatSectors hash grid with:
//   - 256-bit bucket deduplication (max 1 entry per hash group per entity)
//   - null-checked CEntryInfoNode / CPtrNodeDoubleLink pool allocations
//   - optional tracking of newly allocated CEntryInfoNode records & global budget cap
//
// Used by both WorldHooks::CPhysical_Add (in-bounds large custom maps) and
// OobCollision::InjectIntoRepeatSectors (out-of-bounds |x|>3000 entities).
struct RepeatSectorLinkResult {
	int added = 0;
	bool poolExhausted = false;
	bool budgetReached = false;
};

inline RepeatSectorLinkResult linkEntityToRepeatSectors(
	sa::CPhysical* entity,
	int minX, int minY, int maxX, int maxY,
	std::vector<CEntryInfoNode*>* outAllocatedInfos = nullptr,
	int* ioBudgetCounter = nullptr,
	int budgetLimit = 0)
{
	RepeatSectorLinkResult result{};
	if (!entity || minX > maxX || minY > maxY) {
		return result;
	}

	constexpr size_t kPtrSize = sizeof(void*);
	constexpr size_t kRepeatSectorSize = 3 * kPtrSize;
	size_t listOff = 0;
	switch (entity->m_nType) {
		case sa::ENTITY_TYPE_VEHICLE: listOff = 0; break;
		case sa::ENTITY_TYPE_PED:     listOff = kPtrSize; break;
		case sa::ENTITY_TYPE_OBJECT:  listOff = 2 * kPtrSize; break;
		default: return result;
	}

	const uintptr_t pSectors = eng::addr(eng::Sym::CWorld_ms_aRepeatSectors);
	if (!pSectors) {
		return result;
	}

	uint32_t seenMask[8] = {};
	for (int cy = minY; cy <= maxY; ++cy) {
		for (int cx = minX; cx <= maxX; ++cx) {
			const int group = ((cy & 0xF) << 4) | (cx & 0xF);
			const uint32_t bit = 1u << (group & 31);
			if (seenMask[group >> 5] & bit) {
				continue;
			}
			seenMask[group >> 5] |= bit;

			if (ioBudgetCounter && budgetLimit > 0 && *ioBudgetCounter >= budgetLimit) {
				result.budgetReached = true;
				return result;
			}

			auto* pInfo = eng::fn::CEntryInfoNode_OpNew<CEntryInfoNode*>(sizeof(CEntryInfoNode));
			auto* pNode = eng::fn::CPtrNodeDoubleLink_OpNew<sa::CPtrNodeDoubleLink*>(sizeof(sa::CPtrNodeDoubleLink));
			if (!pInfo || !pNode) {
				if (pNode) eng::fn::CPtrNodeDoubleLink_OpDelete(pNode);
				if (pInfo) eng::fn::CEntryInfoNode_OpDelete(pInfo);
				result.poolExhausted = true;
				return result;
			}

			auto** ppHead = reinterpret_cast<sa::CPtrNodeDoubleLink**>(
				pSectors + static_cast<size_t>(group) * kRepeatSectorSize + listOff);

			pNode->m_item = entity;
			pNode->m_next = *ppHead;
			pNode->m_prev = nullptr;
			if (*ppHead) (*ppHead)->m_prev = pNode;
			*ppHead = pNode;

			pInfo->m_pDoubleLinkList = reinterpret_cast<CPtrListDoubleLink*>(ppHead);
			pInfo->m_pDoubleLink = reinterpret_cast<CPtrNodeDoubleLink*>(pNode);
			pInfo->m_pRepeatSector = reinterpret_cast<CRepeatSector*>(
				pSectors + static_cast<size_t>(group) * kRepeatSectorSize);
			pInfo->m_pPrevious = nullptr;
			pInfo->m_pNext = entity->m_pCollisionList.m_pNode;
			if (pInfo->m_pNext) pInfo->m_pNext->m_pPrevious = pInfo;
			entity->m_pCollisionList.m_pNode = pInfo;

			++result.added;
			if (ioBudgetCounter) {
				++(*ioBudgetCounter);
			}
			if (outAllocatedInfos) {
				outAllocatedInfos->push_back(pInfo);
			}
		}
	}
	return result;
}

} // namespace sa_world
