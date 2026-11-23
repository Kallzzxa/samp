#include "../UI/UI.h"
#include "../Client.h"
#include "../Game/Game.h"
#include "../Game/AudioStream.h"
#include "../Java/EditObject.h"
#include "NetGame.h"
#include "RpcHandlers.h"
#include <cmath>

extern UI* pUI;
extern Game* pGame;
extern NetGame* pNetGame;
extern AudioStream* pAudioStream;

void RpcHandlers::OnSetGravity(const proto::SetGravity& p)
{
	spdlog::info("ScriptRPC: ScrSetGravity");
	pGame->SetGravity(p.gravity);
}

void RpcHandlers::OnSetCameraPos(const proto::SetCameraPos& p)
{
	spdlog::info("ScriptRPC: ScrSetCameraPos");
	pGame->GetCamera()->SetPosition(p.position.x, p.position.y, p.position.z, 0.0f, 0.0f, 0.0f);
}

void RpcHandlers::OnSetCameraLookAt(const proto::SetCameraLookAt& p)
{
	spdlog::info("ScriptRPC: ScrSetCameraLookAt");

	uint8_t byteType = p.cutType;
	if (byteType < 1 || byteType > 2) {
		byteType = 2;
	}

	pGame->GetCamera()->LookAtPoint(p.position.x, p.position.y, p.position.z, byteType);
}

void RpcHandlers::OnInterpolateCamera(const proto::InterpolateCamera& p)
{
	spdlog::info("ScriptRPC: ScrInterpolateCamera");

	uint8_t byteMode = p.cutType;
	if (byteMode < 1 || byteMode > 2) {
		byteMode = 2;
	}

	if (p.time > 0) {
		sa::CVector vecFrom(p.from.x, p.from.y, p.from.z);
		sa::CVector vecTo(p.to.x, p.to.y, p.to.z);
		pNetGame->GetPlayerPool()->GetLocalPlayer()->m_bSpectateProcessed = true;
		if (p.setPos) {
			pGame->GetCamera()->InterpolateCameraPos(&vecFrom, &vecTo, p.time, byteMode);
		}
		else {
			pGame->GetCamera()->InterpolateCameraLookAt(&vecFrom, &vecTo, p.time, byteMode);
		}
	}
}

void RpcHandlers::OnTogglePlayerSpectating(const proto::TogglePlayerSpectating& p)
{
	spdlog::info("ScriptRPC: ScrTogglePlayerSpectating");
	spdlog::info("dwToggle: {}", p.toggle);

	pNetGame->GetPlayerPool()->GetLocalPlayer()->ToggleSpectating(p.toggle);
}

void RpcHandlers::OnSetSpawnInfo(const proto::SetSpawnInfo& p)
{
	spdlog::info("ScriptRPC: ScrSetSpawnInfo");

	PLAYER_SPAWN_INFO spawnInfo{};
	spawnInfo.byteTeam = p.spawnInfo.team;
	spawnInfo.iSkin = p.spawnInfo.skin;
	spawnInfo.unk = p.spawnInfo.unk;
	spawnInfo.vecPos = sa::CVector(p.spawnInfo.position.x, p.spawnInfo.position.y, p.spawnInfo.position.z);
	spawnInfo.fRotation = p.spawnInfo.rotation;
	memcpy(spawnInfo.iSpawnWeapons, p.spawnInfo.spawnWeapons.data(), sizeof(spawnInfo.iSpawnWeapons));
	memcpy(spawnInfo.iSpawnWeaponsAmmo, p.spawnInfo.spawnWeaponsAmmo.data(), sizeof(spawnInfo.iSpawnWeaponsAmmo));

	pNetGame->GetPlayerPool()->GetLocalPlayer()->SetSpawnInfo(&spawnInfo);
}

void RpcHandlers::OnShowGangZone(const proto::ShowGangZone& p)
{
	spdlog::info("ScriptRPC: ScrAddGangZone");

	CGangZonePool* pGangZonePool = pNetGame->GetGangZonePool();
	if (pGangZonePool) {
		pGangZonePool->New(p.zoneId, p.min.x, p.min.y, p.max.x, p.max.y, p.color);
	}
}

void RpcHandlers::OnHideGangZone(const proto::HideGangZone& p)
{
	spdlog::info("ScriptRPC: ScrGangZoneDestroy");

	CGangZonePool* pGangZonePool = pNetGame->GetGangZonePool();
	if (pGangZonePool) {
		pGangZonePool->Delete(p.zoneId);
	}
}

void RpcHandlers::OnFlashGangZone(const proto::FlashGangZone& p)
{
	spdlog::info("ScriptRPC: ScrGangZoneFlash");

	CGangZonePool* pGangZonePool = pNetGame->GetGangZonePool();
	if (pGangZonePool) {
		pGangZonePool->Flash(p.zoneId, p.color);
	}
}

void RpcHandlers::OnStopFlashGangZone(const proto::StopFlashGangZone& p)
{
	spdlog::info("ScriptRPC: ScrGangZoneStopFlash");

	CGangZonePool* pGangZonePool = pNetGame->GetGangZonePool();
	if (pGangZonePool) {
		pGangZonePool->StopFlash(p.zoneId);
	}
}

static void ApplyObjectMaterial(CObject* pObject, const proto::ObjectMaterial& m, size_t maxTexNameLen)
{
	if (!pObject) return;

	if (m.materialType == MATERIAL_TYPE_TEXTURE) {
		if (m.txdName.size() >= maxTexNameLen || m.textureName.size() >= maxTexNameLen) {
			return;
		}

		int iMaterialModel = m.modelId;
		if (m.modelId == 0xFFFF || m.modelId > 20000) {
			iMaterialModel = -1;
		}

		pObject->SetMaterial(iMaterialModel, m.materialIndex,
			const_cast<char*>(m.txdName.c_str()),
			const_cast<char*>(m.textureName.c_str()),
			m.materialColor);
	}
	else if (m.materialType == MATERIAL_TYPE_TEXT) {
		if (m.fontName.size() >= 33) {
			return;
		}

		char utf8[2048 + 1];
		strncpy(utf8, Encoding::cp2utf(m.text.c_str()).c_str(), 2048);
		utf8[2048] = '\0';

		pObject->SetMaterialText(m.materialIndex, m.materialSize,
			const_cast<char*>(m.fontName.c_str()),
			m.fontSize, m.fontBold, m.fontColor, m.backgroundColor, m.alignment, utf8);
	}
}

