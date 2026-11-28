#ifndef EZ2_NAVMESH_DYNAMIC_H
#define EZ2_NAVMESH_DYNAMIC_H

#if defined( _WIN32 )
#pragma once
#endif

#include "mathlib/vector.h"
#include <vector>

#ifndef DT_VIRTUAL_QUERYFILTER
#error "Define DT_VIRTUAL_QUERYFILTER in the Detour lib and this project (dtQueryFilter must be virtual)."
#endif
#include "DetourNavMeshQuery.h"

class CBaseEntity;
class CAI_BaseNPC;

bool EZ2_NavPointInPropCutout(const Vector& p);
void EZ2_NavGetPropCutoutFootprints(std::vector<std::vector<Vector> >& out);
// Legacy single-point API (thin wrapper over EZ2_NavBuildCutoutDetour: returns the first wrap-around waypoint)
bool EZ2_NavFindCutoutDetour(const Vector& a, const Vector& b, float clearance, Vector& outPt);
// Wraps the leg a->b around resting-prop footprints inflated by `clearance`.
// Returns 0 = leg clear, 1 = out[] holds waypoints (excluding a and b), -1 = blocked with no way around
// (pBlockCenter / pBlockRadius, if given, describe the blocking prop for EZ2_NavMarkBlocked)
int EZ2_NavBuildCutoutDetour(const Vector& a, const Vector& b, float clearance, std::vector<Vector>& out, Vector* pBlockCenter = NULL, float* pBlockRadius = NULL);

enum
{
	EZ2_NAVDYN_CLEAR = 0,
	EZ2_NAVDYN_DOOR = 1,   // closed door NPCs can open: passable at a cost
	EZ2_NAVDYN_BLOCKED = 2    // locked / unopenable: impassable
};

struct EZ2_NavDynCoverSpot_t
{
	Vector vecPos;
	Vector vecCoverDir;
	float  flYaw;
	float  flHeight;
	bool   bCrouchOnly;
	EHANDLE hSourceEnt;
};

int  EZ2_NavGetDynState(int polyIndex);
int  EZ2_NavGetDynStateByRef(unsigned int polyRef);

// Temporary path-cost danger zone (fire, hurt triggers, etc). Grenades are picked up automatically.
void EZ2_NavAddDanger(const Vector& pos, float radius, float duration, float penalty = 4.0f);

CBaseEntity* EZ2_NavGetDoorForPoly(int polyIndex);
// Temporarily block polys near a point (path-failure feedback; replaces stale-link logic)
void EZ2_NavMarkBlocked(const Vector& pos, float radius, float duration);
// Blacklist one off-mesh jump link (either direction) so path queries skip it for a while
void EZ2_NavMarkJumpFailed(const Vector& a, const Vector& b, float duration);
// Re-evaluate navmesh blocking for a non-physics prop (cars, containers, dynamic props, etc.)
void EZ2_NavUpdatePropObstacle(CBaseEntity* pProp);
// Collect runtime dynamic cover spots around a world origin for AI or debug draw
// Dynamic only (no baked hints). pThreat = position to hide from (default: a fixed point east of origin)
void EZ2_NavCollectDynamicCoverAroundOrigin(const Vector& origin, float radius, std::vector<EZ2_NavDynCoverSpot_t>& outSpots, const Vector* pThreat = NULL);

class CEZ2_NavFilter : public dtQueryFilter
{
public:
	// pNPC (optional): enables per-NPC jump reach screening of off-mesh links
	CEZ2_NavFilter(bool bCanOpenDoors = false, bool bCanJump = true, CAI_BaseNPC* pNPC = NULL, bool bCanClimb = true);

	virtual bool passFilter(const dtPolyRef ref, const dtMeshTile* tile, const dtPoly* poly) const override;
	virtual float getCost(const float* pa, const float* pb,
		const dtPolyRef prevRef, const dtMeshTile* prevTile, const dtPoly* prevPoly,
		const dtPolyRef curRef, const dtMeshTile* curTile, const dtPoly* curPoly,
		const dtPolyRef nextRef, const dtMeshTile* nextTile, const dtPoly* nextPoly) const override;

private:
	bool m_bCanOpenDoors;
	bool m_bCanJump;
	bool m_bCanClimb;
	CAI_BaseNPC* m_pNPC;
};

#endif // EZ2_NAVMESH_DYNAMIC_H