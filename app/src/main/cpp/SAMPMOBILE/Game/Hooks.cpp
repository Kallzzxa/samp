#include <atomic>
#include <sys/sysinfo.h>
#include <sys/stat.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <zlib.h>
#include <thread>
#include <ctime>
#include <dirent.h>
#include <strings.h>
#include <stdio.h>
#include <string.h>

#include "Game.h"
#include "../GameSA/include/Enums/WidgetIDs.h"
#include "Core/Rect.h"
#include <cmath>
#include "../UI/UI.h"
#include "../Net/NetGame.h"
#include "../Client.h"
#include "../Settings.h"
#include "ModLoader.h"
#include "CleoEngine.h"
#include "ThreadOptimizer.h"
#include "../Java/Java.h"
#include "Pickups.h"

extern UI *pUI;
extern Game *pGame;
extern NetGame *pNetGame;

static bool resolve_case_insensitive(const char* base, const char* relIn, char* relOut, size_t outSize)
{
	relOut[0] = 0;
	char cur[0x600];
	snprintf(cur, sizeof(cur), "%s", base);

	char tmp[0x1ff];
	snprintf(tmp, sizeof(tmp), "%s", relIn);
	char* save = nullptr;
	for (char* comp = strtok_r(tmp, "/\\", &save); comp; comp = strtok_r(nullptr, "/\\", &save))
	{
		DIR* d = opendir(cur);
		if (!d) return false;
		struct dirent* e;
		char found[0x1ff] = {0};
		while ((e = readdir(d)))
		{
			if (!strcasecmp(e->d_name, comp))
			{
				snprintf(found, sizeof(found), "%s", e->d_name);
				break;
			}
		}
		closedir(d);
		if (!found[0]) return false;
		size_t curLen = strlen(relOut);
		snprintf(relOut + curLen, outSize - curLen, "%s%s", curLen ? "/" : "", found);
		snprintf(cur, sizeof(cur), "%s/%s", base, relOut);
	}
	return relOut[0] != 0;
}

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
		switch (type)
		{
		case 2: // push
			pUI->enqueueTouchEvent(ImVec2(posX, posY), TouchType::push);
			break;

		case 3: // move
			pUI->enqueueTouchEvent(ImVec2(posX, posY), TouchType::move);
			break;

		case 1: // pop
			pUI->enqueueTouchEvent(ImVec2(posX, posY), TouchType::pop);
			break;
		}

		if (pUI->isModalOpenSnapshot())
		{
			releaseAllGtaPointers();
			return;
		}
		else
		{
			if (pNetGame && pNetGame->GetTextDrawPool())
			{
				if (!pNetGame->GetTextDrawPool()->enqueueTouchEvent(type, posX, posY))
				{
					releaseGtaPointer(num);
					return;
				}
			}

			if (pUI->isButtonPanelHitSnapshot(ImVec2(posX, posY)))
			{
				releaseGtaPointer(num);
				return;
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

DECL_HOOK(void, DisplayScreen)
{
	/*
	 * using the loadingscreen from java so we don't need this anymore
	 */

	/*if (RwCameraBeginUpdate(sa::Scene->m_pRwCamera)) {
		// DefinedState2d
		Memory::callFunction<void>("_Z14DefinedState2dv");

		// CSprite2d::InitPerFrame
		Memory::callFunction<void>("_ZN9CSprite2d12InitPerFrameEv");

		RwRenderStateSet(rwRENDERSTATETEXTUREADDRESS, (void*) rwTEXTUREADDRESSCLAMP);

		// emu_GammaSet
		Memory::callFunction("_Z12emu_GammaSeth", false);

		// CLoadingScreen::m_PercentLoaded
		float fPercentLoaded = *g_saSym->GetSymbol<float*>("_ZN14CLoadingScreen15m_PercentLoadedE");

		pUI->splashscreen()->setProgressBarValue(fPercentLoaded / 100.0f);
		pUI->render();

		RwCameraEndUpdate(sa::Scene->m_pRwCamera);
		RwCameraShowRaster(sa::Scene->m_pRwCamera, nullptr, 0);
	}*/
}

DECL_HOOK(int, OS_FileOpen, int a1, uintptr_t handle, char *name, int a2)
{
	char path[0x1ff] = {0};
	char modPath[0x1ff] = {0};
	const char *origName = name;
	bool isProtectedOnlineScript = false;

	if (name && !strncmp(name, "data\\script\\mainV1.scm", 22))
	{
		spdlog::info("Loading mainV1.scm..");
		snprintf(path, sizeof(path), "SAMP\\main.scm");
		name = path;
		isProtectedOnlineScript = !Client::offlineMode();
		goto check_mod;
	}

	if (name && !strncmp(name, "DATA\\SCRIPT\\SCRIPTV1.IMG", 24))
	{
		spdlog::info("Loading scriptV1.img..");
		snprintf(path, sizeof(path), "SAMP\\script.img");
		name = path;
		isProtectedOnlineScript = !Client::offlineMode();
		goto check_mod;
	}

	if (name && !strncmp(name, "DATA\\GTA.DAT", 12))
	{
		spdlog::info("Loading gta.dat..");
		snprintf(path, sizeof(path), "SAMP\\gta.dat");
		name = path;
		goto check_mod;
	}

	if (name && !strncmp(name, "DATA\\PEDS.IDE", 13))
	{
		spdlog::info("Loading peds.ide..");
		snprintf(path, sizeof(path), "SAMP\\peds.ide");
		name = path;
		goto check_mod;
	}

	if (name && !strncmp(name, "DATA\\TIMECYC.DAT", 16))
	{
		spdlog::info("Loading timecyc.dat..");
		snprintf(path, sizeof(path), "SAMP\\timecyc.dat");
		name = path;
		goto check_mod;
	}

	if (name && !strncmp(name, "data\\paths\\tracks2.dat", 22))
	{
		spdlog::info("Loading tracks2.dat...");
		snprintf(path, sizeof(path), "SAMP\\tracks2.dat");
		name = path;
		goto check_mod;
	}

	if (name && !strncmp(name, "data\\paths\\tracks4.dat", 22))
	{
		spdlog::info("Loading tracks4.dat...");
		snprintf(path, sizeof(path), "SAMP\\tracks4.dat");
		name = path;
		goto check_mod;
	}

check_mod:
	if (!isProtectedOnlineScript && ModLoader::isEnabled() && name)
	{
		// Check both the redirected name (e.g. SAMP\timecyc.dat) and original name (e.g. DATA\TIMECYC.DAT)
		if (ModLoader::resolve(name, modPath, sizeof(modPath)) ||
			(origName && origName != name && ModLoader::resolve(origName, modPath, sizeof(modPath))))
		{
			spdlog::info("ModLoader override: {} -> {}", origName ? origName : name, modPath);
			name = modPath;
		}
	}

	int result = OS_FileOpen(a1, handle, name, a2);
	if (result != 0 && name)
	{
		char relIn[0x1ff] = {0};
		snprintf(relIn, sizeof(relIn), "%s", name);
		size_t inLen = strlen(relIn);
		while (inLen > 0 && (relIn[inLen - 1] == ' ' || relIn[inLen - 1] == '\t'))
			relIn[--inLen] = 0;
		char fixed[0x1ff] = {0};
		if (resolve_case_insensitive(Client::gameDir(), relIn, fixed, sizeof(fixed)))
		{
			spdlog::info("OS_FileOpen retry (case-insensitive): {}", fixed);
			result = OS_FileOpen(a1, handle, fixed, a2);
		}
	}
	if (result != 0)
		spdlog::error("OS_FileOpen failed ({}): {}", result, name ? name : "<null>");
	return result;
}

DECL_HOOK(sa::CPedModelInfo *, CModelInfo_AddPedModel, int id)
{
	if (id < 0 || id >= 25000) return nullptr;
	auto model = new sa::CPedModelInfo{};
	model->vtable = g_saSym->GetVmtFunction<uintptr_t>("_ZTV13CPedModelInfo", 1, true); // CPedModelInfo::~CPedModelInfo

	typedef sa::CStore<sa::CPedModelInfo, 278> PedModelStore;
	auto ms_pedModelStore = g_saSym->Abs<PedModelStore *>(addr::ms_pedModelStore);
	ms_pedModelStore->AddItem();

	// CClumpModelInfo::Init
	model->Init();

	auto ms_modelInfoPtrs = g_saSym->Abs<sa::CBaseModelInfo **>(addr::CModelInfo_ms_modelInfoPtrs);
	ms_modelInfoPtrs[id] = model;
	return model;
}

DECL_HOOK(sa::CAtomicModelInfo *, CModelInfo_AddAtomicModel, int id)
{
	if (id < 0 || id >= 25000) return nullptr;
	auto model = new sa::CAtomicModelInfo{};
	model->vtable = g_saSym->GetVmtFunction<uintptr_t>("_ZTV16CAtomicModelInfo", 1, true); // CAtomicModelInfo::~CAtomicModelInfo

	typedef sa::CStore<sa::CAtomicModelInfo, 14000> AtomicModelStore;
	auto ms_atomicModelStore = g_saSym->Abs<AtomicModelStore *>(addr::ms_atomicModelStore);
	ms_atomicModelStore->AddItem();

	// CAtomicModelInfo::Init
	model->Init();

	// CModelInfo::ms_modelInfoPtrs
	auto ms_modelInfoPtrs = g_saSym->Abs<sa::CBaseModelInfo **>(addr::CModelInfo_ms_modelInfoPtrs);
	ms_modelInfoPtrs[id] = model;
	return model;
}

static const char *texdb_format_ext(sa::TextureDatabaseFormat fmt)
{
	switch (fmt)
	{
	case sa::DF_UNC: return "unc";
	case sa::DF_DXT: return "dxt";
	case sa::DF_360: return "360";
	case sa::DF_PS3: return "ps3";
	case sa::DF_PVR: return "pvr";
	case sa::DF_ETC: return "etc";
	default: return nullptr;
	}
}

static bool texdb_format_exists_in_modloader(const char *dbName, sa::TextureDatabaseFormat fmt)
{
	const char *ext = texdb_format_ext(fmt);
	if (!ext || !dbName || !*dbName || !ModLoader::isEnabled()) return false;
	char rel[0xff] = {0};
	char fixed[0x1ff] = {0};
	snprintf(rel, sizeof(rel), "texdb/%s/%s.%s.toc", dbName, dbName, ext);
	return ModLoader::resolve(rel, fixed, sizeof(fixed));
}

static bool texdb_format_exists_in_base(const char *dbName, sa::TextureDatabaseFormat fmt)
{
	const char *ext = texdb_format_ext(fmt);
	if (!ext || !dbName || !*dbName) return false;
	char rel[0xff] = {0};
	char fixed[0x1ff] = {0};
	snprintf(rel, sizeof(rel), "texdb/%s/%s.%s.toc", dbName, dbName, ext);
	return resolve_case_insensitive(Client::gameDir(), rel, fixed, sizeof(fixed));
}

DECL_HOOK(sa::TextureDatabaseRuntime *, TextureDatabaseRuntime_Load, const char *withName, bool fullyLoad, sa::TextureDatabaseFormat forcedFormat)
{
	sa::TextureDatabaseFormat selected = forcedFormat;
	if (withName && *withName)
	{
		static const sa::TextureDatabaseFormat kPriority[] = {
			sa::DF_ETC, sa::DF_PVR, sa::DF_DXT, sa::DF_UNC
		};

		// Prefer any format supplied by an active ModLoader pack (e.g. .unc/.dxt HUD packs)
		// before falling back to official base files/, so modded texdb packs are never shadowed.
		bool matchedMod = false;
		if (ModLoader::isEnabled())
		{
			if (selected != sa::DF_Default && texdb_format_exists_in_modloader(withName, selected))
			{
				matchedMod = true;
			}
			else
			{
				for (auto candidate : kPriority)
				{
					if (texdb_format_exists_in_modloader(withName, candidate))
					{
						selected = candidate;
						matchedMod = true;
						break;
					}
				}
			}
		}

		if (!matchedMod && (selected == sa::DF_Default || !texdb_format_exists_in_base(withName, selected)))
		{
			for (auto candidate : kPriority)
			{
				if (texdb_format_exists_in_base(withName, candidate))
				{
					selected = candidate;
					break;
				}
			}
		}
		spdlog::info("TextureDatabaseRuntime::Load({}, fully={}, req={}, sel={}, mod={})",
			withName, fullyLoad, (int)forcedFormat, (int)selected, matchedMod);
	}
	return TextureDatabaseRuntime_Load(withName, fullyLoad, selected);
}

static RwTexture *findTextureInDatabase(const char *dbName, const char *texture_name)
{
	auto database_handle = sa::TextureDatabaseRuntime::GetDatabase(dbName);
	if (!database_handle)
	{
		return nullptr;
	}

	auto registered = sa::TextureDatabaseRuntime::registered();
	bool alreadyRegistered = false;
	if (registered.dataPtr && registered.numEntries > 0)
	{
		for (int index = 0; index < registered.numEntries; ++index)
		{
			if (registered.dataPtr[index] == database_handle)
			{
				alreadyRegistered = true;
				break;
			}
		}
	}

	if (alreadyRegistered)
	{
		return nullptr;
	}

	sa::TextureDatabaseRuntime::Register(database_handle);
	RwTexture *texture = sa::TextureDatabaseRuntime::GetTexture(texture_name);
	sa::TextureDatabaseRuntime::Unregister(database_handle);
	return texture;
}

DECL_HOOK(RwTexture *, CTxdStore_TxdStoreFindCB, const char *texture_name)
{
	if (!texture_name || !*texture_name)
	{
		return nullptr;
	}

	if (ModLoader::isEnabled())
	{
		for (const auto &extraDb : ModLoader::extraTexDbs())
		{
			if (RwTexture *tex = findTextureInDatabase(extraDb.c_str(), texture_name))
			{
				return tex;
			}
		}
	}

	static const char *kBaseTexDbs[] = {"gta_int", "gta3", "samp", "menu", "mobile", "samp_extra", "cutscene"};
	for (const char *db : kBaseTexDbs)
	{
		if (RwTexture *tex = findTextureInDatabase(db, texture_name))
		{
			return tex;
		}
	}

	RwTexDictionary *current = Memory::callFunction<RwTexDictionary *>("_Z25RwTexDictionaryGetCurrentv");
	while (current)
	{
		RwTexture *texture = Memory::callFunction<RwTexture *>("_Z31RwTexDictionaryFindNamedTextureP15RwTexDictionaryPKc", current, texture_name);
		if (texture)
		{
			return texture;
		}

		current = sa::CTxdStore::GetTxdParent(current);
	}

	return nullptr;
}

// Guard against AML plugins or game code calling TextureDatabaseRuntime::Register(nullptr)
// when an optional texdb fails to load; a null slot in registered[] crashes GetTexture() at +0x110.
DECL_HOOK(void, TextureDatabaseRuntime_Register, sa::TextureDatabaseRuntime *db)
{
	if (!db)
	{
		return;
	}
	TextureDatabaseRuntime_Register(db);
}

DECL_HOOK(int, CGame_InitialiseRenderWare)
{
	spdlog::info("Initializing samp texture database...");

	int result = CGame_InitialiseRenderWare();
	sa::TextureDatabaseRuntime::Load("samp", false, sa::DF_ETC);
	sa::TextureDatabaseRuntime::Load("samp_extra", false, sa::DF_ETC);
	if (ModLoader::isEnabled())
	{
		for (const auto &db : ModLoader::extraTexDbs())
		{
			spdlog::info("ModLoader: loading custom texture database '{}'", db);
			sa::TextureDatabaseRuntime::Load(db.c_str(), false, sa::DF_Default);
		}
	}
	Client::initializeUI();
	return result;
}

struct CStreamingFile
{
	unsigned char m_name[40];
	bool m_bRegister;
	uint8_t m_padding[3];
	int32_t m_lsn;
};
VALIDATE_SIZE(CStreamingFile, 0x30);

CStreamingFile *CStreaming_ms_files = nullptr;
DECL_HOOK(void, CStreaming_InitImageList)
{
	spdlog::info("Initializing image list...");

	CStreaming_ms_files = g_saSym->GetSymbol<CStreamingFile *>("_ZN10CStreaming8ms_filesE");

	if (CStreaming_ms_files)
	{
		for (int i = 0; i < 8; i++)
		{
			CStreaming_ms_files[i].m_name[0] = '\0';
			CStreaming_ms_files[i].m_lsn = 0;
		}

		// CStreaming::LoadCdDirectory iterates ms_files[0..7] in order and SKIPS
		// any modelId already registered (FIRST-WINS). Mounting ModLoader's virtual
		// models.img at slot 0 lets loose .dff/.ifp/.col/.ipl files override base
		// gta3.img non-destructively (uses 1 of 2 spare slots: 5 + 2 lazy = 7 <= 8).
		if (ModLoader::isEnabled() && ModLoader::hasVirtualModelsImg())
		{
			spdlog::info("ModLoader: registering virtual IMG '{}'", ModLoader::virtualModelsImgRelPath());
			Memory::callFunction<void>("_ZN10CStreaming14AddImageToListEPKcb", ModLoader::virtualModelsImgRelPath(), true);
		}

		Memory::callFunction<void>("_ZN10CStreaming14AddImageToListEPKcb", "TEXDB\\SAMPCOL.IMG", true);
		Memory::callFunction<void>("_ZN10CStreaming14AddImageToListEPKcb", "TEXDB\\GTA3.IMG", true);
		Memory::callFunction<void>("_ZN10CStreaming14AddImageToListEPKcb", "TEXDB\\GTA_INT.IMG", true);
		Memory::callFunction<void>("_ZN10CStreaming14AddImageToListEPKcb", "TEXDB\\SAMP.IMG", true);
		// PLAYER.IMG is registered lazily by the clothes system and CUTSCENE.IMG by
		// gta.dat's IMG line; registering them here too overflows the 8-slot ms_files
		// table and hangs CdStream during level load.
	}
	else
	{
		spdlog::error("Failed to get symbol for CStreaming_ms_files!");
	}
}

// WarDrum's CModelInfoAccelerator caches a sequential model-ID array in models/MINFO.BIN.
// Whenever ModLoader mounts virtual models.img or swaps gta3.img, the IMG entry sequence shifts
// by +N and stale MINFO.BIN causes GetModelInfoFast() to miss every subsequent .dff!
// Resolving directly via CModelInfo::GetModelInfo() avoids desync AND the 20550-entry cap (~3 ms).
DECL_HOOK(void, CModelInfoAccelerator_GetEntry, void *self, void **outInfo, int32_t *outId, char *name)
{
	using FnGetModelInfo = void *(*)(const char *, int32_t *);
	static FnGetModelInfo s_fnGetModelInfo = g_saSym->GetSymbol<FnGetModelInfo>("_ZN10CModelInfo12GetModelInfoEPKcPi");
	if (s_fnGetModelInfo && outInfo)
	{
		*outInfo = s_fnGetModelInfo(name, outId);
		return;
	}
	CModelInfoAccelerator_GetEntry(self, outInfo, outId, name);
}

DECL_HOOK(void, CStreaming_Init2)
{
	CStreaming_Init2();

	// CStreaming::ms_memoryAvailable (GetSymbol returns 0 on failure)
	if (auto* memAvailable = g_saSym->GetSymbol<uint32_t *>("_ZN10CStreaming18ms_memoryAvailableE"))
	{
		*memAvailable *= 3;
	}
}

DECL_HOOK(void, CTimer_StartUserPause)
{
	if (pUI)
	{
		pUI->setVisible(false);
	}

	if (g_java && pNetGame)
	{
		g_java->setPauseState(true);
	}

	if (auto* userPause = g_saSym->GetSymbol<bool *>("_ZN6CTimer11m_UserPauseE"))
	{
		*userPause = true;
	}
}

DECL_HOOK(void, CTimer_EndUserPause)
{
	if (pUI)
	{
		pUI->setVisible(true);
	}

	if (g_java && pNetGame)
	{
		g_java->setPauseState(false);
	}

	if (auto* userPause = g_saSym->GetSymbol<bool *>("_ZN6CTimer11m_UserPauseE"))
	{
		*userPause = false;
	}
}

unsigned int *msCollected;
unsigned int curMS;

static std::atomic<int> s_pendingScreenshotMode{-1};

void Hooks::requestScreenshot(int modeOverride)
{
	int mode = (modeOverride == 0 || modeOverride == 1) ? modeOverride : Settings::screenshotMode();
	s_pendingScreenshotMode.store(mode);
}

static void WritePngChunk(FILE* fp, const char type[4], const uint8_t* data, uint32_t len)
{
	uint8_t lenBE[4] = {
		(uint8_t)((len >> 24) & 0xFF),
		(uint8_t)((len >> 16) & 0xFF),
		(uint8_t)((len >> 8) & 0xFF),
		(uint8_t)(len & 0xFF)
	};
	fwrite(lenBE, 1, 4, fp);
	fwrite(type, 1, 4, fp);
	if (len > 0 && data) {
		fwrite(data, 1, len, fp);
	}
	uLong crc = crc32(0L, Z_NULL, 0);
	crc = crc32(crc, reinterpret_cast<const Bytef*>(type), 4);
	if (len > 0 && data) {
		crc = crc32(crc, reinterpret_cast<const Bytef*>(data), len);
	}
	uint8_t crcBE[4] = {
		(uint8_t)((crc >> 24) & 0xFF),
		(uint8_t)((crc >> 16) & 0xFF),
		(uint8_t)((crc >> 8) & 0xFF),
		(uint8_t)(crc & 0xFF)
	};
	fwrite(crcBE, 1, 4, fp);
}

static void CaptureCurrentFramebufferToPng(int mode)
{
	if (!Client::gameDir()) return;

	int w = sa::RsGlobal ? sa::RsGlobal->screenWidth : 0;
	int h = sa::RsGlobal ? sa::RsGlobal->screenHeight : 0;
	if (w <= 0 || h <= 0) {
		GLint vp[4] = {0, 0, 0, 0};
		glGetIntegerv(GL_VIEWPORT, vp);
		w = vp[2];
		h = vp[3];
	}
	if (w <= 0 || h <= 0) return;

	std::vector<uint8_t> pixels((size_t)w * (size_t)h * 4);

	// GTA:SA Android v2.10 runs OpenGL ES on a dedicated GraphicsThread via
	// WarDrum's RenderQueue. Calling raw glReadPixels() on GameMain has no
	// bound EGL context and silently leaves pixels zeroed (pure black PNG).
	// emu_glReadPixels() flushes pending draw batches, enqueues
	// RQ_Command_rqReadPixels on GraphicsThread, and blocks on
	// RenderQueue::Flush() until pixels are populated.
	using EmuGlReadPixelsFn = void(*)(int, int, int, int, uint32_t, uint32_t, void*);
	static EmuGlReadPixelsFn s_emuReadPixels = nullptr;
	if (!s_emuReadPixels && g_saSym) {
		s_emuReadPixels = g_saSym->GetSymbol<EmuGlReadPixelsFn>("_Z16emu_glReadPixelsiiiijjPv");
	}
	if (s_emuReadPixels) {
		s_emuReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
	} else {
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
	}

	char dir1[512], dir2[512];
	snprintf(dir1, sizeof(dir1), "%sSAMP", Client::gameDir());
	snprintf(dir2, sizeof(dir2), "%sSAMP/screens", Client::gameDir());
	mkdir(dir1, 0775);
	mkdir(dir2, 0775);

	time_t now = time(nullptr);
	struct tm tmNow{};
	localtime_r(&now, &tmNow);
	static int s_shotSeq = 0;
	char filename[128];
	snprintf(filename, sizeof(filename), "samp_%04d%02d%02d_%02d%02d%02d_%02d_%s.png",
		tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday,
		tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec,
		(++s_shotSeq) % 100,
		mode == 1 ? "game" : "hud");

	std::string fullPath = std::string(dir2) + "/" + filename;
	std::string fileLabel(filename);

	if (pUI && pUI->chat()) {
		pUI->chat()->addInfoMessage("-> Screenshot Taken (%s): %s",
			mode == 1 ? "Game Only" : "HUD+Chat", filename);
	}

	std::thread([pixels = std::move(pixels), w, h, fullPath, fileLabel]() {
		const size_t rowBytes = (size_t)w * 4;
		std::vector<uint8_t> raw((rowBytes + 1) * (size_t)h);
		for (int y = 0; y < h; ++y) {
			uint8_t* dst = &raw[(size_t)y * (rowBytes + 1)];
			const uint8_t* src = &pixels[(size_t)(h - 1 - y) * rowBytes];
			dst[0] = 0; // PNG filter type 0 (None)
			memcpy(dst + 1, src, rowBytes);
			for (int x = 0; x < w; ++x) {
				dst[1 + x * 4 + 3] = 0xFF; // force opaque alpha
			}
		}

		uLongf compBound = compressBound((uLong)raw.size());
		std::vector<uint8_t> compressed(compBound);
		if (compress2(compressed.data(), &compBound, raw.data(), (uLong)raw.size(), Z_BEST_SPEED) != Z_OK) {
			spdlog::error("Screenshot compress2 failed for {}", fullPath);
			return;
		}

		FILE* fp = fopen(fullPath.c_str(), "wb");
		if (!fp) {
			spdlog::error("Screenshot fopen failed for {}", fullPath);
			return;
		}
		static const uint8_t kPngSig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
		fwrite(kPngSig, 1, 8, fp);

		uint8_t ihdr[13] = {
			(uint8_t)((w >> 24) & 0xFF), (uint8_t)((w >> 16) & 0xFF), (uint8_t)((w >> 8) & 0xFF), (uint8_t)(w & 0xFF),
			(uint8_t)((h >> 24) & 0xFF), (uint8_t)((h >> 16) & 0xFF), (uint8_t)((h >> 8) & 0xFF), (uint8_t)(h & 0xFF),
			8, // bit depth
			6, // color type RGBA
			0, 0, 0
		};
		WritePngChunk(fp, "IHDR", ihdr, 13);
		WritePngChunk(fp, "IDAT", compressed.data(), (uint32_t)compBound);
		WritePngChunk(fp, "IEND", nullptr, 0);
		fclose(fp);
		spdlog::info("Screenshot saved: {} ({}x{}, {} bytes)", fileLabel, w, h, (uint32_t)compBound);
	}).detach();
}

static std::atomic<bool> s_bPendingAdjustableHud{false};
static bool s_bWatchingAdjustableHudExit = false;

void Hooks::openAdjustableHud()
{
	s_bPendingAdjustableHud.store(true);
}

// GTA:SA v2.10 runs the frontend/menu on a separate frame path
// (DoGameState -> RsEventHandler -> AppEventHandler -> FrontendIdle) that
// never reaches Idle()/Render2dStuff(). Client::process() therefore has to
// be driven from here as well or StartGame() can never fire and the client
// sits in the menu forever behind the Java loading overlay.
DECL_HOOK(void, FrontendIdle)
{
	static bool fired = false;
	if (!fired) {
		fired = true;
		spdlog::info("hook fired: FrontendIdle tid={}", (int) gettid());
	}
	FrontendIdle();

	if (s_bWatchingAdjustableHudExit) {
		auto* pAdjInst = g_saSym->GetSymbol<uintptr_t*>("_ZN14CAdjustableHUD11m_pInstanceE");
		if (pAdjInst && *pAdjInst == 0) {
			s_bWatchingAdjustableHudExit = false;
			Memory::callFunction("_Z20Menu_SwitchOffToGamev");
			EnsureTouchWidgetsCreated();
		}
	}

	Client::process();
}

DECL_HOOK(void, Render2dStuff)
{
	static bool fired = false;
	if (!fired) {
		fired = true;
		spdlog::info("hook fired: Render2dStuff tid={}", (int) gettid());
	}

	// Clean 3D Game-Only Screenshot: capture right before 2D GTA HUD, touch
	// widgets, and SA-MP ImGui Chat/UI are drawn over the 3D scene.
	if (s_pendingScreenshotMode.load() == 1) {
		s_pendingScreenshotMode.store(-1);
		CaptureCurrentFramebufferToPng(1);
	}

	Render2dStuff();

	Client::process();

	// touch-widget watchdog: DeleteAll (CGame::Shutdown / gamemode restart /
	// death) or AML plugins like libSAUtils64.so redirecting m_pWidgets can
	// leave stock or custom combat widgets missing — recreate whenever needed.
	{
		static int wdFrames = 0;
		if (++wdFrames >= 60) {
			wdFrames = 0;
			uintptr_t* widgets = GetTouchWidgets();
			if ((pNetGame || Client::offlineMode()) && pGame && GamePool_FindPlayerPed() && pGame->FindPlayerPed() && widgets) {
				// engine rewrites targeting widgets back onto the attack slot
				// on every RepositionAdjustableWidgets — keep our layout fix
				FixTouchWidgetLayout();
				if (!widgets[sa::WidgetIDs::WIDGET_ATTACK]) {
					spdlog::info("touch widgets wiped, re-creating");
					Memory::callFunction("_Z20Menu_SwitchOffToGamev");
					EnsureTouchWidgetsCreated();
				}
				else if (!widgets[sa::WidgetIDs::WIDGET_BUTTON_CROUCH]
					|| !widgets[sa::WidgetIDs::WIDGET_CAR_CHEATS]
					|| !widgets[sa::WidgetIDs::WIDGET_MISSION_SKIP]) {
					spdlog::info("custom touch widgets missing in live m_pWidgets, re-creating");
					EnsureTouchWidgetsCreated();
				}
			}
		}
	}

	// aim-button latch + block button + on-foot camera toggle: driven every
	// frame in UpdateTouchCombatVisuals() (render visibility) and
	// TickTouchCombatInput() (logic-frame tap/hold sampling).
	if ((pNetGame || Client::offlineMode()) && pGame && GamePool_FindPlayerPed() && pGame->FindPlayerPed()) {
		extern void UpdateTouchCombatVisuals();
		UpdateTouchCombatVisuals();
	}

	// temp widget diagnostic (touch-controls investigation)
	{
		static int diagFrames = -1;
		if (diagFrames >= 0 && diagFrames < 600) {
			++diagFrames;
		}
		else if (diagFrames == -1 && pNetGame && pGame && GamePool_FindPlayerPed() && pGame->FindPlayerPed()) {
			diagFrames = 0;
		}
		if (diagFrames == 120 || diagFrames == 600) {
			if (diagFrames == 600) diagFrames = -2;
			uintptr_t* widgets = GetTouchWidgets();
			int nonNull = 0;
			for (int i = 0; i < 0xBE; ++i) {
				if (widgets && widgets[i]) ++nonNull;
			}
			spdlog::info("WIDGET-DIAG nonnull={}/190 widgets=0x{:x}", nonNull, reinterpret_cast<uintptr_t>(widgets));
			// DrawAll gates (arm64 GOT slots only; CWidget offsets below are arm64-specific)
			if (!VER_x32) {
				for (int i = 0; i < 0xBE && widgets; ++i) {
					uintptr_t w = widgets[i];
					if (!w) continue;
					uint32_t flags = *reinterpret_cast<uint32_t*>(w + 140);
					uint32_t mapping = *reinterpret_cast<uint32_t*>(w + 8);
					float px0 = *reinterpret_cast<float*>(w + 16);
					float py0 = *reinterpret_cast<float*>(w + 20);
					float px1 = *reinterpret_cast<float*>(w + 24);
					float py1 = *reinterpret_cast<float*>(w + 28);
					spdlog::info("WIDGET-DIAG [{}] flags=0x{:08x} map=0x{:08x} pos=({:.0f},{:.0f})-({:.0f},{:.0f})", i, flags, mapping, px0, py0, px1, py1);
				}
				uint8_t* pPhoto = *g_saSym->Abs<uint8_t**>(0x84ea70);
				int* pCurInst = *g_saSym->Abs<int**>(0x851340);
				uintptr_t mobileMenu = *g_saSym->Abs<uintptr_t*>(0x8519b0);
				uint32_t mm36 = mobileMenu ? *reinterpret_cast<uint32_t*>(mobileMenu + 36) : 0;
				uintptr_t mm48 = mobileMenu ? *reinterpret_cast<uintptr_t*>(mobileMenu + 48) : 0;
				spdlog::info("WIDGET-DIAG takePhoto={} curInst={} mobileMenu36={} mobileMenu48=0x{:x}",
					pPhoto ? *pPhoto : -1, pCurInst ? *pCurInst : -1, mm36, mm48);
				// aim-mode gates: targeting button (0x13) is only polled when
				// GetEnterTargeting reaches IsTouched/IsReleased(19) — needs
				// IsLockOnMode or (free-aim && InputType==2)
				uintptr_t ms = *g_saSym->Abs<uintptr_t*>(0x851498);
				spdlog::info("WIDGET-DIAG MobileSettings=0x{:x} aimModeField={} lockon={} freeaim={} inptype={}",
					ms, ms ? *reinterpret_cast<int*>(ms + 256) : -9,
					Memory::callFunction<int>("_ZN14MobileSettings12IsLockOnModeEv"),
					Memory::callFunction<int>("_ZN14MobileSettings13IsFreeAimModeEv"),
					Memory::callFunction<int>("_ZN4CHID12GetInputTypeEv"));
				for (int id : {0x1, 0x12, 0x13, 0x14, 0x20, 0xa5, 0xaa, 0x16, 0x1f, 0x15, 0x98, 0x99}) {
					uintptr_t w = widgets ? widgets[id] : 0;
					spdlog::info("WIDGET-DIAG id=0x{:x} ptr=0x{:x} en={} fl=0x{:03x}", id, w,
						w ? (int)*reinterpret_cast<uint8_t*>(w + 89) : -9,
						w ? *reinterpret_cast<uint32_t*>(w + 140) : 0u);
					if (w) {
						uintptr_t spr = *reinterpret_cast<uintptr_t*>(w + 16);
						spdlog::info("WIDGET-DIAG id=0x{:x} alpha={} cnt={:.1f} spr0=0x{:x} rect=({:.0f},{:.0f},{:.0f},{:.0f})",
							id, (int)*reinterpret_cast<uint8_t*>(w + 88),
							*reinterpret_cast<float*>(w + 136), spr,
							*reinterpret_cast<float*>(w + 44), *reinterpret_cast<float*>(w + 48),
							*reinterpret_cast<float*>(w + 52), *reinterpret_cast<float*>(w + 56));
					}
				}
			}
		}
	}

	if (Settings::displayfps() && msCollected)
	{
		// fps
		msCollected[curMS++ % 0x28] = Memory::callFunction<unsigned int>("_Z9OS_TimeMSv");

		RGBA bgColor = {0, 0, 0, 0};
		RGBA color = {255, 255, 255, 255};

		CFont::SetBackgroundColor(&bgColor);
		CFont::SetScale(1.12f * 0.54, 1.12f);
		CFont::SetOrientation(1);
		CFont::SetJustify(0);
		CFont::SetWrapX(640.0);
		CFont::SetProportional(0);
		CFont::SetFontStyle(1);
		CFont::SetEdge(0);
		CFont::SetColor(&color);

		char szStr[64];
		const unsigned int spanMs = msCollected[(curMS - 1) % 0x28] - msCollected[curMS % 0x28];
		const float fps = (spanMs > 0) ? (39000.0f / static_cast<float>(spanMs)) : 0.0f;
		snprintf(szStr, sizeof(szStr), "FPS: %.2f", fps);
		CFont::PrintString((float)((float)sa::RsGlobal->screenWidth / 640.0) * 4.0, (float)((float)sa::RsGlobal->screenHeight / 448.0) * 200.0, szStr);

		// mem (throttled to ~1 Hz to avoid per-frame mallinfo2 / /proc/meminfo overhead)
		static uint32_t s_lastMemSampleMs = 0;
		static long long s_cachedAllocMB = 0;
		static long long s_cachedTotalMB = 0;
		const uint32_t nowMs = GetTickCount();
		if (s_lastMemSampleMs == 0 || (nowMs - s_lastMemSampleMs) >= 1000)
		{
			s_lastMemSampleMs = nowMs;
			struct mallinfo2 memInfo = mallinfo2();
			s_cachedAllocMB = static_cast<long long>(memInfo.uordblks / (1024 * 1024));
#if VER_x32
			FILE *meminfo = fopen("/proc/meminfo", "r");
			if (meminfo)
			{
				char line[256];
				while (fgets(line, sizeof(line), meminfo))
				{
					if (strncmp(line, "MemTotal:", 9) == 0)
					{
						s_cachedTotalMB = atoll(&line[9]) / 1024;
						break;
					}
				}
				fclose(meminfo);
			}
#else
			struct sysinfo info{};
			if (sysinfo(&info) == 0)
			{
				const unsigned long long unit = info.mem_unit ? info.mem_unit : 1ULL;
				s_cachedTotalMB = static_cast<long long>((static_cast<unsigned long long>(info.totalram) * unit) / (1024ULL * 1024ULL));
			}
#endif
		}

		CFont::SetBackgroundColor(&bgColor);
		CFont::SetScale(1.12f * 0.54, 1.12f);
		CFont::SetOrientation(1);
		CFont::SetJustify(0);
		CFont::SetWrapX(640.0);
		CFont::SetProportional(0);
		CFont::SetFontStyle(1);
		CFont::SetEdge(0);
		CFont::SetColor(&color);

		char szStrMem[64];
		snprintf(szStrMem, sizeof(szStrMem), "MEM: %lld / %lld MB", s_cachedAllocMB, s_cachedTotalMB);
		CFont::PrintString((float)((float)sa::RsGlobal->screenWidth / 640.0) * 4.0, (float)((float)sa::RsGlobal->screenHeight / 448.0) * 210.0, szStrMem);

		// arch
		CFont::SetBackgroundColor(&bgColor);
		CFont::SetScale(1.12f * 0.54, 1.12f);
		CFont::SetOrientation(1);
		CFont::SetJustify(0);
		CFont::SetWrapX(640.0);
		CFont::SetProportional(0);
		CFont::SetFontStyle(1);
		CFont::SetEdge(0);
		CFont::SetColor(&color);

		char szStrArch[64];
		snprintf(szStrArch, sizeof(szStrArch), "ARCH: %s", VER_x32 ? "x32" : "x64");
		CFont::PrintString((float)((float)sa::RsGlobal->screenWidth / 640.0) * 4.0, (float)((float)sa::RsGlobal->screenHeight / 448.0) * 220.0, szStrArch);
	}
}

static void SanitizeAllAlphaLinkLists();

DECL_HOOK(uint32_t, Idle, void *param, bool firstFrame)
{
	static bool fired = false;
	if (!fired) {
		fired = true;
		spdlog::info("hook fired: Idle tid={}", (int) gettid());
	}
	SanitizeAllAlphaLinkLists();
	uint32_t result = Idle(param, firstFrame);

	if (pUI)
	{
		bool bCaptureWithHud = (s_pendingScreenshotMode.load() == 0);
		bool prevBtnPanel = false;
		if (bCaptureWithHud && pUI->buttonpanel()) {
			prevBtnPanel = pUI->buttonpanel()->visible();
			pUI->buttonpanel()->setVisible(false);
		}

		pUI->render();

		if (bCaptureWithHud) {
			s_pendingScreenshotMode.store(-1);
			CaptureCurrentFramebufferToPng(0);
			if (pUI->buttonpanel()) {
				pUI->buttonpanel()->setVisible(prevBtnPanel);
			}
		}
	}

	RwCameraEndUpdate(sa::Scene->m_pRwCamera);

	return result;
}

DECL_HOOK(void, CRadar_ClearBlip, int32_t nIndex)
{
	uintptr_t dwRetAddr = 0;
	GET_LR(dwRetAddr);

	if ((uint16_t)nIndex > 249)
	{
		spdlog::warn("Invalid blip ID ({}). Called from 0x{:X}", nIndex, dwRetAddr);
		return;
	}

	return CRadar_ClearBlip(nIndex);
}

DECL_HOOK(void, CPools_Initialise)
{
	spdlog::info("GTA pools initializing..");
	Patches::initBetterPoolsSize();
}

ThreadOptimizer g_threadOpt;
extern volatile pid_t g_gameThreadTid;
DECL_HOOK(void, AND_RunThread, void *a1)
{
	g_gameThreadTid = gettid();
	g_threadOpt.pushThread(gettid());
	AND_RunThread(a1);
}

extern sa::CPlayerInfo *WORLD_PLAYERS;

DECL_HOOK(sa::CPlayerInfo *, GetPlayerInfoForThisPlayerPed, sa::CPed *ped)
{
	if (!WORLD_PLAYERS || !ped) return nullptr;

	for (int i = 0; i < PLAYER_PED_SLOTS; i++)
	{
		if (WORLD_PLAYERS[i].m_pPed == ped)
		{
			return &WORLD_PLAYERS[i];
		}
	}

	return nullptr;
}

DECL_HOOK(uintptr_t *, RQCommand_rqSetAlphaTest, uintptr_t *result)
{
	if (result) {
		*result += 8;
	}
	return result;
}

DECL_HOOK(int, GetInputType)
{
	return 0;
}

// temp: trace which touch widgets report held — punch-while-walking bug
static int s_touchLogBudget = 300;

extern sa::WidgetIDs GetWidgetTypeFromWidget(uintptr_t pWidget);

// dump the full finger-claim state + CHID path state for one IsTouched hit:
// distinguishes (a) CHID::Implements/IsPressed physical-device path from
// (b) m_apInitialTouch finger-ownership path. arm64 addresses only.
static void DumpTouchState(int widgetID, uintptr_t w)
{
	if (VER_x32) return;
	int mapping = w ? *reinterpret_cast<int*>(w + 8) : -9;
	int finger = w ? *reinterpret_cast<int*>(w + 132) : -9;
	int b84 = w ? (int)*reinterpret_cast<uint8_t*>(w + 84) : -9;
	int hold = w ? *reinterpret_cast<int*>(w + 136) : -9;
	uint32_t fl = w ? *reinterpret_cast<uint32_t*>(w + 140) : 0;
	int impl = -9, pressed = -9;
	if (mapping >= 0) {
		impl = Memory::callFunction<int>("_ZN4CHID10ImplementsE10HIDMapping", mapping);
		pressed = Memory::callFunction<int>("_ZN4CHID9IsPressedE10HIDMappingPf", mapping, (float*)nullptr);
	}
	int curInst = -9; uintptr_t devVtbl = 0;
	if (int** pp = g_saSym->Abs<int**>(0x851340); pp && *pp) {
		curInst = **pp;
		if (uintptr_t* da = *g_saSym->Abs<uintptr_t**>(0x84b1e8); da && curInst >= 0 && curInst < 8) {
			uintptr_t d = da[curInst];
			devVtbl = d ? *reinterpret_cast<uintptr_t*>(d) : 0;
		}
	}
	spdlog::info("TDIAG w=0x{:x} id=0x{:x} map={} f={} b84={} hold={} fl=0x{:03x} | cur={} vtbl=0x{:x} impl={} press={}",
		w, widgetID, mapping, finger, b84, hold, fl, curInst, devVtbl, impl, pressed);
	uintptr_t* init = *g_saSym->Abs<uintptr_t**>(0x84ab30);
	uintptr_t* rel = *g_saSym->Abs<uintptr_t**>(0x84f5b8);
	uint8_t* act = *g_saSym->Abs<uint8_t**>(0x8506d0);
	float* pos = *g_saSym->Abs<float**>(0x84c868);
	float* down = *g_saSym->Abs<float**>(0x84f8c8);
	for (int i = 0; i < 4; ++i) {
		uintptr_t ow = init ? init[i] : 0;
		uintptr_t rw = rel ? rel[i] : 0;
		int isDown = Memory::callFunction<int>("_ZN15CTouchInterface11IsTouchDownEi", i);
		spdlog::info("  f{} down={} act={} owner=0x{:x}({}) rel=0x{:x}({}) pos=({:.0f},{:.0f}) downPos=({:.0f},{:.0f})",
			i, isDown, act ? (int)act[i] : -9,
			ow, (int)GetWidgetTypeFromWidget(ow), rw, (int)GetWidgetTypeFromWidget(rw),
			pos ? pos[i * 2] : -9.f, pos ? pos[i * 2 + 1] : -9.f,
			down ? down[i * 2] : -9.f, down ? down[i * 2 + 1] : -9.f);
	}
}

DECL_HOOK(int, TI_IsTouched, int widgetID, void* pVec, int i)
{
	int r = TI_IsTouched(widgetID, pVec, i);
	if (r && s_touchLogBudget > 0) {
		--s_touchLogBudget;
		uintptr_t* widgets = GetTouchWidgets();
		uintptr_t w = (widgets && widgetID >= 0 && widgetID < 0xBE) ? widgets[widgetID] : 0;
		spdlog::info("IsTouched id=0x{:x} r={} vec=({}, {})", widgetID, r,
			pVec ? *(float*)pVec : -1.f, pVec ? *((float*)pVec + 1) : -1.f);
		DumpTouchState(widgetID, w);
	}
	return r;
}
DECL_HOOK(int, TI_IsHeldDown, int widgetID, int i)
{
	int r = TI_IsHeldDown(widgetID, i);
	if (r && s_touchLogBudget > 0) {
		--s_touchLogBudget;
		spdlog::info("IsHeldDown id=0x{:x} r={}", widgetID, r);
	}
	return r;
}

// engine rewrites targeting widgets back onto the shared attack posIdx on
// every reposition — reapply our split layout immediately afterwards or the
// aim icon lands on the fire button and eats its taps
DECL_HOOK(void, TI_RepositionAdjustableWidgets)
{
	extern void FixTouchWidgetLayout();
	TI_RepositionAdjustableWidgets();
	FixTouchWidgetLayout();
}

// log every finger->widget claim; lr identifies the call site inside the
// dispatcher (0x36f610 clear / 0x36f6e4 claim / 0x36f794 steal, +4 = ret addr)
DECL_HOOK(void, W_SetInitialTouch, int finger, uintptr_t widget)
{
	if (s_touchLogBudget > 0) {
		--s_touchLogBudget;
		uintptr_t lr = 0;
		GET_LR(lr);
		spdlog::info("SetInitialTouch f={} w=0x{:x}(id={}) from=0x{:x}", finger, widget,
			(int)GetWidgetTypeFromWidget(widget), lr);
	}
	W_SetInitialTouch(finger, widget);
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

DECL_HOOK(void, CWidget, uintptr_t pWidget, const char *pszSprite, uintptr_t WidgetPos, unsigned int nFlags, uint32_t Mapping)
{
	spdlog::info("Widget: {}", pszSprite);
	CWidget(pWidget, pszSprite, WidgetPos, nFlags, Mapping);
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
	CWidget_Update(pWidget);
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

DECL_HOOK(int, CWorld_FindPlayerSlotWithPedPointer, sa::CPed *pPlayersPed)
{
	if (!WORLD_PLAYERS || !pPlayersPed) return -1;

	for (int i = 0; i < PLAYER_PED_SLOTS; ++i)
	{
		if (WORLD_PLAYERS[i].m_pPed == pPlayersPed)
		{
			return i;
		}
	}
	return -1;
}

int g_lastObjectModel = -1;

DECL_HOOK(int32_t, CObject_Render, sa::CObject *_this)
{
	if (_this)
	{
		g_lastObjectModel = _this->m_nModelIndex;
	}

	// object material (covers both CObjectPool objects and CPlayerPed attached objects in O(1))
	CObject *pObject = nullptr;
	if (_this && !IsEntityPlaceable(_this))
	{
		pObject = CObject::FindByGtaEntity(_this);
		if (pObject)
		{
			pObject->TryChangeToCustomObjectMaterial();
		}
	}

	int32_t result = CObject_Render(_this);
	g_lastObjectModel = -1;

	if (pObject)
	{
		pObject->ChangeToOriginalObjectMaterial();
	}

	return result;
}

DECL_HOOK(void, CRenderer_RenderEverythingBarRoads)
{
	CRenderer_RenderEverythingBarRoads();

	if (pNetGame)
	{
		CObjectPool *pObjectPool = pNetGame->GetObjectPool();
		if (pObjectPool)
		{
			for (OBJECTID i = 0; i < MAX_OBJECTS; i++)
			{
				CObject *pObject = pObjectPool->GetAt(i);
				if (pObject && pObject->m_bForceRender)
				{
					pObject->Render();
				}
			}
		}
	}
}

DECL_HOOK(void, CWorld_ProcessPedsAfterPreRender)
{
	CWorld_ProcessPedsAfterPreRender();

	if (pNetGame)
	{
		CPlayerPool *pPlayerPool = pNetGame->GetPlayerPool();
		if (pPlayerPool)
		{
			pPlayerPool->ProcessAttachedObjects();
		}
	}
}

extern void BeginUVAnimFrame();
extern RpMaterial* DriveUVAnimMaterialCB(RpMaterial* material, void* data);
extern RwMatrix* FindAtomicUVAnimMatrix(RpAtomic* atomic, bool* outHasDualUV = nullptr);

static void SaveAndForceUVWrap(void*& savedAddr)
{
	savedAddr = (void*) (uintptr_t) rwTEXTUREADDRESSWRAP;
	Memory::callFunction<RwBool>("_Z16RwRenderStateGet13RwRenderStatePv", rwRENDERSTATETEXTUREADDRESS, &savedAddr);
	if (savedAddr != (void*) (uintptr_t) rwTEXTUREADDRESSWRAP) {
		RwRenderStateSet(rwRENDERSTATETEXTUREADDRESS, (void*) (uintptr_t) rwTEXTUREADDRESSWRAP);
	}
}

static void RestoreUVWrap(void* savedAddr)
{
	if (savedAddr != (void*) (uintptr_t) rwTEXTUREADDRESSWRAP) {
		RwRenderStateSet(rwRENDERSTATETEXTUREADDRESS, savedAddr);
	}
}

DECL_HOOK(RpMaterial*, MaterialUpdateUVAnimCB, RpMaterial* material, void* data)
{
	return DriveUVAnimMaterialCB(material, data);
}

DECL_HOOK(void, rpMatFXOpenGLAllInOneRenderCB, RwResEntry *resEntry, uintptr_t object, uint8_t type, uint32_t flags)
{
	if (!resEntry || !object)
	{
		return;
	}

	bool bHasDualUV = false;
	RwMatrix* uvMat = (type == 1 /*rpATOMIC*/)
			? FindAtomicUVAnimMatrix(reinterpret_cast<RpAtomic*>(object), &bHasDualUV)
			: nullptr;
	void* savedAddr = nullptr;
	if (uvMat) {
		// _rwOpenGLSetRenderStateNoExtras applies the global TEXTUREADDRESS state
		// rather than RwTexture::filterAddressing; force WRAP so scrolling UVs tile.
		SaveAndForceUVWrap(savedAddr);
		// Stock _rpMatFXOpenGLAllInOneRenderCB only switches on effects 1..5 and
		// drops effect 6 (rpMATFXEFFECTDUALUVTRANSFORM) to default render.
		if (bHasDualUV) {
			Memory::callFunction("_Z27emu_SetTextureMatrixEnabledPfh", (float*) uvMat, (uint8_t) 1);
		}
	}
	rpMatFXOpenGLAllInOneRenderCB(resEntry, object, type, flags);
	if (uvMat) {
		if (bHasDualUV) {
			Memory::callFunction("_Z27emu_SetTextureMatrixEnabledPfh", (float*) nullptr, (uint8_t) 0);
		}
		RestoreUVWrap(savedAddr);
	}
}

DECL_HOOK(int, CustomPipeRenderCB, RwResEntry *resEntry, uintptr_t object, uint8_t type, uint32_t flags)
{
	if (!resEntry || !object)
	{
		spdlog::warn("CustomPipeRenderCB: Prevent crash - resEntry or object is null");
		return 0;
	}

	RwMatrix* uvMat = (type == 1 /*rpATOMIC*/) ? FindAtomicUVAnimMatrix(reinterpret_cast<RpAtomic*>(object)) : nullptr;
	void* savedAddr = nullptr;
	if (uvMat) {
		SaveAndForceUVWrap(savedAddr);
		Memory::callFunction("_Z27emu_SetTextureMatrixEnabledPfh", (float*) uvMat, (uint8_t) 1);
	}
	int ret = CustomPipeRenderCB(resEntry, object, type, flags);
	if (uvMat) {
		Memory::callFunction("_Z27emu_SetTextureMatrixEnabledPfh", (float*) nullptr, (uint8_t) 0);
		RestoreUVWrap(savedAddr);
	}
	return ret;
}

DECL_HOOK(int, CustomBuildingPipeRenderCB, RwResEntry *resEntry, uintptr_t object, uint8_t type, uint32_t flags)
{
	if (!resEntry || !object)
	{
		return 0;
	}

	RwMatrix* uvMat = (type == 1 /*rpATOMIC*/) ? FindAtomicUVAnimMatrix(reinterpret_cast<RpAtomic*>(object)) : nullptr;
	void* savedAddr = nullptr;
	if (uvMat) {
		SaveAndForceUVWrap(savedAddr);
		Memory::callFunction("_Z27emu_SetTextureMatrixEnabledPfh", (float*) uvMat, (uint8_t) 1);
	}
	int ret = CustomBuildingPipeRenderCB(resEntry, object, type, flags);
	if (uvMat) {
		Memory::callFunction("_Z27emu_SetTextureMatrixEnabledPfh", (float*) nullptr, (uint8_t) 0);
		RestoreUVWrap(savedAddr);
	}
	return ret;
}

DECL_HOOK(int, rxOpenGLDefaultAllInOneRenderCB, RwResEntry *resEntry, uintptr_t object, uint8_t type, uint32_t flags)
{
	if (!resEntry || !object)
	{
		spdlog::warn("rxOpenGLDefaultAllInOneRenderCB: Prevent crash - resEntry or object is null");
		return 0;
	}

	RwMatrix* uvMat = (type == 1 /*rpATOMIC*/) ? FindAtomicUVAnimMatrix(reinterpret_cast<RpAtomic*>(object)) : nullptr;
	void* savedAddr = nullptr;
	if (uvMat) {
		SaveAndForceUVWrap(savedAddr);
		Memory::callFunction("_Z27emu_SetTextureMatrixEnabledPfh", (float*) uvMat, (uint8_t) 1);
	}
	int ret = rxOpenGLDefaultAllInOneRenderCB(resEntry, object, type, flags);
	if (uvMat) {
		Memory::callFunction("_Z27emu_SetTextureMatrixEnabledPfh", (float*) nullptr, (uint8_t) 0);
		RestoreUVWrap(savedAddr);
	}
	return ret;
}

DECL_HOOK(RwBool, _RwTextureDestroy, uintptr_t texture)
{
	if (!texture)
	{
		spdlog::warn("RwTextureDestroy: Prevent crash");
		return 0;
	}
	return _RwTextureDestroy(texture);
}

DECL_HOOK(void *, RwFrameAddChild, void *parent, void *child)
{
	if (!parent || !child)
	{
		spdlog::warn("RwFrameAddChild: Prevent crash");
		return nullptr;
	}
	return RwFrameAddChild(parent, child);
}

DECL_HOOK(uint8_t *, RLEDecompress, uint8_t *dest, unsigned int destSize, const uint8_t *src, unsigned int segSize, unsigned int escape)
{
	if (!dest || !src)
	{
		spdlog::warn("RLEDecompress: Prevent crash");
		return nullptr;
	}
	return RLEDecompress(dest, destSize, src, segSize, escape);
}

DECL_HOOK(int, RwResourcesFreeResEntry, RwResEntry *a1)
{
	if (!a1)
	{
		spdlog::warn("RwResourcesFreeResEntry: Prevent crash");
		return 0;
	}
	return RwResourcesFreeResEntry(a1);
}

static void SanitizeAlphaLinkList(void *listPtr, const char *name)
{
	if (!listPtr) return;
	// CLinkList<AlphaObjectInfo>: 4 sentinel CLinks (usedHead, usedTail, freeHead, freeTail).
	// On arm64: sizeof(CLink)=40 (item 24B, prev +24, next +32).
	// On arm32: sizeof(CLink)=20 (item 12B, prev +12, next +16).
	constexpr size_t kLinkSize = VER_x32 ? 20 : 40;
	constexpr size_t kPrevOff  = VER_x32 ? 12 : 24;
	constexpr size_t kNextOff  = VER_x32 ? 16 : 32;

	uint8_t *head = reinterpret_cast<uint8_t *>(listPtr);
	uint8_t *tail = head + kLinkSize;

	auto getNext = [](uint8_t *node) -> uint8_t * {
		return *reinterpret_cast<uint8_t **>(node + kNextOff);
	};
	auto getPrev = [](uint8_t *node) -> uint8_t * {
		return *reinterpret_cast<uint8_t **>(node + kPrevOff);
	};
	auto setNext = [](uint8_t *node, uint8_t *v) {
		*reinterpret_cast<uint8_t **>(node + kNextOff) = v;
	};
	auto setPrev = [](uint8_t *node, uint8_t *v) {
		*reinterpret_cast<uint8_t **>(node + kPrevOff) = v;
	};

	uint8_t *cur = getNext(head);
	bool corrupted = (cur == nullptr);
	int steps = 0;
	while (!corrupted && cur != tail)
	{
		if (++steps > 4096)
		{
			corrupted = true;
			break;
		}
		uint8_t *p = getPrev(cur);
		uint8_t *n = getNext(cur);
		if (!p || !n)
		{
			corrupted = true;
			break;
		}
		cur = n;
	}

	if (corrupted)
	{
		static int s_logBudget = 20;
		if (s_logBudget > 0)
		{
			--s_logBudget;
			spdlog::warn("CVisibilityPlugins::InitAlphaEntityList: repaired corrupted {}", name);
		}
		setNext(head, tail);
		setPrev(tail, head);
	}
}

static void SanitizeAllAlphaLinkLists()
{
	static void *s_alphaList = g_saSym->GetSymbol<void *>("_ZN18CVisibilityPlugins11m_alphaListE");
	static void *s_boatList = g_saSym->GetSymbol<void *>("_ZN18CVisibilityPlugins21m_alphaBoatAtomicListE");
	static void *s_entityList = g_saSym->GetSymbol<void *>("_ZN18CVisibilityPlugins17m_alphaEntityListE");
	static void *s_underwaterList = g_saSym->GetSymbol<void *>("_ZN18CVisibilityPlugins27m_alphaUnderwaterEntityListE");
	static void *s_lastList = g_saSym->GetSymbol<void *>("_ZN18CVisibilityPlugins25m_alphaReallyDrawLastListE");

	SanitizeAlphaLinkList(s_alphaList, "m_alphaList");
	SanitizeAlphaLinkList(s_boatList, "m_alphaBoatAtomicList");
	SanitizeAlphaLinkList(s_entityList, "m_alphaEntityList");
	SanitizeAlphaLinkList(s_underwaterList, "m_alphaUnderwaterEntityList");
	SanitizeAlphaLinkList(s_lastList, "m_alphaReallyDrawLastList");
}

DECL_HOOK(int, rxOpenGLDefaultAtomicAllInOneNode, void *self, const void *params)
{
	if (params)
	{
		const auto *atomic = *reinterpret_cast<const uint8_t *const *>(params);
		if (atomic)
		{
			// Disassembly-verified RenderWare offsets in libGTASA.so 2.10:
			//   arm32: _rxOpenGLDefaultAtomicAllInOneNode @ 0x220eb0, InstanceVertexArray @ 0x221060
			//   arm64: _rxOpenGLDefaultAtomicAllInOneNode @ 0x2cb5e0, InstanceVertexArray @ 0x2cb820
			// rpworld.h's RpGeometry/RpAtomic struct layouts drift by 8 bytes on arm64, so use raw offsets.
			constexpr size_t kAtomicRepEntryOff  = VER_x32 ? 20  : 40;
			constexpr size_t kAtomicGeomOff      = VER_x32 ? 24  : 48;
			constexpr size_t kAtomicStartMtOff   = VER_x32 ? 80  : 124;
			constexpr size_t kAtomicEndMtOff     = VER_x32 ? 82  : 126;
			constexpr size_t kGeomFlagsOff       = VER_x32 ? 8   : 16;
			constexpr size_t kGeomNumVertsOff    = VER_x32 ? 20  : 28;
			constexpr size_t kGeomNumMtOff       = VER_x32 ? 24  : 32;
			constexpr size_t kGeomMeshOff        = VER_x32 ? 88  : 144;
			constexpr size_t kGeomRepEntryOff    = VER_x32 ? 92  : 152;
			constexpr size_t kGeomMorphTargetOff = VER_x32 ? 96  : 160;
			constexpr size_t kMeshNumMeshesOff   = 4;
			constexpr size_t kMeshSerialOff      = 6;
			constexpr size_t kResEntrySerialOff  = VER_x32 ? 24  : 48;
			constexpr size_t kMorphTargetSize    = VER_x32 ? 28  : 40;
			constexpr size_t kMorphVertsOff      = VER_x32 ? 20  : 24;
			constexpr uint32_t kRpGeometryNative = 0x01000000u;

			const auto *geom = *reinterpret_cast<const uint8_t *const *>(atomic + kAtomicGeomOff);
			if (geom)
			{
				const uint32_t geomFlags = *reinterpret_cast<const uint32_t *>(geom + kGeomFlagsOff);
				const int32_t numVertices = *reinterpret_cast<const int32_t *>(geom + kGeomNumVertsOff);
				const auto *mesh = *reinterpret_cast<const uint8_t *const *>(geom + kGeomMeshOff);
				// Mobile GTA:SA DFFs use rpGEOMETRYNATIVE (verts stored in native GL stream, morphTarget.verts is intentionally null)
				if ((geomFlags & kRpGeometryNative) == 0 &&
					numVertices >= 1 && mesh && *reinterpret_cast<const uint16_t *>(mesh + kMeshNumMeshesOff) != 0)
				{
					const int32_t numMorphTargets = *reinterpret_cast<const int32_t *>(geom + kGeomNumMtOff);
					const auto *repEntry = (numMorphTargets == 1)
						? *reinterpret_cast<const uint8_t *const *>(geom + kGeomRepEntryOff)
						: *reinterpret_cast<const uint8_t *const *>(atomic + kAtomicRepEntryOff);

					bool needsInstance = (repEntry == nullptr);
					if (!needsInstance)
					{
						const uint16_t entrySerial = *reinterpret_cast<const uint16_t *>(repEntry + kResEntrySerialOff);
						const uint16_t meshSerial = *reinterpret_cast<const uint16_t *>(mesh + kMeshSerialOff);
						if (entrySerial != meshSerial)
						{
							needsInstance = true;
						}
					}

					if (needsInstance)
					{
						const int16_t startMt = *reinterpret_cast<const int16_t *>(atomic + kAtomicStartMtOff);
						const int16_t endMt = *reinterpret_cast<const int16_t *>(atomic + kAtomicEndMtOff);
						const auto *morphTargets = *reinterpret_cast<const uint8_t *const *>(geom + kGeomMorphTargetOff);

						auto hasValidVerts = [&](int16_t mt) -> bool {
							if (!morphTargets || mt < 0 || mt >= numMorphTargets) return false;
							const void *verts = *reinterpret_cast<const void *const *>(
								morphTargets + static_cast<size_t>(mt) * kMorphTargetSize + kMorphVertsOff);
							return verts != nullptr;
						};

						if (!hasValidVerts(startMt) || (endMt != startMt && !hasValidVerts(endMt)))
						{
							static int s_skipLogBudget = 20;
							if (s_skipLogBudget > 0)
							{
								--s_skipLogBudget;
								spdlog::warn("rxOpenGLDefaultAtomicAllInOneNode: skip atomic with null verts & no repEntry (model={}, numMt={}, startMt={}, endMt={})",
									g_lastObjectModel, numMorphTargets, startMt, endMt);
							}
							return 1;
						}
					}
				}
			}
		}
	}
	return rxOpenGLDefaultAtomicAllInOneNode(self, params);
}

DECL_HOOK(int, TextureDatabaseRuntime_GetEntry, uintptr_t _this, const char *a2, bool *a3)
{
	if (!_this)
	{
		spdlog::warn("TextureDatabaseRuntime_GetEntry: Prevent crash");
		return -1;
	}
	return TextureDatabaseRuntime_GetEntry(_this, a2, a3);
}

DECL_HOOK(void, CGame_Process)
{
	static bool fired = false;
	if (!fired) {
		fired = true;
		spdlog::info("hook fired: CGame::Process tid={}", (int) gettid());
	}
	BeginUVAnimFrame();
	CGame_Process();

	Settings::applyDynamicCameraAndFreeCam();

	if (auto* pDrawHud = g_saSym->GetSymbol<bool*>("_ZN4CHud19m_Wants_To_Draw_HudE")) {
		*pDrawHud = Settings::showGtaHud();
	}
	if (auto* pHideRadar = g_saSym->GetSymbol<bool*>("_ZN4CHud23bScriptDontDisplayRadarE")) {
		*pHideRadar = (!Settings::showRadar() || !Settings::showGtaHud());
	}

	if (pNetGame && pGame && pGame->FindPlayerPed()) {
		if (Settings::openHudEditOnStart()) {
			static int s_hudEditStartDelay = 0;
			if (++s_hudEditStartDelay >= 45) {
				s_hudEditStartDelay = 0;
				Settings::setOpenHudEditOnStart(false);
				s_bPendingAdjustableHud.store(true);
			}
		}
		if (s_bPendingAdjustableHud.exchange(false)) {
			uintptr_t pMobileMenu = g_saSym->GetSymbol<uintptr_t>("gMobileMenu");
			if (pMobileMenu) {
				Memory::callFunction("_ZN10MobileMenu12InitForPauseEv", pMobileMenu);
				Memory::callFunction("_ZN10MobileMenu14ProcessPendingEv", pMobileMenu);
				Memory::callFunction("_ZN14CAdjustableHUD6ToggleEv");
				s_bWatchingAdjustableHudExit = true;
			}
		}
	}

	if (pNetGame)
	{
		CTextDrawPool *pTextDrawPool = pNetGame->GetTextDrawPool();
		if (pTextDrawPool)
		{
			pTextDrawPool->SnapshotProcess();
		}

		CObjectPool *pObjectPool = pNetGame->GetObjectPool();
		if (pObjectPool)
		{
			pObjectPool->Process();
			pObjectPool->ProcessMaterialText();
		}

		// JNI edit callbacks queue ops from the Java UI thread — apply them
		// here on the game thread where GTA/ScriptCommand access is safe
		EditObject::processPending();

		if (pGame && pGame->GetCamera())
		{
			pGame->GetCamera()->SetToOwner();
		}
	}
}

DECL_HOOK(int, CRadar_SetCoordBlip, uint32_t BlpType, sa::CVector vecPosition, uint32_t nColour, uint32_t DispFlag, char *pScriptName)
{
	// CWorld::FindGroundZForCoord
	vecPosition.z = Memory::callFunction<float>("_ZN6CWorld19FindGroundZForCoordEff", vecPosition.x, vecPosition.y);

	if (Client::offlineMode() && pGame)
	{
		CPlayerPed *pLocal = pGame->FindPlayerPed();
		if (pLocal && pLocal->m_ped)
		{
			RwMatrix mat = pLocal->m_ped->GetMatrix().ToRwMatrix();
			mat.pos = sa::CVector(vecPosition.x, vecPosition.y, vecPosition.z + 5.0f);
			if (pLocal->IsInVehicle() && pLocal->m_ped->pVehicle)
			{
				pLocal->m_ped->pVehicle->SetMatrix((sa::CMatrix &)mat);
			}
			pLocal->m_ped->SetMatrix((sa::CMatrix &)mat);

			if (pUI && pUI->chat())
			{
				pUI->chat()->addInfoMessage("-> Teleported to: %.2f %.2f %.2f", vecPosition.x, vecPosition.y, vecPosition.z);
			}
		}
	}

	if (pNetGame && pNetGame->GetRakClient() && pScriptName && !strncmp(pScriptName, "CODEWAY", 7))
	{
		RakNet::BitStream bsSend;
		bsSend.Write(vecPosition.x);
		bsSend.Write(vecPosition.y);
		bsSend.Write(vecPosition.z);
		pNetGame->GetRakClient()->RPC(&RPC_MapMarker, &bsSend, HIGH_PRIORITY, RELIABLE, 0, false, UNASSIGNED_NETWORK_ID, nullptr);
	}

	return CRadar_SetCoordBlip(BlpType, vecPosition, nColour, DispFlag, pScriptName);
}

DECL_HOOK(void, CRadar_DrawRadarGangOverlay, bool bFullMap)
{
	if (pNetGame)
	{
		CGangZonePool *pGangZonePool = pNetGame->GetGangZonePool();
		if (pGangZonePool)
		{
			pGangZonePool->Draw(bFullMap);
		}
	}
}

#if VER_x32
DECL_HOOK(uint32_t, CHudColours_GetIntColour, uint32_t index)
{
	return TranslateColorCodeToRGBA(index);
}
#else
DECL_HOOK(uint32_t, CHudColours_GetIntColour, uintptr_t _this, uint32_t index)
{
	return TranslateColorCodeToRGBA(index);
}
#endif

DECL_HOOK(uint32_t, CAutoMobile_ProcessEntityCollision, sa::CVehicle *_this, sa::CVehicle *pCollidedVehicle, uintptr_t a3)
{
	if (!pNetGame || !pNetGame->m_netSet || !pNetGame->m_netSet->disableVehicleCollisions ||
		!_this || !pCollidedVehicle ||
		pCollidedVehicle->m_nModelIndex < 400 || pCollidedVehicle->m_nModelIndex > 611 ||
		!_this->pDriver || !pCollidedVehicle->pDriver)
	{
		return CAutoMobile_ProcessEntityCollision(_this, pCollidedVehicle, a3);
	}

	return 0;
}

DECL_HOOK(uint32_t, CBike_ProcessEntityCollision, sa::CVehicle *_this, sa::CVehicle *pCollidedVehicle, uintptr_t a3)
{
	if (!pNetGame || !pNetGame->m_netSet || !pNetGame->m_netSet->disableVehicleCollisions ||
		!_this || !pCollidedVehicle ||
		pCollidedVehicle->m_nModelIndex < 400 || pCollidedVehicle->m_nModelIndex > 611 ||
		!_this->pDriver || !pCollidedVehicle->pDriver)
	{
		return CBike_ProcessEntityCollision(_this, pCollidedVehicle, a3);
	}

	return 0;
}

DECL_HOOK(uint32_t, CMonsterTruck_ProcessEntityCollision, sa::CVehicle *_this, sa::CVehicle *pCollidedVehicle, uintptr_t a3)
{
	if (!pNetGame || !pNetGame->m_netSet || !pNetGame->m_netSet->disableVehicleCollisions ||
		!_this || !pCollidedVehicle ||
		pCollidedVehicle->m_nModelIndex < 400 || pCollidedVehicle->m_nModelIndex > 611 ||
		!_this->pDriver || !pCollidedVehicle->pDriver)
	{
		return CMonsterTruck_ProcessEntityCollision(_this, pCollidedVehicle, a3);
	}

	return 0;
}

DECL_HOOK(uint32_t, CTrailer_ProcessEntityCollision, sa::CVehicle *_this, sa::CVehicle *pCollidedVehicle, uintptr_t a3)
{
	if (!pNetGame || !pNetGame->m_netSet || !pNetGame->m_netSet->disableVehicleCollisions ||
		!_this || !pCollidedVehicle ||
		pCollidedVehicle->m_nModelIndex < 400 || pCollidedVehicle->m_nModelIndex > 611 ||
		!_this->pDriver || !pCollidedVehicle->pDriver)
	{
		if (pCollidedVehicle
				&& pCollidedVehicle->m_nModelIndex >= 400
				&& pCollidedVehicle->m_nModelIndex <= 611
				&& !pCollidedVehicle->pDriver
				&& pCollidedVehicle->m_pTowingVehicle)
		{
			// trailer collided with an already-attached trailer: skip
			return 0;
		}

		return CTrailer_ProcessEntityCollision(_this, pCollidedVehicle, a3);
	}

	return 0;
}

// sector coords are floor(pos/50 + 60); CPhysical::Add rejects any bound
// rect whose coords land outside [-120,119] (unsigned c+120 <= 240), so
// out-of-bounds entities never get sector records at all. Repeat-sector
// groups only hash coord & 0xF — remap out-of-range rect edges mod-16 so
// Add accepts the entity and lands its records in the same groups a
// scanner at its true position hashes to.
// Destination band is the dead zone (w0-112 → coords -112..-97,
// x in [-8600,-7850]): remapping into positive cells 0..15 placed the rect
// over real SF-map territory, and since rect-vs-rect tests use the
// REMAPPED rect, OOB entities ghost-collided with real entities sharing
// their hash group — the residual "sticky" feel. No real entity can ever
// reach x < -3000, so dead-zone rects can never overlap real ones.
// Same mechanism libMultiSA ships: it inline-hooks GetBoundRect at +0x53.
DECL_HOOK(sa::CRect, CPhysical_GetBoundRect, sa::CPhysical *_this)
{
	sa::CRect r = CPhysical_GetBoundRect(_this);
	auto fixAxis = [](float &lo, float &hi) {
		int c0 = (int) floorf(lo / 50.0f + 60.0f);
		int c1 = (int) floorf(hi / 50.0f + 60.0f);
		if (c0 >= -120 && c0 <= 120 && c1 >= -120 && c1 <= 120) return;
		if (c1 < c0) { int t = c0; c0 = c1; c1 = t; }
		int span = c1 - c0;
		if (span > 15) span = 15;
		int w0 = c0 & 0xF;
		lo = (w0 - 172) * 50.0f + 25.0f;
		hi = (w0 + span - 172) * 50.0f + 25.0f;
	};
	fixAxis(r.left, r.right);
	fixAxis(r.bottom, r.top);
	return r;
}

// set while a script/opcode-initiated attach or detach is running, so that
// the BreakTowLink hook can tell intentional breaks from GTA-spontaneous ones
bool g_bIntentionalTrailerDetach = false;

DECL_HOOK(void, CTrailer_BreakTowLink, sa::CVehicle *_this)
{
	if (!g_bIntentionalTrailerDetach && pNetGame && _this
			&& _this->m_pTowingVehicle && _this->fHealth > 0.0f)
	{
		CVehiclePool* pVehiclePool = pNetGame->GetVehiclePool();
		if (pVehiclePool)
		{
			VEHICLEID tractorId = pVehiclePool->FindIDFromGtaPtr(_this->m_pTowingVehicle);
			CVehicle* pTractor = (tractorId != INVALID_VEHICLE_ID) ? pVehiclePool->GetAt(tractorId) : nullptr;
			if (pTractor && pTractor->VerifyInstance() && pTractor->m_pTrailer
					&& !pTractor->IsDriverLocalPlayer())
			{
				// server still owns this attachment: veto GTA-initiated break
				return;
			}
		}
	}

	CTrailer_BreakTowLink(_this);
}

// fix crosshair
float *m_f3rdPersonCHairMultX, *m_f3rdPersonCHairMultY;

DECL_HOOK(void, DrawCrosshair)
{
	if (Settings::crosshairStyle() != 0)
	{
		// Custom crosshair (1..3) drawn via ImGui in UI::drawList(), or Hidden (4)
		return;
	}
	if (!m_f3rdPersonCHairMultX || !m_f3rdPersonCHairMultY)
	{
		DrawCrosshair();
		return;
	}
	float save1 = *m_f3rdPersonCHairMultX;
	*m_f3rdPersonCHairMultX = 0.52f;

	float save2 = *m_f3rdPersonCHairMultY;
	*m_f3rdPersonCHairMultY = 0.435f;

	DrawCrosshair();

	*m_f3rdPersonCHairMultX = save1;
	*m_f3rdPersonCHairMultY = save2;
}

DECL_HOOK(float, CDraw_CalculateAspectRatio)
{
	float ar = CDraw_CalculateAspectRatio();
	int mode = Settings::aspectRatio();
	if (mode > 0)
	{
		switch (mode)
		{
		case 1: ar = 16.0f / 9.0f; break;
		case 2: ar = 20.0f / 9.0f; break;
		case 3: ar = 21.0f / 9.0f; break;
		case 4: ar = 16.0f / 10.0f; break;
		case 5: ar = 4.0f / 3.0f; break;
		default: break;
		}
		if (auto* pAr = g_saSym->GetSymbol<float*>("_ZN5CDraw15ms_fAspectRatioE"))
		{
			*pAr = ar;
		}
	}
	return ar;
}

void Hooks::installWidgetFixHooks()
{
	spdlog::info("Installing widget fix hooks...");

	GetTouchWidgets();

	// HOOK("_ZN7CWidgetC2EPKcRK14WidgetPositionj10HIDMapping", CWidget);
	HOOK("_ZN7CWidget6UpdateEv", CWidget_Update);
	HOOK("_ZN7CWidget10SetEnabledEb", CWidget_SetEnabled);
}

void Hooks::installCrosshairFixHooks()
{
	spdlog::info("Installing crosshair fix hooks...");

	m_f3rdPersonCHairMultX = g_saSym->GetSymbol<float *>("_ZN7CCamera22m_f3rdPersonCHairMultXE");
	m_f3rdPersonCHairMultY = g_saSym->GetSymbol<float *>("_ZN7CCamera22m_f3rdPersonCHairMultYE");

	HOOK("_ZN4CHud14DrawCrossHairsEv", DrawCrosshair);
}

DECL_HOOK(uint16_t*, CText_Get, void* _this, const char* key)
{
	if (uint16_t* custom = CleoEngine::getCustomGxt(key))
	{
		return custom;
	}
	if (!_this || !key)
	{
		return nullptr;
	}
	return CText_Get(_this, key);
}

DECL_HOOK(int, CPlantMgr_AddLocTris, void* entry, const sa::CVector* camPos, int a3, int startTri, int endTri)
{
	if (!entry) return 0;
	auto* entity = *reinterpret_cast<sa::CEntity**>(entry);
	if (!entity || IsEntityPlaceable(entity)) return 0;

	auto* colModel = Memory::callFunction<sa::CColModel*>("_ZN7CEntity11GetColModelEv", entity);
	if (!colModel || !colModel->m_pColData) return 0;

	auto* colData = colModel->m_pColData;
	if (!colData->m_pVertices || !colData->m_pTriangles || colData->m_nNumTriangles == 0) {
		return 0;
	}
	if (startTri < 0 || endTri < startTri || endTri >= static_cast<int>(colData->m_nNumTriangles)) {
		return 0;
	}
	for (int i = startTri; i <= endTri; ++i) {
		const sa::CColTriangle& tri = colData->m_pTriangles[i];
		if (tri.vA < 0 || tri.vB < 0 || tri.vC < 0 ||
			tri.vA > 65535 || tri.vB > 65535 || tri.vC > 65535) {
			return 0;
		}
	}
	return CPlantMgr_AddLocTris(entry, camPos, a3, startTri, endTri);
}

void Hooks::install()
{
	spdlog::info("Installing hooks...");

	msCollected = g_saSym->GetSymbol<unsigned int *>("msCollected");
	if (auto* pCurMS = g_saSym->GetSymbol<unsigned int *>("curMS"))
	{
		curMS = *pCurMS;
	}

	HOOK("_Z11OS_FileOpen14OSFileDataAreaPPvPKc16OSFileAccessType", OS_FileOpen);
	HOOK("_ZN10CStreaming13InitImageListEv", CStreaming_InitImageList);
	HOOK("_ZN21CModelInfoAccelerator8GetEntryEPP14CBaseModelInfoPiPc", CModelInfoAccelerator_GetEntry);
	HOOK("_ZN22TextureDatabaseRuntime8RegisterEPS_", TextureDatabaseRuntime_Register);
	HOOK("_ZN10CStreaming5Init2Ev", CStreaming_Init2);
	HOOK("_ZN6CTimer14StartUserPauseEv", CTimer_StartUserPause);
	HOOK("_ZN6CTimer12EndUserPauseEv", CTimer_EndUserPause);
	HOOK("_ZN5CGame20InitialiseRenderWareEv", CGame_InitialiseRenderWare);
	HOOK("_ZN10CModelInfo11AddPedModelEi", CModelInfo_AddPedModel);
	HOOK("_ZN10CModelInfo14AddAtomicModelEi", CModelInfo_AddAtomicModel);
	HOOK("_ZN6CPools10InitialiseEv", CPools_Initialise);
	HOOK("_ZN22TextureDatabaseRuntime4LoadEPKcb21TextureDatabaseFormat", TextureDatabaseRuntime_Load);
	HOOK("_ZN9CTxdStore14TxdStoreFindCBEPKc", CTxdStore_TxdStoreFindCB);

	// SAMP hooks //

	// UI
	HOOK("_Z12ANDRunThreadPv", AND_RunThread);
	HOOK("_Z12FrontendIdlev", FrontendIdle);
	HOOK("_Z13Render2dStuffv", Render2dStuff);
	HOOK("_Z4IdlePvb", Idle);
	HOOK("_Z14AND_TouchEventiiii", AND_TouchEvent);
	HOOK("_ZN14CLoadingScreen15DisplayPCScreenEv", DisplayScreen);

	// Game process
	HOOK("_ZN5CGame7ProcessEv", CGame_Process);
	HOOK("_ZN5CDraw20CalculateAspectRatioEv", CDraw_CalculateAspectRatio);
	if (CleoEngine::isEnabled())
	{
		HOOK("_ZN5CText3GetEPKc", CText_Get);
	}

	// click map rpc
	HOOK("_ZN6CRadar12SetCoordBlipE9eBlipType7CVectorj12eBlipDisplayPc", CRadar_SetCoordBlip);

	// gangzone
	HOOK("_ZN6CRadar20DrawRadarGangOverlayEb", CRadar_DrawRadarGangOverlay);

	// radar color
	HOOK("_ZN11CHudColours12GetIntColourEh", CHudColours_GetIntColour);

	// vehicle collisions
	HOOK("_ZN11CAutomobile22ProcessEntityCollisionEP7CEntityP9CColPoint", CAutoMobile_ProcessEntityCollision);
	HOOK("_ZN5CBike22ProcessEntityCollisionEP7CEntityP9CColPoint", CBike_ProcessEntityCollision);
	HOOK("_ZN13CMonsterTruck22ProcessEntityCollisionEP7CEntityP9CColPoint", CMonsterTruck_ProcessEntityCollision);
	HOOK("_ZN8CTrailer22ProcessEntityCollisionEP7CEntityP9CColPoint", CTrailer_ProcessEntityCollision);
	HOOK("_ZN8CTrailer12BreakTowLinkEv", CTrailer_BreakTowLink);

	// OOB entities: wrap bound rects so CPhysical::Add accepts them
	HOOK("_ZNK9CPhysical12GetBoundRectEv", CPhysical_GetBoundRect);

	// obj material
	HOOK("_ZN7CObject6RenderEv", CObject_Render);

	// render objects 3000+- pos
	HOOK("_ZN9CRenderer24RenderEverythingBarRoadsEv", CRenderer_RenderEverythingBarRoads);

	// attched objects
	HOOK("_ZN6CWorld25ProcessPedsAfterPreRenderEv", CWorld_ProcessPedsAfterPreRender);

	// crash fixes
	HOOK("_ZN9CPlantMgr45_ProcessEntryCollisionDataSections_AddLocTrisEP17CPlantColEntEntryRK7CVectoriii", CPlantMgr_AddLocTris);
	HOOK("_ZN6CRadar9ClearBlipEi", CRadar_ClearBlip);
	HOOK("_ZN6CWorld28FindPlayerSlotWithPedPointerEPv", CWorld_FindPlayerSlotWithPedPointer);
	HOOK("_Z22MaterialUpdateUVAnimCBP10RpMaterialPv", MaterialUpdateUVAnimCB);
	HOOK("_Z30_rpMatFXOpenGLAllInOneRenderCBP10RwResEntryPvhj", rpMatFXOpenGLAllInOneRenderCB);
	HOOK("_ZN25CCustomBuildingDNPipeline18CustomPipeRenderCBEP10RwResEntryPvhj", CustomPipeRenderCB);
	HOOK("_ZN23CCustomBuildingPipeline18CustomPipeRenderCBEP10RwResEntryPvhj", CustomBuildingPipeRenderCB);
	HOOK("_Z32_rxOpenGLDefaultAllInOneRenderCBP10RwResEntryPvhj", rxOpenGLDefaultAllInOneRenderCB);
	HOOK("_Z16RwTextureDestroyP9RwTexture", _RwTextureDestroy);
	HOOK("_Z15RwFrameAddChildP7RwFrameS0_", RwFrameAddChild);
	HOOK("_Z13RLEDecompressPhjPKhjj", RLEDecompress);
	HOOK("_Z23RwResourcesFreeResEntryP10RwResEntry", RwResourcesFreeResEntry);
	HOOK("_Z34_rxOpenGLDefaultAtomicAllInOneNodeP14RxPipelineNodePK19RxPipelineNodeParam", rxOpenGLDefaultAtomicAllInOneNode);
	HOOK("_ZN22TextureDatabaseRuntime8GetEntryEPKcRb", TextureDatabaseRuntime_GetEntry);
	HOOK("_ZN10CPlayerPed29GetPlayerInfoForThisPlayerPedEv", GetPlayerInfoForThisPlayerPed);

	if (!eglGetProcAddress("glAlphaFuncQCOM"))
	{
		// If "glAlphaFuncQCOM" is not available, try "glAlphaFunc"
		if (eglGetProcAddress("glAlphaFunc"))
		{
			// If "glAlphaFunc" is found, store the address in the global library
			*((void **)(g_saSym->Abs(VER_x32 ? 0x6BCBF8 : 0x89A1B0))) = (void *)eglGetProcAddress("glAlphaFunc");
		}
		else
		{
			// If neither function is available, hook the fallback symbol
			HOOK("_Z25RQ_Command_rqSetAlphaTestRPc", RQCommand_rqSetAlphaTest);
		}
	}

	// fix spin on keyboard/controller
	HOOK("_ZN4CHID12GetInputTypeEv", GetInputType);
	HOOK("_ZN4CHID8ReplacesE10HIDMapping", CHID_Replaces);
	HOOK("_ZN4CHID10ImplementsE10HIDMapping", CHID_Implements);
	HOOK("_ZN15CTouchInterface27RepositionAdjustableWidgetsEv", TI_RepositionAdjustableWidgets);

	Pickups::installHooks();
	RemoveBuilding::installHooks();

	installPadHooks();
	installWidgetFixHooks();
	installCrosshairFixHooks();
}
