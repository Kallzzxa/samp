//
// Created by ALYN on 2024/8/25.
//
#include "Java.h"
#include "../../Client.h"
#include "../../UI/UI.h"
#include "../../Game/Game.h"
#include "../../Net/NetGame.h"

#include <chrono>
#include <deque>
#include <mutex>
#include <string>

extern UI* pUI;
extern NetGame* pNetGame;

namespace {
struct PendingDialogResp {
	int dialogId;
	int buttonId;
	int listItem;
	std::string text;
};

struct PendingHardwareKey {
	int keyCode;
	bool isDown;
};

std::mutex g_jniInputMutex;
std::deque<std::string> g_pendingKeyboardSends;
std::deque<PendingDialogResp> g_pendingDialogResponses;
std::deque<PendingHardwareKey> g_pendingHardwareKeys;
std::atomic<bool> g_javaDialogVisible{false};
std::atomic<bool> g_javaKeyboardVisible{false};
std::atomic<int64_t> g_javaKeyboardClosedMs{0};

int64_t nowSteadyMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline void clearJniException(JNIEnv* env, const char* where)
{
	if (env && env->ExceptionCheck()) {
		spdlog::error("JNI exception in {}", where);
		env->ExceptionDescribe();
		env->ExceptionClear();
	}
}
}

bool Java::isJavaDialogVisible()
{
	return g_javaDialogVisible.load(std::memory_order_acquire);
}

bool Java::isJavaKeyboardVisible()
{
	return g_javaKeyboardVisible.load(std::memory_order_acquire);
}

bool Java::isRadarSuppressedByKeyboard()
{
	if (g_javaKeyboardVisible.load(std::memory_order_acquire)) {
		return true;
	}
	int64_t closedAt = g_javaKeyboardClosedMs.load(std::memory_order_acquire);
	if (closedAt > 0) {
		int64_t delta = nowSteadyMs() - closedAt;
		if (delta >= 0 && delta < 250) {
			return true;
		}
	}
	return false;
}

void Java::processPendingInputs()
{
	std::deque<std::string> kbQueue;
	std::deque<PendingDialogResp> dlgQueue;
	std::deque<PendingHardwareKey> keyQueue;
	{
		std::lock_guard<std::mutex> lock(g_jniInputMutex);
		kbQueue.swap(g_pendingKeyboardSends);
		dlgQueue.swap(g_pendingDialogResponses);
		keyQueue.swap(g_pendingHardwareKeys);
	}

	if (pUI && pUI->chat()) {
		for (const auto& s : kbQueue) {
			pUI->chat()->keyboardEvent(s);
		}
	}

	if (pNetGame) {
		for (auto& d : dlgQueue) {
			pNetGame->SendDialogResponse(d.dialogId, d.buttonId, d.listItem, const_cast<char*>(d.text.c_str()));
		}
	}

	if (pUI && pUI->buttonpanel()) {
		for (const auto& k : keyQueue) {
			pUI->buttonpanel()->handleHardwareKey(k.keyCode, k.isDown);
		}
	}
}

Java::Java(JNIEnv* env, jobject sampObj, jobject uiObj)
{
	spdlog::info("Initializing java..");

	if (!env || !sampObj || !uiObj) {
		spdlog::error("Invalid java objects");
		return;
	}

	m_sampActivity = env->NewGlobalRef(sampObj);
	m_uiActivity = env->NewGlobalRef(uiObj);

	jclass sampClass = env->GetObjectClass(sampObj);
	if (!sampClass) {
		clearJniException(env, "Java::Java(sampClass)");
		spdlog::error("SAMP class not found");
		return;
	}

	jclass uiClass = env->GetObjectClass(uiObj);
	if (!uiClass) {
		env->DeleteLocalRef(sampClass);
		clearJniException(env, "Java::Java(uiClass)");
		spdlog::error("UI class not found");
		return;
	}

	m_setPauseState = env->GetMethodID(uiClass, "setPauseState", "(Z)V");
	clearJniException(env, "GetMethodID(setPauseState)");

	m_showLoadingScreen = env->GetMethodID(uiClass, "showLoadingScreen", "(Z)V");
	clearJniException(env, "GetMethodID(showLoadingScreen)");
	m_showKeyboard = env->GetMethodID(uiClass, "showKeyboard", "(Z)V");
	clearJniException(env, "GetMethodID(showKeyboard)");
	m_showDialog = env->GetMethodID(uiClass, "showDialog", "(IILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
	clearJniException(env, "GetMethodID(showDialog)");
	m_hideDialog = env->GetMethodID(uiClass, "hideDialog", "()V");
	clearJniException(env, "GetMethodID(hideDialog)");
	m_showEditObject = env->GetMethodID(uiClass, "showEditObject", "(Z)V");
	clearJniException(env, "GetMethodID(showEditObject)");

	m_showWantedLevel = env->GetMethodID(uiClass, "showWantedLevel", "(Z)V");
	clearJniException(env, "GetMethodID(showWantedLevel)");
	m_setWantedLevel = env->GetMethodID(uiClass, "setWantedLevel", "(I)V");
	clearJniException(env, "GetMethodID(setWantedLevel)");

	m_exitGame = env->GetMethodID(uiClass, "exitGame", "()V");
	clearJniException(env, "GetMethodID(exitGame)");

	env->DeleteLocalRef(sampClass);
	env->DeleteLocalRef(uiClass);
}

JNIEnv* Java::getEnv()
{
	JavaVM* vm = Client::javaVM();
	if (!vm) {
		spdlog::error("No java vm");
		return nullptr;
	}

	JNIEnv* env = nullptr;
	jint status = vm->GetEnv((void**) &env, JNI_VERSION_1_6);
	if (status == JNI_EDETACHED) {
		// Native threads (e.g. the GTA game thread) may not be attached to the
		// JVM — attach on first use instead of silently dropping every call.
		if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
			spdlog::error("Failed to attach thread to JVM");
			return nullptr;
		}
	}
	else if (status != JNI_OK) {
		spdlog::error("Failed to get env");
		return nullptr;
	}

	return env;
}

