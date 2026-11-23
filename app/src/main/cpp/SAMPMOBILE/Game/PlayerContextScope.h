#pragma once

#include <cstdint>
#include "Aimstuff.h"

// RAII scope that temporarily switches the engine's global local-player state
// (camera mode, weapon camera mode, zoom/aspect, aim vectors, PlayerInFocus)
// to a remote player slot, and restores the saved local state on scope exit.
//
// Saved camera modes and previous PlayerInFocus live on the scope instance
// rather than globals, preventing accidental leakage on early returns.
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
			}
			if (wCameraMode2) {
				savedCameraMode2_ = *wCameraMode2;
				*wCameraMode2 = (remoteMode == 4) ? 0 : remoteMode;
			}
		}

		if (flags_ & kZoomAspect) {
			GameStoreLocalPlayerCameraExtZoomAndAspect();
			GameSetRemotePlayerCameraExtZoomAndAspect(playerNum);
		}

		if (flags_ & kAim) {
			GameStoreLocalPlayerAim();
			GameSetRemotePlayerAim(playerNum);
		}

		// 保留：T08 — weapon skill switching
		// if (flags_ & kSkills) {
		//     GameStoreLocalPlayerSkills();
		//     GameSetRemotePlayerSkills(playerNum);
		// }

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
		// if (flags_ & kSkills) {
		//     GameSetLocalPlayerSkills();
		// }

		if (flags_ & kCamMode) {
			if (pbyteCameraMode) {
				*pbyteCameraMode = savedCameraMode_;
			}
			if (wCameraMode2) {
				*wCameraMode2 = savedCameraMode2_;
			}
		}

		if (flags_ & kZoomAspect) {
			GameSetLocalPlayerCameraExtZoomAndAspect();
		}

		if (flags_ & kAim) {
			GameSetLocalPlayerAim();
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
	bool switchedFocus_ = false;
};
