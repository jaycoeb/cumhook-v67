#include "includes.h"
#include <numeric>

static float pred_foot_delta(const std::deque<std::shared_ptr<LagRecord>>& records) {
    if (records.size() < 3)
        return 0.0f;

    std::vector<float> deltas;
    deltas.reserve(records.size() - 1);

    for (size_t i = 1; i < records.size(); ++i) {
        const auto& prev = records[i - 1];
        const auto& curr = records[i];

        float delta = math::NormalizedAngle(curr->m_body - prev->m_body);
        deltas.push_back(delta);
    }

    float avg_delta = std::accumulate(deltas.begin(), deltas.end(), 0.0f) / deltas.size();
    float max_abs_delta = 0.0f;
    float sign = 1.0f;

    for (float d : deltas) {
        if (fabsf(d) > fabsf(max_abs_delta)) {
            max_abs_delta = d;
            sign = (d >= 0.f) ? 1.f : -1.f;
        }
    }

    if (fabsf(max_abs_delta) > 35.0f) {
        return -sign * 60.0f;
    }

    return 0.0f;
}

static float curvature_heuristic(const std::deque<std::shared_ptr<LagRecord>>& records) {
    if (records.size() < 4)
        return 0.0f;

    std::vector<float> velocity;
    for (size_t i = 1; i < records.size(); ++i) {
        const float dyaw = math::NormalizedAngle(records[i]->m_body - records[i - 1]->m_body);
        const float dt = records[i]->m_anim_time - records[i - 1]->m_anim_time;
        velocity.push_back(dt > 0.0f ? dyaw / dt : 0.0f);
    }

    float curvature = 0.0f;
    for (size_t i = 1; i < velocity.size(); ++i)
        curvature += velocity[i] - velocity[i - 1];

    return curvature;
}

static float pred_foot_yaw(const LagRecord& a, const LagRecord& b, float predicted_offset) {
    constexpr float foot_speed = 80.0f;

    const float old_yaw = a.m_body;
    const float new_yaw = b.m_body;
    const float dt = b.m_anim_time - a.m_anim_time;

    if (dt <= 0.0f)
        return old_yaw;

    const float max_delta = foot_speed * dt;
    const float body_delta = math::NormalizedAngle(new_yaw - old_yaw);
    const float abs_foot_delta = math::NormalizedAngle(b.m_abs_ang.y - a.m_abs_ang.y);

    const float blended_delta = (body_delta * 0.5f) + (abs_foot_delta * 0.5f);
    const float abs_blended = fabsf(blended_delta);

    float resolved;
    if (abs_blended <= max_delta) {
        resolved = old_yaw + blended_delta;
    }
    else {
        resolved = old_yaw + copysignf(max_delta, blended_delta);
        resolved = math::NormalizedAngle(resolved + predicted_offset);
    }

    return math::NormalizedAngle(resolved);
}

static bool lby_spam(const LagRecord& curr, const LagRecord& prev) {
    const auto& layer0_curr = curr.m_layers[0];
    const auto& layer0_prev = prev.m_layers[0];

    return (
        layer0_curr.m_sequence == layer0_prev.m_sequence &&
        layer0_curr.m_cycle < layer0_prev.m_cycle &&
        fabsf(layer0_curr.m_playback_rate - layer0_prev.m_playback_rate) > 0.02f
    );
}

Resolver g_resolver{};;

LagRecord* Resolver::FindIdealRecord( AimPlayer* data ) {
    LagRecord *first_valid, *current;

	if( data->m_records.empty( ) )
		return nullptr;

    first_valid = nullptr;

    // iterate records.
	for( const auto &it : data->m_records ) {
		if( it->dormant( ) || it->immune( ) || !it->valid( ) )
			continue;

        // get current record.
        current = it.get( );

        // first record that was valid, store it for later.
        if( !first_valid )
            first_valid = current;

        // try to find a record with a shot, lby update, walking or no anti-aim.
		if( it->m_shot || it->m_mode == Modes::RESOLVE_BODY || it->m_mode == Modes::RESOLVE_WALK || it->m_mode == Modes::RESOLVE_NONE )
            return current;
	}

	// none found above, return the first valid record if possible.
	return ( first_valid ) ? first_valid : nullptr;
}

