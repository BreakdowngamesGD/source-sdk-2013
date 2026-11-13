//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Combine Grunt - a plain combine_s soldier that bodyguards the
//			Ordinal (first priority), Wallhammer and Suppressor. Grunts attach
//			themselves to a VIP, crowd around it and fight from there.
//
//=============================================================================//

#ifndef NPC_COMBINE_GRUNT_H
#define NPC_COMBINE_GRUNT_H

#ifdef _WIN32
#pragma once
#endif

#include "npc_combines.h"

//=========================================================
//	>> CNPC_CombineGrunt
//=========================================================
class CNPC_CombineGrunt : public CNPC_CombineS
{
	DECLARE_CLASS(CNPC_CombineGrunt, CNPC_CombineS);
	DECLARE_DATADESC();

public:
	void		Spawn(void);
	void		PrescheduleThink(void);

	// Used by other grunts to spread themselves across VIPs
	CBaseEntity* GetProtectee(void) { return m_hProtectee.Get(); }

private:
	CBaseEntity* FindBestProtectee(bool bOrdinalOnly);
	int				CountProtectors(CBaseEntity* pVip);
	void			SetProtectee(CBaseEntity* pVip);
	void			JoinProtecteeSquad(CBaseEntity* pVip);

	EHANDLE		m_hProtectee;
	float		m_flNextProtecteeCheck;
	float		m_flNextOrdinalCheck;
};

#endif // NPC_COMBINE_GRUNT_H