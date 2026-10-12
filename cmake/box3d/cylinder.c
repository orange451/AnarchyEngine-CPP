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