void RpcHandlers::OnCreateObject(const proto::CreateObject& p)
{
	spdlog::info("ScriptRPC: ScrCreateObject");

	// NaN coords/rotation would write straight into the engine transform
	if (!proto::isFinite(p.position) || !proto::isFinite(p.rotation)
			|| !proto::isFinite(p.attachOffset) || !proto::isFinite(p.attachRotation)) {
		spdlog::warn("ScrCreateObject: rejected non-finite transform for object {}", p.objectId);
		return;
	}

	CObjectPool* pObjectPool = pNetGame->GetObjectPool();
	if (!pObjectPool) return;

	sa::CVector vecPos(p.position.x, p.position.y, p.position.z);
	sa::CVector vecRot(p.rotation.x, p.rotation.y, p.rotation.z);
	if (!pObjectPool->New(p.objectId, p.modelId, vecPos, vecRot, p.drawDistance)) {
		return;
	}

	CObject* pObject = pObjectPool->GetAt(p.objectId);
	sa::CVector vecAttachOffset(p.attachOffset.x, p.attachOffset.y, p.attachOffset.z);
	sa::CVector vecAttachRot(p.attachRotation.x, p.attachRotation.y, p.attachRotation.z);
	if (p.attachedVehicleId != INVALID_VEHICLE_ID) {
		if (pObject) {
			pObject->SetAttachedVehicle(p.attachedVehicleId, &vecAttachOffset, &vecAttachRot);
		}
	}
	else if (p.attachedObjectId != INVALID_OBJECT_ID) {
		if (pObject) {
			pObject->SetAttachedObject(p.attachedObjectId, &vecAttachOffset, &vecAttachRot, p.syncRotation);
		}
	}

	for (const auto& mat : p.materials) {
		ApplyObjectMaterial(pObject, mat, 32);
	}

	if (pObject && p.noCameraCol) {
		pObject->m_bNoCameraCollision = true;
	}
}

void RpcHandlers::OnDestroyObject(const proto::DestroyObject& p)
{
	spdlog::info("ScriptRPC: ScrDestroyObject");

	CObjectPool* pObjectPool = pNetGame->GetObjectPool();
	if (!pObjectPool) return;

	CObject* pObject = pObjectPool->GetAt(p.objectId);
	if (pObject) {
		CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
		if (pPlayerPool) {
			CLocalPlayer* pLocalPlayer = pPlayerPool->GetLocalPlayer();
			if (pLocalPlayer && pLocalPlayer->m_surfSync.bIsActive &&
					!pLocalPlayer->m_surfSync.bIsVehicle &&
					pLocalPlayer->m_surfSync.pSurfInst == (uintptr_t) pObject) {
				pLocalPlayer->DisableSurfing();
			}
		}

		if (pObjectPool->Delete(p.objectId)) {
			spdlog::info("DestroyObject; Total objects: {}", pObjectPool->m_iObjectCount);
		}
	}
}

void RpcHandlers::OnSetObjectMaterial(const proto::SetObjectMaterial& p)
{
	spdlog::info("ScriptRPC: ScrSetObjectMaterial");

	CObjectPool* pObjectPool = pNetGame->GetObjectPool();
	if (!pObjectPool || p.objectId >= MAX_OBJECTS) {
		return;
	}

	CObject* pObject = pObjectPool->GetAt(p.objectId);
	if (pObject) {
		ApplyObjectMaterial(pObject, p.material, 33);
	}
}

void RpcHandlers::OnRemoveBuilding(const proto::RemoveBuilding& p)
{
	spdlog::info("ScriptRPC: ScrRemoveBuilding");
	RemoveBuilding::add(p.modelId, {p.position.x, p.position.y, p.position.z}, p.radius);
}

void RpcHandlers::OnStopObjectCameraCollision(const proto::StopObjectCameraCollision& p)
{
	spdlog::info("ScriptRPC: ScrStopObjectCameraCollision");

	CObjectPool* pObjectPool = pNetGame->GetObjectPool();
	if (!pObjectPool) return;

	CObject* pObject = pObjectPool->GetAt(p.objectId);
	if (pObject) {
		pObject->m_bNoCameraCollision = true;
	}
}

void RpcHandlers::OnAttachCameraToObject(const proto::AttachCameraToObject& p)
{
	spdlog::info("ScriptRPC: ScrAttachCameraToObject");

	CObjectPool* pObjectPool = pNetGame->GetObjectPool();
	if (!pObjectPool) return;

	CObject* pObject = pObjectPool->GetAt(p.objectId);
	if (!pObject) return;

	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	if (pPlayerPool && pPlayerPool->GetLocalPlayer()) {
		pPlayerPool->GetLocalPlayer()->m_bSpectateProcessed = true;
	}

	if (pGame && pGame->GetCamera()) {
		pGame->GetCamera()->Attach(pObject);
	}
}

void RpcHandlers::OnSetPlayerSkinLegacyT03(const proto::SetPlayerSkinLegacyT03& p)
{
	spdlog::info("ScriptRPC: ScrSetPlayerSkin");

	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	if (!pPlayerPool) {
		return;
	}

	if (IsValidPedModel(p.skinId)) {
		if (pPlayerPool->GetLocalPlayerID() == p.playerId) {
			pPlayerPool->GetLocalPlayer()->GetPlayerPed()->SetModelIndex(p.skinId);
		}
		else {
			if (pPlayerPool->GetSlotState(p.playerId)) {
				CRemotePlayer* pPlayer = pPlayerPool->GetAt(p.playerId);
				if (!pPlayer || pPlayer->GetState() == PLAYER_STATE_NONE) {
					return;
				}
				CPlayerPed* pPlayerPed = pPlayer->GetPlayerPed();
				if (!pPlayerPed) {
					return;
				}
				pPlayerPed->SetModelIndex(p.skinId);
			}
		}
	}
	else {
		if (pUI) pUI->chat()->addDebugMessage("Warning: SetPlayerSkin %d isn't a valid ped model.", p.skinId);
	}
}

void RpcHandlers::OnSetPlayerMapIcon(const proto::SetPlayerMapIcon& p)
{
	spdlog::info("ScriptRPC: ScrSetPlayerMapIcon");

	uint8_t byteType = p.type;
	// fix crash (invalid GTASA map icon sprite types)
	if (byteType == 1 || byteType == 2 || byteType == 4 || byteType == 56) {
		byteType = 52;
	}

	pNetGame->SetMapIcon(p.iconId, p.position.x, p.position.y, p.position.z, byteType, p.color, p.style);
}

