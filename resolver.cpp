#include "includes.h"
#include <numeric>

// ============================================================
// LASTMOVE RESOLVER
// 2018 HvH resolves desync relative to the last walk record.
// The engine clamps fake yaw within ±58° of the lastmove body.
// We backtrack to that record and brute the desync range from it.
// ============================================================

static bool IsAngleRepeated(AimPlayer* data, float yaw) {
    for (float tried : data->m_tried_angles) {
        float diff = fabsf(math::NormalizedAngle(yaw - tried));
        if (diff < 15.f)
            return true;
    }
    return false;
}

static float NormalizeYaw(float yaw) {
    return math::NormalizedAngle(yaw);
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
// LASTMOVE HELPERS
// ============================================================

// Returns the most recent record where the player was actually walking.
// This is the reference yaw the engine locks desync against in 2018.
static LagRecord* FindLastMoveRecord(AimPlayer* data) {
    for (const auto& rec : data->m_records) {
        if (!rec || rec->dormant() || !rec->valid())
            continue;

        if (rec->m_mode == Resolver::Modes::RESOLVE_WALK)
            return rec.get();

        // also accept any record with meaningful velocity even if mode isn't set yet
        if (rec->m_anim_velocity.length() > 20.f)
            return rec.get();
    }
    return nullptr;
}

// Returns the body yaw from the lastmove record, or current body as fallback.
static float GetLastMoveBody(AimPlayer* data, LagRecord* current) {
    LagRecord* lm = FindLastMoveRecord(data);
    if (lm)
        return lm->m_body;
    return current->m_body;
}

// How long ago (in seconds) the player last moved.
static float GetTimeSinceLastMove(AimPlayer* data, LagRecord* current) {
    LagRecord* lm = FindLastMoveRecord(data);
    if (!lm)
        return 9999.f;
    return current->m_anim_time - lm->m_anim_time;
}

// ============================================================
// FOOT DELTA — now relative to lastmove body
// ============================================================

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
        float scaled = fabsf(max_abs_delta) * 2.0f;
        if (scaled < 30.f)  scaled = 30.f;
        if (scaled > 180.f) scaled = 180.f;
        return -sign * scaled;
    }

    return 0.0f;
}

// pred_foot_yaw — offset applied on top of lastmove body, not current body.
static float pred_foot_yaw_from_lastmove(float lastmove_body,
    const LagRecord& a,
    const LagRecord& b,
    float predicted_offset) {
    constexpr float foot_speed = 80.0f;

    const float dt = b.m_anim_time - a.m_anim_time;
    if (dt <= 0.0f)
        return lastmove_body;

    const float max_delta = foot_speed * dt;
    const float body_delta = math::NormalizedAngle(b.m_body - a.m_body);
    const float abs_foot_delta = math::NormalizedAngle(b.m_abs_ang.y - a.m_abs_ang.y);
    const float blended_delta = (body_delta * 0.5f) + (abs_foot_delta * 0.5f);
    const float abs_blended = fabsf(blended_delta);

    float resolved;
    if (abs_blended <= max_delta) {
        resolved = lastmove_body + blended_delta;
    }
    else {
        resolved = lastmove_body + copysignf(max_delta, blended_delta);
        resolved = math::NormalizedAngle(resolved + predicted_offset);
    }

    return math::NormalizedAngle(resolved);
}

static float curvature_heuristic(const std::deque<std::shared_ptr<LagRecord>>& records) {
    if (records.size() < 4)
        return 0.f;

    float total = 0.f;
    float prev_vel = 0.f;
    bool  has_prev = false;
    int   count = std::min((int)records.size(), 6);

    for (int i = 1; i < count; ++i) {
        const auto& curr = records[i];
        const auto& prev = records[i - 1];

        float dt = curr->m_anim_time - prev->m_anim_time;
        if (dt <= 0.f) continue;

        float dyaw = math::NormalizedAngle(curr->m_body - prev->m_body);
        float vel = dyaw / dt;

        if (has_prev)
            total += (vel - prev_vel);

        prev_vel = vel;
        has_prev = true;
    }

    return total / (float)(count - 1);
}

// ============================================================
// SCORE YAW — now computes delta relative to lastmove body
// ============================================================

