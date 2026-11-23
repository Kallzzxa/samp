#include "Game.h"
#include "PlayerContextScope.h"
#include "../UI/UI.h"
#include "../Net/NetGame.h"
#include "../Client.h"
#include "Enums/WidgetIDs.h"
#include <atomic>

extern UI* pUI;
extern Game* pGame;
extern NetGame* pNetGame;

extern CPlayerPed* g_pCurrentFiredPed;
extern BULLET_DATA* g_pCurrentBulletData;

PAD_KEYS LocalPlayerKeys;
PAD_KEYS RemotePlayerKeys[PLAYER_PED_SLOTS];

sa::CPed* dwCurPlayerActor = nullptr;
uint8_t byteInternalPlayer = 0;
uint8_t byteCurPlayer = 0;

void* ghook_CWorld_ProcessLineOfSight = nullptr;

static CPlayerPed* GetBlockableLocalPed();
static CPlayerPed* GetAimableLocalPed();
static bool Using1stPersonWeaponCam();

// Unified state for touch-screen combat controls (aim toggle, fire edge/hold,
// melee block). Sampled once per game-logic frame right after
// CTouchInterface::Clear() so CPad hooks read consistent, zero-lag state.
struct TouchCombatState {
	bool aimLatch = false;
	bool aimSuppress = false;
	bool fireDown = false;
	bool firePrevDown = false;
	bool fireJustPressed = false;
	uint32_t fireDownTickMs = 0;
	bool blockDown = false;
};

static TouchCombatState g_touchCombat{};

// Tap vs hold threshold for regular firearms: a normal touch tap lasts
// ~40-150ms (3-9 frames), which would otherwise fire 2-3 rounds on automatic
// weapons. Press edge fires immediately; sustained hold repeats after 180ms.
static constexpr uint32_t kFireHoldRepeatDelayMs = 180;

// CWidget field offsets verified on arm32 and arm64 libGTASA.so
static constexpr uintptr_t kWidgetSpriteOffset  = VER_x32 ? 8  : 16;
static constexpr uintptr_t kWidgetPosOffset     = VER_x32 ? 12 : 24;
static constexpr uintptr_t kWidgetAlphaOffset   = VER_x32 ? 76 : 88;
static constexpr uintptr_t kWidgetEnabledOffset = VER_x32 ? 77 : 89;

// Temporarily scales CWidgetRegionMove's WidgetPosition.sx (which controls outer
// ring radius, inner thumb radius, and max drag distance) while preserving the
// full left-screen touch activation rectangles (m_ScreenRect / m_TouchRect).
struct ScopedJoystickScale {
	uintptr_t w = 0;
	float origSx = 0.0f;

	ScopedJoystickScale(uintptr_t widget, bool ensureBaseRectInit = false)
	{
		float scale = Settings::joystickScale();
		if (!widget || scale == 1.0f || scale < 0.4f || scale > 2.0f) return;
		if (ensureBaseRectInit) {
			eng::fn::CWidget_Update( widget);
		}
		w = widget;
		origSx = *reinterpret_cast<float*>(w + kWidgetPosOffset + 8);
		*reinterpret_cast<float*>(w + kWidgetPosOffset + 8) = origSx * scale;
	}

	~ScopedJoystickScale()
	{
		if (w) {
			*reinterpret_cast<float*>(w + kWidgetPosOffset + 8) = origSx;
			eng::fn::CWidget_Update( w);
		}
	}
};

DECL_HOOK(void, CWidgetRegionMove_Update, uintptr_t _this)
{
	ScopedJoystickScale guard(_this, true);
	CWidgetRegionMove_Update(_this);
}

DECL_HOOK(void, CWidgetRegionMove_ClampInternalTouchAnchor, uintptr_t _this)
{
	ScopedJoystickScale guard(_this, false);
	CWidgetRegionMove_ClampInternalTouchAnchor(_this);
}

DECL_HOOK(void, CWidgetRegionMove_Draw, uintptr_t _this)
{
	ScopedJoystickScale guard(_this, false);
	CWidgetRegionMove_Draw(_this);
}

DECL_HOOK(uint32_t, CWidgetRegionMove_IsTouched, uintptr_t _this, uintptr_t pVec2D)
{
	ScopedJoystickScale guard(_this, false);
	return CWidgetRegionMove_IsTouched(_this, pVec2D);
}

// CTaskSimplePlayerOnFoot::m_nTimer offset (2000ms idle fighter timeout)
static constexpr uintptr_t kTaskPlayerOnFootTimerOffset = VER_x32 ? 40 : 56;

extern std::atomic<bool> g_suppressWidgetSetEnabled;
static std::atomic<bool> g_suppressPedUpdatePosition{false};
static std::atomic<bool> g_suppressPedRotFromCam{true};
static bool g_bEnableLineOfSightHook = false;

DECL_HOOK(void, CPed_UpdatePosition_fromProcessControl, sa::CPed* _this)
{
	if (g_suppressPedUpdatePosition.load(std::memory_order_acquire)) {
		return;
	}
	CPed_UpdatePosition_fromProcessControl(_this);
}

DECL_HOOK(void, CTaskSimplePlayerOnFoot_PlayerControlZelda, void* _this, sa::CPed* pPed, bool bUnk)
{
	if (!pPed || !g_suppressPedRotFromCam.load(std::memory_order_acquire)) {
		CTaskSimplePlayerOnFoot_PlayerControlZelda(_this, pPed, bUnk);
		return;
	}
	const float savedAimRot = pPed->m_fAimingRotation;
	CTaskSimplePlayerOnFoot_PlayerControlZelda(_this, pPed, bUnk);
	pPed->m_fAimingRotation = savedAimRot;
}

DECL_HOOK(void, CPed_ProcessControl, sa::CPed* _this)
{
	sa::CPed* prevCurActor = dwCurPlayerActor;
	uint8_t prevCurPlayer = byteCurPlayer;

	dwCurPlayerActor = _this;
	byteInternalPlayer = *pbyteCurrentPlayer;
	uint8_t curPlayer = FindPlayerNumFromPedPtr(_this);

	if (dwCurPlayerActor && (curPlayer != 0) && byteInternalPlayer == 0) {
		byteCurPlayer = curPlayer;
		{
			ScopedPlayerContext ctx(curPlayer, ScopedPlayerContext::kFull);

			// Suppress CPed::UpdatePosition (via hookBL) and CWidget::SetEnabled (via hook)
			// for remote peds without rewriting .text every frame.
			g_suppressPedUpdatePosition.store(true, std::memory_order_release);
			g_suppressWidgetSetEnabled.store(true, std::memory_order_release);

			// call orig
			CPed_ProcessControl(_this);

			// restore
			g_suppressWidgetSetEnabled.store(false, std::memory_order_release);
			g_suppressPedUpdatePosition.store(false, std::memory_order_release);
		}
	}
	else {
		// Allow local player / actors to update m_fAimingRotation from camera
		// in CTaskSimplePlayerOnFoot::PlayerControlZelda without rewriting .text.
		g_suppressPedRotFromCam.store(false, std::memory_order_release);

		// call orig
		CPed_ProcessControl(_this);

		// Re-enable suppression for non-local peds
		g_suppressPedRotFromCam.store(true, std::memory_order_release);
	}

	dwCurPlayerActor = prevCurActor;
	byteCurPlayer = prevCurPlayer;
}

DECL_HOOK(uint16_t, CPad_GetPedWalkLeftRight, uintptr_t _this)
{
	if (*pbyteCurrentPlayer) {
		uint16_t dwResult = RemotePlayerKeys[byteCurPlayer].wKeyLR;
		if ((dwResult == 0xFF80 || dwResult == 0x80) && RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_WALK]) {
			dwResult = 0x40;
		}
		return dwResult;
	}
	else {
		LocalPlayerKeys.wKeyLR = CPad_GetPedWalkLeftRight(_this);
		return LocalPlayerKeys.wKeyLR;
	}
}
DECL_HOOK(uint16_t, CPad_GetPedWalkUpDown, uintptr_t _this)
{
	if (*pbyteCurrentPlayer) {
		uint16_t dwResult = RemotePlayerKeys[byteCurPlayer].wKeyUD;
		if ((dwResult == 0xFF80 || dwResult == 0x80) && RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_WALK]) {
			dwResult = 0x40;
		}
		return dwResult;
	}
	else {
		LocalPlayerKeys.wKeyUD = CPad_GetPedWalkUpDown(_this);
		return LocalPlayerKeys.wKeyUD;
	}
}

DECL_HOOK(uint16_t, CPad_JumpJustDown, uintptr_t _this)
{
	if (*pbyteCurrentPlayer) {
		if (!RemotePlayerKeys[byteCurPlayer].bIgnoreJump && RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_JUMP] && !RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_HANDBRAKE]) {
			RemotePlayerKeys[byteCurPlayer].bIgnoreJump = true;
			return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_JUMP];
		}

		return 0;
	}
	else {
		LocalPlayerKeys.bKeys[ePadKeys::KEY_JUMP] = CPad_JumpJustDown(_this);
		return LocalPlayerKeys.bKeys[ePadKeys::KEY_JUMP];
	}
}
DECL_HOOK(uint16_t, CPad_GetJump, uintptr_t _this)
{
	if (*pbyteCurrentPlayer) {
		if (RemotePlayerKeys[byteCurPlayer].bIgnoreJump) return 0;
		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_JUMP];
	}
	else {
		LocalPlayerKeys.bKeys[ePadKeys::KEY_JUMP] = CPad_GetJump(_this);
		return LocalPlayerKeys.bKeys[ePadKeys::KEY_JUMP];
	}
}