void RpcHandlers::OnRemovePlayerMapIcon(const proto::RemovePlayerMapIcon& p)
{
	spdlog::info("ScriptRPC: ScrRemovePlayerMapIcon");
	pNetGame->DisableMapIcon(p.iconId);
}

void RpcHandlers::OnShowNameTag(const proto::ShowNameTag& p)
{
	spdlog::info("ScriptRPC: ScrShowNameTag");

	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	if (pPlayerPool && pPlayerPool->GetSlotState(p.playerId)) {
		CRemotePlayer* pPlayer = pPlayerPool->GetAt(p.playerId);
		if (pPlayer) {
			pPlayer->m_bShowNameTag = p.show;
		}
	}
}

void RpcHandlers::OnApplyPlayerAnimation(const proto::ApplyPlayerAnimation& p)
{
	spdlog::info("ScriptRPC: ScrApplyPlayerAnimation");

	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	CPlayerPed* pPlayerPed = nullptr;

	if (pPlayerPool) {
		if (pPlayerPool->GetLocalPlayerID() == p.playerId) {
			pPlayerPed = pPlayerPool->GetLocalPlayer()->GetPlayerPed();
		}
		else if (pPlayerPool->GetSlotState(p.playerId)) {
			CRemotePlayer* pRemotePlayer = pPlayerPool->GetAt(p.playerId);
			if (pRemotePlayer) {
				pPlayerPed = pRemotePlayer->GetPlayerPed();
			}
		}

		spdlog::info("ApplyAnimation: {}, {}", p.animLib, p.animName);

		if (pPlayerPed) {
			pPlayerPed->ApplyAnimation(
				const_cast<char*>(p.animName.c_str()),
				const_cast<char*>(p.animLib.c_str()),
				p.delta, p.loop, p.lockX, p.lockY, p.freeze, p.time);
		}
	}
}

void RpcHandlers::OnClearPlayerAnimations(const proto::ClearPlayerAnimations& p)
{
	spdlog::info("ScriptRPC: ScrClearPlayerAnimations");

	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	CPlayerPed* pPlayerPed = nullptr;

	if (pPlayerPool) {
		if (p.playerId == pPlayerPool->GetLocalPlayerID()) {
			pPlayerPed = pPlayerPool->GetLocalPlayer()->GetPlayerPed();
		}
		else {
			if (pPlayerPool->GetSlotState(p.playerId)) {
				CRemotePlayer* pRemotePlayer = pPlayerPool->GetAt(p.playerId);
				if (pRemotePlayer) {
					pPlayerPed = pRemotePlayer->GetPlayerPed();
				}
			}
		}

		if (pPlayerPed && pPlayerPed->m_ped) {
			RwMatrix mat = pPlayerPed->m_ped->GetMatrix().ToRwMatrix();
			pPlayerPed->TeleportTo(mat.pos.x, mat.pos.y, mat.pos.z);
		}
	}
}

void RpcHandlers::OnSetPlayerHealth(const proto::SetPlayerHealth& p)
{
	spdlog::info("ScriptRPC: ScrSetPlayerHealth");

	CPlayerPed* pPlayerPed = pNetGame->GetPlayerPool()->GetLocalPlayer()->GetPlayerPed();
	if (pPlayerPed) {
		pPlayerPed->SetHealth(p.health);
	}
}

void RpcHandlers::OnGivePlayerWeapon(const proto::GivePlayerWeapon& p)
{
	spdlog::info("ScriptRPC: ScrGivePlayerWeapon");

	CPlayerPed* pPlayerPed = pNetGame->GetPlayerPool()->GetLocalPlayer()->GetPlayerPed();
	if (pPlayerPed) {
		pPlayerPed->GiveWeapon(p.weapon, p.ammo);
	}
}

void RpcHandlers::OnSetPlayerInterior(const proto::SetPlayerInterior& p)
{
	spdlog::info("ScriptRPC: ScrSetPlayerInterior");
	pGame->FindPlayerPed()->SetInterior(p.interiorId, true);
}

void RpcHandlers::OnShowTextDraw(const proto::ShowTextDraw& p)
{
	spdlog::info("ScriptRPC: ScrShowTextDraw");

	CTextDrawPool* pTextDrawPool = pNetGame->GetTextDrawPool();
	if (pTextDrawPool == nullptr) {
		return;
	}

	if (p.text.size() < MAX_TEXT_DRAW_LINE) {
		TEXT_DRAW_TRANSMIT textDrawTransmit{};
		textDrawTransmit.byteFlags = p.flags;
		textDrawTransmit.fLetterWidth = p.letterSize.x;
		textDrawTransmit.fLetterHeight = p.letterSize.y;
		textDrawTransmit.dwLetterColor = p.letterColor;
		textDrawTransmit.fLineWidth = p.lineSize.x;
		textDrawTransmit.fLineHeight = p.lineSize.y;
		textDrawTransmit.dwBoxColor = p.boxColor;
		textDrawTransmit.byteShadow = p.shadow;
		textDrawTransmit.byteOutline = p.outline;
		textDrawTransmit.dwBackgroundColor = p.backgroundColor;
		textDrawTransmit.byteStyle = p.style;
		textDrawTransmit.byteSelectable = p.selectable;
		textDrawTransmit.fX = p.position.x;
		textDrawTransmit.fY = p.position.y;
		textDrawTransmit.wModelID = p.modelId;
		textDrawTransmit.vecRot = sa::CVector(p.rotation.x, p.rotation.y, p.rotation.z);
		textDrawTransmit.fZoom = p.zoom;
		textDrawTransmit.wColor1 = p.color1;
		textDrawTransmit.wColor2 = p.color2;

		pTextDrawPool->New(p.textDrawId, &textDrawTransmit, const_cast<char*>(p.text.c_str()));
	}
	else {
		if (pUI) pUI->chat()->addInfoMessage("Warning: ignoring large TetDraw size=%u", static_cast<unsigned>(p.text.size()));
	}
}

void RpcHandlers::OnHideTextDraw(const proto::HideTextDraw& p)
{
	spdlog::info("ScriptRPC: ScrHideTextDraw");

	CTextDrawPool* pTextDrawPool = pNetGame->GetTextDrawPool();
	if (pTextDrawPool == nullptr) {
		return;
	}

	pTextDrawPool->Delete(p.textDrawId);
}

