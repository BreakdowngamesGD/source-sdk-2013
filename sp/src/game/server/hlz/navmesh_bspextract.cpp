//=============================================================================
// navmesh_bspextract.cpp
//
// Purpose: Reads the current map's .bsp, extracts world (model 0) triangles
//          including displacements, static props, non-physics map props, and
//          func_npc_clip brush geometry, and feeds them to
//          BuildAndSaveFullSoloNavMesh.
//
// Console:  ez2_nav_build_full [cellsize]
//
// Notes:
//  - Model 0 (worldspawn) + static props + solid non-physics props +
//    func_npc_clip volumes are baked into the triangle soup before bounds/tile
//    and Recast rasterization.
//  - Physics props (CPhysicsProp) are excluded from static bake.
//  - Compressed (LZMA) lumps are not supported; compile with vbsp default (uncompressed).
//=============================================================================

#include "cbase.h"
#include "bspfile.h"
#include "filesystem.h"
#include "gamebspfile.h"
#include "utlbuffer.h"
#include "props.h"
#include "vcollide.h"
#include "vcollide_parse.h"
#include <vector>
#include <float.h>
#include <string.h>
#include "NavMeshBuilder_Node.h"
#include "ai_network.h"
#include "ai_node.h"
#include "ai_link.h"
#include "ai_basenpc.h"
#include "NavMeshBuilder_Full.h"
// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

#ifndef GAMELUMP_STATIC_PROPS
#define GAMELUMP_STATIC_PROPS ( ('p') | ('r' << 8) | ('p' << 16) | ('s' << 24) )
#endif

extern IFileSystem* filesystem;

static ConVar ez2_nav_agent_radius("ez2_nav_agent_radius", "16", 0, "Navmesh build: agent radius (units)");
static ConVar ez2_nav_agent_height("ez2_nav_agent_height", "72", 0, "Navmesh build: agent height (units)");
static ConVar ez2_nav_agent_climb("ez2_nav_agent_climb", "18", 0, "Navmesh build: max step height (units)");
static ConVar ez2_nav_agent_slope("ez2_nav_agent_slope", "45", 0, "Navmesh build: max walkable slope (degrees)");
static ConVar ez2_nav_cell_size("ez2_nav_cell_size", "8", 0, "Navmesh build: voxel cell size (units). Smaller = better and much slower");
static ConVar ez2_nav_cell_height("ez2_nav_cell_height", "4", 0, "Navmesh build: voxel cell height (units)");
static ConVar ez2_nav_tile_size("ez2_nav_tile_size", "128", 0, "Navmesh build: tile width/height (units) so large props block localized areas");
static ConVar ez2_nav_prop_block("ez2_nav_prop_block", "1", 0, "Carve large non-physics props out of the full navmesh");
static ConVar ez2_nav_prop_block_minheight("ez2_nav_prop_block_minheight", "20", 0, "Props shorter than this are left steppable");
static ConVar ez2_nav_prop_block_maxsize("ez2_nav_prop_block_maxsize", "512", 0, "Props wider than this are never carved (could be walkable interiors)");
static ConVar ez2_nav_jump_radius("ez2_nav_jump_radius", "48", 0, "Off-mesh jump endpoint snap radius");
static ConVar ez2_nav_jump_use_nodegraph("ez2_nav_jump_use_nodegraph", "0", 0, "Also add jump links from the nodegraph (off = geometry-generated only)");
static ConVar ez2_nav_agent_hull("ez2_nav_agent_hull", "-1", 0, "Navmesh build: Hull_t to derive agent radius/height from (-1 = use the agent_* cvars)");
static ConVar ez2_nav_climb_use_nodegraph("ez2_nav_climb_use_nodegraph", "1", 0, "Add ladder links from the nodegraph to the full navmesh");

namespace
{
	// Surfaces that should never become nav geometry
	static const int NAV_SKIP_SURF_FLAGS =
		SURF_SKY | SURF_SKY2D | SURF_NODRAW | SURF_SKIP | SURF_HINT | SURF_TRIGGER;

	// True if a nodraw texinfo is a tools/toolsnpcclip texture (kept as nav geometry)
	static bool IsNpcClipTexInfo(const texinfo_t& ti,
		const dtexdata_t* pTexData, int nTexData,
		const int* pStrTable, int nStrTable,
		const char* pStrData, int nStrData)
	{
		if (ti.texdata < 0 || ti.texdata >= nTexData)
			return false;
		const int sid = pTexData[ti.texdata].nameStringTableID;
		if (sid < 0 || sid >= nStrTable)
			return false;
		const int ofs = pStrTable[sid];
		if (ofs < 0 || ofs >= nStrData)
			return false;
		return Q_stristr(pStrData + ofs, "toolsnpcclip") != NULL;
	}

	// Returns pointer + element count for a lump, with bounds/size validation.
	template <class T>
	bool GetLump(const unsigned char* pBase, int nFileSize, const dheader_t* pHdr, int lumpIdx, const T*& pOut, int& nCount)
	{
		pOut = NULL;
		nCount = 0;

		const lump_t& l = pHdr->lumps[lumpIdx];

		if (l.fourCC[0] || l.fourCC[1] || l.fourCC[2] || l.fourCC[3])
		{
			Warning("[EZ2 NavMesh] Lump %d is compressed; unsupported.\n", lumpIdx);
			return false;
		}

		if (l.filelen == 0)
			return true;

		if (l.fileofs < 0 || l.filelen < 0 || (int64)l.fileofs + l.filelen > nFileSize || (l.filelen % (int)sizeof(T)) != 0)
		{
			Warning("[EZ2 NavMesh] Lump %d has invalid bounds/size.\n", lumpIdx);
			return false;
		}

		pOut = reinterpret_cast<const T*>(pBase + l.fileofs);
		nCount = l.filelen / (int)sizeof(T);
		return true;
	}

