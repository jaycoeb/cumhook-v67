#include "includes.h"

LagCompensation g_lagcomp{};

bool LagCompensation::StartPrediction(AimPlayer* data) {
	// we have no data to work with.
	if (data->m_records.size() <= 0)
		return false;

	// meme.
	if (data->m_player->dormant())
		return false;

	// compute the true amount of updated records
	// since the last time the player entered pvs.
	size_t size{};

	for (const auto& it : data->m_records) {
		if (it->dormant())
			break;
		++size;
	}

	// get first record.
	LagRecord* record = data->m_records[0].get();

	// reset all prediction related variables.
	record->predict();

	// check if lc broken.
	if (size > 1 && ((record->m_origin - data->m_records[1]->m_origin).length_sqr() > 4096.f
		|| size > 2 && (data->m_records[1]->m_origin - data->m_records[2]->m_origin).length_sqr() > 4096.f))
		record->m_broke_lc = true;

	if (!record->m_broke_lc)
		return false;

	int simulation = game::TIME_TO_TICKS(record->m_sim_time);

	// this is too much lag to fix.
	if (std::abs(g_cl.m_arrival_tick - simulation) >= 128)
		return true;

	// FIX: take the larger of the two lag deltas instead of branching on size.
	// During a fakelag peek, size is often 2 and the recent delta understimates
	// the real choke — the previous interval is more reliable in that case.
	int lag_recent = game::TIME_TO_TICKS(record->m_sim_time - data->m_records[1]->m_sim_time);
	int lag_prev = (size > 2)
		? game::TIME_TO_TICKS(data->m_records[1]->m_sim_time - data->m_records[2]->m_sim_time)
		: lag_recent;

	int lag = std::max(lag_recent, lag_prev);
	math::clamp(lag, 1, 17);

	// get the delta in ticks between the last server net update
	// and the net update on which we created this record.
	int updatedelta = g_cl.m_server_tick - record->m_tick;

	// FIX: floor remaining at 1 so a large updatedelta on a fast peek
	// doesn't make this go negative and bail out of prediction entirely.
	int remaining = std::max(lag - updatedelta, 1);

	if (g_cl.m_latency_ticks <= remaining)
		return true;

	// the next update will come in, wait for it.
	int next = record->m_tick + 1;
	if (next + lag >= g_cl.m_arrival_tick)
		return true;

	float change = 0.f, dir = 0.f;

	if (record->m_velocity.y != 0.f || record->m_velocity.x != 0.f)
		dir = math::rad_to_deg(std::atan2(record->m_velocity.y, record->m_velocity.x));

	float dt = record->m_sim_time - data->m_records[1]->m_sim_time;
	float prevdir = 0.f;

	change = (math::NormalizedAngle(dir - prevdir) / dt) * g_csgo.m_globals->m_interval;

	CCSGOPlayerAnimState* state = data->m_player->m_PlayerAnimState();

	CCSGOPlayerAnimState backup{};
	if (state)
		std::memcpy(&backup, state, sizeof(CCSGOPlayerAnimState));

	int pred = 0;

	for (int sim{}; sim < lag; ++sim) {
		dir = math::NormalizedAngle(dir + change);

		float hyp = record->m_pred_velocity.length_2d();

		record->m_pred_velocity.x = std::cos(math::deg_to_rad(dir)) * hyp;
		record->m_pred_velocity.y = std::sin(math::deg_to_rad(dir)) * hyp;

		if (record->m_pred_flags & FL_ONGROUND) {
			if (!g_csgo.sv_enablebunnyhopping->GetInt()) {
				float max = data->m_player->m_flMaxspeed() * 1.1f;
				float speed = record->m_pred_velocity.length();

				if (max > 0.f && speed > max)
					record->m_pred_velocity *= (max / speed);
			}

			record->m_pred_velocity.z = g_csgo.sv_jump_impulse->GetFloat();
		}
		else {
			record->m_pred_velocity.z -= g_csgo.sv_gravity->GetFloat() * g_csgo.m_globals->m_interval;

			float speed2d = record->m_pred_velocity.length_2d();
			float ideal = (speed2d > 0.f) ? math::rad_to_deg(std::asin(15.f / speed2d)) : 90.f;
			math::clamp(ideal, 0.f, 90.f);

			float smove = 0.f;
			float abschange = std::abs(change);

			if (abschange <= ideal || abschange >= 30.f) {
				static float mod{ 1.f };
				dir += (ideal * mod);
				smove = 450.f * mod;
				mod *= -1.f;
			}
			else if (change > 0.f) {
				smove = -450.f;
			}
			else {
				smove = 450.f;
			}

			AirAccelerate(record, ang_t{ 0.f, dir, 0.f }, 0.f, smove);
		}

		PlayerMove(record);

		record->m_pred_time += g_csgo.m_globals->m_interval;
		++pred;

		if (sim == 0 && state)
			PredictAnimations(state, record);
	}
}