void RpcHandlers::OnTextDrawSetString(const proto::TextDrawSetString& p)
{
	spdlog::info("ScriptRPC: ScrTextDrawSetString");

	CTextDrawPool* pTextDrawPool = pNetGame->GetTextDrawPool();
	if (pTextDrawPool == nullptr) {
		return;
	}

	if (p.text.size() < MAX_TEXT_DRAW_LINE) {
		CTextDraw* pTextDraw = pTextDrawPool->GetAt(p.textDrawId);
		if (pTextDraw) {
			pTextDraw->SetText(p.text.c_str());
		}
	}
}

void RpcHandlers::OnSelectTextDraw(const proto::SelectTextDraw& p)
{
	spdlog::info("ScriptRPC: ScrSelectTextDraw");
	spdlog::info("SELECTTEXTDRAW: bEnable: {}, dwColor: {}", p.enable, p.hoverColor);

	pNetGame->GetTextDrawPool()->SetSelectState(p.enable, p.hoverColor);
}

void RpcHandlers::OnSetPlayerAmmo(const proto::SetPlayerAmmo& p)
{
	spdlog::info("ScriptRPC: ScrSetPlayerAmmo");

	CLocalPlayer* pLocalPlayer = pNetGame->GetPlayerPool()->GetLocalPlayer();
	pLocalPlayer->GetPlayerPed()->SetAmmo(p.weapon, p.ammo);
}

void RpcHandlers::OnSetVehicleHealth(const proto::SetVehicleHealth& p)
{
	spdlog::info("ScriptRPC: ScrSetVehicleHealth");

	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
	if (pVehiclePool) {
		CVehicle* pVehicle = pVehiclePool->GetAt(p.vehicleId);
		if (pVehicle) {
			pVehicle->SetHealth(p.health);
		}
	}
}

void RpcHandlers::OnAttachTrailerToVehicle(const proto::AttachTrailerToVehicle& p)
{
	spdlog::info("ScriptRPC: ScrAttachTrailerToVehicle");

	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
	if (!pVehiclePool) {
		return;
	}

	CVehicle* pTrailer = pVehiclePool->GetAt(p.trailerId);
	CVehicle* pVehicle = pVehiclePool->GetAt(p.vehicleId);

	if (pTrailer && pVehicle) {
		pVehicle->SetTrailer(pTrailer);
		pVehicle->AttachTrailer();
	}
}

void RpcHandlers::OnDetachTrailerFromVehicle(const proto::DetachTrailerFromVehicle& p)
{
	spdlog::info("ScriptRPC: ScrDetachTrailerFromVehicle");

	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
	if (!pVehiclePool) {
		return;
	}

	CVehicle* pVehicle = pVehiclePool->GetAt(p.vehicleId);
	if (pVehicle) {
		pVehicle->DetachTrailer();
		pVehicle->SetTrailer(nullptr);
	}
}

void RpcHandlers::OnSetObjectPosLegacyT36(const proto::SetObjectPosLegacyT36& p)
{
	spdlog::info("ScriptRPC: ScrSetObjectPos");

	if (!proto::isFinite(p.position)) {
		spdlog::warn("ScrSetObjectPos: rejected non-finite position for object {}", p.objectId);
		return;
	}

	CObjectPool* pObjectPool = pNetGame->GetObjectPool();
	if (!pObjectPool) {
		return;
	}

	CObject* pObject = pObjectPool->GetAt(p.objectId);
	if (pObject) {
		pObject->TeleportTo(p.position.x, p.position.y, p.position.z);
	}
}

void RpcHandlers::OnSetObjectRotation(const proto::SetObjectRotation& p)
{
	spdlog::info("ScriptRPC: ScrSetObjectRotation");

	if (!proto::isFinite(p.rotation)) {
		spdlog::warn("ScrSetObjectRotation: rejected non-finite rotation for object {}", p.objectId);
		return;
	}

	CObjectPool* pObjectPool = pNetGame->GetObjectPool();
	if (!pObjectPool) {
		return;
	}

	CObject* pObject = pObjectPool->GetAt(p.objectId);
	if (!pObject) {
		return;
	}

	sa::CVector vecRot(p.rotation.x, p.rotation.y, p.rotation.z);
	pObject->SetRotation(&vecRot);
}

void RpcHandlers::OnCreateExplosion(const proto::CreateExplosion& p)
{
	spdlog::info("ScriptRPC: ScrCreateExplosion");
	ScriptCommand(&create_explosion_with_radius, p.position.x, p.position.y, p.position.z, p.type, p.radius);
}

void RpcHandlers::OnSetVehicleNumberPlate(const proto::SetVehicleNumberPlate& p)
{
	spdlog::info("ScriptRPC: ScrSetVehicleNumberPlate");
	if (p.plate.size() > 32) {
		return;
	}

	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
	if (!pVehiclePool) {
		return;
	}

	CVehicle* pVehicle = pVehiclePool->GetAt(p.vehicleId);
	if (!pVehicle) {
		return;
	}

	pVehicle->SetPlateText(p.plate.c_str());
}

#define SPECTATE_TYPE_NORMAL 1
#define SPECTATE_TYPE_FIXED 2
#define SPECTATE_TYPE_SIDE 3

void RpcHandlers::OnSpectatePlayer(const proto::SpectatePlayer& p)
{
	spdlog::info("ScriptRPC: ScrSpectatePlayer");

	uint8_t byteMode = p.mode;
	switch (byteMode) {
		case SPECTATE_TYPE_FIXED: byteMode = 15;
			break;
		case SPECTATE_TYPE_SIDE: byteMode = 14;
			break;
		default: byteMode = 4;
	}

	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	CLocalPlayer* pLocalPlayer = pPlayerPool ? pPlayerPool->GetLocalPlayer() : nullptr;
	if (!pLocalPlayer) {
		return;
	}
	pLocalPlayer->m_byteSpectateMode = byteMode;
	pLocalPlayer->SpectatePlayer(p.playerId);
}

void RpcHandlers::OnSpectateVehicle(const proto::SpectateVehicle& p)
{
	spdlog::info("ScriptRPC: ScrSpectateVehicle");

	uint8_t byteMode = p.mode;
	switch (byteMode) {
		case SPECTATE_TYPE_FIXED: byteMode = 15;
			break;
		case SPECTATE_TYPE_SIDE: byteMode = 14;
			break;
		default: byteMode = 3;
	}

	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	CLocalPlayer* pLocalPlayer = pPlayerPool ? pPlayerPool->GetLocalPlayer() : nullptr;
	if (!pLocalPlayer) {
		return;
	}
	pLocalPlayer->m_byteSpectateMode = byteMode;
	pLocalPlayer->SpectateVehicle(p.vehicleId);
}