	// Emit a triangle wound so its geometric normal agrees with desiredNormal.
	// (Source faces are clockwise; Recast wants CCW-up, so we orient by normal rather than trusting winding.)
	void EmitTri(std::vector<FullNavTriangle_t>& out, const Vector& a, const Vector& b, const Vector& c, const Vector& desiredNormal)
	{
		Vector cr = (b - a).Cross(c - a);
		if (cr.LengthSqr() < 0.0001f)
			return; // degenerate

		FullNavTriangle_t t;
		t.v0 = a;
		if (DotProduct(cr, desiredNormal) < 0.0f)
		{
			t.v1 = c;
			t.v2 = b;
		}
		else
		{
			t.v1 = b;
			t.v2 = c;
		}
		out.push_back(t);
	}

	float Cross2D(const Vector& o, const Vector& a, const Vector& b)
	{
		return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
	}

	// Andrew's monotone chain, XY only
	void ConvexHullXY(std::vector<Vector>& pts, std::vector<Vector>& hull)
	{
		hull.clear();
		const size_t n = pts.size();
		if (n < 3)
			return;

		for (size_t i = 1; i < n; ++i)
		{
			Vector v = pts[i];
			size_t j = i;
			while (j > 0 && (pts[j - 1].x > v.x || (pts[j - 1].x == v.x && pts[j - 1].y > v.y)))
			{
				pts[j] = pts[j - 1];
				--j;
			}
			pts[j] = v;
		}

		hull.resize(2 * n);
		size_t k = 0;
		for (size_t i = 0; i < n; ++i)
		{
			while (k >= 2 && Cross2D(hull[k - 2], hull[k - 1], pts[i]) <= 0) --k;
			hull[k++] = pts[i];
		}
		for (size_t i = n - 1, t = k + 1; i > 0; --i)
		{
			while (k >= t && Cross2D(hull[k - 2], hull[k - 1], pts[i - 1]) <= 0) --k;
			hull[k++] = pts[i - 1];
		}
		hull.resize(k - 1);
	}

	void AddObstacle(const matrix3x4_t& mat, const Vector& mins, const Vector& maxs, FullNavExtras_t* pExtras)
	{
		if (!pExtras || !ez2_nav_prop_block.GetBool())
			return;

		std::vector<Vector> pts;
		float zMin = FLT_MAX, zMax = -FLT_MAX;
		for (int i = 0; i < 8; ++i)
		{
			const Vector c((i & 1) ? maxs.x : mins.x, (i & 2) ? maxs.y : mins.y, (i & 4) ? maxs.z : mins.z);
			Vector w;
			VectorTransform(c, mat, w);
			pts.push_back(w);
			zMin = MIN(zMin, w.z);
			zMax = MAX(zMax, w.z);
		}

		if (zMax - zMin < ez2_nav_prop_block_minheight.GetFloat())
			return;

		FullNavObstacle_t ob;
		ConvexHullXY(pts, ob.footprint);
		if (ob.footprint.size() < 3)
			return;

		float lx = FLT_MAX, hx = -FLT_MAX, ly = FLT_MAX, hy = -FLT_MAX;
		for (size_t i = 0; i < ob.footprint.size(); ++i)
		{
			lx = MIN(lx, ob.footprint[i].x); hx = MAX(hx, ob.footprint[i].x);
			ly = MIN(ly, ob.footprint[i].y); hy = MAX(hy, ob.footprint[i].y);
		}
		if (MAX(hx - lx, hy - ly) > ez2_nav_prop_block_maxsize.GetFloat())
			return;

		ob.zMin = zMin;
		ob.zMax = zMax;
		pExtras->obstacles.push_back(ob);
	}

	// Emits all solids of a vcollide_t transformed by mat, or falls back to boxMins/boxMaxs OBB.
	void AppendCollideOrBoxTriangles(int modelIndex, const matrix3x4_t& mat, const Vector& boxMins, const Vector& boxMaxs,
		bool bAllowMesh, bool bAllSolids, std::vector<FullNavTriangle_t>& outTris)
	{
		vcollide_t* pCollide = (bAllowMesh && modelIndex > 0 && modelinfo && physcollision)
			? modelinfo->GetVCollide(modelIndex) : NULL;

		const bool bUseSolids = pCollide && pCollide->solidCount > 0 &&
			(bAllSolids || pCollide->solidCount == 1);

		if (bUseSolids)
		{
			bool bEmittedAny = false;
			for (int s = 0; s < pCollide->solidCount; ++s)
			{
				if (!pCollide->solids[s])
					continue;

				Vector* pOutVerts = NULL;
				const int nVerts = physcollision->CreateDebugMesh(pCollide->solids[s], &pOutVerts);
				if (nVerts >= 3 && pOutVerts)
				{
					std::vector<Vector> w(nVerts);
					Vector lo(FLT_MAX, FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX, -FLT_MAX);
					for (int i = 0; i < nVerts; ++i)
					{
						VectorTransform(pOutVerts[i], mat, w[i]);
						VectorMin(lo, w[i], lo);
						VectorMax(hi, w[i], hi);
					}
					const Vector center = (lo + hi) * 0.5f;
					for (int i = 0; i + 2 < nVerts; i += 3)
					{
						const Vector cen = (w[i] + w[i + 1] + w[i + 2]) * (1.0f / 3.0f);
						EmitTri(outTris, w[i], w[i + 1], w[i + 2], cen - center);
						bEmittedAny = true;
					}
				}
				if (pOutVerts)
					physcollision->DestroyDebugMesh(nVerts, pOutVerts);
			}

			if (bEmittedAny)
				return;
		}

		Vector c[8];
		Vector center(0, 0, 0);
		for (int i = 0; i < 8; ++i)
		{
			const Vector l((i & 1) ? boxMaxs.x : boxMins.x, (i & 2) ? boxMaxs.y : boxMins.y, (i & 4) ? boxMaxs.z : boxMins.z);
			VectorTransform(l, mat, c[i]);
			center += c[i];
		}
		center *= 0.125f;

		// corner index = x | y<<1 | z<<2
		static const int idx[12][3] = {
			{0,1,3},{0,3,2}, {4,5,7},{4,7,6}, {0,1,5},{0,5,4},
			{2,3,7},{2,7,6}, {0,2,6},{0,6,4}, {1,3,7},{1,7,5}
		};
		for (int i = 0; i < 12; ++i)
		{
			const Vector& a = c[idx[i][0]];
			const Vector& b = c[idx[i][1]];
			const Vector& d = c[idx[i][2]];
			EmitTri(outTris, a, b, d, (a + b + d) * (1.0f / 3.0f) - center);
		}
	}

