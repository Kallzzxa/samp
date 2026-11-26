#pragma once

/**
 * Pure geometry for the World2D overlay layer (no JNI, no game symbols, no android dependency).
 *
 * The reason it is a standalone header, same as CefLogic.java: this is the formula that "decides where the page is drawn",
 * if written wrong, the page drifts off-screen or ends up the wrong size, while device-side logcat on this ROM is
 * unreliable, so you cannot tell whether the formula or the projection is wrong. The formula is constexpr, so the group of
 * static_assert runs on **every compile** -- effectively a permanent regression test hanging on this formula.
 *
 * Semantics mirror PC src/client/core/browser/manager.cpp:1569 area:
 *
 *   if (!CSprite::CalcScreenCoors(pos, &screen, &w, &h, true, true)) continue;
 *   if (screen.z <= 0.0f) continue;                       // Behind the camera
 *   ImVec2 drawPos(screen.x - size.x * pivot.x,
 *                  screen.y - size.y * pivot.y);
 */
namespace cefworld
{

struct Placement
{
    float x = 0.0f;          // Top-left corner of the page (screen pixels)
    float y = 0.0f;
    bool onScreen = false;   // false = should not be shown this frame
};

constexpr float Abs(float v) noexcept
{
    return v < 0.0f ? -v : v;
}

/**
 * Projection result -> page top-left corner.
 *
 * @param screenX/Y/Z CSprite::CalcScreenCoors output
 * @param width/height Browser viewport pixel size
 * @param pivotX/Y Anchor (PC default 0.5 / 1.0, i.e. bottom-centred)
 * @param projected Whether the projection function returned true
 */
constexpr Placement Place2D(float screenX, float screenY, float screenZ,
                            float width, float height,
                            float pivotX, float pivotY,
                            bool projected) noexcept
{
    // For a point behind the camera, screen.z <= 0, PC also skips it directly (otherwise it would be mirrored onto the face).
    if (!projected || !(screenZ > 0.0f))
        return Placement{ 0.0f, 0.0f, false };

    return Placement{
        screenX - width * pivotX,
        screenY - height * pivotY,
        true
    };
}

/** A position change >= eps is worth one JNI pass (PC redraws every frame; per-frame JNI on Android is too expensive). */
constexpr bool MovedBy(float a, float b, float eps = 0.5f) noexcept
{
    return !(Abs(a - b) < eps);
}

// ---------------------------------------------------------------------------
// Compile-time regression. Values come from one World2D measured on a real device:
//   world=(2090.6,1164.6,11.6) -> screen=(1149.0,239.3)
//   size=512x256, pivot=(0.5,1.0) -> pos=(893,-17)   (893 = 1149 - 256,
//   -17 = 239 - 256; page bottom sits exactly on the projection point above the character's head)
// ---------------------------------------------------------------------------

// Bottom-centred anchor: x minus half width, y minus full height.
static_assert(Place2D(1000.0f, 500.0f, 10.0f, 512.0f, 256.0f, 0.5f, 1.0f, true).x == 744.0f);
static_assert(Place2D(1000.0f, 500.0f, 10.0f, 512.0f, 256.0f, 0.5f, 1.0f, true).y == 244.0f);
static_assert(Place2D(1000.0f, 500.0f, 10.0f, 512.0f, 256.0f, 0.5f, 1.0f, true).onScreen);

// That frame on the real device.
static_assert(Place2D(1149.0f, 239.0f, 11.6f, 512.0f, 256.0f, 0.5f, 1.0f, true).x == 893.0f);
static_assert(Place2D(1149.0f, 239.0f, 11.6f, 512.0f, 256.0f, 0.5f, 1.0f, true).y == -17.0f);

// Top-left anchor: no shift.
static_assert(Place2D(300.0f, 200.0f, 5.0f, 128.0f, 64.0f, 0.0f, 0.0f, true).x == 300.0f);
static_assert(Place2D(300.0f, 200.0f, 5.0f, 128.0f, 64.0f, 0.0f, 0.0f, true).y == 200.0f);

// Centre anchor.
static_assert(Place2D(300.0f, 200.0f, 5.0f, 128.0f, 64.0f, 0.5f, 0.5f, true).x == 236.0f);
static_assert(Place2D(300.0f, 200.0f, 5.0f, 128.0f, 64.0f, 0.5f, 0.5f, true).y == 168.0f);

// Behind the camera (z <= 0) and projection failure must both hide it, with coordinates zeroed (so the previous frame's position is not reused).
static_assert(!Place2D(100.0f, 100.0f, 0.0f, 64.0f, 64.0f, 0.5f, 0.5f, true).onScreen);
static_assert(!Place2D(100.0f, 100.0f, -3.0f, 64.0f, 64.0f, 0.5f, 0.5f, true).onScreen);
static_assert(!Place2D(100.0f, 100.0f, 10.0f, 64.0f, 64.0f, 0.5f, 0.5f, false).onScreen);
static_assert(Place2D(100.0f, 100.0f, -3.0f, 64.0f, 64.0f, 0.5f, 0.5f, true).x == 0.0f);
static_assert(Place2D(100.0f, 100.0f, 10.0f, 64.0f, 64.0f, 0.5f, 0.5f, false).y == 0.0f);

// Sub-pixel jitter threshold: < 0.5px no callback, >= 0.5px callback; must hold in both directions.
static_assert(!MovedBy(0.0f, 0.4f));
static_assert(MovedBy(0.0f, 0.5f));
static_assert(!MovedBy(100.0f, 100.4f));
static_assert(MovedBy(100.0f, 99.5f));
static_assert(!MovedBy(-0.4f, 0.0f));
static_assert(MovedBy(-0.5f, 0.0f));

} // namespace cefworld