void RpcHandlers::OnRemoveVehicleComponent(const proto::RemoveVehicleComponent& p)
{
	spdlog::info("ScriptRPC: ScrRemoveVehicleComponent");

	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
	if (!pVehiclePool) {
		return;
	}

	CVehicle* pVehicle = pVehiclePool->GetAt(p.vehicleId);
	if (!pVehicle) {
		return;
	}

	pVehicle->RemoveComponent(p.componentId);
}

void RpcHandlers::OnAttachObjectToPlayer(const proto::AttachObjectToPlayer& p)
{
	spdlog::info("ScriptRPC: ScrAttachObjectToPlayer");

	if (!proto::isFinite(p.offset) || !proto::isFinite(p.rotation)) {
		spdlog::warn("ScrAttachObjectToPlayer: rejected non-finite offset/rot for object {}", p.objectId);
		return;
	}

	CObjectPool* pObjectPool = pNetGame->GetObjectPool();
	if (!pObjectPool) {
		return;
	}
	CObject* pObject = pObjectPool->GetAt(p.objectId);
	if (!pObject || !pObject->m_entity || !pObject->m_gtaId) {
		return;
	}

	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	if (!pPlayerPool) {
		return;
	}

	if (pPlayerPool->GetLocalPlayerID() == p.playerId) {
		CLocalPlayer* pLocalPlayer = pPlayerPool->GetLocalPlayer();
		if (!pLocalPlayer || !pLocalPlayer->GetPlayerPed()) {
			return;
		}
		ScriptCommand(&attach_object_to_actor,
				pObject->m_gtaId,
				pLocalPlayer->GetPlayerPed()->m_gtaId,
				p.offset.x, p.offset.y, p.offset.z,
				p.rotation.x, p.rotation.y, p.rotation.z);
	}
	else {
		CRemotePlayer* pRemotePlayer = pPlayerPool->GetAt(p.playerId);
		if (!pRemotePlayer || !pRemotePlayer->GetPlayerPed()) {
			return;
		}
		ScriptCommand(&attach_object_to_actor,
				pObject->m_gtaId,
				pRemotePlayer->GetPlayerPed()->m_gtaId,
				p.offset.x, p.offset.y, p.offset.z,
				p.rotation.x, p.rotation.y, p.rotation.z);
	}
}

void RpcHandlers::OnSetPlayerWantedLevel(const proto::SetPlayerWantedLevel& p)
{
	spdlog::info("ScriptRPC: ScrSetPlayerWantedLevel");

	if (pGame) {
		pGame->SetWantedLevel(p.level);
	}
}

void RpcHandlers::OnSetPlayerSpecialAction(const proto::SetPlayerSpecialAction& p)
{
	spdlog::info("ScriptRPC: ScrSetPlayerSpecialAction");

	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	if (!pPlayerPool) {
		return;
	}

	pPlayerPool->GetLocalPlayer()->ApplySpecialAction(p.action);
}

void RpcHandlers::OnEnableStuntBonus(const proto::EnableStuntBonus& p)
{
	spdlog::info("ScriptRPC: ScrEnableStuntBonus");
	pGame->EnableStuntBonus(p.enable);
}

void RpcHandlers::OnSetPlayerFightingStyle(const proto::SetPlayerFightingStyle& p)
{
	spdlog::info("ScriptRPC: ScrSetPlayerFightingStyle");

	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	if (!pPlayerPool) {
		return;
	}

	CPlayerPed* pPlayerPed = nullptr;

	if (p.playerId == pPlayerPool->GetLocalPlayerID()) {
		pPlayerPed = pPlayerPool->GetLocalPlayer()->GetPlayerPed();
	}
	else {
		CRemotePlayer* pRemotePlayer = pPlayerPool->GetAt(p.playerId);
		if (pRemotePlayer) {
			pPlayerPed = pRemotePlayer->GetPlayerPed();
		}
	}

	if (pPlayerPed) {
		pPlayerPed->SetFightingStyle(p.style);
	}
}

void RpcHandlers::OnSetPlayerVelocity(const proto::SetPlayerVelocity& p)
{
	spdlog::info("ScriptRPC: ScrSetPlayerVelocity");

	CPlayerPed* pPlayerPed = pGame->FindPlayerPed();
	if (!pPlayerPed) {
		return;
	}

	if (pPlayerPed->IsOnGround()) {
		auto flags = pPlayerPed->GetStateFlags();
		flags.bIsStanding = !flags.bIsStanding;
		flags.bWasStanding = !flags.bWasStanding;
		pPlayerPed->SetStateFlags(flags);
	}

	sa::CVector vecVelocity(p.velocity.x, p.velocity.y, p.velocity.z);
	pPlayerPed->m_ped->SetVelocity(vecVelocity);
}

void RpcHandlers::OnSetVehicleVelocity(const proto::SetVehicleVelocity& p)
{
	spdlog::info("ScriptRPC: ScrSetVehicleVelocity");

	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
	if (!pVehiclePool) {
		return;
	}

	CPlayerPed* pPlayerPed = pGame->FindPlayerPed();
	if (!pPlayerPed) {
		return;
	}

	if (pPlayerPed->IsInVehicle()) {
		sa::CVehicle* pGtaVehicle = pPlayerPed->GetGtaVehicle();
		VEHICLEID VehicleID = pVehiclePool->FindIDFromGtaPtr(pGtaVehicle);
		CVehicle* pVehicle = pVehiclePool->GetAt(VehicleID);
		if (!pVehicle || !pVehicle->m_pVehicle || !pVehicle->VerifyInstance()) {
			return;
		}

		sa::CVector vecVelocity(p.velocity.x, p.velocity.y, p.velocity.z);
		if (p.type) {
			if (p.type == 1) {
				pVehicle->m_pVehicle->SetTurnSpeed(vecVelocity);
			}
		}
		else {
			pVehicle->m_pVehicle->SetVelocity(vecVelocity);
		}
	}
}

void RpcHandlers::OnToggleWidescreen(const proto::ToggleWidescreen& p)
{
	spdlog::info("ScriptRPC: ScrToggleWidescreen");
	ScriptCommand(&toggle_widescreen, p.toggle);
}

