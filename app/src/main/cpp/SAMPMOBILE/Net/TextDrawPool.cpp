#include "../Game/Game.h"
#include "NetGame.h"
#include "Proto/RpcOutbound.h"

extern Game* pGame;
extern NetGame* pNetGame;

CTextDrawPool::CTextDrawPool()
{
	for (int i = 0; i < MAX_TEXT_DRAWS; i++) {
		m_pTextDraw[i] = nullptr;
		m_bSlotState[i] = false;
	}

	ResetTextDrawTextures();

	m_bSelectState = false;
	m_dwHoverColor = 0;
	m_wClickedTextDrawID = 0xFFFF;
}

CTextDrawPool::~CTextDrawPool()
{
	for (int i = 0; i < MAX_TEXT_DRAWS; i++) {
		Delete(i);
	}
}

/**
 * @param wTextDrawID
 * @param pTextDrawTransmit
 * @param szText
 */
void CTextDrawPool::New(uint16_t wTextDrawID, TEXT_DRAW_TRANSMIT* pTextDrawTransmit, const char* szText)
{
	if (wTextDrawID >= MAX_TEXT_DRAWS) return;

	if (m_pTextDraw[wTextDrawID]) {
		Delete(wTextDrawID);
	}

	CTextDraw* pTextDraw = new CTextDraw(pTextDrawTransmit, szText);
	if (pTextDraw == nullptr) return;

	m_pTextDraw[wTextDrawID] = pTextDraw;
	m_bSlotState[wTextDrawID] = true;
}

/**
 * @param wTextDrawID
 */
void CTextDrawPool::Delete(uint16_t wTextDrawID)
{
	if (wTextDrawID >= MAX_TEXT_DRAWS) return;

	if (m_pTextDraw[wTextDrawID]) {
		delete m_pTextDraw[wTextDrawID];
		m_pTextDraw[wTextDrawID] = nullptr;
		m_bSlotState[wTextDrawID] = false;
	}
}

void CTextDrawPool::Draw()
{
	for (int i = 0; i < MAX_TEXT_DRAWS; i++) {
		if (m_bSlotState[i]) {
			m_pTextDraw[i]->Draw();
		}
	}
}

void CTextDrawPool::DrawImGui(ImGuiRenderer* renderer)
{
	for (int i = 0; i < MAX_TEXT_DRAWS; i++) {
		if (m_bSlotState[i]) {
			m_pTextDraw[i]->DrawImGui(renderer);
		}
	}
}

void CTextDrawPool::SetSelectState(bool bState, uint32_t dwColor, bool bSendCancel)
{
	if (bState) {
		m_bSelectState.store(true, std::memory_order_release);
		// SelectTextDraw RPC transmits hover color in RGBA (0xRRGGBBAA), while
		// CTextDraw::Draw / DrawImGui / DrawTextureUV consume ABGR (0xAABBGGRR).
		m_dwHoverColor = __builtin_bswap32(dwColor);
		m_wClickedTextDrawID = 0xFFFF;
		//pGame->DisplayHUD(false);
		//pGame->FindPlayerPed()->TogglePlayerControllable(false);
	}
	else {
		const bool wasSelecting = m_bSelectState.exchange(false, std::memory_order_acq_rel);
		m_dwHoverColor = 0;
		m_wClickedTextDrawID = 0xFFFF;
		{
			std::lock_guard<std::mutex> lock(m_touchMutex);
			m_pendingTouches.clear();
		}
		//pGame->DisplayHUD(true);
		//pGame->FindPlayerPed()->TogglePlayerControllable(true);

		if (wasSelecting && bSendCancel) {
			SendClick();
		}

		for (int i = 0; i < MAX_TEXT_DRAWS; i++) {
			if (m_bSlotState[i] && m_pTextDraw[i]) {
				CTextDraw* pTextDraw = m_pTextDraw[i];
				pTextDraw->m_bHovered = false;
				pTextDraw->m_dwHoverColor = 0;
			}
		}
	}
}

void CTextDrawPool::SendClick()
{
	if (!pNetGame || !pNetGame->GetRakClient()) return;
	RakNet::BitStream bsClick;
	proto::Writer writer(bsClick);
	proto::ClickTextDrawReq req{};
	req.textDrawId = m_wClickedTextDrawID;
	req.serialize(writer, proto::ProtoVersion::V037);
	pNetGame->GetRakClient()->RPC(&RPC_ClickTextDraw, &bsClick, HIGH_PRIORITY, RELIABLE_ORDERED, 0, false, UNASSIGNED_NETWORK_ID, nullptr);
}

bool CTextDrawPool::enqueueTouchEvent(int type, int x, int y)
{
	if (!m_bSelectState.load(std::memory_order_acquire)) return true;

	std::lock_guard<std::mutex> lock(m_touchMutex);
	if (m_pendingTouches.size() >= 64) {
		m_pendingTouches.pop_front();
	}
	m_pendingTouches.push_back({type, x, y});
	return false;
}

void CTextDrawPool::processPendingTouches()
{
	std::deque<PendingTouch> queue;
	{
		std::lock_guard<std::mutex> lock(m_touchMutex);
		if (m_pendingTouches.empty()) return;
		queue.swap(m_pendingTouches);
	}
	for (const auto& ev : queue) {
		onTouchEvent(ev.type, ev.x, ev.y);
	}
}

bool CTextDrawPool::onTouchEvent(int type, int x, int y)
{
	if (!m_bSelectState.load(std::memory_order_acquire)) return true;

	m_wClickedTextDrawID = 0xFFFF;

	for (int i = 0; i < MAX_TEXT_DRAWS; i++) {
		if (m_bSlotState[i] && m_pTextDraw[i]) {
			CTextDraw* pTextDraw = m_pTextDraw[i];
			pTextDraw->m_bHovered = false;
			pTextDraw->m_dwHoverColor = 0;

			if (pTextDraw->m_TextDrawData.byteSelectable && pTextDraw->m_TextDrawData.bHasRectArea) {
				sa::CRect* rect = &pTextDraw->m_rectArea;
				if (rect) {
					if (IsPointInRect(x, y, rect)) {
						switch (type) {
							case 2:
							case 3: m_wClickedTextDrawID = 0xFFFF;
								pTextDraw->m_bHovered = true;
								pTextDraw->m_dwHoverColor = m_dwHoverColor;
								break;

							case 1: m_wClickedTextDrawID = i;
								pTextDraw->m_bHovered = false;
								pTextDraw->m_dwHoverColor = 0;
								break;
						}
					}
				}
			}
		}
	}

	if (m_wClickedTextDrawID != 0xFFFF) {
		SendClick();
	}

	return false;
}

void CTextDrawPool::SnapshotProcess()
{
	for (int i = 0; i < MAX_TEXT_DRAWS; i++) {
		if (m_bSlotState[i] && m_pTextDraw[i]) {
			m_pTextDraw[i]->SnapshotProcess();
		}
	}
}