static float ScoreYaw(AimPlayer* data, LagRecord* record, float yaw, float lastmove_body) {
    float score = 0.f;

    // --------------------------------------------------
    // 1. Proximity to lastmove body (engine desync reference)
    // --------------------------------------------------
    float lm_delta = fabsf(math::NormalizedAngle(yaw - lastmove_body));
    score += (180.f - lm_delta) * 0.7f;   // weighted heavier than before

    // --------------------------------------------------
    // 2. Movement direction
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
    // 3. Previous resolved stability
    // --------------------------------------------------
    if (!data->m_records.empty()) {
        float prev = data->m_resolve_history.last_angle;
        float delta = fabsf(math::NormalizedAngle(yaw - prev));
        score += (180.f - delta) * 0.25f;
    }

    float delta = math::NormalizedAngle(yaw - lastmove_body);

    // ==================================================
    // MISS ADAPTATION (classified against lastmove body)
    // ==================================================

    if (fabsf(delta) < 20.f) {
        if (data->m_resolve_history.miss_center >= 1)
            return -FLT_MAX; // hard deny center
    }

    if (data->m_resolve_history.miss_side > 0) {
        if (delta > 0.f) score -= 90.f;
        else             score += 25.f;
    }

    if (data->m_resolve_history.miss_invert > 0) {
        if (delta < 0.f) score -= 90.f;
        else             score += 25.f;
    }

    if (data->m_resolve_history.miss_bruteforce > 0) {
        if (fabsf(delta) > 140.f)
            score -= 90.f;
    }

    int total_misses =
        data->m_resolve_history.miss_side +
        data->m_resolve_history.miss_invert +
        data->m_resolve_history.miss_center +
        data->m_resolve_history.miss_bruteforce;

    if (total_misses >= 3) {
        if (fabsf(delta) > 140.f)
            score += 40.f;
    }

    // ==================================================
    // HARD ANTI-REPEAT
    // ==================================================

    float last = data->m_resolve_history.last_angle;
    float diff = fabsf(math::NormalizedAngle(yaw - last));
    if (diff < 10.f)
        score -= 1000.f;

    for (auto& tried : data->m_tried_angles) {
        float d = fabsf(math::NormalizedAngle(yaw - tried));
        if (d < 10.f)
            score -= 500.f;
    }

    return score;
}

// ============================================================
// SELECT BEST YAW — candidates built around lastmove body
// ============================================================

static float SelectBestYaw(AimPlayer* data, LagRecord* record) {
    float lastmove_body = GetLastMoveBody(data, record);
    float away = g_resolver.GetAwayAngle(record);

    std::vector<Resolver::ResolveCandidate> candidates;

    // Core desync angles relative to lastmove body (+58 = max engine desync)
    candidates.push_back({ lastmove_body,         0.f });
    candidates.push_back({ lastmove_body + 58.f,  0.f });
    candidates.push_back({ lastmove_body - 58.f,  0.f });
    candidates.push_back({ lastmove_body + 180.f, 0.f });
    candidates.push_back({ lastmove_body + 90.f,  0.f });
    candidates.push_back({ lastmove_body - 90.f,  0.f });

    // Away-based supplements
    candidates.push_back({ away + 180.f, 0.f });
    candidates.push_back({ away + 90.f,  0.f });
    candidates.push_back({ away - 90.f,  0.f });

    // Foot prediction from lastmove body
    if (data->m_records.size() >= 2) {
        const auto& a = *data->m_records[data->m_records.size() - 2];
        const auto& b = *data->m_records[data->m_records.size() - 1];

        float offset = pred_foot_delta(data->m_records);
        float scaled = std::clamp(offset * 3.f, -180.f, 180.f);

        float foot = pred_foot_yaw_from_lastmove(lastmove_body, a, b, scaled);
        candidates.push_back({ foot, 0.f });
    }

    // Curvature hint (minor)
    if (data->m_records.size() >= 4) {
        float curv = curvature_heuristic(data->m_records);
        candidates.push_back({ lastmove_body + curv, 0.f });
    }

    float best_score = -FLT_MAX;
    float best_yaw = lastmove_body;

    for (auto& c : candidates) {
        c.yaw = math::NormalizedAngle(c.yaw);
        c.score = ScoreYaw(data, record, c.yaw, lastmove_body);

        if (c.score > best_score) {
            best_score = c.score;
            best_yaw = c.yaw;
        }
    }

    return best_yaw;
}

