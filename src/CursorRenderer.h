#pragma once

// Decodes a system cursor into straight BGRA pixels so it can be uploaded as a
// texture and drawn magnified.
//
// The monochrome path is not optional. Cursors with hbmColor == NULL store a
// double-height mask: the top half is AND, the bottom half is XOR. I-beam is
// monochrome, and it is the cursor a magnifier user spends the most time
// looking at, so skipping that path would leave the feature visibly
// half-built.
//
// Output is premultiplied so the renderer can blend with ONE / INV_SRC_ALPHA.
// Straight alpha with SRC_ALPHA / INV_SRC_ALPHA leaves a halo on the magnified
// edges, which is exactly where magnification makes it obvious.

#ifndef BETTER_MAGNIFIER_CURSOR_RENDERER_H
#define BETTER_MAGNIFIER_CURSOR_RENDERER_H

#include <windows.h>
#include <cstdint>
#include <vector>

namespace BetterMagnifier {

struct CursorBitmap
{
    std::vector<uint32_t> pixels;   // BGRA, premultiplied, row-major, top-down
    int width    = 0;
    int height   = 0;
    int hotspotX = 0;
    int hotspotY = 0;
};

// False when the handle is null or GDI refuses it. Never throws.
bool DecodeCursor(HCURSOR cursor, CursorBitmap& out);

// Writes a 32-bit top-down BMP. Debug harness only (--dump-cursors).
bool WriteCursorBitmapFile(const CursorBitmap& bmp, const wchar_t* path);

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_CURSOR_RENDERER_H
