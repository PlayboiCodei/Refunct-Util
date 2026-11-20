#pragma once
#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace refunct::ghost_replay_cube_contact {
struct Point { float x,y,z; };
struct Region { Point center{},extent{}; bool sphere=false; };
// Ghosts store capsule centres. The standing half-height is also used by
// the existing ghost renderer. Older tracks do not contain crouch state.
constexpr double kCapsuleRadius=34.0, kHalfHeight=88.0;
constexpr double kSpineHalfHeight=kHalfHeight-kCapsuleRadius;
inline bool Finite(Point p) {return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
inline bool Valid(const Region& r) {
    return Finite(r.center)&&Finite(r.extent)&&r.extent.x>0&&r.extent.y>0&&r.extent.z>0&&
        r.extent.x<=250&&r.extent.y<=250&&r.extent.z<=250&&
        std::abs(r.center.x)<1000000&&std::abs(r.center.y)<1000000&&std::abs(r.center.z)<1000000;
}
inline bool Continuous(Point a,Point b,bool continuous) {
    if (!continuous||!Finite(a)||!Finite(b)) return false;
    const double x=double(b.x)-a.x,y=double(b.y)-a.y,z=double(b.z)-a.z;
    return x*x+y*y+z*z<=2500.0*2500.0;
}
inline bool NearXY(Point a,Point b,const Region& r) {
    if (!Valid(r)||!Finite(a)||!Finite(b)) return false;
    return (std::min)(a.x,b.x)<=r.center.x+r.extent.x+kCapsuleRadius &&
        (std::max)(a.x,b.x)>=r.center.x-r.extent.x-kCapsuleRadius &&
        (std::min)(a.y,b.y)<=r.center.y+r.extent.y+kCapsuleRadius &&
        (std::max)(a.y,b.y)>=r.center.y-r.extent.y-kCapsuleRadius;
}
// Exact minimum distance from a moving capsule spine to a sphere centre.
// Split at the two vertical spine endpoints: each piece is quadratic.
inline bool CrossedSphere(Point a,Point b,const Region& r) {
    const double x=double(a.x)-r.center.x,y=double(a.y)-r.center.y,z=double(a.z)-r.center.z;
    const double dx=double(b.x)-a.x,dy=double(b.y)-a.y,dz=double(b.z)-a.z;
    double cuts[4]{0,1,0,0};int n=2;
    if (dz!=0) for (double plane:{-kSpineHalfHeight,kSpineHalfHeight}) {
        const double t=(plane-z)/dz;if (t>0&&t<1) cuts[n++]=t;
    }
    std::sort(cuts,cuts+n);
    const double radius=r.extent.x+kCapsuleRadius;
    for (int i=0;i+1<n;++i) {
        const double lo=cuts[i],hi=cuts[i+1],mid=z+dz*(lo+hi)*0.5;
        const bool outside=std::abs(mid)>kSpineHalfHeight;
        const double vz=outside?dz:0,oz=outside?z-std::copysign(kSpineHalfHeight,mid):0;
        const double denom=dx*dx+dy*dy+vz*vz;
        const double t=denom>0?std::clamp(-(x*dx+y*dy+oz*vz)/denom,lo,hi):lo;
        const double xx=x+t*dx,yy=y+t*dy,zz=oz+t*vz;
        if (xx*xx+yy*yy+zz*zz<=radius*radius) return true;
    }
    return false;
}
// Conservative fallback when native sphere geometry is not exposed: sweep
// the centre through the actor's collision AABB expanded by the capsule.
inline bool Crossed(Point a,Point b,const Region& r) {
    if (!NearXY(a,b,r)) return false;
    if (r.sphere) return CrossedSphere(a,b,r);
    const double start[3]{a.x,a.y,a.z},end[3]{b.x,b.y,b.z};
    const double center[3]{r.center.x,r.center.y,r.center.z};
    const double extent[3]{r.extent.x+kCapsuleRadius,r.extent.y+kCapsuleRadius,r.extent.z+kHalfHeight};
    double enter=0,leave=1;
    for (int i=0;i<3;++i) {
        const double d=end[i]-start[i],lo=center[i]-extent[i],hi=center[i]+extent[i];
        if (std::abs(d)<1e-8) {if(start[i]<lo||start[i]>hi)return false;continue;}
        const double t0=(lo-start[i])/d,t1=(hi-start[i])/d;
        enter=(std::max)(enter,(std::min)(t0,t1));leave=(std::min)(leave,(std::max)(t0,t1));
        if(enter>leave)return false;
    }
    return true;
}
} // namespace refunct::ghost_replay_cube_contact