DECL_HOOK(uint16_t, CPad_GetDuck, uintptr_t _this)
{
	if (*pbyteCurrentPlayer) {
		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_CROUCH];
	}
	else {
		LocalPlayerKeys.bKeys[ePadKeys::KEY_CROUCH] = CPad_GetDuck(_this);
		return LocalPlayerKeys.bKeys[ePadKeys::KEY_CROUCH];
	}
}

DECL_HOOK(uint16_t, CPad_MeleeAttackJustDown, uintptr_t _this)
{
	if (*pbyteCurrentPlayer) {
		if (RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_HANDBRAKE] && RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_SECONDARY_ATTACK]) {
			return 2;
		}

		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_FIRE];
	}
	else {
		uint32_t dwResult = CPad_MeleeAttackJustDown(_this);
		/*if (dwResult == 2)
		{
			RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_HANDBRAKE] = true;
			RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_SECONDARY_ATTACK] = true;
			return dwResult;
		}*/
		LocalPlayerKeys.bKeys[ePadKeys::KEY_FIRE] = dwResult;
		return dwResult;
	}
}

DECL_HOOK(uint16_t, CPad_DuckJustDown, uintptr_t _this, int unk)
{
	if (*pbyteCurrentPlayer) {
		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_CROUCH];
	}
	else {
		LocalPlayerKeys.bKeys[ePadKeys::KEY_CROUCH] = CPad_DuckJustDown(_this, unk);
		return LocalPlayerKeys.bKeys[ePadKeys::KEY_CROUCH];
	}
}

DECL_HOOK(uint32_t, CPad_GetSprint, uintptr_t _this, uint32_t unk)
{
	if (*pbyteCurrentPlayer) {
		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_SPRINT];
	}
	else {
		LocalPlayerKeys.bKeys[ePadKeys::KEY_SPRINT] = CPad_GetSprint(_this, unk);
		return LocalPlayerKeys.bKeys[ePadKeys::KEY_SPRINT];
	}
}

DECL_HOOK(uint32_t, CPad_GetAutoClimb, uintptr_t _this)
{
	if (*pbyteCurrentPlayer) {
		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_JUMP];
	}
	else {
		LocalPlayerKeys.bKeys[ePadKeys::KEY_JUMP] = CPad_GetAutoClimb(_this);
		return LocalPlayerKeys.bKeys[ePadKeys::KEY_JUMP];
	}
}

DECL_HOOK(uint32_t, CPad_GetAbortClimb, uintptr_t _this)
{
	if (*pbyteCurrentPlayer) {
		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_SECONDARY_ATTACK];
	}
	else {
		LocalPlayerKeys.bKeys[ePadKeys::KEY_SECONDARY_ATTACK] = CPad_GetAbortClimb(_this);
		return LocalPlayerKeys.bKeys[ePadKeys::KEY_SECONDARY_ATTACK];
	}
}

DECL_HOOK(uint32_t, CPad_DiveJustDown, uintptr_t _this)
{
	if (*pbyteCurrentPlayer) {
		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_FIRE];
	}
	else {
		LocalPlayerKeys.bKeys[ePadKeys::KEY_FIRE] = CPad_DiveJustDown(_this);
		return LocalPlayerKeys.bKeys[ePadKeys::KEY_FIRE];
	}
}

DECL_HOOK(uint32_t, CPad_SwimJumpJustDown, uintptr_t _this)
{
	if (*pbyteCurrentPlayer) {
		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_JUMP];
	}
	else {
		LocalPlayerKeys.bKeys[ePadKeys::KEY_JUMP] = CPad_SwimJumpJustDown(_this);
		return LocalPlayerKeys.bKeys[ePadKeys::KEY_JUMP];
	}
}

// CTaskSimplePlayerOnFoot::PlayerControlFighter accumulates m_nTimer when
// ped->m_pTargetedObject == nullptr and CHID::GetInputType() != 2, forcing
// QUIT_FIGHTING (cmd 15) at >1999ms (~2s). While the local player actively
// holds the touch block button, reset that idle fighter timeout so blocking
// never drops mid-hold.
DECL_HOOK(void, CTaskSimplePlayerOnFoot_PlayerControlFighter, uintptr_t _this, sa::CPed* pPed)
{
	if (!*pbyteCurrentPlayer && _this && pPed && pPed == GamePool_FindPlayerPed()
		&& g_touchCombat.blockDown) {
		*reinterpret_cast<uint32_t*>(_this + kTaskPlayerOnFootTimerOffset) = 0;
	}
	CTaskSimplePlayerOnFoot_PlayerControlFighter(_this, pPed);
}

DECL_HOOK(uint32_t, CPad_GetBlock, uintptr_t _this)
{
	if (*pbyteCurrentPlayer) {
		return (RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_JUMP] && RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_HANDBRAKE]);
	}
	if (!_this || *reinterpret_cast<uint16_t*>(_this + 272) != 0) {
		return 0;
	}
	if (GetBlockableLocalPed()) {
		return g_touchCombat.blockDown ? 1u : 0u;
	}
	return 0;
}

DECL_HOOK(int16_t, CPad_GetSteeringLeftRight, uintptr_t _this)
{
	if (*pbyteCurrentPlayer) {
		return (int16_t) RemotePlayerKeys[byteCurPlayer].wKeyLR;
	}
	else {
		LocalPlayerKeys.wKeyLR = CPad_GetSteeringLeftRight(_this);
		return LocalPlayerKeys.wKeyLR;
	}
}

DECL_HOOK(uint16_t, CPad_GetSteeringUpDown, uintptr_t _this)
{
	if (*pbyteCurrentPlayer) {
		return RemotePlayerKeys[byteCurPlayer].wKeyUD;
	}
	else {
		LocalPlayerKeys.wKeyUD = CPad_GetSteeringUpDown(_this);
		return LocalPlayerKeys.wKeyUD;
	}
}

DECL_HOOK(uint16_t, CPad_GetAccelerate, uintptr_t _this)
{
	if (*pbyteCurrentPlayer) {
		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_SPRINT] ? 0xFF : 0x00;
	}
	else {
		sa::CPed* local = GamePool_FindPlayerPed();
		sa::CVehicle* pVehicle = local ? local->pVehicle : nullptr;
		if (pVehicle) {
			bool engineOn = pVehicle->m_nVehicleFlags.bEngineOn;
			if (pVehicle->pDriver == local && !engineOn) return 0;
		}
		uint16_t wResult = CPad_GetAccelerate(_this);
		LocalPlayerKeys.bKeys[ePadKeys::KEY_SPRINT] = wResult;
		return wResult;
	}
}

DECL_HOOK(uint16_t, CPad_GetBrake, uintptr_t _this)
{
	if (*pbyteCurrentPlayer) {
		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_JUMP] ? 0xFF : 0x00;
	}
	else {
		sa::CPed* local = GamePool_FindPlayerPed();
		sa::CVehicle* pVehicle = local ? local->pVehicle : nullptr;
		if (pVehicle) {
			bool engineOn = pVehicle->m_nVehicleFlags.bEngineOn;
			if (pVehicle->pDriver == local && !engineOn) return 0;
		}

		uint16_t wResult = CPad_GetBrake(_this);
		LocalPlayerKeys.bKeys[ePadKeys::KEY_JUMP] = wResult;
		return wResult;
	}
}

DECL_HOOK(uint32_t, CPad_GetHandBrake, uintptr_t _this)
{
	if (*pbyteCurrentPlayer) {
		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_HANDBRAKE] ? 0xFF : 0x00;
	}
	else {
		sa::CPed* local = GamePool_FindPlayerPed();
		sa::CVehicle* pVehicle = local ? local->pVehicle : nullptr;
		if (pVehicle) {
			bool engineOn = pVehicle->m_nVehicleFlags.bEngineOn;
			if (pVehicle->pDriver == local && !engineOn) return 0;
		}

		uint32_t dwResult = CPad_GetHandBrake(_this);
		LocalPlayerKeys.bKeys[ePadKeys::KEY_HANDBRAKE] = dwResult;
		return dwResult;
	}
}

DECL_HOOK(uint32_t, CPad_GetHorn, uintptr_t _this)
{
	if (*pbyteCurrentPlayer) {
		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_CROUCH];
	}
	else {
		uint32_t dwResult = CPad_GetHorn(_this);
		LocalPlayerKeys.bKeys[ePadKeys::KEY_CROUCH] = dwResult;
		return dwResult;
	}
}

DECL_HOOK(uint32_t, CPad_ExitVehicleJustDown, uintptr_t _this, int a2, uintptr_t vehicle, int a4, uintptr_t vec)
{
	static uint32_t dwPassengerEnterExit = GetTickCount();
	if (GetTickCount() - dwPassengerEnterExit < 1000) {
		return 0;
	}

	if (pNetGame) {
		CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
		if (pPlayerPool) {
			CLocalPlayer* pLocalPlayer = pPlayerPool->GetLocalPlayer();
			if (pLocalPlayer) {
				if (pLocalPlayer->HandlePassengerEntry()) {
					dwPassengerEnterExit = GetTickCount();
					return 0;
				}
			}
		}
	}

	uint32_t dwRes = CPad_ExitVehicleJustDown(_this, a2, vehicle, a4, vec);
	// diag: exit-vehicle taps stopped working — log the touch on widget 0
	// vs the orig result and the pad-disable bytes that gate it
	{
		uintptr_t* widgets = GetTouchWidgets();
		uint32_t t0 = (widgets && widgets[0])
			? eng::fn::CTouchInterface_IsTouched(
				0, 0, 0) : 0;
		uint32_t d = (*reinterpret_cast<uint16_t*>(_this + 272) ? 8u : 0u)
			| (*reinterpret_cast<uint8_t*>(_this + 301) ? 4u : 0u)
			| (*reinterpret_cast<uint8_t*>(_this + 280) ? 2u : 0u)
			| (*reinterpret_cast<uint8_t*>(_this + 291) ? 1u : 0u);
		int pair = (dwRes ? 1 : 0) | ((int)t0 << 1) | ((int)d << 8);
		static int s_lastExitPair = -1;
		if (pair != s_lastExitPair && (t0 || dwRes || d)) {
			s_lastExitPair = pair;
			spdlog::info("EXIT-VEH justdown={} touch0={} padDis=0x{:x}",
				dwRes, t0, d);
		}
	}
	return dwRes;
}

