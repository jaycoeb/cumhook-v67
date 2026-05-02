#include "includes.h"

Movement g_movement{ };

void Movement::JumpRelated( ) {
	if( g_cl.m_local->m_MoveType( ) == MOVETYPE_NOCLIP )
		return;

	if( ( g_cl.m_cmd->m_buttons & IN_JUMP ) && !( g_cl.m_flags & FL_ONGROUND ) ) {
		// bhop.
		if( g_menu.main.movement.bhop.get( ) )
			g_cl.m_cmd->m_buttons &= ~IN_JUMP;

		// duck jump ( crate jump ).
		if( g_menu.main.movement.airduck.get( ) )
			g_cl.m_cmd->m_buttons |= IN_DUCK;
	}
}

void Movement::Strafe() {
	vec3_t velocity;
	float  delta, abs_delta, velocity_angle, velocity_delta, correct;

	if (g_cl.m_local->m_MoveType() == MOVETYPE_NOCLIP || g_cl.m_local->m_MoveType() == MOVETYPE_LADDER)
		return;

	if ((g_cl.m_buttons & IN_SPEED) || !(g_cl.m_buttons & IN_JUMP) || (g_cl.m_flags & FL_ONGROUND))
		return;

	velocity = g_cl.m_local->m_vecVelocity();
	m_speed = velocity.length_2d();

	m_ideal = (m_speed > 0.f) ? math::rad_to_deg(std::asin(15.f / m_speed)) : 90.f;
	m_ideal2 = (m_speed > 0.f) ? math::rad_to_deg(std::asin(30.f / m_speed)) : 90.f;

	math::clamp(m_ideal, 0.f, 90.f);
	math::clamp(m_ideal2, 0.f, 90.f);

	m_mins = g_cl.m_local->m_vecMins();
	m_maxs = g_cl.m_local->m_vecMaxs();
	m_origin = g_cl.m_local->m_vecOrigin();

	m_switch_value *= -1.f;
	++m_strafe_index;

	g_cl.m_cmd->m_forward_move = 0.f;

	// =========================================================
	// ALIGN STRAFER
	// =========================================================
	if (g_input.GetKeyState(g_menu.main.movement.astrafe.get())) {
		float angle = std::max(m_ideal2, 4.f);
		if (angle > m_ideal2 && !(m_strafe_index % 5))
			angle = m_ideal2;

		m_circle_yaw = math::NormalizedAngle(m_circle_yaw + angle);
		g_cl.m_cmd->m_view_angles.y = m_circle_yaw;
		g_cl.m_cmd->m_side_move = -450.f;
		return;
	}

	// =========================================================
	// CIRCLE STRAFER
	// =========================================================
	if (g_input.GetKeyState(g_menu.main.movement.cstrafe.get())) {
		if (!g_menu.main.movement.airduck.get())
			g_cl.m_cmd->m_buttons |= IN_DUCK;

		DoPrespeed();
		return;
	}

	// =========================================================
	// Z STRAFER
	// =========================================================
	if (g_input.GetKeyState(g_menu.main.movement.zstrafe.get())) {
		float freq = (g_menu.main.movement.z_freq.get() * 0.2f) * g_csgo.m_globals->m_realtime;
		float factor = g_menu.main.movement.z_dist.get() * 0.5f;
		g_cl.m_cmd->m_view_angles.y += (factor * std::sin(freq));
	}

	if (!g_menu.main.movement.autostrafe.get())
		return;

	// =========================================================
	// WASD-AWARE AUTO STRAFER
	// Reads which movement keys are held and derives a wished
	// direction from them. Strafes optimally toward that direction
	// so W+D circles right, S alone strafes backward, etc.
	// =========================================================

	// Build wish direction from held keys, same as engine does.
	bool w = g_cl.m_buttons & IN_FORWARD;
	bool s = g_cl.m_buttons & IN_BACK;
	bool a = g_cl.m_buttons & IN_MOVELEFT;
	bool d = g_cl.m_buttons & IN_MOVERIGHT;

	float wish_x = 0.f, wish_y = 0.f;
	if (w) wish_x += 1.f;
	if (s) wish_x -= 1.f;
	if (d) wish_y -= 1.f; // engine: right = negative sidemove
	if (a) wish_y += 1.f;

	bool any_key = w || s || a || d;

	// Mouse delta as fallback when no keys held.
	delta = math::NormalizedAngle(g_cl.m_cmd->m_view_angles.y - m_old_yaw);
	abs_delta = std::abs(delta);
	m_circle_yaw = m_old_yaw = g_cl.m_cmd->m_view_angles.y;

	if (any_key) {
		// Convert wish vector to an angle relative to view.
		float wish_yaw_local = math::rad_to_deg(std::atan2(wish_y, wish_x));

		// Absolute wish direction in world space.
		float wish_dir = math::NormalizedAngle(g_cl.m_cmd->m_view_angles.y + wish_yaw_local);

		// Velocity direction in world space.
		velocity_angle = (m_speed > 0.f)
			? math::rad_to_deg(std::atan2(velocity.y, velocity.x))
			: wish_dir;

		// Delta between where we want to go and where we're going.
		velocity_delta = math::NormalizedAngle(wish_dir - velocity_angle);

		// Optimal strafe: add ideal angle toward wish direction,
		// set sidemove to accelerate in that direction.
		if (velocity_delta > 0.f) {
			g_cl.m_cmd->m_view_angles.y = math::NormalizedAngle(velocity_angle + m_ideal);
			g_cl.m_cmd->m_side_move = -450.f;
		}
		else if (velocity_delta < 0.f) {
			g_cl.m_cmd->m_view_angles.y = math::NormalizedAngle(velocity_angle - m_ideal);
			g_cl.m_cmd->m_side_move = 450.f;
		}
		else {
			// Perfectly aligned — switch to maintain speed.
			g_cl.m_cmd->m_view_angles.y += (m_ideal * m_switch_value);
			g_cl.m_cmd->m_side_move = 450.f * m_switch_value;
		}

		// Zero forwardmove — sidemove does all the work.
		g_cl.m_cmd->m_forward_move = 0.f;
	}
	else {
		// No keys held — fall back to mouse-delta strafer (original behaviour).
		if (delta > 0.f)
			g_cl.m_cmd->m_side_move = -450.f;
		else if (delta < 0.f)
			g_cl.m_cmd->m_side_move = 450.f;

		if (abs_delta <= m_ideal || abs_delta >= 30.f) {
			velocity_angle = math::rad_to_deg(std::atan2(velocity.y, velocity.x));
			velocity_delta = math::NormalizedAngle(g_cl.m_cmd->m_view_angles.y - velocity_angle);
			correct = m_ideal2 * 2.f;

			if (velocity_delta <= correct || m_speed <= 15.f) {
				if (-correct <= velocity_delta || m_speed <= 15.f) {
					g_cl.m_cmd->m_view_angles.y += (m_ideal * m_switch_value);
					g_cl.m_cmd->m_side_move = 450.f * m_switch_value;
				}
				else {
					g_cl.m_cmd->m_view_angles.y = velocity_angle - correct;
					g_cl.m_cmd->m_side_move = 450.f;
				}
			}
			else {
				g_cl.m_cmd->m_view_angles.y = velocity_angle + correct;
				g_cl.m_cmd->m_side_move = -450.f;
			}
		}
	}
}

