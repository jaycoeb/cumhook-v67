#include "includes.h"
#include <numeric>

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

static float pred_foot_delta(const std::deque<std::shared_ptr<LagRecord>>& records) {
    constexpr float MAX_DELTA_THRESHOLD = 35.0f;

    if (records.size() < 3)
        return 0.0f;

    float max_abs_delta = 0.0f;
    float sign = 1.0f;

    for (size_t i = 1; i < records.size(); ++i) {
        float delta = math::NormalizedAngle(records[i]->m_body - records[i - 1]->m_body);

        if (fabsf(delta) > fabsf(max_abs_delta)) {
            max_abs_delta = delta;
            sign = (delta >= 0.f) ? 1.f : -1.f;
        }
    }

    if (fabsf(max_abs_delta) > MAX_DELTA_THRESHOLD) {
        // dynamic scale instead of fixed 60
        float scaled = fabsf(max_abs_delta) * 2.0f;

        // manual clamp (no std::clamp needed)
        if (scaled < 30.f) scaled = 30.f;
        if (scaled > 180.f) scaled = 180.f;

        return -sign * scaled;
    }

    return 0.0f;
}

static float curvature_heuristic(const std::deque<std::shared_ptr<LagRecord>>& records) {
    if (records.size() < 4)
        return 0.f;

    float total = 0.f;
    float prev_vel = 0.f;
    bool has_prev = false;

    // iterate recent records (keep it tight, last ~4–6)
    int count = std::min((int)records.size(), 6);

    for (int i = 1; i < count; ++i) {
        const auto& curr = records[i];
        const auto& prev = records[i - 1];

        float dt = curr->m_anim_time - prev->m_anim_time;
        if (dt <= 0.f)
            continue;

        // yaw delta per second (angular velocity)
        float dyaw = math::NormalizedAngle(curr->m_body - prev->m_body);
        float vel = dyaw / dt;

        if (has_prev) {
            // acceleration (change in angular velocity)
            total += (vel - prev_vel);
        }

        prev_vel = vel;
        has_prev = true;
    }

    // average it slightly so it’s not noisy
    return total / (float)(count - 1);
}

static float ScoreYaw(AimPlayer* data, LagRecord* record, float yaw) {
    float score = 0.f;

    float body = record->m_body;

    // --------------------------------------------------
    // 1. LBY proximity (core for 2018)
    // --------------------------------------------------
    float lby_delta = fabsf(math::NormalizedAngle(yaw - body));
    score += (180.f - lby_delta) * 1.5f;

    // --------------------------------------------------
    // 2. movement direction
    // --------------------------------------------------
    if (record->m_velocity.length_2d() > 20.f) {
        float move_dir = math::rad_to_deg(std::atan2(
            record->m_velocity.y,
            record->m_velocity.x
        ));

        float move_delta = fabsf(math::NormalizedAngle(yaw - move_dir));
        score += (180.f - move_delta) * 0.4f;
    }

    // --------------------------------------------------
    // 3. previous resolved stability
    // --------------------------------------------------
    if (!data->m_records.empty()) {
        float prev = data->m_resolve_history.last_angle;
        float delta = fabsf(math::NormalizedAngle(yaw - prev));

        score += (180.f - delta) * 0.25f;
    }

    // --------------------------------------------------
    // 4. trusted LBY boost
    // --------------------------------------------------
    if (data->m_trusted_lby_time > 0.f) {
        float dt = record->m_anim_time - data->m_trusted_lby_time;

        if (dt < 1.1f) {
            float delta = fabsf(math::NormalizedAngle(yaw - data->m_trusted_lby));
            score += (180.f - delta) * 2.0f;
        }
    }

    // ==================================================
    // 🔥 5. MISS ADAPTATION (THIS IS THE GAME CHANGER)
    // ==================================================

    float delta = math::NormalizedAngle(yaw - body);

    // missed center → stop picking center
    if (data->m_resolve_history.miss_bruteforce > 0) {
        if (fabsf(delta) < 20.f)
            score -= 50.f;
    }

    // missed right side → boost LEFT
    if (data->m_resolve_history.miss_side > 0) {
        if (delta > 0.f)   // right side
            score -= 40.f;
        else
            score += 25.f;
    }

    // missed left side → boost RIGHT
    if (data->m_resolve_history.miss_invert > 0) {
        if (delta < 0.f)
            score -= 40.f;
        else
            score += 25.f;
    }

    // heavy misses → force opposite brute
    int total_misses =
        data->m_resolve_history.miss_side +
        data->m_resolve_history.miss_invert +
        data->m_resolve_history.miss_bruteforce;

    if (total_misses >= 3) {
        if (fabsf(delta) > 140.f) // opposite side
            score += 35.f;
    }

    return score;
}