DECL_HOOK(int, CPad_GetTurretLeft, uintptr_t _this)
{
	if (*pbyteCurrentPlayer) {
		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_LOOK_LEFT];
	}
	else {
		LocalPlayerKeys.bKeys[ePadKeys::KEY_LOOK_LEFT] = CPad_GetTurretLeft(_this);
		return LocalPlayerKeys.bKeys[ePadKeys::KEY_LOOK_LEFT];
	}
}

DECL_HOOK(int, CPad_GetTurretRight, uintptr_t _this)
{
	if (*pbyteCurrentPlayer) {
		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_LOOK_RIGHT];
	}
	else {
		LocalPlayerKeys.bKeys[ePadKeys::KEY_LOOK_RIGHT] = CPad_GetTurretRight(_this);
		return LocalPlayerKeys.bKeys[ePadKeys::KEY_LOOK_RIGHT];
	}
}

DECL_HOOK(int, CPad_GetNitroFired, uintptr_t _this)
{
	if (*pbyteCurrentPlayer) {
		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_FIRE];
	}
	else {
		LocalPlayerKeys.bKeys[ePadKeys::KEY_FIRE] = CPad_GetNitroFired(_this);
		return LocalPlayerKeys.bKeys[ePadKeys::KEY_FIRE];
	}
}

DECL_HOOK(int, CPad_GetHydraulicJump, uintptr_t _this)
{
	if (*pbyteCurrentPlayer) {
		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_CROUCH];
	}
	else {
		LocalPlayerKeys.bKeys[ePadKeys::KEY_CROUCH] = CPad_GetHydraulicJump(_this);
		return LocalPlayerKeys.bKeys[ePadKeys::KEY_CROUCH];
	}
}

static inline void ApplyVehicleTyreBurstFix(sa::CVehicle* pVehicle)
{
	if (pVehicle->pDriver) {
		if (pVehicle->m_nVehicleFlags.bTyresDontBurst) {
			pVehicle->m_nVehicleFlags.bTyresDontBurst = 0;
		}
		if (!pVehicle->m_nVehicleFlags.bCanBeDamaged) {
			pVehicle->m_nVehicleFlags.bCanBeDamaged = true;
		}
	}
	else {
		if (!pVehicle->m_nVehicleFlags.bTyresDontBurst) {
			pVehicle->m_nVehicleFlags.bTyresDontBurst = 1;
		}
		if (pVehicle->m_nVehicleFlags.bCanBeDamaged) {
			pVehicle->m_nVehicleFlags.bCanBeDamaged = false;
		}
	}
}

template <bool CallAudioService, typename OrigFn>
static void ProcessRemoteVehicleControl(OrigFn orig, sa::CVehicle* pVehicle)
{
	byteInternalPlayer = *pbyteCurrentPlayer;

	if (pVehicle && pVehicle->pDriver && pVehicle->pDriver->m_nPedType == 0
		&& pVehicle->pDriver != GamePool_FindPlayerPed() && byteInternalPlayer == 0) {
		byteCurPlayer = FindPlayerNumFromPedPtr(pVehicle->pDriver);

		ScopedPlayerContext camAimCtx(byteCurPlayer, ScopedPlayerContext::kVehicleCamAim);

		// Tyre burst fix
		ApplyVehicleTyreBurstFix(pVehicle);

		pVehicle->pDriver->m_nPedType = sa::PEDTYPE_CIVMALE; // So CPed::IsPlayer returns FALSE
		const uint8_t savedControlFlags = pVehicle->nControlFlags;
		pVehicle->nControlFlags = 26;

		if constexpr (CallAudioService) {
			eng::fn::CAEVehicleAudioEntity_Service(&pVehicle->m_VehicleAudioEntity);
		}

		pVehicle->nControlFlags = savedControlFlags;
		pVehicle->pDriver->m_nPedType = sa::PEDTYPE_PLAYER1;

		{
			ScopedPlayerContext focusCtx(byteCurPlayer, ScopedPlayerContext::kFocusOnly);
			orig(pVehicle);
		}
	}
	else {
		if (pVehicle && pVehicle->pDriver && pVehicle->pDriver->m_nPedType == 0
			&& GamePool_FindPlayerPed() == pVehicle->pDriver) {
			ApplyVehicleTyreBurstFix(pVehicle);
		}

		if constexpr (CallAudioService) {
			eng::fn::CAEVehicleAudioEntity_Service(&pVehicle->m_VehicleAudioEntity);
		}

		orig(pVehicle);
	}
}

DECL_HOOK(void, CAutomobile_ProcessControl, sa::CVehicle* _this)   { ProcessRemoteVehicleControl<true>(CAutomobile_ProcessControl, _this); }
DECL_HOOK(void, CBoat_ProcessControl, sa::CVehicle* _this)         { ProcessRemoteVehicleControl<true>(CBoat_ProcessControl, _this); }
DECL_HOOK(void, CBike_ProcessControl, sa::CVehicle* _this)         { ProcessRemoteVehicleControl<true>(CBike_ProcessControl, _this); }
DECL_HOOK(void, CHeli_ProcessControl, sa::CVehicle* _this)         { ProcessRemoteVehicleControl<false>(CHeli_ProcessControl, _this); }
DECL_HOOK(void, CPlane_ProcessControl, sa::CVehicle* _this)        { ProcessRemoteVehicleControl<false>(CPlane_ProcessControl, _this); }
DECL_HOOK(void, CBmx_ProcessControl, sa::CVehicle* _this)          { ProcessRemoteVehicleControl<false>(CBmx_ProcessControl, _this); }
DECL_HOOK(void, CMonsterTruck_ProcessControl, sa::CVehicle* _this) { ProcessRemoteVehicleControl<false>(CMonsterTruck_ProcessControl, _this); }
DECL_HOOK(void, CQuadBike_ProcessControl, sa::CVehicle* _this)     { ProcessRemoteVehicleControl<false>(CQuadBike_ProcessControl, _this); }
DECL_HOOK(void, CTrain_ProcessControl, sa::CVehicle* _this)        { ProcessRemoteVehicleControl<true>(CTrain_ProcessControl, _this); }
DECL_HOOK(void, CTrailer_ProcessControl, sa::CVehicle* _this)      { ProcessRemoteVehicleControl<false>(CTrailer_ProcessControl, _this); }

bool NotifyEnterVehicle(sa::CVehicle* _pVehicle)
{
	spdlog::info("NotifyEnterVehicle: 0x{:X}", reinterpret_cast<uintptr_t>(_pVehicle));

	if (!pNetGame || !_pVehicle) {
		return false;
	}

	VEHICLEID vehicleId = INVALID_VEHICLE_ID;

	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
	if (!pVehiclePool) {
		return false;
	}

	vehicleId = pVehiclePool->FindIDFromGtaPtr(_pVehicle);
	if (vehicleId == INVALID_VEHICLE_ID || !pVehiclePool->GetSlotState(vehicleId)) {
		return false;
	}

	CVehicle* pVehicle = pVehiclePool->GetAt(vehicleId);
	if (pVehicle && pVehicle->m_pVehicle) {
		if (pVehicle->GetDoorState()) {
			return false;
		}

		if (pVehicle->m_pVehicle->m_nModelIndex == TRAIN_PASSENGER) {
			return false;
		}

		if (pVehicle->m_pVehicle->pDriver && pVehicle->m_pVehicle->pDriver->m_nPedType != 0) {
			return false;
		}
	}

	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	CLocalPlayer* pLocalPlayer = pPlayerPool ? pPlayerPool->GetLocalPlayer() : nullptr;
	if (pLocalPlayer) {
		if (pLocalPlayer->GetPlayerPed() && pLocalPlayer->GetPlayerPed()->GetCurrentWeapon() == WEAPON_PARACHUTE) {
			pLocalPlayer->GetPlayerPed()->SetArmedWeapon(0);
		}

		pLocalPlayer->SendEnterVehicleNotification(vehicleId, false);
		return true;
	}

	return false;
}

DECL_HOOK(void, CTaskComplexEnterCarAsDriver, uintptr_t** _this, sa::CVehicle* pVehicle)
{
	NotifyEnterVehicle(pVehicle);
	CTaskComplexEnterCarAsDriver(_this, pVehicle);
}

DECL_HOOK(void, CTaskComplexLeaveCar, uintptr_t** _this, sa::CVehicle* pVehicle, int iTargetDoor, int iDelayTime, bool bSensibleLeaveCar, bool bForceGetOut)
{
	if (pNetGame && pVehicle) {
		sa::CPed* pLocalPed = GamePool_FindPlayerPed();
		if (pLocalPed && pLocalPed->pVehicle == pVehicle) {
			CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
			CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
			if (pVehiclePool && pPlayerPool) {
				VEHICLEID VehicleID = pVehiclePool->FindIDFromGtaPtr(pLocalPed->pVehicle);
				if (VehicleID != INVALID_VEHICLE_ID) {
					CVehicle* pVeh = pVehiclePool->GetAt(VehicleID);
					CLocalPlayer* pLocalPlayer = pPlayerPool->GetLocalPlayer();
					if (pVeh && pVeh->m_pVehicle && pLocalPlayer) {
						if (pVeh->IsATrainPart()) {
							RwMatrix mat = pVeh->m_pVehicle->GetMatrix().ToRwMatrix();
							if (pLocalPlayer->GetPlayerPed()) {
								pLocalPlayer->GetPlayerPed()->RemoveFromVehicleAndPutAt(mat.pos.x + 2.5f, mat.pos.y + 2.5f, mat.pos.z);
							}
						}
						else {
							pLocalPlayer->SendExitVehicleNotification(VehicleID);
						}
					}
				}
			}
		}
	}

	CTaskComplexLeaveCar(_this, pVehicle, iTargetDoor, iDelayTime, bSensibleLeaveCar, bForceGetOut);
}