void Movement::DoPrespeed( ) {
	float   mod, min, max, step, strafe, time, angle;
	vec3_t  plane;

	// min and max values are based on 128 ticks.
	mod = g_csgo.m_globals->m_interval * 128.f;

	// scale min and max based on tickrate.
	min = 2.25f * mod;
	max = 5.f * mod;

	// compute ideal strafe angle for moving in a circle.
	strafe = m_ideal * 2.f;

	// clamp ideal strafe circle value to min and max step.
	math::clamp( strafe, min, max );

	// calculate time.
	time = 320.f / m_speed;

	// clamp time.
	math::clamp( time, 0.35f, 1.f );

	// init step.
	step = strafe;

	while( true ) {
		// if we will not collide with an object or we wont accelerate from such a big step anymore then stop.
		if( !WillCollide( time, step ) || max <= step )
			break;

		// if we will collide with an object with the current strafe step then increment step to prevent a collision.
		step += 0.2f;
	}

	if( step > max ) {
		// reset step.
		step = strafe;

		while( true ) {
			// if we will not collide with an object or we wont accelerate from such a big step anymore then stop.
			if( !WillCollide( time, step ) || step <= -min )
				break;

			// if we will collide with an object with the current strafe step decrement step to prevent a collision.
			step -= 0.2f;
		}

		if( step < -min ) {
			if( GetClosestPlane( plane ) ) {
				// grab the closest object normal
				// compute the angle of the normal
				// and push us away from the object.
				angle = math::rad_to_deg( std::atan2( plane.y, plane.x ) );
				step = -math::NormalizedAngle( m_circle_yaw - angle ) * 0.1f;
			}
		}

		else
			step -= 0.2f;
	}

	else
		step += 0.2f;

	// add the computed step to the steps of the previous circle iterations.
	m_circle_yaw = math::NormalizedAngle( m_circle_yaw + step );

	// apply data to usercmd.
	g_cl.m_cmd->m_view_angles.y = m_circle_yaw;
	g_cl.m_cmd->m_side_move = ( step >= 0.f ) ? -450.f : 450.f;
}