jstring createJString(JNIEnv* env, const char* text)
{
	if (!env) return nullptr;
	const char* safeText = text ? text : "";
	const size_t len = strlen(safeText);

	jclass strClass = env->FindClass("java/lang/String");
	if (!strClass) {
		clearJniException(env, "createJString(FindClass)");
		return nullptr;
	}
	jmethodID ctorID = env->GetMethodID(strClass, "<init>", "([BLjava/lang/String;)V");
	if (!ctorID) {
		env->DeleteLocalRef(strClass);
		clearJniException(env, "createJString(GetMethodID)");
		return nullptr;
	}
	jstring encoding = env->NewStringUTF("UTF-8");
	jbyteArray bytes = env->NewByteArray(static_cast<jsize>(len));
	jstring result = nullptr;
	if (encoding && bytes) {
		if (len > 0) {
			env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(len), reinterpret_cast<const jbyte*>(safeText));
		}
		result = reinterpret_cast<jstring>(env->NewObject(strClass, ctorID, bytes, encoding));
		clearJniException(env, "createJString(NewObject)");
	}
	if (bytes) env->DeleteLocalRef(bytes);
	if (encoding) env->DeleteLocalRef(encoding);
	env->DeleteLocalRef(strClass);
	return result;
}

void Java::setPauseState(bool pause)
{
	JNIEnv* env = getEnv();
	if (!env || !m_uiActivity || !m_setPauseState) {
		return;
	}
	env->CallVoidMethod(m_uiActivity, m_setPauseState, static_cast<jboolean>(pause));
	clearJniException(env, "setPauseState");
}

void Java::showLoadingScreen(bool show)
{
	JNIEnv* env = getEnv();
	if (!env || !m_uiActivity || !m_showLoadingScreen) {
		return;
	}
	env->CallVoidMethod(m_uiActivity, m_showLoadingScreen, static_cast<jboolean>(show));
	clearJniException(env, "showLoadingScreen");
}

void Java::showKeyboard(bool show)
{
	JNIEnv* env = getEnv();
	if (!env || !m_uiActivity || !m_showKeyboard) {
		return;
	}
	env->CallVoidMethod(m_uiActivity, m_showKeyboard, static_cast<jboolean>(show));
	clearJniException(env, "showKeyboard");
}

void Java::showDialog(int dialog_id, int dialog_style, char* title, char* message, char* button1, char* button2)
{
	JNIEnv* env = getEnv();
	if (!env || !m_uiActivity || !m_showDialog) {
		return;
	}

	jstring jtitle = createJString(env, title);
	jstring jmessage = createJString(env, message);
	jstring jbutton1 = createJString(env, button1);
	jstring jbutton2 = createJString(env, button2);

	env->CallVoidMethod(m_uiActivity, m_showDialog, dialog_id, dialog_style, jtitle, jmessage, jbutton1, jbutton2);
	clearJniException(env, "showDialog");

	if (jtitle) env->DeleteLocalRef(jtitle);
	if (jmessage) env->DeleteLocalRef(jmessage);
	if (jbutton1) env->DeleteLocalRef(jbutton1);
	if (jbutton2) env->DeleteLocalRef(jbutton2);
}

void Java::hideDialog()
{
	JNIEnv* env = getEnv();
	if (!env || !m_uiActivity || !m_hideDialog) {
		return;
	}
	env->CallVoidMethod(m_uiActivity, m_hideDialog);
	clearJniException(env, "hideDialog");
}

void Java::showEditObject(bool show)
{
	JNIEnv* env = getEnv();
	if (!env || !m_uiActivity || !m_showEditObject) {
		return;
	}
	env->CallVoidMethod(m_uiActivity, m_showEditObject, static_cast<jboolean>(show));
	clearJniException(env, "showEditObject");
}

void Java::showWantedLevel(bool show)
{
	JNIEnv* env = getEnv();
	if (!env || !m_uiActivity || !m_showWantedLevel) {
		return;
	}
	env->CallVoidMethod(m_uiActivity, m_showWantedLevel, static_cast<jboolean>(show));
	clearJniException(env, "showWantedLevel");
}

