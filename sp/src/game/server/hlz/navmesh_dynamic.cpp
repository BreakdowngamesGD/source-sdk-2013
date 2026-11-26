//=============================================================================
// navmesh_dynamic.cpp
// Runtime state layered over the baked navmesh:
//   - door state   -> per-poly CLEAR / DOOR (cost) / BLOCKED (impassable)
//   - danger zones -> extra path cost near grenades etc.
//   - hint state   -> CAI_Hint enabled/disabled mirrored onto baked hints
// Works on both mesh types (full mesh has no baked doors at all).
//=============================================================================
#include "cbase.h"
#include "NavMeshDynamic.h"
#include "NavMeshManager.h"
#include "NavMeshBuilder_Node.h"   // EZ2_NAV_AREA_*
#include "DetourNavMesh.h"
#include "DetourCommon.h"
#include "ai_hint.h"
#include "ai_dynamiclink.h"
#include "ai_link.h"
#include "ai_node.h"
#include "ai_network.h"
#include "basetoggle.h"
#include "props.h"
#include "igamesystem.h"
#include "BasePropDoor.h"
#include <vector>
#include <map>

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static ConVar ez2_nav_door_penalty("ez2_nav_door_penalty", "150", 0, "Path cost (~units of detour) for passing through a closed, openable door");
static ConVar ez2_nav_func_door_blocks("ez2_nav_func_door_blocks", "1", 0, "1 = closed func_door/func_door_rotating are impassable (NPCs can't open them), 0 = treat like openable doors");

namespace
{
	struct DoorRec_t
	{
		EHANDLE          hDoor;
		std::vector<int> polys;
		bool             bMapped;
		DoorRec_t() : bMapped(false) {}
	};

	struct Danger_t
	{
		Vector pos;
		float  radius;
		float  penalty;
		float  expire;
	};

	struct DangerSrc_t { const char* pszClass; float radius; float penalty; };
	static const DangerSrc_t s_DangerSrc[] =
	{
		{ "npc_grenade_frag",  192.0f, 8.0f },
		{ "grenade_ar2",       160.0f, 6.0f },
		{ "prop_combine_ball", 128.0f, 6.0f },
	};

	static const int MAX_DANGERS = 32;

	static dtNavMesh* s_pMesh = NULL;
	static std::vector<unsigned char>  s_PolyState;
	static std::vector<DoorRec_t>      s_Doors;
	static std::vector<Danger_t>       s_ManualDangers;
	static std::vector<Danger_t>       s_AutoDangers;
	static float                       s_flNextFast = 0.0f;
	static float                       s_flNextHints = 0.0f;

	inline void DetourToSrc(const float* p, Vector& out)
	{
		out.x = p[0];
		out.y = -p[2];
		out.z = p[1];
	}

	// Closed + openable = DOOR, closed + locked/unopenable = BLOCKED, open/not a door = CLEAR
	int GetDoorBlockState(CBaseEntity* pEnt)
	{
		if (FClassnameIs(pEnt, "prop_door_rotating"))
		{
			CBasePropDoor* pDoor = dynamic_cast<CBasePropDoor*>(pEnt);
			if (!pDoor || !pDoor->IsDoorClosed())
				return EZ2_NAVDYN_CLEAR;
			return pDoor->IsDoorLocked() ? EZ2_NAVDYN_BLOCKED : EZ2_NAVDYN_DOOR;
		}

		CBaseToggle* pToggle = dynamic_cast<CBaseToggle*>(pEnt);
		if (!pToggle || pToggle->m_toggle_state != TS_AT_BOTTOM)
			return EZ2_NAVDYN_CLEAR;

		return ez2_nav_func_door_blocks.GetBool() ? EZ2_NAVDYN_BLOCKED : EZ2_NAVDYN_DOOR;
	}