void RpcHandlers::OnSetVehicleTireDamageStatus(const proto::SetVehicleTireDamageStatus& p)
{
	spdlog::info("ScriptRPC: ScrSetVehicleTireDamageStatus");

	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
	if (!pVehiclePool) {
		return;
	}

	CVehicle* pVehicle = pVehiclePool->GetAt(p.vehicleId);
	if (!pVehicle) {
		return;
	}

	pVehicle->SetTireDamageStatus(p.tireDamageStatus);
}

void RpcHandlers::OnSetPlayerTeam(const proto::SetPlayerTeam& p)
{
	spdlog::info("ScriptRPC: ScrSetPlayerTeam");

	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	if (!pPlayerPool) {
		return;
	}

	CLocalPlayer* pLocalPlayer = pPlayerPool->GetLocalPlayer();
	if (!pLocalPlayer) {
		return;
	}

	if (p.playerId == pPlayerPool->GetLocalPlayerID()) {
		pLocalPlayer->m_byteTeam = p.team;
	}
	else {
		CRemotePlayer* pRemotePlayer = pPlayerPool->GetAt(p.playerId);
		if (pRemotePlayer) {
			pRemotePlayer->SetTeam(p.team);
		}
	}
}

void RpcHandlers::OnSetPlayerName(const proto::SetPlayerName& p)
{
	spdlog::info("ScriptRPC: ScrSetPlayerName");

	if (p.name.size() > MAX_PLAYER_NAME) {
		return;
	}

	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	if (!pPlayerPool) {
		return;
	}

	if (p.success == 1) {
		pPlayerPool->SetPlayerName(p.playerId, p.name.c_str());
	}

	if (pPlayerPool->GetLocalPlayerID() == p.playerId) {
		pPlayerPool->SetLocalPlayerName(p.name.c_str());
	}
}

void RpcHandlers::OnSetPlayerPos(const proto::SetPlayerPos& p)
{
	spdlog::info("ScriptRPC: ScrSetPlayerPos");

	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	if (!pPlayerPool) {
		return;
	}
	CLocalPlayer* pLocalPlayer = pPlayerPool->GetLocalPlayer();
	if (!pLocalPlayer) {
		return;
	}

	pLocalPlayer->DisableSurfing();
	pLocalPlayer->GetPlayerPed()->TeleportTo(p.position.x, p.position.y, p.position.z);
}

void RpcHandlers::OnSetPlayerPosFindZ(const proto::SetPlayerPosFindZ& p)
{
	spdlog::info("ScriptRPC: ScrSetPlayerPosFindZ");

	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	if (!pPlayerPool) {
		return;
	}

	CLocalPlayer* pLocalPlayer = pPlayerPool->GetLocalPlayer();
	if (!pLocalPlayer) {
		return;
	}

	float fZ = pGame->FindGroundZForCoord(p.position.x, p.position.y, p.position.z) + 1.5f;
	pLocalPlayer->DisableSurfing();
	pLocalPlayer->GetPlayerPed()->TeleportTo(p.position.x, p.position.y, fZ);
}

void RpcHandlers::OnPutPlayerInVehicle(const proto::PutPlayerInVehicle& p)
{
	spdlog::info("ScriptRPC: ScrPutPlayerInVehicle");

	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
	if (!pVehiclePool) {
		return;
	}

	uint32_t dwGtaVehicleID = pVehiclePool->FindGtaIDFromID(p.vehicleId);
	CVehicle* pVehicle = pVehiclePool->GetAt(p.vehicleId);
	CPlayerPed* pPlayerPed = pGame->FindPlayerPed();

	if (dwGtaVehicleID != INVALID_VEHICLE_ID && pVehicle && pPlayerPed) {
		pPlayerPed->PutDirectlyInVehicle(dwGtaVehicleID, p.seatId);
	}
}

void RpcHandlers::OnRemovePlayerFromVehicle(const proto::RemovePlayerFromVehicle&)
{
	spdlog::info("ScriptRPC: ScrRemovePlayerFromVehicle");
	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	if (!pPlayerPool) {
		return;
	}

	CLocalPlayer* pLocalPlayer = pPlayerPool->GetLocalPlayer();
	if (!pLocalPlayer || !pLocalPlayer->GetPlayerPed()) {
		return;
	}

	pLocalPlayer->GetPlayerPed()->ExitCurrentVehicle();
}

void RpcHandlers::OnSetPlayerColor(const proto::SetPlayerColor& p)
{
	spdlog::info("ScriptRPC: ScrSetPlayerColor");

	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	if (!pPlayerPool) {
		return;
	}

	if (pPlayerPool->GetLocalPlayerID() == p.playerId) {
		CLocalPlayer* pLocalPlayer = pPlayerPool->GetLocalPlayer();
		if (pLocalPlayer) {
			pLocalPlayer->SetPlayerColor(p.color);
		}
	}
	else {
		CRemotePlayer* pRemotePlayer = pPlayerPool->GetAt(p.playerId);
		if (pRemotePlayer) {
			pRemotePlayer->SetPlayerColor(p.color);
		}
	}
}

void RpcHandlers::OnShowGameText(const proto::ShowGameText& p)
{
	spdlog::info("ScriptRPC: ScrShowGameText");

	if (p.text.size() > 200 || p.style < 0 || p.style > 6) {
		return;
	}

	if (pGame) pGame->DisplayGameText(const_cast<char*>(p.text.c_str()), p.time, p.style);
}

void RpcHandlers::OnSetVehiclePos(const proto::SetVehiclePos& p)
{
	spdlog::info("ScriptRPC: ScrSetVehiclePos");

	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
	if (!pVehiclePool) {
		return;
	}

	CVehicle* pVehicle = pVehiclePool->GetAt(p.vehicleId);
	if (!pVehicle) {
		return;
	}

	pVehicle->TeleportTo(p.position.x, p.position.y, p.position.z);
}

void RpcHandlers::OnSetVehicleZAngle(const proto::SetVehicleZAngle& p)
{
	spdlog::info("ScriptRPC: ScrSetVehicleZAngle");

	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
	if (!pVehiclePool) {
		return;
	}

	CVehicle* pVehicle = pVehiclePool->GetAt(p.vehicleId);
	if (!pVehicle) {
		return;
	}

	pVehicle->SetZAngle(p.angle);
}

