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