LagRecord* Resolver::FindLastRecord( AimPlayer* data ) {
    LagRecord* current;

	if( data->m_records.empty( ) )
		return nullptr;

	// iterate records in reverse.
	for( auto it = data->m_records.crbegin( ); it != data->m_records.crend( ); ++it ) {
		current = it->get( );

		// if this record is valid.
		// we are done since we iterated in reverse.
		if( current->valid( ) && !current->immune( ) && !current->dormant( ) )
			return current;
	}

	return nullptr;
}

LagRecord* Resolver::FindPreviousRecord(AimPlayer* data) {
    if (data->m_records.size() < 2)
        return nullptr;

    return data->m_records[1].get();
}

void Resolver::OnBodyUpdate( Player* player, float value ) {
	AimPlayer* data = &g_aimbot.m_players[ player->index( ) - 1 ];

	// set data.
	data->m_old_body = data->m_body;
	data->m_body     = value;
}

float Resolver::GetAwayAngle( LagRecord* record ) {
	float  delta{ std::numeric_limits< float >::max( ) };
	vec3_t pos;
	ang_t  away;

	// other cheats predict you by their own latency.
	// they do this because, then they can put their away angle to exactly
	// where you are on the server at that moment in time.

	// the idea is that you would need to know where they 'saw' you when they created their user-command.
	// lets say you move on your client right now, this would take half of our latency to arrive at the server.
	// the delay between the server and the target client is compensated by themselves already, that is fortunate for us.

	// we have no historical origins.
	// no choice but to use the most recent one.
	//if( g_cl.m_net_pos.empty( ) ) {
		math::VectorAngles( g_cl.m_local->m_vecOrigin( ) - record->m_pred_origin, away );
		return away.y;
	//}

	// half of our rtt.
	// also known as the one-way delay.
	//float owd = ( g_cl.m_latency / 2.f );

	// since our origins are computed here on the client
	// we have to compensate for the delay between our client and the server
	// therefore the OWD should be subtracted from the target time.
	//float target = record->m_pred_time; //- owd;

	// iterate all.
	//for( const auto &net : g_cl.m_net_pos ) {
		// get the delta between this records time context
		// and the target time.
	//	float dt = std::abs( target - net.m_time );

		// the best origin.
	//	if( dt < delta ) {
	//		delta = dt;
	//		pos   = net.m_pos;
	//	}
	//}

	//math::VectorAngles( pos - record->m_pred_origin, away );
	//return away.y;
}

void Resolver::MatchShot( AimPlayer* data, LagRecord* record ) {
	// do not attempt to do this in nospread mode.
	if( g_menu.main.config.mode.get( ) == 1 )
		return;

	float shoot_time = -1.f;

	Weapon* weapon = data->m_player->GetActiveWeapon( );
	if( weapon ) {
		// with logging this time was always one tick behind.
		// so add one tick to the last shoot time.
		shoot_time = weapon->m_fLastShotTime( ) + g_csgo.m_globals->m_interval;
	}

	// this record has a shot on it.
	if( game::TIME_TO_TICKS( shoot_time ) == game::TIME_TO_TICKS( record->m_sim_time ) ) {
		if( record->m_lag <= 2 )
			record->m_shot = true;
		
		// more then 1 choke, cant hit pitch, apply prev pitch.
		else if( data->m_records.size( ) >= 2 ) {
			LagRecord* previous = data->m_records[ 1 ].get( );

			if( previous && !previous->dormant( ) )
				record->m_eye_angles.x = previous->m_eye_angles.x;
		}
	}
}

void Resolver::SetMode( LagRecord* record ) {
	// the resolver has 3 modes to chose from.
	// these modes will vary more under the hood depending on what data we have about the player
	// and what kind of hack vs. hack we are playing (mm/nospread).

	float speed = record->m_anim_velocity.length( );

	// if on ground, moving, and not fakewalking.
	if( ( record->m_flags & FL_ONGROUND ) && speed > 0.1f && !record->m_fake_walk )
		record->m_mode = Modes::RESOLVE_WALK;

	// if on ground, not moving or fakewalking.
	if( ( record->m_flags & FL_ONGROUND ) && ( speed <= 0.1f || record->m_fake_walk ) )
		record->m_mode = Modes::RESOLVE_STAND;

	// if not on ground.
	else if( !( record->m_flags & FL_ONGROUND ) )
		record->m_mode = Modes::RESOLVE_AIR;
}

