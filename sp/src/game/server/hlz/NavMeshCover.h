#ifndef EZ2_NAVMESH_COVER_H
#define EZ2_NAVMESH_COVER_H

#if defined( _WIN32 )
#pragma once
#endif

#include "mathlib/vector.h"
#include <vector>

enum EZ2_CoverKind_t
{
	EZ2_COVER_NONE = 0,
	EZ2_COVER_TALL_WALL_OR_PROP, // Blocks standing sightline to threat
	EZ2_COVER_LOW_SHORT,         // Low wall/prop (~24-44u tall): good crouch/shoot-over cover
	EZ2_COVER_CORNER             // Edge/corner spot: blocks threat from one side, opens to the side
};

struct EZ2_CoverSpot_t
{
	Vector vecPos;          // Walkable position on/near navmesh
	Vector vecCoverNormal;  // Direction from spot toward the blocking wall/prop
	Vector vecPeekDir;      // Suggested lateral direction to step/peek around corner
	float flObstacleHeight; // Estimated height of nearby blocking geometry (Source units)
	float flScore;          // Higher = better cover against vecThreatPos
	EZ2_CoverKind_t kind;
};

void FindCoverSpotsAroundOrigin(
	const Vector& vecCenter,
	const Vector& vecThreatPos,
	float flSearchRadius,
	int maxResults,
	std::vector<EZ2_CoverSpot_t>& outSpots
);

#endif // EZ2_NAVMESH_COVER_H