#include "../Game/Game.h"
#include "NetGame.h"
#include "Proto/RpcOutbound.h"

extern Game* pGame;
extern NetGame* pNetGame;

CPickupPool::CPickupPool()
{
	memset(m_Pickups, 0, sizeof(m_Pickups));

	for (int i = 0; i < MAX_PICKUPS; i++) {
		m_dwHnd[i] = 0;
		m_iTimer[i] = 0;
		m_dwGTAId[i] = 0xFFFFFFFF;
	}
}

CPickupPool::~CPickupPool()
{
	for (int i = 0; i < MAX_PICKUPS; i++) {
		if (m_dwHnd[i] != 0) {
			ScriptCommand(&destroy_pickup, m_dwHnd[i]);
		}
	}
}

void CPickupPool::Process()
{
	for (int i = 0; i < MAX_PICKUPS; i++) {
		if (m_dwHnd[i] != 0) {
			if (m_droppedWeapon[i].bDroppedWeapon || m_Pickups[i].iType == 14) {
				if (ScriptCommand(&is_pickup_picked_up, m_dwHnd[i])) {
					RakNet::BitStream bsPickup;
					if (m_droppedWeapon[i].bDroppedWeapon == false) {
						proto::Writer writer(bsPickup);
						proto::PickedUpPickupReq req{};
						req.pickupId = i;
						req.serialize(writer, proto::ProtoVersion::V037);
						pNetGame->GetRakClient()->RPC(&RPC_PickedUpPickup, &bsPickup, HIGH_PRIORITY, RELIABLE_SEQUENCED, 0, false, UNASSIGNED_NETWORK_ID, nullptr);
					}
					else {
						// T23: notify server via RPC 97 (dropped weapon pickup collected)
						proto::Writer writer(bsPickup);
						proto::PickedUpWeaponPickupReq req{};
						req.owner = m_droppedWeapon[i].PlayerID;
						req.serialize(writer, proto::ProtoVersion::V037);
						pNetGame->GetRakClient()->RPC(&RPC_DestroyWeaponPickup, &bsPickup, HIGH_PRIORITY, RELIABLE_SEQUENCED, 0, false, UNASSIGNED_NETWORK_ID, nullptr);
						Destroy(i);
					}
				}
			}
			else if (m_iTimer[i] > 0) {
				m_iTimer[i]--;
			}
		}
	}
}

/**
 * @param pPickup
 * @param iPickup
 */
void CPickupPool::New(PICKUP* pPickup, int iPickup)
{
	if (iPickup < 0 || iPickup >= MAX_PICKUPS) return;

	if (m_dwHnd[iPickup] != 0) {
		ScriptCommand(&destroy_pickup, m_dwHnd[iPickup]);
	}

	memcpy(&m_Pickups[iPickup], pPickup, sizeof(PICKUP));
	m_droppedWeapon[iPickup].bDroppedWeapon = false;

	int dwGTAId = 0;
	m_dwHnd[iPickup] = pGame->CreatePickup(pPickup->iModel, pPickup->iType,
			pPickup->fX, pPickup->fY, pPickup->fZ, &dwGTAId);
	m_dwGTAId[iPickup] = dwGTAId;
}

void CPickupPool::CreateWeapon(int iModel, float fX, float fY, float fZ, int iAmmo, PLAYERID owner)
{
	if (!pGame || iModel <= 0) return;

	// Replace any previous dropped weapon from the same owner
	DestroyWeaponByOwner(owner);

	int slot = -1;
	for (int i = 0; i < MAX_PICKUPS; ++i) {
		if (m_dwHnd[i] == 0) {
			slot = i;
			break;
		}
	}
	if (slot < 0) return;

	uint32_t hnd = pGame->CreateWeaponPickup(iModel, iAmmo, fX, fY, fZ);
	if (hnd == 0) return;

	m_Pickups[slot].iModel = iModel;
	m_Pickups[slot].iType = 4;
	m_Pickups[slot].fX = fX;
	m_Pickups[slot].fY = fY;
	m_Pickups[slot].fZ = fZ;
	m_dwHnd[slot] = hnd;
	m_dwGTAId[slot] = 0xFFFFFFFF;
	m_iTimer[slot] = 0;
	m_droppedWeapon[slot].bDroppedWeapon = true;
	m_droppedWeapon[slot].PlayerID = owner;
}

/**
 * @param iPickup
 */
void CPickupPool::Destroy(int iPickup)
{
	if (iPickup < 0 || iPickup >= MAX_PICKUPS) return;

	if (m_dwHnd[iPickup]) {
		ScriptCommand(&destroy_pickup, m_dwHnd[iPickup]);
		m_dwHnd[iPickup] = 0;
		m_iTimer[iPickup] = 0;
		m_dwGTAId[iPickup] = 0xFFFFFFFF;
		m_droppedWeapon[iPickup].bDroppedWeapon = false;
		m_droppedWeapon[iPickup].PlayerID = INVALID_PLAYER_ID;
	}
}

/**
 * @param owner owner player id sent by ScrDestroyWeaponPickup
 */
void CPickupPool::DestroyWeaponByOwner(PLAYERID owner)
{
	for (int i = 0; i < MAX_PICKUPS; i++) {
		if (m_dwHnd[i] && m_droppedWeapon[i].bDroppedWeapon && m_droppedWeapon[i].PlayerID == owner) {
			Destroy(i);
		}
	}
}

/**
 * @param dwGTAId
 * @return
 */
int CPickupPool::GetIDFromGTAId(int dwGTAId)
{
	for (int i = 0; i < MAX_PICKUPS; i++) {
		if (m_dwGTAId[i] == dwGTAId) {
			return i;
		}
	}

	return -1;
}

/**
 * @param dwGTAId
 */
void CPickupPool::PickedUp(int dwGTAId)
{
	int PickupID = GetIDFromGTAId(dwGTAId);
	if (PickupID != 0xFFFFFFFF && m_dwHnd[PickupID] != 0 && m_iTimer[PickupID] == 0) {
		if (m_droppedWeapon[PickupID].bDroppedWeapon) return;

		RakNet::BitStream bsPickup;
		proto::Writer writer(bsPickup);
		proto::PickedUpPickupReq req{};
		req.pickupId = PickupID;
		req.serialize(writer, proto::ProtoVersion::V037);
		pNetGame->GetRakClient()->RPC(&RPC_PickedUpPickup, &bsPickup, HIGH_PRIORITY, RELIABLE_ORDERED, 0, false, UNASSIGNED_NETWORK_ID, nullptr);
		m_iTimer[PickupID] = 15;
	}
}