void Resolver::ResolveAngles( Player* player, LagRecord* record ) {
	AimPlayer* data = &g_aimbot.m_players[ player->index( ) - 1 ];

	// mark this record if it contains a shot.
	MatchShot( data, record );

	// next up mark this record with a resolver mode that will be used.
	SetMode( record );

	// if we are in nospread mode, force all players pitches to down.
	// TODO; we should check thei actual pitch and up too, since those are the other 2 possible angles.
	// this should be somehow combined into some iteration that matches with the air angle iteration.
	if( g_menu.main.config.mode.get( ) == 1 )
		record->m_eye_angles.x = 90.f;

	// we arrived here we can do the acutal resolve.
	if( record->m_mode == Modes::RESOLVE_WALK ) 
		ResolveWalk( data, record );

	else if( record->m_mode == Modes::RESOLVE_STAND )
		ResolveStand( data, record, player->m_PlayerAnimState( ) );

	else if( record->m_mode == Modes::RESOLVE_AIR )
		ResolveAir( data, record );

	// normalize the eye angles, doesn't really matter but its clean.
	math::NormalizeAngle( record->m_eye_angles.y );
}

float Resolver::CalculateStand(AimPlayer* data, LagRecord* record, LagRecord* prev, CCSGOPlayerAnimState* state) {
    if (!data || !record || !prev)
        return record ? record->m_body : 0.0f;

    const auto& adjust = record->m_layers[3];
    const auto& prev_adjust = prev->m_layers[3];
    const float anim_dt = record->m_anim_time - prev->m_anim_time;

    if (adjust.m_sequence == 979 || adjust.m_sequence == 978) {
        const bool cycle_reset = prev_adjust.m_cycle > 0.8f && adjust.m_cycle < 0.2f;
        const bool weight_spike = (adjust.m_weight - prev_adjust.m_weight > 0.45f) && (adjust.m_weight > 0.9f);

        if (cycle_reset && weight_spike) {
            return record->m_body;
        }
    }

    if (data->m_records.size() >= 4) {
        float curvature = curvature_heuristic(data->m_records);
        if (fabsf(curvature) > 35.0f) {
            return math::NormalizedAngle(record->m_body + curvature);
        }
    }

    if (adjust.m_weight < 0.01f && fabsf(adjust.m_cycle - prev_adjust.m_cycle) < 0.001f) {
        return math::NormalizedAngle(record->m_body + 180.0f);
    }

    if (data->m_records.size() >= 3) {
        const float pred_offset = pred_foot_delta(data->m_records);
        return math::NormalizedAngle(record->m_body + pred_offset);
    }

    const float max_desync = state->m_fl_aim_yaw_max;
    const float body_delta = math::NormalizedAngle(record->m_body - prev->m_body);

    const bool clamp_l = body_delta < -max_desync + 0.1f;
    const bool clamp_r = body_delta > max_desync - 0.1f;

    if (clamp_l || clamp_r) {
        const float invert = clamp_l ? -max_desync : max_desync;
        return math::NormalizedAngle(record->m_body + invert);
    }

    const int act = data->m_player->GetSequenceActivity(record->m_layers[6].m_sequence);
    if (act == 980 && anim_dt > 0.0f) {
        const float foot_yaw_prev = prev->m_abs_ang.y;
        const float foot_yaw_now = record->m_abs_ang.y;
        const float yaw_delta = math::NormalizedAngle(foot_yaw_now - foot_yaw_prev);
        const float yaw_speed = fabsf(yaw_delta / anim_dt);

        const bool fast_turn = yaw_speed > 120.0f;
        const bool snapped_to_body = fabsf(math::NormalizedAngle(foot_yaw_now - record->m_body)) < 5.0f;

        if (fast_turn && snapped_to_body && adjust.m_cycle < 0.2f) {
            return record->m_body;
        }
    }

    const float abs_yaw_delta = fabsf(math::NormalizedAngle(record->m_abs_ang.y - prev->m_abs_ang.y));
    const float body_yaw_delta = fabsf(math::NormalizedAngle(record->m_body - prev->m_body));

    if (abs_yaw_delta > 58.f && body_yaw_delta < 5.f) {
        return math::NormalizedAngle(record->m_body + 180.0f);
    }

    return math::NormalizedAngle(prev->m_eye_angles.y + body_delta);
}

