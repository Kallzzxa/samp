#include "Game.h"
#include <cmath>

CAMERA_AIM* pcaInternalAim = nullptr;
uint8_t* pbyteCameraMode = nullptr;
float* pfCameraExtZoom = nullptr;
float* pfAspectRatio = nullptr;
uint16_t* wCameraMode2 = nullptr;

uint8_t* pbyteCurrentPlayer = nullptr;

CAMERA_AIM caLocalPlayerAim;
CAMERA_AIM caRemotePlayerAim[PLAYER_PED_SLOTS];
uint8_t byteCameraMode[PLAYER_PED_SLOTS];
float fCameraExtZoom[PLAYER_PED_SLOTS];
float fCameraAspectRatio[PLAYER_PED_SLOTS];
float fLocalCameraExtZoom;
float fLocalAspectRatio;

/**
 * @return
 */
CAMERA_AIM* GameGetInternalAim()
{
	return pcaInternalAim;
}

/**
 * @return
 */
uint8_t GameGetLocalPlayerCameraMode()
{
	return pbyteCameraMode ? *pbyteCameraMode : 4;
}

/**
 * @param bytePlayerNumber
 * @return
 */
uint8_t GameGetPlayerCameraMode(uint8_t bytePlayerNumber)
{
	return bytePlayerNumber < PLAYER_PED_SLOTS ? byteCameraMode[bytePlayerNumber] : 4;
}

float GameGetLocalPlayerCameraExtZoom()
{
	return pfCameraExtZoom ? ((*pfCameraExtZoom - 35.0f) * 0.028571429f) : 0.0f;
}

float GameGetAspectRatio()
{
	return pfAspectRatio ? *pfAspectRatio : 0.0f;
}

void GameAimSyncInit()
{
	memset(&caLocalPlayerAim, 0, sizeof(caLocalPlayerAim));
	memset(&caRemotePlayerAim, 0, sizeof(caRemotePlayerAim));
	memset(byteCameraMode, 4, sizeof(byteCameraMode));

	for (int i = 0; i < PLAYER_PED_SLOTS; ++i) {
		fCameraAspectRatio[i] = 0.333333f;
		fCameraExtZoom[i] = 1.0f;
	}

	sa::CCamera& TheCamera = sa::getTheCamera();

//    sa::CCamera: 951FA8 TheCamera.Cams[0]: 952118
	pcaInternalAim = (CAMERA_AIM*) &TheCamera.m_aCams[0].Front; // +2D8(728) TheCamera.Cams[0].Front
	pbyteCameraMode = (uint8_t*) &TheCamera.m_aCams[0].m_nMode; //+17E(382) TheCamera.Cams[0].m_nMode
	pfAspectRatio = eng::as<float*>(eng::Sym::CDraw_ms_fAspectRatio); // _ZN5CDraw15ms_fAspectRatioE
	pfCameraExtZoom = &TheCamera.m_aCams[0].FOV; // TheCamera+368 +140 TheCamera.Cams[0].m_fFOV
	wCameraMode2 = (uint16_t*) &TheCamera.PlayerWeaponMode; // TheCamera.PlayerWeaponMode

	pbyteCurrentPlayer = eng::as<uint8_t*>(eng::Sym::CWorld_PlayerInFocus); // _ZN6CWorld13PlayerInFocusE
}

CAMERA_AIM* GameGetRemotePlayerAim(uint8_t bytePlayerNumber)
{
	return bytePlayerNumber < PLAYER_PED_SLOTS ? &caRemotePlayerAim[bytePlayerNumber] : &caRemotePlayerAim[0];
}

void GameStoreLocalPlayerCameraExtZoomAndAspect()
{
	if (!pfCameraExtZoom || !pfAspectRatio) return;
	fLocalCameraExtZoom = *pfCameraExtZoom;
	fLocalAspectRatio = *pfAspectRatio;
}

void GameSetRemotePlayerCameraExtZoomAndAspect(uint8_t bytePlayerNumber)
{
	if (!pfCameraExtZoom || !pfAspectRatio || bytePlayerNumber >= PLAYER_PED_SLOTS) return;
	*pfCameraExtZoom = fCameraExtZoom[bytePlayerNumber] * 35.0f + 35.0f;
	*pfAspectRatio = fCameraAspectRatio[bytePlayerNumber] + 1.0f;
}

void GameStoreLocalPlayerAim()
{
	if (!pcaInternalAim) return;
	memcpy(&caLocalPlayerAim, pcaInternalAim, sizeof(CAMERA_AIM));
}

void GameSetRemotePlayerAim(uint8_t bytePlayerNumber)
{
	if (!pcaInternalAim || bytePlayerNumber >= PLAYER_PED_SLOTS) return;
	memcpy(pcaInternalAim, &caRemotePlayerAim[bytePlayerNumber], sizeof(CAMERA_AIM));
}

void GameSetLocalPlayerCameraExtZoomAndAspect()
{
	if (!pfCameraExtZoom || !pfAspectRatio) return;
	*pfCameraExtZoom = fLocalCameraExtZoom;
	*pfAspectRatio = fLocalAspectRatio;
}

void GameSetLocalPlayerAim()
{
	if (!pcaInternalAim) return;
	memcpy(pcaInternalAim, &caLocalPlayerAim, sizeof(CAMERA_AIM));
}

/**
 * @param byteMode
 * @param bytePlayerNumber
 */
void GameSetPlayerCameraMode(uint8_t byteMode, uint8_t bytePlayerNumber)
{
	if (bytePlayerNumber >= PLAYER_PED_SLOTS) return;
	byteCameraMode[bytePlayerNumber] = byteMode;
}

/**
 * @param vec1
 * @param vec2
 */
void calculateAimVector(sa::CVector* vec1, sa::CVector* vec2)
{
	float f1, f2, f3;

	f1 = atan2(vec1->x, vec1->y) - 1.570796370506287; // PI/2
	f2 = sin(f1);
	f3 = cos(f1);
	vec2->x = vec1->y * 0.0 - f3 * vec1->z;
	vec2->y = f2 * vec1->z - vec1->x * 0.0;
	vec2->z = f3 * vec1->x - f2 * vec1->y;
}

/**
 * @param bytePlayerNumber
 * @param pAim
 */
void GameStoreRemotePlayerAim(uint8_t bytePlayerNumber, CAMERA_AIM* pAim)
{
	if (!pAim || bytePlayerNumber >= PLAYER_PED_SLOTS) return;
	memcpy(&caRemotePlayerAim[bytePlayerNumber], pAim, sizeof(CAMERA_AIM));
}

/**
 * @param bytePlayerNumber
 * @param fExtZoom
 * @param fAspectRatio
 */
void GameSetPlayerCameraExtZoomAndAspect(uint8_t bytePlayerNumber, float fExtZoom, float fAspectRatio)
{
	if (bytePlayerNumber >= PLAYER_PED_SLOTS) return;
	fCameraExtZoom[bytePlayerNumber] = fExtZoom;
	fCameraAspectRatio[bytePlayerNumber] = fAspectRatio;
}