	// Collision mesh if the model has a single solid, otherwise its OBB.
	// Triangles are oriented outward from the model centre so we don't depend on vphysics winding.
	void AppendModelTriangles(int modelIndex, const matrix3x4_t& mat, const Vector& boxMins, const Vector& boxMaxs,
		bool bAllowMesh, std::vector<FullNavTriangle_t>& outTris)
	{
		AppendCollideOrBoxTriangles(modelIndex, mat, boxMins, boxMaxs, bAllowMesh, false, outTris);
	}

	void AppendNonPhysicsPropGeometry(std::vector<FullNavTriangle_t>& outTris, FullNavExtras_t* pExtras)
	{
		int nProps = 0;
		const size_t beforeCount = outTris.size();

		for (CBaseEntity* pEnt = gEntList.FirstEnt(); pEnt != NULL; pEnt = gEntList.NextEnt(pEnt))
		{
			if (!pEnt->CollisionProp() || pEnt->IsEffectActive(EF_NODRAW) || pEnt->GetSolid() == SOLID_NONE)
				continue;
			if (pEnt->GetMoveParent() != NULL)
				continue;
			if (dynamic_cast<CPhysicsProp*>(pEnt) != NULL)
				continue;
			if (dynamic_cast<CBaseProp*>(pEnt) == NULL)
				continue;

			// Props whose collision lives on bone followers are marked NOT_SOLID + custom ray/box test
			const bool bFollowerSolid = pEnt->IsSolidFlagSet(FSOLID_NOT_SOLID) &&
				pEnt->IsSolidFlagSet(FSOLID_CUSTOMRAYTEST) && pEnt->IsSolidFlagSet(FSOLID_CUSTOMBOXTEST);
			if (pEnt->IsSolidFlagSet(FSOLID_NOT_SOLID) && !bFollowerSolid)
				continue;

			const matrix3x4_t& mat = pEnt->EntityToWorldTransform();
			const Vector mins = pEnt->CollisionProp()->OBBMins();
			const Vector maxs = pEnt->CollisionProp()->OBBMaxs();

			AppendModelTriangles(pEnt->GetModelIndex(), mat, mins, maxs, true, outTris);
			AddObstacle(mat, mins, maxs, pExtras);
			++nProps;
		}

		Msg("[EZ2 NavMesh] Appended %d non-physics entity props (+%d tris)\n",
			nProps, (int)(outTris.size() - beforeCount));
	}

	// prop_static lives in the BSP game lump, not in the entity list
#pragma pack(push, 1)
	struct NavStaticPropHead_t   // leading fields shared by every static prop lump version
	{
		Vector         origin;
		QAngle         angles;
		unsigned short propType;
		unsigned short firstLeaf;
		unsigned short leafCount;
		unsigned char  solid;
		unsigned char  flags;
	};
#pragma pack(pop)