	// Which polys does the (closed) door footprint overlap? Done once per door.
	void MapDoorPolys(const dtMeshTile* pTile, const Vector& mins, const Vector& maxs, std::vector<int>& out)
	{
		const float pad = 24.0f;
		const float fp[12] =
		{
			mins.x - pad, 0.0f, -(mins.y - pad),
			maxs.x + pad, 0.0f, -(mins.y - pad),
			maxs.x + pad, 0.0f, -(maxs.y + pad),
			mins.x - pad, 0.0f, -(maxs.y + pad)
		};

		for (int i = 0; i < pTile->header->polyCount; ++i)
		{
			const dtPoly* p = &pTile->polys[i];
			if (p->getType() != DT_POLYTYPE_GROUND)
				continue;

			float pv[DT_VERTS_PER_POLYGON * 3];
			float minX = FLT_MAX, maxX = -FLT_MAX, minZ = FLT_MAX, maxZ = -FLT_MAX, sumY = 0.0f;
			for (int v = 0; v < p->vertCount; ++v)
			{
				const float* src = &pTile->verts[p->verts[v] * 3];
				pv[v * 3 + 0] = src[0];
				pv[v * 3 + 1] = src[1];
				pv[v * 3 + 2] = src[2];
				minX = MIN(minX, src[0]); maxX = MAX(maxX, src[0]);
				minZ = MIN(minZ, src[2]); maxZ = MAX(maxZ, src[2]);
				sumY += src[1];
			}

			const float cy = sumY / (float)p->vertCount;   // Detour Y == Source Z
			if (cy < mins.z - 24.0f || cy > maxs.z)
				continue;

			// Huge polys spanning a doorway would penalise a whole room; skip them
			if ((maxX - minX) > 256.0f || (maxZ - minZ) > 256.0f)
				continue;

			if (dtOverlapPolyPoly2D(fp, 4, pv, p->vertCount))
				out.push_back(i);
		}
	}

	static std::vector<float>                            s_PolyBlockUntil;
	static std::map<CAI_DynamicLink*, std::vector<int> > s_LinkPolys;

	// Polys of the node mesh that were baked as DYNLINK corridors along this link
	void MapLinkPolys(const dtMeshTile* t, const Vector& a, const Vector& b, std::vector<int>& out)
	{
		const float pa[3] = { a.x, a.z, -a.y }, pb[3] = { b.x, b.z, -b.y };
		for (int i = 0; i < t->header->polyCount; ++i)
		{
			const dtPoly* p = &t->polys[i];
			if (p->getType() != DT_POLYTYPE_GROUND || p->getArea() != EZ2_NAV_AREA_DYNLINK)
				continue;

			float c[3] = { 0, 0, 0 };
			for (int v = 0; v < p->vertCount; ++v)
				for (int k = 0; k < 3; ++k)
					c[k] += t->verts[p->verts[v] * 3 + k] / (float)p->vertCount;

			float tt;
			if (dtDistancePtSegSqr2D(c, pa, pb, tt) > 48.0f * 48.0f)
				continue;
			if (fabsf(c[1] - (pa[1] + (pb[1] - pa[1]) * tt)) > 64.0f)
				continue;
			out.push_back(i);
		}
	}

	// Live state of info_node_link / link controllers: OFF (and not NPC-restricted) = impassable
	void UpdateDynamicLinks(const dtMeshTile* pTile)
	{
		if (!g_pBigAINet)
			return;

		for (CAI_DynamicLink* p = CAI_DynamicLink::m_pAllDynamicLinks; p; p = p->m_pNextDynamicLink)
		{
			if (p->m_nLinkState != LINK_OFF || p->m_strAllowUse != NULL_STRING)
				continue;
			if (p->m_nSrcID == NO_NODE || p->m_nDestID == NO_NODE)
				continue;

			std::map<CAI_DynamicLink*, std::vector<int> >::iterator it = s_LinkPolys.find(p);
			if (it == s_LinkPolys.end())
			{
				std::vector<int> polys;
				CAI_Node* a = g_pBigAINet->GetNode(p->m_nSrcID, false);
				CAI_Node* b = g_pBigAINet->GetNode(p->m_nDestID, false);
				if (a && b)
					MapLinkPolys(pTile, a->GetOrigin(), b->GetOrigin(), polys);
				it = s_LinkPolys.insert(std::make_pair(p, polys)).first;
			}

			for (size_t k = 0; k < it->second.size(); ++k)
			{
				const int idx = it->second[k];
				if (idx < (int)s_PolyState.size())
					s_PolyState[idx] = (unsigned char)EZ2_NAVDYN_BLOCKED;
			}
		}
	}

