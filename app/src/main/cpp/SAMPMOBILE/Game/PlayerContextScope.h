#pragma once

#include <cstdint>
#include <cstring>
#include "Aimstuff.h"

// RAII scope that temporarily switches the engine's global local-player state
// (camera mode, weapon camera mode, zoom/aspect, aim vectors, PlayerInFocus)
// to a remote player slot, and restores the saved state on scope exit.
//
// All saved values (camera modes, FOV/aspect, aim vectors, PlayerInFocus) live
// on the scope instance rather than globals, so nested scopes cannot clobber
// an outer scope's local-player snapshot.
class ScopedPlayerContext {
public:
	enum Flags : uint32_t {
		kNone       = 0u,
		kCamMode    = 1u << 0,
		kZoomAspect = 1u << 1,
		kAim        = 1u << 2,
		kFocus      = 1u << 3,
		kSkills     = 1u << 4, // Reserved for T08 (weapon skill levels)

		// Presets matching the four existing switching forms
		kFull          = kCamMode | kZoomAspect | kAim | kFocus, // CPed_ProcessControl, CTaskSimpleUseGun_SetPedPosition
		kFireInstant   = kCamMode | kZoomAspect | kAim,          // CPlayerPed::FireInstant (no PlayerInFocus switch)
		kVehicle       = kCamMode | kAim | kFocus,               // Combined vehicle context
		kVehicleCamAim = kCamMode | kAim,                        // Vehicle outer scope (focus switches after AudioService)
		kFocusOnly     = kFocus,                                 // PlayerPed focus-only sites & vehicle inner scope
	};

	ScopedPlayerContext(uint8_t playerNum, uint32_t flags, bool active = true)
		: flags_(active ? flags : kNone)
	{
		if (flags_ == kNone) {
			return;
		}

		if (flags_ & kCamMode) {
			const uint8_t remoteMode = GameGetPlayerCameraMode(playerNum);
			if (pbyteCameraMode) {
				savedCameraMode_ = *pbyteCameraMode;
				*pbyteCameraMode = remoteMode;
				savedCamMode1_ = true;
			}
			if (wCameraMode2) {
				savedCameraMode2_ = *wCameraMode2;
				*wCameraMode2 = (remoteMode == 4) ? 0 : remoteMode;
				savedCamMode2_ = true;
			}
		}

		if (flags_ & kZoomAspect) {
			if (pfCameraExtZoom && pfAspectRatio) {
				savedExtZoom_ = *pfCameraExtZoom;
				savedAspectRatio_ = *pfAspectRatio;
				savedZoomAspect_ = true;
				GameSetRemotePlayerCameraExtZoomAndAspect(playerNum);
			}
		}

		if (flags_ & kAim) {
			if (CAMERA_AIM* internalAim = GameGetInternalAim()) {
				std::memcpy(&savedAim_, internalAim, sizeof(CAMERA_AIM));
				savedAimValid_ = true;
				GameSetRemotePlayerAim(playerNum);
			}
		}

		// 保留：T08 — weapon skill switching (store previous skills on instance)
		// if (flags_ & kSkills) { ... }

		if (flags_ & kFocus) {
			if (pbyteCurrentPlayer) {
				savedCurrentPlayer_ = *pbyteCurrentPlayer;
				*pbyteCurrentPlayer = playerNum;
				switchedFocus_ = true;
			}
		}
	}

	~ScopedPlayerContext()
	{
		if (flags_ == kNone) {
			return;
		}

		if (switchedFocus_ && pbyteCurrentPlayer) {
			*pbyteCurrentPlayer = savedCurrentPlayer_;
		}

		// 保留：T08 — weapon skill restoration
		// if (flags_ & kSkills) { ... }

		if (savedCamMode1_ && pbyteCameraMode) {
			*pbyteCameraMode = savedCameraMode_;
		}
		if (savedCamMode2_ && wCameraMode2) {
			*wCameraMode2 = savedCameraMode2_;
		}

		if (savedZoomAspect_ && pfCameraExtZoom && pfAspectRatio) {
			*pfCameraExtZoom = savedExtZoom_;
			*pfAspectRatio = savedAspectRatio_;
		}

		if (savedAimValid_) {
			if (CAMERA_AIM* internalAim = GameGetInternalAim()) {
				std::memcpy(internalAim, &savedAim_, sizeof(CAMERA_AIM));
			}
		}
	}

	ScopedPlayerContext(const ScopedPlayerContext&) = delete;
	ScopedPlayerContext& operator=(const ScopedPlayerContext&) = delete;
	ScopedPlayerContext(ScopedPlayerContext&&) = delete;
	ScopedPlayerContext& operator=(ScopedPlayerContext&&) = delete;

private:
	uint32_t flags_ = kNone;
	uint8_t savedCameraMode_ = 0;
	uint16_t savedCameraMode2_ = 0;
	uint8_t savedCurrentPlayer_ = 0;
	float savedExtZoom_ = 0.0f;
	float savedAspectRatio_ = 0.0f;
	CAMERA_AIM savedAim_{};
	bool savedCamMode1_ = false;
	bool savedCamMode2_ = false;
	bool savedZoomAspect_ = false;
	bool savedAimValid_ = false;
	bool switchedFocus_ = false;
};