	void AppendStaticPropGeometry(const unsigned char* pBase, int nFileSize, const dheader_t* pHdr,
		std::vector<FullNavTriangle_t>& outTris, FullNavExtras_t* pExtras)
	{
		const lump_t& gl = pHdr->lumps[LUMP_GAME_LUMP];
		if (gl.filelen < (int)sizeof(int) || gl.fileofs < 0 || (int64)gl.fileofs + gl.filelen > nFileSize)
			return;

		int lumpCount = 0;
		memcpy(&lumpCount, pBase + gl.fileofs, sizeof(int));
		const dgamelump_t* pLumps = (const dgamelump_t*)(pBase + gl.fileofs + sizeof(int));
		if (lumpCount <= 0 || sizeof(int) + lumpCount * sizeof(dgamelump_t) > (size_t)gl.filelen)
			return;

		const dgamelump_t* pSprp = NULL;
		for (int i = 0; i < lumpCount; ++i)
		{
			if (pLumps[i].id == GAMELUMP_STATIC_PROPS)
			{
				pSprp = &pLumps[i];
				break;
			}
		}

		if (!pSprp)
			return;
		if (pSprp->flags & 1)
		{
			Warning("[EZ2 NavMesh] Static prop lump is compressed; static props skipped.\n");
			return;
		}
		if (pSprp->fileofs < 0 || pSprp->filelen <= 0 || (int64)pSprp->fileofs + pSprp->filelen > nFileSize)
			return;

		const unsigned char* p = pBase + pSprp->fileofs;
		const unsigned char* pEnd = p + pSprp->filelen;

		int dictCount = 0, leafCount = 0, propCount = 0;
		if (p + 4 > pEnd) return;
		memcpy(&dictCount, p, 4); p += 4;
		if (dictCount < 0 || p + dictCount * 128 > pEnd) return;
		const char* pNames = (const char*)p;
		p += dictCount * 128;

		if (p + 4 > pEnd) return;
		memcpy(&leafCount, p, 4); p += 4;
		if (leafCount < 0 || p + leafCount * 2 > pEnd) return;
		p += leafCount * 2;

		if (p + 4 > pEnd) return;
		memcpy(&propCount, p, 4); p += 4;
		if (propCount <= 0) return;

		const int stride = (int)((pEnd - p) / propCount);   // entry size depends on lump version
		if (stride < (int)sizeof(NavStaticPropHead_t) || stride > 512)
			return;

		// In static prop lump v10+, uniformScale is a float right before flagsEx (2 bytes) at the end of the entry
		const bool bHasUniformScale = (pSprp->version >= 10 && stride >= (int)(sizeof(NavStaticPropHead_t) + 28));

		int nAdded = 0, nMissing = 0;
		const size_t beforeCount = outTris.size();

		for (int i = 0; i < propCount; ++i)
		{
			const unsigned char* pEntry = p + (size_t)i * stride;
			NavStaticPropHead_t h;
			memcpy(&h, pEntry, sizeof(h));

			if (h.solid == SOLID_NONE || h.propType >= dictCount || !modelinfo)
				continue;

			char szModel[128];
			memcpy(szModel, pNames + h.propType * 128, 128);
			szModel[127] = '\0';

			// Precache so the model is registered even if no entity references it
			const bool bAllowPrecache = CBaseEntity::IsPrecacheAllowed();
			CBaseEntity::SetAllowPrecache(true);
			const int modelIndex = CBaseEntity::PrecacheModel(szModel);
			CBaseEntity::SetAllowPrecache(bAllowPrecache);

			const model_t* pModel = (modelIndex > 0) ? modelinfo->GetModel(modelIndex) : NULL;
			if (!pModel)
			{
				++nMissing;
				continue;
			}

			float flScale = 1.0f;
			if (bHasUniformScale)
			{
				// v10+: skin(4) + fadeMin(4) + fadeMax(4) + lightingOrigin(12) + forcedFadeScale(4) +
				// diffuseModulation(4) + disableX360(4) + uniformScale(4) + flagsEx(2)
				const int nScaleOffset = stride - 6;
				if (nScaleOffset >= (int)sizeof(NavStaticPropHead_t) && nScaleOffset + 4 <= stride)
				{
					float s = 1.0f;
					memcpy(&s, pEntry + nScaleOffset, sizeof(float));
					if (s > 0.001f && s < 1000.0f)
						flScale = s;
				}
			}

			Vector mins, maxs;
			modelinfo->GetModelBounds(pModel, mins, maxs);
			mins *= flScale;
			maxs *= flScale;

			matrix3x4_t mat;
			AngleMatrix(h.angles, h.origin, mat);
			if (flScale != 1.0f)
			{
				VectorScale(mat[0], flScale, mat[0]);
				VectorScale(mat[1], flScale, mat[1]);
				VectorScale(mat[2], flScale, mat[2]);
			}

			AppendModelTriangles(modelIndex, mat, mins, maxs, h.solid == SOLID_VPHYSICS, outTris);
			AddObstacle(mat, mins, maxs, pExtras);
			++nAdded;
		}

		Msg("[EZ2 NavMesh] Appended %d static props (+%d tris), %d models not found\n",
			nAdded, (int)(outTris.size() - beforeCount), nMissing);
	}

	// func_npc_clip is a brush entity (*N model) that blocks NPC movement; bake its
	// collision / bounding volume into the triangle soup before rasterization.
	void AppendNpcClipGeometry(std::vector<FullNavTriangle_t>& outTris, FullNavExtras_t* pExtras)
	{
		int nClips = 0;
		const size_t beforeCount = outTris.size();

		for (CBaseEntity* pEnt = gEntList.FirstEnt(); pEnt != NULL; pEnt = gEntList.NextEnt(pEnt))
		{
			if (!FClassnameIs(pEnt, "func_npc_clip"))
				continue;

			const int nModelIndex = pEnt->GetModelIndex();
			if (nModelIndex <= 0 || !modelinfo)
				continue;

			const model_t* pModel = modelinfo->GetModel(nModelIndex);
			if (!pModel)
				continue;

			Vector mins(0, 0, 0), maxs(0, 0, 0);
			if (pEnt->CollisionProp())
			{
				mins = pEnt->CollisionProp()->OBBMins();
				maxs = pEnt->CollisionProp()->OBBMaxs();
			}
			else
			{
				modelinfo->GetModelBounds(pModel, mins, maxs);
			}

			matrix3x4_t mat;
			AngleMatrix(pEnt->GetAbsAngles(), pEnt->GetAbsOrigin(), mat);

			AppendCollideOrBoxTriangles(nModelIndex, mat, mins, maxs, true, true, outTris);
			AddObstacle(mat, mins, maxs, pExtras);
			++nClips;
		}

		Msg("[EZ2 NavMesh] Appended %d func_npc_clip volumes (+%d tris)\n",
			nClips, (int)(outTris.size() - beforeCount));
	}

		// Node origins can sit a few units off the baked floor; Detour only links off-mesh
		// endpoints within walkableClimb vertically, so drop them onto the floor first
	static Vector SnapNodeToFloor(const Vector & p)
	{
		trace_t tr;
		UTIL_TraceLine(p + Vector(0, 0, 24), p - Vector(0, 0, 64), MASK_NPCSOLID_BRUSHONLY, NULL, COLLISION_GROUP_NONE, &tr);
		return (!tr.startsolid && tr.DidHit()) ? tr.endpos : p;
	}

