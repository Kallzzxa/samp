#include "CleoEngine.h"

#include "../Client.h"
#include "../Settings.h"
#include "../Memory/include/Memory.h"
#include "../GameSA/include/Scripts/RunningScript.h"
#include "../UI/ImGuiRenderer.h"
#include "Game.h"
#include "Util.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <fstream>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

extern Game* pGame;

namespace {

// ---------------------------------------------------------------------------
// Constants & Types
// ---------------------------------------------------------------------------
constexpr uint32_t kCleoVerGtaSa200Plus = 17; // VER_GTASA_2_00_OR_HIGHER
constexpr uint32_t kHandleBase = 0x7E000001u;
constexpr int kMaxStepsPerTick = 4096;
constexpr int kMaxScmCallDepth = 32;
constexpr int kMaxSharedVars = 1024;
constexpr int kMenuItemsPerPage = 10;

struct ScmCallFrame {
	uint8_t* returnIp = nullptr;
	uint32_t savedLocals[32]{};
};

struct CleoScript {
	std::string fileName;
	std::string displayName;
	std::vector<uint8_t> code;
	sa::CRunningScript thread{};
	bool invokable = false;
	int32_t invokableId = -1;
	bool startedOnce = false;
	bool waitPassed = false;
	std::vector<ScmCallFrame> callFrames;
	uint32_t context[32]{};
};

struct PtrHandleEntry {
	void* ptr = nullptr;
	size_t size = 0;
	bool owned = false;
};

struct TouchFinger {
	bool down = false;
	int startZone = 0;
	int currentZone = 0;
	uint32_t startTime = 0;
	uint32_t lastZoneTime = 0;
};

struct SlideRecord {
	int fromZone = 0;
	int toZone = 0;
	uint32_t duration = 0;
	uint32_t finishTime = 0;
};

struct ScriptMenuState {
	bool active = false;
	std::string title;
	std::string closeLabel;
	std::vector<std::string> items;
	int activeIndex = 0;
	int page = 0;
	int lastTouchedIndex = -1;
	uint32_t lastTouchedTime = 0;
};

struct BuiltinMenuState {
	bool open = false;
	int page = 0;
	bool showArrowHint = false;
};

struct RectF {
	float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
	bool contains(float x, float y) const {
		return x >= x1 && x <= x2 && y >= y1 && y <= y2;
	}
};

// ---------------------------------------------------------------------------
// Engine Global State
// ---------------------------------------------------------------------------
bool g_enabled = false;
bool g_symbolsReady = false;
bool g_scriptsBooted = false;
std::string g_gameDir;
std::string g_cleoDir;
std::string g_currentDir;
std::string g_logPath;
FILE* g_logFile = nullptr;
std::mutex g_logMutex;

uintptr_t g_gtasaBase = 0;
uint32_t* g_scriptParams = nullptr;
uint8_t* g_scriptSpace = nullptr;
uint32_t* g_timerMs = nullptr;
void* g_theText = nullptr;

using FnInitScript = void (*)(sa::CRunningScript*);
using FnProcessOneCmd = uint8_t (*)(sa::CRunningScript*);
using FnCollectParams = void (*)(sa::CRunningScript*, int16_t);
using FnStoreParams = void (*)(sa::CRunningScript*, int16_t);
using FnUpdateCmpFlag = void (*)(sa::CRunningScript*, uint8_t);
using FnGetVarPtr = uint32_t* (*)(sa::CRunningScript*, uint8_t);
using FnAsciiToGxt = void (*)(const char*, uint16_t*);
using FnSetHelpMsg = void (*)(const char*, uint16_t*, bool, bool, bool, uint32_t);
using FnAddMessage = void (*)(const char*, uint16_t*, uint32_t, uint16_t, bool);
using FnAddBigMessage = void (*)(uint16_t*, uint32_t, uint16_t);
using FnCTextGet = uint16_t* (*)(void*, const char*);

FnInitScript g_fnInitScript = nullptr;
FnProcessOneCmd g_fnProcessOneCmd = nullptr;
FnCollectParams g_fnCollectParams = nullptr;
FnStoreParams g_fnStoreParams = nullptr;
FnUpdateCmpFlag g_fnUpdateCmpFlag = nullptr;
FnGetVarPtr g_fnGetVarPtr = nullptr;
FnAsciiToGxt g_fnAsciiToGxt = nullptr;
FnSetHelpMsg g_fnSetHelpMsg = nullptr;
FnAddMessage g_fnAddMessage = nullptr;
FnAddMessage g_fnAddMessageJumpQ = nullptr;
FnAddBigMessage g_fnAddBigMessage = nullptr;
FnCTextGet g_fnCTextGet = nullptr;

std::vector<CleoScript> g_scripts;
std::unordered_map<std::string, std::vector<uint16_t>> g_customGxt;
std::mutex g_gxtMutex;

std::unordered_map<uint32_t, uint32_t> g_mutexVars;
std::array<uint32_t, kMaxSharedVars> g_sharedVars{};

std::unordered_map<uint32_t, PtrHandleEntry> g_ptrHandles;
uint32_t g_nextHandle = kHandleBase;

std::unordered_map<uint32_t, FILE*> g_openFiles;
uint32_t g_nextFileHandle = 0x5F000001u;

uint32_t g_lastTickMs = 0;

std::mutex g_touchMutex;
TouchFinger g_fingers[8]{};
SlideRecord g_lastSlide{};
bool g_menuBtnDown = false;
uint32_t g_menuBtnStart = 0;
float g_screenW = 1920.0f;
float g_screenH = 1080.0f;

ScriptMenuState g_scriptMenu{};
BuiltinMenuState g_builtinMenu{};
int g_pendingStartScriptIdx = -1;

// Hitboxes refreshed each rendered frame for accurate touch selection
RectF g_menuPanelRect{};
RectF g_menuCloseRect{};
RectF g_menuPrevRect{};
RectF g_menuNextRect{};
std::vector<std::pair<int, RectF>> g_menuRowRects;

// Ring buffer for temporary GXT messages (0ACA..0ACD)
uint16_t g_msgGxtBuf[4][512]{};
int g_msgGxtSlot = 0;

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
void cleoLog(const char* fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	spdlog::info("[CLEO] {}", buf);
	std::lock_guard<std::mutex> lock(g_logMutex);
	if (g_logFile) {
		uint32_t ms = g_timerMs ? *g_timerMs : 0;
		fprintf(g_logFile, "[%u] %s\n", ms, buf);
		fflush(g_logFile);
	}
}

uint32_t nowMs()
{
	return g_timerMs ? *g_timerMs : 0;
}

std::string toUpperAscii(const std::string& s)
{
	std::string out = s;
	for (char& c : out) {
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	}
	return out;
}

std::string toLowerAscii(const std::string& s)
{
	std::string out = s;
	for (char& c : out) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return out;
}

bool endsWithIgnoreCase(const std::string& s, const std::string& suffix)
{
	if (s.size() < suffix.size()) return false;
	return toLowerAscii(s.substr(s.size() - suffix.size())) == toLowerAscii(suffix);
}

std::vector<uint16_t> asciiToWide(const std::string& s)
{
	std::vector<uint16_t> out;
	out.reserve(s.size() + 1);
	for (unsigned char c : s) {
		out.push_back(static_cast<uint16_t>(c));
	}
	out.push_back(0);
	return out;
}

std::string wideGxtToUtf8(const uint16_t* w)
{
	if (!w) return {};
	std::string out;
	for (size_t i = 0; i < 512 && w[i] != 0; ++i) {
		uint16_t ch = w[i];
		if (ch < 0x80) {
			out.push_back(static_cast<char>(ch));
		} else if (ch < 0x800) {
			out.push_back(static_cast<char>(0xC0 | (ch >> 6)));
			out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
		} else {
			out.push_back(static_cast<char>(0xE0 | (ch >> 12)));
			out.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
		}
	}
	return out;
}

// ---------------------------------------------------------------------------
// 32-bit Pointer Handle Registry (safe across arm64 and arm32)
// ---------------------------------------------------------------------------
uint32_t registerHandle(void* ptr, size_t size = 0, bool owned = false)
{
	if (!ptr) return 0;
	if (!owned) {
		for (const auto& kv : g_ptrHandles) {
			if (!kv.second.owned && kv.second.ptr == ptr) {
				return kv.first;
			}
		}
	}
	uint32_t h = g_nextHandle++;
	if (g_nextHandle < kHandleBase) g_nextHandle = kHandleBase;
	g_ptrHandles[h] = PtrHandleEntry{ptr, size, owned};
	return h;
}

void* resolveHandleOrAddr(uint32_t addr, bool addImageBase, size_t* outSize = nullptr)
{
	if (outSize) *outSize = 0;
	if (!addImageBase) {
		auto it = g_ptrHandles.find(addr);
		if (it != g_ptrHandles.end()) {
			if (outSize) *outSize = it->second.size;
			return it->second.ptr;
		}
	}
	if (addImageBase) {
		return reinterpret_cast<void*>(g_gtasaBase + static_cast<uintptr_t>(addr));
	}
#if defined(__arm__)
	return reinterpret_cast<void*>(static_cast<uintptr_t>(addr));
#else
	return nullptr;
#endif
}

bool readBoundedCStr(const char*& cursor, const char* end, std::string& out, size_t maxLen = 256)
{
	out.clear();
	if (!cursor || cursor >= end) return false;
	const char* limit = std::min(end, cursor + maxLen);
	const void* nul = memchr(cursor, '\0', static_cast<size_t>(limit - cursor));
	if (!nul) {
		out.assign(cursor, static_cast<size_t>(limit - cursor));
		cursor = limit;
		return false;
	}
	const char* nulPos = static_cast<const char*>(nul);
	out.assign(cursor, static_cast<size_t>(nulPos - cursor));
	cursor = nulPos + 1;
	return true;
}

void resetTouchFingers()
{
	for (auto& f : g_fingers) {
		f = TouchFinger{};
	}
}

// ---------------------------------------------------------------------------
// FXT Loader & GXT Lookup
// ---------------------------------------------------------------------------
void addCustomGxtEntry(const std::string& rawKey, const std::string& value)
{
	std::string key = toUpperAscii(rawKey);
	if (key.empty()) return;
	std::lock_guard<std::mutex> lock(g_gxtMutex);
	g_customGxt[key] = asciiToWide(value);
}

void removeCustomGxtEntry(const std::string& rawKey)
{
	std::string key = toUpperAscii(rawKey);
	std::lock_guard<std::mutex> lock(g_gxtMutex);
	g_customGxt.erase(key);
}

void loadFxtFile(const std::string& path)
{
	std::ifstream f(path);
	if (!f.is_open()) return;

	int count = 0;
	std::string line;
	while (std::getline(f, line)) {
		while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
			line.pop_back();
		}
		// Skip UTF-8 BOM if present
		if (line.size() >= 3 &&
		    static_cast<unsigned char>(line[0]) == 0xEF &&
		    static_cast<unsigned char>(line[1]) == 0xBB &&
		    static_cast<unsigned char>(line[2]) == 0xBF) {
			line.erase(0, 3);
		}
		size_t start = 0;
		while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) ++start;
		if (start >= line.size() || line[start] == '#' ||
		    (line[start] == '/' && start + 1 < line.size() && line[start + 1] == '/')) {
			continue;
		}
		size_t sep = start;
		while (sep < line.size() && line[sep] != ' ' && line[sep] != '\t') ++sep;
		if (sep >= line.size()) continue;
		std::string key = line.substr(start, sep - start);
		size_t valStart = sep;
		while (valStart < line.size() && (line[valStart] == ' ' || line[valStart] == '\t')) ++valStart;
		if (valStart >= line.size()) continue;
		std::string val = line.substr(valStart);
		addCustomGxtEntry(key, val);
		++count;
	}
	cleoLog("Loaded FXT '%s' (%d entries)", path.c_str(), count);
}

