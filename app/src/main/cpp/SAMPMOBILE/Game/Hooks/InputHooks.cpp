#include "HookDomains.h"
#include "../Hooks.h"
#include "../Game.h"
#include "../TouchCombat.h"
#include "../TouchWidgets.h"
#include "../CleoEngine.h"
#include "../../UI/UI.h"
#include "../../Net/NetGame.h"
#include "../../Java/Java.h"
#include "../../Client.h"
#include "../../Settings.h"
#include "../../GameSA/include/Enums/WidgetIDs.h"
#include <atomic>

using MonetTouchFilterFn = bool (*)(int type, int num, int posX, int posY);
static std::atomic<MonetTouchFilterFn> g_monetTouchFilter{nullptr};

extern "C" __attribute__((visibility("default")))
void SAMPMOBILE_RegisterTouchFilter(MonetTouchFilterFn fn)
{
	g_monetTouchFilter.store(fn, std::memory_order_release);
}

DECL_HOOK(void, AND_TouchEvent, int type, int num, int posX, int posY)
{
	// Track which pointer slots (0..3) are currently pressed inside libGTASA.so.
	// Previously every intercepted touch called AND_TouchEvent(1, 0, 0, 0),
	// which left fingers 1..3 permanently stuck down whenever they hit an overlay
	// (buttonpanel / TextDraw / Monet / CLEO) or a modal dialog opened — and also
	// spuriously killed finger 0's joystick when finger 1 tapped an overlay.
	static bool s_gtaPointerDown[4] = {false, false, false, false};

	auto forwardToGta = [&](int t, int n, int x, int y) {
		if (n >= 0 && n < 4) {
			if (t == 2) s_gtaPointerDown[n] = true;
			else if (t == 1) s_gtaPointerDown[n] = false;
		}
		AND_TouchEvent(t, n, x, y);
	};

	auto releaseGtaPointer = [&](int n) {
		if (n >= 0 && n < 4 && s_gtaPointerDown[n]) {
			s_gtaPointerDown[n] = false;
			AND_TouchEvent(1, n, 0, 0);
		}
	};

	auto releaseAllGtaPointers = [&]() {
		for (int i = 0; i < 4; ++i) {
			if (s_gtaPointerDown[i]) {
				s_gtaPointerDown[i] = false;
				AND_TouchEvent(1, i, 0, 0);
			}
		}
	};

	if (Game::IsGamePaused())
	{
		forwardToGta(type, num, posX, posY);
		return;
	}

	const bool javaDialogOpen = Java::isJavaDialogVisible();
	const bool modalUiOpen = javaDialogOpen || (pUI != nullptr && pUI->isModalOpenSnapshot());
	if (javaDialogOpen)
	{
		if (pUI != nullptr) {
			pUI->enqueueTouchEvent(ImVec2(0.0f, 0.0f), TouchType::pop, true);
		}
		releaseAllGtaPointers();
		return;
	}

	if (!modalUiOpen)
	{
		ImVec2 disp = pUI ? pUI->displaySizeSnapshot() : ImVec2(1920.0f, 1080.0f);
		if (CleoEngine::handleTouch(type, num, posX, posY, disp.x, disp.y))
		{
			releaseGtaPointer(num);
			return;
		}
		if (auto touchFilter = g_monetTouchFilter.load(std::memory_order_acquire))
		{
			if (touchFilter(type, num, posX, posY))
			{
				releaseGtaPointer(num);
				return;
			}
		}
	}

	if (pUI != nullptr)
	{
		const bool tdSelecting = !pUI->isModalOpenSnapshot()
			&& pNetGame && pNetGame->GetTextDrawPool() && pNetGame->GetTextDrawPool()->GetState();
		const bool hitBtnPanel = pUI->isButtonPanelHitSnapshot(ImVec2(posX, posY));

		// Give selectable TextDraws priority over background ImGui widgets (chat/etc.)
		// while still letting the player tap ButtonPanel (e.g. ESC) to cancel selection.
		if (tdSelecting && !hitBtnPanel)
		{
			if (!pNetGame->GetTextDrawPool()->enqueueTouchEvent(type, posX, posY))
			{
				releaseGtaPointer(num);
				return;
			}
		}

		switch (type)
		{
		case 2: // push
			pUI->enqueueTouchEvent(ImVec2(posX, posY), TouchType::push, false, num);
			break;

		case 3: // move
			pUI->enqueueTouchEvent(ImVec2(posX, posY), TouchType::move, false, num);
			break;

		case 1: // pop
			pUI->enqueueTouchEvent(ImVec2(posX, posY), TouchType::pop, false, num);
			break;
		}

		if (pUI->isModalOpenSnapshot())
		{
			releaseAllGtaPointers();
			return;
		}
		else
		{
			if (pNetGame && pNetGame->GetMenuPool())
			{
				if (!pNetGame->GetMenuPool()->enqueueTouchEvent(type, posX, posY))
				{
					releaseGtaPointer(num);
					return;
				}
			}

			if (hitBtnPanel)
			{
				releaseGtaPointer(num);
				return;
			}

			if (pNetGame && pNetGame->GetTextDrawPool())
			{
				if (!pNetGame->GetTextDrawPool()->enqueueTouchEvent(type, posX, posY))
				{
					releaseGtaPointer(num);
					return;
				}
			}

			if (pNetGame && pNetGame->GetObjectPool())
			{
				if (!pNetGame->GetObjectPool()->enqueueTouchEvent(type, posX, posY))
				{
					releaseGtaPointer(num);
					return;
				}
			}
		}
	}

	if (pGame && pGame->IsGameInputEnabled())
	{
		forwardToGta(type, num, posX, posY);
	}
	else
	{
		releaseAllGtaPointers();
	}
}