	void CollectNodegraphJumpLinks(FullNavExtras_t& extras)
	{
		if (!g_pBigAINet || g_pBigAINet->NumNodes() <= 0)
		{
			Msg("[EZ2 NavMesh] No nodegraph loaded: full mesh will have no jump links.\n");
			return;
		}

		const int hull = 0;   // HULL_HUMAN, same default as ez2_nav_node_hull
		CAI_Network* pNet = g_pBigAINet;
		for (int i = 0; i < pNet->NumNodes(); ++i)
		{
			CAI_Node* pNode = pNet->GetNode(i);
			if (!pNode || pNode->GetType() != NODE_GROUND)
				continue;

			for (int l = 0; l < pNode->NumLinks(); ++l)
			{
				CAI_Link* pLink = pNode->GetLinkByIndex(l);
				if (!pLink || pLink->m_iSrcID != i)
					continue;

				CAI_Node* pDest = pNet->GetNode(pLink->DestNodeID(i));
				if (!pDest || pDest->GetType() != NODE_GROUND)
					continue;

				const int mt = pLink->m_iAcceptedMoveTypes[hull];
				if ((mt & bits_CAP_MOVE_GROUND) || !(mt & bits_CAP_MOVE_JUMP))
					continue;

				FullNavJumpLink_t j;
				j.a = SnapNodeToFloor(pNode->GetOrigin());
				j.b = SnapNodeToFloor(pDest->GetOrigin());
				extras.jumps.push_back(j);
			}
		}
	}

	void CollectNodegraphClimbs(FullNavExtras_t& extras)
	{
		const int hull = (ez2_nav_agent_hull.GetInt() >= 0) ? ez2_nav_agent_hull.GetInt() : 0;
		EZ2_NavCollectNodegraphClimbs(hull, extras.climbs);
		for (size_t i = 0; i < extras.climbs.size(); ++i)
		{
			extras.climbs[i].base = SnapNodeToFloor(extras.climbs[i].base);
			extras.climbs[i].top = SnapNodeToFloor(extras.climbs[i].top);
		}
		Msg("[EZ2 NavMesh] Collected %d ladder links from the nodegraph\n", (int)extras.climbs.size());
	}
}

