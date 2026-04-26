#include "includes.h"

LagCompensation g_lagcomp{};

#define LAG_COMPENSATION_TELEPORTED_DISTANCE_SQR ( 64.0f * 64.0f )

bool ExtrapolateShort(const LagRecord* last, LagRecord& out) {
	if (!last)
		return false;

	float interval = g_csgo.m_globals->m_interval;
	float dt = g_csgo.m_globals->m_curtime - last->m_sim_time;

	// ❗ hard limit: only allow 1–2 ticks
	float max_dt = interval * 2.f;
	dt = std::clamp(dt, 0.f, max_dt);

	// copy base
	out = *last;

	// clamp velocity (safety)
	vec3_t vel = last->m_velocity;
	float speed = vel.length();

	float max_speed = 300.f;
	if (speed > max_speed)
		vel *= (max_speed / speed);

	// simple forward step
	out.m_origin = last->m_origin + vel * dt;
	out.m_velocity = vel;
	out.m_sim_time = last->m_sim_time + dt;

	// ❗ reject if movement is too large (teleport-like)
	if ((out.m_origin - last->m_origin).length_sqr() > 4096.f) {
		out.m_origin = last->m_origin;
		return false;
	}

	return true;
}

bool LagCompensation::StartPrediction(AimPlayer* data) {
	if (data->m_records.empty())
		return false;

	if (data->m_player->dormant())
		return false;

	size_t size{};
	for (const auto& it : data->m_records) {
		if (it->dormant())
			break;
		++size;
	}

	LagRecord* record = data->m_records[0].get();

	// =========================
	// INTERPOLATION FIRST
	// =========================
	LagRecord interpolated;
	if (InterpolateRecord(data, interpolated)) {
		*record = interpolated;
		record->invalidate();
		g_bones.setup(data->m_player, nullptr, record);
		return true;
	}

	// =========================
	// SHORT EXTRAPOLATION (SAFE)
	// =========================
	float dt_now = g_csgo.m_globals->m_curtime - record->m_sim_time;
	if (dt_now <= g_csgo.m_globals->m_interval * 2.f) {
		LagRecord extrapolated;
		if (ExtrapolateShort(record, extrapolated)) {
			*record = extrapolated;
			record->invalidate();
			g_bones.setup(data->m_player, nullptr, record);
			return true;
		}
	}

	// =========================
	// PREP
	// =========================
	record->predict();

	float dt = 0.f;
	float confidence = 1.f;

	if (size > 1) {
		dt = record->m_sim_time - data->m_records[1]->m_sim_time;

		if (dt > 0.f) {
			// compute confidence EARLY
			float ideal = g_csgo.m_globals->m_interval;
			float jitter = std::abs(dt - ideal);
			float jitter_ratio = std::clamp(jitter / ideal, 0.f, 1.f);

			confidence = 1.f - jitter_ratio;

			float dist = (record->m_origin - data->m_records[1]->m_origin).length();
			if (dist > 64.f)
				confidence *= 0.5f;

			confidence = std::clamp(confidence, 0.1f, 1.f);

			// velocity
			record->m_velocity = (record->m_origin - data->m_records[1]->m_origin) / dt;

			float max_speed = 300.f * (0.5f + 0.5f * confidence);

			float speed = record->m_velocity.length();
			if (speed > max_speed)
				record->m_velocity *= (max_speed / speed);

			record->m_pred_velocity = record->m_velocity;
		}
	}

	// =========================
	// BROKEN LC CHECK
	// =========================
	if (size > 1 && ((record->m_origin - data->m_records[1]->m_origin).length_sqr() > LAG_COMPENSATION_TELEPORTED_DISTANCE_SQR
		|| size > 2 && (data->m_records[1]->m_origin - data->m_records[2]->m_origin).length_sqr() > LAG_COMPENSATION_TELEPORTED_DISTANCE_SQR))
		record->m_broke_lc = true;

	if (!record->m_broke_lc)
		return false;

	int simulation = game::TIME_TO_TICKS(record->m_sim_time);

	if (std::abs(g_cl.m_arrival_tick - simulation) >= 128)
		return true;

	int lag = game::TIME_TO_TICKS(record->m_sim_time - data->m_records[1]->m_sim_time);

	if (lag <= 0 && size > 2)
		lag = game::TIME_TO_TICKS(data->m_records[1]->m_sim_time - data->m_records[2]->m_sim_time);

	int max_predict = static_cast<int>(6 * confidence);
	max_predict = std::clamp(max_predict, 1, 6);

	lag = std::clamp(lag, 1, max_predict);

	int updatedelta = g_cl.m_server_tick - record->m_tick;

	if (g_cl.m_latency_ticks <= lag - updatedelta && !record->m_broke_lc)
		return true;

	int next = record->m_tick + 1;
	if (next + lag >= g_cl.m_arrival_tick)
		return true;

	float change = 0.f, dir = 0.f;

	if (record->m_velocity.y != 0.f || record->m_velocity.x != 0.f)
		dir = math::rad_to_deg(std::atan2(record->m_velocity.y, record->m_velocity.x));

	if (size > 1 && dt > 0.f) {
		float prevdir = 0.f;

		if (data->m_records[1]->m_velocity.y != 0.f || data->m_records[1]->m_velocity.x != 0.f)
			prevdir = math::rad_to_deg(std::atan2(data->m_records[1]->m_velocity.y, data->m_records[1]->m_velocity.x));

		change = math::NormalizedAngle(dir - prevdir) / game::TIME_TO_TICKS(dt);

		// dampen turning
		change *= confidence;
	}

	if (std::abs(change) > 45.f)
		change = 0.f;

	CCSGOPlayerAnimState* state = data->m_player->m_PlayerAnimState();
	CCSGOPlayerAnimState backup{};
	if (state)
		std::memcpy(&backup, state, sizeof(CCSGOPlayerAnimState));

	int pred = 0;

	while (true) {
		next += std::min(lag, 4); // reduced from 8
		if (next >= g_cl.m_arrival_tick)
			break;

		for (int sim{}; sim < lag; ++sim) {
			if (confidence < 0.2f)
				break;

			dir = math::NormalizedAngle(dir + change);

			float hyp = record->m_pred_velocity.length_2d();

			record->m_pred_velocity.x = std::cos(math::deg_to_rad(dir)) * hyp;
			record->m_pred_velocity.y = std::sin(math::deg_to_rad(dir)) * hyp;

			if (!(record->m_pred_flags & FL_ONGROUND)) {
				record->m_pred_velocity.z -= g_csgo.sv_gravity->GetFloat() * g_csgo.m_globals->m_interval;
			}

			PlayerMove(record);

			record->m_pred_time += g_csgo.m_globals->m_interval;
			++pred;

			if (sim == 0 && state)
				PredictAnimations(state, record);
		}
	}

	if (state)
		std::memcpy(state, &backup, sizeof(CCSGOPlayerAnimState));

	if (pred <= 0)
		return true;

	record->invalidate();
	g_bones.setup(data->m_player, nullptr, record);

	return true;
}