	void UpdateDoors(dtNavMesh* pMesh)
	{
		const dtMeshTile* pTile = static_cast<const dtNavMesh*>(pMesh)->getTile(0);
		if (!pTile || !pTile->header)
			return;

		if (pMesh != s_pMesh)
		{
			s_pMesh = pMesh;
			s_Doors.clear();
			s_PolyState.assign(pTile->header->polyCount, 0);
			s_PolyBlockUntil.assign(pTile->header->polyCount, 0.0f);
			s_LinkPolys.clear();
		}

		for (int i = (int)s_Doors.size() - 1; i >= 0; --i)
		{
			if (!s_Doors[i].hDoor.Get())
				s_Doors.erase(s_Doors.begin() + i);
		}

		static const char* s_Classes[] = { "prop_door_rotating", "func_door", "func_door_rotating" };
		for (int c = 0; c < 3; ++c)
		{
			for (CBaseEntity* pEnt = gEntList.FindEntityByClassname(NULL, s_Classes[c]); pEnt; pEnt = gEntList.FindEntityByClassname(pEnt, s_Classes[c]))
			{
				bool bKnown = false;
				for (size_t i = 0; i < s_Doors.size(); ++i)
				{
					if (s_Doors[i].hDoor.Get() == pEnt) { bKnown = true; break; }
				}
				if (!bKnown)
				{
					DoorRec_t rec;
					rec.hDoor = pEnt;
					s_Doors.push_back(rec);
				}
			}
		}

		std::fill(s_PolyState.begin(), s_PolyState.end(), (unsigned char)0);

		for (size_t i = 0; i < s_Doors.size(); ++i)
		{
			DoorRec_t& rec = s_Doors[i];
			CBaseEntity* pDoor = rec.hDoor.Get();
			if (!pDoor)
				continue;

			const int state = GetDoorBlockState(pDoor);
			if (state == EZ2_NAVDYN_CLEAR)
				continue;

			// Map polys the first time we see the door at rest/closed (bounds are valid then)
			if (!rec.bMapped)
			{
				Vector mins, maxs;
				pDoor->CollisionProp()->WorldSpaceSurroundingBounds(&mins, &maxs);
				MapDoorPolys(pTile, mins, maxs, rec.polys);
				rec.bMapped = true;
			}

			for (size_t k = 0; k < rec.polys.size(); ++k)
			{
				const int idx = rec.polys[k];
				if (idx < (int)s_PolyState.size() && s_PolyState[idx] < state)
					s_PolyState[idx] = (unsigned char)state;
			}
		}
		UpdateDynamicLinks(pTile);
	}

	void UpdateDangers()
	{
		const float now = gpGlobals->curtime;

		for (int i = (int)s_ManualDangers.size() - 1; i >= 0; --i)
		{
			if (s_ManualDangers[i].expire <= now)
				s_ManualDangers.erase(s_ManualDangers.begin() + i);
		}

		s_AutoDangers.clear();
		for (int c = 0; c < ARRAYSIZE(s_DangerSrc); ++c)
		{
			for (CBaseEntity* pEnt = gEntList.FindEntityByClassname(NULL, s_DangerSrc[c].pszClass); pEnt; pEnt = gEntList.FindEntityByClassname(pEnt, s_DangerSrc[c].pszClass))
			{
				if ((int)s_AutoDangers.size() >= MAX_DANGERS)
					return;

				Danger_t d;
				d.pos = pEnt->GetAbsOrigin();
				d.radius = s_DangerSrc[c].radius;
				d.penalty = s_DangerSrc[c].penalty;
				d.expire = 0.0f;
				s_AutoDangers.push_back(d);
			}
		}
	}

	// Mirror CAI_Hint enabled/disabled state onto baked hints (matched by Hammer ID)
	void UpdateHints()
	{
		std::vector<EZ2_NavHint_t>& hints = g_EZ2NavMeshManager.AccessNavHints();
		if (hints.empty())
			return;

		std::map<int, bool> live;   // WC id -> disabled
		AIHintIter_t it;
		for (CAI_Hint* p = CAI_HintManager::GetFirstHint(&it); p; p = CAI_HintManager::GetNextHint(&it))
			live[p->GetWCId()] = p->IsDisabled();

		for (size_t i = 0; i < hints.size(); ++i)
		{
			if (hints[i].sourceWCNodeID <= 0)
				continue;   // generated cover hints have no source entity

			std::map<int, bool>::const_iterator f = live.find(hints[i].sourceWCNodeID);
			hints[i].bDisabled = (f == live.end()) ? true : f->second;   // entity gone = disabled
		}
	}