//-----------------------------------------------------------------------------
// Extracts world triangles from maps/<mapName>.bsp. Returns false on failure.
//-----------------------------------------------------------------------------
static bool ExtractWorldTriangles(const char* pszMapName, std::vector<FullNavTriangle_t>& outTris, FullNavExtras_t* pExtras)
{
	outTris.clear();

	char szBspPath[MAX_PATH];
	Q_snprintf(szBspPath, sizeof(szBspPath), "maps/%s.bsp", pszMapName);

	CUtlBuffer buf;
	if (!filesystem->ReadFile(szBspPath, "GAME", buf))
	{
		Warning("[EZ2 NavMesh] Could not read '%s'\n", szBspPath);
		return false;
	}

	const unsigned char* pBase = (const unsigned char*)buf.Base();
	const int nSize = buf.TellPut();

	if (nSize < (int)sizeof(dheader_t))
		return false;

	const dheader_t* pHdr = reinterpret_cast<const dheader_t*>(pBase);
	if (pHdr->ident != IDBSPHEADER)
	{
		Warning("[EZ2 NavMesh] '%s' is not a valid BSP.\n", szBspPath);
		return false;
	}

	const Vector* pVerts = NULL;        int nVerts = 0;
	const dedge_t* pEdges = NULL;       int nEdges = 0;
	const int* pSurfEdges = NULL;       int nSurfEdges = 0;
	const dface_t* pFaces = NULL;       int nFaces = 0;
	const texinfo_t* pTexInfo = NULL;   int nTexInfo = 0;
	const dplane_t* pPlanes = NULL;     int nPlanes = 0;
	const dmodel_t* pModels = NULL;     int nModels = 0;
	const ddispinfo_t* pDispInfo = NULL;  int nDispInfo = 0;
	const CDispVert* pDispVerts = NULL; int nDispVerts = 0;
	const dtexdata_t* pTexData = NULL;     int nTexData = 0;
	const int* pTexStrTable = NULL;        int nTexStrTable = 0;
	const char* pTexStrData = NULL;        int nTexStrData = 0;

	if (!GetLump(pBase, nSize, pHdr, LUMP_TEXDATA, pTexData, nTexData) ||
		!GetLump(pBase, nSize, pHdr, LUMP_TEXDATA_STRING_TABLE, pTexStrTable, nTexStrTable) ||
		!GetLump(pBase, nSize, pHdr, LUMP_TEXDATA_STRING_DATA, pTexStrData, nTexStrData))
		return false;

	if (!GetLump(pBase, nSize, pHdr, LUMP_VERTEXES, pVerts, nVerts) ||
		!GetLump(pBase, nSize, pHdr, LUMP_EDGES, pEdges, nEdges) ||
		!GetLump(pBase, nSize, pHdr, LUMP_SURFEDGES, pSurfEdges, nSurfEdges) ||
		!GetLump(pBase, nSize, pHdr, LUMP_FACES, pFaces, nFaces) ||
		!GetLump(pBase, nSize, pHdr, LUMP_TEXINFO, pTexInfo, nTexInfo) ||
		!GetLump(pBase, nSize, pHdr, LUMP_PLANES, pPlanes, nPlanes) ||
		!GetLump(pBase, nSize, pHdr, LUMP_MODELS, pModels, nModels) ||
		!GetLump(pBase, nSize, pHdr, LUMP_DISPINFO, pDispInfo, nDispInfo) ||
		!GetLump(pBase, nSize, pHdr, LUMP_DISP_VERTS, pDispVerts, nDispVerts))
	{
		return false;
	}

	if (nModels < 1)
		return false;

	// Model 0 is the world
	const dmodel_t& world = pModels[0];
	const int firstFace = world.firstface;
	const int lastFace = world.firstface + world.numfaces;
	if (firstFace < 0 || lastFace > nFaces)
	{
		Warning("[EZ2 NavMesh] World model face range invalid.\n");
		return false;
	}

	std::vector<Vector> poly;
	int nDispFaces = 0;

	for (int fi = firstFace; fi < lastFace; ++fi)
	{
		const dface_t& f = pFaces[fi];

		if (f.numedges < 3)
			continue;
		if (f.planenum >= nPlanes)
			continue;

		if (f.texinfo >= 0 && f.texinfo < nTexInfo)
		{
			const texinfo_t& ti = pTexInfo[f.texinfo];
			const int skip = ti.flags & NAV_SKIP_SURF_FLAGS;
			if (skip & ~SURF_NODRAW)
				continue;   // sky, skip, hint, trigger
			if ((skip & SURF_NODRAW) &&
				!IsNpcClipTexInfo(ti, pTexData, nTexData, pTexStrTable, nTexStrTable, pTexStrData, nTexStrData))
				continue;   // ordinary nodraw, but keep npc clip faces
		}

		// Collect polygon vertices in winding order
		poly.clear();
		bool bOk = true;
		for (int i = 0; i < f.numedges; ++i)
		{
			const int seIdx = f.firstedge + i;
			if (seIdx < 0 || seIdx >= nSurfEdges) { bOk = false; break; }

			const int se = pSurfEdges[seIdx];
			const int eIdx = (se >= 0) ? se : -se;
			if (eIdx >= nEdges) { bOk = false; break; }

			const int vIdx = (se >= 0) ? pEdges[eIdx].v[0] : pEdges[eIdx].v[1];
			if (vIdx >= nVerts) { bOk = false; break; }

			poly.push_back(pVerts[vIdx]);
		}
		if (!bOk)
			continue;

		Vector faceNormal = pPlanes[f.planenum].normal;
		if (f.side)
			faceNormal = -faceNormal;

		// ---- Displacement face ----
		if (f.dispinfo >= 0)
		{
			if (f.dispinfo >= nDispInfo || poly.size() != 4)
				continue;

			const ddispinfo_t& di = pDispInfo[f.dispinfo];
			const int size = (1 << di.power) + 1;
			if (di.power < 2 || di.power > 4)
				continue;
			if (di.m_iDispVertStart < 0 || di.m_iDispVertStart + size * size > nDispVerts)
				continue;

			// Rotate corners so corner 0 is the one nearest the displacement start position
			int startCorner = 0;
			float bestDist = FLT_MAX;
			for (int c = 0; c < 4; ++c)
			{
				float d = (poly[c] - di.startPosition).LengthSqr();
				if (d < bestDist)
				{
					bestDist = d;
					startCorner = c;
				}
			}

			Vector pts[4];
			for (int c = 0; c < 4; ++c)
				pts[c] = poly[(startCorner + c) & 3];

			std::vector<Vector> grid(size * size);
			const float invStep = 1.0f / (float)(size - 1);

			for (int y = 0; y < size; ++y)
			{
				const Vector edge0 = pts[0] + (pts[3] - pts[0]) * (y * invStep);
				const Vector edge1 = pts[1] + (pts[2] - pts[1]) * (y * invStep);

				for (int x = 0; x < size; ++x)
				{
					const Vector base = edge0 + (edge1 - edge0) * (x * invStep);
					const CDispVert& dv = pDispVerts[di.m_iDispVertStart + y * size + x];
					grid[y * size + x] = base + dv.m_vVector * dv.m_flDist;
				}
			}

			for (int y = 0; y < size - 1; ++y)
			{
				for (int x = 0; x < size - 1; ++x)
				{
					const Vector& p00 = grid[y * size + x];
					const Vector& p10 = grid[y * size + x + 1];
					const Vector& p11 = grid[(y + 1) * size + x + 1];
					const Vector& p01 = grid[(y + 1) * size + x];

					EmitTri(outTris, p00, p10, p11, faceNormal);
					EmitTri(outTris, p00, p11, p01, faceNormal);
				}
			}

			++nDispFaces;
			continue;
		}

		// ---- Regular convex face: fan triangulation ----
		for (size_t i = 1; i + 1 < poly.size(); ++i)
		{
			EmitTri(outTris, poly[0], poly[i], poly[i + 1], faceNormal);
		}
	}

	Msg("[EZ2 NavMesh] Extracted %d triangles from '%s' (%d displacement faces)\n",
		(int)outTris.size(), szBspPath, nDispFaces);

	AppendStaticPropGeometry(pBase, nSize, pHdr, outTris, pExtras);

	return !outTris.empty();
}

//-----------------------------------------------------------------------------
// Shared soup: base geometry + extras, cached so runtime carving can rebuild
//-----------------------------------------------------------------------------
static std::vector<FullNavTriangle_t> s_SoupTris;
static FullNavExtras_t s_SoupExtras;
static FullNavBuildConfig_t s_SoupCfg;
static char s_SoupMap[128] = "";