DECL_HOOK(uint32_t, CTaskSimpleUseGun_SetPedPosition, uintptr_t _this, sa::CPed* ped)
{
	dwCurPlayerActor = ped;
	byteInternalPlayer = *pbyteCurrentPlayer;
	byteCurPlayer = FindPlayerNumFromPedPtr(ped);

	if (dwCurPlayerActor && (byteCurPlayer != 0) && byteInternalPlayer == 0) // remote player
	{
		ScopedPlayerContext ctx(byteCurPlayer, ScopedPlayerContext::kFull);
		CTaskSimpleUseGun_SetPedPosition(_this, ped);
	}
	else // localplayer
	{
		CTaskSimpleUseGun_SetPedPosition(_this, ped);
	}

	return 0;
}

// Replicates CPad::GetWeapon's top-level safety gates: pad controls disabled
// (+272), script fire disable (+303), ped alive/on-foot, and not entering a
// vehicle (tasks 700/701).
static bool CanLocalPedUseOnFootFire(uintptr_t pad, sa::CPed* pPed)
{
	if (!pad || !pPed || !pPed->m_pPlayerData) return false;
	if (*reinterpret_cast<uint16_t*>(pad + 272) != 0) return false;
	if (*reinterpret_cast<uint8_t*>(pad + 303) != 0) return false;

	CPlayerPed* pLocal = pGame ? pGame->FindPlayerPed() : nullptr;
	if (!pLocal || pLocal->m_ped != pPed || pLocal->IsDead()
		|| pLocal->IsInVehicle() || pLocal->IsInJetpack()) {
		return false;
	}

	if (pPed->m_pIntelligence) {
		void* pTaskMgr = &pPed->m_pIntelligence->m_TaskMgr;
		if (eng::fn::CTaskManager_FindActiveTaskByType( pTaskMgr, 700)
			|| eng::fn::CTaskManager_FindActiveTaskByType( pTaskMgr, 701)) {
			return false;
		}
	}
	return true;
}

// Weapons that natively require continuous hold (throwables & continuous spray)
static bool WeaponUsesHoldFire(uint8_t type)
{
	switch (type) {
		case sa::eWeaponType::WEAPONTYPE_GRENADE:
		case sa::eWeaponType::WEAPONTYPE_TEARGAS:
		case sa::eWeaponType::WEAPONTYPE_MOLOTOV:
		case sa::eWeaponType::WEAPONTYPE_FLAMETHROWER:
		case sa::eWeaponType::WEAPONTYPE_SPRAYCAN:
		case sa::eWeaponType::WEAPONTYPE_EXTINGUISHER:
			return true;
	}
	return false;
}

// Single-shot / scoped / trigger-only weapons: fire strictly on press edge
static bool WeaponUsesEdgeOnlyFire(uint8_t type)
{
	switch (type) {
		case sa::eWeaponType::WEAPONTYPE_SNIPERRIFLE:
		case sa::eWeaponType::WEAPONTYPE_RLAUNCHER:
		case sa::eWeaponType::WEAPONTYPE_RLAUNCHER_HS:
		case sa::eWeaponType::WEAPONTYPE_DETONATOR:
		case sa::eWeaponType::WEAPONTYPE_CAMERA:
		case sa::eWeaponType::WEAPONTYPE_NIGHTVISION:
		case sa::eWeaponType::WEAPONTYPE_INFRARED:
			return true;
	}
	return false;
}

static bool EvaluateLocalTouchFireLevel(uint8_t weaponType)
{
	if (!g_touchCombat.fireDown) {
		return false;
	}
	if (WeaponUsesHoldFire(weaponType)) {
		return true;
	}
	if (WeaponUsesEdgeOnlyFire(weaponType)) {
		return g_touchCombat.fireJustPressed;
	}
	// Regular firearms: immediate shot on press edge; sustained hold enters
	// continuous fire only after kFireHoldRepeatDelayMs so a tap never bursts.
	if (g_touchCombat.fireJustPressed) {
		return true;
	}
	return (GetTickCount() - g_touchCombat.fireDownTickMs) >= kFireHoldRepeatDelayMs;
}

DECL_HOOK(uint32_t, CPad_GetWeapon, uintptr_t _this, sa::CPed* pPed, bool bShoulderPossible)
{
	if (*pbyteCurrentPlayer) {
		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_FIRE] ? 0xFF : 0x00;
	}

	CPlayerPed* pLocal = pGame ? pGame->FindPlayerPed() : nullptr;
	if (!pLocal || pLocal->IsInVehicle() || pLocal->IsInJetpack()) {
		uint32_t dwResult = CPad_GetWeapon(_this, pPed, bShoulderPossible);
		LocalPlayerKeys.bKeys[ePadKeys::KEY_FIRE] = dwResult;
		return dwResult;
	}

	uint32_t dwResult = 0;
	if (CanLocalPedUseOnFootFire(_this, pPed)) {
		dwResult = EvaluateLocalTouchFireLevel(pLocal->GetCurrentWeapon()) ? 0xFF : 0x00;
	}
	LocalPlayerKeys.bKeys[ePadKeys::KEY_FIRE] = dwResult;
	return dwResult;
}

// Mobile libGTASA bug fix: CPlayerInfo::Process() probes whether the player
// can enter a nearby vehicle EVERY frame by calling
// activeTask->MakeAbortable(ped, ABORT_PRIORITY_URGENT=1, nullptr) BEFORE
// checking ExitVehicleJustDown(). For CTaskSimplePlayerOnFoot, MakeAbortable
// unconditionally aborts secondary task 0 (CTaskSimpleUseGun), which:
//   1) kills hipfire every frame near vehicles (stuck in raise-gun frame 1);
//   2) destroys & recreates CTaskSimpleUseGun between aim-tap shots, skipping
//      the weapon's post-shot cooldown animation (~160ms instead of ~720ms).
// Scope-guard CPlayerInfo::Process and suppress its speculative probe abort
// unless the player actually pressed/released the enter-car widget.
static bool s_bInPlayerInfoProcess = false;

DECL_HOOK(void, CPlayerInfo_Process, uintptr_t _this, int playerNumber)
{
	bool prev = s_bInPlayerInfoProcess;
	if (playerNumber == 0) {
		s_bInPlayerInfoProcess = true;
	}
	CPlayerInfo_Process(_this, playerNumber);
	s_bInPlayerInfoProcess = prev;
}

DECL_HOOK(uint32_t, CTaskSimplePlayerOnFoot_MakeAbortable, uintptr_t _this, sa::CPed* pPed, int priority, uintptr_t pEvent)
{
	if (s_bInPlayerInfoProcess && !*pbyteCurrentPlayer && priority == 1 && pEvent == 0
		&& pPed && pPed == GamePool_FindPlayerPed() && pPed->m_pIntelligence) {
		uintptr_t useGun = eng::fn::CPedIntelligence_GetTaskUseGun( pPed->m_pIntelligence);
		if (useGun) {
			bool bEnterCarInput = false;
			uintptr_t* widgets = GetTouchWidgets();
			if (widgets && widgets[sa::WidgetIDs::WIDGET_ENTER_CAR]) {
				bEnterCarInput =
					eng::fn::CTouchInterface_IsTouched(
						sa::WidgetIDs::WIDGET_ENTER_CAR, 0, 0) != 0
					|| eng::fn::CTouchInterface_IsReleased(
						sa::WidgetIDs::WIDGET_ENTER_CAR, 0, 0) != 0;
			}
			if (!bEnterCarInput) {
				bool bAiming = (pPed->m_pPlayerData && pPed->m_pPlayerData->m_bFreeAiming)
					|| Using1stPersonWeaponCam();
				// Aiming: report non-abortable (no enter-car prompt);
				// Hipfire: report abortable so enter-car prompt stays visible,
				// without prematurely killing CTaskSimpleUseGun.
				return bAiming ? 0u : 1u;
			}
		}
	}
	return CTaskSimplePlayerOnFoot_MakeAbortable(_this, pPed, priority, pEvent);
}

DECL_HOOK(uint32_t, CPad_WeaponJustDown, uintptr_t _this, sa::CPed* pPed, bool bCheckControl, bool bCheckVehicle)
{
	if (*pbyteCurrentPlayer) {
		return CPad_WeaponJustDown(_this, pPed, bCheckControl, bCheckVehicle);
	}

	CPlayerPed* pLocal = pGame ? pGame->FindPlayerPed() : nullptr;
	if (!pLocal || pLocal->IsInVehicle() || pLocal->IsInJetpack()) {
		return CPad_WeaponJustDown(_this, pPed, bCheckControl, bCheckVehicle);
	}

	if (!CanLocalPedUseOnFootFire(_this, pPed)) {
		return 0;
	}
	return g_touchCombat.fireJustPressed ? 1u : 0u;
}

DECL_HOOK(bool, CPad_CycleWeaponLeftJustDown, uintptr_t _this)
{
	if (*pbyteCurrentPlayer) {
		return false;
	}
	else {
		bool isPressed = CPad_CycleWeaponLeftJustDown(_this);
		if (isPressed) {
			spdlog::info("CycleWeaponLeftJustDown isPressed");
		}
		return isPressed;
	}
}