static float SelectBestYaw(AimPlayer* data, LagRecord* record) {
    std::vector<Resolver::ResolveCandidate> candidates;

    float body = record->m_body;
    float away = g_resolver.GetAwayAngle(record);

    // --------------------------------------------------
    // core angles (2018 meta)
    // --------------------------------------------------
    candidates.push_back({ body, 0.f });
    candidates.push_back({ body + 180.f, 0.f });
    candidates.push_back({ body + 90.f, 0.f });
    candidates.push_back({ body - 90.f, 0.f });

    candidates.push_back({ away + 180.f, 0.f });
    candidates.push_back({ away + 90.f, 0.f });
    candidates.push_back({ away - 90.f, 0.f });

    // --------------------------------------------------
    // foot prediction (if available)
    // --------------------------------------------------
    if (data->m_records.size() >= 2) {
        const auto& a = *data->m_records[data->m_records.size() - 2];
        const auto& b = *data->m_records[data->m_records.size() - 1];

        float offset = pred_foot_delta(data->m_records);
        float scaled = std::clamp(offset * 3.f, -180.f, 180.f);

        float foot = pred_foot_yaw(a, b, scaled);
        candidates.push_back({ foot, 0.f });
    }

    // --------------------------------------------------
    // curvature
    // --------------------------------------------------
    if (data->m_records.size() >= 4) {
        float curv = curvature_heuristic(data->m_records);
        candidates.push_back({ body + curv, 0.f });
    }

    // --------------------------------------------------
    // score all
    // --------------------------------------------------
    float best_score = -FLT_MAX;
    float best_yaw = body;

    for (auto& c : candidates) {
        c.yaw = math::NormalizedAngle(c.yaw);
        c.score = ScoreYaw(data, record, c.yaw);

        if (c.score > best_score) {
            best_score = c.score;
            best_yaw = c.yaw;
        }
    }

    return best_yaw;
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
	if (!record)
		return 0.0f;

	ang_t away;
	math::VectorAngles( g_cl.m_local->m_vecOrigin( ) - record->m_pred_origin, away );
	return away.y;
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

    data->m_resolve_history.last_angle = record->m_eye_angles.y;
}

