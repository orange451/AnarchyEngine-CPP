# Cylinder Shape Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A PhysicsObject whose Shape is Cylinder collides in Box3D as a true cylinder (`b3_cylinderShape`), so it rolls as smoothly and straight as a sphere and rests on its side or cap without creeping.

**Architecture:** A new Box3D shape type. Its geometry, support functions, and contact manifolds live in our own C file, `cmake/box3d/cylinder.c`, compiled into the `box3d` target. Box3D's own files gain the plumbing (shape type, switches, contact dispatch, a GJK on support functions) through one patch, `cmake/box3d/cylinder.patch`, applied at configure time with a guard. Contact runs GJK on the cylinder's core (the cylinder shrunk by a small margin) and builds points from the cylinder's side line or cap facing the other shape, with feature IDs that do not change as it rolls. Queries and deep overlaps use a 32-sided stand-in hull kept with the shape. Last, `PhysicsWorld` makes one true cylinder for a Cylinder PhysicsObject.

**Tech Stack:** C11 (Box3D, MSVC 19.23 with `c11_shim.h`), C++17 engine, CMake, Catch2 v3 sandbox tests, git.

**Spec:** `docs/superpowers/specs/2026-10-12-cylinder-shape-design.md`

## Global Constraints

- Engine distances are "units"; never write "studs".
- Box3D stays pinned at `9f998c862d54c03a633ecea3831937385c78b532`.
- Edit Box3D's own files only in `build/_deps/box3d-src`. Right after every such edit, regenerate the patch from Git Bash, never by hand: `git -C build/_deps/box3d-src diff > cmake/box3d/cylinder.patch`. Then check it reverses cleanly: `git -C build/_deps/box3d-src apply --reverse --check ../../../cmake/box3d/cylinder.patch` prints nothing.
- New Box3D-side code goes in `cmake/box3d/cylinder.c`, in Box3D's style: tabs, braces on their own lines, `b3` prefixes, spaces inside parentheses.
- Constants:
  - side ends of the tread: feature IDs 0 and 1; cap rim points: IDs 2–17 (first cap) and 18–33 (second cap);
  - cap threshold: cos 10° = `0.98480775f`; line thresholds: sin 10° = `0.17364818f`, parallel `0.998f`;
  - face tolerance: hull `0.998f`, triangle `0.2f` (as Box3D's own capsule pairs);
  - core margin: `min(B3_SPECULATIVE_DISTANCE, 0.25 * min(radius, halfHeight))`;
  - stand-in hull: 32 sides (`B3_CYLINDER_HULL_SIDES`); cap points: 16 (`B3_CYLINDER_CAP_POINTS`).
- Build and test (Git Bash, repo root):
  - Build: `cmake --build build --config Release --target sandbox > build/cyl.log 2>&1; grep -E " error " build/cyl.log`.
  - Run: `./build/Release/sandbox.exe "[cylinder]"`.
  - Never run a background test while building: a running `sandbox.exe` locks the link.
- Test names: `CY<n> sentence` with tag `[cylinder]` (Box3D-level), `P<n> sentence` with tags `[physics][cylinder]` (engine).
- Commits: one or more per task. Message style `Area: what changed, as a sentence`, ending with:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  ```

## Review Focus

1. **Shape remade over and over:** a Cylinder's Size, Shape, or Mass changed many times in a session, each remaking its Box3D shape. Its stand-in hull must be freed each time (CY9).
2. **Tiny cylinders:** Size 0.01 (`kMinSize`), or a Cylinder under a GameObject scaled to 0.001. Box3D asserts below its linear slop, so this must fall back, not crash (P58).
3. **Long thin pipes:** a cylinder of radius 0.1 and length 10 lying on a box rests still (CY22b).
4. **Seams:** a wheel crossing the seam between two boxes, or two Brushes, rolls over it without a jump (CY22).
5. **Anchored cylinders as supports:** a box resting on two anchored logs stays put, with the cylinder as the static shape of the pair (CY22c).

---

### Task 1: Patch pipeline and the cylinder's geometry

**Files:**
- Create: `cmake/box3d/cylinder.c`, `cmake/box3d/cylinder.patch` (generated), `.gitattributes`, `sandbox/cylinder_shape_tests.cpp`
- Modify: `CMakeLists.txt` (after `set(BOX3D_SOURCE_DIR ...)`, about line 480; the sandbox source list, about line 1082; after `target_link_libraries(sandbox PRIVATE flecs)`, about line 1111), `cmake/box3d/CMakeLists.txt` (`add_library` line)
- Patch: `build/_deps/box3d-src/include/box3d/types.h`, `build/_deps/box3d-src/include/box3d/collision.h`

**Interfaces:**
- Produces:
  - `typedef struct b3Cylinder { b3Vec3 center1; b3Vec3 center2; float radius; } b3Cylinder;` in `types.h`;
  - `#define B3_CYLINDER_HULL_SIDES 32`;
  - `b3MassData b3ComputeCylinderMass( const b3Cylinder* shape, float density );`;
  - `b3AABB b3ComputeCylinderAABB( const b3Cylinder* shape, b3Transform transform );`;
  - `b3CastOutput b3RayCastCylinder( const b3Cylinder* shape, const b3RayCastInput* input );`;
  - `b3HullData* b3MakeCylinderHull( const b3Cylinder* shape );` (caller destroys with `b3DestroyHull`).
- Internal to `cylinder.c`, used by later tasks: `typedef struct b3CylinderFrame { b3Vec3 center; b3Vec3 axis; float halfHeight; float radius; }` and `static b3CylinderFrame b3MakeCylinderFrame( const b3Cylinder* shape )`.

- [ ] **Step 1: Keep the patch byte-exact in git**

Create `.gitattributes`:

```
# git apply needs the Box3D patch exactly as git diff wrote it: no CRLF.
cmake/box3d/cylinder.patch -text
```

- [ ] **Step 2: Apply the patch at configure time, guarded**

In `CMakeLists.txt`, right after `set(BOX3D_SOURCE_DIR "${box3d_SOURCE_DIR}")`, insert:

```cmake
# The cylinder shape (docs/superpowers/specs/2026-10-12-cylinder-shape-design.md)
# patches Box3D's own files; cmake/box3d/cylinder.c is ours. The patch goes in
# once: when it already reverses cleanly it is in place and left alone. One that
# does not apply means the pin moved, and configuring stops rather than build
# Box3D without it.
set(BOX3D_PATCH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/box3d/cylinder.patch")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${BOX3D_PATCH}")
find_package(Git REQUIRED)
execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${BOX3D_PATCH}"
    WORKING_DIRECTORY "${BOX3D_SOURCE_DIR}"
    RESULT_VARIABLE box3d_patch_in_place
    OUTPUT_QUIET ERROR_QUIET)
if(NOT box3d_patch_in_place EQUAL 0)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply "${BOX3D_PATCH}"
        WORKING_DIRECTORY "${BOX3D_SOURCE_DIR}"
        RESULT_VARIABLE box3d_patch_failed
        ERROR_VARIABLE box3d_patch_error)
    if(NOT box3d_patch_failed EQUAL 0)
        message(FATAL_ERROR "cmake/box3d/cylinder.patch does not apply to Box3D in ${BOX3D_SOURCE_DIR}: "
                            "${box3d_patch_error}")
    endif()
    message(STATUS "Box3D: applied cmake/box3d/cylinder.patch")
endif()
```

In `cmake/box3d/CMakeLists.txt`, change the `add_library` line to build our file too:

```cmake
# cylinder.c is the engine's own: a true cylinder shape (cylinder.patch wires it in).
add_library(box3d STATIC ${BOX3D_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/cylinder.c")
```

In `CMakeLists.txt`, add `sandbox/cylinder_shape_tests.cpp` to the `add_executable(sandbox ...)` list, after `sandbox/archivable_tests.cpp`. After `target_link_libraries(sandbox PRIVATE flecs)` add:

```cmake
# The cylinder tests call Box3D directly (cmake/box3d/cylinder.c).
target_link_libraries(sandbox PRIVATE box3d)
```

- [ ] **Step 3: Declare the cylinder in Box3D's headers**

In `build/_deps/box3d-src/include/box3d/types.h`, right after the `b3Capsule` struct (`} b3Capsule;`) and its `/**@}*/ // capsule` line, insert:

```c
/**
 * @defgroup cylinder Cylinder
 * @brief Cylinder primitive (Anarchy Engine)
 * @{
 */

/// A solid cylinder: two flat caps joined by a round side.
typedef struct b3Cylinder
{
	/// Local center of the first cap
	b3Vec3 center1;

	/// Local center of the second cap
	b3Vec3 center2;

	/// The radius of the caps
	float radius;
} b3Cylinder;

/**@}*/ // cylinder
```

In `build/_deps/box3d-src/include/box3d/collision.h`, right before the comment `/// Collide two spheres.`, insert:

```c
/// The sides of a cylinder's stand-in hull (b3MakeCylinderHull). Anarchy Engine.
#define B3_CYLINDER_HULL_SIDES 32

/// Compute mass properties of a cylinder. Anarchy Engine.
B3_API b3MassData b3ComputeCylinderMass( const b3Cylinder* shape, float density );

/// Compute the bounding box of a transformed cylinder. Anarchy Engine.
B3_API b3AABB b3ComputeCylinderAABB( const b3Cylinder* shape, b3Transform transform );

/// Ray cast versus cylinder in local space. A ray starting inside reports a hit at its origin with zero
/// fraction and zero normal, as b3RayCastCapsule does. Anarchy Engine.
B3_API b3CastOutput b3RayCastCylinder( const b3Cylinder* shape, const b3RayCastInput* input );

/// The cylinder as a hull of B3_CYLINDER_HULL_SIDES sides, its points on the true surface: the stand-in a
/// cylinder shape's queries use. NULL if Box3D builds none. Destroy it with b3DestroyHull. Anarchy Engine.
B3_API b3HullData* b3MakeCylinderHull( const b3Cylinder* shape );
```

Regenerate the patch (Global Constraints) and check it reverses cleanly.

- [ ] **Step 4: Write the failing tests**

Create `sandbox/cylinder_shape_tests.cpp`:

```cpp
// The cylinder shape in Box3D (cmake/box3d/cylinder.c): its geometry and its
// contact manifolds, checked against analytic answers.

#include "box3d/box3d.h"
#include "box3d/collision.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

namespace {

constexpr float kPi = 3.14159265358979f;

bool near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

b3Vec3 v3(float x, float y, float z) { return b3Vec3{x, y, z}; }

// Radius radius, standing along Y from -half to +half.
b3Cylinder upright(float radius, float half) { return b3Cylinder{v3(0.f, -half, 0.f), v3(0.f, half, 0.f), radius}; }

}  // namespace

TEST_CASE("CY1 a cylinder's mass, centre and inertia match the formulas, along any axis", "[cylinder]") {
    const float density = 2.f;
    const b3Cylinder standing = upright(0.5f, 1.f);
    const b3MassData m = b3ComputeCylinderMass(&standing, density);
    const float mass = density * kPi * 0.25f * 2.f;
    const float axial = 0.5f * mass * 0.25f;
    const float across = mass * (3.f * 0.25f + 4.f) / 12.f;
    REQUIRE(near(m.mass, mass, 1e-5f * mass));
    REQUIRE(near(m.center.x, 0.f, 1e-6f));
    REQUIRE(near(m.center.y, 0.f, 1e-6f));
    REQUIRE(near(m.inertia.cy.y, axial, 1e-5f * axial));
    REQUIRE(near(m.inertia.cx.x, across, 1e-5f * across));
    REQUIRE(near(m.inertia.cz.z, across, 1e-5f * across));
    REQUIRE(near(m.inertia.cx.y, 0.f, 1e-6f));

    // Laid along X and moved: the centre follows, the axial inertia moves to X.
    const b3Cylinder lying{v3(-1.f, 3.f, 2.f), v3(1.f, 3.f, 2.f), 0.5f};
    const b3MassData l = b3ComputeCylinderMass(&lying, density);
    REQUIRE(near(l.mass, mass, 1e-5f * mass));
    REQUIRE(near(l.center.y, 3.f, 1e-6f));
    REQUIRE(near(l.center.z, 2.f, 1e-6f));
    REQUIRE(near(l.inertia.cx.x, axial, 1e-5f * axial));
    REQUIRE(near(l.inertia.cy.y, across, 1e-5f * across));

    // Turned 45 degrees in XY: the standing tensor turned, so its trace holds
    // and its XY term is (axial - across) / 2.
    const float s = std::sqrt(0.5f);
    const b3Cylinder tilted{v3(-s, -s, 0.f), v3(s, s, 0.f), 0.5f};
    const b3MassData t = b3ComputeCylinderMass(&tilted, density);
    REQUIRE(near(t.inertia.cx.x + t.inertia.cy.y + t.inertia.cz.z, axial + 2.f * across, 1e-4f));
    REQUIRE(near(t.inertia.cx.y, 0.5f * (axial - across), 1e-4f));
}

TEST_CASE("CY2 a cylinder's AABB is exact, however it is turned", "[cylinder]") {
    const b3Cylinder c{v3(0.f, -0.75f, 0.f), v3(0.f, 0.75f, 0.f), 0.4f};
    for (const float degrees : {0.f, 30.f, 90.f}) {
        INFO(degrees);
        const b3Transform xf{v3(1.f, 2.f, 3.f), b3MakeQuatFromAxisAngle(v3(0.f, 0.f, 1.f), degrees * kPi / 180.f)};
        const b3AABB box = b3ComputeCylinderAABB(&c, xf);
        // The extremes of 3600 rim points on each cap.
        b3Vec3 lo{1e9f, 1e9f, 1e9f};
        b3Vec3 hi{-1e9f, -1e9f, -1e9f};
        for (int cap = 0; cap < 2; ++cap) {
            for (int i = 0; i < 3600; ++i) {
                const float a = 2.f * kPi * static_cast<float>(i) / 3600.f;
                const b3Vec3 local{0.4f * std::cos(a), cap == 0 ? -0.75f : 0.75f, 0.4f * std::sin(a)};
                const b3Vec3 p = b3TransformPoint(xf, local);
                lo = b3Min(lo, p);
                hi = b3Max(hi, p);
            }
        }
        REQUIRE(near(box.lowerBound.x, lo.x, 1e-4f));
        REQUIRE(near(box.lowerBound.y, lo.y, 1e-4f));
        REQUIRE(near(box.lowerBound.z, lo.z, 1e-4f));
        REQUIRE(near(box.upperBound.x, hi.x, 1e-4f));
        REQUIRE(near(box.upperBound.y, hi.y, 1e-4f));
        REQUIRE(near(box.upperBound.z, hi.z, 1e-4f));
    }
}

TEST_CASE("CY3 a ray hits a cylinder's side, cap and rim exactly, and misses past it", "[cylinder]") {
    const b3Cylinder c = upright(0.5f, 1.f);
    auto cast = [&](b3Vec3 origin, b3Vec3 translation) {
        const b3RayCastInput input{origin, translation, 1.f};
        return b3RayCastCylinder(&c, &input);
    };
    // The side, head on from +X.
    const b3CastOutput side = cast(v3(3.f, 0.2f, 0.f), v3(-4.f, 0.f, 0.f));
    REQUIRE(side.hit);
    REQUIRE(near(side.fraction, 2.5f / 4.f, 1e-6f));
    REQUIRE(near(side.point.x, 0.5f, 1e-5f));
    REQUIRE(near(side.normal.x, 1.f, 1e-5f));
    // The top cap, from above.
    const b3CastOutput cap = cast(v3(0.1f, 3.f, 0.2f), v3(0.f, -4.f, 0.f));
    REQUIRE(cap.hit);
    REQUIRE(near(cap.fraction, 0.5f, 1e-6f));
    REQUIRE(near(cap.normal.y, 1.f, 1e-6f));
    // Up the axis from below: the bottom cap.
    const b3CastOutput axis = cast(v3(0.f, -3.f, 0.f), v3(0.f, 4.f, 0.f));
    REQUIRE(axis.hit);
    REQUIRE(near(axis.point.y, -1.f, 1e-5f));
    REQUIRE(near(axis.normal.y, -1.f, 1e-6f));
    // Just inside the rim, straight down: the cap's edge.
    const b3CastOutput rim = cast(v3(0.49f, 2.f, 0.f), v3(0.f, -2.f, 0.f));
    REQUIRE(rim.hit);
    REQUIRE(near(rim.point.y, 1.f, 1e-5f));
    // Just outside the rim, straight down: past the side, never touching.
    REQUIRE_FALSE(cast(v3(0.51f, 2.f, 0.f), v3(0.f, -4.f, 0.f)).hit);
    // Level with the top but just over it.
    REQUIRE_FALSE(cast(v3(3.f, 1.01f, 0.f), v3(-6.f, 0.f, 0.f)).hit);
    // Short of the side.
    REQUIRE_FALSE(cast(v3(3.f, 0.f, 0.f), v3(-2.f, 0.f, 0.f)).hit);
    // Starting inside: a hit at the origin, as a capsule reports one.
    const b3CastOutput inside = cast(v3(0.f, 0.f, 0.f), v3(1.f, 0.f, 0.f));
    REQUIRE(inside.hit);
    REQUIRE(inside.fraction == 0.f);
}

TEST_CASE("CY4 a cylinder's stand-in hull is 32 sides on its true surface, along its axis", "[cylinder]") {
    const b3Cylinder lying{v3(-1.f, 0.f, 0.f), v3(1.f, 0.f, 0.f), 0.5f};
    b3HullData* hull = b3MakeCylinderHull(&lying);
    REQUIRE(hull != nullptr);
    REQUIRE(hull->vertexCount == 2 * B3_CYLINDER_HULL_SIDES);
    REQUIRE(hull->faceCount == B3_CYLINDER_HULL_SIDES + 2);
    const b3Vec3* points = b3GetHullPoints(hull);
    for (int i = 0; i < hull->vertexCount; ++i) {
        REQUIRE(near(std::fabs(points[i].x), 1.f, 1e-5f));
        REQUIRE(near(std::sqrt(points[i].y * points[i].y + points[i].z * points[i].z), 0.5f, 1e-5f));
    }
    b3DestroyHull(hull);
}
```

- [ ] **Step 5: Run the tests to see them fail**

Create `cmake/box3d/cylinder.c` empty except for its header comment (so the build has the file):

```c
// SPDX-FileCopyrightText: 2026 Anarchy Engine
// SPDX-License-Identifier: MIT
```

Run: the build command (Global Constraints). Configure runs first, because CMakeLists changed.
Expected:
- the configure output shows `Box3D: applied cmake/box3d/cylinder.patch` only if the source was clean (it already holds your edits, so usually nothing prints);
- the link FAILS with unresolved externals `b3ComputeCylinderMass`, `b3ComputeCylinderAABB`, `b3RayCastCylinder`, `b3MakeCylinderHull`.

- [ ] **Step 6: Write the geometry**

Replace `cmake/box3d/cylinder.c` with:

```c
// SPDX-FileCopyrightText: 2026 Anarchy Engine
// SPDX-License-Identifier: MIT

// A true cylinder for Box3D: the engine's own addition, built into the box3d
// target beside Box3D's sources. Box3D's files reach it through
// cylinder.patch. See docs/superpowers/specs/2026-10-12-cylinder-shape-design.md.

#include "manifold.h"
#include "math_internal.h"
#include "shape.h"

#include "box3d/collision.h"
#include "box3d/constants.h"

#include <float.h>
#include <math.h>
#include <stddef.h>

// The cylinder about its middle: a unit axis from center1 to center2.
typedef struct b3CylinderFrame
{
	b3Vec3 center;
	b3Vec3 axis;
	float halfHeight;
	float radius;
} b3CylinderFrame;

static b3CylinderFrame b3MakeCylinderFrame( const b3Cylinder* shape )
{
	b3CylinderFrame frame;
	float length;
	frame.axis = b3GetLengthAndNormalize( &length, b3Sub( shape->center2, shape->center1 ) );
	if ( length == 0.0f )
	{
		frame.axis = b3Vec3_axisY;
	}
	frame.center = b3Lerp( shape->center1, shape->center2, 0.5f );
	frame.halfHeight = 0.5f * length;
	frame.radius = shape->radius;
	return frame;
}

b3MassData b3ComputeCylinderMass( const b3Cylinder* shape, float density )
{
	b3CylinderFrame frame = b3MakeCylinderFrame( shape );
	float radiusSqr = frame.radius * frame.radius;
	float height = 2.0f * frame.halfHeight;
	float mass = density * B3_PI * radiusSqr * height;

	// About the axis, and about any line across it through the middle.
	float axial = 0.5f * mass * radiusSqr;
	float across = mass * ( 3.0f * radiusSqr + height * height ) / 12.0f;

	// across * I + (axial - across) * axis * axis^T, about the centre of mass.
	b3Vec3 u = frame.axis;
	float k = axial - across;
	b3MassData out;
	out.mass = mass;
	out.center = frame.center;
	out.inertia.cx = ( b3Vec3 ){ across + k * u.x * u.x, k * u.y * u.x, k * u.z * u.x };
	out.inertia.cy = ( b3Vec3 ){ k * u.x * u.y, across + k * u.y * u.y, k * u.z * u.y };
	out.inertia.cz = ( b3Vec3 ){ k * u.x * u.z, k * u.y * u.z, across + k * u.z * u.z };
	return out;
}

b3AABB b3ComputeCylinderAABB( const b3Cylinder* shape, b3Transform transform )
{
	b3Vec3 c1 = b3TransformPoint( transform, shape->center1 );
	b3Vec3 c2 = b3TransformPoint( transform, shape->center2 );
	b3Vec3 axis = b3RotateVector( transform.q, b3MakeCylinderFrame( shape ).axis );
	float r = shape->radius;

	// A disc of radius r across axis reaches r * sqrt(1 - axis_i^2) along world axis i.
	b3Vec3 reach = { r * sqrtf( b3MaxFloat( 0.0f, 1.0f - axis.x * axis.x ) ),
					 r * sqrtf( b3MaxFloat( 0.0f, 1.0f - axis.y * axis.y ) ),
					 r * sqrtf( b3MaxFloat( 0.0f, 1.0f - axis.z * axis.z ) ) };

	b3AABB box;
	box.lowerBound = b3Sub( b3Min( c1, c2 ), reach );
	box.upperBound = b3Add( b3Max( c1, c2 ), reach );
	return box;
}

b3CastOutput b3RayCastCylinder( const b3Cylinder* shape, const b3RayCastInput* input )
{
	B3_ASSERT( b3IsValidRay( input ) );

	b3CastOutput output = { 0 };
	b3CylinderFrame frame = b3MakeCylinderFrame( shape );
	float r = frame.radius;
	float h = frame.halfHeight;

	// The origin and the translation, along the axis and across it.
	b3Vec3 s = b3Sub( input->origin, frame.center );
	b3Vec3 d = input->translation;
	float sAlong = b3Dot( s, frame.axis );
	float dAlong = b3Dot( d, frame.axis );
	b3Vec3 sAcross = b3MulSub( s, sAlong, frame.axis );
	b3Vec3 dAcross = b3MulSub( d, dAlong, frame.axis );
	float sAcrossSqr = b3Dot( sAcross, sAcross );

	if ( sAcrossSqr <= r * r && b3AbsFloat( sAlong ) <= h )
	{
		// Starts inside: a hit at the origin, as b3RayCastCapsule reports it.
		output.hit = true;
		output.point = input->origin;
		return output;
	}

	float best = input->maxFraction;
	bool hit = false;
	b3Vec3 normal = b3Vec3_zero;

	// The round side: |sAcross + t * dAcross| = r, entering (the smaller root),
	// only from outside the infinite tube.
	float a = b3Dot( dAcross, dAcross );
	float c = sAcrossSqr - r * r;
	if ( a > 1000.0f * FLT_MIN && c > 0.0f )
	{
		float b = b3Dot( sAcross, dAcross );
		float discriminant = b * b - a * c;
		if ( discriminant >= 0.0f )
		{
			float t = ( -b - sqrtf( discriminant ) ) / a;
			if ( 0.0f <= t && t <= best && b3AbsFloat( sAlong + t * dAlong ) <= h )
			{
				best = t;
				hit = true;
				normal = b3Normalize( b3MulAdd( sAcross, t, dAcross ) );
			}
		}
	}

	// The caps: the plane at +h or -h, within the radius, entered from beyond it.
	if ( b3AbsFloat( dAlong ) > 1000.0f * FLT_MIN )
	{
		for ( int side = -1; side <= 1; side += 2 )
		{
			float sign = (float)side;
			if ( sign * sAlong <= h || sign * dAlong >= 0.0f )
			{
				continue;
			}

			float t = ( sign * h - sAlong ) / dAlong;
			if ( t < 0.0f || t > best )
			{
				continue;
			}

			b3Vec3 across = b3MulAdd( sAcross, t, dAcross );
			if ( b3Dot( across, across ) <= r * r )
			{
				best = t;
				hit = true;
				normal = b3MulSV( sign, frame.axis );
			}
		}
	}

	if ( hit )
	{
		output.hit = true;
		output.fraction = best;
		output.normal = normal;
		output.point = b3MulAdd( input->origin, best, d );
	}
	return output;
}

b3HullData* b3MakeCylinderHull( const b3Cylinder* shape )
{
	b3CylinderFrame frame = b3MakeCylinderFrame( shape );
	if ( frame.radius <= 0.0f || frame.halfHeight <= 0.0f )
	{
		return NULL;
	}

	// Box3D's tessellated cylinder stands along Y from yOffset: centred, then
	// turned onto the axis and moved to the middle.
	b3HullData* upright =
		b3CreateCylinder( 2.0f * frame.halfHeight, frame.radius, -frame.halfHeight, B3_CYLINDER_HULL_SIDES );
	if ( upright == NULL )
	{
		return NULL;
	}

	b3Transform place = { frame.center, b3ComputeQuatBetweenUnitVectors( b3Vec3_axisY, frame.axis ) };
	b3HullData* placed = b3CloneAndTransformHull( upright, place, b3Vec3_one );
	b3DestroyHull( upright );
	return placed;
}
```

- [ ] **Step 7: Run the tests to see them pass**

Run: build, then `./build/Release/sandbox.exe "[cylinder]"`.
Expected: `All tests passed (… assertions in 4 test cases)`.

- [ ] **Step 8: Configuring twice patches once**

Run: `cmake -S . -B build > build/cyl-config.log 2>&1; cmake -S . -B build >> build/cyl-config.log 2>&1; grep -c "applied cmake/box3d/cylinder.patch" build/cyl-config.log; git -C build/_deps/box3d-src diff --stat | tail -1`
Expected: `0` (the source already holds the patch, so neither run applies it), and the diff stat shows the same 2 files as the patch.

Then on a scratch copy, prove a fresh source gets patched once:
`rm -rf build/box3d-fresh && git -C build/_deps/box3d-src worktree add ../../box3d-fresh HEAD >/dev/null 2>&1 || cp -r build/_deps/box3d-src build/box3d-fresh && git -C build/box3d-fresh checkout -- . && (cd build/box3d-fresh && git apply --reverse --check ../../cmake/box3d/cylinder.patch; echo "reverse on clean: $?"; git apply ../../cmake/box3d/cylinder.patch; echo "apply: $?"; git apply --reverse --check ../../cmake/box3d/cylinder.patch; echo "reverse after: $?")`
Expected: `reverse on clean: 1`, `apply: 0`, `reverse after: 0`. Then `rm -rf build/box3d-fresh` (and `git -C build/_deps/box3d-src worktree prune`).

- [ ] **Step 9: Commit**

```bash
git add .gitattributes CMakeLists.txt cmake/box3d/CMakeLists.txt cmake/box3d/cylinder.c cmake/box3d/cylinder.patch sandbox/cylinder_shape_tests.cpp
git commit -m "Box3D: a cylinder's mass, bounds, ray cast, and stand-in hull, through a patch applied at configure

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: The cylinder as a Box3D shape type

**Files:**
- Create: `sandbox/cylinder_world_tests.cpp`
- Modify: `CMakeLists.txt` (sandbox list: add `sandbox/cylinder_world_tests.cpp` after `sandbox/cylinder_shape_tests.cpp`)
- Patch:
  - `include/box3d/types.h` (shape type enum);
  - `include/box3d/box3d.h` (create, get, set);
  - `src/shape.h` (union), `src/shape.c` (every type switch);
  - `src/physics_world.c` (debug shape), `src/sensor.c` (sensor overlap), `src/world_snapshot.c`;
  - `src/recording.h`, `src/recording.c`, `src/recording_replay.c`, `src/recording_ops.inl`.

**Interfaces:**
- Consumes (Task 1): `b3Cylinder`, `b3ComputeCylinderMass`, `b3ComputeCylinderAABB`, `b3RayCastCylinder`, `b3MakeCylinderHull`.
- Produces:
  - enum value `b3_cylinderShape`;
  - `b3ShapeId b3CreateCylinderShape( b3BodyId bodyId, const b3ShapeDef* def, const b3Cylinder* cylinder );`;
  - `b3Cylinder b3Shape_GetCylinder( b3ShapeId shapeId );`, `void b3Shape_SetCylinder( b3ShapeId shapeId, const b3Cylinder* cylinder );`;
  - `b3Shape` fields `shape->cylinder` (a `b3Cylinder`) and `shape->cylinderHull` (a `const b3HullData*`, in the world's hull database).

- [ ] **Step 1: Write the failing tests**

Create `sandbox/cylinder_world_tests.cpp`:

```cpp
// The cylinder shape in a Box3D world: what its shape answers, and how it
// rolls, rests, and stacks.

#include "box3d/box3d.h"
#include "box3d/collision.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <vector>

namespace {

constexpr float kPi = 3.14159265358979f;
// The engine's step: 240 Hz, one substep (PhysicsWorld::simulate).
constexpr float kStep = 1.f / 240.f;

bool near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

b3Vec3 v3(float x, float y, float z) { return b3Vec3{x, y, z}; }

struct World {
    b3WorldId id;

    World() {
        b3WorldDef def = b3DefaultWorldDef();
        def.gravity = v3(0.f, -10.f, 0.f);
        id = b3CreateWorld(&def);
    }
    ~World() { b3DestroyWorld(id); }
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    b3BodyId body(b3BodyType type, b3Vec3 at, b3Quat rotation = b3Quat_identity) {
        b3BodyDef def = b3DefaultBodyDef();
        def.type = type;
        def.position = at;
        def.rotation = rotation;
        return b3CreateBody(id, &def);
    }

    // A static box, half extents half, centred at center.
    b3BodyId box(b3Vec3 center, b3Vec3 half, float friction = 0.6f, b3Quat rotation = b3Quat_identity) {
        const b3BodyId ground = body(b3_staticBody, center, rotation);
        const b3BoxHull hull = b3MakeBoxHull(half.x, half.y, half.z);
        b3ShapeDef def = b3DefaultShapeDef();
        def.baseMaterial.friction = friction;
        b3CreateHullShape(ground, &def, &hull.base);
        return ground;
    }

    // A wide static floor, its top at y = 0.
    b3BodyId floor(float friction = 0.6f) { return box(v3(0.f, -0.5f, 0.f), v3(50.f, 0.5f, 50.f), friction); }

    // A cylinder of radius r and half height h along its body's local Y.
    b3BodyId cylinder(b3BodyType type, b3Vec3 at, b3Quat rotation, float r, float h, float friction = 0.6f) {
        const b3BodyId b = body(type, at, rotation);
        const b3Cylinder c{v3(0.f, -h, 0.f), v3(0.f, h, 0.f), r};
        b3ShapeDef def = b3DefaultShapeDef();
        def.baseMaterial.friction = friction;
        b3CreateCylinderShape(b, &def, &c);
        return b;
    }

    void step(int count = 1) {
        for (int i = 0; i < count; ++i) {
            b3World_Step(id, kStep, 1);
        }
    }
    void seconds(float time) { step(static_cast<int>(time / kStep + 0.5f)); }
};

// Lying on its side: local Y (its axis) turned onto world Z.
b3Quat on_side() { return b3MakeQuatFromAxisAngle(v3(1.f, 0.f, 0.f), 0.5f * kPi); }

// Its axis in world space.
b3Vec3 axis_of(b3BodyId body) { return b3RotateVector(b3Body_GetRotation(body), v3(0.f, 1.f, 0.f)); }

b3Vec3 position_of(b3BodyId body) { return b3Body_GetPosition(body); }

}  // namespace

TEST_CASE("CY5 a body with a cylinder shape weighs it, and the shape reads back", "[cylinder]") {
    World world;
    const b3BodyId body = world.body(b3_dynamicBody, v3(0.f, 5.f, 0.f));
    const b3Cylinder c{v3(0.f, -1.f, 0.f), v3(0.f, 1.f, 0.f), 0.5f};
    b3ShapeDef def = b3DefaultShapeDef();
    def.density = 2.f;
    const b3ShapeId shape = b3CreateCylinderShape(body, &def, &c);
    REQUIRE(b3Shape_IsValid(shape));
    REQUIRE(b3Shape_GetType(shape) == b3_cylinderShape);
    const b3Cylinder back = b3Shape_GetCylinder(shape);
    REQUIRE(back.radius == 0.5f);
    REQUIRE(back.center2.y == 1.f);
    REQUIRE(near(b3Body_GetMass(body), 2.f * kPi * 0.25f * 2.f, 1e-4f));
    // Made longer in place: the body weighs the new length once it applies it.
    const b3Cylinder longer{v3(0.f, -2.f, 0.f), v3(0.f, 2.f, 0.f), 0.5f};
    b3Shape_SetCylinder(shape, &longer);
    b3Body_ApplyMassFromShapes(body);
    REQUIRE(near(b3Body_GetMass(body), 2.f * kPi * 0.25f * 4.f, 1e-4f));
}

TEST_CASE("CY6 a world ray cast meets a cylinder's round side, not its stand-in's flats", "[cylinder]") {
    World world;
    // Lying along Z, radius 0.5, at the origin.
    world.cylinder(b3_staticBody, v3(0.f, 0.f, 0.f), on_side(), 0.5f, 1.f);
    // Straight down at x = 0.1: between two of the stand-in's corners.
    const b3RayResult hit =
        b3World_CastRayClosest(world.id, v3(0.1f, 2.f, 0.3f), v3(0.f, -4.f, 0.f), b3DefaultQueryFilter());
    REQUIRE(hit.hit);
    REQUIRE(near(hit.point.y, std::sqrt(0.25f - 0.01f), 1e-5f));
}

TEST_CASE("CY7 overlaps and shape casts find a cylinder through its stand-in hull", "[cylinder]") {
    World world;
    world.cylinder(b3_staticBody, v3(0.f, 0.f, 0.f), b3Quat_identity, 0.5f, 1.f);
    auto overlaps = [&](b3Vec3 at) {
        const b3Vec3 point = v3(0.f, 0.f, 0.f);
        const b3ShapeProxy proxy{&point, 1, 0.05f};
        int found = 0;
        b3World_OverlapShape(
            world.id, at, &proxy, b3DefaultQueryFilter(),
            [](b3ShapeId, void* context) {
                ++*static_cast<int*>(context);
                return true;
            },
            &found);
        return found;
    };
    REQUIRE(overlaps(v3(0.5f, 0.f, 0.f)) == 1);
    REQUIRE(overlaps(v3(0.6f, 0.f, 0.f)) == 0);

    // A small box swept at it from +X stops within 0.5 % of the radius of the side.
    const b3BoxHull small = b3MakeBoxHull(0.1f, 0.1f, 0.1f);
    const b3ShapeProxy proxy{b3GetHullPoints(&small.base), small.base.vertexCount, 0.f};
    float fraction = -1.f;
    b3World_CastShape(
        world.id, v3(3.f, 0.f, 0.f), &proxy, v3(-4.f, 0.f, 0.f), b3DefaultQueryFilter(),
        [](b3ShapeId, b3Pos, b3Vec3, float f, uint64_t, int, int, void* context) {
            *static_cast<float*>(context) = f;
            return f;
        },
        &fraction);
    REQUIRE(fraction > 0.f);
    const float face = 3.f - 0.1f - 4.f * fraction;
    REQUIRE(near(face, 0.5f, 0.0025f));
}

TEST_CASE("CY8 a sensor cylinder notices a body passing through it", "[cylinder]") {
    World world;
    const b3BodyId holder = world.body(b3_staticBody, v3(0.f, 0.f, 0.f));
    const b3Cylinder c{v3(0.f, -1.f, 0.f), v3(0.f, 1.f, 0.f), 0.5f};
    b3ShapeDef sensor = b3DefaultShapeDef();
    sensor.isSensor = true;
    sensor.enableSensorEvents = true;
    b3CreateCylinderShape(holder, &sensor, &c);
    const b3BodyId ball = world.body(b3_dynamicBody, v3(0.f, 3.f, 0.f));
    b3ShapeDef def = b3DefaultShapeDef();
    def.enableSensorEvents = true;
    const b3Sphere sphere{v3(0.f, 0.f, 0.f), 0.1f};
    b3CreateSphereShape(ball, &def, &sphere);
    bool began = false;
    for (int i = 0; i < 480 && !began; ++i) {
        world.step();
        began = b3World_GetSensorEvents(world.id).beginCount > 0;
    }
    REQUIRE(began);
}

TEST_CASE("CY9 a cylinder shape made, reshaped, and destroyed again and again frees its stand-in hull", "[cylinder]") {
    World world;
    const b3BodyId body = world.body(b3_dynamicBody, v3(0.f, 5.f, 0.f));
    const b3Cylinder c{v3(0.f, -1.f, 0.f), v3(0.f, 1.f, 0.f), 0.5f};
    const b3Cylinder wider{v3(0.f, -1.f, 0.f), v3(0.f, 1.f, 0.f), 0.75f};
    b3ShapeDef def = b3DefaultShapeDef();
    // Once first, so the world's own arrays have grown.
    b3DestroyShape(b3CreateCylinderShape(body, &def, &c), true);
    const int64_t before = b3World_GetCounters(world.id).byteCount;
    for (int i = 0; i < 200; ++i) {
        const b3ShapeId shape = b3CreateCylinderShape(body, &def, &c);
        b3Shape_SetCylinder(shape, &wider);
        b3DestroyShape(shape, true);
    }
    REQUIRE(b3World_GetCounters(world.id).byteCount == before);
}
```

Add `sandbox/cylinder_world_tests.cpp` to the sandbox list in `CMakeLists.txt`.

- [ ] **Step 2: Run them to see them fail**

Run: build.
Expected: FAIL to compile: `b3_cylinderShape`, `b3CreateCylinderShape`, `b3Shape_GetCylinder`, `b3Shape_SetCylinder` undeclared.

- [ ] **Step 3: The shape type and its fields**

In `include/box3d/types.h`, in `typedef enum b3ShapeType`, right after `b3_sphereShape,` (before `b3_shapeTypeCount`):

```c

	/// A solid cylinder (Anarchy Engine)
	b3_cylinderShape,
```

In `src/shape.h`, in the `b3Shape` union, after `const b3CompoundData* compound;`:

```c
		// A cylinder, and the stand-in hull its queries use (b3MakeCylinderHull),
		// in the world's hull database.
		struct
		{
			b3Cylinder cylinder;
			const b3HullData* cylinderHull;
		};
```

In `include/box3d/box3d.h`, right after the declaration of `b3CreateCapsuleShape`:

```c
/// Create a cylinder shape and attach it to a body. Its radius and its length must each be at least
/// B3_LINEAR_SLOP. Contacts are computed immediately. Anarchy Engine.
B3_API b3ShapeId b3CreateCylinderShape( b3BodyId bodyId, const b3ShapeDef* def, const b3Cylinder* cylinder );
```

Right after the declaration of `b3Shape_GetCapsule`:

```c
/// Get a copy of the shape's cylinder. Asserts the type is correct. Anarchy Engine.
B3_API b3Cylinder b3Shape_GetCylinder( b3ShapeId shapeId );
```

Right after the declaration of `b3Shape_SetCapsule`:

```c
/// Allows you to change a shape to be a cylinder or update the current cylinder. Anarchy Engine.
B3_API void b3Shape_SetCylinder( b3ShapeId shapeId, const b3Cylinder* cylinder );
```

- [ ] **Step 4: Every type switch in `src/shape.c`**

Each block goes into the named function's `switch ( shape->type )` (or `switch ( shapeType )`), beside its `b3_capsuleShape` case:

```c
// b3ComputeShapeMargin
		case b3_cylinderShape:
		{
			// The middle to a rim.
			float halfLength = 0.5f * b3Distance( shape->cylinder.center2, shape->cylinder.center1 );
			float radius = shape->cylinder.radius;
			margin = sqrtf( halfLength * halfLength + radius * radius );
		}
		break;

// b3CreateShape (the geometry switch on shapeType)
		case b3_cylinderShape:
		{
			shape->cylinder = *(b3Cylinder*)geometry;
			b3HullData* standIn = b3MakeCylinderHull( &shape->cylinder );
			if ( standIn == NULL )
			{
				b3FreeId( &world->shapeIdPool, shapeId );
				shape->id = B3_NULL_INDEX;
				return NULL;
			}
			shape->cylinderHull = b3AddOwnedHullToDatabase( world, standIn );
		}
		break;

// b3ComputeShapeAABB
		case b3_cylinderShape:
			return b3ComputeCylinderAABB( &shape->cylinder, transform );

// b3ComputeSweptShapeAABB: the stand-in's, grown by how far inside the true surface it lies
		case b3_cylinderShape:
		{
			b3AABB box = b3ComputeSweptHullAABB( shape->cylinderHull, xf1, xf2 );
			return b3AABB_Inflate( box, 0.005f * shape->cylinder.radius );
		}

// b3GetShapeCentroid
		case b3_cylinderShape:
			return b3Lerp( shape->cylinder.center1, shape->cylinder.center2, 0.5f );

// b3GetShapeArea: its whole surface
		case b3_cylinderShape:
		{
			float radius = shape->cylinder.radius;
			float length = b3Distance( shape->cylinder.center1, shape->cylinder.center2 );
			return 2.0f * B3_PI * radius * ( length + radius );
		}

// b3GetShapeProjectedArea: the caps' disc foreshortened, plus the side's rectangle
		case b3_cylinderShape:
		{
			float radius = shape->cylinder.radius;
			b3Vec3 axis = b3Sub( shape->cylinder.center2, shape->cylinder.center1 );
			float length = b3Length( axis );
			b3Vec3 unit = length > 0.0f ? b3MulSV( 1.0f / length, axis ) : b3Vec3_axisY;
			float capArea = B3_PI * radius * radius * b3AbsFloat( b3Dot( unit, planeNormal ) );
			float sideArea = 2.0f * radius * length * b3Length( b3Cross( unit, planeNormal ) );
			return capArea + sideArea;
		}

// b3ComputeShapeMass
		case b3_cylinderShape:
			return b3ComputeCylinderMass( &shape->cylinder, shape->density );

// b3ComputeShapeExtent
		case b3_cylinderShape:
			extent = b3ComputeHullExtent( shape->cylinderHull, localCenter );
			break;

// b3RayCastShape: exact
		case b3_cylinderShape:
			output = b3RayCastCylinder( &shape->cylinder, &localInput );
			break;

// b3ShapeCastShape: the stand-in
		case b3_cylinderShape:
			output = b3ShapeCastHull( shape->cylinderHull, &localInput );
			break;

// b3OverlapShape: the stand-in
		case b3_cylinderShape:
			return b3OverlapHull( shape->cylinderHull, transform, proxy );

// b3CollideMover: the stand-in
		case b3_cylinderShape:
			planeCount = b3CollideMoverAndHull( planes, shape->cylinderHull, &localMover );
			break;

// b3DestroyShapeAllocationForShapeChange
		case b3_cylinderShape:
			b3RemoveHullFromDatabase( world, shape->cylinderHull );
			shape->cylinderHull = NULL;
			break;

// b3MakeShapeProxy: the stand-in's points
		case b3_cylinderShape:
		{
			const b3HullData* hull = shape->cylinderHull;
			return ( b3ShapeProxy ){ b3GetHullPoints( hull ), hull->vertexCount, 0.0f };
		}
```

The debug-shape switch in `shape.c` (the one with `debugShape.capsule = &shape->capsule;`, about line 1940) gets:

```c
				case b3_cylinderShape:
					// A debug renderer draws its stand-in hull.
					debugShape.type = b3_hullShape;
					debugShape.hull = shape->cylinderHull;
					shape->userShape = world->createDebugShape( &debugShape, world->userDebugShapeContext );
					break;
```

After `b3CreateCapsuleShape`, add:

```c
b3ShapeId b3CreateCylinderShape( b3BodyId bodyId, const b3ShapeDef* def, const b3Cylinder* cylinder )
{
	B3_ASSERT( cylinder->radius >= B3_LINEAR_SLOP );
	B3_ASSERT( b3Distance( cylinder->center1, cylinder->center2 ) >= B3_LINEAR_SLOP );
	b3ShapeId shapeId = b3CreateShape( bodyId, def, cylinder, b3_cylinderShape, b3Transform_identity, b3Vec3_one, false );
	if ( shapeId.index1 != 0 )
	{
		b3World* world = b3GetUnlockedWorld( bodyId.world0 );
		if ( world != NULL )
		{
			B3_REC_CREATE( world, CreateCylinderShape, shapeId, bodyId, *def, *cylinder );
		}
	}
	return shapeId;
}
```

After `b3Shape_GetCapsule`, add:

```c
b3Cylinder b3Shape_GetCylinder( b3ShapeId shapeId )
{
	b3World* world = b3GetWorld( shapeId.world0 );
	b3Shape* shape = b3GetShape( world, shapeId );
	B3_ASSERT( shape->type == b3_cylinderShape );
	return shape->cylinder;
}
```

After `b3Shape_SetCapsule`, add:

```c
void b3Shape_SetCylinder( b3ShapeId shapeId, const b3Cylinder* cylinder )
{
	b3World* world = b3GetUnlockedWorld( shapeId.world0 );
	if ( world == NULL )
	{
		return;
	}

	b3HullData* standIn = b3MakeCylinderHull( cylinder );
	if ( standIn == NULL )
	{
		return;
	}

	B3_REC( world, ShapeSetCylinder, shapeId, *cylinder );

	world->locked = true;

	b3Shape* shape = b3GetShape( world, shapeId );

	b3DestroyShapeAllocationForShapeChange( world, shape );

	shape->cylinder = *cylinder;
	shape->cylinderHull = b3AddOwnedHullToDatabase( world, standIn );
	shape->type = b3_cylinderShape;
	shape->aabbMargin = b3ComputeShapeMargin( shape );

	// need to wake bodies so they can react to the shape change
	bool wakeBodies = true;
	bool destroyProxy = true;
	b3ResetProxy( world, shape, wakeBodies, destroyProxy );

	world->locked = false;
}
```

If `b3Shape_SetCapsule`'s body (read it) does anything after `b3ResetProxy` beyond `world->locked = false;`, copy that into `b3Shape_SetCylinder` too.

- [ ] **Step 5: The other files' type switches**

`src/physics_world.c`, in the debug-shape switch beside `case b3_capsuleShape:` (about line 1327):

```c
				case b3_cylinderShape:
					// A debug renderer draws its stand-in hull.
					debugShape.type = b3_hullShape;
					debugShape.hull = shape->cylinderHull;
					shape->userShape = world->createDebugShape( &debugShape, world->userDebugShapeContext );
					break;
```

`src/sensor.c`, in the sensor overlap switch beside `case b3_capsuleShape:` (about line 52):

```c
		case b3_cylinderShape:
			return b3OverlapHull( sensorShape->cylinderHull, b3Transform_identity, &localProxy );
```

`src/world_snapshot.c`:
- Beside the write `case b3_capsuleShape:` (about line 575):

  ```c
			case b3_cylinderShape:
				b3SnapW_I32( buf, (int)b3_cylinderShape );
				b3SnapW_Bytes( buf, &src->cylinder, sizeof( b3Cylinder ) );
				break;
  ```
- Beside the read `case b3_capsuleShape:` (about line 718):

  ```c
			case b3_cylinderShape:
			{
				b3SnapR_Bytes( r, &dst->cylinder, sizeof( b3Cylinder ) );
				b3HullData* standIn = b3MakeCylinderHull( &dst->cylinder );
				dst->cylinderHull = standIn != NULL ? b3AddOwnedHullToDatabase( world, standIn ) : NULL;
				break;
			}
  ```
- Beside the hull release before overwrite (`if ( s->type == b3_hullShape && s->hull != NULL )`, about line 970):

  ```c
		if ( s->type == b3_cylinderShape && s->cylinderHull != NULL )
		{
			b3RemoveHullFromDatabase( world, s->cylinderHull );
			s->cylinderHull = NULL;
		}
  ```

Recording:
- `src/recording.h`:
  - after `typedef b3Capsule b3RecCType_CAPSULE;` add `typedef b3Cylinder b3RecCType_CYLINDER;`;
  - after `void b3RecW_CAPSULE( b3RecBuffer* buf, b3Capsule v );` add `void b3RecW_CYLINDER( b3RecBuffer* buf, b3Cylinder v );`.
- `src/recording.c`, after `b3RecW_CAPSULE`:

  ```c
void b3RecW_CYLINDER( b3RecBuffer* buf, b3Cylinder v )
{
	b3RecBufAppend( buf, &v, (int)sizeof( b3Cylinder ) );
}
  ```
- `src/recording_replay.c`:
  - after `b3RecR_CAPSULE`:

    ```c
b3Cylinder b3RecR_CYLINDER( b3RecReader* rdr )
{
	b3Cylinder c;
	b3RecRdrBlob( rdr, &c, (int)sizeof( c ) );
	return c;
}
    ```
  - after `b3RecDispatch_CreateCapsuleShape`:

    ```c
static void b3RecDispatch_CreateCylinderShape( const b3RecArgs_CreateCylinderShape* a, b3RecReader* rdr )
{
	b3ShapeId recId = b3RecR_SHAPEID( rdr );
	b3BodyId bodyId = b3RecMakeBodyId( rdr, a->body );
	b3ShapeId gotId = b3CreateCylinderShape( bodyId, &a->def, &a->cylinder );
	b3RecCheckShapeId( rdr, gotId, recId );
}
    ```
  - after `b3RecDispatch_ShapeSetCapsule`:

    ```c
static void b3RecDispatch_ShapeSetCylinder( const b3RecArgs_ShapeSetCylinder* a, b3RecReader* rdr )
{
	b3Shape_SetCylinder( b3RecMakeShapeId( rdr, a->shape ), &a->cylinder );
}
    ```
- `src/recording_ops.inl`, at the end:

  ```c
B3_REC_OP( 0xF3, CreateCylinderShape, RET_SHAPEID, ARG( BODYID, body ) ARG( SHAPEDEF, def ) ARG( CYLINDER, cylinder ) )
B3_REC_OP( 0xF4, ShapeSetCylinder, RET_NONE, ARG( SHAPEID, shape ) ARG( CYLINDER, cylinder ) )
  ```
  Check first that 0xF3 and 0xF4 are unused: `grep -c "0xF3\|0xF4" build/_deps/box3d-src/src/recording_ops.inl` prints `0` before the edit.

- [ ] **Step 6: Check every capsule site has its cylinder**

Run: `grep -n "b3_capsuleShape" build/_deps/box3d-src/src/*.c | grep -v "contact.c\|mesh_contact.c\|compound.c\|convex_manifold.c\|capsule.c"`
Expected: each line printed has a `b3_cylinderShape` case in the same switch, added above. (`contact.c` and `mesh_contact.c` come in Tasks 3–6. `compound.c`: compound children cannot be cylinders. The wind function's capsule case stays alone, since its early type check skips cylinders.) If a site is not covered, add a case that answers with the stand-in hull, as the hull case beside it does with `shape->hull`.

Regenerate the patch and check it reverses.

- [ ] **Step 7: Run the tests to see them pass**

Run: build, then `./build/Release/sandbox.exe "[cylinder]"`.
Expected: `All tests passed (… in 9 test cases)`.

- [ ] **Step 8: Commit**

```bash
git add CMakeLists.txt cmake/box3d/cylinder.patch sandbox/cylinder_world_tests.cpp
git commit -m "Box3D: b3_cylinderShape, with its stand-in hull answering queries, sensors, snapshots, and recordings

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: GJK on support functions, and the sphere and capsule pairs

**Files:**
- Modify: `cmake/box3d/cylinder.c`, `sandbox/cylinder_shape_tests.cpp`, `sandbox/cylinder_world_tests.cpp`
- Patch: `src/distance.c`, `include/box3d/collision.h`, `src/contact.c`

**Interfaces:**
- Consumes: `b3CylinderFrame`, `b3MakeCylinderFrame` (Task 1); `shape->cylinder`, `shape->cylinderHull` (Task 2).
- Produces:
  - `typedef b3Vec3 b3SupportFcn( const void* shape, b3Vec3 direction );`;
  - `b3DistanceOutput b3SupportDistance( b3SupportFcn* supportA, const void* shapeA, b3SupportFcn* supportB, const void* shapeB, b3Transform transformBtoA );`;
  - `void b3CollideCylinderAndSphere( b3LocalManifold* manifold, int capacity, const b3Cylinder* cylinderA, const b3Sphere* sphereB, b3Transform transformBtoA );`;
  - `void b3CollideCylinderAndCapsule( b3LocalManifold* manifold, int capacity, const b3Cylinder* cylinderA, const b3HullData* standInA, const b3Capsule* capsuleB, b3Transform transformBtoA, b3SimplexCache* cache );`.
- Internal to `cylinder.c`, used later:
  - `b3CylinderCore { b3Vec3 center1, center2, axis; float radius, margin; }` and `b3MakeCylinderCore`;
  - `b3CylinderCoreSupport`, `b3PointCloud { const b3Vec3* points; int count; }`, `b3PointCloudSupport`;
  - `b3SinglePoint`, and the constants `b3_cylinderCapCos`, `b3_cylinderSideSin`.

- [ ] **Step 1: Write the failing tests**

Append to `sandbox/cylinder_shape_tests.cpp`:

```cpp
namespace {

struct Cloud {
    const b3Vec3* points;
    int count;
};

b3Vec3 cloud_support(const void* data, b3Vec3 direction) {
    const Cloud* cloud = static_cast<const Cloud*>(data);
    int best = 0;
    for (int i = 1; i < cloud->count; ++i) {
        if (b3Dot(cloud->points[i], direction) > b3Dot(cloud->points[best], direction)) {
            best = i;
        }
    }
    return cloud->points[best];
}

}  // namespace

TEST_CASE("CY10 GJK on support functions finds what Box3D's point GJK finds", "[cylinder]") {
    const b3BoxHull a = b3MakeBoxHull(0.5f, 0.5f, 0.5f);
    const b3BoxHull b = b3MakeBoxHull(0.3f, 0.6f, 0.2f);
    const Cloud cloudA{b3GetHullPoints(&a.base), a.base.vertexCount};
    const Cloud cloudB{b3GetHullPoints(&b.base), b.base.vertexCount};
    for (const b3Vec3 offset : {v3(2.f, 0.4f, -0.3f), v3(0.f, 1.5f, 0.2f), v3(0.1f, 0.2f, 0.f)}) {
        const b3Transform xf{offset, b3MakeQuatFromAxisAngle(b3Normalize(v3(1.f, 2.f, 3.f)), 0.7f)};
        b3DistanceInput input{};
        input.proxyA = b3ShapeProxy{cloudA.points, cloudA.count, 0.f};
        input.proxyB = b3ShapeProxy{cloudB.points, cloudB.count, 0.f};
        input.transform = xf;
        input.useRadii = false;
        b3SimplexCache cache{};
        const b3DistanceOutput expected = b3ShapeDistance(&input, &cache, nullptr, 0);
        const b3DistanceOutput got = b3SupportDistance(cloud_support, &cloudA, cloud_support, &cloudB, xf);
        INFO(offset.x << " " << offset.y << " " << offset.z);
        REQUIRE(near(got.distance, expected.distance, 1e-5f));
        if (expected.distance > 0.f) {
            REQUIRE(near(got.normal.x, expected.normal.x, 1e-4f));
            REQUIRE(near(got.normal.y, expected.normal.y, 1e-4f));
            REQUIRE(near(got.normal.z, expected.normal.z, 1e-4f));
        }
    }
}

TEST_CASE("CY11 a sphere touches a cylinder's side, cap, and rim at the true surface, at any depth", "[cylinder]") {
    const b3Cylinder c = upright(0.5f, 1.f);
    b3LocalManifoldPoint points[4];
    b3LocalManifold m{};
    m.points = points;
    auto collide = [&](b3Vec3 at, float radius) {
        const b3Sphere sphere{v3(0.f, 0.f, 0.f), radius};
        b3CollideCylinderAndSphere(&m, 4, &c, &sphere, b3Transform{at, b3Quat_identity});
    };
    // Beside the side, 0.01 away.
    collide(v3(0.71f, 0.3f, 0.f), 0.2f);
    REQUIRE(m.pointCount == 1);
    REQUIRE(near(m.normal.x, 1.f, 1e-6f));
    REQUIRE(near(m.points[0].separation, 0.01f, 1e-5f));
    // Above the top cap, 0.05 into it.
    collide(v3(0.1f, 1.15f, 0.2f), 0.2f);
    REQUIRE(m.pointCount == 1);
    REQUIRE(near(m.normal.y, 1.f, 1e-6f));
    REQUIRE(near(m.points[0].separation, -0.05f, 1e-5f));
    // Off the rim, diagonally: the rim is nearest.
    collide(v3(0.6f, 1.1f, 0.f), 0.13f);
    REQUIRE(m.pointCount == 1);
    REQUIRE(near(m.normal.x, std::sqrt(0.5f), 1e-5f));
    REQUIRE(near(m.normal.y, std::sqrt(0.5f), 1e-5f));
    REQUIRE(near(m.points[0].separation, std::sqrt(0.02f) - 0.13f, 1e-5f));
    // Deep inside, nearer the side than a cap: out through the side.
    collide(v3(0.3f, 0.f, 0.f), 0.1f);
    REQUIRE(m.pointCount == 1);
    REQUIRE(near(m.normal.x, 1.f, 1e-6f));
    REQUIRE(near(m.points[0].separation, -0.3f, 1e-5f));
    // Far off: nothing.
    collide(v3(3.f, 0.f, 0.f), 0.1f);
    REQUIRE(m.pointCount == 0);
}

TEST_CASE("CY12 a capsule along a cylinder's side or across its cap touches at two points, else one", "[cylinder]") {
    const b3Cylinder c = upright(0.5f, 1.f);
    b3HullData* standIn = b3MakeCylinderHull(&c);
    b3LocalManifoldPoint points[4];
    b3LocalManifold m{};
    m.points = points;
    b3SimplexCache cache{};
    // Along the side: parallel to the axis, 0.01 off it.
    const b3Capsule along{v3(0.f, -0.5f, 0.f), v3(0.f, 0.5f, 0.f), 0.1f};
    b3CollideCylinderAndCapsule(&m, 4, &c, standIn, &along, b3Transform{v3(0.61f, 0.f, 0.f), b3Quat_identity}, &cache);
    REQUIRE(m.pointCount == 2);
    REQUIRE(near(m.normal.x, 1.f, 1e-5f));
    REQUIRE(near(m.points[0].separation, 0.01f, 1e-5f));
    REQUIRE(near(m.points[1].separation, 0.01f, 1e-5f));
    // Lying across the top cap, 0.01 into it.
    const b3Capsule across{v3(-0.3f, 0.f, 0.f), v3(0.3f, 0.f, 0.f), 0.1f};
    b3CollideCylinderAndCapsule(&m, 4, &c, standIn, &across, b3Transform{v3(0.f, 1.09f, 0.f), b3Quat_identity}, &cache);
    REQUIRE(m.pointCount == 2);
    REQUIRE(near(m.normal.y, 1.f, 1e-5f));
    REQUIRE(near(m.points[0].separation, -0.01f, 1e-5f));
    REQUIRE(near(m.points[1].separation, -0.01f, 1e-5f));
    // Slanting, its low end 0.0177 off the side: one point, at that end.
    const b3Quat slant = b3MakeQuatFromAxisAngle(v3(0.f, 0.f, 1.f), 0.6f);
    b3CollideCylinderAndCapsule(&m, 4, &c, standIn, &along, b3Transform{v3(0.9f, 0.f, 0.f), slant}, &cache);
    REQUIRE(m.pointCount == 1);
    REQUIRE(near(m.points[0].separation, 0.9f - 0.5f * std::sin(0.6f) - 0.6f, 1e-4f));
    b3DestroyHull(standIn);
}
```

Append to `sandbox/cylinder_world_tests.cpp`:

```cpp
TEST_CASE("CY13 a sphere rests on a cylinder's cap, a capsule along its side", "[cylinder]") {
    World world;
    // Standing, radius 0.5, its top at y = 1.
    world.cylinder(b3_staticBody, v3(0.f, 0.f, 0.f), b3Quat_identity, 0.5f, 1.f);
    const b3BodyId ball = world.body(b3_dynamicBody, v3(0.1f, 1.5f, 0.f));
    b3ShapeDef def = b3DefaultShapeDef();
    const b3Sphere sphere{v3(0.f, 0.f, 0.f), 0.2f};
    b3CreateSphereShape(ball, &def, &sphere);
    // Lying along Z at x = 5, its top at y = 0.5: a capsule laid along it.
    world.cylinder(b3_staticBody, v3(5.f, 0.f, 0.f), on_side(), 0.5f, 2.f);
    const b3BodyId stick = world.body(b3_dynamicBody, v3(5.f, 0.8f, 0.f));
    const b3Capsule capsule{v3(0.f, 0.f, -1.f), v3(0.f, 0.f, 1.f), 0.2f};
    b3CreateCapsuleShape(stick, &def, &capsule);
    world.seconds(2.f);
    REQUIRE(near(position_of(ball).y, 1.2f, 0.01f));
    REQUIRE(near(position_of(ball).x, 0.1f, 0.01f));
    REQUIRE(near(position_of(stick).y, 0.7f, 0.01f));
}
```

- [ ] **Step 2: Run them to see them fail**

Run: build.
Expected: FAIL to compile: `b3SupportDistance`, `b3CollideCylinderAndSphere`, `b3CollideCylinderAndCapsule` undeclared.

- [ ] **Step 3: GJK on support functions (`src/distance.c`, patch)**

In `include/box3d/collision.h`, right after the declaration of `b3ShapeDistance`:

```c
/// A support function: the point of a convex shape farthest along direction, in the shape's own frame.
/// Anarchy Engine.
typedef b3Vec3 b3SupportFcn( const void* shape, b3Vec3 direction );

/// GJK distance between two convex shapes given by support functions, for shapes no point cloud holds
/// exactly (a cylinder). Results are in frame A. No cache: a support function has no indices to warm start
/// from. Anarchy Engine.
B3_API b3DistanceOutput b3SupportDistance( b3SupportFcn* supportA, const void* shapeA, b3SupportFcn* supportB,
										   const void* shapeB, b3Transform transformBtoA );
```

In `src/distance.c`, right after the end of `b3ShapeDistance` (before the comment `// Separation function:`):

```c
// Anarchy Engine: b3ShapeDistance's loop with support functions in place of
// point clouds, so the same simplex solver serves a round shape. A support
// point never repeats exactly on a round surface, so it also stops when a new
// support point brings the simplex no nearer than a millionth.
#define B3_MAX_SUPPORT_GJK_ITERATIONS 64

b3DistanceOutput b3SupportDistance( b3SupportFcn* supportA, const void* shapeA, b3SupportFcn* supportB,
									const void* shapeB, b3Transform transformBtoA )
{
	b3Transform xf = transformBtoA;
	b3Matrix3 m = b3MakeMatrixFromQuat( xf.q );
	b3Matrix3 mt = b3Transpose( m );

	b3Simplex simplex = { 0 };
	b3SimplexVertex* vs = simplex.vertices;

	b3Vec3 start = b3Vec3_axisX;
	vs[0].wA = supportA( shapeA, b3Neg( start ) );
	vs[0].wB = b3Add( b3MulMV( m, supportB( shapeB, b3MulMV( mt, start ) ) ), xf.p );
	vs[0].w = b3Sub( vs[0].wB, vs[0].wA );
	vs[0].a = 1.0f;
	vs[0].indexA = 0;
	vs[0].indexB = 0;
	simplex.count = 1;

	b3Simplex backup = { 0 };
	b3DistanceOutput output = { 0 };
	float distanceSq = FLT_MAX;
	b3Vec3 normal = b3Vec3_zero;

	int iteration = 0;
	for ( ; iteration < B3_MAX_SUPPORT_GJK_ITERATIONS; ++iteration )
	{
		bool solved = false;
		switch ( simplex.count )
		{
			case 1:
				vs[0].a = 1.0f;
				solved = true;
				break;
			case 2:
				solved = b3SolveSimplex2( &simplex );
				break;
			case 3:
				solved = b3SolveSimplex3( &simplex );
				break;
			case 4:
				solved = b3SolveSimplex4( &simplex );
				break;
			default:
				B3_ASSERT( !"Should never get here!" );
				break;
		}

		if ( solved == false )
		{
			B3_ASSERT( backup.count != 0 );
			simplex = backup;
			break;
		}

		if ( simplex.count == B3_MAX_SIMPLEX_VERTICES )
		{
			// Overlap
			b3ComputeWitnessPoints( &simplex, &output.pointA, &output.pointB );
			output.normal = b3Vec3_zero;
			output.distance = 0.0f;
			output.iterations = iteration;
			return output;
		}

		float oldDistanceSq = distanceSq;

		b3Vec3 closest = b3Vec3_zero;
		switch ( simplex.count )
		{
			case 1:
				closest = vs[0].w;
				break;
			case 2:
				closest = b3Blend2( vs[0].a, vs[0].w, vs[1].a, vs[1].w );
				break;
			case 3:
				closest = b3Blend3( vs[0].a, vs[0].w, vs[1].a, vs[1].w, vs[2].a, vs[2].w );
				break;
			default:
				B3_ASSERT( !"Should never get here!" );
				break;
		}

		distanceSq = b3Dot( closest, closest );
		if ( distanceSq >= oldDistanceSq )
		{
			B3_ASSERT( backup.count != 0 );
			simplex = backup;
			break;
		}

		if ( distanceSq < 1000.0f * FLT_MIN )
		{
			// The origin is on the simplex: overlap.
			b3ComputeWitnessPoints( &simplex, &output.pointA, &output.pointB );
			output.normal = b3Vec3_zero;
			output.distance = 0.0f;
			output.iterations = iteration;
			return output;
		}

		// The closest point's direction is the normal, from A to B.
		normal = closest;

		// Support of B - A toward the origin.
		b3Vec3 search = b3Neg( closest );
		b3Vec3 wA = supportA( shapeA, b3Neg( search ) );
		b3Vec3 wB = b3Add( b3MulMV( m, supportB( shapeB, b3MulMV( mt, search ) ) ), xf.p );
		b3Vec3 w = b3Sub( wB, wA );

		// Converged: the new support point gets no nearer the origin.
		if ( distanceSq - b3Dot( closest, w ) <= 1.0e-6f * distanceSq )
		{
			break;
		}

		backup = simplex;

		bool duplicate = false;
		for ( int i = 0; i < simplex.count; ++i )
		{
			if ( b3DistanceSquared( vs[i].w, w ) <= 1.0e-12f * ( 1.0f + distanceSq ) )
			{
				duplicate = true;
				break;
			}
		}

		if ( duplicate )
		{
			break;
		}

		vs[simplex.count].indexA = simplex.count;
		vs[simplex.count].indexB = simplex.count;
		vs[simplex.count].wA = wA;
		vs[simplex.count].wB = wB;
		vs[simplex.count].w = w;
		simplex.count += 1;
	}

	b3ComputeWitnessPoints( &simplex, &output.pointA, &output.pointB );
	output.iterations = iteration;

	normal = b3Normalize( normal );
	if ( b3IsNormalized( normal ) == false )
	{
		output.distance = 0.0f;
		output.normal = b3Vec3_zero;
		return output;
	}

	output.distance = b3Distance( output.pointA, output.pointB );
	output.normal = normal;
	return output;
}
```

- [ ] **Step 4: Cores, supports, and the two pairs (`cylinder.c`)**

Append to `cmake/box3d/cylinder.c`:

```c
// Normal and axis within 10 degrees: a cap faces the other shape.
static const float b3_cylinderCapCos = 0.98480775f;
// Normal and axis within 10 degrees of square: the side faces it.
static const float b3_cylinderSideSin = 0.17364818f;

// The cylinder shrunk by margin on every side. The true shape is the core grown
// by margin: exact on its side and caps, its rims rounded by margin. GJK keeps a
// positive distance between cores while the true shapes overlap by less than
// margin, as Box3D's capsule (a segment plus a radius) does.
typedef struct b3CylinderCore
{
	b3Vec3 center1;
	b3Vec3 center2;
	b3Vec3 axis;
	float radius;
	float margin;
} b3CylinderCore;

static b3CylinderCore b3MakeCylinderCore( const b3Cylinder* shape )
{
	b3CylinderFrame frame = b3MakeCylinderFrame( shape );
	float margin = b3MinFloat( B3_SPECULATIVE_DISTANCE, 0.25f * b3MinFloat( frame.radius, frame.halfHeight ) );
	b3CylinderCore core;
	core.axis = frame.axis;
	core.center1 = b3MulSub( frame.center, frame.halfHeight - margin, frame.axis );
	core.center2 = b3MulAdd( frame.center, frame.halfHeight - margin, frame.axis );
	core.radius = frame.radius - margin;
	core.margin = margin;
	return core;
}

static b3Vec3 b3CylinderCoreSupport( const void* data, b3Vec3 direction )
{
	const b3CylinderCore* core = data;
	float along = b3Dot( direction, core->axis );
	b3Vec3 point = along >= 0.0f ? core->center2 : core->center1;
	b3Vec3 across = b3MulSub( direction, along, core->axis );
	float length = b3Length( across );
	if ( length > 1000.0f * FLT_MIN )
	{
		point = b3MulAdd( point, core->radius / length, across );
	}
	return point;
}

typedef struct b3PointCloud
{
	const b3Vec3* points;
	int count;
} b3PointCloud;

static b3Vec3 b3PointCloudSupport( const void* data, b3Vec3 direction )
{
	const b3PointCloud* cloud = data;
	return cloud->points[b3GetPointSupport( cloud->points, cloud->count, direction )];
}

// One contact point midway between pointA on A and pointB on B.
static void b3SinglePoint( b3LocalManifold* manifold, b3Vec3 normal, b3Vec3 pointA, b3Vec3 pointB, float separation )
{
	manifold->normal = normal;
	manifold->pointCount = 1;
	manifold->points[0].point = b3Lerp( pointA, pointB, 0.5f );
	manifold->points[0].separation = separation;
	manifold->points[0].pair = b3FeaturePair_single;
}

void b3CollideCylinderAndSphere( b3LocalManifold* manifold, int capacity, const b3Cylinder* cylinderA, const b3Sphere* sphereB,
								 b3Transform transformBtoA )
{
	manifold->pointCount = 0;
	if ( capacity < 1 )
	{
		return;
	}

	b3CylinderFrame frame = b3MakeCylinderFrame( cylinderA );
	b3Vec3 center = b3TransformPoint( transformBtoA, sphereB->center );
	b3Vec3 offset = b3Sub( center, frame.center );
	float along = b3Dot( offset, frame.axis );
	b3Vec3 across = b3MulSub( offset, along, frame.axis );
	float acrossLength;
	b3Vec3 radial = b3GetLengthAndNormalize( &acrossLength, across );
	if ( acrossLength == 0.0f )
	{
		radial = b3Normalize( b3Perp( frame.axis ) );
	}

	// The closest point on the solid cylinder: exact at any depth.
	b3Vec3 normal = b3Vec3_zero;
	b3Vec3 surface = b3Vec3_zero;
	float distance = 0.0f;
	bool outside = acrossLength > frame.radius || b3AbsFloat( along ) > frame.halfHeight;
	if ( outside )
	{
		float clampedAlong = b3ClampFloat( along, -frame.halfHeight, frame.halfHeight );
		float clampedAcross = b3MinFloat( acrossLength, frame.radius );
		surface = b3MulAdd( b3MulAdd( frame.center, clampedAlong, frame.axis ), clampedAcross, radial );
		normal = b3GetLengthAndNormalize( &distance, b3Sub( center, surface ) );
	}

	if ( outside == false || distance == 0.0f )
	{
		// Inside: out through the nearer of the side and a cap.
		float toSide = frame.radius - acrossLength;
		float toCap = frame.halfHeight - b3AbsFloat( along );
		if ( toSide < toCap )
		{
			normal = radial;
			surface = b3MulAdd( b3MulAdd( frame.center, along, frame.axis ), frame.radius, radial );
			distance = -toSide;
		}
		else
		{
			float sign = along >= 0.0f ? 1.0f : -1.0f;
			normal = b3MulSV( sign, frame.axis );
			surface = b3Add( b3MulAdd( frame.center, sign * frame.halfHeight, frame.axis ), across );
			distance = -toCap;
		}
	}

	float separation = distance - sphereB->radius;
	if ( separation > B3_SPECULATIVE_DISTANCE )
	{
		return;
	}

	b3SinglePoint( manifold, normal, surface, b3MulSub( center, sphereB->radius, normal ), separation );
}

// The part of segment p1-p2 over the disc (center, unit normal, radius),
// measured in the disc's plane, as fractions s0 <= s1 along it. False: none.
static bool b3ClipSegmentToDisc( float* s0, float* s1, b3Vec3 p1, b3Vec3 p2, b3Vec3 center, b3Vec3 normal, float radius )
{
	b3Vec3 a = b3Sub( p1, center );
	a = b3MulSub( a, b3Dot( a, normal ), normal );
	b3Vec3 d = b3Sub( p2, p1 );
	d = b3MulSub( d, b3Dot( d, normal ), normal );
	float qa = b3Dot( d, d );
	float qb = b3Dot( a, d );
	float qc = b3Dot( a, a ) - radius * radius;
	if ( qa < 1000.0f * FLT_MIN )
	{
		*s0 = 0.0f;
		*s1 = 1.0f;
		return qc <= 0.0f;
	}

	float discriminant = qb * qb - qa * qc;
	if ( discriminant < 0.0f )
	{
		return false;
	}

	float root = sqrtf( discriminant );
	*s0 = b3MaxFloat( ( -qb - root ) / qa, 0.0f );
	*s1 = b3MinFloat( ( -qb + root ) / qa, 1.0f );
	return *s0 <= *s1;
}

void b3CollideCylinderAndCapsule( b3LocalManifold* manifold, int capacity, const b3Cylinder* cylinderA,
								  const b3HullData* standInA, const b3Capsule* capsuleB, b3Transform transformBtoA,
								  b3SimplexCache* cache )
{
	manifold->pointCount = 0;
	if ( capacity < 2 )
	{
		return;
	}

	b3CylinderFrame frame = b3MakeCylinderFrame( cylinderA );
	b3CylinderCore core = b3MakeCylinderCore( cylinderA );
	b3PointCloud segmentB = { &capsuleB->center1, 2 };
	b3DistanceOutput output =
		b3SupportDistance( b3CylinderCoreSupport, &core, b3PointCloudSupport, &segmentB, transformBtoA );
	float capsuleRadius = capsuleB->radius;
	float separation = output.distance - core.margin - capsuleRadius;
	if ( separation > B3_SPECULATIVE_DISTANCE )
	{
		return;
	}

	if ( output.distance < 100.0f * FLT_EPSILON )
	{
		// The cores overlap: Box3D's hull and capsule answer, the stand-in in the cylinder's place.
		b3CollideHullAndCapsule( manifold, capacity, standInA, capsuleB, transformBtoA, cache );
		return;
	}

	b3Vec3 normal = output.normal;
	b3Vec3 p1 = b3TransformPoint( transformBtoA, capsuleB->center1 );
	b3Vec3 p2 = b3TransformPoint( transformBtoA, capsuleB->center2 );
	b3Vec3 segment = b3Normalize( b3Sub( p2, p1 ) );
	float normalAlong = b3Dot( normal, frame.axis );
	float segmentAlong = b3Dot( segment, frame.axis );
	float speculative = B3_SPECULATIVE_DISTANCE;

	if ( b3AbsFloat( normalAlong ) < b3_cylinderSideSin && b3AbsFloat( segmentAlong ) > 0.998f )
	{
		// Along the side: a line contact, where the two overlap along the axis.
		b3Vec3 radial = b3Normalize( b3MulSub( normal, normalAlong, frame.axis ) );
		float t1 = b3Dot( b3Sub( p1, frame.center ), frame.axis );
		float t2 = b3Dot( b3Sub( p2, frame.center ), frame.axis );
		float lo = b3ClampFloat( b3MinFloat( t1, t2 ), -frame.halfHeight, frame.halfHeight );
		float hi = b3ClampFloat( b3MaxFloat( t1, t2 ), -frame.halfHeight, frame.halfHeight );
		if ( hi - lo > B3_LINEAR_SLOP )
		{
			float ends[2] = { lo, hi };
			int count = 0;
			for ( int i = 0; i < 2; ++i )
			{
				float s = ( ends[i] - t1 ) / ( t2 - t1 );
				b3Vec3 onSegment = b3Lerp( p1, p2, s );
				b3Vec3 onSide = b3MulAdd( b3MulAdd( frame.center, ends[i], frame.axis ), frame.radius, radial );
				float pointSeparation = b3Dot( b3Sub( onSegment, onSide ), radial ) - capsuleRadius;
				if ( pointSeparation > speculative )
				{
					continue;
				}
				b3LocalManifoldPoint* mp = manifold->points + count;
				mp->point = b3Lerp( onSide, b3MulSub( onSegment, capsuleRadius, radial ), 0.5f );
				mp->separation = pointSeparation;
				mp->pair = b3MakeFeaturePair( b3_featureShapeA, i, b3_featureShapeB, i );
				count += 1;
			}
			if ( count > 0 )
			{
				manifold->normal = radial;
				manifold->pointCount = count;
				return;
			}
		}
	}

	if ( b3AbsFloat( normalAlong ) >= b3_cylinderCapCos && b3AbsFloat( segmentAlong ) < b3_cylinderSideSin )
	{
		// Across a cap: a line contact, the segment's part over the cap.
		float sign = normalAlong > 0.0f ? 1.0f : -1.0f;
		b3Vec3 capNormal = b3MulSV( sign, frame.axis );
		b3Vec3 capCenter = b3MulAdd( frame.center, sign * frame.halfHeight, frame.axis );
		float s0, s1;
		if ( b3ClipSegmentToDisc( &s0, &s1, p1, p2, capCenter, capNormal, frame.radius ) && ( s1 - s0 ) > 0.0f )
		{
			float fractions[2] = { s0, s1 };
			int count = 0;
			for ( int i = 0; i < 2; ++i )
			{
				b3Vec3 onSegment = b3Lerp( p1, p2, fractions[i] );
				float height = b3Dot( b3Sub( onSegment, capCenter ), capNormal );
				float pointSeparation = height - capsuleRadius;
				if ( pointSeparation > speculative )
				{
					continue;
				}
				b3Vec3 onCap = b3MulSub( onSegment, height, capNormal );
				b3LocalManifoldPoint* mp = manifold->points + count;
				mp->point = b3Lerp( onCap, b3MulSub( onSegment, capsuleRadius, capNormal ), 0.5f );
				mp->separation = pointSeparation;
				mp->pair = b3MakeFeaturePair( b3_featureShapeA, 2 + i, b3_featureShapeB, i );
				count += 1;
			}
			if ( count > 0 )
			{
				manifold->normal = capNormal;
				manifold->pointCount = count;
				return;
			}
		}
	}

	// Otherwise one point, the true surfaces' nearest.
	b3Vec3 surfaceA = b3MulAdd( output.pointA, core.margin, normal );
	b3Vec3 surfaceB = b3MulSub( output.pointB, capsuleRadius, normal );
	b3SinglePoint( manifold, normal, surfaceA, surfaceB, separation );
}
```

Declare both pairs in `include/box3d/collision.h` (patch), right before `/// Collide two spheres.`:

```c
/// Collide a cylinder and a sphere. Exact at any depth. Anarchy Engine.
B3_API void b3CollideCylinderAndSphere( b3LocalManifold* manifold, int capacity, const b3Cylinder* cylinderA,
										const b3Sphere* sphereB, b3Transform transformBtoA );

/// Collide a cylinder and a capsule. standInA is the cylinder's stand-in hull, for deep overlap. Anarchy Engine.
B3_API void b3CollideCylinderAndCapsule( b3LocalManifold* manifold, int capacity, const b3Cylinder* cylinderA,
										 const b3HullData* standInA, const b3Capsule* capsuleB, b3Transform transformBtoA,
										 b3SimplexCache* cache );
```

- [ ] **Step 5: Dispatch the pairs (`src/contact.c`, patch)**

In `b3InitializeContactRegisters` (the `b3AddType` list), after `b3AddType( b3_heightShape, b3_hullShape );`:

```c
		b3AddType( b3_cylinderShape, b3_sphereShape );
		b3AddType( b3_cylinderShape, b3_capsuleShape );
```

In `b3ComputeConvexManifold`, change the shape-A dispatch. Right before the final `else` that asserts `typeA == b3_hullShape`, insert:

```c
	else if ( typeA == b3_cylinderShape )
	{
		if ( typeB == b3_sphereShape )
		{
			b3CollideCylinderAndSphere( &geomManifold, pointCapacity, &shapeA->cylinder, &shapeB->sphere, transformBtoA );
		}
		else
		{
			B3_ASSERT( typeB == b3_capsuleShape );
			b3CollideCylinderAndCapsule( &geomManifold, pointCapacity, &shapeA->cylinder, shapeA->cylinderHull,
										 &shapeB->capsule, transformBtoA, &cache->simplexCache );
		}
	}
```

In both rolling-resistance radius blocks in `contact.c` (one in contact creation, about line 300; one in `b3UpdateConvexContact`, about line 664), add to each `radiusA` and `radiusB` chain:

```c
	else if ( typeA == b3_cylinderShape )
	{
		radiusA = shapeA->cylinder.radius;
	}
```

(and the `typeB` / `shapeB` / `radiusB` form for B).

Regenerate the patch and check it reverses.

- [ ] **Step 6: Run the tests to see them pass**

Run: build, then `./build/Release/sandbox.exe "[cylinder]"`.
Expected: `All tests passed (… in 13 test cases)`.

- [ ] **Step 7: Commit**

```bash
git add cmake/box3d/cylinder.c cmake/box3d/cylinder.patch sandbox/cylinder_shape_tests.cpp sandbox/cylinder_world_tests.cpp
git commit -m "Box3D: GJK on support functions, and a cylinder's contacts with spheres and capsules

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: Cylinder against a hull: rolling, resting, and tipping

**Files:**
- Modify: `cmake/box3d/cylinder.c`, `sandbox/cylinder_shape_tests.cpp`, `sandbox/cylinder_world_tests.cpp`
- Patch: `include/box3d/collision.h`, `src/contact.c`

**Interfaces:**
- Consumes: `b3MakeCylinderCore`, `b3CylinderCoreSupport`, `b3PointCloud`, `b3PointCloudSupport`, `b3SinglePoint`, `b3_cylinderCapCos` (Task 3).
- Produces:
  - `void b3CollideHullAndCylinder( b3LocalManifold* manifold, int capacity, const b3HullData* hullA, const b3Cylinder* cylinderB, const b3HullData* standInB, b3Transform transformBtoA, b3SATCache* cache );`.
- Internal to `cylinder.c`, used by Tasks 5–6:
  - `#define B3_CYLINDER_CAP_POINTS 16`;
  - `b3CylinderFeature`, `b3GetCylinderFeature`, `b3CylinderFeatureVertices`;
  - `b3ClipToPlane`, `b3ReducePoints`, `b3BuildCylinderFaceContact`.

- [ ] **Step 1: Write the failing tests**

Append to `sandbox/cylinder_shape_tests.cpp`:

```cpp
TEST_CASE("CY14 a cylinder on its side on a box: two points along its tread, the same two however it has turned",
          "[cylinder]") {
    const b3BoxHull box = b3MakeBoxHull(5.f, 0.5f, 5.f);  // top at y = 0.5
    const b3Cylinder wheel = upright(0.5f, 0.25f);
    b3HullData* standIn = b3MakeCylinderHull(&wheel);
    b3LocalManifoldPoint points[4];
    b3LocalManifold m{};
    m.points = points;
    b3SATCache cache{};
    const b3Quat lie = b3MakeQuatFromAxisAngle(v3(1.f, 0.f, 0.f), 0.5f * kPi);
    for (const float spin : {0.f, 0.37f, 1.3f, 2.9f}) {
        INFO(spin);
        // Lying along Z, 0.005 into the box, turned spin about its own axis.
        const b3Quat turned = b3MulQuat(lie, b3MakeQuatFromAxisAngle(v3(0.f, 1.f, 0.f), spin));
        b3CollideHullAndCylinder(&m, 4, &box.base, &wheel, standIn, b3Transform{v3(0.f, 0.995f, 0.f), turned}, &cache);
        REQUIRE(m.pointCount == 2);
        REQUIRE(near(m.normal.y, 1.f, 1e-6f));
        REQUIRE(near(m.points[0].separation, -0.005f, 1e-5f));
        REQUIRE(near(m.points[1].separation, -0.005f, 1e-5f));
        // The ends of its tread, by name, whatever the spin.
        const int first = std::min(m.points[0].pair.index1, m.points[1].pair.index1);
        const int second = std::max(m.points[0].pair.index1, m.points[1].pair.index1);
        REQUIRE(first == 0);
        REQUIRE(second == 1);
    }
    b3DestroyHull(standIn);
}

TEST_CASE("CY15 a cylinder on its cap on a box: four points, normal up", "[cylinder]") {
    const b3BoxHull box = b3MakeBoxHull(5.f, 0.5f, 5.f);
    const b3Cylinder drum = upright(0.5f, 1.f);
    b3HullData* standIn = b3MakeCylinderHull(&drum);
    b3LocalManifoldPoint points[4];
    b3LocalManifold m{};
    m.points = points;
    b3SATCache cache{};
    b3CollideHullAndCylinder(&m, 4, &box.base, &drum, standIn, b3Transform{v3(0.f, 1.495f, 0.f), b3Quat_identity}, &cache);
    REQUIRE(m.pointCount == 4);
    REQUIRE(near(m.normal.y, 1.f, 1e-6f));
    for (int i = 0; i < 4; ++i) {
        REQUIRE(near(m.points[i].separation, -0.005f, 1e-5f));
    }
    b3DestroyHull(standIn);
}

TEST_CASE("CY16 a leaning wheel's tread rises from its low end, with no change of radius", "[cylinder]") {
    const b3BoxHull box = b3MakeBoxHull(5.f, 0.5f, 5.f);
    const b3Cylinder wheel = upright(0.5f, 0.25f);
    b3HullData* standIn = b3MakeCylinderHull(&wheel);
    b3LocalManifoldPoint points[4];
    b3LocalManifold m{};
    m.points = points;
    b3SATCache cache{};
    // Lying along Z, then leaned 2 degrees about X; its low rim 0.005 into the box.
    const float lean = 2.f * kPi / 180.f;
    const b3Quat q = b3MakeQuatFromAxisAngle(v3(1.f, 0.f, 0.f), 0.5f * kPi + lean);
    const float y = 0.5f + 0.5f * std::cos(lean) + 0.25f * std::sin(lean) - 0.005f;
    b3CollideHullAndCylinder(&m, 4, &box.base, &wheel, standIn, b3Transform{v3(0.f, y, 0.f), q}, &cache);
    REQUIRE(m.pointCount == 2);
    const float low = std::min(m.points[0].separation, m.points[1].separation);
    const float high = std::max(m.points[0].separation, m.points[1].separation);
    REQUIRE(near(low, -0.005f, 1e-5f));
    REQUIRE(near(high, -0.005f + 0.5f * std::sin(lean), 1e-5f));
    b3DestroyHull(standIn);
}
```

Append to `sandbox/cylinder_world_tests.cpp`:

```cpp
namespace {

// Lying along Z on the floor at x, set rolling toward +X at speed without slipping.
b3BodyId rolling_wheel(World& world, float x, float speed, float radius = 0.5f, float half = 0.25f) {
    const b3BodyId wheel = world.cylinder(b3_dynamicBody, v3(x, radius, 0.f), on_side(), radius, half);
    world.seconds(0.25f);
    b3Body_SetLinearVelocity(wheel, v3(speed, 0.f, 0.f));
    b3Body_SetAngularVelocity(wheel, v3(0.f, 0.f, -speed / radius));
    return wheel;
}

}  // namespace

TEST_CASE("CY17 a cylinder rolls 20 units with its centre level, straight, and upright", "[cylinder]") {
    World world;
    world.floor();
    const b3BodyId wheel = rolling_wheel(world, -10.f, 4.f);
    float low = 1e9f;
    float high = -1e9f;
    float drift = 0.f;
    float tilt = 0.f;
    for (int i = 0; i < 240 * 8 && position_of(wheel).x < 10.f; ++i) {
        world.step();
        const b3Vec3 at = position_of(wheel);
        low = std::min(low, at.y);
        high = std::max(high, at.y);
        drift = std::max(drift, std::fabs(at.z));
        tilt = std::max(tilt, std::fabs(axis_of(wheel).y));
    }
    INFO(low << " .. " << high << " drift " << drift << " tilt " << tilt);
    REQUIRE(position_of(wheel).x >= 10.f);
    REQUIRE(high - low < 1e-4f);
    REQUIRE(drift < 1e-3f);
    REQUIRE(tilt < 1.75e-4f);  // sin 0.01 degrees
}

TEST_CASE("CY18 a cylinder on its side rolls down a 5 degree slope, and lies still on the flat", "[cylinder]") {
    World world;
    // A slope falling toward +X, its surface through the origin.
    const float slope = 5.f * kPi / 180.f;
    const b3Quat tilt = b3MakeQuatFromAxisAngle(v3(0.f, 0.f, 1.f), -slope);
    world.box(b3RotateVector(tilt, v3(0.f, -0.5f, 0.f)), v3(20.f, 0.5f, 5.f), 1.f, tilt);
    const b3BodyId down = world.cylinder(b3_dynamicBody, v3(0.f, 0.51f, 0.f), on_side(), 0.5f, 0.25f, 1.f);
    world.seconds(2.f);
    REQUIRE(position_of(down).x > 1.f);

    World flat;
    flat.floor();
    const b3BodyId still = flat.cylinder(b3_dynamicBody, v3(0.f, 0.5f, 0.f), on_side(), 0.5f, 0.25f);
    flat.seconds(1.f);
    const b3Vec3 before = position_of(still);
    flat.seconds(10.f);
    REQUIRE(b3Distance(before, position_of(still)) < 1e-4f);
    REQUIRE_FALSE(b3Body_IsAwake(still));
}

TEST_CASE("CY19 a cylinder on its cap does not creep, and sleeps", "[cylinder]") {
    World world;
    world.floor();
    const b3BodyId drum = world.cylinder(b3_dynamicBody, v3(0.f, 1.f, 0.f), b3Quat_identity, 0.5f, 1.f);
    world.seconds(1.f);
    const b3Vec3 before = position_of(drum);
    world.seconds(10.f);
    REQUIRE(b3Distance(before, position_of(drum)) < 1e-4f);
    REQUIRE_FALSE(b3Body_IsAwake(drum));
}

TEST_CASE("CY20 a coin spun on its rim settles on a cap", "[cylinder]") {
    World world;
    world.floor();
    // A coin, radius 0.5 and 0.05 thick, on its rim, leaning 30 degrees, spinning.
    const b3Quat lean = b3MakeQuatFromAxisAngle(v3(1.f, 0.f, 0.f), 0.5f * kPi - 30.f * kPi / 180.f);
    const b3BodyId coin = world.cylinder(b3_dynamicBody, v3(0.f, 0.47f, 0.f), lean, 0.5f, 0.025f);
    b3Body_SetAngularVelocity(coin, v3(0.f, 6.f, 0.f));
    world.seconds(10.f);
    REQUIRE(std::fabs(axis_of(coin).y) > 0.99f);
    REQUIRE(position_of(coin).y < 0.05f);
}

TEST_CASE("CY21 the same rolling run twice gives the same poses, bit for bit", "[cylinder]") {
    auto run = [] {
        World world;
        world.floor();
        const b3BodyId wheel = rolling_wheel(world, 0.f, 4.f);
        world.seconds(2.f);
        const b3Vec3 at = position_of(wheel);
        const b3Quat q = b3Body_GetRotation(wheel);
        return std::vector<float>{at.x, at.y, at.z, q.v.x, q.v.y, q.v.z, q.s};
    };
    const std::vector<float> first = run();
    const std::vector<float> second = run();
    REQUIRE(std::memcmp(first.data(), second.data(), first.size() * sizeof(float)) == 0);
}

TEST_CASE("CY22 a cylinder rolls across the seam between two boxes without a jump", "[cylinder]") {
    World world;
    world.box(v3(-5.f, -0.5f, 0.f), v3(5.f, 0.5f, 5.f));
    world.box(v3(5.f, -0.5f, 0.f), v3(5.f, 0.5f, 5.f));
    const b3BodyId wheel = rolling_wheel(world, -3.f, 3.f);
    float low = 1e9f;
    float high = -1e9f;
    for (int i = 0; i < 240 * 3 && position_of(wheel).x < 3.f; ++i) {
        world.step();
        low = std::min(low, position_of(wheel).y);
        high = std::max(high, position_of(wheel).y);
    }
    REQUIRE(position_of(wheel).x >= 3.f);
    REQUIRE(high - low < 5e-4f);
}

TEST_CASE("CY22b a long thin pipe lying on a box rests still", "[cylinder]") {
    World world;
    world.floor();
    const b3BodyId pipe = world.cylinder(b3_dynamicBody, v3(0.f, 0.1f, 0.f), on_side(), 0.1f, 5.f);
    world.seconds(2.f);
    const b3Vec3 before = position_of(pipe);
    world.seconds(3.f);
    REQUIRE(b3Distance(before, position_of(pipe)) < 1e-3f);
    REQUIRE(near(position_of(pipe).y, 0.1f, 2e-3f));
}

TEST_CASE("CY22c a box rests on two anchored logs", "[cylinder]") {
    World world;
    // Two static logs along Z at x = -0.5 and +0.5, their tops at y = 0.3.
    world.cylinder(b3_staticBody, v3(-0.5f, 0.f, 0.f), on_side(), 0.3f, 2.f);
    world.cylinder(b3_staticBody, v3(0.5f, 0.f, 0.f), on_side(), 0.3f, 2.f);
    const b3BodyId crate = world.body(b3_dynamicBody, v3(0.f, 0.6f, 0.f));
    const b3BoxHull hull = b3MakeBoxHull(1.f, 0.2f, 1.f);
    b3ShapeDef def = b3DefaultShapeDef();
    b3CreateHullShape(crate, &def, &hull.base);
    world.seconds(3.f);
    REQUIRE(near(position_of(crate).y, 0.5f, 0.01f));
    REQUIRE(near(position_of(crate).x, 0.f, 0.01f));
}
```

Add `#include <algorithm>` to `sandbox/cylinder_world_tests.cpp`'s includes.

- [ ] **Step 2: Run them to see them fail**

Run: build.
Expected: FAIL to compile: `b3CollideHullAndCylinder` undeclared.

- [ ] **Step 3: Features, clipping, reduction, and the hull pair (`cylinder.c`)**

Append to `cmake/box3d/cylinder.c`:

```c
#define B3_CYLINDER_CAP_POINTS 16

// What the cylinder presents to another shape: its near cap, as points round
// the cap's rim fixed in its frame (ids 2..17 for the first cap, 18..33 for the
// second), or its side line, the tread from one rim to the other (ids 0 and 1).
// An id names the feature, not where on the round surface it lies, so a
// rolling cylinder's points keep their ids and Box3D's warm starting holds.
typedef struct b3CylinderFeature
{
	b3Vec3 points[B3_CYLINDER_CAP_POINTS];
	uint8_t ids[B3_CYLINDER_CAP_POINTS];
	int count;
	bool cap;
} b3CylinderFeature;

// direction: unit, in the cylinder's frame, from the cylinder toward the other shape.
static void b3GetCylinderFeature( b3CylinderFeature* feature, const b3CylinderFrame* frame, b3Vec3 direction )
{
	float along = b3Dot( direction, frame->axis );
	if ( b3AbsFloat( along ) >= b3_cylinderCapCos )
	{
		float sign = along > 0.0f ? 1.0f : -1.0f;
		b3Vec3 capCenter = b3MulAdd( frame->center, sign * frame->halfHeight, frame->axis );
		b3Vec3 e1 = b3Normalize( b3Perp( frame->axis ) );
		b3Vec3 e2 = b3Cross( frame->axis, e1 );
		int first = along > 0.0f ? 2 + B3_CYLINDER_CAP_POINTS : 2;
		for ( int i = 0; i < B3_CYLINDER_CAP_POINTS; ++i )
		{
			float angle = 2.0f * B3_PI * (float)i / (float)B3_CYLINDER_CAP_POINTS;
			b3Vec3 rim = b3Add( b3MulSV( cosf( angle ), e1 ), b3MulSV( sinf( angle ), e2 ) );
			feature->points[i] = b3MulAdd( capCenter, frame->radius, rim );
			feature->ids[i] = (uint8_t)( first + i );
		}
		feature->count = B3_CYLINDER_CAP_POINTS;
		feature->cap = true;
		return;
	}

	b3Vec3 radial = b3Normalize( b3MulSub( direction, along, frame->axis ) );
	feature->points[0] = b3MulAdd( b3MulSub( frame->center, frame->halfHeight, frame->axis ), frame->radius, radial );
	feature->points[1] = b3MulAdd( b3MulAdd( frame->center, frame->halfHeight, frame->axis ), frame->radius, radial );
	feature->ids[0] = 0;
	feature->ids[1] = 1;
	feature->count = 2;
	feature->cap = false;
}

// The cylinder's feature facing towardOther (the cylinder's own frame), as clip
// vertices in the frame transform takes it to, paired with the other shape's
// feature otherIndex.
static int b3CylinderFeatureVertices( b3ClipVertex* out, const b3Cylinder* cylinder, b3Vec3 towardOther,
									  b3Transform transform, b3FeatureOwner cylinderOwner, b3FeatureOwner otherOwner,
									  int otherIndex )
{
	b3CylinderFrame frame = b3MakeCylinderFrame( cylinder );
	b3CylinderFeature feature;
	b3GetCylinderFeature( &feature, &frame, towardOther );
	for ( int i = 0; i < feature.count; ++i )
	{
		out[i].position = b3TransformPoint( transform, feature.points[i] );
		out[i].separation = 0.0f;
		out[i].pair = b3MakeFeaturePair( cylinderOwner, feature.ids[i], otherOwner, otherIndex );
	}
	return feature.count;
}

// The part of the points on the inner side of plane (separation <= 0). Two
// points are a segment, more a closed polygon. A point made where an edge
// crosses the plane names clipId as its feature on the clipping shape.
static int b3ClipToPlane( b3ClipVertex* out, const b3ClipVertex* in, int count, b3Plane plane, int clipId )
{
	int outCount = 0;
	if ( count == 1 )
	{
		if ( b3PlaneSeparation( plane, in[0].position ) <= 0.0f )
		{
			out[outCount++] = in[0];
		}
		return outCount;
	}

	int edgeCount = count == 2 ? 1 : count;
	for ( int i = 0; i < edgeCount; ++i )
	{
		const b3ClipVertex* a = in + i;
		const b3ClipVertex* b = in + ( i + 1 ) % count;
		float da = b3PlaneSeparation( plane, a->position );
		float db = b3PlaneSeparation( plane, b->position );
		if ( da <= 0.0f )
		{
			out[outCount++] = *a;
		}
		if ( ( da < 0.0f && db > 0.0f ) || ( da > 0.0f && db < 0.0f ) )
		{
			b3ClipVertex crossing;
			crossing.position = b3Lerp( a->position, b->position, da / ( da - db ) );
			crossing.separation = 0.0f;
			// Named for the end the plane cut away, and the clipping edge.
			crossing.pair = da > 0.0f ? a->pair : b->pair;
			crossing.pair.index2 = (uint8_t)clipId;
			out[outCount++] = crossing;
		}
	}

	if ( count == 2 && b3PlaneSeparation( plane, in[1].position ) <= 0.0f )
	{
		out[outCount++] = in[1];
	}
	return outCount;
}

// At most min(capacity, 4) of the points: the deepest, the farthest from it,
// then the two spanning the most area, as Box3D's hull pairs keep theirs.
static int b3ReducePoints( b3ClipVertex* points, int count, int capacity, b3Vec3 normal )
{
	int limit = b3MinInt( capacity, 4 );
	if ( count <= limit )
	{
		return count;
	}

	int chosen[4];
	int chosenCount = 0;

	int deepest = 0;
	for ( int i = 1; i < count; ++i )
	{
		if ( points[i].separation < points[deepest].separation )
		{
			deepest = i;
		}
	}
	chosen[chosenCount++] = deepest;

	if ( limit > 1 )
	{
		int farthest = -1;
		float best = -1.0f;
		for ( int i = 0; i < count; ++i )
		{
			float d = b3DistanceSquared( points[i].position, points[deepest].position );
			if ( d > best )
			{
				best = d;
				farthest = i;
			}
		}
		chosen[chosenCount++] = farthest;
	}

	if ( limit > 2 )
	{
		b3Vec3 p0 = points[chosen[0]].position;
		b3Vec3 p1 = points[chosen[1]].position;
		int third = -1;
		float best = -1.0f;
		for ( int i = 0; i < count; ++i )
		{
			float area = b3AbsFloat( b3Dot( b3Cross( b3Sub( p1, p0 ), b3Sub( points[i].position, p0 ) ), normal ) );
			if ( area > best && i != chosen[0] && i != chosen[1] )
			{
				best = area;
				third = i;
			}
		}
		chosen[chosenCount++] = third;
	}

	if ( limit > 3 )
	{
		b3Vec3 corners[3] = { points[chosen[0]].position, points[chosen[1]].position, points[chosen[2]].position };
		float winding = b3Dot( b3Cross( b3Sub( corners[1], corners[0] ), b3Sub( corners[2], corners[0] ) ), normal );
		float sign = winding >= 0.0f ? 1.0f : -1.0f;
		int fourth = -1;
		float best = 0.0f;
		for ( int i = 0; i < count; ++i )
		{
			if ( i == chosen[0] || i == chosen[1] || i == chosen[2] )
			{
				continue;
			}
			// How far outside the triangle it reaches, over its three edges.
			for ( int e = 0; e < 3; ++e )
			{
				b3Vec3 a = corners[e];
				b3Vec3 b = corners[( e + 1 ) % 3];
				float outside = -sign * b3Dot( b3Cross( b3Sub( b, a ), b3Sub( points[i].position, a ) ), normal );
				if ( outside > best )
				{
					best = outside;
					fourth = i;
				}
			}
		}
		if ( fourth >= 0 )
		{
			chosen[chosenCount++] = fourth;
		}
	}

	b3ClipVertex kept[4];
	for ( int i = 0; i < chosenCount; ++i )
	{
		kept[i] = points[chosen[i]];
	}
	for ( int i = 0; i < chosenCount; ++i )
	{
		points[i] = kept[i];
	}
	return chosenCount;
}

// A face contact: the cylinder's feature clipped to the reference face's
// sides, its points within maxSeparation of the face, at most capacity of
// them, the normal the face's.
static void b3BuildCylinderFaceContact( b3LocalManifold* manifold, int capacity, const b3ClipVertex* feature,
										int featureCount, b3Plane refPlane, const b3Plane* sidePlanes, int sideCount,
										float maxSeparation )
{
	b3ClipVertex bufferA[B3_MAX_CLIP_POINTS];
	b3ClipVertex bufferB[B3_MAX_CLIP_POINTS];
	b3ClipVertex* in = bufferA;
	b3ClipVertex* out = bufferB;
	int count = featureCount;
	for ( int i = 0; i < count; ++i )
	{
		in[i] = feature[i];
	}

	for ( int i = 0; i < sideCount && count > 0; ++i )
	{
		B3_ASSERT( count < B3_MAX_CLIP_POINTS );
		count = b3ClipToPlane( out, in, count, sidePlanes[i], 128 + ( i & 127 ) );
		b3ClipVertex* swap = in;
		in = out;
		out = swap;
	}

	int kept = 0;
	for ( int i = 0; i < count; ++i )
	{
		float separation = b3PlaneSeparation( refPlane, in[i].position );
		if ( separation <= maxSeparation )
		{
			in[kept] = in[i];
			in[kept].separation = separation;
			kept += 1;
		}
	}

	kept = b3ReducePoints( in, kept, capacity, refPlane.normal );
	manifold->normal = refPlane.normal;
	manifold->pointCount = kept;
	for ( int i = 0; i < kept; ++i )
	{
		b3LocalManifoldPoint* mp = manifold->points + i;
		mp->point = b3MulSub( in[i].position, 0.5f * in[i].separation, refPlane.normal );
		mp->separation = in[i].separation;
		mp->pair = in[i].pair;
	}
}

// The planes through a hull face's edges, facing out of the face.
static int b3GetHullFaceSidePlanes( b3Plane* planes, int capacity, const b3HullData* hull, int faceIndex )
{
	const b3HullFace* faces = b3GetHullFaces( hull );
	const b3HullHalfEdge* edges = b3GetHullEdges( hull );
	const b3Vec3* points = b3GetHullPoints( hull );
	b3Vec3 normal = b3GetHullPlanes( hull )[faceIndex].normal;
	const b3HullFace* face = faces + faceIndex;

	int count = 0;
	int edgeIndex = face->edge;
	do
	{
		const b3HullHalfEdge* edge = edges + edgeIndex;
		const b3HullHalfEdge* next = edges + edge->next;
		b3Vec3 v1 = points[edge->origin];
		b3Vec3 v2 = points[next->origin];
		b3Vec3 binormal = b3Normalize( b3Cross( b3Sub( v2, v1 ), normal ) );
		if ( count < capacity )
		{
			planes[count++] = b3MakePlaneFromNormalAndPoint( binormal, v1 );
		}
		edgeIndex = edge->next;
	}
	while ( edgeIndex != face->edge );

	return count;
}

void b3CollideHullAndCylinder( b3LocalManifold* manifold, int capacity, const b3HullData* hullA, const b3Cylinder* cylinderB,
							   const b3HullData* standInB, b3Transform transformBtoA, b3SATCache* cache )
{
	manifold->pointCount = 0;
	if ( capacity < 1 )
	{
		return;
	}

	b3CylinderCore core = b3MakeCylinderCore( cylinderB );
	b3PointCloud cloudA = { b3GetHullPoints( hullA ), hullA->vertexCount };
	b3DistanceOutput output =
		b3SupportDistance( b3PointCloudSupport, &cloudA, b3CylinderCoreSupport, &core, transformBtoA );
	float separation = output.distance - core.margin;
	if ( separation > B3_SPECULATIVE_DISTANCE )
	{
		return;
	}

	if ( output.distance < 100.0f * FLT_EPSILON )
	{
		// The cores overlap: Box3D's hulls answer, the stand-in in the cylinder's place.
		b3CollideHulls( manifold, capacity, hullA, standInB, transformBtoA, cache );
		return;
	}

	b3Vec3 normal = output.normal;
	int refFace = b3FindHullSupportFace( hullA, normal );
	b3Plane refPlane = b3GetHullPlanes( hullA )[refFace];
	if ( b3Dot( refPlane.normal, normal ) > 0.998f )
	{
		// On a face: the cylinder's side line or cap, clipped to the face.
		b3ClipVertex feature[B3_CYLINDER_CAP_POINTS];
		b3Vec3 towardA = b3InvRotateVector( transformBtoA.q, b3Neg( refPlane.normal ) );
		int count =
			b3CylinderFeatureVertices( feature, cylinderB, towardA, transformBtoA, b3_featureShapeB, b3_featureShapeA, refFace );
		b3Plane sides[B3_MAX_CLIP_POINTS];
		int sideCount = b3GetHullFaceSidePlanes( sides, B3_MAX_CLIP_POINTS - B3_CYLINDER_CAP_POINTS, hullA, refFace );
		b3BuildCylinderFaceContact( manifold, capacity, feature, count, refPlane, sides, sideCount, B3_SPECULATIVE_DISTANCE );
		if ( manifold->pointCount > 0 )
		{
			return;
		}
	}

	// An edge or a corner of the hull against the cylinder: one point.
	b3Vec3 surfaceB = b3MulSub( output.pointB, core.margin, normal );
	b3SinglePoint( manifold, normal, output.pointA, surfaceB, separation );
}
```

Declare it in `include/box3d/collision.h` (patch), with the other cylinder pairs:

```c
/// Collide a hull and a cylinder. standInB is the cylinder's stand-in hull, for deep overlap. Anarchy Engine.
B3_API void b3CollideHullAndCylinder( b3LocalManifold* manifold, int capacity, const b3HullData* hullA,
									  const b3Cylinder* cylinderB, const b3HullData* standInB, b3Transform transformBtoA,
									  b3SATCache* cache );
```

- [ ] **Step 4: Dispatch hull against cylinder (`src/contact.c`, patch)**

Register the pair, after the Task 3 lines:

```c
		b3AddType( b3_hullShape, b3_cylinderShape );
```

Widen the assertion in `b3CreateContact`:

```c
	B3_ASSERT( shapeB->type == b3_sphereShape || shapeB->type == b3_capsuleShape || shapeB->type == b3_hullShape ||
			   shapeB->type == b3_cylinderShape );
```

In `b3ComputeConvexManifold`'s hull branch, before its final `else` (hull against hull):

```c
		else if ( typeB == b3_cylinderShape )
		{
			b3CollideHullAndCylinder( &geomManifold, pointCapacity, shapeA->hull, &shapeB->cylinder, shapeB->cylinderHull,
									  transformBtoA, &cache->satCache );
		}
```

Regenerate the patch and check it reverses.

- [ ] **Step 5: Run the tests to see them pass**

Run: build, then `./build/Release/sandbox.exe "[cylinder]"`.
Expected: `All tests passed (… in 24 test cases)`. If CY17's height range fails, print `low`/`high` per step (`INFO` already shows them). Compare against CY14's IDs: a range near Box3D's linear slop means the IDs are changing between steps. Check that `b3CylinderFeatureVertices` gets the side line, not the cap.

- [ ] **Step 6: Commit**

```bash
git add cmake/box3d/cylinder.c cmake/box3d/cylinder.patch sandbox/cylinder_shape_tests.cpp sandbox/cylinder_world_tests.cpp
git commit -m "Box3D: a cylinder against a hull rolls on two named points, rests on its cap, and settles like a coin

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Cylinder against a mesh triangle (terrain, anchored Customs)

**Files:**
- Modify: `cmake/box3d/cylinder.c`, `sandbox/cylinder_shape_tests.cpp`, `sandbox/cylinder_world_tests.cpp`
- Patch: `include/box3d/collision.h`, `src/contact.c`, `src/mesh_contact.c`

**Interfaces:**
- Consumes: Task 4's `b3CylinderFeatureVertices`, `b3BuildCylinderFaceContact`; Task 3's core and supports.
- Produces: `void b3CollideTriangleAndCylinder( b3LocalManifold* manifold, int capacity, const b3Vec3* triangleA, int triangleFlags, const b3Cylinder* cylinderB, const b3HullData* standInB, b3SATCache* cache, bool enableSpeculative );`. The triangle is in the cylinder's frame, and the normal points from triangle to cylinder.

- [ ] **Step 1: Write the failing tests**

Append to `sandbox/cylinder_shape_tests.cpp`:

```cpp
TEST_CASE("CY23 a cylinder on its side on one triangle: two points, the triangle's face", "[cylinder]") {
    // The triangle, counter-clockwise from above, in the world.
    const b3Vec3 world[3] = {v3(-5.f, 0.f, 5.f), v3(5.f, 0.f, 5.f), v3(0.f, 0.f, -5.f)};
    // The cylinder lies along Z, 0.005 into the triangle; Box3D gives the triangle in its frame.
    const b3Cylinder wheel = upright(0.5f, 0.25f);
    b3HullData* standIn = b3MakeCylinderHull(&wheel);
    const b3Transform xf{v3(0.f, 0.495f, 0.f), b3MakeQuatFromAxisAngle(v3(1.f, 0.f, 0.f), 0.5f * kPi)};
    b3Vec3 local[3];
    for (int i = 0; i < 3; ++i) {
        local[i] = b3InvTransformPoint(xf, world[i]);
    }
    b3LocalManifoldPoint points[8];
    b3LocalManifold m{};
    m.points = points;
    b3SATCache cache{};
    b3CollideTriangleAndCylinder(&m, 8, local, 0, &wheel, standIn, &cache, true);
    REQUIRE(m.pointCount == 2);
    REQUIRE(m.feature == b3_featureTriangleFace);
    const b3Vec3 up = b3RotateVector(xf.q, m.normal);
    REQUIRE(near(up.y, 1.f, 1e-5f));
    REQUIRE(near(m.points[0].separation, -0.005f, 1e-5f));
    REQUIRE(near(m.points[1].separation, -0.005f, 1e-5f));
    b3DestroyHull(standIn);
}
```

Append to `sandbox/cylinder_world_tests.cpp`:

```cpp
namespace {

// A static mesh floor at y = 0: cells 1 unit across from -half to +half, two
// triangles each, its edges identified so Box3D smooths over the seams.
b3BodyId mesh_floor(World& world, int half, std::vector<b3Vec3>& vertices, std::vector<int32_t>& indices,
                    b3MeshData*& mesh) {
    const int side = 2 * half + 1;
    for (int z = -half; z <= half; ++z) {
        for (int x = -half; x <= half; ++x) {
            vertices.push_back(v3(static_cast<float>(x), 0.f, static_cast<float>(z)));
        }
    }
    for (int z = 0; z < side - 1; ++z) {
        for (int x = 0; x < side - 1; ++x) {
            const int i = z * side + x;
            // Counter-clockwise seen from above.
            indices.insert(indices.end(), {i, i + side, i + 1, i + 1, i + side, i + side + 1});
        }
    }
    b3MeshDef def{};
    def.vertices = vertices.data();
    def.indices = indices.data();
    def.vertexCount = static_cast<int>(vertices.size());
    def.triangleCount = static_cast<int>(indices.size() / 3);
    def.identifyEdges = true;
    mesh = b3CreateMesh(&def, nullptr, 0);
    const b3BodyId ground = world.body(b3_staticBody, v3(0.f, 0.f, 0.f));
    b3ShapeDef shape = b3DefaultShapeDef();
    b3CreateMeshShape(ground, &shape, mesh, v3(1.f, 1.f, 1.f));
    return ground;
}

}  // namespace

TEST_CASE("CY24 a cylinder rolls across a mesh's triangle seams without a jump", "[cylinder]") {
    std::vector<b3Vec3> vertices;
    std::vector<int32_t> indices;
    b3MeshData* mesh = nullptr;
    {
        World world;
        mesh_floor(world, 15, vertices, indices, mesh);
        REQUIRE(mesh != nullptr);
        // Off the grid lines, so it crosses diagonals and edges both.
        const b3BodyId wheel = world.cylinder(b3_dynamicBody, v3(-10.f, 0.5f, 0.3f), on_side(), 0.5f, 0.25f);
        world.seconds(0.25f);
        b3Body_SetLinearVelocity(wheel, v3(4.f, 0.f, 0.f));
        b3Body_SetAngularVelocity(wheel, v3(0.f, 0.f, -8.f));
        float low = 1e9f;
        float high = -1e9f;
        for (int i = 0; i < 240 * 6 && position_of(wheel).x < 10.f; ++i) {
            world.step();
            low = std::min(low, position_of(wheel).y);
            high = std::max(high, position_of(wheel).y);
        }
        INFO(low << " .. " << high);
        REQUIRE(position_of(wheel).x >= 10.f);
        REQUIRE(high - low < 5e-4f);
    }
    b3DestroyMesh(mesh);
}

TEST_CASE("CY25 a cylinder lying on a mesh does not creep, and sleeps", "[cylinder]") {
    std::vector<b3Vec3> vertices;
    std::vector<int32_t> indices;
    b3MeshData* mesh = nullptr;
    {
        World world;
        mesh_floor(world, 5, vertices, indices, mesh);
        const b3BodyId wheel = world.cylinder(b3_dynamicBody, v3(0.3f, 0.5f, 0.2f), on_side(), 0.5f, 0.25f);
        world.seconds(1.f);
        const b3Vec3 before = position_of(wheel);
        world.seconds(10.f);
        REQUIRE(b3Distance(before, position_of(wheel)) < 1e-4f);
        REQUIRE_FALSE(b3Body_IsAwake(wheel));
    }
    b3DestroyMesh(mesh);
}
```

- [ ] **Step 2: Run them to see them fail**

Run: build.
Expected: FAIL to compile: `b3CollideTriangleAndCylinder` undeclared.

- [ ] **Step 3: The triangle pair (`cylinder.c`)**

Append to `cmake/box3d/cylinder.c`:

```c
// Which part of the triangle the point on it lies on: its face, an edge, or a corner.
static b3TriangleFeature b3TriangleFeatureAt( b3Vec3 v1, b3Vec3 v2, b3Vec3 v3, b3Vec3 point )
{
	b3Vec3 e1 = b3Sub( v2, v1 );
	b3Vec3 e2 = b3Sub( v3, v1 );
	b3Vec3 p = b3Sub( point, v1 );
	float d11 = b3Dot( e1, e1 );
	float d12 = b3Dot( e1, e2 );
	float d22 = b3Dot( e2, e2 );
	float dp1 = b3Dot( p, e1 );
	float dp2 = b3Dot( p, e2 );
	float det = d11 * d22 - d12 * d12;
	if ( det < 1000.0f * FLT_MIN )
	{
		return b3_featureTriangleFace;
	}

	float w2 = ( d22 * dp1 - d12 * dp2 ) / det;
	float w3 = ( d11 * dp2 - d12 * dp1 ) / det;
	float w1 = 1.0f - w2 - w3;
	const float tolerance = 1.0e-4f;
	bool on1 = w1 > tolerance;
	bool on2 = w2 > tolerance;
	bool on3 = w3 > tolerance;
	if ( on1 && on2 && on3 )
	{
		return b3_featureTriangleFace;
	}
	if ( on1 && on2 )
	{
		return b3_featureEdge1;
	}
	if ( on2 && on3 )
	{
		return b3_featureEdge2;
	}
	if ( on3 && on1 )
	{
		return b3_featureEdge3;
	}
	if ( on1 )
	{
		return b3_featureVertex1;
	}
	if ( on2 )
	{
		return b3_featureVertex2;
	}
	return b3_featureVertex3;
}

void b3CollideTriangleAndCylinder( b3LocalManifold* manifold, int capacity, const b3Vec3* triangleA, int triangleFlags,
								   const b3Cylinder* cylinderB, const b3HullData* standInB, b3SATCache* cache,
								   bool enableSpeculative )
{
	manifold->pointCount = 0;
	if ( capacity < 2 )
	{
		return;
	}

	b3Vec3 v1 = triangleA[0];
	b3Vec3 v2 = triangleA[1];
	b3Vec3 v3 = triangleA[2];
	b3Plane plane = b3MakePlaneFromPoints( v1, v2, v3 );
	b3CylinderFrame frame = b3MakeCylinderFrame( cylinderB );
	if ( b3PlaneSeparation( plane, frame.center ) < 0.0f )
	{
		// Cull the back side, as Box3D's capsule does.
		return;
	}

	float maxSeparation = enableSpeculative ? B3_SPECULATIVE_DISTANCE : 0.0f;
	b3CylinderCore core = b3MakeCylinderCore( cylinderB );
	b3PointCloud cloudA = { triangleA, 3 };
	b3DistanceOutput output =
		b3SupportDistance( b3PointCloudSupport, &cloudA, b3CylinderCoreSupport, &core, b3Transform_identity );
	float separation = output.distance - core.margin;
	if ( separation > maxSeparation )
	{
		return;
	}

	if ( output.distance < 100.0f * FLT_EPSILON )
	{
		// The cores overlap: Box3D's triangle and hull answer, the stand-in in the cylinder's place.
		b3CollideTriangleAndHull( manifold, capacity, v1, v2, v3, triangleFlags, standInB, cache, enableSpeculative );
		return;
	}

	b3Vec3 normal = output.normal;
	if ( b3Dot( plane.normal, normal ) > 0.2f )
	{
		// Prefer the face, as Box3D's capsule does: the cylinder's side line or
		// cap, clipped to the triangle.
		b3ClipVertex feature[B3_CYLINDER_CAP_POINTS];
		int count = b3CylinderFeatureVertices( feature, cylinderB, b3Neg( plane.normal ), b3Transform_identity,
											   b3_featureShapeB, b3_featureShapeA, 0 );
		b3Vec3 corners[3] = { v1, v2, v3 };
		b3Plane sides[3];
		for ( int i = 0; i < 3; ++i )
		{
			b3Vec3 a = corners[i];
			b3Vec3 b = corners[( i + 1 ) % 3];
			sides[i] = b3MakePlaneFromNormalAndPoint( b3Normalize( b3Cross( b3Sub( b, a ), plane.normal ) ), a );
		}
		b3BuildCylinderFaceContact( manifold, capacity, feature, count, plane, sides, 3, maxSeparation );
		if ( manifold->pointCount > 0 )
		{
			manifold->feature = b3_featureTriangleFace;
			return;
		}
	}

	b3Vec3 surfaceB = b3MulSub( output.pointB, core.margin, normal );
	b3SinglePoint( manifold, normal, output.pointA, surfaceB, separation );
	manifold->feature = b3TriangleFeatureAt( v1, v2, v3, output.pointA );
}
```

Declare it in `include/box3d/collision.h` (patch), with the other cylinder pairs:

```c
/// Collide a triangle and a cylinder, the triangle in the cylinder's frame. Normal points from triangle to
/// cylinder. standInB is the cylinder's stand-in hull, for deep overlap. Anarchy Engine.
B3_API void b3CollideTriangleAndCylinder( b3LocalManifold* manifold, int capacity, const b3Vec3* triangleA, int triangleFlags,
										  const b3Cylinder* cylinderB, const b3HullData* standInB, b3SATCache* cache,
										  bool enableSpeculative );
```

- [ ] **Step 4: Dispatch mesh against cylinder (patch)**

In `src/contact.c`, register the pair:

```c
		b3AddType( b3_meshShape, b3_cylinderShape );
```

In `src/mesh_contact.c`, in the per-triangle `switch ( shapeB->type )`, beside `case b3_hullShape:`:

```c
			case b3_cylinderShape:
				b3CollideTriangleAndCylinder( manifold, pointCapacity, vertices, triangle.flags, &shapeB->cylinder,
											  shapeB->cylinderHull, &cache->satCache, enableSpeculative );
				break;
```

In the same file's rolling-resistance `radiusB` chain (about line 1172):

```c
	else if ( shapeB->type == b3_cylinderShape )
	{
		radiusB = shapeB->cylinder.radius;
	}
```

Regenerate the patch and check it reverses.

- [ ] **Step 5: Run the tests to see them pass**

Run: build, then `./build/Release/sandbox.exe "[cylinder]"`.
Expected: `All tests passed (… in 27 test cases)`.

- [ ] **Step 6: Commit**

```bash
git add cmake/box3d/cylinder.c cmake/box3d/cylinder.patch sandbox/cylinder_shape_tests.cpp sandbox/cylinder_world_tests.cpp
git commit -m "Box3D: a cylinder against a mesh's triangles, smooth across their seams

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: Cylinder against cylinder: logs and drums

**Files:**
- Modify: `cmake/box3d/cylinder.c`, `sandbox/cylinder_shape_tests.cpp`, `sandbox/cylinder_world_tests.cpp`
- Patch: `include/box3d/collision.h`, `src/contact.c`

**Interfaces:**
- Consumes: everything above.
- Produces: `void b3CollideCylinders( b3LocalManifold* manifold, int capacity, const b3Cylinder* cylinderA, const b3HullData* standInA, const b3Cylinder* cylinderB, const b3HullData* standInB, b3Transform transformBtoA, b3SATCache* cache );`

- [ ] **Step 1: Write the failing tests**

Append to `sandbox/cylinder_shape_tests.cpp`:

```cpp
TEST_CASE("CY26 a log on a log, side by side and parallel: two points along their line", "[cylinder]") {
    // Both along Z, B on top of A, 0.005 into it.
    const b3Cylinder log{v3(0.f, 0.f, -1.f), v3(0.f, 0.f, 1.f), 0.5f};
    b3HullData* standIn = b3MakeCylinderHull(&log);
    b3LocalManifoldPoint points[4];
    b3LocalManifold m{};
    m.points = points;
    b3SATCache cache{};
    b3CollideCylinders(&m, 4, &log, standIn, &log, standIn, b3Transform{v3(0.f, 0.995f, 0.f), b3Quat_identity}, &cache);
    REQUIRE(m.pointCount == 2);
    REQUIRE(near(m.normal.y, 1.f, 1e-5f));
    REQUIRE(near(m.points[0].separation, -0.005f, 1e-5f));
    REQUIRE(near(m.points[1].separation, -0.005f, 1e-5f));
    b3DestroyHull(standIn);
}

TEST_CASE("CY27 a drum on a drum, cap on cap: four points, normal up", "[cylinder]") {
    const b3Cylinder drum = upright(0.5f, 1.f);
    b3HullData* standIn = b3MakeCylinderHull(&drum);
    b3LocalManifoldPoint points[4];
    b3LocalManifold m{};
    m.points = points;
    b3SATCache cache{};
    b3CollideCylinders(&m, 4, &drum, standIn, &drum, standIn, b3Transform{v3(0.1f, 1.995f, 0.f), b3Quat_identity}, &cache);
    REQUIRE(m.pointCount == 4);
    REQUIRE(near(m.normal.y, 1.f, 1e-5f));
    for (int i = 0; i < 4; ++i) {
        REQUIRE(near(m.points[i].separation, -0.005f, 1e-5f));
    }
    b3DestroyHull(standIn);
}
```

Append to `sandbox/cylinder_world_tests.cpp`:

```cpp
TEST_CASE("CY28 drums stacked three high and a pyramid of three logs hold for 10 seconds", "[cylinder]") {
    World world;
    world.floor(1.f);
    // Three drums, each radius 0.5 and 1 tall, cap on cap.
    std::vector<b3BodyId> drums;
    for (int i = 0; i < 3; ++i) {
        drums.push_back(world.cylinder(b3_dynamicBody, v3(0.f, 0.5f + static_cast<float>(i), 0.f), b3Quat_identity, 0.5f,
                                       0.5f, 1.f));
    }
    // A pyramid of logs along Z at x = 5: two side by side, one in the groove above.
    const b3BodyId left = world.cylinder(b3_dynamicBody, v3(4.5f, 0.5f, 0.f), on_side(), 0.5f, 1.f, 1.f);
    const b3BodyId right = world.cylinder(b3_dynamicBody, v3(5.5f, 0.5f, 0.f), on_side(), 0.5f, 1.f, 1.f);
    const b3BodyId top = world.cylinder(b3_dynamicBody, v3(5.f, 0.5f + std::sqrt(0.75f), 0.f), on_side(), 0.5f, 1.f, 1.f);
    world.seconds(1.f);
    const b3Vec3 drumBefore = position_of(drums[2]);
    const float topBefore = position_of(top).y;
    world.seconds(10.f);
    REQUIRE(b3Distance(drumBefore, position_of(drums[2])) < 1e-3f);
    REQUIRE(topBefore - position_of(top).y < 0.01f);
    REQUIRE(std::fabs(position_of(left).x - 4.5f) < 0.05f);
    REQUIRE(std::fabs(position_of(right).x - 5.5f) < 0.05f);
}
```

- [ ] **Step 2: Run them to see them fail**

Run: build.
Expected: FAIL to compile: `b3CollideCylinders` undeclared.

- [ ] **Step 3: The cylinder pair (`cylinder.c`)**

Append to `cmake/box3d/cylinder.c`:

```c
void b3CollideCylinders( b3LocalManifold* manifold, int capacity, const b3Cylinder* cylinderA, const b3HullData* standInA,
						 const b3Cylinder* cylinderB, const b3HullData* standInB, b3Transform transformBtoA,
						 b3SATCache* cache )
{
	manifold->pointCount = 0;
	if ( capacity < 2 )
	{
		return;
	}

	b3CylinderCore coreA = b3MakeCylinderCore( cylinderA );
	b3CylinderCore coreB = b3MakeCylinderCore( cylinderB );
	b3DistanceOutput output =
		b3SupportDistance( b3CylinderCoreSupport, &coreA, b3CylinderCoreSupport, &coreB, transformBtoA );
	float separation = output.distance - coreA.margin - coreB.margin;
	float speculative = B3_SPECULATIVE_DISTANCE;
	if ( separation > speculative )
	{
		return;
	}

	if ( output.distance < 100.0f * FLT_EPSILON )
	{
		// The cores overlap: Box3D's hulls answer, the stand-ins in the cylinders' places.
		b3CollideHulls( manifold, capacity, standInA, standInB, transformBtoA, cache );
		return;
	}

	b3Vec3 normal = output.normal;
	b3CylinderFrame frameA = b3MakeCylinderFrame( cylinderA );
	b3Cylinder placedB = { b3TransformPoint( transformBtoA, cylinderB->center1 ),
						   b3TransformPoint( transformBtoA, cylinderB->center2 ), cylinderB->radius };
	b3CylinderFrame frameB = b3MakeCylinderFrame( &placedB );
	float aAlong = b3Dot( normal, frameA.axis );
	float bAlong = b3Dot( normal, frameB.axis );
	float axesDot = b3Dot( frameA.axis, frameB.axis );

	if ( b3AbsFloat( aAlong ) < b3_cylinderSideSin && b3AbsFloat( bAlong ) < b3_cylinderSideSin && b3AbsFloat( axesDot ) > 0.998f )
	{
		// Side by side and parallel: a line contact where they overlap along A's axis.
		b3Vec3 radial = b3Normalize( b3MulSub( normal, aAlong, frameA.axis ) );
		b3Vec3 b1 = b3MulSub( placedB.center1, frameB.radius, radial );
		b3Vec3 b2 = b3MulSub( placedB.center2, frameB.radius, radial );
		float t1 = b3Dot( b3Sub( b1, frameA.center ), frameA.axis );
		float t2 = b3Dot( b3Sub( b2, frameA.center ), frameA.axis );
		float lo = b3ClampFloat( b3MinFloat( t1, t2 ), -frameA.halfHeight, frameA.halfHeight );
		float hi = b3ClampFloat( b3MaxFloat( t1, t2 ), -frameA.halfHeight, frameA.halfHeight );
		if ( hi - lo > B3_LINEAR_SLOP )
		{
			float ends[2] = { lo, hi };
			int count = 0;
			for ( int i = 0; i < 2; ++i )
			{
				b3Vec3 onB = b3Lerp( b1, b2, ( ends[i] - t1 ) / ( t2 - t1 ) );
				b3Vec3 onA = b3MulAdd( b3MulAdd( frameA.center, ends[i], frameA.axis ), frameA.radius, radial );
				float pointSeparation = b3Dot( b3Sub( onB, onA ), radial );
				if ( pointSeparation > speculative )
				{
					continue;
				}
				b3LocalManifoldPoint* mp = manifold->points + count;
				mp->point = b3Lerp( onA, onB, 0.5f );
				mp->separation = pointSeparation;
				mp->pair = b3MakeFeaturePair( b3_featureShapeA, i, b3_featureShapeB, i );
				count += 1;
			}
			if ( count > 0 )
			{
				manifold->normal = radial;
				manifold->pointCount = count;
				return;
			}
		}
	}

	if ( b3AbsFloat( aAlong ) >= b3_cylinderCapCos )
	{
		// A's cap faces B: B's side line or cap, kept over A's cap.
		float sign = aAlong > 0.0f ? 1.0f : -1.0f;
		b3Vec3 capNormal = b3MulSV( sign, frameA.axis );
		b3Vec3 capCenter = b3MulAdd( frameA.center, sign * frameA.halfHeight, frameA.axis );
		b3ClipVertex feature[B3_CYLINDER_CAP_POINTS];
		b3Vec3 towardA = b3InvRotateVector( transformBtoA.q, b3Neg( capNormal ) );
		int count = b3CylinderFeatureVertices( feature, cylinderB, towardA, transformBtoA, b3_featureShapeB,
											   b3_featureShapeA, sign > 0.0f ? 1 : 0 );
		b3ClipVertex kept[B3_CYLINDER_CAP_POINTS];
		int keptCount = 0;
		if ( count == 2 )
		{
			float s0, s1;
			if ( b3ClipSegmentToDisc( &s0, &s1, feature[0].position, feature[1].position, capCenter, capNormal, frameA.radius ) )
			{
				kept[0] = feature[0];
				kept[1] = feature[1];
				kept[0].position = b3Lerp( feature[0].position, feature[1].position, s0 );
				kept[1].position = b3Lerp( feature[0].position, feature[1].position, s1 );
				keptCount = 2;
			}
		}
		else
		{
			for ( int i = 0; i < count; ++i )
			{
				b3Vec3 offset = b3Sub( feature[i].position, capCenter );
				b3Vec3 across = b3MulSub( offset, b3Dot( offset, capNormal ), capNormal );
				if ( b3Dot( across, across ) <= frameA.radius * frameA.radius )
				{
					kept[keptCount++] = feature[i];
				}
			}
		}

		int written = 0;
		for ( int i = 0; i < keptCount; ++i )
		{
			float pointSeparation = b3Dot( b3Sub( kept[i].position, capCenter ), capNormal );
			if ( pointSeparation <= speculative )
			{
				kept[written] = kept[i];
				kept[written].separation = pointSeparation;
				written += 1;
			}
		}

		written = b3ReducePoints( kept, written, capacity, capNormal );
		if ( written > 0 )
		{
			manifold->normal = capNormal;
			manifold->pointCount = written;
			for ( int i = 0; i < written; ++i )
			{
				b3LocalManifoldPoint* mp = manifold->points + i;
				mp->point = b3MulSub( kept[i].position, 0.5f * kept[i].separation, capNormal );
				mp->separation = kept[i].separation;
				mp->pair = kept[i].pair;
			}
			return;
		}
	}

	if ( b3AbsFloat( bAlong ) >= b3_cylinderCapCos && b3AbsFloat( aAlong ) < b3_cylinderSideSin )
	{
		// B's cap faces A's side: A's side line, kept over B's cap.
		float signB = bAlong < 0.0f ? 1.0f : -1.0f;
		b3Vec3 capOut = b3MulSV( signB, frameB.axis );
		b3Vec3 capCenter = b3MulAdd( frameB.center, signB * frameB.halfHeight, frameB.axis );
		b3Vec3 radial = b3Normalize( b3MulSub( normal, aAlong, frameA.axis ) );
		b3Vec3 a1 = b3MulAdd( b3MulSub( frameA.center, frameA.halfHeight, frameA.axis ), frameA.radius, radial );
		b3Vec3 a2 = b3MulAdd( b3MulAdd( frameA.center, frameA.halfHeight, frameA.axis ), frameA.radius, radial );
		float s0, s1;
		if ( b3ClipSegmentToDisc( &s0, &s1, a1, a2, capCenter, capOut, frameB.radius ) )
		{
			float fractions[2] = { s0, s1 };
			int count = 0;
			for ( int i = 0; i < 2; ++i )
			{
				b3Vec3 onA = b3Lerp( a1, a2, fractions[i] );
				float pointSeparation = b3Dot( b3Sub( onA, capCenter ), capOut );
				if ( pointSeparation > speculative )
				{
					continue;
				}
				b3LocalManifoldPoint* mp = manifold->points + count;
				mp->point = b3MulSub( onA, 0.5f * pointSeparation, capOut );
				mp->separation = pointSeparation;
				mp->pair = b3MakeFeaturePair( b3_featureShapeA, i, b3_featureShapeB, signB > 0.0f ? 35 : 34 );
				count += 1;
			}
			if ( count > 0 )
			{
				manifold->normal = b3Neg( capOut );
				manifold->pointCount = count;
				return;
			}
		}
	}

	// Otherwise one point, the true surfaces' nearest.
	b3Vec3 surfaceA = b3MulAdd( output.pointA, coreA.margin, normal );
	b3Vec3 surfaceB = b3MulSub( output.pointB, coreB.margin, normal );
	b3SinglePoint( manifold, normal, surfaceA, surfaceB, separation );
}
```

Declare it in `include/box3d/collision.h` (patch), with the other cylinder pairs:

```c
/// Collide two cylinders. The stand-in hulls answer for deep overlap. Anarchy Engine.
B3_API void b3CollideCylinders( b3LocalManifold* manifold, int capacity, const b3Cylinder* cylinderA,
								const b3HullData* standInA, const b3Cylinder* cylinderB, const b3HullData* standInB,
								b3Transform transformBtoA, b3SATCache* cache );
```

- [ ] **Step 4: Dispatch cylinder against cylinder (`src/contact.c`, patch)**

Register:

```c
		b3AddType( b3_cylinderShape, b3_cylinderShape );
```

In `b3ComputeConvexManifold`'s cylinder branch (Task 3), turn its `else` into:

```c
		else if ( typeB == b3_capsuleShape )
		{
			b3CollideCylinderAndCapsule( &geomManifold, pointCapacity, &shapeA->cylinder, shapeA->cylinderHull,
										 &shapeB->capsule, transformBtoA, &cache->simplexCache );
		}
		else
		{
			B3_ASSERT( typeB == b3_cylinderShape );
			b3CollideCylinders( &geomManifold, pointCapacity, &shapeA->cylinder, shapeA->cylinderHull, &shapeB->cylinder,
								shapeB->cylinderHull, transformBtoA, &cache->satCache );
		}
```

Regenerate the patch and check it reverses.

- [ ] **Step 5: Run the tests to see them pass**

Run: build, then `./build/Release/sandbox.exe "[cylinder]"`.
Expected: `All tests passed (… in 30 test cases)`.

- [ ] **Step 6: Commit**

```bash
git add cmake/box3d/cylinder.c cmake/box3d/cylinder.patch sandbox/cylinder_shape_tests.cpp sandbox/cylinder_world_tests.cpp
git commit -m "Box3D: cylinders against cylinders, so logs stack and drums stand on drums

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: The engine makes a true cylinder

**Files:**
- Modify:
  - `src/engine_core/PhysicsWorld.cpp`: the constants near line 55; the `PhysicsObject::Shape::Cylinder` case of `make_object_shape`, near line 3850; the `Cylinder` case of `collision_outline`, near line 4586;
  - `src/engine_instances/PhysicsObject.hpp`: the Shape doc comment;
  - `sandbox/physics_tests.cpp`: P52 and new tests.

**Interfaces:**
- Consumes: `b3Cylinder`, `b3CreateCylinderShape`, `b3ComputeCylinderMass` (Box3D, Tasks 1–2).
- Produces: nothing new for other tasks. A Cylinder PhysicsObject now has one shape (`PhysicsWorld::shape_count == 1`), except when elliptical or tiny.

- [ ] **Step 1: Write the failing tests**

In `sandbox/physics_tests.cpp`, change P52's last checks to the true cylinder's:

```cpp
    INFO(low << " .. " << high);
    // A true cylinder (b3_cylinderShape): level to a tenth of a millimetre.
    REQUIRE(high - low < 1e-4f);
    REQUIRE(z_of(wheel.transform()) > -6.f);
```

Append:

```cpp
TEST_CASE("P53 a Cylinder is one true cylinder: it weighs its Mass, and a ray meets its round side",
          "[physics][cylinder]") {
    PhysicsRig rig;
    // A log lying along X, anchored: radius 0.5, 2 long.
    const Matrix4 lying = engine_core::matrix4_multiply(
        at(0.f, 0.f, 0.f), engine_core::matrix4_axis_angle(Vec3{0.f, 0.f, 1.f}, 3.14159265358979 / 2.0));
    PhysicsObject& log = rig.body(lying, Vec3{1.f, 2.f, 1.f}, true);
    REQUIRE_FALSE(log.set_shape(static_cast<int>(PhysicsObject::Shape::Cylinder)));
    PhysicsObject& wheel = rig.body(at(5.f, 5.f, 0.f), Vec3{1.f, 0.5f, 1.f}, false);
    REQUIRE_FALSE(wheel.set_shape(static_cast<int>(PhysicsObject::Shape::Cylinder)));
    REQUIRE_FALSE(wheel.set_mass(7.0));
    rig.play();
    rig.steps(1);
    REQUIRE(rig.physics.shape_count(log.id()) == 1);
    REQUIRE(rig.physics.shape_count(wheel.id()) == 1);
    REQUIRE(near(rig.physics.body_mass(wheel.id()), 7.f, 1e-3f));
    // Straight down at z = 0.1: the round side, not a polygon's flat.
    const auto hit = rig.physics.raycast(rig.game, Vec3{0.3f, 3.f, 0.1f}, Vec3{0.f, -5.f, 0.f}, engine_core::RayFilter{});
    REQUIRE(hit);
    REQUIRE(hit->instance == log.id());
    REQUIRE(near(hit->position.y, std::sqrt(0.25f - 0.01f), 1e-5f));
}

TEST_CASE("P54 a Cylinder scaled unevenly across X and Z stays layered hulls; evenly, one cylinder again",
          "[physics][cylinder]") {
    PhysicsRig rig;
    GameObject& part = create_part(rig.game);
    Matrix4 stretched = at(0.f, 5.f, 0.f);
    stretched.m[0] = 2.f;  // X twice Z: an ellipse across
    part.set_transform(stretched);
    PhysicsObject& body = rig.body(engine_core::matrix4_identity(), Vec3{1.f, 1.f, 1.f}, false, part.id());
    REQUIRE_FALSE(body.set_shape(static_cast<int>(PhysicsObject::Shape::Cylinder)));
    rig.sync_steps(1);
    REQUIRE(rig.physics.shape_count(body.id()) == 4);
    stretched.m[10] = 2.f;  // Z too: round again
    part.set_transform(stretched);
    rig.sync_steps(1);
    REQUIRE(rig.physics.shape_count(body.id()) == 1);
}

TEST_CASE("P55 a PlayerController stands and walks along a lying Cylinder", "[physics][cylinder]") {
    PhysicsRig rig;
    // A log, radius 1, lying along X, anchored, its top at y = 1.
    const Matrix4 lying = engine_core::matrix4_multiply(
        at(0.f, 0.f, 0.f), engine_core::matrix4_axis_angle(Vec3{0.f, 0.f, 1.f}, 3.14159265358979 / 2.0));
    PhysicsObject& log = rig.body(lying, Vec3{2.f, 20.f, 2.f}, true);
    REQUIRE_FALSE(log.set_shape(static_cast<int>(PhysicsObject::Shape::Cylinder)));
    PlayerController& player = rig.controller(at(-3.f, 1.5f, 0.f));
    REQUIRE_FALSE(player.set_friction(0.0));
    rig.play();
    rig.seconds(1.0);
    REQUIRE(player.on_ground());
    REQUIRE(near(y_of(player.transform()), 1.f, 0.05f));
    REQUIRE_FALSE(player.set_velocity(Vec3{3.f, 0.f, 0.f}));
    rig.seconds(1.0);
    REQUIRE(x_of(player.transform()) > -1.f);
    REQUIRE(player.on_ground());
}

TEST_CASE("P56 a CanCollide-off Cylinder is a sensor", "[physics][cylinder]") {
    PhysicsRig rig;
    PhysicsObject& zone = rig.body(at(0.f, 0.f, 0.f), Vec3{2.f, 2.f, 2.f}, true);
    REQUIRE_FALSE(zone.set_shape(static_cast<int>(PhysicsObject::Shape::Cylinder)));
    REQUIRE_FALSE(zone.set_can_collide(false));
    rig.sync_steps(1);
    REQUIRE(rig.physics.shape_count(zone.id()) == 1);
    REQUIRE(rig.physics.shapes_are_sensors(zone.id()));
}

TEST_CASE("P57 a fast Cylinder does not tunnel through a thin wall", "[physics][cylinder]") {
    PhysicsRig rig;
    rig.body(at(5.f, 0.f, 0.f), Vec3{0.05f, 4.f, 4.f}, true);
    PhysicsObject& slug = rig.body(at(0.f, 0.f, 0.f), Vec3{0.4f, 0.4f, 0.4f}, false);
    REQUIRE_FALSE(slug.set_shape(static_cast<int>(PhysicsObject::Shape::Cylinder)));
    REQUIRE_FALSE(slug.set_velocity(Vec3{300.f, 0.f, 0.f}));
    rig.play();
    rig.seconds(0.2);
    REQUIRE(x_of(slug.transform()) < 5.f);
}

TEST_CASE("P58 a tiny Cylinder, or one under a GameObject scaled to almost nothing, does not crash",
          "[physics][cylinder]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& tiny = rig.body(at(0.f, 1.f, 0.f), Vec3{0.01f, 0.01f, 0.01f}, false);
    REQUIRE_FALSE(tiny.set_shape(static_cast<int>(PhysicsObject::Shape::Cylinder)));
    GameObject& part = create_part(rig.game);
    Matrix4 shrunk = at(2.f, 1.f, 0.f);
    shrunk.m[0] = shrunk.m[5] = shrunk.m[10] = 0.001f;
    part.set_transform(shrunk);
    PhysicsObject& speck = rig.body(engine_core::matrix4_identity(), Vec3{1.f, 1.f, 1.f}, false, part.id());
    REQUIRE_FALSE(speck.set_shape(static_cast<int>(PhysicsObject::Shape::Cylinder)));
    rig.play();
    rig.seconds(1.0);
    REQUIRE(rig.physics.shape_count(tiny.id()) >= 1);
    REQUIRE(rig.physics.shape_count(speck.id()) >= 1);
    REQUIRE(std::isfinite(y_of(tiny.transform())));
    REQUIRE(std::isfinite(y_of(speck.transform())));
    REQUIRE(y_of(tiny.transform()) < 1.f);
}
```

Check the `PhysicsRig`, `GameObject`, `PlayerController` names these use against `sandbox/physics_rig.hpp` and the top of `physics_tests.cpp`. Add `#include "PlayerController.hpp"` and `#include "GameObject.hpp"` if missing, and a `using engine_core::GameObject;` / `using engine_core::PlayerController;` if the file's other tests do not already bring them in.

- [ ] **Step 2: Run them to see them fail**

Run: build, then `./build/Release/sandbox.exe "[cylinder]" P52*`.
Expected: P52 FAILS (the layered hulls' range is several mm). P53 fails, `shape_count == 4`. P54's second check fails. P58 may pass already.

- [ ] **Step 3: Make a true cylinder (`PhysicsWorld.cpp`)**

Replace the constants' comment near line 55:

```cpp
// The sides around a Cone, and around an elliptical Cylinder (scaled unevenly
// across X and Z), as Box3D builds no round hull. A prism of n sides has 3n
// edges, and a hull keeps at most 128.
constexpr int kRoundSides = 16;
// An elliptical Cylinder PhysicsObject's shape: this many such hulls, each
// turned a further share of a side about its axis, so together they roll as
// one of kCylinderLayers * kRoundSides sides. A round one is a true cylinder
// (b3_cylinderShape, cmake/box3d/cylinder.c).
constexpr int kCylinderLayers = 4;
```

In `make_object_shape`, replace the whole `case PhysicsObject::Shape::Cylinder: { ... }` block with:

```cpp
        case PhysicsObject::Shape::Cylinder: {
            // A true cylinder (b3_cylinderShape), round across X and Z. Scaled
            // unevenly across those it would be elliptical, which one cannot
            // be, and below Box3D's linear slop Box3D refuses one: layered
            // hulls then, as before.
            const float radius = size.x * 0.5f;
            const bool round = std::fabs(record.scale.x - record.scale.z) <=
                               1e-4f * std::max({std::fabs(record.scale.x), std::fabs(record.scale.z), 1.f});
            if (round && radius >= B3_LINEAR_SLOP && size.y >= B3_LINEAR_SLOP) {
                const float half = size.y * 0.5f;
                const b3Cylinder cylinder{placed_point(record, b3Vec3{center.x, center.y - half, center.z}),
                                          placed_point(record, b3Vec3{center.x, center.y + half, center.z}), radius};
                record.volume = b3ComputeCylinderMass(&cylinder, 1.f).mass;
                def.density = shape_density(object, record);
                add_shape(record, b3CreateCylinderShape(record.body, &def, &cylinder));
                break;
            }
            // Layered hulls (kCylinderLayers), sharing Mass by their summed volume.
            primitive_points(object.shape(), size, record.center, points);
            b3HullData* layers[kCylinderLayers] = {};
            float volume = 0.f;
            for (int layer = 0; layer < kCylinderLayers; ++layer) {
                const float angle = 2.f * kPi / static_cast<float>(kRoundSides) * static_cast<float>(layer) /
                                    static_cast<float>(kCylinderLayers);
                const float c = std::cos(angle);
                const float s = std::sin(angle);
                turned_points.clear();
                for (const b3Vec3& p : points) {
                    const float x = p.x - record.center.x;
                    const float z = p.z - record.center.z;
                    turned_points.push_back(b3Vec3{record.center.x + x * c - z * s, p.y, record.center.z + x * s + z * c});
                }
                layers[layer] = build_hull(turned_points);
                if (layers[layer] != nullptr) {
                    volume += b3ComputeHullMass(layers[layer], 1.f).mass;
                }
            }
            record.volume = volume;
            def.density = shape_density(object, record);
            for (b3HullData* hull : layers) {
                if (hull != nullptr) {
                    add_shape(record, create_hull(record, def, hull));
                    b3DestroyHull(hull);
                }
            }
            break;
        }
```

(The layered branch is the old code, unchanged.) Check that `<algorithm>` is included for the initializer-list `std::max`; it is used elsewhere in the file.

In `collision_outline`, split `Cylinder` from the `Cone`/`Wedge` case. Before `case PhysicsObject::Shape::Cylinder:` (keep that label in the hull case below for the elliptical fall-through), insert a round branch. The outline's existing `x`, `y`, `z` direction vectors, `size`, `center`, `scale`, `add_arc`, and `add_line` are the ones the Capsule case uses:

```cpp
    case PhysicsObject::Shape::Cylinder:
        if (std::fabs(scale.x - scale.z) <= 1e-4f * std::max({std::fabs(scale.x), std::fabs(scale.z), 1.f})) {
            // As make_shape builds it: a true cylinder, two rims and four side lines along Y.
            const float radius = size.x * 0.5f;
            const float half = size.y * 0.5f;
            const Vec3 top{center.x, center.y + half, center.z};
            const Vec3 bottom{center.x, center.y - half, center.z};
            add_arc(lines, top, x, z, radius, 0.f, 2.f * kPi);
            add_arc(lines, bottom, x, z, radius, 0.f, 2.f * kPi);
            for (const Vec3& side : {Vec3{radius, 0.f, 0.f}, Vec3{-radius, 0.f, 0.f}, Vec3{0.f, 0.f, radius},
                                     Vec3{0.f, 0.f, -radius}}) {
                add_line(lines, Vec3{bottom.x + side.x, bottom.y, bottom.z + side.z},
                         Vec3{top.x + side.x, top.y, top.z + side.z});
            }
            return;
        }
        [[fallthrough]];
    case PhysicsObject::Shape::Cone:
    case PhysicsObject::Shape::Wedge: {
```

(and delete the old `case PhysicsObject::Shape::Cylinder:` label that sat with Cone and Wedge). If `size` in `collision_outline` is not yet multiplied by `scale`, use the same expression the Capsule case uses for its radius and half height.

In `src/engine_instances/PhysicsObject.hpp`, change the Shape doc's Cylinder sentence to:

```
//                            height Y, along Y. Cylinder: diameter X,
//                            height Y, along Y; it collides as a true
//                            cylinder, or as layered hulls when scaled
//                            unevenly across X and Z. Cone: base diameter X at
```

- [ ] **Step 4: Run them to see them pass**

Run: build, then `./build/Release/sandbox.exe "[cylinder]" P52* P21* "[physics]"`.
Expected: all pass, the existing `[physics]` tests included.

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/PhysicsWorld.cpp src/engine_instances/PhysicsObject.hpp sandbox/physics_tests.cpp
git commit -m "PhysicsObject: a Cylinder collides as a true cylinder; an elliptical or tiny one stays layered hulls

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 8: Speed, the car, a broken patch, and the whole suite

**Files:**
- Create: `sandbox/cylinder_car_tests.cpp`
- Modify: `sandbox/cylinder_world_tests.cpp`, `CMakeLists.txt` (sandbox list)

**Interfaces:**
- Consumes: everything above.

- [ ] **Step 1: The speed test**

Append to `sandbox/cylinder_world_tests.cpp`:

```cpp
#include <chrono>

namespace {

// 200 cylinders, radius 0.3 and 0.6 tall, dropped into a walled bin; the
// fastest of three runs' time for 600 steps. layered: each as the engine's
// old four turned 16-sided hulls instead of one true cylinder.
double bin_seconds(bool layered) {
    double best = 1e9;
    for (int run = 0; run < 3; ++run) {
        World world;
        world.floor();
        world.box(v3(-4.5f, 2.f, 0.f), v3(0.5f, 2.f, 5.f));
        world.box(v3(4.5f, 2.f, 0.f), v3(0.5f, 2.f, 5.f));
        world.box(v3(0.f, 2.f, -4.5f), v3(5.f, 2.f, 0.5f));
        world.box(v3(0.f, 2.f, 4.5f), v3(5.f, 2.f, 0.5f));
        b3HullData* layers[4] = {};
        if (layered) {
            for (int layer = 0; layer < 4; ++layer) {
                b3HullData* upright = b3CreateCylinder(0.6f, 0.3f, -0.3f, 16);
                const b3Transform turn{v3(0.f, 0.f, 0.f),
                                       b3MakeQuatFromAxisAngle(v3(0.f, 1.f, 0.f), 2.f * kPi / 16.f * layer / 4.f)};
                layers[layer] = b3CloneAndTransformHull(upright, turn, v3(1.f, 1.f, 1.f));
                b3DestroyHull(upright);
            }
        }
        for (int i = 0; i < 200; ++i) {
            const b3Vec3 at = v3(-3.f + 0.75f * static_cast<float>(i % 8), 1.f + 0.8f * static_cast<float>(i / 64),
                                 -3.f + 0.75f * static_cast<float>((i / 8) % 8));
            const b3Quat q = b3MakeQuatFromAxisAngle(b3Normalize(v3(1.f, 0.3f * (i % 3), 0.5f)), 0.4f * static_cast<float>(i));
            if (layered) {
                const b3BodyId b = world.body(b3_dynamicBody, at, q);
                b3ShapeDef def = b3DefaultShapeDef();
                for (b3HullData* hull : layers) {
                    b3CreateHullShape(b, &def, hull);
                }
            } else {
                world.cylinder(b3_dynamicBody, at, q, 0.3f, 0.3f);
            }
        }
        const auto start = std::chrono::steady_clock::now();
        world.step(600);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        best = std::min(best, seconds);
        for (b3HullData* hull : layers) {
            if (hull != nullptr) {
                b3DestroyHull(hull);
            }
        }
    }
    return best;
}

}  // namespace

TEST_CASE("CY29 200 tumbling cylinders cost no more than the layered hulls they replace", "[cylinder][.speed]") {
    const double layered = bin_seconds(true);
    const double cylinders = bin_seconds(false);
    INFO("layered " << layered << " s, cylinders " << cylinders << " s");
    CHECK(cylinders <= layered);
}
```

Run with nothing else building or testing: `./build/Release/sandbox.exe "[.speed]"`.
Expected: PASS, with the times in the output. If it fails, profile the manifolds before relaxing the bar (likely `b3SupportDistance` iteration counts: print `output.iterations`). If a relaxed bar is still needed, record it as a ruling in the ledger with both times.

- [ ] **Step 2: The car**

Create `sandbox/cylinder_car_tests.cpp`. It drives the H1Z1 car straight on cylinder wheels and is skipped where the project is not on this machine:

```cpp
// The H1Z1 car (spec test 15), on true cylinder wheels: driven straight for
// about 120 units, it drifts under 0.5 units, and its body rolls under 0.05
// degrees RMS. Hidden ([.car]): it needs the project on this machine.

#include "physics_rig.hpp"

#include "JointConstraint.hpp"
#include "Project.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using namespace physics_rig;

TEST_CASE("CY30 the H1Z1 car on cylinder wheels drives straight and level", "[cylinder][.car]") {
    const std::filesystem::path project = "C:/Users/Andrew/Documents/Anarchy Engine Projects/H1Z1 Z1 Human";
    if (!std::filesystem::exists(project)) {
        SKIP("the H1Z1 project is not on this machine");
    }
    TempDir dir;
    std::filesystem::copy(project, dir.path, std::filesystem::copy_options::recursive);
    PhysicsRig rig;
    engine_core::Project loaded = engine_core::Project::load(dir.path, rig.game);
    for (const InstanceId child : rig.game.get_children(workspace_of(rig.game))) {
        if (rig.game.name(child) != "Car") {
            rig.game.destroy(child);
        }
    }
    const InstanceId car = rig.game.find_first_child(workspace_of(rig.game), "Car");
    REQUIRE(car != 0);
    auto* chassis = dynamic_cast<engine_core::PhysicsBase*>(rig.game.instance(rig.game.find_first_child(car, "Chassis")));
    REQUIRE(chassis != nullptr);
    std::vector<engine_core::JointConstraint*> axles;
    for (const InstanceId child : rig.game.get_children(car)) {
        if (auto* axle = dynamic_cast<engine_core::JointConstraint*>(rig.game.instance(child))) {
            axles.push_back(axle);
        }
        if (rig.game.name(child).find("Wheel") != std::string::npos) {
            auto* collider = dynamic_cast<PhysicsObject*>(rig.game.instance(rig.game.find_first_child(child, "Collider")));
            REQUIRE(collider != nullptr);
            REQUIRE_FALSE(collider->set_shape(static_cast<int>(PhysicsObject::Shape::Cylinder)));
            REQUIRE_FALSE(collider->set_size(Vec3{1.1f, 0.45f, 1.1f}));
        }
    }
    // A wide floor under the wheels' bottoms (36.425 - 0.55).
    rig.body(at(982.f, 35.875f - 0.5f, -661.4f), Vec3{600.f, 1.f, 600.f}, true);
    rig.play();
    auto wheels = [&](double speed, double torque) {
        for (engine_core::JointConstraint* axle : axles) {
            axle->set_param(engine_core::JointParam::SpinMotorSpeed, speed);
            axle->set_param(engine_core::JointParam::MaxSpinTorque, torque);
        }
    };
    wheels(0.0, 2500.0);
    rig.engine_steps(240);
    const Matrix4 start = chassis->transform();
    wheels(1500.0, 900.0);
    rig.engine_steps(240 * 3);
    double roll = 0.0;
    double rollSquared = 0.0;
    int samples = 0;
    for (int i = 0; i < 240 * 6; ++i) {
        rig.engine_steps(1);
        const double lean = chassis->transform().m[4];  // up's sideways part
        roll += lean;
        rollSquared += lean * lean;
        ++samples;
    }
    const Matrix4 end = chassis->transform();
    const double mean = roll / samples;
    const double rms = std::sqrt(std::max(0.0, rollSquared / samples - mean * mean)) * 57.2958;
    const float drift = x_of(end) - x_of(start);
    INFO("travelled " << (z_of(end) - z_of(start)) << ", drift " << drift << ", roll RMS " << rms << " deg");
    REQUIRE(z_of(end) - z_of(start) > 100.f);
    REQUIRE(std::fabs(drift) < 0.5f);
    REQUIRE(rms < 0.05);
}
```

Add `sandbox/cylinder_car_tests.cpp` to the sandbox list. Build, then run `./build/Release/sandbox.exe "[.car]"`.
Expected: PASS, the `INFO` line showing drift and roll. If the drift or roll misses, record the numbers and compare them with the sphere numbers (0.1 units, 0.000°) in the final report; do not tune the car's files in this task.

- [ ] **Step 3: A broken patch stops configuring**

Run:

```bash
cp cmake/box3d/cylinder.patch build/cylinder.patch.good
git -C build/_deps/box3d-src stash
sed -i '0,/b3_cylinderShape,/s//b3_cylinderShapeBroken,/' cmake/box3d/cylinder.patch
cmake -S . -B build > build/cyl-broken.log 2>&1; echo "configure exit: $?"; grep -c "cylinder.patch does not apply" build/cyl-broken.log
cp build/cylinder.patch.good cmake/box3d/cylinder.patch
git -C build/_deps/box3d-src stash pop
cmake -S . -B build > build/cyl-config.log 2>&1; echo "configure exit: $?"
```

Expected: `configure exit: 1` and `1` match first; after restoring, `configure exit: 0`. `git -C build/_deps/box3d-src diff --stat` matches the patch again.

- [ ] **Step 4: The whole suite, both configurations**

Run (nothing else running):
- `cmake --build build --config Release --target sandbox AnarchyStudio AnarchyPlayer > build/cyl-rel.log 2>&1; grep -E " error " build/cyl-rel.log; ./build/Release/sandbox.exe > build/cyl-suite-rel.log 2>&1; tail -3 build/cyl-suite-rel.log`
- `cmake --build build --config Debug --target sandbox > build/cyl-dbg.log 2>&1; ./build/Debug/sandbox.exe > build/cyl-suite-dbg.log 2>&1; tail -3 build/cyl-suite-dbg.log`

Expected: Release all green. Debug green except the known Debug-only timeouts (A37, A39, A46, Q1–Q6), which also fail without this branch. List any other failure by name in the report.

- [ ] **Step 5: Commit**

```bash
git add sandbox/cylinder_world_tests.cpp sandbox/cylinder_car_tests.cpp CMakeLists.txt
git commit -m "Cylinder: speed against the layered hulls, and the H1Z1 car on cylinder wheels, as hidden tests

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```