DECL_HOOK(uint32_t, CPad_CycleWeaponRightJustDown, uintptr_t _this)
{
	if (*pbyteCurrentPlayer) {
		return 0;
	}
	else {
		return CPad_CycleWeaponRightJustDown(_this);
	}
}

// the engine only polls the targeting buttons (widgets 0x13/0x14) for
// scoped/special weapons {33..36,43,44,45} while in lock-on aim mode —
// regular guns get no aim button unless the aim-mode setting is free-aim,
// and its free-aim set additionally drops flamethrower/minigun/spraycan
// which SAMP lets you aim. Show the button for everything aimable:
// any weapon <=45 except the knife/katana melee specials.
static bool WeaponShowsAimButton(uint8_t type)
{
	if (type == sa::eWeaponType::WEAPONTYPE_UNARMED
		|| type > sa::eWeaponType::WEAPONTYPE_INFRARED) {
		return false;
	}
	return type != sa::eWeaponType::WEAPONTYPE_KNIFE
		&& type != sa::eWeaponType::WEAPONTYPE_KATANA;
}

static CPlayerPed* GetAimableLocalPed()
{
	CPlayerPed* pPed = pGame ? pGame->FindPlayerPed() : nullptr;
	if (!pPed || pPed->IsDead() || pPed->IsInVehicle() || pPed->IsInJetpack()
		|| !WeaponShowsAimButton(pPed->GetCurrentWeapon())) {
		return nullptr;
	}
	return pPed;
}

// blocking is melee-only: fists and melee weapons (unarmed..cane).
static CPlayerPed* GetBlockableLocalPed()
{
	CPlayerPed* pPed = pGame ? pGame->FindPlayerPed() : nullptr;
	if (!pPed || pPed->IsDead() || pPed->IsInVehicle() || pPed->IsInJetpack()
		|| pPed->GetCurrentWeapon() > sa::eWeaponType::WEAPONTYPE_CANE) {
		return nullptr;
	}
	return pPed;
}

// scoped weapons aim through a camera weapon-mode + PED_SNIPER_MODE, not the
// free-aim flag: ProcessPlayerWeapon's enter jump-table maps weapon -> mode
// (34->7, 35->8, 36->51, 43->46). For the projectiles (35/36) the native
// enter-poll is unreachable entirely (ProcessPlayerWeapon diverts before it),
// so TickTouchCombatWidgets drives this path directly.
static int ScopedAimCamMode(uint8_t type)
{
	switch (type) {
		case sa::eWeaponType::WEAPONTYPE_SNIPERRIFLE: return 7;
		case sa::eWeaponType::WEAPONTYPE_RLAUNCHER: return 8;
		case sa::eWeaponType::WEAPONTYPE_RLAUNCHER_HS: return 51;
		case sa::eWeaponType::WEAPONTYPE_CAMERA: return 46;
	}
	return 0;
}

static bool Using1stPersonWeaponCam()
{
	return eng::fn::CCamera_Using1stPersonWeaponMode(
		&sa::getTheCamera());
}

// engine-side aim state: CPlayerPedData::m_bFreeAiming for lock-on/free-aim
// weapons, or a first-person weapon camera for scoped ones.
static bool EngineIsAiming(CPlayerPed* pPed)
{
	if (!pPed || !pPed->m_ped) return false;
	if (pPed->m_ped->m_pPlayerData && pPed->m_ped->m_pPlayerData->m_bFreeAiming) {
		return true;
	}
	return Using1stPersonWeaponCam();
}

static uint32_t PollTouchRelease(int32_t widgetID)
{
	return eng::fn::CTouchInterface_IsReleased(
		widgetID, 0, 1);
}

extern void FixTouchWidgetLayout();

// Custom aim-toggle button lives in slot 0x98 (WIDGET_CAR_CHEATS — unused in SAMP).
// Engine widgets 19/20 stay disabled so their icons never cover the attack slot.
static constexpr int32_t kAimToggleWidget = sa::WidgetIDs::WIDGET_CAR_CHEATS;
static constexpr int32_t kBlockWidget     = sa::WidgetIDs::WIDGET_MISSION_SKIP;

static bool PollTouchDown(int32_t widgetID)
{
	uintptr_t* widgets = GetTouchWidgets();
	if (!widgets || !widgets[widgetID]) return false;
	return eng::fn::CTouchInterface_IsTouched(
		widgetID, 0, 1) != 0;
}

// Render-phase helper (called from Render2dStuff): keeps layout and button
// visibility/sprites in sync without mutating game-logic input state.
void UpdateTouchCombatVisuals()
{
	uintptr_t* widgets = GetTouchWidgets();
	if (!widgets) return;

	FixTouchWidgetLayout();

	CPlayerPed* pLocal = pGame ? pGame->FindPlayerPed() : nullptr;
	bool bOnFoot = pLocal && !pLocal->IsDead() && !pLocal->IsInVehicle() && !pLocal->IsInJetpack();

	if (bOnFoot && widgets[sa::WidgetIDs::WIDGET_ATTACK]) {
		uintptr_t wAtk = widgets[sa::WidgetIDs::WIDGET_ATTACK];
		*reinterpret_cast<uint8_t*>(wAtk + kWidgetEnabledOffset) = 1;
		*reinterpret_cast<uint8_t*>(wAtk + kWidgetAlphaOffset) = 0xff;
	}

	// On-foot camera toggle (widget 0x12): GTA:SA Mobile only polls
	// CycleCameraModeDownJustDown while in a vehicle, leaving the top-right
	// camera button hidden on foot. Keep it visible on foot (outside 1st-person
	// weapon scope) without touching it in vehicles where the engine owns it.
	if (bOnFoot && widgets[sa::WidgetIDs::WIDGET_CAM_TOGGLE]) {
		bool bShowCamToggle = !Using1stPersonWeaponCam();
		uintptr_t wCam = widgets[sa::WidgetIDs::WIDGET_CAM_TOGGLE];
		*reinterpret_cast<uint8_t*>(wCam + kWidgetEnabledOffset) = bShowCamToggle ? 1 : 0;
		*reinterpret_cast<uint8_t*>(wCam + kWidgetAlphaOffset) = bShowCamToggle ? 0xff : 0;
	}

	if (uintptr_t wAim = widgets[kAimToggleWidget]) {
		bool bAimable = GetAimableLocalPed() != nullptr;
		*reinterpret_cast<uint8_t*>(wAim + kWidgetEnabledOffset) = bAimable ? 1 : 0;
		*reinterpret_cast<uint8_t*>(wAim + kWidgetAlphaOffset) = bAimable ? 0xff : 0;
		uintptr_t wSrc = widgets[sa::WidgetIDs::WIDGET_ENTER_TARGETING];
		if (wSrc && *reinterpret_cast<uintptr_t*>(wSrc + kWidgetSpriteOffset)) {
			*reinterpret_cast<uintptr_t*>(wAim + kWidgetSpriteOffset) =
				*reinterpret_cast<uintptr_t*>(wSrc + kWidgetSpriteOffset);
		}
	}

	if (uintptr_t wBlk = widgets[kBlockWidget]) {
		bool bBlockable = GetBlockableLocalPed() != nullptr;
		*reinterpret_cast<uint8_t*>(wBlk + kWidgetEnabledOffset) = bBlockable ? 1 : 0;
		*reinterpret_cast<uint8_t*>(wBlk + kWidgetAlphaOffset) = bBlockable ? 0xff : 0;
		uintptr_t wSrc = widgets[sa::WidgetIDs::WIDGET_ATTACK];
		if (wSrc && *reinterpret_cast<uintptr_t*>(wSrc + kWidgetSpriteOffset)
			&& !*reinterpret_cast<uintptr_t*>(wBlk + kWidgetSpriteOffset)) {
			*reinterpret_cast<uintptr_t*>(wBlk + kWidgetSpriteOffset) =
				*reinterpret_cast<uintptr_t*>(wSrc + kWidgetSpriteOffset);
		}
	}
}

// Logic-phase input sampler (called right after CTouchInterface::Clear() inside
// CGame::Process(), before CWorld::Process / CPlayerPed::ProcessControl).
// Samples fire/block/aim once per logic frame so every CPad hook sees coherent,
// zero-lag state.
static void TickTouchCombatInput()
{
	uintptr_t* widgets = GetTouchWidgets();
	if (!widgets) return;

	CPlayerPed* pLocal = pGame ? pGame->FindPlayerPed() : nullptr;
	bool bOnFoot = pLocal && !pLocal->IsDead() && !pLocal->IsInVehicle() && !pLocal->IsInJetpack();

	// 1) Fire button edge + hold tracking
	bool bFireNow = bOnFoot && PollTouchDown(sa::WidgetIDs::WIDGET_ATTACK);
	g_touchCombat.fireJustPressed = bFireNow && !g_touchCombat.firePrevDown;
	if (g_touchCombat.fireJustPressed) {
		g_touchCombat.fireDownTickMs = GetTickCount();
	}
	g_touchCombat.firePrevDown = bFireNow;
	g_touchCombat.fireDown = bFireNow;

	// 2) Block button (pure held state, independent of aim latch)
	g_touchCombat.blockDown = (GetBlockableLocalPed() != nullptr) && PollTouchDown(kBlockWidget);

	// 2.5) On-foot camera distance toggle (close=1, normal=2, far=3)
	// Matches CCamera::CamControl's m_nPedZoom range {1, 2, 3}.
	if (bOnFoot && !Using1stPersonWeaponCam()
		&& widgets[sa::WidgetIDs::WIDGET_CAM_TOGGLE]
		&& PollTouchRelease(sa::WidgetIDs::WIDGET_CAM_TOGGLE) != 0) {
		auto& cam = sa::getTheCamera();
		int32_t cur = cam.m_nPedZoom;
		cam.m_nPedZoom = (cur >= 1 && cur < 3) ? (cur + 1) : 1;
	}

	// 3) Aim-toggle state machine (aimable weapons only)
	CPlayerPed* pAimPed = GetAimableLocalPed();
	if (!pAimPed) {
		g_touchCombat.aimLatch = false;
		g_touchCombat.aimSuppress = false;
		return;
	}

	bool bTapped = widgets[kAimToggleWidget] && PollTouchRelease(kAimToggleWidget) != 0;
	bool bAiming = EngineIsAiming(pAimPed);
	if (bTapped) {
		g_touchCombat.aimLatch = !g_touchCombat.aimLatch;
		g_touchCombat.aimSuppress = !g_touchCombat.aimLatch;
	}
	if (!bAiming) {
		g_touchCombat.aimSuppress = false;
	}
	else if (!g_touchCombat.aimLatch && !g_touchCombat.aimSuppress) {
		g_touchCombat.aimLatch = true;
	}

	int scopedMode = ScopedAimCamMode(pAimPed->GetCurrentWeapon());
	if (scopedMode) {
		if (g_touchCombat.aimLatch && !Using1stPersonWeaponCam()) {
			eng::fn::CCamera_SetNewPlayerWeaponMode(
				&sa::getTheCamera(), scopedMode, 0, 0);
			eng::fn::CPed_SetPedState(
				pAimPed->m_ped, sa::ePedState::PED_SNIPER_MODE);
		}
		else if (!g_touchCombat.aimLatch && Using1stPersonWeaponCam()) {
			eng::fn::CCamera_ClearPlayerWeaponMode(
				&sa::getTheCamera());
			if (pAimPed->m_ped->m_pPlayerData) {
				pAimPed->m_ped->m_pPlayerData->m_bFreeAiming = false;
			}
			eng::fn::CPlayerPed_ClearWeaponTarget( pAimPed->m_ped);
		}
	}
}