void Resolver::ResolveWalk( AimPlayer* data, LagRecord* record ) {
	// apply lby to eyeangles.
	record->m_eye_angles.y = record->m_body;

	// delay body update.
	data->m_body_update = record->m_anim_time + 0.22f;

	// reset stand and body index.
	data->m_stand_index  = 0;
	data->m_stand_index2 = 0;
	data->m_body_index   = 0;

	// copy the last record that this player was walking
	// we need it later on because it gives us crucial data.
	std::memcpy( &data->m_walk_record, record, sizeof( LagRecord ) );
}

void Resolver::ResolveStand(AimPlayer* data, LagRecord* record, CCSGOPlayerAnimState* state) {
    // for no-spread call a seperate resolver.
    if (g_menu.main.config.mode.get() == 1) {
        StandNS(data, record);
        return;
    }

    float away = GetAwayAngle(record);
    LagRecord* move = &data->m_walk_record;

    C_AnimationLayer* adjust = &record->m_layers[3];
    LagRecord* prev = FindPreviousRecord(data);

    // hhh, lil paste
    if (move->m_sim_time > 0.f) {
        vec3_t delta = move->m_origin - record->m_origin;
        if (delta.length() <= 128.f) {
            data->m_moved = true;
        }
    }

    int act = data->m_player->GetSequenceActivity(adjust->m_sequence);
    if (prev) {
        const auto& prev_adjust = prev->m_layers[3];

        const bool adjust_sequence_valid =
            adjust->m_sequence == 979 || adjust->m_sequence == 978;

        const bool adjust_reset =
            prev_adjust.m_cycle > 0.8f && adjust->m_cycle < 0.2f;

        const bool weight_spike =
            (adjust->m_weight - prev_adjust.m_weight > 0.45f) &&
            (adjust->m_weight > 0.9f);

        const bool lby_snap = adjust_sequence_valid && adjust_reset && weight_spike;

        if (lby_snap) {
            data->m_trusted_lby = record->m_body;
            data->m_trusted_lby_time = record->m_anim_time;
            record->m_eye_angles.y = record->m_body;
            return;
        }
    }

    const auto& curr_idle = record->m_layers[11];
    float delta_cycle = 0.0f;
    float delta_playback = 0.0f;

    if (prev) {
        const auto& prev_idle = prev->m_layers[11];
        delta_cycle = fabsf(curr_idle.m_cycle - prev_idle.m_cycle);
        delta_playback = fabsf(curr_idle.m_playback_rate - prev_idle.m_playback_rate);

        if (curr_idle.m_sequence == 979 && prev_idle.m_sequence == 979) {
            if (delta_cycle < 0.01f && delta_playback < 0.005f)
                data->m_fake_idle_ticks++;
            else
                data->m_fake_idle_ticks = 0;

            if (data->m_fake_idle_ticks > 2) {
                record->m_eye_angles.y = GetAwayAngle(record) + 180.f;
                return;
            }
        }
    }

    if (act == 980 && prev) {
        const float last_foot_yaw = prev->m_abs_ang.y;
        const float curr_foot_yaw = record->m_abs_ang.y;
        const float dt = record->m_anim_time - prev->m_anim_time;

        if (dt > 0.0f) {
            float yaw_delta = math::NormalizedAngle(curr_foot_yaw - last_foot_yaw);
            float yaw_speed = fabsf(yaw_delta / dt);

            const bool turned_fast = yaw_speed > 120.0f;
            const bool aligned_with_body = fabsf(math::NormalizedAngle(curr_foot_yaw - record->m_body)) < 5.0f;
            const bool adjust_cycle_low = adjust->m_cycle < 0.2f && adjust->m_weight > 0.9f;

            if (turned_fast && aligned_with_body && adjust_cycle_low) {
                record->m_eye_angles.y = record->m_body;
                return;
            }
        }
    }

    if (data->m_moved) {
        float delta = record->m_anim_time - move->m_anim_time;

        if (delta < 0.22f) {
            record->m_eye_angles.y = move->m_body;
            return;
        }
        else if (record->m_anim_time >= data->m_body_update) {
            if (data->m_body_index <= 3) {
                record->m_eye_angles.y = record->m_body;
                data->m_body_update = record->m_anim_time + 1.1f;
                return;
            }

            record->m_eye_angles.y = move->m_body;

            if (!(data->m_stand_index % 3))
                record->m_eye_angles.y += g_csgo.RandomFloat(-35.f, 35.f);

            if (data->m_stand_index > 6 && act != 980)
                record->m_eye_angles.y = move->m_body + 180.f;
            else if (data->m_stand_index > 4 && act != 980)
                record->m_eye_angles.y = away + 180.f;

            return;
        }
    }

    if (data->m_records.size() >= 2) {
        const auto& a = *data->m_records[data->m_records.size() - 2];
        const auto& b = *data->m_records[data->m_records.size() - 1];

        float predicted_offset = pred_foot_delta(data->m_records);

        if (fabsf(predicted_offset) > 1.0f) {
            record->m_eye_angles.y = pred_foot_yaw(a, b, predicted_offset);
        }
        else {
            record->m_eye_angles.y = CalculateStand(data, record, prev, state);
        }

        return;
    }

    if (data->m_records.size() >= 4) {
        float curvature = curvature_heuristic(data->m_records);

        if (fabsf(curvature) > 40.0f) {
            const auto& a = *data->m_records[data->m_records.size() - 2];
            const auto& b = *data->m_records[data->m_records.size() - 1];

            float predicted_offset = pred_foot_delta(data->m_records);
            float predicted_yaw = pred_foot_yaw(a, b, predicted_offset);

            record->m_eye_angles.y = predicted_yaw;
            return;
        }
    }

    {
        if (state) {
            const float foot_yaw = record->m_abs_ang.y;
            const float width_factor = 1.0f;
            const float yaw_max = state->m_fl_aim_yaw_max * width_factor;
            const float yaw_min = state->m_fl_aim_yaw_min * width_factor;

            const float dy = math::NormalizedAngle(record->m_body - foot_yaw);
            float corrected_yaw = foot_yaw;

            if (dy > yaw_max - 0.5f && dy < yaw_max + 0.5f) {
                corrected_yaw = foot_yaw + yaw_max;
                record->m_eye_angles.y = math::NormalizedAngle(corrected_yaw);
                return;
            }
            else if (dy < yaw_min + 0.5f && dy > yaw_min - 0.5f) {
                corrected_yaw = foot_yaw + yaw_min;
                record->m_eye_angles.y = math::NormalizedAngle(corrected_yaw);
                return;
            }
        }
    }

    if (data->m_records.size() >= 2) {
        const auto& prev_rec = *data->m_records[data->m_records.size() - 2].get();
        const auto& curr_rec = *data->m_records[data->m_records.size() - 1].get();

        if (lby_spam(curr_rec, prev_rec)) {
            record->m_eye_angles.y = math::NormalizedAngle(prev_rec.m_body + 180.f);
            return;
        }
    }

    const auto& layer0 = record->m_layers[0];
    const float body = record->m_body;
    const float away_normal = away;
    const float fl_weight = adjust->m_weight;
    const float fl_cycle = adjust->m_cycle;

    bool is_adjust = act == 980;
    bool is_mad = (fl_weight < 0.2f && fl_cycle > 0.01f && fl_cycle < 0.9f);
    bool is_lol = (
        layer0.m_playback_rate > 0.96f &&
        layer0.m_cycle < 0.07f &&
        prev && layer0.m_sequence == prev->m_layers[0].m_sequence &&
        !is_adjust
    );
    bool is_fake_idle = (adjust->m_sequence == 979 && fl_cycle < 0.01f);

    int index = data->m_stand_index2 % 6;

    switch (index) {
    case 0:
        record->m_eye_angles.y = away_normal + 180.f;
        break;

    case 1:
        record->m_eye_angles.y = body;
        break;

    case 2:
        if (is_mad)
            record->m_eye_angles.y = body + 70.f;
        else
            record->m_eye_angles.y = body + 110.f;
        break;

    case 3:
        if (is_mad)
            record->m_eye_angles.y = body - 70.f;
        else
            record->m_eye_angles.y = body - 110.f;
        break;

    case 4:
        if (is_fake_idle || is_lol)
            record->m_eye_angles.y = away_normal + g_csgo.RandomFloat(-25.f, 25.f);
        else
            record->m_eye_angles.y = away_normal + 180.f + g_csgo.RandomFloat(-20.f, 20.f);
        break;
    }

    math::NormalizeAngle(record->m_eye_angles.y);
}

