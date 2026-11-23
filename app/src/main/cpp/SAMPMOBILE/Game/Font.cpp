#include "../Client.h"
#include "Font.h"

void CFont::StringToAscii(const char* str, uint16_t* ascii)
{
	const char* ptr = str;
	while (*ptr) {
		unsigned char ch1 = *ptr;
		if (ch1 < 0x80) {
			*ascii++ = ch1;
		}
		else if (ch1 >= 0xC0 && ch1 <= 0xDF) {
			unsigned char ch2 = ptr[1];
			*ascii++ = ((ch1 & 0x1F) << 6) | (ch2 & 0x3F);
			ptr++;
		}
		else if (ch1 >= 0xE0 && ch1 <= 0xEF) {
			unsigned char ch2 = ptr[1];
			unsigned char ch3 = ptr[2];
			*ascii++ = ((ch1 & 0x0F) << 12) | ((ch2 & 0x3F) << 6) | (ch3 & 0x3F);
			ptr += 2;
		}
		else if (ch1 >= 0xF0 && ch1 <= 0xF7) {
			// Supplementary-plane characters cannot be represented in the
			// 16-bit output buffer; replace with '?'.
			*ascii++ = 63;
			ptr += 3;
		}
		else {
			// Invalid character
			ptr++;
		}
		ptr++;
	}
	*ascii = 0; // Terminate with zero
}

void CFont::AsciiToGxtCharU8(uint16_t* ascii, uint16_t* gxt)
{
	while (*ascii) {
		int asciiValue = static_cast<int>(*ascii);

		bool found = false;
		for (auto& i : g_asciiMap) {
			if (i[0] == asciiValue) {
				*gxt++ = static_cast<uint16_t>(i[1]);
				found = true;
				break;
			}
		}
		if (!found) {
			// handle unknown characters
			*gxt++ = 63; // Fill unsupported text with question marks
		}

		ascii++;
	}
	*gxt = 0; // Terminate with zero
}

void CFont::AsciiToGxtChar(const char* ascii, uint16_t* gxt)
{
	if (ascii) {
		uint8_t currentChar = (uint8_t) *ascii;
		if (*ascii) {
			const char* nextCharPointer = ascii + 1;
			do {
				*gxt++ = currentChar;
				currentChar = (uint8_t) *nextCharPointer++;
			}
			while (currentChar);
		}
		*gxt = 0;
	}
}

void CFont::SetScale(float x, float y)
{
	sa::CFontDetails& fontDetails = *eng::as<sa::CFontDetails*>(eng::Sym::CFont_Details);
	fontDetails.ScaleX = x;
	fontDetails.ScaleY = y;
}

void CFont::SetColor(RGBA* dwColor)
{
	eng::fn::CFont_SetColor( dwColor);
}

void CFont::SetJustify(uint8_t just)
{
	eng::fn::CFont_SetJustify( just);
}

void CFont::SetOrientation(uint8_t orientation)
{
	eng::fn::CFont_SetOrientation( orientation);
}

void CFont::SetWrapX(float wrap)
{
	eng::fn::CFont_SetWrapx( wrap);
}

void CFont::SetCentreSize(float size)
{
	eng::fn::CFont_SetCentreSize( size);
}

void CFont::SetBackground(uint8_t bBackground, uint8_t bOnlyText)
{
	eng::fn::CFont_SetBackground( bBackground, bOnlyText);
}

void CFont::SetBackgroundColor(RGBA* dwColor)
{
	eng::fn::CFont_SetBackgroundColor( dwColor);
}

void CFont::SetProportional(uint8_t prop)
{
	eng::fn::CFont_SetProportional( prop);
}

void CFont::SetDropShadowPosition(uint8_t pos)
{
	eng::fn::CFont_SetDropShadowPosition( pos);
}

void CFont::SetDropColor(RGBA* dwColor)
{
	eng::fn::CFont_SetDropColor( dwColor);
}

void CFont::PrintString(float fX, float fY, const char* szText)
{
	auto gxt_string = new uint16_t[800];
	//AsciiToGxtChar(szText, gxt_string);

	uint16_t szAscii[801];
	StringToAscii(Encoding::cp2utf(szText).c_str(), szAscii);
	AsciiToGxtCharU8(szAscii, gxt_string);

	// CFont::PrintString
	eng::fn::CFont_PrintString( fX, fY, gxt_string);

	delete[] gxt_string;

	// CFont::RenderFontBuffer
	eng::fn::CFont_RenderFontBuffer();
}

void CFont::SetFontStyle(uint8_t style)
{
	eng::fn::CFont_SetFontStyle( style);
}

void CFont::SetEdge(uint8_t edge)
{
	eng::fn::CFont_SetEdge( edge);
}