static bool BuildSoup(const char* pszMap, std::vector<FullNavTriangle_t>& tris, FullNavExtras_t& extras)
{
	extras = FullNavExtras_t();
	extras.jumpRadius = ez2_nav_jump_radius.GetFloat();

	if (ez2_nav_climb_use_nodegraph.GetBool())
		CollectNodegraphClimbs(extras);

	if (!ExtractWorldTriangles(pszMap, tris, &extras))
		return false;

	AppendNonPhysicsPropGeometry(tris, &extras);
	AppendNpcClipGeometry(tris, &extras);
	if (ez2_nav_jump_use_nodegraph.GetBool())
		 CollectNodegraphJumpLinks(extras);
	EZ2_NavAppendEntityHints(extras.entityHints, 0, true);   // cover comes from BakeFullMeshCoverHints
	return true;
}

static float FitCellSize(const std::vector<FullNavTriangle_t>& tris, float cellSize)
{
	if (cellSize < 1.0f)
		cellSize = 1.0f;
	if (tris.empty())
		return cellSize;

	float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
	for (size_t i = 0; i < tris.size(); ++i)
	{
		const Vector* v[3] = { &tris[i].v0, &tris[i].v1, &tris[i].v2 };
		for (int k = 0; k < 3; ++k)
		{
			minX = MIN(minX, v[k]->x);
			maxX = MAX(maxX, v[k]->x);
			minY = MIN(minY, v[k]->y);
			maxY = MAX(maxY, v[k]->y);
		}
	}

	const float extX = maxX - minX;
	const float extY = maxY - minY;
	while ((extX / cellSize) * (extY / cellSize) > 4.0e6f)
	{
		cellSize *= 2.0f;
		Warning("[EZ2 NavMesh] Map is large; increasing cell size to %.1f\n", cellSize);
	}
	return cellSize;
}

static FullNavBuildConfig_t MakeBuildConfig(float cellSize)
{
	FullNavBuildConfig_t cfg;
	cfg.agentRadius = ez2_nav_agent_radius.GetFloat();
	cfg.agentHeight = ez2_nav_agent_height.GetFloat();
	cfg.agentMaxClimb = ez2_nav_agent_climb.GetFloat();
	cfg.agentMaxSlopeDeg = ez2_nav_agent_slope.GetFloat();
	cfg.cellSize = cellSize;
	cfg.cellHeight = ez2_nav_cell_height.GetFloat();
	return cfg;

	const int hull = ez2_nav_agent_hull.GetInt();
	if (hull >= 0 && hull < NUM_HULLS)
	{
		cfg.agentRadius = MAX(1.0f, NAI_Hull::Width((Hull_t)hull) * 0.5f);
		cfg.agentHeight = MAX(1.0f, NAI_Hull::Height((Hull_t)hull));
	}
}

//-----------------------------------------------------------------------------
// Carving: triangles are clipped in the carve box's local XY, so floor that
// straddles the box edge is split rather than removed whole.
//-----------------------------------------------------------------------------
namespace
{
	struct ClipPlane_t { float a, b, c; };   // keep where a*x + b*y - c <= 0

	void ClipPolyXY(const std::vector<Vector>& in, std::vector<Vector>& out, const ClipPlane_t& pl)
	{
		out.clear();
		const size_t n = in.size();
		for (size_t i = 0; i < n; ++i)
		{
			const Vector& P = in[i];
			const Vector& Q = in[(i + 1) % n];
			const float fp = pl.a * P.x + pl.b * P.y - pl.c;
			const float fq = pl.a * Q.x + pl.b * Q.y - pl.c;
			if (fp <= 0.0f)
				out.push_back(P);
			if ((fp < 0.0f && fq > 0.0f) || (fp > 0.0f && fq < 0.0f))
			{
				const float t = fp / (fp - fq);
				out.push_back(P + (Q - P) * t);
			}
		}
	}

	// Convex piece in box-centred local coords -> world triangle fan.
	// Rigid transform, so winding is preserved.
	void EmitCarvePiece(const std::vector<Vector>& piece, const matrix3x4_t& mat, const Vector& c,
		std::vector<FullNavTriangle_t>& out)
	{
		if (piece.size() < 3)
			return;

		std::vector<Vector> world(piece.size());
		for (size_t i = 0; i < piece.size(); ++i)
			VectorTransform(piece[i] + c, mat, world[i]);

		for (size_t i = 1; i + 1 < world.size(); ++i)
		{
			const Vector cr = (world[i] - world[0]).Cross(world[i + 1] - world[0]);
			if (cr.LengthSqr() < 1e-4f)
				continue;
			FullNavTriangle_t t;
			t.v0 = world[0];
			t.v1 = world[i];
			t.v2 = world[i + 1];
			out.push_back(t);
		}
	}

	void CarveTriangle(const FullNavTriangle_t& tri, const matrix3x4_t& mat, const Vector& c, const Vector& h,
		std::vector<FullNavTriangle_t>& out)
	{
		const Vector n = (tri.v1 - tri.v0).Cross(tri.v2 - tri.v0);
		const float len = n.Length();
		if (len < 1e-4f || fabsf(n.z) < 0.1f * len)
		{
			out.push_back(tri);   // walls and degenerate faces are never carved
			return;
		}

		Vector L[3];
		const Vector* src[3] = { &tri.v0, &tri.v1, &tri.v2 };
		for (int k = 0; k < 3; ++k)
		{
			VectorITransform(*src[k], mat, L[k]);
			L[k] -= c;
		}

		float minX = FLT_MAX, maxX = -FLT_MAX, minY = FLT_MAX, maxY = -FLT_MAX, minZ = FLT_MAX, maxZ = -FLT_MAX;
		for (int k = 0; k < 3; ++k)
		{
			minX = MIN(minX, L[k].x); maxX = MAX(maxX, L[k].x);
			minY = MIN(minY, L[k].y); maxY = MAX(maxY, L[k].y);
			minZ = MIN(minZ, L[k].z); maxZ = MAX(maxZ, L[k].z);
		}

		// Only floor at the prop's height is carved, not floors above or well below it
		if (minZ > h.z || maxZ < -h.z - 32.0f)
		{
			out.push_back(tri);
			return;
		}
		if (maxX < -h.x || minX > h.x || maxY < -h.y || minY > h.y)
		{
			out.push_back(tri);
			return;
		}

		// Triangle minus box = union of convex pieces outside each plane, in turn
		const ClipPlane_t box[4] = { {1, 0, h.x}, {-1, 0, h.x}, {0, 1, h.y}, {0, -1, h.y} };
		std::vector<Vector> rem(L, L + 3), keep, piece;
		for (int p = 0; p < 4 && rem.size() >= 3; ++p)
		{
			const ClipPlane_t outside = { -box[p].a, -box[p].b, -box[p].c };
			ClipPolyXY(rem, piece, outside);
			EmitCarvePiece(piece, mat, c, out);
			ClipPolyXY(rem, keep, box[p]);
			rem.swap(keep);
		}
		// whatever remains is inside the box and is dropped
	}