void RpcHandlers::OnSetVehicleParamsForPlayer(const proto::SetVehicleParamsForPlayer& p)
{
	spdlog::info("ScriptRPC: ScrSetVehicleParams");
	spdlog::info("VehicleID: {}", p.vehicleId);
	spdlog::info("objective: {}, doors: {}", p.objective, p.doorsLocked);

	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
	if (pVehiclePool) {
		pVehiclePool->AssignSpecialParamsToVehicle(p.vehicleId, p.objective, p.doorsLocked);
	}
}

void RpcHandlers::OnSetCameraBehindPlayer(const proto::SetCameraBehindPlayer&)
{
	spdlog::info("ScriptRPC: ScrSetPlayerCameraBehindPlayer");
	pGame->GetCamera()->SetBehindPlayer();
}

void RpcHandlers::OnTogglePlayerControllable(const proto::TogglePlayerControllable& p)
{
	spdlog::info("ScriptRPC: ScrTogglePlayerControllable");
	pGame->FindPlayerPed()->TogglePlayerControllable(p.controllable);
}

void RpcHandlers::OnPlayerPlaySound(const proto::PlayerPlaySound&)
{
	spdlog::info("ScriptRPC: ScrPlayerPlaySound");
	// TODO: Implement (T06)
}

void RpcHandlers::OnSetWorldBounds(const proto::SetWorldBounds& p)
{
	spdlog::info("ScriptRPC: ScrSetWorldBounds");

	pNetGame->m_netSet->worldBounds[0] = p.bounds[0];
	pNetGame->m_netSet->worldBounds[1] = p.bounds[1];
	pNetGame->m_netSet->worldBounds[2] = p.bounds[2];
	pNetGame->m_netSet->worldBounds[3] = p.bounds[3];
}

void RpcHandlers::OnGivePlayerMoney(const proto::GivePlayerMoney& p)
{
	spdlog::info("ScriptRPC: ScrGivePlayerMoney");
	pGame->AddToLocalMoney(p.money);
}

void RpcHandlers::OnSetPlayerFacingAngle(const proto::SetPlayerFacingAngle& p)
{
	spdlog::info("ScriptRPC: ScrSetPlayerFacingAngle");
	pGame->FindPlayerPed()->SetTargetRotation(p.angle);
}

void RpcHandlers::OnResetPlayerMoney(const proto::ResetPlayerMoney&)
{
	spdlog::info("ScriptRPC: ScrResetPlayerMoney");
	pGame->ResetLocalMoney();
}

void RpcHandlers::OnResetPlayerWeapons(const proto::ResetPlayerWeapons&)
{
	spdlog::info("ScriptRPC: ScrResetPlayerWeapons");
	pNetGame->GetPlayerPool()->GetLocalPlayer()->GetPlayerPed()->ClearWeapons();
}

void RpcHandlers::OnLinkVehicleToInterior(const proto::LinkVehicleToInterior& p)
{
	spdlog::info("ScriptRPC: ScrLinkVehicleToInterior");

	CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
	if (!pVehiclePool) {
		return;
	}

	pVehiclePool->LinkToInterior(p.vehicleId, p.interiorId);
}

void RpcHandlers::OnSetPlayerArmour(const proto::SetPlayerArmour& p)
{
	spdlog::info("ScriptRPC: ScrSetPlayerArmour");

	CLocalPlayer* pLocalPlayer = pNetGame->GetPlayerPool()->GetLocalPlayer();
	pLocalPlayer->GetPlayerPed()->SetArmour(p.armour);
}

void RpcHandlers::OnSetArmedWeapon(const proto::SetArmedWeapon& p)
{
	spdlog::info("ScriptRPC: ScrSetArmedWeapon");
	if (p.weapon <= 46) {
		CPlayerPed* pPlayerPed = pGame->FindPlayerPed();
		if (pPlayerPed) {
			pPlayerPed->SetArmedWeapon(p.weapon);
		}
	}
}

void RpcHandlers::OnSetPlayerAttachedObject(const proto::SetPlayerAttachedObject& p)
{
	spdlog::info("ScriptRPC: ScrSetPlayerAttachedObject");

	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	if (pPlayerPool) {
		CPlayerPed* pPlayerPed = nullptr;

		if (pPlayerPool->GetLocalPlayerID() == p.playerId) {
			if (pPlayerPool->GetLocalPlayer()) {
				pPlayerPed = pPlayerPool->GetLocalPlayer()->GetPlayerPed();
			}
		}
		else {
			CRemotePlayer* pRemotePlayer = pPlayerPool->GetAt(p.playerId);
			if (pRemotePlayer) {
				pPlayerPed = pRemotePlayer->GetPlayerPed();
			}
		}

		if (pPlayerPed) {
			if (p.create) {
				NEW_ATTACHED_OBJECT NewAttachedObject{};
				NewAttachedObject.iModel = p.modelId;
				NewAttachedObject.iBoneID = p.boneId;
				NewAttachedObject.vecOffset = sa::CVector(p.offset.x, p.offset.y, p.offset.z);
				NewAttachedObject.vecRot = sa::CVector(p.rotation.x, p.rotation.y, p.rotation.z);
				NewAttachedObject.vecScale = sa::CVector(p.scale.x, p.scale.y, p.scale.z);
				NewAttachedObject.dwMaterialColor1 = p.materialColor1;
				NewAttachedObject.dwMaterialColor2 = p.materialColor2;
				pPlayerPed->SetAttachedObject(p.index, &NewAttachedObject);
			}
			else {
				pPlayerPed->RemoveAttachedObject(p.index);
			}
		}
	}
}

void RpcHandlers::OnApplyActorAnimation(const proto::ApplyActorAnimation& p)
{
	spdlog::info("ScriptRPC: ScrApplyActorAnimation");

	CActorPool* pActorPool = pNetGame->GetActorPool();
	if (!pActorPool) {
		return;
	}

	CActor* pActor = pActorPool->GetAt(p.actorId);
	if (pActor) {
		pActor->ApplyAnimation(
			const_cast<char*>(p.animName.c_str()),
			const_cast<char*>(p.animLib.c_str()),
			p.delta, p.loop, p.lockX, p.lockY, p.freeze, p.time);
	}
}

void RpcHandlers::OnClearActorAnimation(const proto::ClearActorAnimation& p)
{
	spdlog::info("ScriptRPC: ScrClearActorAnimation");

	CActorPool* pActorPool = pNetGame->GetActorPool();
	if (!pActorPool) {
		return;
	}

	CActor* pActor = pActorPool->GetAt(p.actorId);
	if (pActor) {
		pActor->ClearAnimation();
	}
}

