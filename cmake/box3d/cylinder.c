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
