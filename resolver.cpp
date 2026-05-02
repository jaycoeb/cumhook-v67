#include "includes.h"
#include <numeric>

Resolver g_resolver{};

// ============================================================
// UTILITIES
// ============================================================

static float NormalizeYaw(float yaw) {
    return math::NormalizedAngle(yaw);
}

static bool IsAngleRepeated(AimPlayer* data, float yaw) {
    for (float tried : data->m_tried_angles) {
        if (fabsf(math::NormalizedAngle(yaw - tried)) < 15.f)
            return true;
    }
    return false;
}

static AimPlayer::AngleStat* GetAngleStat(AimPlayer* data, float yaw) {
    yaw = NormalizeYaw(yaw);

    for (auto& s : data->m_angle_stats) {
        if (fabsf(math::NormalizedAngle(s.yaw - yaw)) < 10.f)
            return &s;
    }

    data->m_angle_stats.push_back({ yaw, 0, 0 });

    if (data->m_angle_stats.size() > 16)
        data->m_angle_stats.erase(data->m_angle_stats.begin());

    return &data->m_angle_stats.back();
}

// ============================================================
// EXPLOIT DETECTION
// ============================================================

// Detects fakemove — player sends movement input to invoke
// the walk resolver while their origin barely moves.
static bool IsFakeMove(AimPlayer* data, LagRecord* record) {
    float anim_speed = record->m_anim_velocity.length();
    if (anim_speed < 0.15f)
        return false;

    if (data->m_records.size() < 2)
        return false;

    LagRecord* prev = data->m_records[1].get();
    if (!prev || !prev->valid())
        return false;

    float time_delta = record->m_sim_time - prev->m_sim_time;
    if (time_delta <= 0.f)
        return false;

    float real_speed = (record->m_origin - prev->m_origin).length() / time_delta;

    if (real_speed < 5.f) {
        // Layer 6 = move layer. Real walking always advances cycle.
        const auto& mc = record->m_layers[6];
        const auto& mp = prev->m_layers[6];

        bool cycle_advancing =
            mc.m_cycle > mp.m_cycle ||
            (mp.m_cycle > 0.9f && mc.m_cycle < 0.1f);

        if (mc.m_weight > 0.1f && !cycle_advancing)
            return true;

        if (real_speed < 1.f)
            return true;
    }

    return false;
}

// ============================================================
// LAST MOVE HELPERS
// ============================================================

static LagRecord* FindLastMoveRecord(AimPlayer* data) {
    for (const auto& rec : data->m_records) {
        if (!rec || rec->dormant() || !rec->valid())
            continue;

        if (IsFakeMove(data, rec.get()))
            continue;

        if (rec->m_mode == Resolver::Modes::RESOLVE_WALK)
            return rec.get();

        if (rec->m_anim_velocity.length() > 20.f)
            return rec.get();
    }
    return nullptr;
}

static float GetLastMoveBody(AimPlayer* data, LagRecord* current) {
    LagRecord* lm = FindLastMoveRecord(data);
    return lm ? lm->m_body : current->m_body;
}

static float GetTimeSinceLastMove(AimPlayer* data, LagRecord* current) {
    LagRecord* lm = FindLastMoveRecord(data);
    if (!lm) return 9999.f;
    return current->m_anim_time - lm->m_anim_time;
}

// ============================================================
// RECORD FINDERS
// ============================================================

LagRecord* Resolver::FindIdealRecord(AimPlayer* data) {
    if (data->m_records.empty())
        return nullptr;

    LagRecord* first_valid = nullptr;
    LagRecord* best_shot = nullptr;
    LagRecord* best_walk = nullptr;

    for (const auto& it : data->m_records) {
        if (it->dormant() || it->immune() || !it->valid())
            continue;

        LagRecord* current = it.get();

        if (!first_valid)
            first_valid = current;

        if (it->m_shot && !best_shot)
            best_shot = current;

        if (!IsFakeMove(data, current) &&
            (it->m_mode == Modes::RESOLVE_WALK ||
                it->m_mode == Modes::RESOLVE_NONE) &&
            !best_walk)
            best_walk = current;
    }

    if (best_shot) return best_shot;
    if (best_walk) return best_walk;
    return first_valid;
}

