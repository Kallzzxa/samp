#pragma once

#include <cstdint>
#include <string>
#include <imgui.h>

class ImGuiRenderer;

class CleoEngine {
public:
	static void initialize(const std::string& gameDir);
	static bool isEnabled();

	// Called once per game tick on MainThread (both online & offline)
	static void process();

	// Called from CText::Get hook; returns custom .fxt / dynamic 0ADF GXT wide string or nullptr
	static uint16_t* getCustomGxt(const char* key);

	// Called from AND_TouchEvent_hook (when no modal SAMP dialog/keyboard is open).
	// Returns true if CLEO menu UI consumed the touch event.
	static bool handleTouch(int type, int num, int x, int y, float screenW, float screenH);

	// Called from UI::drawList() to draw built-in CLEO .csi launcher menu or script 0DF2 menu
	static void renderMenu(ImGuiRenderer* renderer, const ImVec2& displaySize);

	// Toggle built-in CLEO .csi script menu (invoked by /cleo chat command)
	static void toggleMenu();
};