bool Movement::GetClosestPlane( vec3_t &plane ) {
	CGameTrace            trace;
	CTraceFilterWorldOnly filter;
	vec3_t                start{ m_origin };
	float                 smallest{ 1.f };
	const float		      dist{ 75.f };

	// trace around us in a circle
	for( float step{ }; step <= math::pi_2; step += ( math::pi / 10.f ) ) {
		// extend endpoint x units.
		vec3_t end = start;
		end.x += std::cos( step ) * dist;
		end.y += std::sin( step ) * dist;

		g_csgo.m_engine_trace->TraceRay( Ray( start, end, m_mins, m_maxs ), CONTENTS_SOLID, &filter, &trace );

		// we found an object closer, then the previouly found object.
		if( trace.m_fraction < smallest ) {
			// save the normal of the object.
			plane = trace.m_plane.m_normal;
			smallest = trace.m_fraction;
		}
	}

	// did we find any valid object?
	return smallest != 1.f && plane.z < 0.1f;
}

bool Movement::WillCollide( float time, float change ) {
	struct PredictionData_t {
		vec3_t start;
		vec3_t end;
		vec3_t velocity;
		float  direction;
		bool   ground;
		float  predicted;
	};

	PredictionData_t      data;
	CGameTrace            trace;
	CTraceFilterWorldOnly filter;

	// set base data.
	data.ground = g_cl.m_flags & FL_ONGROUND;
	data.start = m_origin;
	data.end = m_origin;
	data.velocity = g_cl.m_local->m_vecVelocity( );
	data.direction = math::rad_to_deg( std::atan2( data.velocity.y, data.velocity.x ) );

	for( data.predicted = 0.f; data.predicted < time; data.predicted += g_csgo.m_globals->m_interval ) {
		// predict movement direction by adding the direction change.
		// make sure to normalize it, in case we go over the -180/180 turning point.
		data.direction = math::NormalizedAngle( data.direction + change );

		// pythagoras.
		float hyp = data.velocity.length_2d( );

		// adjust velocity for new direction.
		data.velocity.x = std::cos( math::deg_to_rad( data.direction ) ) * hyp;
		data.velocity.y = std::sin( math::deg_to_rad( data.direction ) ) * hyp;

		// assume we bhop, set upwards impulse.
		if( data.ground )
			data.velocity.z = g_csgo.sv_jump_impulse->GetFloat( );

		else
			data.velocity.z -= g_csgo.sv_gravity->GetFloat( ) * g_csgo.m_globals->m_interval;

		// we adjusted the velocity for our new direction.
		// see if we can move in this direction, predict our new origin if we were to travel at this velocity.
		data.end += ( data.velocity * g_csgo.m_globals->m_interval );

		// trace
		g_csgo.m_engine_trace->TraceRay( Ray( data.start, data.end, m_mins, m_maxs ), MASK_PLAYERSOLID, &filter, &trace );

		// check if we hit any objects.
		if( trace.m_fraction != 1.f && trace.m_plane.m_normal.z <= 0.9f )
			return true;
		if( trace.m_startsolid || trace.m_allsolid )
			return true;

		// adjust start and end point.
		data.start = data.end = trace.m_endpos;

		// move endpoint 2 units down, and re-trace.
		// do this to check if we are on th floor.
		g_csgo.m_engine_trace->TraceRay( Ray( data.start, data.end - vec3_t{ 0.f, 0.f, 2.f }, m_mins, m_maxs ), MASK_PLAYERSOLID, &filter, &trace );

		// see if we moved the player into the ground for the next iteration.
		data.ground = trace.hit( ) && trace.m_plane.m_normal.z > 0.7f;
	}

	// the entire loop has ran
	// we did not hit shit.
	return false;
}

