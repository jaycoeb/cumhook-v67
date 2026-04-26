#include "includes.h"

bool Hooks::InPrediction() {
    static Address ret1 = pattern::find(g_csgo.m_client_dll,
        XOR("84 C0 75 0B 8B 0D ? ? ? ? 8B 01 FF 50 4C"));

    static Address ret2 = pattern::find(g_csgo.m_client_dll,
        XOR("84 C0 75 08 57 8B CE E8 ? ? ? ? 8B 06"));

    // fallback immediately if anything critical missing
    if (!g_cl.m_local || !g_menu.main.visuals.novisrecoil.get() || !ret1 || !ret2)
        return g_hooks.m_prediction.GetOldMethod<InPrediction_t>(CPrediction::INPREDICTION)(this);

    Stack stack{};
    Address ret = stack.ReturnAddress();

    // only compare if patterns are valid
    if (ret == ret1)
        return true;

    if (ret == ret2) {
        ang_t* angles = stack.next().arg(0xC).to<ang_t*>();

        // stronger validation
        if (angles && std::isfinite(angles->x) && std::isfinite(angles->y)) {
            ang_t recoil =
                g_cl.m_local->m_viewPunchAngle() +
                (g_cl.m_local->m_aimPunchAngle() *
                    g_csgo.weapon_recoil_scale->GetFloat()) *
                g_csgo.view_recoil_tracking->GetFloat();

            // clamp instead of raw subtract (prevents crazy values)
            ang_t new_angles = *angles - recoil;
            new_angles.normalize();   // make sure your ang_t has this

            *angles = new_angles;
        }

        return true;
    }

    return g_hooks.m_prediction.GetOldMethod<InPrediction_t>(CPrediction::INPREDICTION)(this);
}

void Hooks::RunCommand(Entity* ent, CUserCmd* cmd, IMoveHelper* movehelper) {
	if (!ent || !cmd)
		return;

	// don't completely skip prediction
	if (cmd->m_tick >= std::numeric_limits<int>::max()) {
		cmd->m_tick = 0; // safer fallback than skipping
	}

	g_hooks.m_prediction.GetOldMethod<RunCommand_t>(CPrediction::RUNCOMMAND)(
		this, ent, cmd, movehelper);

	// validate before storing
	if (g_cl.m_local && ent == g_cl.m_local)
		g_netdata.store();
}