void RpcHandlers::OnSetActorFacingAngle(const proto::SetActorFacingAngle& p)
{
	spdlog::info("ScriptRPC: ScrSetActorFacingAngle");

	CActorPool* pActorPool = pNetGame->GetActorPool();
	if (!pActorPool) {
		return;
	}

	CActor* pActor = pActorPool->GetAt(p.actorId);
	if (pActor) {
		pActor->SetFacingAngle(p.angle);
	}
}

void RpcHandlers::OnSetActorPos(const proto::SetActorPos& p)
{
	spdlog::info("ScriptRPC: ScrSetActorPos");

	CActorPool* pActorPool = pNetGame->GetActorPool();
	if (!pActorPool) {
		return;
	}

	CActor* pActor = pActorPool->GetAt(p.actorId);
	if (pActor) {
		pActor->TeleportTo(p.position.x, p.position.y, p.position.z);
	}
}

void RpcHandlers::OnSetActorHealth(const proto::SetActorHealth& p)
{
	spdlog::info("ScriptRPC: ScrSetActorHealth");

	CActorPool* pActorPool = pNetGame->GetActorPool();
	if (!pActorPool) {
		return;
	}

	CActor* pActor = pActorPool->GetAt(p.actorId);
	if (pActor) {
		pActor->SetHealth(p.health);
	}
}

void RpcHandlers::OnPlayAudioStream(const proto::PlayAudioStream& p)
{
	spdlog::info("ScriptRPC: ScrPlayAudioStream");

	if (pUI && pUI->chat()) {
		pUI->chat()->addInfoMessage("[Audio Stream]: %s", p.url.c_str());
	}

	if (pAudioStream) {
		pAudioStream->Play(p.url.c_str(), p.position.x, p.position.y, p.position.z, p.radius, p.usePos != 0);
	}
}

void RpcHandlers::OnStopAudioStream(const proto::StopAudioStream&)
{
	spdlog::info("ScriptRPC: ScrStopAudioStream");

	if (pAudioStream) {
		pAudioStream->Stop(false);
	}
	// pAudioStream->Stop(true);
}

void RpcHandlers::OnMoveObject(const proto::MoveObject& p)
{
	spdlog::info("ScriptRPC: ScrMoveObject");

	if (!proto::isFinite(p.targetPos) || !proto::isFinite(p.targetRot) || !std::isfinite(p.speed)) {
		spdlog::warn("ScrMoveObject: rejected non-finite params for object {}", p.objectId);
		return;
	}

	CObjectPool* pObjectPool = pNetGame->GetObjectPool();
	if (!pObjectPool) return;

	if (EditObject::m_visible && EditObject::m_type == EditObject::TYPE_OBJECT &&
			EditObject::m_id == (int) p.objectId) {
		return;
	}

	CObject* pObject = pObjectPool->GetAt(p.objectId);
	if (pObject) {
		pObject->MoveTo(p.targetPos.x, p.targetPos.y, p.targetPos.z, p.speed,
			p.targetRot.x, p.targetRot.y, p.targetRot.z);
	}
}

void RpcHandlers::OnStopObject(const proto::StopObject& p)
{
	spdlog::info("ScriptRPC: ScrStopObject");

	CObjectPool* pObjectPool = pNetGame->GetObjectPool();
	if (!pObjectPool) return;

	CObject* pObject = pObjectPool->GetAt(p.objectId);
	if (pObject) {
		pObject->StopMoving();
	}
}

void RpcHandlers::OnSetPlayerDrunkLevel(const proto::SetPlayerDrunkLevel& p)
{
	spdlog::info("ScriptRPC: ScrSetPlayerDrunkLevel");

	CPlayerPed* pPlayerPed = pGame->FindPlayerPed();
	if (pPlayerPed) {
		pPlayerPed->SetDrunkLevel(p.level);
	}
}

void RpcHandlers::OnSetPlayerDrunkVisuals(const proto::SetPlayerDrunkVisuals& p)
{
	spdlog::info("ScriptRPC: ScrSetPlayerDrunkVisuals");
	ScriptCommand(&set_player_drunk_visuals, 0, p.visuals);
}

void RpcHandlers::OnSetPlayerDrunkHandling(const proto::SetPlayerDrunkHandling& p)
{
	spdlog::info("ScriptRPC: ScrSetPlayerDrunkHandling");
	ScriptCommand(&set_player_drunk_handling, 0, p.handling);
}

void RpcHandlers::OnDeathMessage(const proto::DeathMessage& p)
{
	spdlog::info("ScriptRPC: ScrDeathMessage");

	std::string killername, playername;
	unsigned int killercolor = 0, playercolor = 0;

	CPlayerPool* pPlayerPool = pNetGame->GetPlayerPool();
	if (pPlayerPool) {
		if (pPlayerPool->GetLocalPlayerID() == p.playerId) {
			playername = pPlayerPool->GetLocalPlayerName();
			playercolor = pPlayerPool->GetLocalPlayer()->GetPlayerColorAsRGBA();
		}
		else {
			if (pPlayerPool->GetSlotState(p.playerId)) {
				char* szName = pPlayerPool->GetPlayerName(p.playerId);
				if (szName) playername = szName;
				CRemotePlayer* pRemotePlayer = pPlayerPool->GetAt(p.playerId);
				if (pRemotePlayer) playercolor = pRemotePlayer->GetPlayerColor();
			}
		}

		if (pPlayerPool->GetLocalPlayerID() == p.killerId) {
			killername = pPlayerPool->GetLocalPlayerName();
			killercolor = pPlayerPool->GetLocalPlayer()->GetPlayerColorAsRGBA();
		}
		else {
			if (pPlayerPool->GetSlotState(p.killerId)) {
				char* szName = pPlayerPool->GetPlayerName(p.killerId);
				if (szName) killername = szName;
				CRemotePlayer* pRemotePlayer = pPlayerPool->GetAt(p.killerId);
				if (pRemotePlayer) killercolor = pRemotePlayer->GetPlayerColor();
			}
		}
	}

	if (pUI) {
		pUI->deathwindow()->record(playername.c_str(), UI::fixcolor(playercolor), killername.c_str(), killercolor, p.reason);
	}
}

void RpcHandlers::OnDisableVehicleCollisions(const proto::DisableVehicleCollisions& p)
{
	spdlog::info("ScriptRPC: ScrDisableVehicleCollisions");
	pNetGame->m_netSet->disableVehicleCollisions = p.disable;
}