// ============================================================
// LBY SPAM DETECTION (kept for shot matching, not resolving)
// ============================================================

static bool lby_spam(const LagRecord& curr, const LagRecord& prev) {
    const auto& layer0_curr = curr.m_layers[0];
    const auto& layer0_prev = prev.m_layers[0];

    return (
        layer0_curr.m_sequence == layer0_prev.m_sequence &&
        layer0_curr.m_cycle < layer0_prev.m_cycle &&
        fabsf(layer0_curr.m_playback_rate - layer0_prev.m_playback_rate) > 0.02f
        );
}

Resolver g_resolver{};

// ============================================================
// RECORD FINDERS
// ============================================================

LagRecord* Resolver::FindIdealRecord(AimPlayer* data) {
    LagRecord* first_valid = nullptr;

    if (data->m_records.empty())
        return nullptr;

    for (const auto& it : data->m_records) {
        if (it->dormant() || it->immune() || !it->valid())
            continue;

        LagRecord* current = it.get();

        if (!first_valid)
            first_valid = current;

        if (it->m_shot || it->m_mode == Modes::RESOLVE_BODY ||
            it->m_mode == Modes::RESOLVE_WALK || it->m_mode == Modes::RESOLVE_NONE)
            return current;
    }

    return first_valid ? first_valid : nullptr;
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
// BODY / AIM EVENTS
// ============================================================

void Resolver::OnBodyUpdate(Player* player, float value) {
    AimPlayer* data = &g_aimbot.m_players[player->index() - 1];
    data->m_old_body = data->m_body;
    data->m_body = value;
}

float Resolver::GetAwayAngle(LagRecord* record) {
    if (!record)
        return 0.0f;

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

void Resolver::SetMode(LagRecord* record) {
    float speed = record->m_anim_velocity.length();

    if (!(record->m_flags & FL_ONGROUND))
        record->m_mode = Modes::RESOLVE_AIR;
    else if (speed > 0.15f && !record->m_fake_walk)
        record->m_mode = Modes::RESOLVE_WALK;
    else
        record->m_mode = Modes::RESOLVE_STAND;
}

// ============================================================
// RESOLVE ANGLES — top-level dispatch
// ============================================================

void Resolver::ResolveAngles(Player* player, LagRecord* record) {
    AimPlayer* data = &g_aimbot.m_players[player->index() - 1];

    MatchShot(data, record);
    SetMode(record);

    if (g_menu.main.config.mode.get() == 1)
        record->m_eye_angles.x = 90.f;

    if (g_menu.main.aimbot.random_resolver.get()) {
        record->m_eye_angles.y = rand() % 360;
    }
    else {
        record->m_eye_angles.y = SelectBestYaw(data, record);

        if (record->m_mode == Modes::RESOLVE_WALK)
            ResolveWalk(data, record);
        else if (record->m_mode == Modes::RESOLVE_STAND)
            ResolveStand(data, record, player->m_PlayerAnimState());
        else if (record->m_mode == Modes::RESOLVE_AIR)
            ResolveAir(data, record);
    }

    math::NormalizeAngle(record->m_eye_angles.y);
    data->m_resolve_history.last_angle = record->m_eye_angles.y;
}

// ============================================================
// CALCULATE STAND — uses lastmove body as desync reference
// ============================================================

float Resolver::CalculateStand(AimPlayer* data, LagRecord* record, LagRecord* prev, CCSGOPlayerAnimState* state) {
    if (!data || !record || !prev)
        return record ? record->m_body : 0.f;

    float confidence = ComputeConfidence(data);
    float lastmove_body = GetLastMoveBody(data, record);
    float time_since_lm = GetTimeSinceLastMove(data, record);

    // if the player stopped very recently, trust lastmove body directly
    if (time_since_lm < 0.3f)
        return lastmove_body;

    // low confidence — flip lastmove by 180
    if (confidence < 0.4f)
        return math::NormalizedAngle(lastmove_body + 180.f);

    // foot prediction anchored to lastmove
    if (confidence > 0.6f && data->m_records.size() >= 3) {
        float offset = pred_foot_delta(data->m_records);
        if (fabsf(offset) > 1.0f)
            return math::NormalizedAngle(lastmove_body + offset);
    }

    // clamp using max desync delta from animstate
    if (state) {
        float max_desync = state->m_fl_aim_yaw_max;
        float delta = math::NormalizedAngle(record->m_body - lastmove_body);

        if (fabsf(delta) > max_desync - 1.f)
            return math::NormalizedAngle(lastmove_body + (delta > 0 ? max_desync : -max_desync));
    }

    return math::NormalizedAngle(lastmove_body + 180.f);
}

// ============================================================
// RESOLVE WALK
// ============================================================

void Resolver::ResolveWalk(AimPlayer* data, LagRecord* record) {
    // Walking: eye yaw = body yaw, no desync active.
    record->m_eye_angles.y = record->m_body;

    data->m_body_update = record->m_anim_time + 0.22f;
    data->m_stand_index = 0;
    data->m_stand_index2 = 0;
    data->m_body_index = 0;

    // Store as the authoritative lastmove record.
    std::memcpy(&data->m_walk_record, record, sizeof(LagRecord));
}

// ============================================================
// RESOLVE STAND — backtracks to lastmove, bruteforces desync
// ============================================================

void Resolver::ResolveStand(AimPlayer* data, LagRecord* record, CCSGOPlayerAnimState* state) {
    if (!data || !record)
        return;

    if (g_menu.main.config.mode.get() == 1) {
        StandNS(data, record);
        return;
    }

    LagRecord* prev = FindPreviousRecord(data);
    float       away = GetAwayAngle(record);
    float       lastmove_body = GetLastMoveBody(data, record);
    float       final_yaw = lastmove_body; // default = lastmove reference

    // =========================================================
    // STEP 1: FORCE BRUTE AFTER REPEATED MISSES (anti-repeat)
    // =========================================================
    int misses =
        data->m_resolve_history.miss_side +
        data->m_resolve_history.miss_invert +
        data->m_resolve_history.miss_bruteforce;

    if (misses >= 2) {
        // Walk the desync range in steps until we find an untried angle
        float options[] = {
            lastmove_body + 58.f,
            lastmove_body - 58.f,
            lastmove_body + 180.f,
            lastmove_body + 90.f,
            lastmove_body - 90.f,
        };

        for (float yaw : options) {
            yaw = math::NormalizedAngle(yaw);
            if (!IsAngleRepeated(data, yaw)) {
                record->m_eye_angles.y = yaw;
                return;
            }
        }
    }

    // =========================================================
    // STEP 2: BUILD CANDIDATE SET (lastmove-relative)
    // =========================================================
    std::vector<float> candidates;

    // Desync range anchored to lastmove body
    if (data->m_resolve_history.miss_center < 1)
        candidates.push_back(lastmove_body);

    candidates.push_back(lastmove_body + 58.f);
    candidates.push_back(lastmove_body - 58.f);
    candidates.push_back(lastmove_body + 180.f);
    candidates.push_back(lastmove_body + 90.f);
    candidates.push_back(lastmove_body - 90.f);

    // Away-relative candidates
    candidates.push_back(away + 180.f);
    candidates.push_back(away + 90.f);
    candidates.push_back(away - 90.f);

    // =========================================================
    // STEP 3: FOOT PREDICTION (anchored to lastmove body)
    // =========================================================
    if (data->m_records.size() >= 2) {
        const auto& a = *data->m_records[data->m_records.size() - 2];
        const auto& b = *data->m_records[data->m_records.size() - 1];

        float offset = pred_foot_delta(data->m_records);
        float scaled = std::clamp(offset * 2.5f, -180.f, 180.f);

        float foot = pred_foot_yaw_from_lastmove(lastmove_body, a, b, scaled);
        candidates.push_back(foot);
    }

    // =========================================================
    // STEP 4: MISS-BASED SUPPLEMENTS
    // =========================================================
    if (data->m_resolve_history.miss_side > 0)
        candidates.push_back(lastmove_body - 58.f);

    if (data->m_resolve_history.miss_invert > 0)
        candidates.push_back(lastmove_body + 58.f);

    if (data->m_resolve_history.miss_bruteforce > 1)
        candidates.push_back(lastmove_body + 180.f);

    // =========================================================
    // STEP 5: DEDUPLICATE
    // =========================================================
    for (auto& yaw : candidates)
        yaw = math::NormalizedAngle(yaw);

    std::sort(candidates.begin(), candidates.end());
    candidates.erase(
        std::unique(candidates.begin(), candidates.end(),
            [](float a, float b) {
                return fabsf(math::NormalizedAngle(a - b)) < 1.0f;
            }),
        candidates.end()
    );

    // Filter out directions we've confirmed don't work
    candidates.erase(
        std::remove_if(candidates.begin(), candidates.end(),
            [&](float yaw) {
                float delta = math::NormalizedAngle(yaw - lastmove_body);

                if (fabsf(delta) < 20.f && data->m_resolve_history.miss_center >= 2)
                    return true;

                if (delta > 0.f && data->m_resolve_history.miss_side >= 2)
                    return true;

                if (delta < 0.f && data->m_resolve_history.miss_invert >= 2)
                    return true;

                return false;
            }),
        candidates.end()
    );

    // Miss weight helper (uses lastmove_body as reference)
    auto GetMissWeight = [&](float yaw) -> float {
        float delta = math::NormalizedAngle(yaw - lastmove_body);
        float weight = 0.f;

        if (fabsf(delta) < 20.f)
            weight -= data->m_resolve_history.miss_center * 50.f;
        else if (delta > 0.f)
            weight -= data->m_resolve_history.miss_side * 50.f;
        else
            weight -= data->m_resolve_history.miss_invert * 50.f;

        return weight;
        };

    // =========================================================
    // STEP 6: SCORE AND SELECT
    // =========================================================
    float best_score = -FLT_MAX;

    for (float yaw : candidates) {
        float norm = math::NormalizedAngle(yaw);
        float score = ScoreYaw(data, record, norm, lastmove_body);
        score += GetMissWeight(norm);

        if (score > best_score) {
            best_score = score;
            final_yaw = norm;
        }
    }

    record->m_eye_angles.y = final_yaw;
    math::NormalizeAngle(record->m_eye_angles.y);
}

// ============================================================
// STAND NS (nospread bruteforce)
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
    case 7: record->m_eye_angles.y = away + 0.f;   break;
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
// POSE RESOLVER
// ============================================================

void Resolver::ResolvePoses(Player* player, LagRecord* record) {
    AimPlayer* data = &g_aimbot.m_players[player->index() - 1];

    if (record->m_mode == Modes::RESOLVE_AIR) {
        player->m_flPoseParameter()[2] = g_csgo.RandomInt(0, 4) * 0.25f;
        player->m_flPoseParameter()[11] = g_csgo.RandomInt(1, 3) * 0.25f;
    }
}

// ============================================================
// CONFIDENCE
// ============================================================

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

// ============================================================
// ON MISS — classify delta relative to lastmove body
// ============================================================

void Resolver::OnMiss(AimPlayer* data) {
    float shot = data->m_resolve_history.last_angle;
    float lastmove_body = GetLastMoveBody(data, FindLastRecord(data));

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
    if (stat)
        stat->misses++;

    for (auto& s : data->m_angle_stats) {
        if (s.misses > 0)
            s.misses--;
    }

    g_csgo.m_cvar->ConsoleColorPrintf(
        { 255, 200, 100, 255 },
        "[MISS] delta vs lastmove: %.1f -> %s\n",
        delta,
        type
    );
}

// ============================================================
// ON HIT
// ============================================================

void Resolver::OnHit(AimPlayer* data) {
    data->m_resolve_history.last_hit_angle = data->m_resolve_history.last_angle;

    data->m_resolve_history.miss_bruteforce = 0;
    data->m_resolve_history.miss_side = 0;
    data->m_resolve_history.miss_invert = 0;
    data->m_resolve_history.miss_center = 0;
    data->m_resolve_history.brute_index = 0;

    data->m_tried_angles.clear();

    float yaw = data->m_resolve_history.last_angle;
    auto* stat = GetAngleStat(data, yaw);
    if (stat)
        stat->hits++;
}

// ============================================================
// MAX DESYNC DELTA
// ============================================================

float Resolver::GetMaxDesyncDelta(CCSGOPlayerAnimState* state) {
    if (!state)
        return 58.f; // 2018 engine cap

    float yaw_max = state->m_fl_aim_yaw_max;
    float yaw_min = state->m_fl_aim_yaw_min;

    float delta = std::max(fabsf(yaw_max), fabsf(yaw_min));
    return std::clamp(delta, 20.f, 58.f); // 2018 hard cap is 58, not 120
}