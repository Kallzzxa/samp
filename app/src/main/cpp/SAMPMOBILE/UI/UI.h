#pragma once

#define DEBUG_GUI 0

#include <atomic>
#include <deque>
#include <mutex>
#include <encoding.h>

#include "../Settings.h"
#include "ImGuiWrapper.h"
#include "UISettings.h"

#include "Widget.h"
#include "Widgets/Layout.h"
#include "Widgets/Label.h"
#include "Widgets/Button.h"
#include "Widgets/Image.h"
#include "Widgets/ProgressBar.h"
#include "Widgets/ScrollPanel.h"
#include "Widgets/ListBox.h"
#include "Widgets/EditBox.h"

#include "SAMPWidgets/PlayerTags.h"
#include "SAMPWidgets/Keyboard.h"
#include "SAMPWidgets/SplashScreen.h"
#include "SAMPWidgets/Chat.h"
#include "SAMPWidgets/Spawn.h"
#include "SAMPWidgets/ButtonPanel.h"
#include "SAMPWidgets/VoiceButton.h"
#include "SAMPWidgets/Scoreboard.h"
#include "SAMPWidgets/DebugLabels.h"
#include "SAMPWidgets/DeathWindow.h"

#include "SAMPWidgets/Dialogs//Dialog.h"

class UI : public Widget, public ImGuiWrapper {
public:
	UI(const ImVec2& display_size, const std::string& fonts_path);

	bool initialize() override;
	void render() override;
	void shutdown() override;

	SplashScreen* splashscreen() const { return m_splashScreen; }
	Chat* chat() const { return m_chat; }
	Spawn* spawn() const { return m_spawn; }
	Keyboard* keyboard() const { return m_keyboard; }
	Dialog* dialog() const { return m_dialog; }
	ButtonPanel* buttonpanel() const { return m_buttonPanel; }
	Scoreboard* scoreboard() const { return m_scoreboard; }
	VoiceButton* voicebutton() const { return m_voiceButton; }
	DebugLabels* debuglabels() const { return m_debugLabels; }
	DeathWindow* deathwindow() const { return m_deathWindow; }

	static const ImColor fixcolor(uint32_t color)
	{
		return ImColor(
				(int) ((color & 0xFF000000) >> 24),
				(int) ((color & 0x00FF0000) >> 16),
				(int) ((color & 0x0000FF00) >> 8));
	}

	float ScaleX(float x)
	{
		return x * displaySize().x * (1.0f / 1920.0f);
	}

	float ScaleY(float y)
	{
		return y * displaySize().y * (1.0f / 1080.0f);
	}

	float GetFontSize()
	{
		return UISettings::fontSize();
	}

	virtual void touchEvent(const ImVec2& pos, TouchType type) override;

	// Thread-safe touch bridge for NVEvent thread -> GameMain thread
	void enqueueTouchEvent(const ImVec2& pos, TouchType type, bool isCancel = false);
	void processPendingTouchEvents();
	void updateTouchSnapshot();
	bool isModalOpenSnapshot() const { return m_snapModalOpen.load(std::memory_order_acquire); }
	bool isButtonPanelHitSnapshot(const ImVec2& pos) const;
	ImVec2 displaySizeSnapshot() const;

protected:
	void drawList() override;

private:
	struct PendingTouch {
		ImVec2 pos;
		TouchType type;
		bool isCancel;
	};

	Keyboard* m_keyboard = nullptr;
	SplashScreen* m_splashScreen = nullptr;
	Chat* m_chat = nullptr;
	Spawn* m_spawn = nullptr;
	Dialog* m_dialog = nullptr;
	ButtonPanel* m_buttonPanel = nullptr;
	Scoreboard* m_scoreboard = nullptr;
	VoiceButton* m_voiceButton = nullptr;
	DebugLabels* m_debugLabels = nullptr;
	DeathWindow* m_deathWindow = nullptr;

	mutable std::mutex m_touchMutex;
	std::deque<PendingTouch> m_pendingTouches;
	std::atomic<bool> m_snapModalOpen{false};
	bool m_snapButtonPanelVisible = false;
	ImVec2 m_snapButtonPanelPos{0.0f, 0.0f};
	ImVec2 m_snapButtonPanelSize{0.0f, 0.0f};
	ImVec2 m_snapDisplaySize{1920.0f, 1080.0f};

public:
	bool m_needClearMousePos = false;
};