DECL_HOOK(void, CTouchInterface_Clear)
{
	CTouchInterface_Clear();
	TickTouchCombatInput();
}

DECL_HOOK(uint32_t, CPad_GetEnterTargeting, uintptr_t _this)
{
	if (*pbyteCurrentPlayer) {
		return 0;
	}
	if (!_this || *reinterpret_cast<uint16_t*>(_this + 272) != 0) {
		return 0;
	}
	CPlayerPed* pPed = GetAimableLocalPed();
	return (pPed && g_touchCombat.aimLatch) ? 1u : 0u;
}

DECL_HOOK(uint32_t, CPad_GetExitTargeting, uintptr_t _this)
{
	if (*pbyteCurrentPlayer) {
		return 0;
	}
	CPlayerPed* pPed = GetAimableLocalPed();
	if (pPed && !g_touchCombat.aimLatch && EngineIsAiming(pPed)) {
		eng::fn::CPlayerPed_ClearWeaponTarget( pPed->m_ped);
		if (pPed->m_ped->m_pPlayerData) {
			pPed->m_ped->m_pPlayerData->m_bFreeAiming = false;
		}
		return 1;
	}
	return 0;
}

DECL_HOOK(uint32_t, CCamera_IsTargetingActive, uintptr_t _this, sa::CPed* pPed)
{
	//if (*pbyteCurrentPlayer)
	if (pPed != GamePool_FindPlayerPed()) {
		return RemotePlayerKeys[byteCurPlayer].bKeys[ePadKeys::KEY_HANDBRAKE] ? 1 : 0;
	}
	else {
		/* CCamera::IsTargetingActive */
		sa::CCamera* TheCamera = &sa::getTheCamera();
		bool bIsTargeting = eng::fn::CCamera_IsTargetingActive( TheCamera);
		LocalPlayerKeys.bKeys[ePadKeys::KEY_HANDBRAKE] = bIsTargeting;
		return bIsTargeting;
	}
}

RwMatrix* RwMatrixOrthoNormalize(RwMatrix* matrixOut, const RwMatrix* matrixIn)
{
	return eng::fn::RwMatrixOrthoNormalize<RwMatrix*>( matrixOut, matrixIn);
}

void SendBulletSync(sa::CVector* vecOrigin, sa::CVector* vecPos, sa::CEntity** ppEntity)
{
	static BULLET_DATA bulletData;
	memset(&bulletData, 0, sizeof(BULLET_DATA));

	bulletData.vecOrigin.x = vecOrigin->x;
	bulletData.vecOrigin.y = vecOrigin->y;
	bulletData.vecOrigin.z = vecOrigin->z;
	bulletData.vecPos.x = vecPos->x;
	bulletData.vecPos.y = vecPos->y;
	bulletData.vecPos.z = vecPos->z;

	if (ppEntity) {
		static sa::CEntity* pEntity;
		pEntity = *ppEntity;
		if (pEntity) {
			if (pEntity->m_matrix) {
				const int lagComp = pNetGame ? pNetGame->session().lagCompensationMode : 0;
				if (lagComp) {
					bulletData.vecOffset.x = vecPos->x - pEntity->m_matrix->m_pos.x;
					bulletData.vecOffset.y = vecPos->y - pEntity->m_matrix->m_pos.y;
					bulletData.vecOffset.z = vecPos->z - pEntity->m_matrix->m_pos.z;
				}
				else {
					static RwMatrix mat1;
					memset(&mat1, 0, sizeof(mat1));

					static RwMatrix mat2;
					memset(&mat2, 0, sizeof(mat2));

					RwMatrix entityMat = pEntity->m_matrix->ToRwMatrix();
					RwMatrixOrthoNormalize(&mat2, &entityMat);
					RwMatrixInvert(&mat1, &mat2);
					ProjectMatrix(&bulletData.vecOffset, reinterpret_cast<sa::CMatrix*>(&mat1), vecPos);
				}
			}

			bulletData.pEntity = pEntity;
		}
	}

	pGame->FindPlayerPed()->ProcessBulletData(&bulletData);
}

struct ScopedLineOfSightHook {
	bool prev;
	ScopedLineOfSightHook() : prev(g_bEnableLineOfSightHook) { g_bEnableLineOfSightHook = true; }
	~ScopedLineOfSightHook() { g_bEnableLineOfSightHook = prev; }
};

DECL_HOOK(uint32_t, CWeapon_FireInstantHit, sa::CWeapon* _this, sa::CPed* pFiringEntity, sa::CVector* vecOrigin, sa::CVector* muzzlePosn, sa::CEntity* targetEntity, sa::CVector* target, sa::CVector* originForDriveBy, bool arg6, bool muzzle)
{
	ScopedLineOfSightHook losGuard;

	if (pFiringEntity != GamePool_FindPlayerPed()) {
		return muzzle;
	}

	if (pNetGame) {
		CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
		if (pPlayerPool) {
			pPlayerPool->ApplyCollisionChecking();
		}
	}

	if (pGame) {
		CPlayerPed* pPlayerPed = pGame->FindPlayerPed();
		if (pPlayerPed) {
			pPlayerPed->FireInstant();
		}
	}

	if (pNetGame) {
		CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
		if (pPlayerPool) {
			pPlayerPool->ResetCollisionChecking();
		}
	}

	return muzzle;
}

DECL_HOOK(uint32_t, CWeapon_FireInstantHit_full, sa::CWeapon* _this, sa::CPed* pFiringEntity, sa::CVector* vecOrigin, sa::CVector* muzzlePosn, sa::CEntity* targetEntity, sa::CVector* target, sa::CVector* originForDriveBy, bool arg6, bool muzzle)
{
	ScopedLineOfSightHook losGuard;
	return CWeapon_FireInstantHit_full(_this, pFiringEntity, vecOrigin, muzzlePosn, targetEntity, target, originForDriveBy, arg6, muzzle);
}

DECL_HOOK(uint32_t, CWeapon_FireSniper, sa::CWeapon* _this, sa::CPed* pFiringEntity, sa::CEntity* victim, sa::CVector* target)
{
	if (pFiringEntity == GamePool_FindPlayerPed()) {
		if (pGame) {
			CPlayerPed* pPlayerPed = pGame->FindPlayerPed();
			if (pPlayerPed) {
				pPlayerPed->FireInstant();
			}
		}
	}

	return true;
}