LagRecord* Resolver::FindLastRecord(AimPlayer* data) {
    if (data->m_records.empty())
        return nullptr;

    for (auto it = data->m_records.crbegin(); it != data->m_records.crend(); ++it) {
        LagRecord* current = it->get();
        if (current->valid() && !current->immune() && !current->dormant())
            return current;
    }

    return nullptr;
}

LagRecord* Resolver::FindPreviousRecord(AimPlayer* data) {
    if (data->m_records.size() < 2)
        return nullptr;
    return data->m_records[1].get();
}

// ============================================================
// MODE DETECTION (exploit-aware)
// ============================================================

void Resolver::SetMode(LagRecord* record, AimPlayer* data) {
    float speed = record->m_anim_velocity.length();

    // Validate anim speed against real origin delta to catch fakemove.
    if (speed > 0.15f && data->m_records.size() >= 2) {
        LagRecord* prev = data->m_records[1].get();
        if (prev && prev->valid()) {
            float time_delta = record->m_sim_time - prev->m_sim_time;
            float real_speed = (time_delta > 0.f)
                ? (record->m_origin - prev->m_origin).length() / time_delta
                : 0.f;

            if (real_speed < 5.f) {
                const auto& mc = record->m_layers[6];
                const auto& mp = prev->m_layers[6];

                bool cycle_ok =
                    mc.m_cycle > mp.m_cycle ||
                    (mp.m_cycle > 0.9f && mc.m_cycle < 0.1f);

                if ((mc.m_weight > 0.1f && !cycle_ok) || real_speed < 1.f)
                    speed = 0.f;
            }
        }
    }

    if (!(record->m_flags & FL_ONGROUND))
        record->m_mode = Modes::RESOLVE_AIR;
    else if (speed > 0.15f && !record->m_fake_walk)
        record->m_mode = Modes::RESOLVE_WALK;
    else
        record->m_mode = Modes::RESOLVE_STAND;
}

// ============================================================
// BODY / EVENTS
// ============================================================

void Resolver::OnBodyUpdate(Player* player, float value) {
    AimPlayer* data = &g_aimbot.m_players[player->index() - 1];
    data->m_old_body = data->m_body;
    data->m_body = value;
}

float Resolver::GetAwayAngle(LagRecord* record) {
    if (!record) return 0.f;

    ang_t away;
    math::VectorAngles(g_cl.m_local->m_vecOrigin() - record->m_pred_origin, away);
    return away.y;
}

void Resolver::MatchShot(AimPlayer* data, LagRecord* record) {
    if (g_menu.main.config.mode.get() == 1)
        return;

    float shoot_time = -1.f;
    Weapon* weapon = data->m_player->GetActiveWeapon();
    if (weapon)
        shoot_time = weapon->m_fLastShotTime() + g_csgo.m_globals->m_interval;

    if (game::TIME_TO_TICKS(shoot_time) == game::TIME_TO_TICKS(record->m_sim_time)) {
        if (record->m_lag <= 2)
            record->m_shot = true;
        else if (data->m_records.size() >= 2) {
            LagRecord* previous = data->m_records[1].get();
            if (previous && !previous->dormant())
                record->m_eye_angles.x = previous->m_eye_angles.x;
        }
    }
}

// ============================================================
// CONFIDENCE
// ============================================================

float Resolver::ComputeConfidence(AimPlayer* data) {
    if (data->m_records.size() < 2)
        return 0.f;

    LagRecord* a = data->m_records[0].get();
    LagRecord* b = data->m_records[1].get();

    float dt = a->m_sim_time - b->m_sim_time;
    float ideal = g_csgo.m_globals->m_interval;
    float jitter = fabsf(dt - ideal);
    float ratio = std::clamp(jitter / ideal, 0.f, 1.f);
    float conf = 1.f - ratio;

    if ((a->m_origin - b->m_origin).length() > 64.f)
        conf *= 0.5f;

    return std::clamp(conf, 0.1f, 1.f);
}

// ============================================================
// RESOLVE ANGLES — top level
// ============================================================

