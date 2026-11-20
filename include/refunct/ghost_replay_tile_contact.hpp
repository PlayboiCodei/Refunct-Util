#pragma once
// Pure swept-footstep geometry. No UE object access, calls or allocations.
#include <algorithm>
#include <cmath>

namespace refunct::ghost_replay_tile_contact {
struct Point { float x, y, z; };
struct Bounds { Point center, extent; };
constexpr float kCapsuleFeet = 88.f; // Refunct's existing 3D ghost model.
constexpr float kHorizontalMargin = 16.f;
constexpr float kBelowSurface = 38.f;
constexpr float kAboveSurface = 58.f;
constexpr float kMaxContinuousStep = 2500.f; // Recording teleport discontinuity.
inline bool Valid(const Bounds& b) {
    return std::isfinite(b.center.x) && std::isfinite(b.center.y) &&
        std::isfinite(b.center.z) && std::isfinite(b.extent.x) &&
        std::isfinite(b.extent.y) && std::isfinite(b.extent.z) &&
        b.extent.x >= 1.f && b.extent.x <= 800.f &&
        b.extent.y >= 1.f && b.extent.y <= 800.f &&
        b.extent.z >= 1.f && b.extent.z <= 1000.f &&
        std::abs(b.center.x) < 1000000.f &&
        std::abs(b.center.y) < 1000000.f &&
        std::abs(b.center.z) < 1000000.f;
}
// Convert the static mesh's local bounding box into a world-axis-aligned
// box relative to its component location (UE FRotator: pitch, yaw, roll).
// Compute all eight corners to handle rotated and negatively scaled meshes.
inline bool TransformMeshBounds(Point minimum, Point maximum, Point rotation,
                                Point scale, Bounds& result) {
    const auto finite = [](Point p) {
        return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
    };
    if (!finite(minimum) || !finite(maximum) || !finite(rotation) || !finite(scale) ||
        minimum.x >= maximum.x || minimum.y >= maximum.y || minimum.z >= maximum.z ||
        std::abs(scale.x) < 0.001f || std::abs(scale.y) < 0.001f ||
        std::abs(scale.z) < 0.001f) return false;
    constexpr double radians = 0.01745329251994329577;
    const double sp=std::sin(rotation.x*radians), cp=std::cos(rotation.x*radians);
    const double sy=std::sin(rotation.y*radians), cy=std::cos(rotation.y*radians);
    const double sr=std::sin(rotation.z*radians), cr=std::cos(rotation.z*radians);
    const double matrix[3][3] = {
        {cp*cy, sr*sp*cy-cr*sy, -(cr*sp*cy+sr*sy)},
        {cp*sy, sr*sp*sy+cr*cy, cy*sr-cr*sp*sy},
        {sp, -sr*cp, cr*cp}
    };
    double lo[3]{},hi[3]{};
    for (int i=0;i<8;++i) {
        const double local[3] = {
            static_cast<double>(i&1 ? maximum.x : minimum.x)*scale.x,
            static_cast<double>(i&2 ? maximum.y : minimum.y)*scale.y,
            static_cast<double>(i&4 ? maximum.z : minimum.z)*scale.z
        };
        for (int j=0;j<3;++j) {
            const double v=matrix[j][0]*local[0]+matrix[j][1]*local[1]+matrix[j][2]*local[2];
            if (!std::isfinite(v)) return false;
            if (i==0) lo[j]=hi[j]=v;
            else {lo[j]=(std::min)(lo[j],v);hi[j]=(std::max)(hi[j],v);}
        }
    }
    result = {{static_cast<float>((lo[0]+hi[0])*0.5),
               static_cast<float>((lo[1]+hi[1])*0.5),
               static_cast<float>((lo[2]+hi[2])*0.5)},
              {static_cast<float>((hi[0]-lo[0])*0.5),
               static_cast<float>((hi[1]-lo[1])*0.5),
               static_cast<float>((hi[2]-lo[2])*0.5)}};
    return Valid(result);
}
// A swept AABB/slab test avoids skipping small tiles at 2x replay speed.
// The recorded position is the player capsule centre, not their foot.
inline bool Crossed(const Point& from, const Point& to, const Bounds& tile) {
    if (!Valid(tile) || !std::isfinite(from.x) || !std::isfinite(from.y) ||
        !std::isfinite(from.z) || !std::isfinite(to.x) ||
        !std::isfinite(to.y) || !std::isfinite(to.z)) return false;
    const double dx = static_cast<double>(to.x) - from.x;
    const double dy = static_cast<double>(to.y) - from.y;
    const double dz = static_cast<double>(to.z) - from.z;
    const bool teleported = dx*dx + dy*dy + dz*dz >
        static_cast<double>(kMaxContinuousStep)*kMaxContinuousStep;
    const Point a = teleported ? to : from;
    const double surface = static_cast<double>(tile.center.z) + tile.extent.z;
    const double lower[3] = {tile.center.x - tile.extent.x - kHorizontalMargin,
                             tile.center.y - tile.extent.y - kHorizontalMargin,
                             surface + kCapsuleFeet - kBelowSurface};
    const double upper[3] = {tile.center.x + tile.extent.x + kHorizontalMargin,
                             tile.center.y + tile.extent.y + kHorizontalMargin,
                             surface + kCapsuleFeet + kAboveSurface};
    const double start[3] = {a.x,a.y,a.z};
    const double end[3] = {to.x,to.y,to.z};
    double enter=0.0,leave=1.0;
    for (int i=0;i<3;++i) {
        const double d=end[i]-start[i];
        if (std::abs(d)<1e-7) {
            if (start[i]<lower[i] || start[i]>upper[i]) return false;
            continue;
        }
        const double t0=(lower[i]-start[i])/d;
        const double t1=(upper[i]-start[i])/d;
        enter=std::max(enter,std::min(t0,t1));
        leave=std::min(leave,std::max(t0,t1));
        if (enter>leave) return false;
    }
    return true;
}
// Cheap XY-only reject before asking Unreal for the tile's *current* Z.
inline bool CrossedXY(const Point& from, const Point& to,
                      const Bounds& tile) {
    if (!Valid(tile)) return false;
    Bounds tall=tile;
    tall.center.z=0;
    tall.extent.z=1;
    Point a=from,b=to;
    a.z=b.z=kCapsuleFeet+1;
    return Crossed(a,b,tall);
}
} // namespace refunct::ghost_replay_tile_contact
