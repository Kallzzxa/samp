#include "Animation.h"
#include "../Client.h"
#include "Game.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace {
constexpr int kAnimBlockCount = static_cast<int>(sizeof(g_szAnimBlockNames) / sizeof(g_szAnimBlockNames[0]));
} // namespace

const char* GetAnimByIdx(int idx)
{
	if (idx < 0 || idx >= kAnimBlockCount) {
		return nullptr;
	}
	return &g_szAnimBlockNames[idx][0];
}

int GetAnimIdxByName(const char* szName)
{
	if (!szName) return -1;
	for (int i = 0; i < kAnimBlockCount; i++) {
		if (!strcasecmp(szName, g_szAnimBlockNames[i])) {
			return i;
		}
	}
	return -1;
}

namespace {
std::unordered_map<uint32_t, uint16_t> g_animHashToIdx;
bool g_animHashInited = false;

uint16_t g_queuedAnimIdx = 0;
float g_queuedAnimBlend = 4.0f;

void EnsureAnimHashTable()
{
	if (g_animHashInited) {
		return;
	}
	g_animHashInited = true;
	g_animHashToIdx.reserve(kAnimBlockCount);
	for (int i = 0; i < kAnimBlockCount; ++i) {
		const char* sep = std::strchr(g_szAnimBlockNames[i], ':');
		if (!sep || !sep[1]) continue;
		const uint32_t key = eng::fn::CKeyGen_GetUppercaseKey(sep + 1);
		if (key != 0) {
			g_animHashToIdx.emplace(key, static_cast<uint16_t>(i + 1));
		}
	}
}
} // namespace

uint16_t GetAnimIdxByHashKey(uint32_t hashKey)
{
	if (hashKey == 0) return 0;
	EnsureAnimHashTable();
	auto it = g_animHashToIdx.find(hashKey);
	return (it != g_animHashToIdx.end()) ? it->second : 0;
}

void RecordLocalPedQueuedAnim(int groupId, int animId, float blendDelta)
{
	// Match PC hooks.cpp: ignore idle-chat micro anim (group 0, id 0xA0)
	if (groupId == 0 && animId == 0xA0) {
		return;
	}
	if (groupId < 0 || animId < 0) {
		return;
	}
	// Resolve eagerly while AddAnimation/BlendAnimation has guaranteed the group is loaded;
	// avoids dereferencing an unloaded CAnimBlendAssocGroup later in PackLocalPedQueuedAnim().
	auto* assoc = static_cast<sa::CAnimBlendStaticAssociation*>(
		eng::fn::CAnimManager_GetAnimAssociation(groupId, animId));
	if (!assoc || !assoc->m_pHierarchy) {
		return;
	}
	const uint16_t idx = GetAnimIdxByHashKey(assoc->m_pHierarchy->m_hashKey);
	if (idx == 0) {
		return;
	}
	g_queuedAnimIdx = idx;
	g_queuedAnimBlend = std::isfinite(blendDelta) ? blendDelta : 4.0f;
}

void ResetLocalPedQueuedAnim()
{
	g_queuedAnimIdx = 0;
	g_queuedAnimBlend = 4.0f;
}

uint32_t PackLocalPedQueuedAnim()
{
	static constexpr uint32_t kDefaultIdlePacked = static_cast<uint32_t>(1189u) | (4u << 16) | (0x80u << 24);
	if (g_queuedAnimIdx == 0) {
		return kDefaultIdlePacked;
	}
	const int clampedSpeed = std::isfinite(g_queuedAnimBlend)
		? std::clamp(static_cast<int>(g_queuedAnimBlend), 0, 255)
		: 4;
	return static_cast<uint32_t>(g_queuedAnimIdx)
		| (static_cast<uint32_t>(clampedSpeed) << 16)
		| 0x80000000u;
}