void Resolver::ResolveAngles(Player* player, LagRecord* record) {
    AimPlayer* data = &g_aimbot.m_players[player->index() - 1];

    MatchShot(data, record);
    SetMode(record, data);

    if (g_menu.main.config.mode.get() == 1)
        record->m_eye_angles.x = 90.f;

    // Random resolver — meme mode, overrides everything.
    if (g_menu.main.aimbot.random_resolver.get()) {
        record->m_eye_angles.y = (float)(rand() % 360) - 180.f;
        math::NormalizeAngle(record->m_eye_angles.y);
        data->m_resolve_history.last_angle = record->m_eye_angles.y;
        return;
    }

    if (record->m_mode == Modes::RESOLVE_WALK)
        ResolveWalk(data, record);
    else if (record->m_mode == Modes::RESOLVE_STAND)
        ResolveStand(data, record, player->m_PlayerAnimState());
    else if (record->m_mode == Modes::RESOLVE_AIR)
        ResolveAir(data, record);

    math::NormalizeAngle(record->m_eye_angles.y);
    data->m_resolve_history.last_angle = record->m_eye_angles.y;
}

// ============================================================
// RESOLVE WALK
// Walking: no desync active, body = real yaw.
// ============================================================

void Resolver::ResolveWalk(AimPlayer* data, LagRecord* record) {
    record->m_eye_angles.y = record->m_body;

    data->m_body_update = record->m_anim_time + 0.22f;
    data->m_stand_index = 0;
    data->m_stand_index2 = 0;
    data->m_body_index = 0;

    std::memcpy(&data->m_walk_record, record, sizeof(LagRecord));
}

// ============================================================
// RESOLVE STAND
//
// How 2018 desync works:
//  - m_body (LBY) = lower body yaw, last updated when moving.
//  - Engine clamps eye yaw to ±58° of LBY while standing still.
//  - We use the lastmove body as the reference since the engine
//    measures the desync delta from the last walk record, not
//    the current body value.
//  - We do NOT predict or time LBY flicks — detectable/countered.
//  - After misses we brute-cycle deterministically so we never
//    sit on the same wrong angle twice.
// ============================================================

