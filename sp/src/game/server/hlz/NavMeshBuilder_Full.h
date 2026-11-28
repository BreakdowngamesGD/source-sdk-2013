#ifndef EZ2_NAVMESH_BUILDER_FULL_H
#define EZ2_NAVMESH_BUILDER_FULL_H
#if defined( _WIN32 )
#pragma once
#endif

#include <vector>
#include "mathlib/vector.h"
#include "NavMeshManager.h"   // EZ2_NavHint_t, EZ2_NavClimb_t

class dtNavMesh;

struct FullNavTriangle_t
{
	Vector v0;
	Vector v1;
	Vector v2;
};

struct FullNavJumpLink_t
{
	Vector a;
	Vector b;
	bool bBidir;   // false = one-way (drop too high to jump back up)

	FullNavJumpLink_t() : bBidir(true) {}
};

struct FullNavObstacle_t
{
	std::vector<Vector> footprint;
	float zMin;
	float zMax;
};

struct FullNavExtras_t
{
	std::vector<FullNavObstacle_t> obstacles;
	std::vector<FullNavJumpLink_t> jumps;
	std::vector<EZ2_NavHint_t> entityHints;
	std::vector<EZ2_NavClimb_t> climbs;
	float jumpRadius;

	FullNavExtras_t() : jumpRadius(48.0f) {}
};

struct FullNavBuildConfig_t
{
	float cellSize;
	float cellHeight;
	float agentHeight;
	float agentRadius;
	float agentMaxClimb;
	float agentMaxSlopeDeg;
};

// A box removed from the walkable floor. Lives in entity space (the prop's OBB).
struct EZ2_NavCarve_t
{
	Vector origin;   // entity origin
	QAngle angles;   // entity angles
	Vector center;   // OBB center, entity space
	Vector halfExt;  // OBB half extents, entity space, already scaled
};

bool BuildAndSaveFullSoloNavMesh(
	const char* pszMapName,
	const std::vector<FullNavTriangle_t>& triangles,
	const FullNavBuildConfig_t& config,
	bool bAttachImmediately = true,
	const FullNavExtras_t* pExtras = NULL,
	bool bSaveToDisk = true
);

// Rebuilds the full mesh from the cached map soup with the given boxes carved out.
// Runtime only: does not write the .nav file.
bool EZ2_NavRebuildCarved(const std::vector<EZ2_NavCarve_t>& carves);

#endif // EZ2_NAVMESH_BUILDER_FULL_H