void Java::setWantedLevel(int level)
{
	JNIEnv* env = getEnv();
	if (!env || !m_uiActivity || !m_setWantedLevel) {
		return;
	}
	env->CallVoidMethod(m_uiActivity, m_setWantedLevel, level);
	clearJniException(env, "setWantedLevel");
}

void Java::exitGame()
{
	JNIEnv* env = getEnv();
	if (!env || !m_uiActivity || !m_exitGame) {
		return;
	}
	env->CallVoidMethod(m_uiActivity, m_exitGame);
	clearJniException(env, "exitGame");
}


const std::string EXPECTED_SIGNATURE(OBFUSCATE("89cb277c5cd1a90145acb3c2a89cea995bfbd3b3e5ebe2b180d2248efc4aa041"));

extern "C" JNIEXPORT jboolean JNICALL Java_com_sampmobile_launcher_utils_SignatureChecker_nativeCheckSignature(JNIEnv* env, jclass clazz, jstring computedSignature)
{
	if (!env || !computedSignature) return JNI_TRUE;
	const char* computedSignatureCStr = env->GetStringUTFChars(computedSignature, nullptr);
	if (computedSignatureCStr) {
		std::string computedSignatureStr(computedSignatureCStr);
		env->ReleaseStringUTFChars(computedSignature, computedSignatureCStr);
	}
	return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL Java_com_sampmobile_game_SAMP_initializeSAMP(JNIEnv* env, jobject sampObj, jobject uiObj, jstring gameDir, jboolean isOffline)
{
	if (!env || !gameDir) return;
	const char* gameDirCStr = env->GetStringUTFChars(gameDir, nullptr);
	if (!gameDirCStr) return;
	std::string gameDirStr(gameDirCStr);
	env->ReleaseStringUTFChars(gameDir, gameDirCStr);

	Client::initialize(gameDirStr, isOffline == JNI_TRUE);
	g_java = new Java(env, sampObj, uiObj);
}

extern "C" JNIEXPORT void JNICALL Java_com_sampmobile_game_ui_UI_setJavaDialogVisible(JNIEnv* env, jobject obj, jboolean visible)
{
	g_javaDialogVisible.store(visible == JNI_TRUE, std::memory_order_release);
}

extern "C" JNIEXPORT void JNICALL Java_com_sampmobile_game_ui_UI_setJavaKeyboardVisible(JNIEnv* env, jobject obj, jboolean visible)
{
	bool isVis = (visible == JNI_TRUE);
	bool wasVis = g_javaKeyboardVisible.exchange(isVis, std::memory_order_acq_rel);
	if (wasVis && !isVis) {
		g_javaKeyboardClosedMs.store(nowSteadyMs(), std::memory_order_release);
	}
}

extern "C" JNIEXPORT void JNICALL Java_com_sampmobile_game_ui_UI_keyboardSend(JNIEnv* env, jobject obj, jbyteArray str)
{
	if (!env || !str) return;
	jboolean isCopy = JNI_FALSE;
	jbyte* msg = env->GetByteArrayElements(str, &isCopy);
	if (!msg) return;
	jsize length = env->GetArrayLength(str);

	std::string str_string(reinterpret_cast<const char*>(msg), length > 0 ? static_cast<size_t>(length) : 0u);
	env->ReleaseByteArrayElements(str, msg, JNI_ABORT);

	std::lock_guard<std::mutex> lock(g_jniInputMutex);
	if (g_pendingKeyboardSends.size() < 32) {
		g_pendingKeyboardSends.push_back(std::move(str_string));
	}
}

extern "C" JNIEXPORT void JNICALL Java_com_sampmobile_game_ui_UI_sendDialogResponse(JNIEnv* env, jobject obj, jint dialog_id, jint button_id, jint list_item, jbyteArray str)
{
	spdlog::info("Java_com_sampmobile_game_ui_UI_sendDialogResponse");
	if (!env) return;

	std::string szStr;
	if (str) {
		jboolean isCopy = JNI_FALSE;
		jbyte* msg = env->GetByteArrayElements(str, &isCopy);
		if (msg) {
			jsize length = env->GetArrayLength(str);
			szStr.assign(reinterpret_cast<const char*>(msg), length > 0 ? static_cast<size_t>(length) : 0u);
			env->ReleaseByteArrayElements(str, msg, JNI_ABORT);
		}
	}

	std::lock_guard<std::mutex> lock(g_jniInputMutex);
	if (g_pendingDialogResponses.size() < 32) {
		g_pendingDialogResponses.push_back({dialog_id, button_id, list_item, std::move(szStr)});
	}
}

extern "C" JNIEXPORT void JNICALL Java_com_sampmobile_game_SAMP_onHardwareKeyEvent(JNIEnv* env, jobject obj, jint keyCode, jboolean isDown)
{
	std::lock_guard<std::mutex> lock(g_jniInputMutex);
	if (g_pendingHardwareKeys.size() < 64) {
		g_pendingHardwareKeys.push_back({static_cast<int>(keyCode), isDown == JNI_TRUE});
	}
}