float Resolver::CalculateStand(AimPlayer* data, LagRecord* record, LagRecord* prev, CCSGOPlayerAnimState* state) {
    if (!data || !record || !prev)
        return record ? record->m_body : 0.f;

    float confidence = ComputeConfidence(data);

    // 🔒 LOW CONFIDENCE → NEVER PREDICT
    if (confidence < 0.4f) {
        return math::NormalizedAngle(record->m_body + 180.f);
    }

    // ✅ TRUST LBY SNAP FIRST
    const auto& adjust = record->m_layers[3];
    const auto& prev_adjust = prev->m_layers[3];

    if ((adjust.m_sequence == 979 || adjust.m_sequence == 978)) {
        bool reset = prev_adjust.m_cycle > 0.8f && adjust.m_cycle < 0.2f;
        bool spike = (adjust.m_weight - prev_adjust.m_weight > 0.45f) && adjust.m_weight > 0.9f;

        if (reset && spike)
            return record->m_body;
    }

    // 🔒 ONLY USE FOOT DELTA IF STABLE
    if (confidence > 0.6f && data->m_records.size() >= 3) {
        float offset = pred_foot_delta(data->m_records);

        if (fabsf(offset) > 1.0f)
            return math::NormalizedAngle(record->m_body + offset);
    }

    // 🔒 REMOVE CURVATURE (too unstable)
    // (yes, completely remove it)

    // 🔒 CLAMP USING DESYNC LIMIT
    if (state) {
        float max_desync = state->m_fl_aim_yaw_max;
        float delta = math::NormalizedAngle(record->m_body - prev->m_body);

        if (fabsf(delta) > max_desync - 1.f) {
            return math::NormalizedAngle(record->m_body + (delta > 0 ? max_desync : -max_desync));
        }
    }

    // fallback = safest
    return math::NormalizedAngle(record->m_body + 180.f);
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

//chatgpt FINAL FORM $$$
void Resolver::ResolveStand(AimPlayer* data, LagRecord* record, CCSGOPlayerAnimState* state) {
    if (!data || !record)
        return;

    if (g_menu.main.config.mode.get() == 1) {
        StandNS(data, record);
        return;
    }

    LagRecord* prev = FindPreviousRecord(data);
    float away = GetAwayAngle(record);

    float final_yaw = record->m_body; // default fallback

    // =========================================================
    // STEP 1: HARD LBY SNAP (ONLY TRUE HARD OVERRIDE)
    // =========================================================
    if (prev) {
        float delta = fabsf(math::NormalizedAngle(record->m_body - prev->m_body));

        if (delta > 35.f) {
            data->m_trusted_lby = record->m_body;
            data->m_trusted_lby_time = record->m_anim_time;

            record->m_eye_angles.y = record->m_body;
            return; // ONLY hard return allowed
        }
    }

    // =========================================================
    // STEP 2: BUILD CANDIDATE BASE
    // =========================================================
    std::vector<float> candidates;

    float body = record->m_body;

    candidates.push_back(body);
    candidates.push_back(body + 180.f);
    candidates.push_back(body + 90.f);
    candidates.push_back(body - 90.f);

    candidates.push_back(away + 180.f);
    candidates.push_back(away + 90.f);
    candidates.push_back(away - 90.f);

    // =========================================================
    // STEP 3: ADD FOOT PREDICTION (SAFE VERSION)
    // =========================================================
    if (data->m_records.size() >= 2) {
        const auto& a = *data->m_records[data->m_records.size() - 2];
        const auto& b = *data->m_records[data->m_records.size() - 1];

        float offset = pred_foot_delta(data->m_records);
        float scaled = std::clamp(offset * 2.5f, -180.f, 180.f);

        float foot = pred_foot_yaw(a, b, scaled);
        candidates.push_back(foot);
    }

    // =========================================================
    // STEP 4: ADD TRUSTED LBY (if recent)
    // =========================================================
    if (data->m_trusted_lby_time > 0.f) {
        float dt = record->m_anim_time - data->m_trusted_lby_time;

        if (dt < 1.1f) {
            candidates.push_back(data->m_trusted_lby);
            candidates.push_back(data->m_trusted_lby + 180.f);
        }
    }

    // =========================================================
    // STEP 5: MISS-BASED ADAPTATION (VERY IMPORTANT)
    // =========================================================
    if (data->m_resolve_history.miss_side > 0) {
        candidates.push_back(body - 90.f);
    }

    if (data->m_resolve_history.miss_invert > 0) {
        candidates.push_back(body + 90.f);
    }

    if (data->m_resolve_history.miss_bruteforce > 1) {
        candidates.push_back(body + 180.f);
    }

    // =========================================================
    // STEP 6: SCORE EVERYTHING
    // =========================================================
    float best_score = -FLT_MAX;

    for (float yaw : candidates) {
        float norm = math::NormalizedAngle(yaw);
        float score = ScoreYaw(data, record, norm);

        if (score > best_score) {
            best_score = score;
            final_yaw = norm;
        }
    }

    record->m_eye_angles.y = final_yaw;
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

float Resolver::ComputeConfidence(AimPlayer* data) {
    if (data->m_records.size() < 2)
        return 0.0f;

    LagRecord* a = data->m_records[0].get();
    LagRecord* b = data->m_records[1].get();

    float dt = a->m_sim_time - b->m_sim_time;
    float ideal = g_csgo.m_globals->m_interval;

    float jitter = fabsf(dt - ideal);
    float ratio = std::clamp(jitter / ideal, 0.f, 1.f);

    float confidence = 1.f - ratio;

    float dist = (a->m_origin - b->m_origin).length();
    if (dist > 64.f)
        confidence *= 0.5f;

    return std::clamp(confidence, 0.1f, 1.f);
}

void Resolver::OnMiss(AimPlayer* data) {
    float shot = data->m_resolve_history.last_angle;
    float body = data->m_body;

    float delta = math::NormalizedAngle(shot - body);

    // categorize miss
    if (fabsf(delta) < 20.f)
        data->m_resolve_history.miss_bruteforce++;

    else if (delta > 0.f)
        data->m_resolve_history.miss_side++;     // right side missed

    else
        data->m_resolve_history.miss_invert++;   // left side missed
}

void Resolver::OnHit(AimPlayer* data) {
    data->m_resolve_history.last_hit_angle =
        data->m_resolve_history.last_angle;

    // reset misses (important)
    data->m_resolve_history.miss_bruteforce = 0;
    data->m_resolve_history.miss_side = 0;
    data->m_resolve_history.miss_invert = 0;
}

float Resolver::GetMaxDesyncDelta(CCSGOPlayerAnimState* state) {
    if (!state)
        return 60.f;

    float yaw_max = state->m_fl_aim_yaw_max;
    float yaw_min = state->m_fl_aim_yaw_min;

    // take the larger magnitude
    float delta = std::max(fabsf(yaw_max), fabsf(yaw_min));

    // safety clamp (engine limits)
    return std::clamp(delta, 20.f, 120.f);
}