void Movement::FixMove( CUserCmd *cmd, const ang_t &wish_angles ) {
	vec3_t  move, dir;
	float   delta, len;
	ang_t   move_angle;

	// roll nospread fix.
	if( !( g_cl.m_flags & FL_ONGROUND ) && cmd->m_view_angles.z != 0.f )
		cmd->m_side_move = 0.f;

	// convert movement to vector.
	move = { cmd->m_forward_move, cmd->m_side_move, 0.f };

	// get move length and ensure we're using a unit vector ( vector with length of 1 ).
	len = move.normalize( );
	if( !len )
		return;

	// convert move to an angle.
	math::VectorAngles( move, move_angle );

	// calculate yaw delta.
	delta = ( cmd->m_view_angles.y - wish_angles.y );

	// accumulate yaw delta.
	move_angle.y += delta;

	// calculate our new move direction.
	// dir = move_angle_forward * move_length
	math::AngleVectors( move_angle, &dir );

	// scale to og movement.
	dir *= len;

	// strip old flags.
	g_cl.m_cmd->m_buttons &= ~( IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT );

	// fix ladder and noclip.
	if( g_cl.m_local->m_MoveType( ) == MOVETYPE_LADDER ) {
		// invert directon for up and down.
		if( cmd->m_view_angles.x >= 45.f && wish_angles.x < 45.f && std::abs( delta ) <= 65.f )
			dir.x = -dir.x;

		// write to movement.
		cmd->m_forward_move = dir.x;
		cmd->m_side_move = dir.y;

		// set new button flags.
		if( cmd->m_forward_move > 200.f )
			cmd->m_buttons |= IN_FORWARD;

		else if( cmd->m_forward_move < -200.f )
			cmd->m_buttons |= IN_BACK;

		if( cmd->m_side_move > 200.f )
			cmd->m_buttons |= IN_MOVERIGHT;

		else if( cmd->m_side_move < -200.f )
			cmd->m_buttons |= IN_MOVELEFT;
	}

	// we are moving normally.
	else {
		// we must do this for pitch angles that are out of bounds.
		if( cmd->m_view_angles.x < -90.f || cmd->m_view_angles.x > 90.f )
			dir.x = -dir.x;

		// set move.
		cmd->m_forward_move = dir.x;
		cmd->m_side_move = dir.y;

		// set new button flags.
		if( cmd->m_forward_move > 0.f )
			cmd->m_buttons |= IN_FORWARD;

		else if( cmd->m_forward_move < 0.f )
			cmd->m_buttons |= IN_BACK;

		if( cmd->m_side_move > 0.f )
			cmd->m_buttons |= IN_MOVERIGHT;

		else if( cmd->m_side_move < 0.f )
			cmd->m_buttons |= IN_MOVELEFT;
	}
}