	void ResetAll()
	{
		s_pMesh = NULL;
		s_PolyState.clear();
		s_Doors.clear();
		s_ManualDangers.clear();
		s_AutoDangers.clear();
		s_flNextFast = 0.0f;
		s_flNextHints = 0.0f;
		s_PolyBlockUntil.clear(); 
		s_LinkPolys.clear();
	}
}

//-----------------------------------------------------------------------------
// Public API
//-----------------------------------------------------------------------------
int EZ2_NavGetDynState(int polyIndex)
{
	if (polyIndex < 0)
		return EZ2_NAVDYN_CLEAR;
	if (polyIndex < (int)s_PolyBlockUntil.size() && s_PolyBlockUntil[polyIndex] > gpGlobals->curtime)
		return EZ2_NAVDYN_BLOCKED;
	return (polyIndex < (int)s_PolyState.size()) ? s_PolyState[polyIndex] : EZ2_NAVDYN_CLEAR;
}

CBaseEntity* EZ2_NavGetDoorForPoly(int polyIndex)
{
	for (size_t i = 0; i < s_Doors.size(); ++i)
	{
		CBaseEntity* pDoor = s_Doors[i].hDoor.Get();
		if (!pDoor || !s_Doors[i].bMapped || GetDoorBlockState(pDoor) != EZ2_NAVDYN_DOOR)
			continue;
		for (size_t k = 0; k < s_Doors[i].polys.size(); ++k)
			if (s_Doors[i].polys[k] == polyIndex)
				return pDoor;
	}
	return NULL;
}

void EZ2_NavMarkBlocked(const Vector& pos, float radius, float duration)
{
	dtNavMesh* pMesh = g_EZ2NavMeshManager.GetNavMesh();
	dtNavMeshQuery* pQuery = g_EZ2NavMeshManager.GetNavMeshQuery();
	const dtMeshTile* pTile = pMesh ? static_cast<const dtNavMesh*>(pMesh)->getTile(0) : NULL;
	if (!pQuery || !pTile || !pTile->header)
		return;

	if ((int)s_PolyBlockUntil.size() != pTile->header->polyCount)
		s_PolyBlockUntil.assign(pTile->header->polyCount, 0.0f);

	const float c[3] = { pos.x, pos.z, -pos.y };
	const float ext[3] = { radius, 48.0f, radius };
	dtQueryFilter f;
	dtPolyRef refs[16];
	int n = 0;
	if (dtStatusFailed(pQuery->queryPolygons(c, ext, &f, refs, &n, 16)))
		return;

	for (int i = 0; i < n; ++i)
	{
		const int idx = (int)pMesh->decodePolyIdPoly(refs[i]);
		if (idx < (int)s_PolyBlockUntil.size())
			s_PolyBlockUntil[idx] = gpGlobals->curtime + duration;
	}
}

int EZ2_NavGetDynStateByRef(unsigned int polyRef)
{
	dtNavMesh* pMesh = g_EZ2NavMeshManager.GetNavMesh();
	if (!pMesh || !polyRef)
		return EZ2_NAVDYN_CLEAR;
	return EZ2_NavGetDynState((int)pMesh->decodePolyIdPoly((dtPolyRef)polyRef));
}

void EZ2_NavAddDanger(const Vector& pos, float radius, float duration, float penalty)
{
	if ((int)s_ManualDangers.size() >= MAX_DANGERS)
		s_ManualDangers.erase(s_ManualDangers.begin());

	Danger_t d;
	d.pos = pos;
	d.radius = MAX(radius, 1.0f);
	d.penalty = penalty;
	d.expire = gpGlobals->curtime + duration;
	s_ManualDangers.push_back(d);
}

//-----------------------------------------------------------------------------
// Query filter
//-----------------------------------------------------------------------------
CEZ2_NavFilter::CEZ2_NavFilter(bool bCanOpenDoors, bool bCanJump)
	: m_bCanOpenDoors(bCanOpenDoors)
	, m_bCanJump(bCanJump)
{
	setAreaCost(EZ2_NAV_AREA_DYNLINK, 3.0f);
	setAreaCost(EZ2_NAV_AREA_JUMP, 2.0f);
}