void LagCompensation::PlayerMove(LagRecord* record) {
	vec3_t                start, end, normal;
	CGameTrace            trace;
	CTraceFilterWorldOnly filter;

	// FIX: was incorrectly writing to m_velocity instead of m_pred_velocity,
	// causing air prediction to diverge from the movement integration below.
	if (record->m_pred_flags & FL_ONGROUND)
		record->m_pred_velocity.z = g_csgo.sv_jump_impulse->GetFloat();
	else
		record->m_pred_velocity.z -= g_csgo.sv_gravity->GetFloat() * g_csgo.m_globals->m_interval;

	int m_choked_ticks = std::clamp(game::TIME_TO_TICKS(record->m_lag), 1, 16);

	start = record->m_origin;
	end = start + ((record->m_pred_velocity * g_csgo.m_globals->m_interval) * m_choked_ticks);

	g_csgo.m_engine_trace->TraceRay(Ray(start, end, record->m_mins, record->m_maxs), CONTENTS_SOLID, &filter, &trace);

	if (trace.m_fraction != 1.f) {
		for (int i{}; i < 2; ++i) {
			record->m_pred_velocity -= trace.m_plane.m_normal * record->m_pred_velocity.dot(trace.m_plane.m_normal);

			float adjust = record->m_pred_velocity.dot(trace.m_plane.m_normal);
			if (adjust < 0.f)
				record->m_pred_velocity -= (trace.m_plane.m_normal * adjust);

			start = trace.m_endpos;
			end = start + (record->m_pred_velocity * (g_csgo.m_globals->m_interval * (1.f - trace.m_fraction)));

			g_csgo.m_engine_trace->TraceRay(Ray(start, end, record->m_mins, record->m_maxs), CONTENTS_SOLID, &filter, &trace);
			if (trace.m_fraction == 1.f)
				break;
		}
	}

	start = end = record->m_origin = trace.m_endpos;
	end.z -= 2.2f;

	g_csgo.m_engine_trace->TraceRay(Ray(start, end, record->m_mins, record->m_maxs), CONTENTS_SOLID, &filter, &trace);

	record->m_flags &= ~FL_ONGROUND;

	if (trace.m_fraction != 1.f && trace.m_plane.m_normal.z > 0.7f)
		record->m_flags |= FL_ONGROUND;
}

void LagCompensation::AirAccelerate(LagRecord* record, ang_t angle, float fmove, float smove) {
	vec3_t fwd, right, wishvel, wishdir;
	float  maxspeed, wishspd, wishspeed, currentspeed, addspeed, accelspeed;

	math::AngleVectors(angle, &fwd, &right);

	fwd.z = 0.f;
	right.z = 0.f;

	fwd.normalize();
	right.normalize();

	for (int i{}; i < 2; ++i)
		wishvel[i] = (fwd[i] * fmove) + (right[i] * smove);

	wishvel.z = 0.f;

	wishdir = wishvel;
	wishspeed = wishdir.normalize();

	maxspeed = record->m_player->m_flMaxspeed();

	if (wishspeed != 0.f && wishspeed > maxspeed)
		wishspeed = maxspeed;

	wishspd = wishspeed;

	if (wishspd > 40.f)
		wishspd = 40.f;

	currentspeed = record->m_pred_velocity.dot(wishdir);
	addspeed = wishspd - currentspeed;

	if (addspeed <= 0.f)
		return;

	accelspeed = g_csgo.sv_airaccelerate->GetFloat() * wishspeed * g_csgo.m_globals->m_interval;

	if (accelspeed > addspeed)
		accelspeed = addspeed;

	// FIX: removed 'rand() % -20 + 50' (undefined behaviour) and the
	// scalar addition to vec3_t which was corrupting velocity for air targets.
	record->m_pred_velocity += wishdir * accelspeed;
}

void LagCompensation::PredictAnimations(CCSGOPlayerAnimState* state, LagRecord* record) {
	struct AnimBackup_t {
		float  curtime;
		float  frametime;
		int    flags;
		int    eflags;
		vec3_t velocity;
	};

	Player* player = record->m_player;

	AnimBackup_t backup;
	backup.curtime = g_csgo.m_globals->m_curtime;
	backup.frametime = g_csgo.m_globals->m_frametime;
	backup.flags = player->m_fFlags();
	backup.eflags = player->m_iEFlags();
	backup.velocity = player->m_vecAbsVelocity();

	g_csgo.m_globals->m_curtime = record->m_pred_time;
	g_csgo.m_globals->m_frametime = g_csgo.m_globals->m_interval;

	// EFL_DIRTY_ABSVELOCITY — skip call to C_BaseEntity::CalcAbsoluteVelocity
	player->m_iEFlags() &= ~0x1000;

	player->m_fFlags() = record->m_pred_flags;
	player->m_vecAbsVelocity() = record->m_pred_velocity;

	if (state->m_frame >= g_csgo.m_globals->m_frame)
		state->m_frame = g_csgo.m_globals->m_frame - 1;

	bool fake = g_menu.main.aimbot.correct.get();

	if (fake)
		g_resolver.ResolveAngles(player, record);

	game::UpdateAnimationState(state, record->m_eye_angles);

	if (fake)
		g_resolver.ResolvePoses(player, record);

	player->GetPoseParameters(record->m_poses);
	player->GetAnimLayers(record->m_layers);
	record->m_abs_ang = player->GetAbsAngles();

	g_csgo.m_globals->m_curtime = backup.curtime;
	g_csgo.m_globals->m_frametime = backup.frametime;

	player->m_fFlags() = backup.flags;
	player->m_iEFlags() = backup.eflags;
	player->m_vecAbsVelocity() = backup.velocity;
}