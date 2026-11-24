#pragma once

class ButtonPanel : public Widget {
public:
	ButtonPanel();

	bool extended() { return m_extended; }

	void minimize();
	void maximize();

	void performLayout() override;
	void draw(ImGuiRenderer* renderer) override;

	void applyHeldKeys();
	void handleHardwareKey(int androidKeyCode, bool isDown);
	static void dismissTopModal();

private:
	bool m_extended;

	/* keys currently held down on the panel; re-pinned every frame in
	 * draw() so they survive GetKeys' per-read memset and engine hook
	 * overwrites (spectator-sync class selection reads them) */
	bool m_heldBKeys[32];
	uint16_t m_heldLR;
	uint16_t m_heldUD;

	Button* m_extend;

	Layout* m_rowTop;
	Layout* m_rowMid;
	Layout* m_rowBottom;

	Button* m_esc;
	Button* m_tab;
	Button* m_alt;
	Button* m_spc;
	Button* m_f;
	Button* m_g;
	Button* m_h;
	Button* m_ctrl;
	Button* m_c;
	Button* m_2;
	Button* m_y;
	Button* m_n;
	Button* m_q;
	Button* m_e;
	Button* m_left;
	Button* m_up;
	Button* m_right;
	Button* m_down;
	Button* m_num8;
	Button* m_num4;
	Button* m_num6;
	Button* m_num2;
	Button* m_f8;
	Button* m_hud;

public:
	class PassengerButton : public Image {
	public:
		PassengerButton();

		void draw(ImGuiRenderer* renderer) override;
		void touchPopEvent() override;
	};
};