bool CEZ2_NavFilter::passFilter(const dtPolyRef ref, const dtMeshTile* tile, const dtPoly* poly) const
{
	if (!dtQueryFilter::passFilter(ref, tile, poly))
		return false;

	if (!m_bCanJump &&
		(poly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION || poly->getArea() == EZ2_NAV_AREA_JUMP))
		return false;

	const int state = EZ2_NavGetDynState((int)(poly - tile->polys));
	if (state == EZ2_NAVDYN_BLOCKED)
		return false;
	if (state == EZ2_NAVDYN_DOOR && !m_bCanOpenDoors)
		return false;

	return true;
}

float CEZ2_NavFilter::getCost(const float* pa, const float* pb,
	const dtPolyRef prevRef, const dtMeshTile* prevTile, const dtPoly* prevPoly,
	const dtPolyRef curRef, const dtMeshTile* curTile, const dtPoly* curPoly,
	const dtPolyRef nextRef, const dtMeshTile* nextTile, const dtPoly* nextPoly) const
{
	float cost = dtQueryFilter::getCost(pa, pb, prevRef, prevTile, prevPoly, curRef, curTile, curPoly, nextRef, nextTile, nextPoly);

	if (curTile && curPoly && EZ2_NavGetDynState((int)(curPoly - curTile->polys)) == EZ2_NAVDYN_DOOR)
		cost += ez2_nav_door_penalty.GetFloat();

	if (!s_ManualDangers.empty() || !s_AutoDangers.empty())
	{
		const float mid[3] = { (pa[0] + pb[0]) * 0.5f, (pa[1] + pb[1]) * 0.5f, (pa[2] + pb[2]) * 0.5f };
		Vector vecMid;
		DetourToSrc(mid, vecMid);
		const float len = dtVdist(pa, pb);

		for (int pass = 0; pass < 2; ++pass)
		{
			const std::vector<Danger_t>& list = pass ? s_AutoDangers : s_ManualDangers;
			for (size_t i = 0; i < list.size(); ++i)
			{
				const float d = vecMid.DistTo(list[i].pos);
				if (d < list[i].radius)
					cost += len * list[i].penalty * (1.0f - d / list[i].radius);
			}
		}
	}

	return cost;
}

//-----------------------------------------------------------------------------
// Per-frame refresh
//-----------------------------------------------------------------------------
class CEZ2_NavDynamicSystem : public CAutoGameSystemPerFrame
{
public:
	CEZ2_NavDynamicSystem() : CAutoGameSystemPerFrame("EZ2_NavDynamic") {}

	virtual void LevelShutdownPostEntity() OVERRIDE { ResetAll(); }

	virtual void FrameUpdatePostEntityThink() OVERRIDE
	{
		if (!g_EZ2NavMeshManager.IsReady())
			return;

		const float now = gpGlobals->curtime;

		if (now >= s_flNextFast)
		{
			s_flNextFast = now + 0.25f;
			UpdateDoors(g_EZ2NavMeshManager.GetNavMesh());
			UpdateDangers();
		}

		if (now >= s_flNextHints)
		{
			s_flNextHints = now + 1.0f;
			UpdateHints();
		}
	}
};

static CEZ2_NavDynamicSystem s_EZ2NavDynamicSystem;

CON_COMMAND(ez2_nav_danger_add, "Adds a temporary danger zone at the crosshair: ez2_nav_danger_add [radius] [seconds] [penalty]")
{
	CBasePlayer* pPlayer = UTIL_GetCommandClient();
	if (!pPlayer)
		pPlayer = UTIL_GetLocalPlayer();
	if (!pPlayer)
		return;

	Vector vecForward;
	pPlayer->EyeVectors(&vecForward);

	trace_t tr;
	UTIL_TraceLine(pPlayer->EyePosition(), pPlayer->EyePosition() + vecForward * 4096.0f, MASK_SOLID, pPlayer, COLLISION_GROUP_NONE, &tr);

	const float radius = (args.ArgC() >= 2) ? atof(args[1]) : 192.0f;
	const float secs = (args.ArgC() >= 3) ? atof(args[2]) : 20.0f;
	const float penalty = (args.ArgC() >= 4) ? atof(args[3]) : 8.0f;

	EZ2_NavAddDanger(tr.endpos, radius, secs, penalty);
	Msg("[EZ2 NavMesh] Danger zone at (%.0f, %.0f, %.0f) r=%.0f for %.0fs\n", tr.endpos.x, tr.endpos.y, tr.endpos.z, radius, secs);
}