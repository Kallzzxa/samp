#pragma once

#include <atomic>
#include "../../Game/Game.h"
#include "../../Game/Util.h"

#include "../../Settings.h"

class VoiceButton : public Button {
public:
	VoiceButton()
			: Button("VOICE", UISettings::fontSize() / 2)
	{
		m_recording.store(false, std::memory_order_relaxed);
		m_texture_micro_on = LoadTextureFromTxd("samp", "voiceactive");
		m_texture_micro_off = LoadTextureFromTxd("samp", "voicepassive");
	}

	virtual void draw(ImGuiRenderer* renderer) override
	{
		if (!Settings::voice() || !Settings::voiceMicEnable()) {
			if (m_recording.load(std::memory_order_relaxed)) {
				setRecording(false);
			}
			return;
		}

		if (m_texture_micro_on && m_texture_micro_on->raster && m_texture_micro_off && m_texture_micro_off->raster) {
			renderer->drawImage(absolutePosition(), absolutePosition() + size(), recording() ? m_texture_micro_on->raster : m_texture_micro_off->raster);
		}
		else {
			Button::draw(renderer);
		}
	}

	void touchPopEvent() override
	{
		if (Settings::voiceMicMode() == 0) {
			setRecording(false);
		}
	}

	void focuseEvent(bool focused) override
	{
		if (!focused && Settings::voiceMicMode() == 0) {
			setRecording(false);
		}
	}

	void touchPushEvent() override
	{
		if (!Settings::voice() || !Settings::voiceMicEnable()) {
			setRecording(false);
			return;
		}
		if (Settings::voiceMicMode() == 1) {
			setRecording(!m_recording.load(std::memory_order_relaxed));
		}
		else {
			setRecording(true);
		}
	}

	void setRecording(bool recording)
	{
		m_recording.store(recording, std::memory_order_release);
		this->setCaptionColor(recording ? ImColor(1.0f, 0.0f, 0.0f) : ImColor(1.0f, 1.0f, 1.0f));
	}

	[[nodiscard]] bool recording() const { return m_recording.load(std::memory_order_acquire); }

private:
	std::atomic<bool> m_recording{false};
	RwTexture* m_texture_micro_on;
	RwTexture* m_texture_micro_off;
};
