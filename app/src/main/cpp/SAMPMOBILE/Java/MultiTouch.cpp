#include "../../Client.h"
#include "../../UI/UI.h"
#include "Java.h"
#include "MultiTouch.h"
#include "NV_Event.h"

#include <unistd.h>

extern UI* pUI;

/* MultiTouch */

using FnAndTouchEvent = void (*)(int type, int num, int posX, int posY);
static FnAndTouchEvent g_fnAndTouchEvent = nullptr;

extern void AND_TouchEvent_hook(int type, int num, int posX, int posY);

void touch_event(int type, int num, int x, int y)
{
	// While SAMP's modal keyboard/dialog or Java Dialog is open, dispatch directly
	// to our hook so third-party AML touch overlays cannot steal modal UI taps.
	const bool modalOpen = Java::isJavaDialogVisible() ||
		(pUI != nullptr && pUI->isModalOpenSnapshot());
	if (!modalOpen && g_fnAndTouchEvent != nullptr) {
		g_fnAndTouchEvent(type, num, x, y);
		return;
	}

	AND_TouchEvent_hook(type, num, x, y);
}

static constexpr uint32_t kMultiTouchPresentMaskShift = 16;
static constexpr uint32_t kMultiTouchPresentMaskBits  = 0x0Fu << kMultiTouchPresentMaskShift;
static constexpr uint32_t kMultiTouchPrimaryBit       = 1u << 20;

