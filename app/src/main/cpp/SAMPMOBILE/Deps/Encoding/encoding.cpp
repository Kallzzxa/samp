#include "include/encoding.h"

#include <spdlog/spdlog.h>
#include <iconv.h>
#include <stdexcept>

// Server codepage used for decoding (cp2utf) and encoding (utf2cp).
// Default is GBK so that Chinese servers display correctly out of the box.
// Can be changed at runtime via Encoding::setCharset (e.g. "CP1251", "GBK", "BIG5").
static std::string g_charset = "GBK";

void Encoding::setCharset(const std::string& cs)
{
	if (!cs.empty()) {
		g_charset = cs;
	}
}

const std::string& Encoding::charset()
{
	return g_charset;
}

static std::string iconv_convert(const std::string& s, const char* to, const char* from)
{
	iconv_t cd = iconv_open(to, from);
	if (cd == (iconv_t) -1) {
		spdlog::error("iconv_open failed ({} -> {}): {}", from, to, strerror(errno));
		return s;
	}

	std::string in = s;
	std::string out;
	out.resize(in.size() * 4 + 4);
	char* inbuf = const_cast<char*>(in.data());
	char* outbuf = const_cast<char*>(out.data());
	size_t inbytesleft = in.size();
	size_t outbytesleft = out.size();
	size_t res = iconv(cd, &inbuf, &inbytesleft, &outbuf, &outbytesleft);
	iconv_close(cd);
	if (res == (size_t) -1) {
		spdlog::error("iconv failed ({} -> {}): {}", from, to, strerror(errno));
		return s;
	}
	out.resize(out.size() - outbytesleft);
	return out;
}

// Strict UTF-8 validation: rejects overlong forms, surrogates and
// out-of-range codepoints. Some servers send UTF-8 directly regardless
// of the configured legacy charset, so valid UTF-8 is passed through
// untouched instead of being double-converted into mojibake.
static bool is_valid_utf8(const std::string& s)
{
	size_t i = 0, n = s.size();
	while (i < n) {
		unsigned char c = (unsigned char) s[i];
		size_t len;
		uint32_t cp;
		if (c < 0x80) { ++i; continue; }
		else if (c < 0xC2) return false;              // stray continuation / overlong
		else if (c < 0xE0) { len = 2; cp = c & 0x1F; }
		else if (c < 0xF0) { len = 3; cp = c & 0x0F; }
		else if (c < 0xF5) { len = 4; cp = c & 0x07; }
		else return false;
		if (i + len > n) return false;
		for (size_t j = 1; j < len; ++j) {
			unsigned char t = (unsigned char) s[i + j];
			if ((t & 0xC0) != 0x80) return false;
			cp = (cp << 6) | (t & 0x3F);
		}
		if ((len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000)) return false; // overlong
		if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;        // invalid
		i += len;
	}
	return true;
}

std::string Encoding::cp2utf(const std::string& s)
{
	if (s.empty()) return s;
	if (g_charset == "UTF-8" || g_charset == "utf8") {
		// UTF-8 selected: pass valid UTF-8 through, but treat invalid
		// sequences as GBK so a legacy-charset server still decodes.
		if (is_valid_utf8(s)) return s;
		return iconv_convert(s, "UTF-8", "GBK");
	}
	if (is_valid_utf8(s)) return s;
	return iconv_convert(s, "UTF-8", g_charset.c_str());
}

std::string Encoding::utf2cp(const std::string& s)
{
	if (s.empty()) return s;
	if (g_charset == "UTF-8" || g_charset == "utf8") return s;
	return iconv_convert(s, g_charset.c_str(), "UTF-8");
}
