#include "includes.h"

bool Hooks::ShouldDrawParticles() {
	return g_hooks.m_client_mode.GetOldMethod< ShouldDrawParticles_t >(IClientMode::SHOULDDRAWPARTICLES)(this);
}

bool Hooks::ShouldDrawFog() {
	// remove fog.
	if (g_menu.main.visuals.nofog.get())
		return false;

	return g_hooks.m_client_mode.GetOldMethod< ShouldDrawFog_t >(IClientMode::SHOULDDRAWFOG)(this);
}

void Hooks::OverrideView(CViewSetup* view) {
	// damn son.
	g_cl.m_local = g_csgo.m_entlist->GetClientEntity< Player* >(g_csgo.m_engine->GetLocalPlayer());

	// g_grenades.think( );
	g_visuals.ThirdpersonThink();

	// call original.
	g_hooks.m_client_mode.GetOldMethod< OverrideView_t >(IClientMode::OVERRIDEVIEW)(this, view);

	// remove scope edge blur.
	if (g_menu.main.visuals.noscope.get()) {
		if (g_cl.m_local && g_cl.m_local->m_bIsScoped())
			view->m_edge_blur = 0;
	}
}

bool Hooks::CreateMove(float time, CUserCmd* cmd) {
	Stack   stack;
	bool    ret;

	// let original run first.
	ret = g_hooks.m_client_mode.GetOldMethod< CreateMove_t >(IClientMode::CREATEMOVE)(this, time, cmd);
	if (!cmd || !cmd->m_command_number)
		return ret;


	if (g_csgo.m_engine->ISCONNECTED && g_csgo.m_engine->ISINGAME)
	{
		// if we arrived here, called from -> CInput::CreateMove
		// call EngineClient::SetViewAngles according to what the original returns.
		if (ret)
			g_csgo.m_engine->SetViewAngles(cmd->m_view_angles);

		// random_seed isn't generated in ClientMode::CreateMove yet, we must set generate it ourselves.
		cmd->m_random_seed = g_csgo.MD5_PseudoRandom(cmd->m_command_number) & 0x7fffffff;

		// get bSendPacket off the stack.
		g_cl.m_packet = stack.next().local(0x1c).as< bool* >();

		// get bFinalTick off the stack.
		g_cl.m_final_packet = stack.next().local(0x1b).as< bool* >();

		if (callbacks::IsDoubleTapOn() && g_aimbot.m_dt.armed) {
			// Safety: disarm if too much time passed (missed fire window).
			if (g_csgo.m_globals->m_curtime - g_aimbot.m_dt.arm_time > 0.3f) {
				g_aimbot.m_dt.armed = false;
			}
			else {
				*g_cl.m_packet = true;
				*g_cl.m_final_packet = true;
				cmd->m_view_angles = g_aimbot.m_dt.arm_angle;
				cmd->m_tick = g_aimbot.m_dt.arm_tick;
				cmd->m_random_seed = g_aimbot.m_dt.arm_seed;
				cmd->m_buttons |= IN_ATTACK;
				g_aimbot.m_dt.armed = false;

				math::NormalizeAngle(cmd->m_view_angles.y);

				// Skip OnTick entirely — nothing else should touch
				// this tick, we just need the flush to go through.
				return false;
			}
		}

		// invoke move function.
		g_cl.OnTick(cmd);

	}
	return false;
}

bool Hooks::DoPostScreenSpaceEffects(CViewSetup* setup) {
	g_visuals.RenderGlow();

	return g_hooks.m_client_mode.GetOldMethod< DoPostScreenSpaceEffects_t >(IClientMode::DOPOSTSPACESCREENEFFECTS)(this, setup);
}