std::string lookupGxtUtf8(const std::string& keyOrLiteral)
{
	if (uint16_t* custom = CleoEngine::getCustomGxt(keyOrLiteral.c_str())) {
		return wideGxtToUtf8(custom);
	}
	if (g_fnCTextGet && g_theText) {
		if (uint16_t* native = g_fnCTextGet(g_theText, keyOrLiteral.c_str())) {
			std::string s = wideGxtToUtf8(native);
			if (!s.empty() && s.find("missing") == std::string::npos) {
				return s;
			}
		}
	}
	return keyOrLiteral;
}

// ---------------------------------------------------------------------------
// Script Discovery & Initialization
// ---------------------------------------------------------------------------
std::string makeDisplayName(const std::string& fileName)
{
	std::string name = fileName;
	size_t dot = name.rfind('.');
	if (dot != std::string::npos) name = name.substr(0, dot);
	// Strip common "gtasa." prefix for cleaner CLEO menu display
	if (name.size() > 6 && toLowerAscii(name.substr(0, 6)) == "gtasa.") {
		name = name.substr(6);
	}
	return name;
}

void scanDirectory(const std::string& dirPath, bool scanScripts)
{
	DIR* d = opendir(dirPath.c_str());
	if (!d) return;

	std::vector<std::string> names;
	while (struct dirent* ent = readdir(d)) {
		if (ent->d_name[0] == '\0' || ent->d_name[0] == '.') continue;
		names.emplace_back(ent->d_name);
	}
	closedir(d);
	std::sort(names.begin(), names.end());

	for (const std::string& name : names) {
		if (endsWithIgnoreCase(name, ".disabled")) continue;
		std::string full = dirPath + name;
		struct stat st{};
		if (stat(full.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) continue;

		if (endsWithIgnoreCase(name, ".fxt")) {
			loadFxtFile(full);
			continue;
		}

		if (!scanScripts) continue;

		bool isAuto = endsWithIgnoreCase(name, ".csa") || endsWithIgnoreCase(name, ".cs");
		bool isInvokable = endsWithIgnoreCase(name, ".csi");
		if (!isAuto && !isInvokable) continue;

		if (st.st_size < 2 || st.st_size > 8 * 1024 * 1024) {
			cleoLog("Skip script '%s' (invalid size %lld)", name.c_str(), (long long) st.st_size);
			continue;
		}

		std::ifstream in(full, std::ios::binary);
		if (!in.is_open()) continue;

		CleoScript sc{};
		sc.fileName = name;
		sc.displayName = makeDisplayName(name);
		sc.code.resize(static_cast<size_t>(st.st_size));
		in.read(reinterpret_cast<char*>(sc.code.data()), st.st_size);
		sc.invokable = isInvokable;
		if (isInvokable) {
			int32_t nextId = 0;
			for (const auto& existing : g_scripts) {
				if (existing.invokable) ++nextId;
			}
			sc.invokableId = nextId;
		}
		g_scripts.push_back(std::move(sc));
		cleoLog("Queued %s script '%s' (%zu bytes)",
		        isInvokable ? "invokable (.csi)" : "auto (.csa/.cs)",
		        name.c_str(),
		        g_scripts.back().code.size());
	}
}

void startOrRestartScript(CleoScript& sc, bool preserveLocals, uint32_t initialWaitMs)
{
	uint32_t savedLocals[42]{};
	if (preserveLocals && sc.startedOnce) {
		memcpy(savedLocals, sc.thread.m_nLocals, sizeof(savedLocals));
	}

	memset(&sc.thread, 0, sizeof(sc.thread));
	if (g_fnInitScript) {
		g_fnInitScript(&sc.thread);
	}

	if (preserveLocals && sc.startedOnce) {
		memcpy(sc.thread.m_nLocals, savedLocals, sizeof(savedLocals));
	}

	std::string shortName = sc.displayName.substr(0, 7);
	snprintf(sc.thread.m_szScriptName, sizeof(sc.thread.m_szScriptName), "%s", shortName.c_str());
	sc.thread.m_pBaseAddressOfThisScript = sc.code.data();
	sc.thread.m_pPCPointer = sc.code.data();
	sc.thread.m_nStackDepth = 0;
	sc.thread.m_bActive = 1;
	sc.thread.m_bCmpFlag = 0;
	sc.thread.m_nAndOrState = 0;
	sc.thread.m_bNotForLatestExpression = 0;
	sc.thread.m_nActivateTime = static_cast<int32_t>(nowMs() + initialWaitMs);
	sc.startedOnce = true;
	sc.waitPassed = (initialWaitMs == 0);
	sc.callFrames.clear();

	cleoLog("Started script '%s' (invokable=%d, preserveLocals=%d)",
	        sc.fileName.c_str(), sc.invokable ? 1 : 0, preserveLocals ? 1 : 0);
}

// ---------------------------------------------------------------------------
// Touch Zone Helpers (3x3 screen grid, CLEO Android standard 1..9)
// ---------------------------------------------------------------------------
int calcTouchZone(float x, float y, float w, float h)
{
	if (w <= 1.0f || h <= 1.0f) return 0;
	int col = static_cast<int>(x * 3.0f / w);
	int row = static_cast<int>(y * 3.0f / h);
	col = std::clamp(col, 0, 2);
	row = std::clamp(row, 0, 2);
	return row * 3 + col + 1; // 1..9
}

bool isZoneTouchedTimed(int zone, uint32_t minTimeMs)
{
	std::lock_guard<std::mutex> lock(g_touchMutex);
	uint32_t t = nowMs();
	for (const auto& f : g_fingers) {
		if (f.down && f.currentZone == zone) {
			if (minTimeMs == 0 || (t >= f.lastZoneTime && (t - f.lastZoneTime) >= minTimeMs)) {
				return true;
			}
		}
	}
	return false;
}

bool isAnyFingerDown()
{
	std::lock_guard<std::mutex> lock(g_touchMutex);
	for (const auto& f : g_fingers) {
		if (f.down) return true;
	}
	return false;
}

bool checkSlideDone(int fromZone, int toZone, uint32_t minMs, uint32_t maxMs)
{
	std::lock_guard<std::mutex> lock(g_touchMutex);
	uint32_t t = nowMs();
	if (g_lastSlide.fromZone == fromZone && g_lastSlide.toZone == toZone &&
	    g_lastSlide.finishTime > 0 && t >= g_lastSlide.finishTime && (t - g_lastSlide.finishTime) <= 350 &&
	    g_lastSlide.duration >= minMs && (maxMs == 0 || g_lastSlide.duration <= maxMs)) {
		g_lastSlide.finishTime = 0; // consume once
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// SCM Parameter Helpers
// ---------------------------------------------------------------------------
bool readTargetLabelOffset(CleoScript& sc, uint8_t*& outTarget)
{
	g_fnCollectParams(&sc.thread, 1);
	int32_t off = static_cast<int32_t>(g_scriptParams[0]);
	size_t rel = (off >= 0) ? static_cast<size_t>(off) : static_cast<size_t>(-static_cast<int64_t>(off));
	if (rel >= sc.code.size()) {
		cleoLog("Script '%s': jump offset %d out of bounds (size=%zu), terminating",
		        sc.fileName.c_str(), off, sc.code.size());
		sc.thread.m_bActive = 0;
		return false;
	}
	outTarget = sc.code.data() + rel;
	return true;
}

bool readStringParam(CleoScript& sc, std::string& out)
{
	out.clear();
	uint8_t* ip = sc.thread.m_pPCPointer;
	uint8_t* end = sc.code.data() + sc.code.size();
	if (ip >= end) return false;

	uint8_t type = *ip;
	switch (type) {
		case sa::SCRIPT_PARAM_STATIC_SHORT_STRING: { // 0x09 (8 bytes)
			if (ip + 9 > end) return false;
			char tmp[9]{};
			memcpy(tmp, ip + 1, 8);
			out = tmp;
			sc.thread.m_pPCPointer = ip + 9;
			return true;
		}
		case sa::SCRIPT_PARAM_STATIC_PASCAL_STRING: { // 0x0E (len + bytes)
			if (ip + 2 > end) return false;
			uint8_t len = ip[1];
			if (ip + 2 + len > end) return false;
			out.assign(reinterpret_cast<const char*>(ip + 2), len);
			sc.thread.m_pPCPointer = ip + 2 + len;
			return true;
		}
		case sa::SCRIPT_PARAM_STATIC_LONG_STRING: { // 0x0F (16 bytes)
			if (ip + 17 > end) return false;
			char tmp[17]{};
			memcpy(tmp, ip + 1, 16);
			out = tmp;
			sc.thread.m_pPCPointer = ip + 17;
			return true;
		}
		case sa::SCRIPT_PARAM_GLOBAL_SHORT_STRING_VARIABLE:
		case sa::SCRIPT_PARAM_LOCAL_SHORT_STRING_VARIABLE: {
			uint32_t* p = g_fnGetVarPtr(&sc.thread, 0);
			if (!p) return false;
			char tmp[9]{};
			memcpy(tmp, p, 8);
			out = tmp;
			return true;
		}
		case sa::SCRIPT_PARAM_GLOBAL_LONG_STRING_VARIABLE:
		case sa::SCRIPT_PARAM_LOCAL_LONG_STRING_VARIABLE: {
			uint32_t* p = g_fnGetVarPtr(&sc.thread, 0);
			if (!p) return false;
			char tmp[17]{};
			memcpy(tmp, p, 16);
			out = tmp;
			return true;
		}
		default: {
			// Integer / variable holding a 32-bit handle/pointer from 0DD0 / 0AC6 / 0AC8
			g_fnCollectParams(&sc.thread, 1);
			size_t bufSize = 0;
			void* ptr = resolveHandleOrAddr(g_scriptParams[0], false, &bufSize);
			if (ptr) {
				const char* cs = static_cast<const char*>(ptr);
				const size_t maxLen = (bufSize > 0) ? bufSize : 256;
				out.assign(cs, strnlen(cs, maxLen));
				return true;
			}
			return false;
		}
	}
}

uint16_t* makeTempGxt(const std::string& text)
{
	uint16_t* buf = g_msgGxtBuf[g_msgGxtSlot];
	g_msgGxtSlot = (g_msgGxtSlot + 1) & 3;
	if (g_fnAsciiToGxt) {
		std::string clipped = text.substr(0, 500);
		g_fnAsciiToGxt(clipped.c_str(), buf);
	} else {
		size_t n = std::min<size_t>(text.size(), 510);
		for (size_t i = 0; i < n; ++i) buf[i] = static_cast<unsigned char>(text[i]);
		buf[n] = 0;
	}
	return buf;
}

std::string resolveScriptPath(const std::string& rawPath)
{
	if (rawPath.empty()) return g_currentDir;
	std::string p = rawPath;
	for (char& c : p) {
		if (c == '\\') c = '/';
	}
	// Reject path traversal outside game/cleo sandbox
	std::vector<std::string> parts;
	size_t i = 0;
	while (i < p.size()) {
		while (i < p.size() && p[i] == '/') ++i;
		size_t start = i;
		while (i < p.size() && p[i] != '/') ++i;
		if (i > start) {
			std::string seg = p.substr(start, i - start);
			if (seg == ".") continue;
			if (seg == "..") {
				cleoLog("Blocked unsafe CLEO path traversal: '%s'", rawPath.c_str());
				return {};
			}
			parts.push_back(std::move(seg));
		}
	}
	if (parts.empty()) return g_currentDir;
	std::string joined;
	for (size_t k = 0; k < parts.size(); ++k) {
		if (k > 0) joined.push_back('/');
		joined += parts[k];
	}
	return g_currentDir + joined;
}

// ---------------------------------------------------------------------------
// Custom CLEO Opcode Dispatcher (returns true if handled)
// ---------------------------------------------------------------------------
bool handleCustomOpcode(CleoScript& sc, uint16_t rawOp)
{
	const bool isNot = (rawOp & 0x8000u) != 0;
	const uint16_t op = rawOp & 0x7FFFu;

	// Helper to set condition with NOT inversion handled by GTA UpdateCompareFlag
	auto setCond = [&](bool val) {
		sc.thread.m_bNotForLatestExpression = isNot ? 1 : 0;
		g_fnUpdateCmpFlag(&sc.thread, val ? 1 : 0);
	};

	switch (op) {
		// -------- Core Control Flow (relative to script.code) --------
		case 0x0002: { // JUMP
			uint8_t* target = nullptr;
			if (readTargetLabelOffset(sc, target)) {
				sc.thread.m_pPCPointer = target;
			}
			return true;
		}
		case 0x004C: { // JUMP_IF_TRUE (JT)
			uint8_t* target = nullptr;
			if (readTargetLabelOffset(sc, target) && sc.thread.m_bCmpFlag != 0) {
				sc.thread.m_pPCPointer = target;
			}
			return true;
		}
		case 0x004D: { // JUMP_IF_FALSE (JF)
			uint8_t* target = nullptr;
			if (readTargetLabelOffset(sc, target) && sc.thread.m_bCmpFlag == 0) {
				sc.thread.m_pPCPointer = target;
			}
			return true;
		}
		case 0x0050: { // GOSUB (CALL)
			uint8_t* target = nullptr;
			if (readTargetLabelOffset(sc, target)) {
				if (sc.thread.m_nStackDepth < 8) {
					sc.thread.m_pPCStack[sc.thread.m_nStackDepth++] = sc.thread.m_pPCPointer;
					sc.thread.m_pPCPointer = target;
				} else {
					cleoLog("Script '%s': GOSUB stack overflow, terminating", sc.fileName.c_str());
					sc.thread.m_bActive = 0;
				}
			}
			return true;
		}
		case 0x0051: { // RETURN
			if (sc.thread.m_nStackDepth > 0) {
				sc.thread.m_pPCPointer = sc.thread.m_pPCStack[--sc.thread.m_nStackDepth];
			} else {
				sc.thread.m_bActive = 0;
			}
			return true;
		}
		case 0x004E: // END_THREAD
		case 0x05DC: // END_CUSTOM_THREAD
		case 0x0A93: { // TERMINATE_THIS_CUSTOM_SCRIPT
			cleoLog("Script '%s' terminated (opcode 0x%04X)", sc.fileName.c_str(), op);
			sc.thread.m_bActive = 0;
			return true;
		}

		// -------- CLEO Android Opcodes (0DD0..0DF6) --------
		case 0x0DD0:   // GET_LABEL_ADDR
		case 0x0AC6: { // GET_LABEL_POINTER
			uint32_t* outVar = g_fnGetVarPtr(&sc.thread, 0);
			uint8_t* target = nullptr;
			if (readTargetLabelOffset(sc, target) && outVar) {
				*outVar = registerHandle(target, sc.code.data() + sc.code.size() - target, false);
			}
			return true;
		}
		case 0x0DD1: { // GET_FUNC_ADDR_CSTR
			uint32_t* outVar = g_fnGetVarPtr(&sc.thread, 0);
			std::string symName;
			readStringParam(sc, symName);
			void* sym = symName.empty() ? nullptr : g_saSym->GetSymbol<void*>(symName.c_str());
			if (!sym && !symName.empty()) {
				sym = dlsym(RTLD_DEFAULT, symName.c_str());
			}
			if (outVar) *outVar = registerHandle(sym, 0, false);
			return true;
		}
		case 0x0DD2: { // CONTEXT_CALL (32-bit only; safe no-op on 64-bit)
			g_fnCollectParams(&sc.thread, 1);
			cleoLog("Script '%s': 0DD2 CONTEXT_CALL skipped (unsafe in dual-ABI/SA-MP)", sc.fileName.c_str());
			return true;
		}
		case 0x0DD3: { // CONTEXT_SET_REG
			g_fnCollectParams(&sc.thread, 2);
			uint32_t reg = g_scriptParams[0];
			if (reg < 32) sc.context[reg] = g_scriptParams[1];
			return true;
		}
		case 0x0DD4: { // CONTEXT_GET_REG
			uint32_t* outVar = g_fnGetVarPtr(&sc.thread, 0);
			g_fnCollectParams(&sc.thread, 1);
			uint32_t reg = g_scriptParams[0];
			if (outVar) *outVar = (reg < 32) ? sc.context[reg] : 0;
			return true;
		}
		case 0x0DD5: { // GET_PLATFORM (1 = Android)
			uint32_t* outVar = g_fnGetVarPtr(&sc.thread, 0);
			if (outVar) *outVar = 1;
			return true;
		}
		case 0x0DD6: { // GET_GAME_VER
			uint32_t* outVar = g_fnGetVarPtr(&sc.thread, 0);
			if (outVar) *outVar = kCleoVerGtaSa200Plus;
			return true;
		}
		case 0x0DD7: { // GET_IMAGE_BASE
			uint32_t* outVar = g_fnGetVarPtr(&sc.thread, 0);
			if (outVar) *outVar = registerHandle(reinterpret_cast<void*>(g_gtasaBase), 0, false);
			return true;
		}
		case 0x0DD8: { // READ_MEM(outVar, addr, size, addBase)
			uint32_t* outVar = g_fnGetVarPtr(&sc.thread, 0);
			g_fnCollectParams(&sc.thread, 3);
			uint32_t addr = g_scriptParams[0];
			uint32_t sz = std::min<uint32_t>(g_scriptParams[1], 4);
			bool addBase = g_scriptParams[2] != 0;
			if (outVar) {
				*outVar = 0;
				size_t bufSize = 0;
				if (void* ptr = resolveHandleOrAddr(addr, addBase, &bufSize)) {
					if (bufSize == 0 || sz <= bufSize) {
						memcpy(outVar, ptr, sz);
					}
				}
			}
			return true;
		}
		case 0x0DD9: { // WRITE_MEM(addr, val, size, addBase, vp)
			g_fnCollectParams(&sc.thread, 5);
			uint32_t addr = g_scriptParams[0];
			uint32_t val = g_scriptParams[1];
			uint32_t sz = std::min<uint32_t>(g_scriptParams[2], 4);
			bool addBase = g_scriptParams[3] != 0;
			// Only allow writing to CLEO-owned handles (protects libGTASA.so code on 64-bit)
			auto it = g_ptrHandles.find(addr);
			if (!addBase && it != g_ptrHandles.end() && it->second.ptr && sz <= it->second.size) {
				memcpy(it->second.ptr, &val, sz);
			}
			return true;
		}
		case 0x0DDA: { // SEARCH_MEM (safe stub returning 0)
			uint32_t* outVar = g_fnGetVarPtr(&sc.thread, 0);
			g_fnCollectParams(&sc.thread, 2);
			if (outVar) *outVar = 0;
			return true;
		}
		case 0x0DDB: { // GET_GAME_VER_EX
			uint32_t* id = g_fnGetVarPtr(&sc.thread, 0);
			uint32_t* ver = g_fnGetVarPtr(&sc.thread, 0);
			uint32_t* code = g_fnGetVarPtr(&sc.thread, 0);
			if (id) *id = 0x53414D50u;
			if (ver) *ver = 210;
			if (code) *code = 210;
			return true;
		}
		case 0x0DDC: { // SET_MUTEX_VAR
			g_fnCollectParams(&sc.thread, 2);
			g_mutexVars[g_scriptParams[0]] = g_scriptParams[1];
			return true;
		}
		case 0x0DDD: { // GET_MUTEX_VAR
			uint32_t* outVar = g_fnGetVarPtr(&sc.thread, 0);
			g_fnCollectParams(&sc.thread, 1);
			auto it = g_mutexVars.find(g_scriptParams[0]);
			if (outVar) *outVar = (it != g_mutexVars.end()) ? it->second : 0;
			return true;
		}
		case 0x0DDE: { // CALL_FUNCTION (consume varargs safely without crashing)
			g_fnCollectParams(&sc.thread, 2);
			uint8_t* end = sc.code.data() + sc.code.size();
			while (sc.thread.m_pPCPointer < end) {
				if (*sc.thread.m_pPCPointer == 0) {
					sc.thread.m_pPCPointer++;
					break;
				}
				std::string tag;
				if (!readStringParam(sc, tag)) break;
				tag = toLowerAscii(tag);
				if (tag == "i" || tag == "f") {
					g_fnCollectParams(&sc.thread, 1);
				} else if (tag == "ref" || tag == "resi" || tag == "resf") {
					uint32_t* v = g_fnGetVarPtr(&sc.thread, 0);
					if ((tag == "resi" || tag == "resf") && v) *v = 0;
				} else {
					break;
				}
			}
			return true;
		}
		case 0x0DE0: { // GET_TOUCH_POINT_STATE(outVar, zone, minTime)
			uint32_t* outVar = g_fnGetVarPtr(&sc.thread, 0);
			g_fnCollectParams(&sc.thread, 2);
			if (outVar) *outVar = isZoneTouchedTimed(static_cast<int>(g_scriptParams[0]), g_scriptParams[1]) ? 1 : 0;
			return true;
		}
		case 0x0DE1: { // GET_TOUCH_SLIDE_STATE(outVar, from, to, minTime, maxTime)
			uint32_t* outVar = g_fnGetVarPtr(&sc.thread, 0);
			g_fnCollectParams(&sc.thread, 4);
			if (outVar) {
				*outVar = checkSlideDone(static_cast<int>(g_scriptParams[0]),
				                         static_cast<int>(g_scriptParams[1]),
				                         g_scriptParams[2],
				                         g_scriptParams[3]) ? 1 : 0;
			}
			return true;
		}
		case 0x0DE2: { // GET_MENU_BUTTON_STATE
			uint32_t* outVar = g_fnGetVarPtr(&sc.thread, 0);
			if (outVar) *outVar = g_menuBtnDown ? 1 : 0;
			return true;
		}
		case 0x0DE3: { // GET_MENU_BUTTON_PRESSED(outVar, minTime)
			uint32_t* outVar = g_fnGetVarPtr(&sc.thread, 0);
			g_fnCollectParams(&sc.thread, 1);
			uint32_t t = nowMs();
			bool ok = g_menuBtnDown && (t >= g_menuBtnStart && (t - g_menuBtnStart) >= g_scriptParams[0]);
			if (outVar) *outVar = ok ? 1 : 0;
			return true;
		}
		case 0x0DE4: { // GET_PSP_CONTROL_STATE
			uint32_t* outVar = g_fnGetVarPtr(&sc.thread, 0);
			g_fnCollectParams(&sc.thread, 1);
			if (outVar) *outVar = 0;
			return true;
		}
		case 0x0DE5: { // GET_PSP_CONTROL_PRESSED
			uint32_t* outVar = g_fnGetVarPtr(&sc.thread, 0);
			g_fnCollectParams(&sc.thread, 2);
			if (outVar) *outVar = 0;
			return true;
		}
		case 0x0DEE: { // INVOKABLE_SCRIPT_STATS(count, pageCount)
			uint32_t invCount = 0;
			for (const auto& s : g_scripts) {
				if (s.invokable) ++invCount;
			}
			uint32_t* pCount = g_fnGetVarPtr(&sc.thread, 0);
			uint32_t* pPages = g_fnGetVarPtr(&sc.thread, 0);
			if (pCount) *pCount = invCount;
			if (pPages) *pPages = (invCount / 12) + 1;
			return true;
		}
		case 0x0DEF: { // START_INVOKABLE_SCRIPT(outStatus, id)
			uint32_t* outVar = g_fnGetVarPtr(&sc.thread, 0);
			g_fnCollectParams(&sc.thread, 1);
			int32_t id = static_cast<int32_t>(g_scriptParams[0]);
			uint32_t status = static_cast<uint32_t>(-1);
			for (auto& s : g_scripts) {
				if (s.invokable && s.invokableId == id) {
					if (!s.startedOnce) {
						startOrRestartScript(s, false, 0);
						status = 0;
					} else if (!s.thread.m_bActive) {
						startOrRestartScript(s, true, 0);
						status = 1;
					} else {
						status = 2;
					}
					break;
				}
			}
			if (outVar) *outVar = status;
			return true;
		}
		case 0x0DF0: { // SHOW_MENU_OPEN_ARROW
			g_builtinMenu.showArrowHint = true;
			return true;
		}
		case 0x0DF1: { // HIDE_MENU_OPEN_ARROW
			g_builtinMenu.showArrowHint = false;
			return true;
		}
		case 0x0DF2: { // CREATE_MENU(addrHandle, itemCount)
			g_fnCollectParams(&sc.thread, 2);
			uint32_t addr = g_scriptParams[0];
			uint32_t itemCount = std::min<uint32_t>(g_scriptParams[1], 128);
			size_t bufSize = 0;
			const uint8_t* ptr = static_cast<const uint8_t*>(resolveHandleOrAddr(addr, false, &bufSize));
			if (ptr && (bufSize == 0 || bufSize >= 6)) {
				const size_t maxScan = (bufSize > 4) ? (bufSize - 4) : 4096;
				uint8_t flags = ptr[0];
				bool useGxt = (flags & 1) != 0;
				const char* p = reinterpret_cast<const char*>(ptr + 4);
				const char* end = p + maxScan;

				std::string title, closeLabel;
				if (!readBoundedCStr(p, end, title, 128)) {
					return true;
				}
				if (!readBoundedCStr(p, end, closeLabel, 64)) {
					return true;
				}

				std::string logTitle;
				size_t logCount = 0;
				{
					std::lock_guard<std::mutex> lock(g_touchMutex);
					g_scriptMenu = ScriptMenuState{};
					g_scriptMenu.active = true;
					g_scriptMenu.title = useGxt ? lookupGxtUtf8(title) : title;
					g_scriptMenu.closeLabel = closeLabel.empty() ? "CLOSE" : (useGxt ? lookupGxtUtf8(closeLabel) : closeLabel);
					g_scriptMenu.activeIndex = 0;
					g_scriptMenu.page = 0;
					g_scriptMenu.lastTouchedIndex = -1;
					g_scriptMenu.lastTouchedTime = 0;

					while (p < end && *p && itemCount > 0) {
						std::string raw;
						if (!readBoundedCStr(p, end, raw, 128)) break;
						g_scriptMenu.items.push_back(useGxt ? lookupGxtUtf8(raw) : raw);
						--itemCount;
					}
					g_builtinMenu.open = false;
					g_menuRowRects.clear();
					logTitle = g_scriptMenu.title;
					logCount = g_scriptMenu.items.size();
				}
				cleoLog("Script '%s' opened 0DF2 menu '%s' (%zu items)",
				        sc.fileName.c_str(), logTitle.c_str(), logCount);
			}
			return true;
		}
		case 0x0DF3: { // DELETE_MENU
			std::lock_guard<std::mutex> lock(g_touchMutex);
			g_scriptMenu.active = false;
			g_scriptMenu.items.clear();
			g_menuRowRects.clear();
			return true;
		}
		case 0x0DF4: { // GET_MENU_TOUCHED_ITEM_INDEX(outIdx, maxTimeMs)
			int32_t* outVar = reinterpret_cast<int32_t*>(g_fnGetVarPtr(&sc.thread, 0));
			g_fnCollectParams(&sc.thread, 1);
			uint32_t maxTime = g_scriptParams[0];
			int32_t res = -1;
			uint32_t t = nowMs();
			{
				std::lock_guard<std::mutex> lock(g_touchMutex);
				if (g_scriptMenu.lastTouchedIndex != -1 && t >= g_scriptMenu.lastTouchedTime &&
				    (maxTime == 0 || (t - g_scriptMenu.lastTouchedTime) <= maxTime)) {
					res = g_scriptMenu.lastTouchedIndex;
					g_scriptMenu.lastTouchedIndex = -1;
				}
			}
			if (outVar) *outVar = res;
			return true;
		}
		case 0x0DF5: { // SET_MENU_ACTIVE_ITEM_INDEX(idx)
			g_fnCollectParams(&sc.thread, 1);
			int32_t idx = static_cast<int32_t>(g_scriptParams[0]);
			std::lock_guard<std::mutex> lock(g_touchMutex);
			if (idx >= 0 && idx < static_cast<int32_t>(g_scriptMenu.items.size())) {
				g_scriptMenu.activeIndex = idx;
				g_scriptMenu.page = idx / kMenuItemsPerPage;
			}
			return true;
		}
		case 0x0DF6: { // GET_MENU_ACTIVE_ITEM_INDEX(outIdx)
			int32_t* outVar = reinterpret_cast<int32_t*>(g_fnGetVarPtr(&sc.thread, 0));
			std::lock_guard<std::mutex> lock(g_touchMutex);
			if (outVar) *outVar = g_scriptMenu.activeIndex;
			return true;
		}

		// -------- Popular PC CLEO4 Opcodes --------
		case 0x0A8E: // int add
		case 0x0A8F: // int sub
		case 0x0A90: // int mul
		case 0x0A91: { // int div
			g_fnCollectParams(&sc.thread, 2);
			int32_t a = static_cast<int32_t>(g_scriptParams[0]);
			int32_t b = static_cast<int32_t>(g_scriptParams[1]);
			int32_t r = 0;
			if (op == 0x0A8E) r = static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
			else if (op == 0x0A8F) r = static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
			else if (op == 0x0A90) r = static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
			else r = (b != 0 && !(a == INT32_MIN && b == -1)) ? (a / b) : 0;
			g_scriptParams[0] = static_cast<uint32_t>(r);
			g_fnStoreParams(&sc.thread, 1);
			return true;
		}
		case 0x0A99: { // SET_CURRENT_DIRECTORY(0=userdir/cleo, 1=root/files)
			g_fnCollectParams(&sc.thread, 1);
			g_currentDir = (g_scriptParams[0] == 1) ? g_gameDir : g_cleoDir;
			return true;
		}
		case 0x0A9A: { // OPENFILE(path, mode, outHandle)
			std::string path, mode;
			readStringParam(sc, path);
			readStringParam(sc, mode);
			uint32_t* outVar = g_fnGetVarPtr(&sc.thread, 0);
			std::string resolved = resolveScriptPath(path);
			FILE* fp = (g_openFiles.size() < 64 && !resolved.empty() && !mode.empty()) ? fopen(resolved.c_str(), mode.c_str()) : nullptr;
			if (fp) {
				uint32_t fh = g_nextFileHandle++;
				g_openFiles[fh] = fp;
				if (outVar) *outVar = fh;
				setCond(true);
			} else {
				if (outVar) *outVar = 0;
				setCond(false);
			}
			return true;
		}
		case 0x0A9B: { // CLOSEFILE(handle)
			g_fnCollectParams(&sc.thread, 1);
			auto it = g_openFiles.find(g_scriptParams[0]);
			if (it != g_openFiles.end()) {
				if (it->second) fclose(it->second);
				g_openFiles.erase(it);
			}
			return true;
		}
		case 0x0A9C: { // GET_FILE_SIZE(handle, outSize)
			g_fnCollectParams(&sc.thread, 1);
			uint32_t* outVar = g_fnGetVarPtr(&sc.thread, 0);
			uint32_t sz = 0;
			auto it = g_openFiles.find(g_scriptParams[0]);
			if (it != g_openFiles.end() && it->second) {
				long cur = ftell(it->second);
				fseek(it->second, 0, SEEK_END);
				long end = ftell(it->second);
				fseek(it->second, cur, SEEK_SET);
				if (end > 0) sz = static_cast<uint32_t>(end);
			}
			if (outVar) *outVar = sz;
			return true;
		}
		case 0x0A9D: { // READFILE(handle, size, outVar)
			g_fnCollectParams(&sc.thread, 2);
			uint32_t* outVar = g_fnGetVarPtr(&sc.thread, 0);
			auto it = g_openFiles.find(g_scriptParams[0]);
			uint32_t sz = std::min<uint32_t>(g_scriptParams[1], 4);
			if (outVar) {
				*outVar = 0;
				if (it != g_openFiles.end() && it->second) {
					fread(outVar, 1, sz, it->second);
				}
			}
			return true;
		}
		case 0x0A9E: { // WRITEFILE(handle, size, val)
			g_fnCollectParams(&sc.thread, 3);
			auto it = g_openFiles.find(g_scriptParams[0]);
			uint32_t sz = std::min<uint32_t>(g_scriptParams[1], 4);
			uint32_t val = g_scriptParams[2];
			if (it != g_openFiles.end() && it->second) {
				fwrite(&val, 1, sz, it->second);
			}
			return true;
		}
		case 0x0AA0: { // GOSUB_IF_FALSE
			uint8_t* target = nullptr;
			if (readTargetLabelOffset(sc, target) && sc.thread.m_bCmpFlag == 0) {
				if (sc.thread.m_nStackDepth < 8) {
					sc.thread.m_pPCStack[sc.thread.m_nStackDepth++] = sc.thread.m_pPCPointer;
					sc.thread.m_pPCPointer = target;
				}
			}
			return true;
		}
		case 0x0AA1: { // RETURN_IF_FALSE
			if (sc.thread.m_bCmpFlag == 0 && sc.thread.m_nStackDepth > 0) {
				sc.thread.m_pPCPointer = sc.thread.m_pPCStack[--sc.thread.m_nStackDepth];
			}
			return true;
		}
		case 0x0AAB: { // FILE_EXISTS(path)
			std::string path;
			readStringParam(sc, path);
			struct stat st{};
			std::string resolved = resolveScriptPath(path);
			setCond(!resolved.empty() && stat(resolved.c_str(), &st) == 0 && !S_ISDIR(st.st_mode));
			return true;
		}
		case 0x0AB1: { // CALL_SCM_FUNC(label, argCount, args..., outVars...)
			uint8_t* target = nullptr;
			if (!readTargetLabelOffset(sc, target)) return true;
			g_fnCollectParams(&sc.thread, 1);
			int argCount = std::clamp<int>(static_cast<int>(g_scriptParams[0]), 0, 32);
			uint32_t args[32]{};
			if (argCount > 0) {
				g_fnCollectParams(&sc.thread, static_cast<int16_t>(argCount));
				memcpy(args, g_scriptParams, sizeof(uint32_t) * argCount);
			}
			if (static_cast<int>(sc.callFrames.size()) >= kMaxScmCallDepth) {
				cleoLog("Script '%s': 0AB1 call depth exceeded, terminating", sc.fileName.c_str());
				sc.thread.m_bActive = 0;
				return true;
			}
			ScmCallFrame frame{};
			frame.returnIp = sc.thread.m_pPCPointer; // points at return output vars (terminated by 0x00)
			memcpy(frame.savedLocals, sc.thread.m_nLocals, sizeof(frame.savedLocals));
			sc.callFrames.push_back(frame);

			memset(sc.thread.m_nLocals, 0, sizeof(uint32_t) * 32);
			for (int i = 0; i < argCount; ++i) {
				sc.thread.m_nLocals[i] = args[i];
			}
			sc.thread.m_pPCPointer = target;
			return true;
		}
		case 0x0AB2:   // RET(retCount, retVals...)
		case 0x0AF6:   // RET_IF_FALSE
		case 0x0AF7: { // RET_IF_TRUE
			g_fnCollectParams(&sc.thread, 1);
			int retCount = std::clamp<int>(static_cast<int>(g_scriptParams[0]), 0, 32);
			uint32_t rets[32]{};
			if (retCount > 0) {
				g_fnCollectParams(&sc.thread, static_cast<int16_t>(retCount));
				memcpy(rets, g_scriptParams, sizeof(uint32_t) * retCount);
			}
			if (sc.callFrames.empty()) {
				sc.thread.m_bActive = 0;
				return true;
			}
			ScmCallFrame frame = sc.callFrames.back();
			sc.callFrames.pop_back();
			memcpy(sc.thread.m_nLocals, frame.savedLocals, sizeof(frame.savedLocals));
			sc.thread.m_pPCPointer = frame.returnIp;
			uint8_t* codeEnd = sc.code.data() + sc.code.size();
			for (int i = 0; i < retCount; ++i) {
				if (sc.thread.m_pPCPointer >= codeEnd || *sc.thread.m_pPCPointer == 0) {
					break;
				}
				if (uint32_t* dst = g_fnGetVarPtr(&sc.thread, 0)) {
					*dst = rets[i];
				}
			}
			// Skip vararg terminator byte (0x00)
			if (sc.thread.m_pPCPointer < codeEnd && *sc.thread.m_pPCPointer == 0) {
				sc.thread.m_pPCPointer++;
			}
			if (op == 0x0AF6) setCond(false);
			else if (op == 0x0AF7) setCond(true);
			return true;
		}
		case 0x0AB3: { // SET_CLEO_SHARED_VAR(idx, val)
			g_fnCollectParams(&sc.thread, 2);
			uint32_t idx = g_scriptParams[0];
			if (idx < kMaxSharedVars) g_sharedVars[idx] = g_scriptParams[1];
			return true;
		}
		case 0x0AB4: { // GET_CLEO_SHARED_VAR(idx, outVal)
			g_fnCollectParams(&sc.thread, 1);
			uint32_t idx = g_scriptParams[0];
			uint32_t* outVar = g_fnGetVarPtr(&sc.thread, 0);
			if (outVar) *outVar = (idx < kMaxSharedVars) ? g_sharedVars[idx] : 0;
			return true;
		}
		case 0x0ABA: { // TERMINATE_ALL_CUSTOM_SCRIPTS_WITH_THIS_NAME
			std::string name;
			readStringParam(sc, name);
			std::string target = toLowerAscii(name);
			for (auto& s : g_scripts) {
				if (toLowerAscii(s.thread.m_szScriptName) == target ||
				    toLowerAscii(s.displayName) == target ||
				    toLowerAscii(s.fileName) == target) {
					s.thread.m_bActive = 0;
				}
			}
			return true;
		}
		case 0x0AC8: { // ALLOCATE_MEMORY(size, outPtrHandle)
			g_fnCollectParams(&sc.thread, 1);
			uint32_t sz = std::clamp<uint32_t>(g_scriptParams[0], 1, 4 * 1024 * 1024);
			uint32_t* outVar = g_fnGetVarPtr(&sc.thread, 0);
			void* mem = calloc(1, sz);
			if (outVar) *outVar = mem ? registerHandle(mem, sz, true) : 0;
			setCond(mem != nullptr);
			return true;
		}
		case 0x0AC9: { // FREE_MEMORY(ptrHandle)
			g_fnCollectParams(&sc.thread, 1);
			auto it = g_ptrHandles.find(g_scriptParams[0]);
			if (it != g_ptrHandles.end() && it->second.owned) {
				free(it->second.ptr);
				g_ptrHandles.erase(it);
			}
			return true;
		}
		case 0x0ACA: { // SHOW_TEXT_BOX(text)
			std::string text;
			readStringParam(sc, text);
			if (g_fnSetHelpMsg) {
				g_fnSetHelpMsg(nullptr, makeTempGxt(text), true, false, false, 0);
			}
			return true;
		}
		case 0x0ACB: { // SHOW_STYLED_TEXT(text, timeMs, style)
			std::string text;
			readStringParam(sc, text);
			g_fnCollectParams(&sc.thread, 2);
			if (g_fnAddBigMessage) {
				g_fnAddBigMessage(makeTempGxt(text), g_scriptParams[0], static_cast<uint16_t>(g_scriptParams[1]));
			}
			return true;
		}
		case 0x0ACC: { // SHOW_TEXT_LOWPRIORITY(text, timeMs)
			std::string text;
			readStringParam(sc, text);
			g_fnCollectParams(&sc.thread, 1);
			if (g_fnAddMessage) {
				g_fnAddMessage(nullptr, makeTempGxt(text), g_scriptParams[0], 0, false);
			}
			return true;
		}
		case 0x0ACD: { // SHOW_TEXT_HIGHPRIORITY(text, timeMs)
			std::string text;
			readStringParam(sc, text);
			g_fnCollectParams(&sc.thread, 1);
			if (g_fnAddMessageJumpQ) {
				g_fnAddMessageJumpQ(nullptr, makeTempGxt(text), g_scriptParams[0], 0, false);
			}
			return true;
		}
		case 0x0AD3: { // STRING_FORMAT(outBuf, fmt, args...)
			// Destination can be a short/long string var or an allocated 0AC8 handle
			uint8_t* codeEnd = sc.code.data() + sc.code.size();
			char* dstBuf = nullptr;
			size_t dstCap = 0;
			if (sc.thread.m_pPCPointer < codeEnd) {
				uint8_t t = *sc.thread.m_pPCPointer;
				if (t == sa::SCRIPT_PARAM_GLOBAL_SHORT_STRING_VARIABLE || t == sa::SCRIPT_PARAM_LOCAL_SHORT_STRING_VARIABLE) {
					dstBuf = reinterpret_cast<char*>(g_fnGetVarPtr(&sc.thread, 0));
					dstCap = 8;
				} else if (t == sa::SCRIPT_PARAM_GLOBAL_LONG_STRING_VARIABLE || t == sa::SCRIPT_PARAM_LOCAL_LONG_STRING_VARIABLE) {
					dstBuf = reinterpret_cast<char*>(g_fnGetVarPtr(&sc.thread, 0));
					dstCap = 16;
				} else {
					g_fnCollectParams(&sc.thread, 1);
					auto it = g_ptrHandles.find(g_scriptParams[0]);
					if (it != g_ptrHandles.end() && it->second.owned && it->second.ptr && it->second.size > 0) {
						dstBuf = static_cast<char*>(it->second.ptr);
						dstCap = it->second.size;
					}
				}
			}
			std::string fmt;
			readStringParam(sc, fmt);

			// Collect varargs up to 0x00 terminator
			struct FmtArg {
				bool isStr = false;
				uint32_t val = 0;
				std::string str;
			};
			std::vector<FmtArg> args;
			int drained = 0;
			while (sc.thread.m_pPCPointer < codeEnd && drained < 64) {
				uint8_t pt = *sc.thread.m_pPCPointer;
				if (pt == 0) {
					sc.thread.m_pPCPointer++;
					break;
				}
				FmtArg a{};
				if (pt == sa::SCRIPT_PARAM_STATIC_SHORT_STRING ||
				    pt == sa::SCRIPT_PARAM_STATIC_PASCAL_STRING ||
				    pt == sa::SCRIPT_PARAM_STATIC_LONG_STRING ||
				    pt == sa::SCRIPT_PARAM_GLOBAL_SHORT_STRING_VARIABLE ||
				    pt == sa::SCRIPT_PARAM_LOCAL_SHORT_STRING_VARIABLE ||
				    pt == sa::SCRIPT_PARAM_GLOBAL_LONG_STRING_VARIABLE ||
				    pt == sa::SCRIPT_PARAM_LOCAL_LONG_STRING_VARIABLE) {
					a.isStr = true;
					readStringParam(sc, a.str);
				} else {
					g_fnCollectParams(&sc.thread, 1);
					a.val = g_scriptParams[0];
				}
				if (args.size() < 16) {
					args.push_back(std::move(a));
				}
				++drained;
			}

			// Format safely (no unchecked printf with user format string)
			std::string formatted;
			size_t argIdx = 0;
			for (size_t i = 0; i < fmt.size(); ++i) {
				if (fmt[i] != '%' || i + 1 >= fmt.size()) {
					formatted.push_back(fmt[i]);
					continue;
				}
				if (fmt[i + 1] == '%') {
					formatted.push_back('%');
					++i;
					continue;
				}
				size_t specStart = i;
				++i;
				int dotCount = 0;
				while (i < fmt.size() && (fmt[i] == '-' || fmt[i] == '+' || fmt[i] == ' ' || fmt[i] == '#' || fmt[i] == '0' ||
				       (fmt[i] >= '0' && fmt[i] <= '9') || fmt[i] == '.')) {
					if (fmt[i] == '.') ++dotCount;
					++i;
				}
				if (i >= fmt.size()) break;
				char spec = fmt[i];
				std::string specStr = fmt.substr(specStart, i - specStart + 1);
				if (specStr.size() > 16 || dotCount > 1) {
					formatted += specStr;
					continue;
				}
				char tmp[128]{};
				if (argIdx < args.size()) {
					const FmtArg& a = args[argIdx++];
					if (spec == 's') {
						if (a.isStr) {
							formatted += a.str;
						} else {
							size_t sBufSize = 0;
							if (void* p = resolveHandleOrAddr(a.val, false, &sBufSize)) {
								const char* cs = static_cast<const char*>(p);
								const size_t maxS = (sBufSize > 0 && sBufSize < 256) ? sBufSize : 256;
								std::string safeS;
								readBoundedCStr(cs, cs + maxS, safeS, maxS);
								formatted += safeS;
							}
						}
					} else if (spec == 'f' || spec == 'F' || spec == 'g' || spec == 'G' || spec == 'e' || spec == 'E') {
						float fv = 0.0f;
						memcpy(&fv, &a.val, 4);
						snprintf(tmp, sizeof(tmp), specStr.c_str(), static_cast<double>(fv));
						formatted += tmp;
					} else if (spec == 'd' || spec == 'i') {
						snprintf(tmp, sizeof(tmp), specStr.c_str(), static_cast<int32_t>(a.val));
						formatted += tmp;
					} else if (spec == 'u' || spec == 'x' || spec == 'X' || spec == 'o') {
						snprintf(tmp, sizeof(tmp), specStr.c_str(), a.val);
						formatted += tmp;
					} else if (spec == 'c') {
						formatted.push_back(static_cast<char>(a.val & 0xFF));
					} else {
						formatted += specStr;
					}
				}
			}

			if (dstBuf && dstCap > 0) {
				size_t copyLen = std::min(formatted.size(), dstCap - 1);
				memcpy(dstBuf, formatted.data(), copyLen);
				dstBuf[copyLen] = '\0';
			}
			return true;
		}
		case 0x0ADF: { // ADD_DYNAMIC_GXT_ENTRY(key, text)
			std::string key, text;
			readStringParam(sc, key);
			readStringParam(sc, text);
			addCustomGxtEntry(key, text);
			return true;
		}
		case 0x0AE0: { // REMOVE_DYNAMIC_GXT_ENTRY(key)
			std::string key;
			readStringParam(sc, key);
			removeCustomGxtEntry(key);
			return true;
		}
		case 0x0AEE: { // POW(base, exp, out)
			g_fnCollectParams(&sc.thread, 2);
			float a = 0.0f, b = 0.0f;
			memcpy(&a, &g_scriptParams[0], 4);
			memcpy(&b, &g_scriptParams[1], 4);
			float r = std::pow(a, b);
			memcpy(&g_scriptParams[0], &r, 4);
			g_fnStoreParams(&sc.thread, 1);
			return true;
		}
		case 0x0AEF: { // LOG(val, base, out)
			g_fnCollectParams(&sc.thread, 2);
			float a = 0.0f, b = 0.0f;
			memcpy(&a, &g_scriptParams[0], 4);
			memcpy(&b, &g_scriptParams[1], 4);
			float r = (a > 0.0f && b > 0.0f && b != 1.0f) ? (std::log(a) / std::log(b)) : 0.0f;
			memcpy(&g_scriptParams[0], &r, 4);
			g_fnStoreParams(&sc.thread, 1);
			return true;
		}
		case 0x0AFF: { // SET_COMPARE_FLAG(flag)
			g_fnCollectParams(&sc.thread, 1);
			setCond(g_scriptParams[0] != 0);
			return true;
		}
		case 0x0CB2: { // ATOF(str, outFloat)
			std::string s;
			readStringParam(sc, s);
			float f = static_cast<float>(atof(s.c_str()));
			memcpy(&g_scriptParams[0], &f, 4);
			g_fnStoreParams(&sc.thread, 1);
			return true;
		}
		case 0x0CB3: { // ATOI(str, outInt)
			std::string s;
			readStringParam(sc, s);
			int32_t v = atoi(s.c_str());
			g_scriptParams[0] = static_cast<uint32_t>(v);
			g_fnStoreParams(&sc.thread, 1);
			return true;
		}
		case 0x0CB4: { // GET_SCREEN_RES(outW, outH)
			float w = 1920.0f, h = 1080.0f;
			{
				std::lock_guard<std::mutex> lock(g_touchMutex);
				w = g_screenW;
				h = g_screenH;
			}
			g_scriptParams[0] = static_cast<uint32_t>(w);
			g_scriptParams[1] = static_cast<uint32_t>(h);
			g_fnStoreParams(&sc.thread, 2);
			return true;
		}
		case 0x0CB5: { // IS_ANY_FINGER_ONSCREEN
			setCond(isAnyFingerDown());
			return true;
		}
		default:
			return false;
	}
}

void stepScript(CleoScript& sc, uint32_t currentTime)
{
	if (!sc.thread.m_bActive) return;
	if (currentTime < static_cast<uint32_t>(sc.thread.m_nActivateTime)) return;
	sc.waitPassed = true;

	uint8_t* codeStart = sc.code.data();
	uint8_t* codeEnd = codeStart + sc.code.size();

	for (int step = 0; step < kMaxStepsPerTick && sc.thread.m_bActive; ++step) {
		uint8_t* ip = sc.thread.m_pPCPointer;
		if (ip < codeStart || ip + 2 > codeEnd) {
			cleoLog("Script '%s': IP out of bounds (%td / %zu), terminating",
			        sc.fileName.c_str(), ip - codeStart, sc.code.size());
			sc.thread.m_bActive = 0;
			break;
		}

		uint16_t rawOp = 0;
		memcpy(&rawOp, ip, 2);

		// CLEO Android 3x3 touch-zone shortcut via opcode 00E1 / 80E1 with pad=3
		if ((rawOp == 0x00E1 || rawOp == 0x80E1) && ip + 6 <= codeEnd) {
			uint16_t p1 = 0;
			memcpy(&p1, ip + 2, 2);
			if (p1 == 0x0304 && ip[4] == 0x04) {
				int zone = ip[5];
				bool touched = isZoneTouchedTimed(zone, 0);
				sc.thread.m_bNotForLatestExpression = (rawOp == 0x80E1) ? 1 : 0;
				g_fnUpdateCmpFlag(&sc.thread, touched ? 1 : 0);
				sc.thread.m_pPCPointer = ip + 6;
				continue;
			}
		}

		// Try custom CLEO opcode first (advance past 2-byte opcode)
		sc.thread.m_pPCPointer = ip + 2;
		if (handleCustomOpcode(sc, rawOp)) {
			continue;
		}

		// Standard GTA:SA 2.10 CRunningScript::ProcessOneCommand only dispatches opcodes 0..2699 (0x0000..0x0A8B).
		// Reject any unhandled CLEO4/CLEO5/SAMPFUNCS opcode (> 2699) cleanly instead of risking native OOB crash.
		const uint16_t maskedOp = rawOp & 0x7FFFu;
		if (maskedOp > 2699u) {
			cleoLog("Script '%s': unsupported custom opcode 0x%04X at offset %td, terminating script safely",
			        sc.fileName.c_str(), maskedOp, ip - codeStart);
			sc.thread.m_bActive = 0;
			break;
		}

		// Rewind and delegate standard GTA:SA opcode to native CRunningScript::ProcessOneCommand
		sc.thread.m_pPCPointer = ip;
		uint8_t yielded = g_fnProcessOneCmd(&sc.thread);
		if (yielded != 0) {
			break;
		}
	}
}

void syncPlayerScriptGlobals()
{
	if (!g_scriptSpace) return;
	sa::CPed* localPed = GamePool_FindPlayerPed();
	if (!localPed) return;

	// $PLAYER_CHAR (+0x08) = 0, $PLAYER_ACTOR (+0x0C) = local player ped handle
	int32_t playerChar = 0;
	int32_t playerActor = GamePool_Ped_GetIndex(localPed);
	memcpy(g_scriptSpace + 0x08, &playerChar, 4);
	memcpy(g_scriptSpace + 0x0C, &playerActor, 4);
}

// ---------------------------------------------------------------------------
// Menu Helpers (Built-in .csi Launcher & Script 0DF2 Menu)
// ---------------------------------------------------------------------------
bool isAnyMenuVisible()
{
	return g_scriptMenu.active || g_builtinMenu.open;
}

std::vector<int> collectInvokableScriptIndices()
{
	std::vector<int> out;
	for (int i = 0; i < static_cast<int>(g_scripts.size()); ++i) {
		if (g_scripts[i].invokable) out.push_back(i);
	}
	return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void CleoEngine::initialize(const std::string& gameDir)
{
	// Clean up any previous session state before (re)initializing
	{
		std::lock_guard<std::mutex> lock(g_logMutex);
		if (g_logFile) {
			fclose(g_logFile);
			g_logFile = nullptr;
		}
	}
	for (auto& kv : g_openFiles) {
		if (kv.second) fclose(kv.second);
	}
	g_openFiles.clear();
	for (auto& kv : g_ptrHandles) {
		if (kv.second.owned && kv.second.ptr) free(kv.second.ptr);
	}
	g_ptrHandles.clear();
	g_nextHandle = kHandleBase;
	g_nextFileHandle = 0x5F000001u;
	{
		std::lock_guard<std::mutex> lock(g_gxtMutex);
		g_customGxt.clear();
	}
	g_mutexVars.clear();
	g_sharedVars.fill(0);
	{
		std::lock_guard<std::mutex> lock(g_touchMutex);
		g_scriptMenu = ScriptMenuState{};
		g_builtinMenu = BuiltinMenuState{};
		g_menuRowRects.clear();
		g_pendingStartScriptIdx = -1;
		resetTouchFingers();
		g_lastSlide = SlideRecord{};
	}

	g_enabled = Settings::cleo();
	g_scripts.clear();
	g_scriptsBooted = false;
	g_gameDir = gameDir;
	if (!g_gameDir.empty() && g_gameDir.back() != '/') g_gameDir.push_back('/');
	g_cleoDir = g_gameDir + "cleo/";
	g_currentDir = g_cleoDir;

	if (!g_enabled) {
		spdlog::info("[CLEO] Disabled by settings (cleo_scripts=false)");
		return;
	}

	mkdir(g_cleoDir.c_str(), 0775);
	g_logPath = g_cleoDir + "cleo.log";
	{
		std::lock_guard<std::mutex> lock(g_logMutex);
		g_logFile = fopen(g_logPath.c_str(), "w");
	}

	g_gtasaBase = g_saSym->GetLibAddr();
	g_scriptParams = g_saSym->GetSymbol<uint32_t*>("ScriptParams");
	g_scriptSpace = g_saSym->GetSymbol<uint8_t*>("_ZN11CTheScripts11ScriptSpaceE");
	g_timerMs = g_saSym->GetSymbol<uint32_t*>("_ZN6CTimer22m_snTimeInMillisecondsE");
	g_theText = g_saSym->GetSymbol<void*>("TheText");

	g_fnInitScript = g_saSym->GetSymbol<FnInitScript>("_ZN14CRunningScript4InitEv");
	g_fnProcessOneCmd = g_saSym->GetSymbol<FnProcessOneCmd>("_ZN14CRunningScript17ProcessOneCommandEv");
	g_fnCollectParams = g_saSym->GetSymbol<FnCollectParams>("_ZN14CRunningScript17CollectParametersEs");
	g_fnStoreParams = g_saSym->GetSymbol<FnStoreParams>("_ZN14CRunningScript15StoreParametersEs");
	g_fnUpdateCmpFlag = g_saSym->GetSymbol<FnUpdateCmpFlag>("_ZN14CRunningScript17UpdateCompareFlagEh");
	g_fnGetVarPtr = g_saSym->GetSymbol<FnGetVarPtr>("_ZN14CRunningScript26GetPointerToScriptVariableEh");
	g_fnAsciiToGxt = g_saSym->GetSymbol<FnAsciiToGxt>("_Z14AsciiToGxtCharPKcPt");
	g_fnSetHelpMsg = g_saSym->GetSymbol<FnSetHelpMsg>("_ZN4CHud14SetHelpMessageEPKcPtbbbj");
	g_fnAddMessage = g_saSym->GetSymbol<FnAddMessage>("_ZN9CMessages10AddMessageEPKcPtjtb");
	g_fnAddMessageJumpQ = g_saSym->GetSymbol<FnAddMessage>("_ZN9CMessages15AddMessageJumpQEPKcPtjtb");
	g_fnAddBigMessage = g_saSym->GetSymbol<FnAddBigMessage>("_ZN9CMessages13AddBigMessageEPtjt");
	g_fnCTextGet = g_saSym->GetSymbol<FnCTextGet>("_ZN5CText3GetEPKc");

	g_symbolsReady = (g_scriptParams && g_scriptSpace && g_timerMs &&
	                  g_fnInitScript && g_fnProcessOneCmd && g_fnCollectParams &&
	                  g_fnStoreParams && g_fnUpdateCmpFlag && g_fnGetVarPtr);

	cleoLog("SAMP-Mobile Native CLEO Engine initialized (%s, GTA:SA 2.10)",
	        VER_x32 ? "armeabi-v7a" : "arm64-v8a");
	if (!g_symbolsReady) {
		cleoLog("ERROR: Required libGTASA.so script symbols missing; CLEO runtime paused.");
		g_enabled = false;
		return;
	}

	// Scan files/cleo/ (scripts + .fxt) and files/cleo/cleo_text/ (.fxt only)
	scanDirectory(g_cleoDir, true);
	scanDirectory(g_cleoDir + "cleo_text/", false);

	size_t autoCount = 0, invCount = 0;
	for (const auto& s : g_scripts) {
		if (s.invokable) ++invCount;
		else ++autoCount;
	}
	cleoLog("Scan complete: %zu auto (.csa/.cs), %zu invokable (.csi), %zu custom GXT entries",
	        autoCount, invCount, g_customGxt.size());
}

bool CleoEngine::isEnabled()
{
	return g_enabled && g_symbolsReady;
}

void CleoEngine::toggleMenu()
{
	if (!g_enabled || !g_symbolsReady) return;
	std::lock_guard<std::mutex> lock(g_touchMutex);
	if (g_scriptMenu.active) {
		g_scriptMenu.lastTouchedIndex = -2;
		g_scriptMenu.lastTouchedTime = nowMs();
		g_scriptMenu.active = false;
		g_menuRowRects.clear();
	} else {
		g_builtinMenu.open = !g_builtinMenu.open;
		g_builtinMenu.page = 0;
		if (!g_builtinMenu.open) g_menuRowRects.clear();
	}
}

void CleoEngine::process()
{
	if (!g_enabled || !g_symbolsReady) return;
	if (Game::IsGamePaused()) {
		std::lock_guard<std::mutex> lock(g_touchMutex);
		resetTouchFingers();
		return;
	}
	if (!GamePool_FindPlayerPed()) return;

	uint32_t t = nowMs();
	int startIdx = -1;
	{
		std::lock_guard<std::mutex> lock(g_touchMutex);
		// Expire any stuck touch finger if a POP event was lost across background/dialog transitions
		for (auto& f : g_fingers) {
			if (f.down && t > f.lastZoneTime && (t - f.lastZoneTime) > 15000u) {
				f = TouchFinger{};
			}
		}
		startIdx = g_pendingStartScriptIdx;
		g_pendingStartScriptIdx = -1;
	}
	if (startIdx >= 0 && startIdx < static_cast<int>(g_scripts.size())) {
		CleoScript& sc = g_scripts[startIdx];
		if (!sc.thread.m_bActive) {
			startOrRestartScript(sc, sc.startedOnce, 0);
		}
	}
	if (!g_scriptsBooted) {
		g_scriptsBooted = true;
		g_lastTickMs = t;
		syncPlayerScriptGlobals();
		for (auto& sc : g_scripts) {
			if (!sc.invokable) {
				startOrRestartScript(sc, false, 1500);
			}
		}
		cleoLog("World ready — booted auto CLEO scripts (timer=%u)", t);
		return;
	}

	uint32_t delta = (t >= g_lastTickMs) ? std::min<uint32_t>(t - g_lastTickMs, 250) : 0;
	g_lastTickMs = t;

	syncPlayerScriptGlobals();

	for (auto& sc : g_scripts) {
		if (!sc.thread.m_bActive) continue;
		sc.thread.m_nLocals[32] += delta;
		sc.thread.m_nLocals[33] += delta;
		stepScript(sc, t);
	}
}

uint16_t* CleoEngine::getCustomGxt(const char* key)
{
	if (!g_enabled || !key || !key[0]) return nullptr;
	std::string upper = toUpperAscii(key);
	std::lock_guard<std::mutex> lock(g_gxtMutex);
	auto it = g_customGxt.find(upper);
	if (it != g_customGxt.end() && !it->second.empty()) {
		return it->second.data();
	}
	return nullptr;
}

bool CleoEngine::handleTouch(int type, int num, int x, int y, float screenW, float screenH)
{
	if (!g_enabled || !g_symbolsReady) return false;
	std::lock_guard<std::mutex> lock(g_touchMutex);
	if (screenW > 1.0f && screenH > 1.0f) {
		g_screenW = screenW;
		g_screenH = screenH;
	}

	const float fx = static_cast<float>(x);
	const float fy = static_cast<float>(y);
	const uint32_t t = nowMs();

	// 1) If a CLEO menu overlay is visible, consume touches on it
	if (isAnyMenuVisible()) {
		if (type == 1) { // POP (tap release)
			if (g_menuCloseRect.contains(fx, fy) || !g_menuPanelRect.contains(fx, fy)) {
				if (g_scriptMenu.active) {
					g_scriptMenu.lastTouchedIndex = -2;
					g_scriptMenu.lastTouchedTime = t;
					g_scriptMenu.active = false;
				} else {
					g_builtinMenu.open = false;
				}
				g_menuRowRects.clear();
				return true;
			}
			if (g_menuPrevRect.contains(fx, fy)) {
				if (g_scriptMenu.active && g_scriptMenu.page > 0) --g_scriptMenu.page;
				else if (g_builtinMenu.open && g_builtinMenu.page > 0) --g_builtinMenu.page;
				return true;
			}
			if (g_menuNextRect.contains(fx, fy)) {
				if (g_scriptMenu.active) {
					int maxPage = std::max(0, static_cast<int>(g_scriptMenu.items.size() - 1) / kMenuItemsPerPage);
					if (g_scriptMenu.page < maxPage) ++g_scriptMenu.page;
				} else if (g_builtinMenu.open) {
					auto inv = collectInvokableScriptIndices();
					int maxPage = inv.empty() ? 0 : static_cast<int>(inv.size() - 1) / kMenuItemsPerPage;
					if (g_builtinMenu.page < maxPage) ++g_builtinMenu.page;
				}
				return true;
			}
			for (const auto& entry : g_menuRowRects) {
				if (entry.second.contains(fx, fy)) {
					if (g_scriptMenu.active) {
						g_scriptMenu.activeIndex = entry.first;
						g_scriptMenu.lastTouchedIndex = entry.first;
						g_scriptMenu.lastTouchedTime = t;
					} else if (g_builtinMenu.open) {
						int idx = entry.first;
						if (idx >= 0 && idx < static_cast<int>(g_scripts.size())) {
							// Defer startOrRestartScript to GameMain::CleoEngine::process() (never call GTA CRunningScript::Init on NVEvent thread!)
							g_pendingStartScriptIdx = idx;
							g_builtinMenu.open = false;
							g_menuRowRects.clear();
						}
					}
					return true;
				}
			}
		}
		return true;
	}

	// 2) Track 3x3 CLEO touch zones & swipe gestures (2 -> 8 opens CLEO .csi menu)
	if (type == 4) { // ACTION_CANCEL resets all fingers so none stay stuck
		resetTouchFingers();
		return false;
	}
	if (num >= 0 && num < 8) {
		int zone = calcTouchZone(fx, fy, g_screenW, g_screenH);
		TouchFinger& f = g_fingers[num];
		if (type == 2) { // PUSH
			f.down = true;
			f.startZone = zone;
			f.currentZone = zone;
			f.startTime = t;
			f.lastZoneTime = t;
		} else if (type == 3) { // MOVE
			if (!f.down) {
				f.down = true;
				f.startZone = zone;
				f.startTime = t;
			}
			if (zone != f.currentZone) {
				f.currentZone = zone;
				f.lastZoneTime = t;
			}
		} else if (type == 1 || type == 4) { // POP / CANCEL
			if (f.down && f.startZone > 0 && zone > 0 && f.startZone != zone) {
				uint32_t dur = (t >= f.startTime) ? (t - f.startTime) : 0;
				g_lastSlide = SlideRecord{f.startZone, zone, dur, t};

				// Classic Android CLEO swipe gesture: top-center (2) -> bottom-center (8)
				if (f.startZone == 2 && zone == 8 && dur >= 100 && dur <= 1200 && !g_scripts.empty()) {
					g_builtinMenu.open = true;
					g_builtinMenu.page = 0;
					cleoLog("Opened CLEO menu via swipe gesture (2->8, %ums)", dur);
				}
			}
			f.down = false;
			f.currentZone = 0;
		}
	}

	return false;
}

void CleoEngine::renderMenu(ImGuiRenderer* renderer, const ImVec2& displaySize)
{
	if (!renderer || !g_enabled || !g_symbolsReady) return;
	std::lock_guard<std::mutex> lock(g_touchMutex);
	if (!isAnyMenuVisible()) return;

	g_screenW = displaySize.x;
	g_screenH = displaySize.y;

	const float panelW = std::clamp(displaySize.x * 0.36f, 420.0f, 680.0f);
	const float rowH = std::clamp(displaySize.y * 0.056f, 44.0f, 64.0f);
	const float headerH = rowH * 1.15f;
	const float footerH = rowH * 1.05f;
	const float pad = 16.0f;

	std::string title;
	std::string closeText;
	std::vector<std::pair<int, std::string>> rows;
	int page = 0;
	int totalItems = 0;
	int activeIdx = -1;

	if (g_scriptMenu.active) {
		title = g_scriptMenu.title.empty() ? "CLEO MENU" : g_scriptMenu.title;
		closeText = g_scriptMenu.closeLabel.empty() ? "CLOSE" : g_scriptMenu.closeLabel;
		page = g_scriptMenu.page;
		totalItems = static_cast<int>(g_scriptMenu.items.size());
		activeIdx = g_scriptMenu.activeIndex;
		int start = page * kMenuItemsPerPage;
		int end = std::min(totalItems, start + kMenuItemsPerPage);
		for (int i = start; i < end; ++i) {
			rows.emplace_back(i, g_scriptMenu.items[i]);
		}
	} else {
		auto inv = collectInvokableScriptIndices();
		size_t runningAuto = 0;
		for (const auto& s : g_scripts) {
			if (!s.invokable && s.thread.m_bActive) ++runningAuto;
		}
		char hdr[128];
		snprintf(hdr, sizeof(hdr), "CLEO 菜单  (%zu .csi | %zu .csa 运行中)", inv.size(), runningAuto);
		title = hdr;
		closeText = "关闭";
		page = g_builtinMenu.page;
		totalItems = static_cast<int>(inv.size());
		int start = page * kMenuItemsPerPage;
		int end = std::min(totalItems, start + kMenuItemsPerPage);
		for (int i = start; i < end; ++i) {
			const CleoScript& sc = g_scripts[inv[i]];
			std::string label = sc.displayName;
			if (sc.thread.m_bActive) label += "  [运行中]";
			rows.emplace_back(inv[i], label);
		}
	}

	const int visibleRowCount = std::max<int>(1, static_cast<int>(rows.size()));
	const float panelH = headerH + visibleRowCount * rowH + footerH + pad * 2.0f;
	const float x1 = (displaySize.x - panelW) * 0.5f;
	const float y1 = (displaySize.y - panelH) * 0.5f;
	const float x2 = x1 + panelW;
	const float y2 = y1 + panelH;
	g_menuPanelRect = RectF{x1, y1, x2, y2};

	const ImColor bgCol(18, 18, 22, 232);
	const ImColor borderCol(245, 183, 0, 230);
	const ImColor headerBg(34, 30, 18, 245);
	const ImColor goldText(245, 196, 40, 255);
	const ImColor rowBgA(32, 32, 38, 210);
	const ImColor rowBgB(26, 26, 32, 210);
	const ImColor rowActiveBg(90, 68, 12, 235);
	const ImColor whiteText(242, 242, 245, 255);
	const ImColor dimText(170, 170, 180, 255);
	const ImColor btnBg(50, 50, 60, 235);

	// Panel backdrop & gold border
	renderer->drawRect(ImVec2(x1, y1), ImVec2(x2, y2), bgCol, true);
	renderer->drawRect(ImVec2(x1, y1), ImVec2(x2, y2), borderCol, false, 2.5f);

	// Header
	renderer->drawRect(ImVec2(x1 + 2, y1 + 2), ImVec2(x2 - 2, y1 + headerH), headerBg, true);
	const float titleFont = rowH * 0.44f;
	renderer->drawText(ImVec2(x1 + pad, y1 + (headerH - titleFont) * 0.5f), goldText, title, true, titleFont);

	// Rows
	g_menuRowRects.clear();
	const float itemFont = rowH * 0.42f;
	float curY = y1 + headerH + pad * 0.5f;
	if (rows.empty()) {
		renderer->drawText(ImVec2(x1 + pad, curY + (rowH - itemFont) * 0.5f), dimText,
		                   "未发现 .csi 交互脚本（.csa 脚本自动在后台运行）", true, itemFont * 0.9f);
		curY += rowH;
	} else {
		for (size_t i = 0; i < rows.size(); ++i) {
			RectF r{x1 + pad, curY + 2.0f, x2 - pad, curY + rowH - 2.0f};
			bool isSelected = (rows[i].first == activeIdx);
			ImColor fill = isSelected ? rowActiveBg : ((i & 1) ? rowBgB : rowBgA);
			renderer->drawRect(ImVec2(r.x1, r.y1), ImVec2(r.x2, r.y2), fill, true);
			if (isSelected) {
				renderer->drawRect(ImVec2(r.x1, r.y1), ImVec2(r.x2, r.y2), borderCol, false, 1.5f);
			}
			renderer->drawText(ImVec2(r.x1 + 14.0f, r.y1 + (rowH - 4.0f - itemFont) * 0.5f),
			                   isSelected ? goldText : whiteText, rows[i].second, true, itemFont);
			g_menuRowRects.emplace_back(rows[i].first, r);
			curY += rowH;
		}
	}

	// Footer buttons: [Prev] [Next] [Close]
	const float footY1 = y2 - footerH - pad * 0.4f;
	const float footY2 = y2 - pad * 0.4f;
	const float btnW = (panelW - pad * 4.0f) / 3.0f;
	const float btnFont = rowH * 0.40f;

	g_menuPrevRect = RectF{x1 + pad, footY1, x1 + pad + btnW, footY2};
	g_menuNextRect = RectF{x1 + pad * 2.0f + btnW, footY1, x1 + pad * 2.0f + btnW * 2.0f, footY2};
	g_menuCloseRect = RectF{x1 + pad * 3.0f + btnW * 2.0f, footY1, x2 - pad, footY2};

	auto drawBtn = [&](const RectF& rc, const std::string& label, const ImColor& fill, const ImColor& txt) {
		renderer->drawRect(ImVec2(rc.x1, rc.y1), ImVec2(rc.x2, rc.y2), fill, true);
		renderer->drawRect(ImVec2(rc.x1, rc.y1), ImVec2(rc.x2, rc.y2), ImColor(90, 90, 105, 220), false, 1.2f);
		ImVec2 ts = renderer->calculateTextSize(label, btnFont);
		float tx = rc.x1 + (rc.x2 - rc.x1 - ts.x) * 0.5f;
		float ty = rc.y1 + (rc.y2 - rc.y1 - ts.y) * 0.5f;
		renderer->drawText(ImVec2(tx, ty), txt, label, true, btnFont);
	};

	const int totalPages = std::max(1, (totalItems + kMenuItemsPerPage - 1) / kMenuItemsPerPage);
	drawBtn(g_menuPrevRect, "< 上一页", btnBg, page > 0 ? whiteText : dimText);
	char nextLbl[48];
	snprintf(nextLbl, sizeof(nextLbl), "下一页 (%d/%d) >", page + 1, totalPages);
	drawBtn(g_menuNextRect, nextLbl, btnBg, (page + 1 < totalPages) ? whiteText : dimText);
	drawBtn(g_menuCloseRect, closeText, ImColor(135, 34, 34, 235), whiteText);
}