static void dispatchMultiTouchEvent(const NVEvent& event)
{
	static bool s_pointerDown[4] = {false, false, false, false};
	static int s_lastX[4] = {0, 0, 0, 0};
	static int s_lastY[4] = {0, 0, 0, 0};

	const uint32_t rawAction = static_cast<uint32_t>(event.m_data.m_multi.m_action);
	const int type = static_cast<int>(rawAction & NV_MULTITOUCH_ACTION_MASK);
	const int num = static_cast<int>((rawAction & NV_MULTITOUCH_POINTER_MASK) >> NV_MULTITOUCH_POINTER_SHIFT);
	const uint32_t presentMask = (rawAction & kMultiTouchPresentMaskBits) >> kMultiTouchPresentMaskShift;
	const bool isPrimary = (rawAction & kMultiTouchPrimaryBit) != 0;

	const int xs[4] = {
		static_cast<int>(event.m_data.m_multi.m_x1),
		static_cast<int>(event.m_data.m_multi.m_x2),
		static_cast<int>(event.m_data.m_multi.m_x3),
		static_cast<int>(event.m_data.m_multi.m_x4)
	};
	const int ys[4] = {
		static_cast<int>(event.m_data.m_multi.m_y1),
		static_cast<int>(event.m_data.m_multi.m_y2),
		static_cast<int>(event.m_data.m_multi.m_y3),
		static_cast<int>(event.m_data.m_multi.m_y4)
	};

	// ACTION_CANCEL (e.g. MIUI 3-finger screenshot gesture, palm rejection,
	// notification shade, focus loss) cancels ALL active pointers at once.
	// Previously only pointer 0 got UP while pointers 1..3 got MOVE, leaving
	// phantom fingers permanently held down in CTouchInterface and forcing
	// single-finger camera drags into two-finger pinch zoom.
	if (type == NV_MULTITOUCH_CANCEL) {
		if (pUI != nullptr) {
			pUI->enqueueTouchEvent(ImVec2(0.0f, 0.0f), TouchType::pop, true);
		}
		for (int i = 0; i < 4; ++i) {
			if (s_pointerDown[i]) {
				const bool hasCoord = (presentMask & (1u << i)) != 0;
				touch_event(NV_MULTITOUCH_UP, i, hasCoord ? xs[i] : s_lastX[i], hasCoord ? ys[i] : s_lastY[i]);
				s_pointerDown[i] = false;
			}
		}
		return;
	}

	if (type == NV_MULTITOUCH_DOWN) {
		// Primary ACTION_DOWN starts a brand-new gesture; flush any stale fingers.
		if (isPrimary) {
			for (int i = 0; i < 4; ++i) {
				if (i != num && s_pointerDown[i]) {
					touch_event(NV_MULTITOUCH_UP, i, s_lastX[i], s_lastY[i]);
					s_pointerDown[i] = false;
				}
			}
		}
		// Self-heal any pointer absent from the active MotionEvent snapshot.
		for (int i = 0; i < 4; ++i) {
			if (i != num && s_pointerDown[i] && !(presentMask & (1u << i))) {
				touch_event(NV_MULTITOUCH_UP, i, s_lastX[i], s_lastY[i]);
				s_pointerDown[i] = false;
			}
		}
		if (num >= 0 && num < 4) {
			if (s_pointerDown[num]) {
				touch_event(NV_MULTITOUCH_UP, num, s_lastX[num], s_lastY[num]);
			}
			touch_event(NV_MULTITOUCH_DOWN, num, xs[num], ys[num]);
			s_pointerDown[num] = true;
			s_lastX[num] = xs[num];
			s_lastY[num] = ys[num];
		}
		for (int i = 0; i < 4; ++i) {
			if (i == num) continue;
			if (presentMask & (1u << i)) {
				if (s_pointerDown[i]) {
					touch_event(NV_MULTITOUCH_MOVE, i, xs[i], ys[i]);
				}
				else {
					touch_event(NV_MULTITOUCH_DOWN, i, xs[i], ys[i]);
					s_pointerDown[i] = true;
				}
				s_lastX[i] = xs[i];
				s_lastY[i] = ys[i];
			}
		}
		return;
	}

	if (type == NV_MULTITOUCH_UP) {
		if (num >= 0 && num < 4) {
			if (s_pointerDown[num] || (presentMask & (1u << num))) {
				touch_event(NV_MULTITOUCH_UP, num, xs[num], ys[num]);
			}
			s_pointerDown[num] = false;
		}
		if (isPrimary) {
			// Primary ACTION_UP ends the entire gesture: ensure zero stuck fingers.
			for (int i = 0; i < 4; ++i) {
				if (s_pointerDown[i]) {
					touch_event(NV_MULTITOUCH_UP, i, s_lastX[i], s_lastY[i]);
					s_pointerDown[i] = false;
				}
			}
		}
		else {
			for (int i = 0; i < 4; ++i) {
				if (i == num) continue;
				if (presentMask & (1u << i)) {
					if (s_pointerDown[i]) {
						touch_event(NV_MULTITOUCH_MOVE, i, xs[i], ys[i]);
						s_lastX[i] = xs[i];
						s_lastY[i] = ys[i];
					}
				}
				else if (s_pointerDown[i]) {
					touch_event(NV_MULTITOUCH_UP, i, s_lastX[i], s_lastY[i]);
					s_pointerDown[i] = false;
				}
			}
		}
		return;
	}

	// NV_MULTITOUCH_MOVE: update all present pointers and self-heal any missing ones.
	for (int i = 0; i < 4; ++i) {
		if (presentMask & (1u << i)) {
			if (!s_pointerDown[i]) {
				touch_event(NV_MULTITOUCH_DOWN, i, xs[i], ys[i]);
				s_pointerDown[i] = true;
			}
			else {
				touch_event(NV_MULTITOUCH_MOVE, i, xs[i], ys[i]);
			}
			s_lastX[i] = xs[i];
			s_lastY[i] = ys[i];
		}
		else if (s_pointerDown[i]) {
			touch_event(NV_MULTITOUCH_UP, i, s_lastX[i], s_lastY[i]);
			s_pointerDown[i] = false;
		}
	}
}

DECL_HOOK(int32_t, NVEventGetNextEvent, NVEvent* ev, int waitMSecs)
{
	if (!ev) {
		spdlog::warn("NVEvent: %d", waitMSecs);
	}
	static volatile pid_t nvEventTid = 0;
	if (!nvEventTid) nvEventTid = gettid();

	int32_t ret = NVEventGetNextEvent(ev, waitMSecs);

	if (ret && ev) {
		if (ev->m_type == NV_EVENT_MULTITOUCH) {
			ev->m_type = (NVEventType) 228;
		}
		else {
			// Delivered event visibility for boot-stall diagnosis
			// (1=KEY 2=CHAR 3=TOUCH 4=MULTITOUCH 5=ACCEL 6=WIN_SIZE 7=QUIT 8=PAUSE 9=RESUME)
			static int logged = 0;
			if (logged < 8) {
				++logged;
				spdlog::info("nv_event: delivered type={} waitMSecs={} tid={}", (int) ev->m_type, waitMSecs, (int) nvEventTid);
			}
		}
	}

	// Drain queued Java multi-touch events (including synthetic ACTION_CANCEL on
	// focus loss/pause that has no matching native NV_EVENT_MULTITOUCH).
	for (int drained = 0; drained < 32; ++drained) {
		NVEvent event;
		NV_Event::GetNextEvent(&event);
		if (event.m_type != NV_EVENT_MULTITOUCH) {
			break;
		}
		dispatchMultiTouchEvent(event);
	}

	return ret;
}