DECL_HOOK(uint32_t, CWorld_ProcessLineOfSight, sa::CVector* vecOrigin, sa::CVector* vecEnd, sa::CColPoint* colPoint, sa::CEntity** ppEntity, bool b1, bool b2, bool b3, bool b4, bool b5, bool b6, bool b7, bool b8)
{
	if (!g_bEnableLineOfSightHook) {
		return CWorld_ProcessLineOfSight(vecOrigin, vecEnd, colPoint, ppEntity, b1, b2, b3, b4, b5, b6, b7, b8);
	}

	static sa::CVector vecPosPlusOffset;
	CPlayerPed* pLocalPlayerPed = pGame ? pGame->FindPlayerPed() : nullptr;

	const int lagComp = pNetGame ? pNetGame->session().lagCompensationMode : 0;

	if (lagComp != 2 && vecOrigin && vecEnd) {
		if (g_pCurrentFiredPed && g_pCurrentFiredPed != pLocalPlayerPed) {
			if (g_pCurrentBulletData && g_pCurrentBulletData->pEntity) {
				if (!IsEntityPlaceable(g_pCurrentBulletData->pEntity) && g_pCurrentBulletData->pEntity->m_matrix) {
					if (lagComp) {
						vecPosPlusOffset.x = g_pCurrentBulletData->pEntity->m_matrix->m_pos.x + g_pCurrentBulletData->vecOffset.x;
						vecPosPlusOffset.y = g_pCurrentBulletData->pEntity->m_matrix->m_pos.y + g_pCurrentBulletData->vecOffset.y;
						vecPosPlusOffset.z = g_pCurrentBulletData->pEntity->m_matrix->m_pos.z + g_pCurrentBulletData->vecOffset.z;
					}
					else {
						if (g_pCurrentBulletData->vecOffset != nullptr) {
							if (g_pCurrentBulletData->pEntity->m_matrix->m_up != nullptr &&
									g_pCurrentBulletData->pEntity->m_matrix->m_forward != nullptr &&
									g_pCurrentBulletData->pEntity->m_matrix->m_right != nullptr &&
									g_pCurrentBulletData->pEntity->m_matrix->m_pos != nullptr) {
								ProjectMatrix(&vecPosPlusOffset, g_pCurrentBulletData->pEntity->m_matrix, &g_pCurrentBulletData->vecOffset);
							}
						}
					}

					vecEnd->x = vecPosPlusOffset.x - vecOrigin->x + vecPosPlusOffset.x;
					vecEnd->y = vecPosPlusOffset.y - vecOrigin->y + vecPosPlusOffset.y;
					vecEnd->z = vecPosPlusOffset.z - vecOrigin->z + vecPosPlusOffset.z;
				}
			}
		}
	}

	uint32_t result = CWorld_ProcessLineOfSight(vecOrigin, vecEnd, colPoint, ppEntity, b1, b2, b3, b4, b5, b6, b7, b8);

	if (lagComp == 2) {
		if (g_pCurrentFiredPed && g_pCurrentFiredPed == pLocalPlayerPed && vecOrigin && colPoint) {
			SendBulletSync(vecOrigin, &colPoint->m_vecPoint, ppEntity);
		}
		return result;
	}

	if (g_pCurrentFiredPed) {
		if (g_pCurrentFiredPed != pLocalPlayerPed) {
			if (g_pCurrentBulletData && g_pCurrentBulletData->pEntity == nullptr && ppEntity) {
				sa::CPed* pLocalPed = GamePool_FindPlayerPed();
				if (pLocalPed && (*ppEntity == pLocalPed || (pLocalPed->IsInVehicle() && *ppEntity == pLocalPed->pVehicle))) {
					result = 0;
					*ppEntity = nullptr;
					if (colPoint) {
						colPoint->m_vecPoint.x = 0.0f;
						colPoint->m_vecPoint.y = 0.0f;
						colPoint->m_vecPoint.z = 0.0f;
					}
					return result;
				}
			}
		}
		else if (vecOrigin && colPoint) {
			SendBulletSync(vecOrigin, &colPoint->m_vecPoint, ppEntity);
		}
	}

	return result;
}

DECL_HOOK(sa::CEntity *, CBulletInfo_Update, uintptr_t _this)
{
	ScopedLineOfSightHook losGuard;
	return CBulletInfo_Update(_this);
}

#pragma pack(push, 1)
struct CPedDamageResponseCalculator {
	sa::CPed* m_pInflictor;
	float m_fRawDamage;
	int32_t m_eHitZone;
	int32_t m_eWeaponUsed;
	bool m_bJumpedOutOfMovingCar;
	uint8_t pad[3];
};
#pragma pack(pop)
VALIDATE_SIZE(CPedDamageResponseCalculator, (VER_x32 ? 0x14 : 0x18));

PLAYERID FindPlayerIDFromGtaPtr(sa::CEntity* pEntity)
{
	if (pEntity == nullptr || !pNetGame) return INVALID_PLAYER_ID;

	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
	if (!pPlayerPool) return INVALID_PLAYER_ID;

	PLAYERID PlayerID = pPlayerPool->FindRemotePlayerIDFromGtaPtr((sa::CPed*) pEntity);
	if (PlayerID != INVALID_PLAYER_ID) return PlayerID;

	if (pVehiclePool) {
		VEHICLEID VehicleID = pVehiclePool->FindIDFromGtaPtr((sa::CVehicle*) pEntity);
		if (VehicleID != INVALID_VEHICLE_ID) {
			for (PLAYERID i = 0; i < MAX_PLAYERS; i++) {
				CRemotePlayer* pRemotePlayer = pPlayerPool->GetAt(i);
				if (pRemotePlayer && pRemotePlayer->CurrentVehicleID() == VehicleID) {
					return i;
				}
			}
		}
	}

	return INVALID_PLAYER_ID;
}

PLAYERID FindActorIDFromGtaPtr(sa::CPed* pPed)
{
	if (pPed && pNetGame && pNetGame->GetActorPool()) {
		return pNetGame->GetActorPool()->FindIDFromGtaPtr(pPed);
	}

	return INVALID_PLAYER_ID;
}

bool ComputeDamageResponse(CPedDamageResponseCalculator* calculator, sa::CPed* pPed)
{
	sa::CPed* pGamePed = GamePool_FindPlayerPed();
	bool isLocalPed = false;

	if (!pNetGame || !pGamePed) {
		return false;
	}

	sa::CPed* pDamager = calculator->m_pInflictor;
	if (pDamager != pGamePed && (pPed && *(uintptr_t*) pPed == g_saSym->Abs<uintptr_t>(addr::vptr$CCivilianPed))) { /* CCivilianPed */
		return true;
	}

	if (pPed == pGamePed) {
		isLocalPed = true;
	}
	else if (pDamager != pGamePed) {
		return false;
	}

	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	CLocalPlayer* pLocalPlayer = pPlayerPool ? pPlayerPool->GetLocalPlayer() : nullptr;
	if (!pLocalPlayer) {
		return false;
	}
	PLAYERID PlayerID = INVALID_PLAYER_ID;

	if (isLocalPed) {
		if (pLocalPlayer->m_surfSync.bIsActive && calculator->m_eWeaponUsed == WEAPON_COLLISION
				&& pLocalPlayer->m_surfSync.dwSurfVehID >= 0) {
			if (pLocalPlayer->m_surfSync.bIsVehicle) {
				CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
				CVehicle* pSurfingVehicle = (pVehiclePool && pLocalPlayer->m_surfSync.dwSurfVehID < MAX_VEHICLES)
						? pVehiclePool->GetAt((VEHICLEID) pLocalPlayer->m_surfSync.dwSurfVehID) : nullptr;
				if (pSurfingVehicle && pSurfingVehicle->m_entity == (sa::CEntity*) pDamager) {
					return true;
				}
			}
			else {
				CObjectPool* pObjectPool = pNetGame->GetObjectPool();
				CObject* pSurfingObject = (pObjectPool && pLocalPlayer->m_surfSync.dwSurfVehID < MAX_OBJECTS)
						? pObjectPool->GetAt((OBJECTID) pLocalPlayer->m_surfSync.dwSurfVehID) : nullptr;
				if (pSurfingObject && pSurfingObject->m_entity == (sa::CEntity*) pDamager) {
					return true;
				}
			}
		}

		PlayerID = FindPlayerIDFromGtaPtr(pDamager);
		pLocalPlayer->SendTakeDamageEvent(PlayerID, calculator->m_fRawDamage, calculator->m_eWeaponUsed, calculator->m_eHitZone);
	}
	else {
		PlayerID = FindPlayerIDFromGtaPtr(pPed);
		if (PlayerID != INVALID_PLAYER_ID) {
			pLocalPlayer->SendGiveDamageEvent(PlayerID, calculator->m_fRawDamage, calculator->m_eWeaponUsed, calculator->m_eHitZone);
			CRemotePlayer* pRemote = pPlayerPool->GetAt(PlayerID);
			if (pRemote && pRemote->IsNPC()) {
				return true;
			}
		}
		else {
			PLAYERID ActorID = FindActorIDFromGtaPtr(pPed);
			if (ActorID != INVALID_PLAYER_ID) {
				pLocalPlayer->SendGiveDamageActorEvent(ActorID, calculator->m_fRawDamage, calculator->m_eWeaponUsed, calculator->m_eHitZone);
				return true;
			}
		}
	}

	// :check_friendly_fire
	if (!pNetGame->m_netSet || !pNetGame->m_netSet->friendlyFire) {
		return false;
	}

	uint8_t byteTeam = pLocalPlayer->m_byteTeam;
	CRemotePlayer* pTargetRemote = (PlayerID != INVALID_PLAYER_ID) ? pPlayerPool->GetAt(PlayerID) : nullptr;
	if (byteTeam == NO_TEAM || !pTargetRemote || pTargetRemote->m_byteTeam != byteTeam) {
		return false;
	}

	return true;
}

DECL_HOOK(void, CPedDamageResponseCalculator_ComputeDamageResponse, CPedDamageResponseCalculator* _this, sa::CPed* pVictim, uintptr_t damageResponse, const bool bTriggerPainAudio)
{
	if (_this != nullptr && pVictim != nullptr) {
		if (ComputeDamageResponse(_this, pVictim)) {
			return;
		}
	}

	return CPedDamageResponseCalculator_ComputeDamageResponse(_this, pVictim, damageResponse, bTriggerPainAudio);
}

DECL_HOOK(uint32_t, CBulletInfo_AddBullet, sa::CEntity* creator, int weaponType, sa::CVector pos, sa::CVector velocity)
{
	velocity.x *= 50.0f;
	velocity.y *= 50.0f;
	velocity.z *= 50.0f;

	CBulletInfo_AddBullet(creator, weaponType, pos, velocity);

	// CBulletInfo::Update
	eng::fn::CBulletInfo_Update();
	return true;
}