void Resolver::ResolveStand(AimPlayer* data, LagRecord* record, CCSGOPlayerAnimState* state) {
    if (!data || !record)
        return;

    if (g_menu.main.config.mode.get() == 1) {
        StandNS(data, record);
        return;
    }

    float lastmove_body = GetLastMoveBody(data, record);
    float away = GetAwayAngle(record);
    float final_yaw = lastmove_body;

    // =========================================================
    // BRUTE CYCLE — fires after 2+ misses.
    // Advances by miss count so we never repeat an angle.
    // =========================================================
    int total_misses =
        data->m_resolve_history.miss_side +
        data->m_resolve_history.miss_invert +
        data->m_resolve_history.miss_center +
        data->m_resolve_history.miss_bruteforce;

    if (total_misses >= 2) {
        float cycle[] = {
            lastmove_body + 58.f,
            lastmove_body - 58.f,
            lastmove_body + 180.f,
            lastmove_body + 90.f,
            lastmove_body - 90.f,
            lastmove_body + 135.f,
            lastmove_body - 135.f,
        };
        int count = (int)(sizeof(cycle) / sizeof(cycle[0]));
        int idx = total_misses % count;

        record->m_eye_angles.y = math::NormalizedAngle(cycle[idx]);
        return;
    }

    // =========================================================
// DELAYED LBY FLICK DETECTION
// Some cheats delay the LBY update by 2-8 ticks to throw
// off resolvers that time the flick. We detect this by
// watching for a body delta that appeared late relative to
// when the animstate says it should have updated, then
// try the delayed angle for 1-2 shots before falling back.
// We only act on this if the player hasn't genuinely moved
// (which would reset LBY naturally and give a false positive).
// =========================================================

    bool player_moved_recently = GetTimeSinceLastMove(data, record) < 0.5f;

    if (!player_moved_recently && data->m_records.size() >= 3) {
        LagRecord* prev = data->m_records[1].get();
        LagRecord* prev2 = data->m_records[2].get();

        if (prev && prev->valid() && prev2 && prev2->valid()) {
            float body_now = record->m_body;
            float body_prev = prev->m_body;
            float body_old = prev2->m_body;

            float delta_now = fabsf(math::NormalizedAngle(body_now - body_prev));
            float delta_prev = fabsf(math::NormalizedAngle(body_prev - body_old));

            // A late LBY flick looks like: stable body for 2+ ticks,
            // then a sudden large snap. Real movement resets gradually.
            bool stable_before = delta_prev < 8.f;
            bool late_snap = delta_now > 35.f;

            if (stable_before && late_snap) {
                // We haven't tried this yet — use the snapped body directly
                // for up to 2 shots before the brute cycle takes over.
                int lby_shots_tried = data->m_resolve_history.miss_bruteforce +
                    data->m_resolve_history.miss_side +
                    data->m_resolve_history.miss_invert +
                    data->m_resolve_history.miss_center;

                if (lby_shots_tried < 2 && !IsAngleRepeated(data, body_now)) {
                    record->m_eye_angles.y = math::NormalizedAngle(body_now);
                    return;
                }

                // One shot at the opposite of the snapped body too.
                float opposite = math::NormalizedAngle(body_now + 180.f);
                if (lby_shots_tried < 2 && !IsAngleRepeated(data, opposite)) {
                    record->m_eye_angles.y = opposite;
                    return;
                }

                // Both failed — fall through to normal brute cycle,
                // which handles it from here.
            }
        }
    }

    // =========================================================
    // CANDIDATE SET
    // Anchored to lastmove body + engine desync cap (58°).
    // =========================================================
    std::vector<float> candidates;

    if (data->m_resolve_history.miss_center < 1)
        candidates.push_back(lastmove_body);

    candidates.push_back(lastmove_body + 58.f);
    candidates.push_back(lastmove_body - 58.f);
    candidates.push_back(lastmove_body + 180.f);
    candidates.push_back(lastmove_body + 90.f);
    candidates.push_back(lastmove_body - 90.f);

    candidates.push_back(away + 180.f);
    candidates.push_back(away + 90.f);
    candidates.push_back(away - 90.f);

    // Animstate-derived desync limit.
    if (state) {
        float max_desync = std::clamp(fabsf(state->m_fl_aim_yaw_max), 20.f, 58.f);
        candidates.push_back(lastmove_body + max_desync);
        candidates.push_back(lastmove_body - max_desync);
    }

    // Miss-based supplements.
    if (data->m_resolve_history.miss_side > 0)
        candidates.push_back(lastmove_body - 58.f);
    if (data->m_resolve_history.miss_invert > 0)
        candidates.push_back(lastmove_body + 58.f);
    if (data->m_resolve_history.miss_bruteforce > 1)
        candidates.push_back(lastmove_body + 180.f);

    // =========================================================
    // NORMALIZE + DEDUPLICATE
    // =========================================================
    for (auto& y : candidates)
        y = math::NormalizedAngle(y);

    std::sort(candidates.begin(), candidates.end());
    candidates.erase(
        std::unique(candidates.begin(), candidates.end(),
            [](float a, float b) {
                return fabsf(math::NormalizedAngle(a - b)) < 1.f;
            }),
        candidates.end()
    );

    // Remove confirmed bad directions.
    candidates.erase(
        std::remove_if(candidates.begin(), candidates.end(),
            [&](float yaw) {
                float d = math::NormalizedAngle(yaw - lastmove_body);
                if (fabsf(d) < 20.f && data->m_resolve_history.miss_center >= 2) return true;
                if (d > 0.f && data->m_resolve_history.miss_side >= 2) return true;
                if (d < 0.f && data->m_resolve_history.miss_invert >= 2) return true;
                return false;
            }),
        candidates.end()
    );

    // =========================================================
    // SCORE + SELECT
    // =========================================================
    float best_score = -FLT_MAX;

    for (float yaw : candidates) {
        float norm = math::NormalizedAngle(yaw);
        float score = 0.f;
        float d = math::NormalizedAngle(norm - lastmove_body);

        // Proximity to lastmove body.
        score += (180.f - fabsf(d)) * 0.7f;

        // Away angle.
        score += (180.f - fabsf(math::NormalizedAngle(norm - away))) * 0.3f;

        // Stability vs last resolved angle.
        float prev_d = fabsf(math::NormalizedAngle(norm - data->m_resolve_history.last_angle));
        score += (180.f - prev_d) * 0.2f;

        // Miss penalties.
        if (fabsf(d) < 20.f)     score -= data->m_resolve_history.miss_center * 60.f;
        else if (d > 0.f)        score -= data->m_resolve_history.miss_side * 60.f;
        else                     score -= data->m_resolve_history.miss_invert * 60.f;
        if (fabsf(d) > 140.f)   score -= data->m_resolve_history.miss_bruteforce * 60.f;

        // Hard deny center if already missed it once.
        if (fabsf(d) < 20.f && data->m_resolve_history.miss_center >= 1)
            score = -FLT_MAX;

        // Anti-repeat.
        if (fabsf(math::NormalizedAngle(norm - data->m_resolve_history.last_angle)) < 10.f)
            score -= 1000.f;

        for (auto& tried : data->m_tried_angles) {
            if (fabsf(math::NormalizedAngle(norm - tried)) < 10.f)
                score -= 500.f;
        }

        // Angle stat history.
        auto* stat = GetAngleStat(data, norm);
        if (stat) {
            score += stat->hits * 50.f;
            score -= stat->misses * 40.f;
        }

        if (score > best_score) {
            best_score = score;
            final_yaw = norm;
        }
    }

    record->m_eye_angles.y = final_yaw;
    math::NormalizeAngle(record->m_eye_angles.y);
}