int g_points[1000];
int g_pointers[1000];

void MultiTouch::initialize()
{
	spdlog::info("Initializing multi touch..");

	// Points
	memset(g_points, 0, 999 * sizeof(int));
	Memory::protectAddr(g_saSym->Abs(addr::GOT_MultiTouch_points));
	*g_saSym->Abs<int**>(addr::GOT_MultiTouch_points) = &g_points[0];

	// pointers
	memset(g_pointers, 0, 999 * sizeof(int));
	Memory::protectAddr(g_saSym->Abs(addr::GOT_MultiTouch_pointers));
	*g_saSym->Abs<int**>(addr::GOT_MultiTouch_pointers) = &g_pointers[0];

#if VER_x32
	Memory::writeMemory(g_saSym->Abs<uintptr_t>(addr::OS_PointerGetNumber_patch), (uintptr_t) "\x04\x20", 2);
	Memory::writeMemory(g_saSym->Abs<uintptr_t>(addr::OS_PointerGetType_patch), (uintptr_t) "\x04\x28", 2);
	Memory::writeMemory(g_saSym->Abs<uintptr_t>(addr::LIB_PointerGetCoordinates_patch), (uintptr_t) "\x03\x28", 2);
	Memory::writeMemory(g_saSym->Abs<uintptr_t>(addr::LIB_PointerGetWheel_patch), (uintptr_t) "\x03\x28", 2);
	Memory::writeMemory(g_saSym->Abs<uintptr_t>(addr::LIB_PointerDoubleClicked_patch), (uintptr_t) "\x03\x28", 2);
	Memory::writeMemory(g_saSym->Abs<uintptr_t>(addr::LIB_PointerGetButton_patch), (uintptr_t) "\x03\x2A", 2);
#else
	Memory::writeMemory(g_saSym->Abs<uintptr_t>(addr::OS_PointerGetNumber_patch), (uintptr_t) "\x80\x00\x80\x52", 4);
	Memory::writeMemory(g_saSym->Abs<uintptr_t>(addr::OS_PointerGetType_patch), (uintptr_t) "\x1F\x10\x00\x71", 4);
	Memory::writeMemory(g_saSym->Abs<uintptr_t>(addr::LIB_PointerGetCoordinates_patch), (uintptr_t) "\x1F\x0C\x00\x71", 4);
	Memory::writeMemory(g_saSym->Abs<uintptr_t>(addr::LIB_PointerGetWheel_patch), (uintptr_t) "\x1F\x0C\x00\x71", 4);
	Memory::writeMemory(g_saSym->Abs<uintptr_t>(addr::LIB_PointerDoubleClicked_patch), (uintptr_t) "\x1F\x0C\x00\x71", 4);
	Memory::writeMemory(g_saSym->Abs<uintptr_t>(addr::LIB_PointerGetButton_patch), (uintptr_t) "\x1F\x0D\x00\x71", 4);
#endif

	// Resolve live _Z14AND_TouchEventiiii entry point so touches flow through
	// AML plugins hooked after Hooks::install() (e.g. libFPP64.so) and chain into AND_TouchEvent_hook.
	g_fnAndTouchEvent = eng::as<FnAndTouchEvent>(eng::Sym::AND_TouchEvent);

	// NVEventGetNextEvent
	HOOK_SYM(NVEventGetNextEvent, NVEventGetNextEvent);
}