void Movement::AutoPeek( ) {
	// set to invert if we press the button.
	if( g_input.GetKeyState( g_menu.main.movement.autopeek.get( ) ) ) {
		if( g_cl.m_old_shot )
			m_invert = true;

		vec3_t move{ g_cl.m_cmd->m_forward_move, g_cl.m_cmd->m_side_move, 0.f };

		if( m_invert ) {
			move *= -1.f;
			g_cl.m_cmd->m_forward_move = move.x;
			g_cl.m_cmd->m_side_move = move.y;
		}
	}

	else m_invert = false;

	bool can_stop = g_menu.main.movement.autostop_always_on.get() || g_input.GetKeyState(g_menu.main.movement.autostop.get());
	if( ( g_input.GetKeyState( g_menu.main.movement.autopeek.get( ) ) || can_stop ) && g_aimbot.m_stop ) {
		Movement::QuickStop( );
	}
}

void Movement::QuickStop( ) {
	// convert velocity to angular momentum.
	const vec3_t velocity = g_cl.m_local->m_vecVelocity();
	ang_t angle;
	math::VectorAngles(velocity, angle);

	float speed = velocity.length();
	// fix direction by factoring in where we are looking.
	angle.y = g_cl.m_view_angles.y - angle.y;

	// convert corrected angle back to a direction.
	vec3_t direction;
	math::AngleVectors( angle, &direction );

	vec3_t stop = direction * -speed;

	if( g_cl.m_speed > 13.f ) {
		g_cl.m_cmd->m_forward_move = stop.x;
		g_cl.m_cmd->m_side_move = stop.y;
	}
	else {
		g_cl.m_cmd->m_forward_move = 0.f;
		g_cl.m_cmd->m_side_move = 0.f;
	}
}

void Movement::FakeWalk( ) {
	vec3_t velocity{ g_cl.m_local->m_vecVelocity( ) };
	int       ticks{ };
	const int max_ticks{ 16 };

	if( !g_input.GetKeyState( g_menu.main.movement.fakewalk.get( ) ) )
		return;

	if( !g_cl.m_local->GetGroundEntity( ) )
		return;

	// user was running previously and abrubtly held the fakewalk key
	// we should quick-stop under this circumstance to hit the 0.22 flick
	// perfectly, and speed up our fakewalk after running even more.
	//if( g_cl.m_initial_flick ) {
	//	Movement::QuickStop( );
	//	return;
	//}
	
	// reference:
	// https://github.com/ValveSoftware/source-sdk-2013/blob/master/mp/src/game/shared/gamemovement.cpp#L1612

	// calculate friction.
	float friction = g_csgo.sv_friction->GetFloat( ) * g_cl.m_local->m_surfaceFriction( );

	for( ; ticks < g_cl.m_max_lag; ++ticks ) {
		// calculate speed.
		float speed = velocity.length( );

		// if too slow return.
		if( speed <= 0.1f )
			break;

		// bleed off some speed, but if we have less than the bleed, threshold, bleed the threshold amount.
		float control = std::max( speed, g_csgo.sv_stopspeed->GetFloat( ) );

		// calculate the drop amount.
		float drop = control * friction * g_csgo.m_globals->m_interval;

		// scale the velocity.
		float newspeed = std::max( 0.f, speed - drop );

		if( newspeed != speed ) {
			// determine proportion of old speed we are using.
			newspeed /= speed;

			// adjust velocity according to proportion.
			velocity *= newspeed;
		}
	}

	// zero forwardmove and sidemove.
	if (ticks > ((max_ticks - 1) - g_csgo.m_cl->m_choked_commands) || !g_csgo.m_cl->m_choked_commands) {
		g_cl.m_cmd->m_forward_move = g_cl.m_cmd->m_side_move = 0.f;
	}
}