void LagCompensation::PlayerMove(LagRecord* record) {
	vec3_t                start, end;
	CGameTrace            trace;
	CTraceFilterWorldOnly filter;

	start = record->m_pred_origin;
	end = start + (record->m_pred_velocity * g_csgo.m_globals->m_interval);

	g_csgo.m_engine_trace->TraceRay(
		Ray(start, end, record->m_mins, record->m_maxs),
		CONTENTS_SOLID, &filter, &trace
	);

	// handle collision
	if (trace.m_fraction != 1.f) {
		for (int i = 0; i < 4; ++i) { // was 2 → increase for stability
			// slide along plane
			float dot = record->m_pred_velocity.dot(trace.m_plane.m_normal);
			record->m_pred_velocity -= trace.m_plane.m_normal * dot;

			// kill tiny movement (prevents jitter)
			if (record->m_pred_velocity.length_sqr() < 1.f)
				record->m_pred_velocity = vec3_t{};

			start = trace.m_endpos;
			end = start + record->m_pred_velocity * (g_csgo.m_globals->m_interval * (1.f - trace.m_fraction));

			g_csgo.m_engine_trace->TraceRay(
				Ray(start, end, record->m_mins, record->m_maxs),
				CONTENTS_SOLID, &filter, &trace
			);

			if (trace.m_fraction == 1.f)
				break;
		}
	}

	// set final origin directly from trace (more accurate)
	record->m_pred_origin = trace.m_endpos;

	// ===== GROUND CHECK =====
	start = record->m_pred_origin;
	end = start;
	end.z -= 2.f;

	g_csgo.m_engine_trace->TraceRay(
		Ray(start, end, record->m_mins, record->m_maxs),
		CONTENTS_SOLID, &filter, &trace
	);

	record->m_pred_flags &= ~FL_ONGROUND;

	if (trace.m_fraction < 1.f && trace.m_plane.m_normal.z > 0.7f)
		record->m_pred_flags |= FL_ONGROUND;
}