// ============================================================
// STAND NS
// ============================================================

void Resolver::StandNS(AimPlayer* data, LagRecord* record) {
    float away = GetAwayAngle(record);

    switch (data->m_shots % 8) {
    case 0: record->m_eye_angles.y = away + 180.f; break;
    case 1: record->m_eye_angles.y = away + 90.f;  break;
    case 2: record->m_eye_angles.y = away - 90.f;  break;
    case 3: record->m_eye_angles.y = away + 45.f;  break;
    case 4: record->m_eye_angles.y = away - 45.f;  break;
    case 5: record->m_eye_angles.y = away + 135.f; break;
    case 6: record->m_eye_angles.y = away - 135.f; break;
    case 7: record->m_eye_angles.y = away;          break;
    default: break;
    }

    record->m_body = record->m_eye_angles.y;
}

// ============================================================
// RESOLVE AIR
// ============================================================

void Resolver::ResolveAir(AimPlayer* data, LagRecord* record) {
    if (g_menu.main.config.mode.get() == 1) {
        AirNS(data, record);
        return;
    }

    if (record->m_velocity.length_2d() < 60.f) {
        record->m_mode = Modes::RESOLVE_STAND;
        ResolveStand(data, record, record->m_player->m_PlayerAnimState());
        return;
    }

    float velyaw = math::rad_to_deg(std::atan2(record->m_velocity.y, record->m_velocity.x));

    switch (data->m_shots % 3) {
    case 0: record->m_eye_angles.y = velyaw + 180.f; break;
    case 1: record->m_eye_angles.y = velyaw - 90.f;  break;
    case 2: record->m_eye_angles.y = velyaw + 90.f;  break;
    }
}

void Resolver::AirNS(AimPlayer* data, LagRecord* record) {
    float away = GetAwayAngle(record);

    switch (data->m_shots % 9) {
    case 0: record->m_eye_angles.y = away + 180.f; break;
    case 1: record->m_eye_angles.y = away + 150.f; break;
    case 2: record->m_eye_angles.y = away - 150.f; break;
    case 3: record->m_eye_angles.y = away + 165.f; break;
    case 4: record->m_eye_angles.y = away - 165.f; break;
    case 5: record->m_eye_angles.y = away + 135.f; break;
    case 6: record->m_eye_angles.y = away - 135.f; break;
    case 7: record->m_eye_angles.y = away + 90.f;  break;
    case 8: record->m_eye_angles.y = away - 90.f;  break;
    default: break;
    }
}

// ============================================================
// RESOLVE POSES
// ============================================================

void Resolver::ResolvePoses(Player* player, LagRecord* record) {
    if (record->m_mode == Modes::RESOLVE_AIR) {
        player->m_flPoseParameter()[2] = g_csgo.RandomInt(0, 4) * 0.25f;
        player->m_flPoseParameter()[11] = g_csgo.RandomInt(1, 3) * 0.25f;
    }
}

// ============================================================
// CALCULATE STAND (used by prediction)
// ============================================================

