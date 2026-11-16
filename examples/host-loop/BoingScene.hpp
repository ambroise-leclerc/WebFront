#pragma once

// The Boing Ball scene: the colour of one pixel as a function of the scene of a frame. The CPU renderer and the CUDA
// kernel both compile this file, so it stays plain C++17: floats, no allocation, no recursion and no WebFront header.
// Rays are bounded by construction: the floor reflects once, and what a glass ball reflects or refracts is the floor
// or the sky, never the ball again. The glass ball also sends a coloured beam through each of its red squares.

#include <cmath>
#include <cstdint>

#ifdef __CUDACC__
    #define BOING_FN __host__ __device__ inline
#else
    #define BOING_FN inline
#endif

namespace boing {

struct Vec {
    float x, y, z;
};
BOING_FN Vec operator+(Vec a, Vec b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
BOING_FN Vec operator-(Vec a, Vec b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
BOING_FN Vec operator*(Vec a, Vec b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}
BOING_FN Vec operator*(Vec a, float s) {
    return {a.x * s, a.y * s, a.z * s};
}
BOING_FN float dot(Vec a, Vec b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
BOING_FN Vec normalize(Vec a) {
    return a * (1.F / sqrtf(dot(a, a)));
}
BOING_FN Vec mix(Vec a, Vec b, float t) {
    return a * (1.F - t) + b * t;
}

/// One frame: where the ball (radius 1) and the light are, how far the ball has turned, and the image to compute.
struct Scene {
    Vec   ball;
    Vec   light;
    float spinCos, spinSin;
    int   glass;    ///< 0: opaque checker, 1: tinted glass that sends coloured beams
    int   width, height;
    int   samples;  ///< rays per pixel along each axis: 1 or 2 (a 2 x 2 grid)
};

/// The animation is a function of time: the ball crosses the floor and back, bounces, and rolls with its travel, so
/// that it turns the other way after each side; the light circles above.
inline Scene sceneAt(float seconds, bool glass, int width, int height, int samples) {
    const auto fraction = [](float value) {
        return value - std::floor(value);
    };
    const float travel = 2.6F * (4.F * std::fabs(fraction(seconds / 7.F) - 0.5F) - 1.F);
    const float bounce = fraction(seconds / 1.2F);
    const float spin   = travel * 1.9F;
    Scene       scene{};
    scene.ball    = {travel, 1.F + 8.4F * bounce * (1.F - bounce), 0.F};
    scene.light   = {3.5F * std::cos(seconds * 0.6F), 6.5F, 3.F + 1.5F * std::sin(seconds * 0.6F)};
    scene.spinCos = std::cos(spin);
    scene.spinSin = std::sin(spin);
    scene.glass   = glass ? 1 : 0;
    scene.width   = width;
    scene.height  = height;
    scene.samples = samples;
    return scene;
}

/// Distance along a normalized ray to the ball, or 0 when the ray misses it.
BOING_FN float hitBall(const Scene& scene, Vec origin, Vec direction) {
    const Vec   toOrigin     = origin - scene.ball;
    const float half         = dot(toOrigin, direction);
    const float discriminant = half * half - (dot(toOrigin, toOrigin) - 1.F);
    if (discriminant < 0.F)
        return 0.F;
    const float distance = -half - sqrtf(discriminant);
    return distance > 1e-3F ? distance : 0.F;
}

/// Distance along a ray to the floor (the plane y = 0), or 0 when the ray does not go down.
BOING_FN float hitFloor(Vec origin, Vec direction) {
    return direction.y < -1e-4F ? -origin.y / direction.y : 0.F;
}

/// How bright the day is: the glass ball sends coloured beams, which show in a dimmer scene.
BOING_FN float daylight(const Scene& scene) {
    return scene.glass ? 0.4F : 1.F;
}

BOING_FN Vec sky(const Scene& scene, Vec direction) {
    const float up = direction.y < 0.F ? 0.F : (direction.y > 0.6F ? 1.F : direction.y / 0.6F);
    return mix({0.72F, 0.74F, 0.78F}, {0.30F, 0.38F, 0.55F}, up) * daylight(scene);
}

/// The square of the checker that a direction from the centre of the ball points at, as latitude * 16 + longitude:
/// 8 bands of latitude and 16 of longitude, on a ball tilted and turned about its axis.
BOING_FN int squareAt(const Scene& scene, Vec normal) {
    const float tiltCos = 0.9394F, tiltSin = 0.3429F;  // 0.35 rad
    const Vec   tilted{normal.x * tiltCos + normal.y * tiltSin, normal.y * tiltCos - normal.x * tiltSin, normal.z};
    const Vec   local{tilted.x * scene.spinCos + tilted.z * scene.spinSin, tilted.y, tilted.z * scene.spinCos - tilted.x * scene.spinSin};
    const float bands     = 8.F / 3.14159265F;
    const float height    = local.y < -1.F ? -1.F : (local.y > 1.F ? 1.F : local.y);
    const int   latitude  = static_cast<int>(acosf(height) * bands);
    const int   longitude = static_cast<int>((atan2f(local.z, local.x) + 3.14159265F) * bands);
    return latitude * 16 + (longitude & 15);
}

/// True on the red squares.
BOING_FN bool redSquare(const Scene& scene, Vec normal) {
    const int square = squareAt(scene, normal);
    return (((square >> 4) + square) & 1) != 0;
}

/// The light the glass ball sends in a direction: a beam of one colour through each red square, none through the others.
BOING_FN Vec beam(const Scene& scene, Vec fromBall) {
    const int square = squareAt(scene, fromBall);
    if ((((square >> 4) + square) & 1) == 0)
        return {0.F, 0.F, 0.F};
    const Vec colours[6] = {
        {  1.F, 0.15F, 0.15F},
        {  1.F,  0.7F,  0.1F},
        { 0.2F,   1.F, 0.25F},
        { 0.1F,  0.9F,   1.F},
        {0.25F, 0.35F,   1.F},
        {  1.F,  0.2F,  0.9F}
    };
    return colours[(square * 7) % 6];
}

/// The beams a ray crosses in the haze before it reaches what it sees, `length` away (0: nothing): 32 samples along
/// the ray, shifted by `shift` (0 to 1) from one ray to the next so that the samples do not line up into bands. This
/// is what makes the glass ball expensive: 32 times the work of the rest of the pixel.
BOING_FN Vec beamsAlong(const Scene& scene, Vec origin, Vec direction, float length, float shift) {
    const int   samples = 32;
    const float step    = (length > 0.F && length < 24.F ? length : 24.F) / static_cast<float>(samples);
    Vec         sum{0.F, 0.F, 0.F};
    for (int i = 0; i < samples; ++i) {
        const Vec   fromBall = origin + direction * ((static_cast<float>(i) + shift) * step) - scene.ball;
        const float square   = dot(fromBall, fromBall);
        if (square > 1.F)
            sum = sum + beam(scene, fromBall * (1.F / sqrtf(square))) * (step * 0.6F / (1.F + square));
    }
    return sum;
}

/// The floor seen from `distance`: a purple grid on grey, lit by the point light and by the beams of the glass ball,
/// in the sharp shadow of the ball, fading into the horizon.
BOING_FN Vec floorColor(const Scene& scene, Vec point, float distance) {
    const float lineWidth = 0.025F + 0.0015F * distance;
    const bool  line      = fabsf(point.x - floorf(point.x + 0.5F)) < lineWidth || fabsf(point.z - floorf(point.z + 0.5F)) < lineWidth;
    const Vec   toLight   = normalize(scene.light - point);
    const float shadow    = hitBall(scene, point, toLight) > 0.F ? (scene.glass ? 0.7F : 0.25F) : 1.F;
    const Vec   base      = line ? Vec{0.55F, 0.10F, 0.60F} : Vec{0.66F, 0.66F, 0.66F};
    Vec         lit       = base * ((0.3F + 0.7F * toLight.y * shadow) * daylight(scene));
    if (scene.glass) {
        const Vec   fromBall = point - scene.ball;
        const float square   = dot(fromBall, fromBall);
        const Vec   down     = fromBall * (1.F / sqrtf(square));
        lit                  = lit + base * beam(scene, down) * (-down.y * 4.F / square);
    }
    return mix(Vec{0.72F, 0.74F, 0.78F} * daylight(scene), lit, expf(-0.022F * distance));
}

/// What a ray that cannot meet the ball sees: the floor or the sky.
BOING_FN Vec surroundings(const Scene& scene, Vec origin, Vec direction) {
    const float distance = hitFloor(origin, direction);
    return distance > 0.F ? floorColor(scene, origin + direction * distance, distance) : sky(scene, direction);
}

/// The ball where a ray reaches it: a lit red and white checker, or tinted glass that reflects its surroundings and
/// refracts what lies behind it (in at `point`, out at the other end of the chord; a sphere never traps the ray).
BOING_FN Vec ballColor(const Scene& scene, Vec point, Vec direction) {
    const Vec   normal   = point - scene.ball;
    const Vec   toLight  = normalize(scene.light - point);
    const float diffuse  = dot(normal, toLight) > 0.F ? dot(normal, toLight) : 0.F;
    float       specular = dot(normal, normalize(toLight - direction));
    specular             = specular > 0.F ? specular : 0.F;
    for (int i = 0; i < 5; ++i)
        specular *= specular;  // exponent 32
    const Vec  highlight{specular, specular, specular};
    const bool red = redSquare(scene, normal);
    if (!scene.glass)
        return (red ? Vec{0.90F, 0.07F, 0.05F} : Vec{0.95F, 0.95F, 0.95F}) * (0.25F + 0.75F * diffuse) + highlight * 0.6F;

    const float index     = 1.5F;
    const float cosIn     = -dot(direction, normal);
    const float graze     = 1.F - cosIn;
    const float fresnel   = 0.04F + 0.96F * graze * graze * graze * graze * graze;
    const Vec   reflected = surroundings(scene, point, direction + normal * (2.F * cosIn));
    const Vec   inside    = direction * (1.F / index) + normal * (cosIn / index - sqrtf(1.F - (1.F - cosIn * cosIn) / (index * index)));
    const Vec   leave     = point + inside * (-2.F * dot(inside, normal));
    const Vec   outNormal = leave - scene.ball;
    const float cosOut    = dot(inside, outNormal);
    const float leaving   = 1.F - index * index * (1.F - cosOut * cosOut);
    const Vec   outside   = inside * index - outNormal * (index * cosOut - sqrtf(leaving > 0.F ? leaving : 0.F));
    const Vec   redTint{1.F, 0.28F, 0.22F}, clearTint{0.93F, 0.97F, 1.F};
    const Vec   tint = (red ? redTint : clearTint) * (redSquare(scene, outNormal) ? redTint : clearTint);
    return reflected * fresnel + surroundings(scene, leave, outside) * tint * (1.F - fresnel) + highlight;
}

/// The colour a camera ray brings back: the ball, the floor with the reflection of the ball or of the sky, or the sky.
BOING_FN Vec trace(const Scene& scene, Vec origin, Vec direction) {
    const float toBall  = hitBall(scene, origin, direction);
    const float toFloor = hitFloor(origin, direction);
    if (toBall > 0.F && (toFloor <= 0.F || toBall < toFloor))
        return ballColor(scene, origin + direction * toBall, direction);
    if (toFloor <= 0.F)
        return sky(scene, direction);
    const Vec   point = origin + direction * toFloor;
    const Vec   mirrored{direction.x, -direction.y, direction.z};
    const float reflectedBall = hitBall(scene, point, mirrored);
    const Vec   reflection    = reflectedBall > 0.F ? ballColor(scene, point + mirrored * reflectedBall, mirrored) : sky(scene, mirrored);
    const float fade          = 0.22F * expf(-0.022F * toFloor);
    return floorColor(scene, point, toFloor) * (1.F - fade) + reflection * fade;
}

/// The pixel (x, y) of the image, as the bytes R, G, B, A in memory order on a little-endian machine: what a canvas
/// ImageData holds.
BOING_FN std::uint32_t pixel(const Scene& scene, int x, int y) {
    const Vec   camera{0.F, 2.3F, 8.5F};
    const float halfView = 0.42F, aspect = static_cast<float>(scene.width) / static_cast<float>(scene.height);
    Vec         sum{0.F, 0.F, 0.F};
    for (int sy = 0; sy < scene.samples; ++sy)
        for (int sx = 0; sx < scene.samples; ++sx) {
            const float u = (static_cast<float>(x) + (static_cast<float>(sx) + 0.5F) / static_cast<float>(scene.samples)) / static_cast<float>(scene.width);
            const float v = (static_cast<float>(y) + (static_cast<float>(sy) + 0.5F) / static_cast<float>(scene.samples)) / static_cast<float>(scene.height);
            const Vec   direction = normalize({(2.F * u - 1.F) * aspect * halfView, (1.F - 2.F * v) * halfView - 0.12F, -1.F});
            sum                   = sum + trace(scene, camera, direction);
            if (scene.glass) {
                const float toBall = hitBall(scene, camera, direction), toFloor = hitFloor(camera, direction);
                const auto  ray = static_cast<unsigned>(x) * 1973U + static_cast<unsigned>(y) * 9277U + static_cast<unsigned>(sy * 2 + sx) * 26699U;
                sum             = sum
                      + beamsAlong(scene,
                                   camera,
                                   direction,
                                   toBall > 0.F && (toFloor <= 0.F || toBall < toFloor) ? toBall : toFloor,
                                   static_cast<float>(ray & 255U) / 256.F);
            }
        }
    const float scale = 1.F / static_cast<float>(scene.samples * scene.samples);
    const auto  byte  = [scale](float value) {
        const float shown = sqrtf(value * scale);  // gamma 2
        return static_cast<std::uint32_t>((shown > 1.F ? 1.F : shown) * 255.F + 0.5F);
    };
    return byte(sum.x) | (byte(sum.y) << 8) | (byte(sum.z) << 16) | 0xff000000U;
}

}  // namespace boing