void Hooks::installPadHooks()
{
	spdlog::info("Installing pad hooks...");

	memset(&LocalPlayerKeys, 0, sizeof(PAD_KEYS));

	// playerped & per-frame guard hooks (replaces per-frame .text nop/restoreNop)
	REG_HOOK(Boot, CPlayerPed_ProcessControl, CPed_ProcessControl, "Remote player ped control context switch");
	REG_HOOK_BL(Boot, addr::CPed_UpdatePosition_BranchBL1, "CPed_UpdatePosition.ProcessControl",
		&CPed_UpdatePosition_fromProcessControl_hook, &CPed_UpdatePosition_fromProcessControl,
		"Suppress CPed::UpdatePosition for remote peds");
	REG_HOOK(Boot, CTaskSimplePlayerOnFoot_PlayerControlZelda, CTaskSimplePlayerOnFoot_PlayerControlZelda,
		"Preserve remote ped m_fAimingRotation from camera");

	// vehicles
	REG_HOOK(Boot, CAutomobile_ProcessControl, CAutomobile_ProcessControl, "Remote automobile control context switch");
	REG_HOOK(Boot, CBoat_ProcessControl, CBoat_ProcessControl, "Remote boat control context switch");
	REG_HOOK(Boot, CBike_ProcessControl, CBike_ProcessControl, "Remote bike control context switch");
	REG_HOOK(Boot, CPlane_ProcessControl, CPlane_ProcessControl, "Remote plane control context switch");
	REG_HOOK(Boot, CHeli_ProcessControl, CHeli_ProcessControl, "Remote heli control context switch");
	REG_HOOK(Boot, CBmx_ProcessControl, CBmx_ProcessControl, "Remote bmx control context switch");
	REG_HOOK(Boot, CMonsterTruck_ProcessControl, CMonsterTruck_ProcessControl, "Remote monster truck control context switch");
	REG_HOOK(Boot, CQuadBike_ProcessControl, CQuadBike_ProcessControl, "Remote quad bike control context switch");
	REG_HOOK(Boot, CTrain_ProcessControl, CTrain_ProcessControl, "Remote train control context switch");
	REG_HOOK(Boot, CTrailer_ProcessControl, CTrailer_ProcessControl, "Remote trailer control context switch");

	REG_HOOK(Boot, CPad_GetPedWalkLeftRight, CPad_GetPedWalkLeftRight, "Pad walk left/right sync");
	REG_HOOK(Boot, CPad_GetPedWalkUpDown, CPad_GetPedWalkUpDown, "Pad walk up/down sync");

	REG_HOOK(Boot, CPad_GetSprint, CPad_GetSprint, "Pad sprint sync");

	REG_HOOK(Boot, CPad_GetJump, CPad_GetJump, "Pad jump sync");
	REG_HOOK(Boot, CPad_JumpJustDown, CPad_JumpJustDown, "Pad jump just-down sync");

	REG_HOOK(Boot, CPad_GetDuck, CPad_GetDuck, "Pad duck sync");
	REG_HOOK(Boot, CPad_DuckJustDown, CPad_DuckJustDown, "Pad duck just-down sync");

	REG_HOOK(Boot, CPad_MeleeAttackJustDown, CPad_MeleeAttackJustDown, "Pad melee attack sync");
	REG_HOOK(Boot, CPad_GetBlock, CPad_GetBlock, "Pad block widget override");
	REG_HOOK(Boot, CTaskSimplePlayerOnFoot_PlayerControlFighter, CTaskSimplePlayerOnFoot_PlayerControlFighter, "Touch block timeout guard");
	REG_HOOK(Boot, CTouchInterface_Clear, CTouchInterface_Clear, "Multi-touch pinch-zoom guard");

	REG_HOOK(Boot, CWidgetRegionMove_Update, CWidgetRegionMove_Update, "Virtual joystick scale (Update)");
	REG_HOOK(Boot, CWidgetRegionMove_ClampInternalTouchAnchor, CWidgetRegionMove_ClampInternalTouchAnchor, "Virtual joystick scale (Anchor)");
	REG_HOOK(Boot, CWidgetRegionMove_Draw, CWidgetRegionMove_Draw, "Virtual joystick scale (Draw)");
	REG_HOOK(Boot, CWidgetRegionMove_IsTouched, CWidgetRegionMove_IsTouched, "Virtual joystick scale (IsTouched)");

#if VER_x32
	REG_HOOK_ADDR(Boot, addr::CPad_GetAbortClimb_arm32, CPad_GetAbortClimb, true, "Pad abort climb sync (arm32)");
#else
	REG_HOOK(Boot, CPad_GetAbortClimb, CPad_GetAbortClimb, "Pad abort climb sync");
#endif
	REG_HOOK(Boot, CPad_GetAutoClimb, CPad_GetAutoClimb, "Pad auto climb sync");

	REG_HOOK(Boot, CPad_SwimJumpJustDown, CPad_SwimJumpJustDown, "Pad swim jump sync");
	REG_HOOK(Boot, CPad_DiveJustDown, CPad_DiveJustDown, "Pad dive sync");

	REG_HOOK(Boot, CPad_GetSteeringUpDown, CPad_GetSteeringUpDown, "Pad vehicle steer up/down sync");
	REG_HOOK(Boot, CPad_GetSteeringLeftRight, CPad_GetSteeringLeftRight, "Pad vehicle steer left/right sync");

	REG_HOOK(Boot, CPad_GetHandBrake, CPad_GetHandBrake, "Pad handbrake sync");
	REG_HOOK(Boot, CPad_GetBrake, CPad_GetBrake, "Pad brake sync");
	REG_HOOK(Boot, CPad_GetAccelerate, CPad_GetAccelerate, "Pad accelerate sync");
	REG_HOOK(Boot, CPad_GetHorn, CPad_GetHorn, "Pad horn sync");

	REG_HOOK(Boot, CPad_ExitVehicleJustDown, CPad_ExitVehicleJustDown, "Pad exit vehicle sync");

	REG_HOOK(Boot, CPad_GetTurretRight, CPad_GetTurretRight, "Pad turret right sync");
	REG_HOOK(Boot, CPad_GetTurretLeft, CPad_GetTurretLeft, "Pad turret left sync");

	REG_HOOK(Boot, CPad_GetHydraulicJump, CPad_GetHydraulicJump, "Pad hydraulic jump sync");
	REG_HOOK(Boot, CPad_GetNitroFired, CPad_GetNitroFired, "Pad nitro sync");

	REG_HOOK_BL(Boot, addr::CTaskComplexEnterCarAsDriver_Branch1, "CTaskComplexEnterCarAsDriver.Branch1",
		&CTaskComplexEnterCarAsDriver_hook, &CTaskComplexEnterCarAsDriver, "Notify server on vehicle enter");
	REG_HOOK_BL(Boot, addr::CTaskComplexLeaveCar_Branch1, "CTaskComplexLeaveCar.Branch1",
		&CTaskComplexLeaveCar_hook, &CTaskComplexLeaveCar, "Notify server on vehicle exit (branch 1)");
	REG_HOOK_BL(Boot, addr::CTaskComplexLeaveCar_Branch2, "CTaskComplexLeaveCar.Branch2",
		&CTaskComplexLeaveCar_hook, &CTaskComplexLeaveCar, "Notify server on vehicle exit (branch 2)");

	REG_HOOK(Boot, CTaskSimpleUseGun_SetPedPosition, CTaskSimpleUseGun_SetPedPosition, "Remote player aim/gun context switch");
	REG_HOOK(Boot, CPlayerInfo_Process, CPlayerInfo_Process, "Car-proximity hipfire abort guard");
	REG_HOOK(Boot, CTaskSimplePlayerOnFoot_MakeAbortable, CTaskSimplePlayerOnFoot_MakeAbortable, "Protect UseGun from proximity probe");

	REG_HOOK(Boot, CPad_CycleWeaponRightJustDown, CPad_CycleWeaponRightJustDown, "Pad weapon cycle right");
	REG_HOOK(Boot, CPad_CycleWeaponLeftJustDown, CPad_CycleWeaponLeftJustDown, "Pad weapon cycle left");
	REG_HOOK(Boot, CPad_GetWeapon, CPad_GetWeapon, "Pad weapon fire sync");
	REG_HOOK(Boot, CPad_WeaponJustDown, CPad_WeaponJustDown, "Pad weapon just-down sync");
	REG_HOOK(Boot, CCamera_IsTargetingActivePed, CCamera_IsTargetingActive, "Remote player targeting check");
	REG_HOOK(Boot, CPad_GetEnterTargeting, CPad_GetEnterTargeting, "Touch aim button enter");
	REG_HOOK(Boot, CPad_GetExitTargeting, CPad_GetExitTargeting, "Touch aim button exit");

	REG_HOOK_BL(Boot, addr::CWeapon_FireInstantHit_branch1, "CWeapon_FireInstantHit.Branch1",
		&CWeapon_FireInstantHit_hook, &CWeapon_FireInstantHit, "Bullet sync fire hook (branch 1)");
	REG_HOOK_BL(Boot, addr::CWeapon_FireInstantHit_branch2, "CWeapon_FireInstantHit.Branch2",
		&CWeapon_FireInstantHit_hook, &CWeapon_FireInstantHit, "Bullet sync fire hook (branch 2)");

	REG_HOOK(Boot, CWeapon_FireInstantHit, CWeapon_FireInstantHit_full, "Lag-comp instant hit full hook");
	REG_HOOK(Boot, CWeapon_FireSniper, CWeapon_FireSniper, "Sniper fire sync");

	ghook_CWorld_ProcessLineOfSight = REG_HOOK(Boot, CWorld_ProcessLineOfSight, CWorld_ProcessLineOfSight,
		"Bullet raycast hit capture (gated in hook)");

	REG_HOOK(Boot, CBulletInfo_Update, CBulletInfo_Update, "Projectile bullet update hook");
	REG_HOOK(Boot, CPedDamageResponseCalculator_ComputeDamageResponse, CPedDamageResponseCalculator_ComputeDamageResponse,
		"Give/take damage reporting");
	REG_HOOK(Boot, CBulletInfo_AddBullet, CBulletInfo_AddBullet, "Bullet tracer sync");
}