	void ApplyCarves(const std::vector<FullNavTriangle_t>& in, const std::vector<EZ2_NavCarve_t>& carves,
		std::vector<FullNavTriangle_t>& out)
	{
		out = in;
		for (size_t i = 0; i < carves.size(); ++i)
		{
			const EZ2_NavCarve_t& cv = carves[i];
			matrix3x4_t mat;
			AngleMatrix(cv.angles, cv.origin, mat);

			std::vector<FullNavTriangle_t> next;
			next.reserve(out.size());
			for (size_t t = 0; t < out.size(); ++t)
				CarveTriangle(out[t], mat, cv.center, cv.halfExt, next);
			out.swap(next);
		}
	}
}

//-----------------------------------------------------------------------------
// Runtime rebuild with carves applied. Does not write the .nav file.
//-----------------------------------------------------------------------------
bool EZ2_NavRebuildCarved(const std::vector<EZ2_NavCarve_t>& carves)
{
	if (!gpGlobals || gpGlobals->mapname == NULL_STRING)
		return false;
	const char* pszMap = STRING(gpGlobals->mapname);

	if (s_SoupTris.empty() || Q_stricmp(s_SoupMap, pszMap) != 0)
	{
		// Not built this session: extract now. Uses the cvar cell size, so a
		// build with a cellsize argument will differ. Rebuild with the command to fix.
		std::vector<FullNavTriangle_t> tris;
		FullNavExtras_t extras;
		if (!BuildSoup(pszMap, tris, extras))
			return false;

		s_SoupCfg = MakeBuildConfig(FitCellSize(tris, ez2_nav_cell_size.GetFloat()));
		s_SoupTris.swap(tris);
		s_SoupExtras = extras;
		Q_strncpy(s_SoupMap, pszMap, sizeof(s_SoupMap));
	}

	std::vector<FullNavTriangle_t> carved;
	ApplyCarves(s_SoupTris, carves, carved);
	return BuildAndSaveFullSoloNavMesh(pszMap, carved, s_SoupCfg, true, &s_SoupExtras, false);
}

//-----------------------------------------------------------------------------
// ez2_nav_build_full [cellsize]
//-----------------------------------------------------------------------------
CON_COMMAND(ez2_nav_build_full, "Builds and saves maps/<map>_full.nav from the current map's BSP: ez2_nav_build_full [cellsize]")
{
	if (!gpGlobals || gpGlobals->mapname == NULL_STRING)
	{
		Warning("[EZ2 NavMesh] No map loaded.\n");
		return;
	}
	const char* pszMap = STRING(gpGlobals->mapname);

	std::vector<FullNavTriangle_t> tris;
	FullNavExtras_t extras;
	if (!BuildSoup(pszMap, tris, extras))
	{
		Warning("[EZ2 NavMesh] Triangle extraction failed for '%s'.\n", pszMap);
		return;
	}

	Msg("[EZ2 NavMesh] Extras: %d prop obstacles, %d jump links, %d entity hints\n",
		(int)extras.obstacles.size(), (int)extras.jumps.size(), (int)extras.entityHints.size());

	float cellSize = ez2_nav_cell_size.GetFloat();
	if (args.ArgC() >= 2)
		cellSize = atof(args[1]);
	const FullNavBuildConfig_t cfg = MakeBuildConfig(FitCellSize(tris, cellSize));

	Msg("[EZ2 NavMesh] Building '%s' (%d tris, cs=%.1f ch=%.1f r=%.1f h=%.1f climb=%.1f slope=%.1f)...\n",
		pszMap, (int)tris.size(), cfg.cellSize, cfg.cellHeight, cfg.agentRadius, cfg.agentHeight,
		cfg.agentMaxClimb, cfg.agentMaxSlopeDeg);

	const double t0 = Plat_FloatTime();
	const bool bOk = BuildAndSaveFullSoloNavMesh(pszMap, tris, cfg, true, &extras, true);
	const double dt = Plat_FloatTime() - t0;

	if (bOk)
	{
		// Cache the clean soup so carved rebuilds don't re-extract
		s_SoupTris = tris;
		s_SoupExtras = extras;
		s_SoupCfg = cfg;
		Q_strncpy(s_SoupMap, pszMap, sizeof(s_SoupMap));
		Msg("[EZ2 NavMesh] Build succeeded in %.2fs. Set ez2_nav_mode 1 and run ez2_nav_status.\n", dt);
	}
	else
	{
		Warning("[EZ2 NavMesh] Build FAILED after %.2fs (see [Recast] output with developer 1).\n", dt);
	}
}