void Resolver::StandNS( AimPlayer* data, LagRecord* record ) {
	// get away angles.
	float away = GetAwayAngle( record );

	switch( data->m_shots % 8 ) {
	case 0:
		record->m_eye_angles.y = away + 180.f;
		break;

	case 1:
		record->m_eye_angles.y = away + 90.f;
		break;
	case 2:
		record->m_eye_angles.y = away - 90.f;
		break;

	case 3:
		record->m_eye_angles.y = away + 45.f;
		break;
	case 4:
		record->m_eye_angles.y = away - 45.f;
		break;

	case 5:
		record->m_eye_angles.y = away + 135.f;
		break;
	case 6:
		record->m_eye_angles.y = away - 135.f;
		break;

	case 7:
		record->m_eye_angles.y = away + 0.f;
		break;

	default:
		break;
	}

	// force LBY to not fuck any pose and do a true bruteforce.
	record->m_body = record->m_eye_angles.y;
}

void Resolver::ResolveAir( AimPlayer* data, LagRecord* record ) {
	// for no-spread call a seperate resolver.
	if( g_menu.main.config.mode.get( ) == 1 ) {
		AirNS( data, record );
		return;
	}

	// else run our matchmaking air resolver.

	// we have barely any speed. 
	// either we jumped in place or we just left the ground.
	// or someone is trying to fool our resolver.
	if( record->m_velocity.length_2d( ) < 60.f ) {
		// set this for completion.
		// so the shot parsing wont pick the hits / misses up.
		// and process them wrongly.
		record->m_mode = Modes::RESOLVE_STAND;

		// invoke our stand resolver.
		ResolveStand( data, record, record->m_player->m_PlayerAnimState( ) );

		// we are done.
		return;
	}

	// try to predict the direction of the player based on his velocity direction.
	// this should be a rough estimation of where he is looking.
	float velyaw = math::rad_to_deg( std::atan2( record->m_velocity.y, record->m_velocity.x ) );

	switch( data->m_shots % 3 ) {
	case 0:
		record->m_eye_angles.y = velyaw + 180.f;
		break;

	case 1:
		record->m_eye_angles.y = velyaw - 90.f;
		break;

	case 2:
		record->m_eye_angles.y = velyaw + 90.f;
		break;
	}
}

