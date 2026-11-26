#ifndef EZ2_NAVMESH_DYNAMIC_H
#define EZ2_NAVMESH_DYNAMIC_H

#if defined( _WIN32 )
#pragma once
#endif

#include "mathlib/vector.h"

#ifndef DT_VIRTUAL_QUERYFILTER
#error "Define DT_VIRTUAL_QUERYFILTER in the Detour lib and this project (dtQueryFilter must be virtual)."
#endif
#include "DetourNavMeshQuery.h"

enum
{
	EZ2_NAVDYN_CLEAR = 0,
	EZ2_NAVDYN_DOOR = 1,   // closed door NPCs can open: passable at a cost
	EZ2_NAVDYN_BLOCKED = 2    // locked / unopenable: impassable
};

int  EZ2_NavGetDynState(int polyIndex);
int  EZ2_NavGetDynStateByRef(unsigned int polyRef);

// Temporary path-cost danger zone (fire, hurt triggers, etc). Grenades are picked up automatically.
void EZ2_NavAddDanger(const Vector& pos, float radius, float duration, float penalty = 4.0f);

class CBaseEntity;
CBaseEntity* EZ2_NavGetDoorForPoly(int polyIndex);
// Temporarily block polys near a point (path-failure feedback; replaces stale-link logic)
void EZ2_NavMarkBlocked(const Vector& pos, float radius, float duration);

class CEZ2_NavFilter : public dtQueryFilter
{
public:
	CEZ2_NavFilter(bool bCanOpenDoors = false, bool bCanJump = true);

	virtual bool passFilter(const dtPolyRef ref, const dtMeshTile* tile, const dtPoly* poly) const override;
	virtual float getCost(const float* pa, const float* pb,
		const dtPolyRef prevRef, const dtMeshTile* prevTile, const dtPoly* prevPoly,
		const dtPolyRef curRef, const dtMeshTile* curTile, const dtPoly* curPoly,
		const dtPolyRef nextRef, const dtMeshTile* nextTile, const dtPoly* nextPoly) const override;

private:
	bool m_bCanOpenDoors;
	bool m_bCanJump;
};

#endif // EZ2_NAVMESH_DYNAMIC_H