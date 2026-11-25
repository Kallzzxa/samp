//
// Created by ALYN on 2024/8/25.
//

#ifndef SA_MP_MOBILE_JAVA_H
#define SA_MP_MOBILE_JAVA_H

#include <jni.h>

class Java {
public:
	Java(JNIEnv* env, jobject sampObj, jobject uiObj);
	JNIEnv* getEnv();

	// Drain JNI input callbacks (chat send, dialog response, hardware keys, Back dismiss)
	// safely on the GameMain thread inside Client::process().
	static void processPendingInputs();

	void setPauseState(bool pause);

	void showLoadingScreen(bool show);
	void showKeyboard(bool show);
	void showDialog(int dialog_id, int dialog_style, char* title, char* message, char* button1, char* button2);
	void hideDialog();
	void showEditObject(bool show);

	void showWantedLevel(bool show);
	void setWantedLevel(int level);

	void exitGame();

	jobject sampActivity() { return m_sampActivity; }
	jobject uiActivity() { return m_uiActivity; }

private:
	jobject m_sampActivity = nullptr;
	jobject m_uiActivity = nullptr;

	jmethodID m_setPauseState = nullptr;

	jmethodID m_showLoadingScreen = nullptr;
	jmethodID m_showKeyboard = nullptr;
	jmethodID m_showDialog = nullptr;
	jmethodID m_hideDialog = nullptr;
	jmethodID m_showEditObject = nullptr;

	jmethodID m_showWantedLevel = nullptr;
	jmethodID m_setWantedLevel = nullptr;

	jmethodID m_exitGame = nullptr;

	int m_cachedWantedVisible = -1;
	int m_cachedWantedLevel = -1;
};

extern Java* g_java;

#endif //SA_MP_MOBILE_JAVA_H
