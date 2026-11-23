#include "Game.h"
#include "../Client.h"
#include <atomic>

sa::CRunningScript* gst;
char ScriptBuf[0xff];
uint32_t* pdwParamVars[MAX_SCRIPT_VARS];

uint8_t ExecuteScriptBuf()
{
	gst->m_pPCPointer = (uint8_t*) ScriptBuf;
	gst->ProcessOneCommand();
	return gst->m_bCmpFlag;
}

void InitScripting()
{
	spdlog::info("Initiating scripting...");
	if (!gst) {
		gst = new sa::CRunningScript{};
	}
}

std::atomic<uint16_t> g_lastScriptOpcode{0};

int ScriptCommand(const SCRIPT_COMMAND* pScriptCommand, ...)
{
	// join-time script calls (e.g. spawn weapon setup 0x087E) can run
	// before InitScripting — lazy-init instead of dereferencing null gst
	if (!gst) {
		InitScripting();
	}

	g_lastScriptOpcode.store(pScriptCommand->OpCode, std::memory_order_relaxed);

	va_list ap;
	const char* p = pScriptCommand->Params;
	va_start(ap, pScriptCommand);
	memcpy(&ScriptBuf, &pScriptCommand->OpCode, 2);
	int buf_pos = 2;
	uint16_t var_pos = 0;

	for (int i = 0; i < MAX_SCRIPT_VARS; i++)
		gst->m_nLocals[i] = 0;

	while (*p) {
		switch (*p) {
			case 'i': {
				if (buf_pos + 5 > (int) sizeof(ScriptBuf)) { va_end(ap); return 0; }
				int i = va_arg(ap, int);
				ScriptBuf[buf_pos] = 0x01;
				buf_pos++;
				memcpy(&ScriptBuf[buf_pos], &i, 4);
				buf_pos += 4;
				break;
			}
			case 'f': {
				if (buf_pos + 5 > (int) sizeof(ScriptBuf)) { va_end(ap); return 0; }
				float f = (float) va_arg(ap, double);
				ScriptBuf[buf_pos] = 0x06;
				buf_pos++;
				memcpy(&ScriptBuf[buf_pos], &f, 4);
				buf_pos += 4;
				break;
			}
			case 'v': {
				if (buf_pos + 3 > (int) sizeof(ScriptBuf) || var_pos >= MAX_SCRIPT_VARS) { va_end(ap); return 0; }
				uint32_t* v = va_arg(ap, uint32_t*);
				if (!v) { va_end(ap); return 0; }
				ScriptBuf[buf_pos] = 0x03;
				buf_pos++;
				pdwParamVars[var_pos] = v;
				gst->m_nLocals[var_pos] = *v;
				memcpy(&ScriptBuf[buf_pos], &var_pos, 2);
				buf_pos += 2;
				var_pos++;
				break;
			}
			case 's': {
				char* sz = va_arg(ap, char*);
				if (!sz) { va_end(ap); return 0; }
				size_t len = strlen(sz);
				if (len > 255) len = 255;
				if (buf_pos + 2 + (int) len > (int) sizeof(ScriptBuf)) { va_end(ap); return 0; }
				unsigned char aLen = (unsigned char) len;
				ScriptBuf[buf_pos] = 0x0E;
				buf_pos++;
				ScriptBuf[buf_pos] = aLen;
				buf_pos++;
				memcpy(&ScriptBuf[buf_pos], sz, aLen);
				buf_pos += aLen;
				break;
			}
			case 'z': {
				if (buf_pos + 1 > (int) sizeof(ScriptBuf)) { va_end(ap); return 0; }
				ScriptBuf[buf_pos] = 0x00;
				buf_pos++;
				break;
			}
			default: {
				va_end(ap);
				return 0;
			}
		}
		++p;
	}
	va_end(ap);

	int result = ExecuteScriptBuf();
	if (var_pos) {
		for (int i = 0; i < var_pos; i++)
			*pdwParamVars[i] = gst->m_nLocals[i];
	}

	return result;
}