void Resolver::AirNS( AimPlayer* data, LagRecord* record ) {
	// get away angles.
	float away = GetAwayAngle( record );

	switch( data->m_shots % 9 ) {
	case 0:
		record->m_eye_angles.y = away + 180.f;
		break;

	case 1:
		record->m_eye_angles.y = away + 150.f;
		break;
	case 2:
		record->m_eye_angles.y = away - 150.f;
		break;

	case 3:
		record->m_eye_angles.y = away + 165.f;
		break;
	case 4:
		record->m_eye_angles.y = away - 165.f;
		break;

	case 5:
		record->m_eye_angles.y = away + 135.f;
		break;
	case 6:
		record->m_eye_angles.y = away - 135.f;
		break;

	case 7:
		record->m_eye_angles.y = away + 90.f;
		break;
	case 8:
		record->m_eye_angles.y = away - 90.f;
		break;

	default:
		break;
	}
}

void Resolver::ResolvePoses( Player* player, LagRecord* record ) {
	AimPlayer* data = &g_aimbot.m_players[ player->index( ) - 1 ];

	// only do this bs when in air.
	if( record->m_mode == Modes::RESOLVE_AIR ) {
		// ang = pose min + pose val x ( pose range )

		// lean_yaw
		player->m_flPoseParameter( )[ 2 ]  = g_csgo.RandomInt( 0, 4 ) * 0.25f;   

		// body_yaw
		player->m_flPoseParameter( )[ 11 ] = g_csgo.RandomInt( 1, 3 ) * 0.25f;
	}
}