float Resolver::CalculateStand(AimPlayer* data, LagRecord* record, LagRecord* prev, CCSGOPlayerAnimState* state) {
    if (!data || !record || !prev)
        return record ? record->m_body : 0.f;

    float lastmove_body = GetLastMoveBody(data, record);
    float time_since_lm = GetTimeSinceLastMove(data, record);
    float confidence = ComputeConfidence(data);

    if (time_since_lm < 0.3f)
        return lastmove_body;

    if (confidence < 0.4f)
        return math::NormalizedAngle(lastmove_body + 180.f);

    if (state) {
        float max_desync = std::clamp(fabsf(state->m_fl_aim_yaw_max), 20.f, 58.f);
        float delta = math::NormalizedAngle(record->m_body - lastmove_body);

        if (fabsf(delta) > max_desync - 1.f)
            return math::NormalizedAngle(lastmove_body + (delta > 0.f ? max_desync : -max_desync));
    }

    return math::NormalizedAngle(lastmove_body + 180.f);
}

// ============================================================
// ON MISS
// ============================================================

void Resolver::OnMiss(AimPlayer* data) {
    float shot = data->m_resolve_history.last_angle;
    LagRecord* last = FindLastRecord(data);
    float lastmove_body = last ? GetLastMoveBody(data, last) : data->m_body;
    float delta = math::NormalizedAngle(shot - lastmove_body);
    const char* type = "UNKNOWN";

    if (fabsf(delta) < 20.f) {
        data->m_resolve_history.miss_center++;
        type = "CENTER";
    }
    else if (delta > 0.f && fabsf(delta) <= 140.f) {
        data->m_resolve_history.miss_side++;
        type = "RIGHT";
    }
    else if (delta < 0.f && fabsf(delta) <= 140.f) {
        data->m_resolve_history.miss_invert++;
        type = "LEFT";
    }
    else {
        data->m_resolve_history.miss_bruteforce++;
        type = "OPPOSITE";
    }

    float norm = math::NormalizedAngle(shot);
    data->m_tried_angles.push_front(norm);

    if (data->m_tried_angles.size() > 8)
        data->m_tried_angles.pop_back();

    auto* stat = GetAngleStat(data, norm);
    if (stat) stat->misses++;

    for (auto& s : data->m_angle_stats) {
        if (s.misses > 0) s.misses--;
    }

    g_csgo.m_cvar->ConsoleColorPrintf(
        { 255, 200, 100, 255 },
        "[RESOLVER MISS] delta: %.1f -> %s | total misses: %d\n",
        delta, type,
        data->m_resolve_history.miss_side +
        data->m_resolve_history.miss_invert +
        data->m_resolve_history.miss_center +
        data->m_resolve_history.miss_bruteforce
    );
}

// ============================================================
// ON HIT
// ============================================================

void Resolver::OnHit(AimPlayer* data) {
    data->m_resolve_history.last_hit_angle = data->m_resolve_history.last_angle;

    // Only clear brute state if player genuinely moved —
    // prevents tiny jiggle / breaklastmove from wiping progress.
    bool real_movement = false;
    if (data->m_records.size() >= 2) {
        LagRecord* a = data->m_records[0].get();
        LagRecord* b = data->m_records[1].get();
        if (a && b) {
            float dt = a->m_sim_time - b->m_sim_time;
            float speed = (dt > 0.f) ? (a->m_origin - b->m_origin).length() / dt : 0.f;
            real_movement = speed > 80.f;
        }
    }

    if (real_movement) {
        data->m_resolve_history.miss_bruteforce = 0;
        data->m_resolve_history.miss_side = 0;
        data->m_resolve_history.miss_invert = 0;
        data->m_resolve_history.miss_center = 0;
        data->m_resolve_history.brute_index = 0;
        data->m_tried_angles.clear();
    }

    float yaw = data->m_resolve_history.last_angle;
    auto* stat = GetAngleStat(data, yaw);
    if (stat) stat->hits++;
}

// ============================================================
// MAX DESYNC DELTA
// ============================================================

float Resolver::GetMaxDesyncDelta(CCSGOPlayerAnimState* state) {
    if (!state) return 58.f;
    float delta = std::max(fabsf(state->m_fl_aim_yaw_max), fabsf(state->m_fl_aim_yaw_min));
    return std::clamp(delta, 20.f, 58.f);
}