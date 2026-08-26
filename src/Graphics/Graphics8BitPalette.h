/*
	Author: bitluni 2019
	License:
	Creative Commons Attribution ShareAlike 4.0
	https://creativecommons.org/licenses/by-sa/4.0/

	For further details check out:
		https://youtube.com/bitlunislab
		https://github.com/bitluni
		http://bitluni.net

	Modified for VGA8: pixel byte is now a pure 8-bit palette index.
	Sync bits are handled at the output stage (VGA/HDMI DMA handlers).
*/
#pragma once
#include "Graphics.h"

// Virtual (uniform) framebuffer rows — see the block at the top of Video.cpp.
// A NULL row pointer means "this row is a single colour"; writers must
// materialize the row first, readers take the colour byte.
extern "C" unsigned char* video_fb_row_w(int y);
extern "C" unsigned char  video_fb_row_color(int y);
extern "C" void           video_fb_fill_row(int y, unsigned char c);

class Graphics8BitPalette: public Graphics<unsigned char>
{
	public:
	typedef unsigned char Color;
	static const Color RGBAXMask = 0xff; // full 8-bit palette index
	Color SBits; // kept for VGA sync generation, not embedded in pixels

	Graphics8BitPalette()
	{
		SBits = 0xc0;
		frontColor = 0xff;
	}

	// ── Profi DS80 transparent colour remap ─────────────────────────────────
	// In DS80 mode every framebuffer byte indexes the HDMI "packed-pair" conv_color
	// table (VIDEO::profi_pair_lookup), NOT the standard ZX palette.  All OSD / LED /
	// menu drawing still passes standard ZX colour bytes to dotFast().  To make those
	// render in the intended colour without the caller knowing the video mode, dotFast()
	// remaps the byte through ds80_color_lut[] while ds80_active.
	//
	// 17-entry table indexed by ZX colour index 0..16 (the full range zxColor() can
	// produce: 0..15 + ORANGE=16).  Built by Video.cpp as
	// lut[c] = profi_pair_lookup[c & 0xF][c & 0xF] (ZX colour 0..15 == Profi 0..15;
	// ORANGE wraps to 0 — never used as a draw colour outside the LED indicators,
	// which map themselves).  Rebuilt whenever the Profi palette / pair table changes.
	static bool ds80_active;
	static Color ds80_color_lut[17];

	inline Color mapColor(Color c) const
	{
		// All OSD/menu draws use zxColor() → indices 0..16; clamp defensively.
		return ds80_active ? ds80_color_lut[c <= 16 ? c : 0] : c;
	}

	// Legacy accessors — no longer meaningful for palette indices
	// but kept for API compatibility (dead code, not called)
	virtual int R(Color c) const
	{
		return (((int)c & 3) * 255 + 1) / 3;
	}
	virtual int G(Color c) const
	{
		return (((int)(c >> 2) & 3) * 255 + 1) / 3;
	}
	virtual int B(Color c) const
	{
		return (((int)(c >> 4) & 3) * 255 + 1) / 3;
	}
	virtual int A(Color c) const
	{
		return (((int)(c >> 6) & 3) * 255 + 1) / 3;
	}

	virtual Color RGBA(int r, int g, int b, int a = 255) const
	{
		return ((r >> 6) & 0b11) | ((g >> 4) & 0b1100) | ((b >> 2) & 0b110000) | (a & 0b11000000);
	}

	// A NULL row is a virtual (uniform) border row — materialize before writing,
	// drop the pixel when the heap can't give the row (never crash).
	inline Color* rowW(int y)
	{
		Color* row = frameBuffer[y];
		return row ? row : video_fb_row_w(y);
	}

	virtual void dotFast(int x, int y, Color color)
	{
		Color* row = rowW(y);
		if (row) row[x^2] = mapColor(color);
	}

	virtual void dot(int x, int y, Color color)
	{
		if ((unsigned int)x < xres && (unsigned int)y < yres)
			dotFast(x, y, color);
	}

	virtual void dotAdd(int x, int y, Color color)
	{
		if ((unsigned int)x < xres && (unsigned int)y < yres)
			dotFast(x, y, color); // simplified — was dead code
	}

	virtual void dotMix(int x, int y, Color color)
	{
		if ((unsigned int)x < xres && (unsigned int)y < yres)
			dotFast(x, y, color); // simplified — was dead code
	}

	virtual Color get(int x, int y)
	{
		if ((unsigned int)x < xres && (unsigned int)y < yres) {
			Color* row = frameBuffer[y];
			return row ? row[x^2] : video_fb_row_color(y);
		}
		return 0;
	}

	virtual void clear(Color color = 0)
	{
		// Row-wise: a virtual row just takes the colour byte (no materialization).
		for (int y = 0; y < this->yres; y++)
			video_fb_fill_row(y, color);
	}
};
