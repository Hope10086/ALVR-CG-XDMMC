#pragma once
#ifndef ALXR_XR_UTILS_H
#define ALXR_XR_UTILS_H

#include "pch.h"
#include <limits>

namespace ALXR {;

using float_limits = std::numeric_limits<float>;
constexpr inline const XrPosef IdentityPose { {0,0,0,1},{0,0,0} };
constexpr inline const XrPosef ZeroPose { {0,0,0,0},{0,0,0} };
constexpr inline const XrPosef InfinityPose {
    {
        float_limits::infinity(),
        float_limits::infinity(),
        float_limits::infinity(),
        float_limits::infinity(),
    },
    {
        float_limits::infinity(),
        float_limits::infinity(),
        float_limits::infinity(),
    }
};

constexpr inline bool operator==(const XrPosef& lhs, const XrPosef& rhs) {
    return lhs.position.x == rhs.position.x &&
           lhs.position.y == rhs.position.y &&
           lhs.position.z == rhs.position.z &&
           lhs.orientation.x == rhs.orientation.x &&
           lhs.orientation.y == rhs.orientation.y &&
           lhs.orientation.z == rhs.orientation.z &&
           lhs.orientation.w == rhs.orientation.w;
}

constexpr inline bool operator!=(const XrPosef& lhs, const XrPosef& rhs) {
    return !(lhs == rhs);
}

struct SpaceLoc
{
    XrPosef pose = IdentityPose;
    XrVector3f linearVelocity = { 0,0,0 };
    XrVector3f angularVelocity = { 0,0,0 };

    constexpr inline bool is_zero() const     { return pose == ZeroPose; }
    constexpr inline bool is_infinity() const { return pose == InfinityPose; }
};

constexpr inline const SpaceLoc IdentitySpaceLoc = { IdentityPose, {0,0,0}, {0,0,0} };
constexpr inline const SpaceLoc ZeroSpaceLoc = { ZeroPose, {0,0,0}, {0,0,0} };
constexpr inline const SpaceLoc InfinitySpaceLoc = { InfinityPose, {0,0,0}, {0,0,0} };

inline SpaceLoc GetSpaceLocation
(
    const XrSpace& targetSpace,
    const XrSpace& baseSpace,
    const XrTime& time,
    const SpaceLoc& initLoc = IdentitySpaceLoc
)
{
    XrSpaceVelocity velocity{ XR_TYPE_SPACE_VELOCITY, nullptr };
    XrSpaceLocation spaceLocation{ XR_TYPE_SPACE_LOCATION, &velocity };
    const auto res = xrLocateSpace(targetSpace, baseSpace, time, &spaceLocation);
    //CHECK_XRRESULT(res, "xrLocateSpace");

    SpaceLoc result = initLoc;
    if (!XR_UNQUALIFIED_SUCCESS(res))
        return result;

    const auto& pose = spaceLocation.pose;
    if ((spaceLocation.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0)
        result.pose.position = pose.position;

    if ((spaceLocation.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0)
        result.pose.orientation = pose.orientation;

    if ((velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0)
        result.linearVelocity = velocity.linearVelocity;

    if ((velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0)
        result.angularVelocity = velocity.angularVelocity;

    return result;
}
}
#endif