void LagCompensation::AirAccelerate(LagRecord* record, ang_t angle, float fmove, float smove) {
	vec3_t fwd, right, wishvel, wishdir;
	float  maxspeed, wishspd, wishspeed, currentspeed, addspeed, accelspeed;

	// determine movement angles.
	math::AngleVectors(angle, &fwd, &right);

	// zero out z components of movement vectors.
	fwd.z = 0.f;
	right.z = 0.f;

	// normalize remainder of vectors.
	fwd.normalize();
	right.normalize();


	for (int i{}; i < 2; ++i)       // Determine x and y parts of velocity
		wishvel[i] = fwd[i] * fmove + right[i] * smove;
	wishvel[2] = 0;             // Zero out z part of velocity
	// zero out z part of velocity.
	//wishvel.z = 0.f;

	// determine maginitude of speed of move.
	wishdir = wishvel;
	wishspeed = wishdir.normalize();

	// get maxspeed.
	// TODO; maybe global this or whatever its 260 anyway always.
	maxspeed = record->m_player->m_flMaxspeed();

	// clamp to server defined max speed.
	if (wishspeed != 0.f && wishspeed > maxspeed)
		wishspeed = maxspeed;

	// make copy to preserve original variable.
	wishspd = wishspeed;

	// cap speed.
	if (wishspd > 30.f)
		wishspd = 30.f;

	// determine veer amount.
	currentspeed = record->m_pred_velocity.dot(wishdir);

	// see how much to add.
	addspeed = wishspd - currentspeed;

	// if not adding any, done.
	if (addspeed <= 0)
		return;

	// Determine acceleration speed after acceleration
	accelspeed = g_csgo.sv_airaccelerate->GetFloat() * wishspeed * g_csgo.m_globals->m_frametime * record->m_player->m_surfaceFriction();

	// cap it.
	if (accelspeed > addspeed)
		accelspeed = addspeed;

	for (int i = 0; i < 3; i++)
	{
		record->m_pred_velocity[i] += (accelspeed * wishdir[i]);
	}
	//record->m_pred_velocity += (accelspeed * wishdir);
}

void LagCompensation::PredictAnimations(CCSGOPlayerAnimState* state, LagRecord* record) {
	struct AnimBackup_t {
		float  curtime;
		float  frametime;
		int    flags;
		int    eflags;
		vec3_t velocity;
	};

	// get player ptr.
	Player* player = record->m_player;

	// backup data.
	AnimBackup_t backup;
	backup.curtime = g_csgo.m_globals->m_curtime;
	backup.frametime = g_csgo.m_globals->m_frametime;
	backup.flags = player->m_fFlags();
	backup.eflags = player->m_iEFlags();
	backup.velocity = player->m_vecAbsVelocity();

	// set globals appropriately for animation.
	g_csgo.m_globals->m_curtime = record->m_pred_time;
	g_csgo.m_globals->m_frametime = g_csgo.m_globals->m_interval;

	// EFL_DIRTY_ABSVELOCITY
	// skip call to C_BaseEntity::CalcAbsoluteVelocity
	player->m_iEFlags() &= ~0x1000;

	// set predicted flags and velocity.
	player->m_fFlags() = record->m_pred_flags;
	player->m_vecAbsVelocity() = record->m_pred_velocity;

	// enable re-animation in the same frame if animated already.
	if (state->m_frame >= g_csgo.m_globals->m_frame)
		state->m_frame = g_csgo.m_globals->m_frame - 1;

	bool fake = g_menu.main.aimbot.correct.get();

	// rerun the resolver since we edited the origin.
	if (fake)
		g_resolver.ResolveAngles(player, record);

	// update animations.
	game::UpdateAnimationState(state, record->m_eye_angles);

	// rerun the pose correction cuz we are re-setupping them.
	if (fake)
		g_resolver.ResolvePoses(player, record);

	// get new rotation poses and layers.
	player->GetPoseParameters(record->m_poses);
	player->GetAnimLayers(record->m_layers);
	record->m_abs_ang = player->GetAbsAngles();

	// restore globals.
	g_csgo.m_globals->m_curtime = backup.curtime;
	g_csgo.m_globals->m_frametime = backup.frametime;

	// restore player data.
	player->m_fFlags() = backup.flags;
	player->m_iEFlags() = backup.eflags;
	player->m_vecAbsVelocity() = backup.velocity;
}

bool LagCompensation::InterpolateRecord(AimPlayer* data, LagRecord& record) {
	if (data->m_records.size() < 2)
		return false;

	float lerp = game::GetClientInterpAmount();
	float render_time = g_csgo.m_globals->m_curtime - lerp;

	LagRecord* prev = nullptr;
	LagRecord* next = nullptr;

	for (size_t i = 0; i < data->m_records.size() - 1; i++) {
		auto& r0 = data->m_records[i];
		auto& r1 = data->m_records[i + 1];

		if (r0->m_sim_time <= render_time && r1->m_sim_time >= render_time) {
			prev = r0.get();
			next = r1.get();
			break;
		}
	}

	if (!prev || !next)
		return false;

	float dt = next->m_sim_time - prev->m_sim_time;
	if (dt <= 0.f)
		return false;

	float t = (render_time - prev->m_sim_time) / dt;
	t = std::clamp(t, 0.f, 1.f);

	// teleport check
	if ((next->m_origin - prev->m_origin).length_sqr() > 4096.f) {
		record = *next;
		return true;
	}

	// linear interpolation
	record = *prev;
	record.m_origin = prev->m_origin + (next->m_origin - prev->m_origin) * t;
	record.m_velocity = prev->m_velocity + (next->m_velocity - prev->m_velocity) * t;
	record.m_sim_time = render_time;

	return true;
}