DECL_HOOK(int, GetInputType)
{
	return 0;
}

// engine rewrites targeting widgets back onto the shared attack posIdx on
// every reposition — reapply our split layout immediately afterwards or the
// aim icon lands on the fire button and eats its taps
DECL_HOOK(void, TI_RepositionAdjustableWidgets)
{
	TI_RepositionAdjustableWidgets();
	FixTouchWidgetLayout();
}

// No HID device may claim to natively provide a control: DrawAll hides a
// touch widget whenever Replaces(mapping) is true, and on this client the
// keyboard-class device claims every button mapping (emulator / physical
// keyboard present) which suppressed all on-screen buttons and the stick.
DECL_HOOK(int, CHID_Replaces, uint32_t mapping)
{
	return 0;
}

// No HID device may implement a touch-widget mapping either: CWidget::IsTouched
// / CWidget::Update treat Implements(mapping) && IsPressed(mapping) as "widget
// held", and the emulator's virtual input device reports pressed for attack
// (mapping 1) and the move axes (30/31) whenever a finger is down, causing
// punch-while-walking. With Implements==0 every widget falls back to the real
// finger-claim path. Consistent with Replaces/GetInputType being forced to 0.
DECL_HOOK(int, CHID_Implements, uint32_t mapping)
{
	return 0;
}

DECL_HOOK(void, CWidget_Update, uintptr_t pWidget)
{
	if (pNetGame)
	{
		switch (ProcessFixedWidget(pWidget))
		{
		case STATE_NONE:
			break;
		case STATE_DISABLED:
			return;
		}
	}
	RunWithUnscaledJoystickSx(pWidget, CWidget_Update);
}

std::atomic<bool> g_suppressWidgetSetEnabled{false};

DECL_HOOK(void, CWidget_SetEnabled, uintptr_t pWidget, bool bEnabled)
{
	if (g_suppressWidgetSetEnabled.load(std::memory_order_acquire))
	{
		return;
	}
	if (pNetGame)
	{
		switch (ProcessFixedWidget(pWidget))
		{
		case STATE_NONE:
			break;
		case STATE_DISABLED:
			bEnabled = false;
			break;
		}
	}
	CWidget_SetEnabled(pWidget, bEnabled);
}

void Hooks::installWidgetFixHooks()
{
	spdlog::info("Installing widget fix hooks...");

	GetTouchWidgets();

	// HOOK("_ZN7CWidgetC2EPKcRK14WidgetPositionj10HIDMapping", CWidget);
	REG_HOOK(Boot, CWidget_Update, CWidget_Update, "Touch widget layout & visibility fix");
	REG_HOOK(Boot, CWidget_SetEnabled, CWidget_SetEnabled, "Suppress widget disable during remote ped control");
}

void InstallInputHooks()
{
	REG_HOOK(Boot, AND_TouchEvent, AND_TouchEvent, "Touch routing (UI/CLEO/Monet/TextDraw)");
	REG_HOOK(Boot, CHID_GetInputType, GetInputType, "Keyboard/controller input type override");
	REG_HOOK(Boot, CHID_Replaces, CHID_Replaces, "HID mapping replaces override");
	REG_HOOK(Boot, CHID_Implements, CHID_Implements, "HID mapping implements override");
	REG_HOOK(Boot, CTouchInterface_RepositionAdjustableWidgets, TI_RepositionAdjustableWidgets, "Preserve custom buttons on HUD adjust");
}