extern "C"
{
JNIEXPORT jboolean JNICALL Java_com_sampmobile_game_SAMP_multiTouchEvent4Ex(JNIEnv* env, jobject obj, jint action,
		jint pointer, jint x1, jint y1, jint x2, jint y2, jint x3, jint y3, jint x4, jint y4)
{
	static jclass motionEvent = env->FindClass("android/view/MotionEvent");

	static jfieldID ACTION_DOWN_id = env->GetStaticFieldID(motionEvent, "ACTION_DOWN", "I");
	static jfieldID ACTION_UP_id = env->GetStaticFieldID(motionEvent, "ACTION_UP", "I");

	static jfieldID ACTION_POINTER_DOWN_id = env->GetStaticFieldID(motionEvent, "ACTION_POINTER_DOWN", "I");
	static jfieldID ACTION_POINTER_UP_id = env->GetStaticFieldID(motionEvent, "ACTION_POINTER_UP", "I");

	static jfieldID ACTION_CANCEL_id = env->GetStaticFieldID(motionEvent, "ACTION_CANCEL", "I");
	static jfieldID ACTION_POINTER_INDEX_SHIFT_id = env->GetStaticFieldID(motionEvent, "ACTION_POINTER_ID_SHIFT", "I");
	static jfieldID ACTION_POINTER_INDEX_MASK_id = env->GetStaticFieldID(motionEvent, "ACTION_POINTER_ID_MASK", "I");
	static jfieldID ACTION_MASK_id = env->GetStaticFieldID(motionEvent, "ACTION_MASK", "I");

	static int ACTION_DOWN = env->GetStaticIntField(motionEvent, ACTION_DOWN_id);
	static int ACTION_UP = env->GetStaticIntField(motionEvent, ACTION_UP_id);

	static int ACTION_POINTER_DOWN = env->GetStaticIntField(motionEvent, ACTION_POINTER_DOWN_id);
	static int ACTION_POINTER_UP = env->GetStaticIntField(motionEvent, ACTION_POINTER_UP_id);

	static int ACTION_CANCEL = env->GetStaticIntField(motionEvent, ACTION_CANCEL_id);
	static int ACTION_POINTER_INDEX_MASK = env->GetStaticIntField(motionEvent, ACTION_POINTER_INDEX_MASK_id);
	static int ACTION_POINTER_INDEX_SHIFT = env->GetStaticIntField(motionEvent, ACTION_POINTER_INDEX_SHIFT_id);
	static int ACTION_MASK = env->GetStaticIntField(motionEvent, ACTION_MASK_id);

	NVEvent ev;
	memset(&ev, 0, sizeof(ev));

	ev.m_type = NV_EVENT_MULTITOUCH;

	uint32_t actionType = NV_MULTITOUCH_MOVE;
	uint32_t extraFlags = 0;

	if (action == ACTION_UP) {
		actionType = NV_MULTITOUCH_UP;
		extraFlags |= kMultiTouchPrimaryBit;
	}
	else if (action == ACTION_DOWN) {
		actionType = NV_MULTITOUCH_DOWN;
		extraFlags |= kMultiTouchPrimaryBit;
	}
	else if (action == ACTION_POINTER_DOWN) {
		actionType = NV_MULTITOUCH_DOWN;
	}
	else if (action == ACTION_POINTER_UP) {
		actionType = NV_MULTITOUCH_UP;
	}
	else if (action == ACTION_CANCEL) {
		actionType = NV_MULTITOUCH_CANCEL;
	}
	else {
		actionType = NV_MULTITOUCH_MOVE;
	}

	const uint32_t actionPtr = static_cast<uint32_t>(pointer) & 0xFFu;
	const uint32_t presentMask = (static_cast<uint32_t>(pointer) >> 8) & 0x0Fu;

	ev.m_data.m_multi.m_action = static_cast<NVMultiTouchEventType>(
		actionType
		| (actionPtr << NV_MULTITOUCH_POINTER_SHIFT)
		| (presentMask << kMultiTouchPresentMaskShift)
		| extraFlags);

	ev.m_data.m_multi.m_x1 = x1;
	ev.m_data.m_multi.m_y1 = y1;

	ev.m_data.m_multi.m_x2 = x2;
	ev.m_data.m_multi.m_y2 = y2;

	ev.m_data.m_multi.m_x3 = x3;
	ev.m_data.m_multi.m_y3 = y3;

	ev.m_data.m_multi.m_x4 = x4;
	ev.m_data.m_multi.m_y4 = y4;

	NV_Event::InsertNewest(&ev);

	return JNI_TRUE;
}
}
