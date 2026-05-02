#include "includes.h"

HVH g_hvh{ };

int m_fake_flick_ticks = 0;
int m_fake_flick_cooldown = 0;
int m_desync_side = 1;
int m_hits_taken = 0;
int m_flip_threshold = 1;
bool m_flip = false;
bool m_should_flip = false;
float m_flip_time = 0.f;
int m_hit_count = 0;

// ============================================================
// LBY DELAY STATE
// Tracks a queued body update and defers it by a random tick
// count (2–8) so resolvers can't time the flick.
// ============================================================
struct LBYDelay_t {
    bool    pending = false;
    float   queued_yaw = 0.f;
    int     ticks_remain = 0;
    float   last_body = 0.f;
};

static LBYDelay_t s_lby_delay;

// Call once per frame before DoRealAntiAim / DoFakeAntiAim.
// Returns true when a delayed flick fires this tick.
static bool UpdateLBYDelay(float current_body, float& out_yaw) {
    // Detect a body update (LBY flick incoming).
    float delta = fabsf(math::NormalizedAngle(current_body - s_lby_delay.last_body));
    s_lby_delay.last_body = current_body;

    if (delta > 35.f && !s_lby_delay.pending) {
        s_lby_delay.pending = true;
        s_lby_delay.queued_yaw = current_body;
        // Random delay: 2–8 ticks
        s_lby_delay.ticks_remain = 2 + (rand() % 7);
    }

    if (s_lby_delay.pending) {
        s_lby_delay.ticks_remain--;

        if (s_lby_delay.ticks_remain <= 0) {
            s_lby_delay.pending = false;
            out_yaw = s_lby_delay.queued_yaw;
            return true; // fire now
        }
    }

    return false;
}

// ============================================================
// FREESTAND HEAD GUARD
// Returns true if applying 'yaw' would expose our head
// in the direction of the nearest enemy.
// ============================================================
static bool WouldExposeHead(float yaw) {
    Player* best = nullptr;
    float   best_fov = 180.f;

    for (int i = 1; i <= g_csgo.m_globals->m_max_clients; ++i) {
        Player* p = g_csgo.m_entlist->GetClientEntity<Player*>(i);
        if (!g_aimbot.IsValidTarget(p) || p->dormant())
            continue;

        float fov = math::GetFOV(g_cl.m_view_angles, g_cl.m_shoot_pos, p->WorldSpaceCenter());
        if (fov < best_fov) {
            best_fov = fov;
            best = p;
        }
    }

    if (!best)
        return false;

    // Angle from enemy to us.
    ang_t to_us;
    math::VectorAngles(g_cl.m_local->m_vecOrigin() - best->m_vecOrigin(), to_us);

    // If our fake yaw is within 45° of the enemy's aim-at-us angle,
    // the fake side is pointing toward them → head may be exposed.
    float diff = fabsf(math::NormalizedAngle(yaw - to_us.y));
    return diff < 45.f;
}

// ============================================================

void HVH::OnHit() {
    m_hit_count++;
    int trigger = (rand() % 2) + 1;

    if (m_hit_count >= trigger) {
        m_hit_count = 0;
        m_should_flip = true;
        m_flip_time = g_csgo.m_globals->m_curtime + g_csgo.RandomFloat(0.1f, 0.35f);
    }
}

void HVH::UpdateHotkeys(Stage_t stage) {
    if (stage != FRAME_RENDER_START || !g_cl.m_processing)
        return;

    auto current = -1;

    static bool prev_state;
    const auto state = current >= 0;
    if (prev_state != state) {
        if (state) {
            if (current == direction)
                direction = -1;
            else
                direction = current;
        }
        prev_state = state;
    }
}

void HVH::IdealPitch() {
    CCSGOPlayerAnimState* state = g_cl.m_local->m_PlayerAnimState();
    if (!state)
        return;

    g_cl.m_cmd->m_view_angles.x = -89.f;
}

void HVH::AntiAimPitch() {
    bool safe = false;

    switch (m_pitch) {
    case 1: g_cl.m_cmd->m_view_angles.x = 89.f;                              break;
    case 2: g_cl.m_cmd->m_view_angles.x = safe ? -89.f : -540.f;             break;
    case 3: g_cl.m_cmd->m_view_angles.x = safe ? 89.f : 540.f;               break;
    case 4: IdealPitch();                                                      break;
    case 5: g_cl.m_cmd->m_view_angles.x = g_csgo.RandomFloat(safe ? -89.f : -540.f, safe ? 89.f : 1080.f); break;
    default: break;
    }
}

void HVH::AutoDirection() {
    constexpr float STEP{ 4.f };
    constexpr float RANGE{ 36.f };
    float angel = 0.f;

    struct AutoTarget_t { float fov; Player* player; };
    AutoTarget_t target{ 180.f + 1.f, nullptr };

    for (int i{ 1 }; i <= g_csgo.m_globals->m_max_clients; ++i) {
        Player* player = g_csgo.m_entlist->GetClientEntity<Player*>(i);

        if (!g_aimbot.IsValidTarget(player) || player->dormant())
            continue;

        float fov = math::GetFOV(g_cl.m_view_angles, g_cl.m_shoot_pos, player->WorldSpaceCenter());
        if (fov < target.fov) {
            target.fov = fov;
            target.player = player;
        }
    }

    if (!target.player) {
        if (m_auto_last > 0.f && m_auto_time > 0.f && g_csgo.m_globals->m_curtime < (m_auto_last + m_auto_time))
            return;

        m_auto = math::NormalizedAngle(m_view - 180.f);
        m_auto_dist = -1.f;
        return;
    }

    std::vector<AdaptiveAngle> angles;
    angles.emplace_back(m_view - 180.f);
    angles.emplace_back(m_view + angel);
    angles.emplace_back(m_view - angel);

    vec3_t start = target.player->GetAbsOrigin() + vec3_t(0, 0, 56);
    bool valid{ false };

    for (auto it = angles.begin(); it != angles.end(); ++it) {
        vec3_t end{
            g_cl.m_shoot_pos.x + std::cos(math::deg_to_rad(it->m_yaw)) * RANGE,
            g_cl.m_shoot_pos.y + std::sin(math::deg_to_rad(it->m_yaw)) * RANGE,
            g_cl.m_shoot_pos.z
        };

        vec3_t dir = end - start;
        float len = dir.normalize();

        if (len <= 0.f)
            continue;

        for (float i{ 0.f }; i < len; i += STEP) {
            vec3_t point = start + (dir * i);
            int    contents = g_csgo.m_engine_trace->GetPointContents(point, MASK_SHOT);

            if (!(contents & MASK_SHOT))
                continue;

            float mult = 1.f;
            if (i > (len * 0.5f))  mult = 1.25f;
            if (i > (len * 0.75f)) mult = 1.25f;
            if (i > (len * 0.9f))  mult = 2.f;

            it->m_dist += (STEP * mult);
            valid = true;
        }
    }

    if (!valid) {
        m_auto = math::NormalizedAngle(m_view - 180.f);
        m_auto_dist = -1.f;
        return;
    }

    std::sort(angles.begin(), angles.end(),
        [](const AdaptiveAngle& a, const AdaptiveAngle& b) {
            return a.m_dist > b.m_dist;
        });

    AdaptiveAngle* best = &angles.front();

    if (best->m_dist != m_auto_dist) {
        m_auto = math::NormalizedAngle(best->m_yaw);
        m_auto_dist = best->m_dist;
        m_auto_last = g_csgo.m_globals->m_curtime;
    }
}

void HVH::GetAntiAimDirection() {
    // Edge AA — freestand head guard applied here.
    if (g_menu.main.antiaim.edge.get() && g_cl.m_local->m_vecVelocity().length_2d() < 320.f) {
        ang_t ang;
        if (DoEdgeAntiAim(g_cl.m_local, ang)) {
            // Guard: make sure edge AA doesn't accidentally point head at enemy.
            if (!WouldExposeHead(ang.y)) {
                m_direction = ang.y;
                return;
            }
            // Exposed — flip 180 to cover.
            m_direction = math::NormalizedAngle(ang.y + 180.f);
            return;
        }
    }

    bool lock = g_menu.main.antiaim.dir_lock.get();

    if ((lock && g_cl.m_speed > 0.1f) || !lock)
        m_view = g_cl.m_cmd->m_view_angles.y;

    if (true) {
        if (m_base_angle == 0) {
            m_view = 0.f;
        }
        else {
            float  best_fov{ std::numeric_limits<float>::max() };
            Player* best_target{ nullptr };

            for (int i{ 1 }; i <= g_csgo.m_globals->m_max_clients; ++i) {
                Player* target = g_csgo.m_entlist->GetClientEntity<Player*>(i);

                if (!g_aimbot.IsValidTarget(target) || target->dormant())
                    continue;

                if (m_base_angle == 2) {
                    float fov = math::GetFOV(g_cl.m_view_angles, g_cl.m_shoot_pos, target->WorldSpaceCenter());
                    if (fov < best_fov) {
                        best_fov = fov;
                        best_target = target;
                    }
                }
            }

            if (best_target) {
                ang_t angle;
                math::VectorAngles(best_target->m_vecOrigin() - g_cl.m_local->m_vecOrigin(), angle);
                m_view = angle.y;
            }
        }
    }

    if ((g_cl.m_ground && g_cl.m_speed <= 0.1f && false) || (g_cl.m_ground && g_cl.m_speed > 0.1f && false)) {
        AutoDirection();
        m_direction = m_auto;

        if (false) {
            switch (direction) {
            case 0: m_direction = m_view + 180.f; break;
            case 1: m_direction = m_view + 90.f;  break;
            case 2: m_direction = m_view - 90.f;  break;
            }
        }
    }
    else {
        m_direction = m_view + 180.f;

        if (false) {
            switch (direction) {
            case 0: m_direction = m_view + 180.f; break;
            case 1: m_direction = m_view + 90.f;  break;
            case 2: m_direction = m_view - 90.f;  break;
            }
        }
    }

    math::NormalizeAngle(m_direction);
}

bool HVH::DoEdgeAntiAim(Player* player, ang_t& out) {
    CGameTrace trace;
    static CTraceFilterSimple_game filter{ };

    if (player->m_MoveType() == MOVETYPE_LADDER)
        return false;

    filter.SetPassEntity(player);

    vec3_t mins = player->m_vecMins();
    vec3_t maxs = player->m_vecMaxs();
    mins.x -= 20.f; mins.y -= 20.f;
    maxs.x += 20.f; maxs.y += 20.f;

    vec3_t start = player->GetAbsOrigin();
    start.z += 56.f;

    g_csgo.m_engine_trace->TraceRay(Ray(start, start, mins, maxs), CONTENTS_SOLID, (ITraceFilter*)&filter, &trace);
    if (!trace.m_startsolid)
        return false;

    float  smallest = 1.f;
    vec3_t plane;

    for (float step{ }; step <= math::pi_2; step += (math::pi / 10.f)) {
        vec3_t end = start;
        end.x += std::cos(step) * 32.f;
        end.y += std::sin(step) * 32.f;

        g_csgo.m_engine_trace->TraceRay(Ray(start, end, { -1.f, -1.f, -8.f }, { 1.f, 1.f, 8.f }), CONTENTS_SOLID, (ITraceFilter*)&filter, &trace);

        if (trace.m_fraction < smallest) {
            plane = trace.m_plane.m_normal;
            smallest = trace.m_fraction;
        }
    }

    if (smallest == 1.f || plane.z >= 0.1f)
        return false;

    vec3_t inv = -plane;
    vec3_t dir = inv;
    dir.normalize();

    vec3_t point = start;
    point.x += (dir.x * 24.f);
    point.y += (dir.y * 24.f);

    if (g_csgo.m_engine_trace->GetPointContents(point, CONTENTS_SOLID) & CONTENTS_SOLID) {
        g_csgo.m_engine_trace->TraceRay(Ray(point + vec3_t{ 0.f, 0.f, 16.f }, point), CONTENTS_SOLID, (ITraceFilter*)&filter, &trace);

        if (trace.m_fraction < 1.f && !trace.m_startsolid && trace.m_plane.m_normal.z > 0.7f) {
            out.y = math::rad_to_deg(std::atan2(inv.y, inv.x));
            return true;
        }
    }

    mins = { (dir.x * -3.f) - 1.f, (dir.y * -3.f) - 1.f, -1.f };
    maxs = { (dir.x * 3.f) + 1.f,  (dir.y * 3.f) + 1.f,   1.f };

    vec3_t left = start;
    left.x = point.x - (inv.y * 48.f);
    left.y = point.y - (inv.x * -48.f);

    g_csgo.m_engine_trace->TraceRay(Ray(left, point, mins, maxs), CONTENTS_SOLID, (ITraceFilter*)&filter, &trace);
    float l = trace.m_startsolid ? 0.f : trace.m_fraction;

    vec3_t right = start;
    right.x = point.x + (inv.y * 48.f);
    right.y = point.y + (inv.x * -48.f);

    g_csgo.m_engine_trace->TraceRay(Ray(right, point, mins, maxs), CONTENTS_SOLID, (ITraceFilter*)&filter, &trace);
    float r = trace.m_startsolid ? 0.f : trace.m_fraction;

    if (l == 0.f && r == 0.f)
        return false;

    out.y = math::rad_to_deg(std::atan2(inv.y, inv.x));

    if (l == 0.f) { out.y += 90.f; return true; }
    if (r == 0.f) { out.y -= 90.f; return true; }

    return false;
}

void HVH::DoRealAntiAim() {
    if (m_yaw > 0) {
        g_cl.m_cmd->m_view_angles.y = m_direction;

        bool stand = g_menu.main.antiaim.body_fake_stand.get() > 0 && m_mode == AntiAimMode::STAND;
        bool air = g_menu.main.antiaim.body_fake_air.get() > 0 && m_mode == AntiAimMode::AIR;
        static int negative = false;

        // One tick before the LBY update.
        if (stand && !g_cl.m_lag &&
            g_csgo.m_globals->m_curtime >= (g_cl.m_body_pred - g_cl.m_anim_frame) &&
            g_csgo.m_globals->m_curtime < g_cl.m_body_pred) {
            if (g_menu.main.antiaim.body_fake_stand.get() == 4)
                g_cl.m_cmd->m_view_angles.y -= 90.f;
        }

        // =====================================================
        // LBY DELAY — intercept the body update tick and defer
        // it by a random 2–8 tick window.
        // =====================================================
        float delayed_yaw = 0.f;
        bool  fire_delayed = UpdateLBYDelay(g_cl.m_body, delayed_yaw);

        if (!g_cl.m_lag && g_csgo.m_globals->m_curtime >= g_cl.m_body_pred && (stand || air)) {

            // If a delayed flick is ready to fire this tick, use it.
            if (fire_delayed) {
                g_cl.m_cmd->m_view_angles.y = delayed_yaw;
                math::NormalizeAngle(g_cl.m_cmd->m_view_angles.y);
                return;
            }

            // Still delaying — suppress the natural flick; hold current yaw.
            if (s_lby_delay.pending) {
                // do nothing, yaw stays as m_direction.
            }
            else if (stand) {
                switch (g_menu.main.antiaim.body_fake_stand.get()) {
                case 1:
                    if (false && GetAsyncKeyState(VK_SPACE)) break;
                    g_cl.m_cmd->m_view_angles.y += 110.f;
                    break;
                case 2:
                    if (false && GetAsyncKeyState(VK_SPACE)) break;
                    g_cl.m_cmd->m_view_angles.y -= 110.f;
                    break;
                case 3:
                    if (false && GetAsyncKeyState(VK_SPACE)) break;
                    g_cl.m_cmd->m_view_angles.y += 180.f;
                    break;
                case 4:
                    if (false && GetAsyncKeyState(VK_SPACE)) break;
                    negative ? g_cl.m_cmd->m_view_angles.y += 110.f
                        : g_cl.m_cmd->m_view_angles.y -= 110.f;
                    negative = ~negative;
                    break;
                case 5:
                    if (false && GetAsyncKeyState(VK_SPACE)) break;
                    g_cl.m_cmd->m_view_angles.y = m_view + 180.f;
                    break;
                case 6:
                    if (false && GetAsyncKeyState(VK_SPACE)) break;
                    g_cl.m_cmd->m_view_angles.y += 0.f;
                    break;
                case 7:
                {
                    float desync = 120.f;
                    float yaw = m_direction;
                    int   side = m_desync_side;
                    if (m_flip) side *= -1;

                    if (*g_cl.m_packet) yaw += desync * side;
                    else                yaw -= desync * side;

                    static int jitter_tick = 0;
                    jitter_tick++;
                    if (jitter_tick > 3) {
                        jitter_tick = 0;
                        static float jitter = 0.f;
                        jitter = g_csgo.RandomFloat(-30.f, 30.f);
                        yaw += jitter;
                    }

                    if (m_flip) yaw += 45.f;

                    g_cl.m_cmd->m_view_angles.y = yaw;
                    break;
                }
                }
            }
            else if (air) {
                switch (g_menu.main.antiaim.body_fake_air.get()) {
                case 1: g_cl.m_cmd->m_view_angles.y += 90.f;  break;
                case 2: g_cl.m_cmd->m_view_angles.y -= 90.f;  break;
                case 3: g_cl.m_cmd->m_view_angles.y += 180.f; break;
                }
            }
        }
        else {
            // If a delayed flick fired outside the body_pred window, still honor it.
            if (fire_delayed) {
                g_cl.m_cmd->m_view_angles.y = delayed_yaw;
                math::NormalizeAngle(g_cl.m_cmd->m_view_angles.y);
                return;
            }

            switch (m_yaw) {
            case 1:
                g_cl.m_cmd->m_view_angles.y += m_jitter_range;
                break;
            case 2:
            {
                float range = m_jitter_range / 2.f;
                g_cl.m_cmd->m_view_angles.y += g_csgo.RandomFloat(-range, range);
                break;
            }
            case 3:
                g_cl.m_cmd->m_view_angles.y = (m_direction - m_rot_range / 2.f);
                g_cl.m_cmd->m_view_angles.y += std::fmod(g_csgo.m_globals->m_curtime * (m_rot_speed * 20.f), m_rot_range);
                break;
            case 4:
                if (g_csgo.m_globals->m_curtime >= m_next_random_update) {
                    m_random_angle = g_csgo.RandomFloat(-180.f, 180.f);
                    m_next_random_update = g_csgo.m_globals->m_curtime + m_rand_update;
                }
                g_cl.m_cmd->m_view_angles.y = m_random_angle;
                break;
            case 5:
                m_sway++;
                g_cl.m_cmd->m_view_angles.y += sin(m_sway) * 20;
                if (m_sway % 10 == 0)
                    g_cl.m_cmd->m_view_angles.y += ((rand() % 100) - (rand() % 90)) + m_sway;
                if ((sin(m_sway) * 30) > 105) {
                    g_cl.m_cmd->m_view_angles.y += m_sway;
                    m_sway = 0;
                }
                break;
            case 6:
                m_sway++;
                g_cl.m_cmd->m_view_angles.y += (sin(m_sway) * 30) + (cos(m_sway) * 30) + (tan(m_sway) * 30);
                if (g_cl.m_cmd->m_view_angles.y > 360)
                    m_sway = 1;
                break;
            default:
                break;
            }

            if (!*g_cl.m_packet) {
                bool moving = m_mode == AntiAimMode::WALK || m_mode == AntiAimMode::AIR;

                // Check if edge AA fired this frame by seeing if m_direction
                // was overridden — compare against view+180 (the default backwards).
                float expected_dir = math::NormalizedAngle(m_view + 180.f);
                float actual_dir = m_direction;
                bool  edge_active = fabsf(math::NormalizedAngle(actual_dir - expected_dir)) > 20.f;

                if (moving && !edge_active) {
                    static int  lm_break_tick = 0;
                    static bool lm_flip = false;
                    lm_break_tick++;

                    int interval = 2 + (lm_break_tick % 3);

                    if (lm_break_tick % interval == 0) {
                        lm_flip = !lm_flip;
                        g_cl.m_cmd->m_view_angles.y += lm_flip ? 58.f : -58.f;
                    }
                }
            }
        }
    }

    math::NormalizeAngle(g_cl.m_cmd->m_view_angles.y);
}

void HVH::DoFakeAntiAim() {
    *g_cl.m_packet = true;
    int rand26;

    switch (g_menu.main.antiaim.fake_yaw.get()) {
    case 1:
        g_cl.m_cmd->m_view_angles.y = m_direction + 180.f;
        g_cl.m_cmd->m_view_angles.y += g_csgo.RandomFloat(-90.f, 90.f);
        break;

    case 2:
        g_cl.m_cmd->m_view_angles.y = m_direction + 180.f;
        g_cl.m_cmd->m_view_angles.y += g_menu.main.antiaim.fake_relative.get();
        break;

    case 3:
    {
        float range = g_menu.main.antiaim.fake_jitter_range.get() / 2.f;
        g_cl.m_cmd->m_view_angles.y = m_direction + 180.f;
        g_cl.m_cmd->m_view_angles.y += g_csgo.RandomFloat(-range, range);
        break;
    }

    case 4:
        g_cl.m_cmd->m_view_angles.y = m_direction + 90.f + std::fmod(g_csgo.m_globals->m_curtime * 360.f, 180.f);
        break;

    case 5:
        g_cl.m_cmd->m_view_angles.y = g_csgo.RandomFloat(-180.f, 180.f);
        break;

    case 6:
        g_cl.m_cmd->m_view_angles.y = g_cl.m_view_angles.y + g_menu.main.antiaim.fake_relative.get();
        break;

    case 7:
        g_cl.m_cmd->m_view_angles.y = m_direction + 90.f + std::fmod(g_csgo.m_globals->m_curtime * 360.f, 360.f);
        break;

    case 8:
        g_cl.m_cmd->m_view_angles.y = g_cl.m_body + 180.f + g_menu.main.antiaim.fake_relative.get();
        break;

    case 9:
        g_cl.m_cmd->m_view_angles.y = g_cl.m_body + g_menu.main.antiaim.fake_relative.get();
        break;

    case 10:
        rand26 = rand() % 64;
        if (g_cl.m_tick < 10)                   g_cl.m_cmd->m_view_angles.y += 162.f;
        else if (m_sway == 5)                    g_cl.m_cmd->m_view_angles.y += 80.f;
        else if (m_sway == 20)                   g_cl.m_cmd->m_view_angles.y -= 87.f;
        else if (g_cl.m_tick == 10)              g_cl.m_cmd->m_view_angles.y += 177.f;
        else if (g_cl.m_tick == rand26)          g_cl.m_cmd->m_view_angles.y += 179.f;
        else if (g_cl.m_tick == 12)              g_cl.m_cmd->m_view_angles.y += 170.f;
        else if (g_cl.m_tick == 21)              g_cl.m_cmd->m_view_angles.y -= 179.f;
        else if (g_cl.m_tick == 37)              g_cl.m_cmd->m_view_angles.y += 179.f;
        else if (g_cl.m_tick == 63)              g_cl.m_cmd->m_view_angles.y += 177.f;
        else if (g_csgo.m_globals->m_curtime == 5)  g_cl.m_cmd->m_view_angles.y += 177.f;
        else if (g_csgo.m_globals->m_curtime == 12) g_cl.m_cmd->m_view_angles.y -= 177.f;
        else if (g_csgo.m_globals->m_curtime == 3)  g_cl.m_cmd->m_view_angles.y += 177.f;
        else if (g_csgo.m_globals->m_curtime == 9)  g_cl.m_cmd->m_view_angles.y -= 177.f;
        else if (m_sway % 5 == 0)               g_cl.m_cmd->m_view_angles.y += ((rand() % 180) - (rand() % -180)) + m_sway;
        else                                     g_cl.m_cmd->m_view_angles.y = pow(m_sway, asin(m_sway / 69));
        break;

    case 11:
    {
        float desync = 120.f;
        float yaw = m_direction;
        int   side = m_desync_side;
        if (m_flip) side *= -1;

        if (*g_cl.m_packet) yaw += desync * side;
        else                yaw -= desync * side;

        if (m_fake_flick_cooldown > 0)
            m_fake_flick_cooldown--;

        if (m_fake_flick_cooldown == 0 && (rand() % 30) == 0) {
            m_fake_flick_ticks = 1 + (rand() % 2);
            m_fake_flick_cooldown = 20 + (rand() % 20);
        }

        if (*g_cl.m_packet && m_fake_flick_ticks > 0) {
            yaw += (rand() % 2) ? 120.f : -120.f;
            m_fake_flick_ticks--;
        }
        else {
            static bool flip = false;
            flip = !flip;
            yaw += flip ? 30.f : -30.f;
        }

        if (m_flip) yaw += 45.f;

        g_cl.m_cmd->m_view_angles.y = yaw;
        break;
    }

    case 12:
        g_cl.m_cmd->m_view_angles.y = rand() % 360;
        if (g_cl.m_tick % 10 == 0) g_cl.m_cmd->m_view_angles.y += 90.f;
        else if (g_cl.m_tick % 15 == 0) g_cl.m_cmd->m_view_angles.y -= 90.f;
        else if (g_cl.m_tick % 20 == 0) g_cl.m_cmd->m_view_angles.y += 180.f;
        else if (g_cl.m_tick % 25 == 0) g_cl.m_cmd->m_view_angles.y -= 180.f;
        else if (g_cl.m_tick % 30 == 0) g_cl.m_cmd->m_view_angles.y += ((rand() % 180) - (rand() % -180));
        else                             g_cl.m_cmd->m_view_angles.y += ((rand() % 90) - (rand() % -90));
        g_cl.m_cmd->m_view_angles.y += g_menu.main.antiaim.fake_relative.get();
        g_cl.m_cmd->m_view_angles.y += sin(g_csgo.m_globals->m_curtime * 5.f) * 30.f;
        break;

    case 13:
    {
        chaos;
        float time = g_cl.m_tick * 0.1f;

        if ((int)(time) % 3 == 0) {
            chaos.mode = (chaos.mode + 1) % 4;
            chaos.targetAmplitude = 30.f + std::fmod(std::abs(std::sin(time * 1.7f)) * 120.f, 120.f);
            chaos.targetFrequency = 2.f + std::fmod(std::abs(std::cos(time * 0.9f)) * 10.f, 10.f);
        }

        float lerpSpeed = 0.05f;
        chaos.amplitude += (chaos.targetAmplitude - chaos.amplitude) * lerpSpeed;
        chaos.frequency += (chaos.targetFrequency - chaos.frequency) * lerpSpeed;

        float signal = 0.f;
        switch (chaos.mode) {
        case 0: signal = std::sin(time * chaos.frequency); break;
        case 1: signal = std::sin(time * chaos.frequency) * std::cos(time * chaos.frequency * 0.5f); break;
        case 2: signal = std::cos(time * chaos.frequency * 1.3f) + std::sin(time * chaos.frequency * 0.3f); break;
        case 3: signal = std::sin(time * chaos.frequency * 2.0f) * std::sin(time * chaos.frequency * 0.25f); break;
        }

        float fractal = 0.f, scale = 1.f;
        for (int i = 0; i < 4; ++i) { fractal += std::sin(time * chaos.frequency * scale) / scale; scale *= 2.f; }

        float noise = std::sin(time * 12.9898f) * std::cos(time * 78.233f);
        float burst = ((int)(time * 4.f) % 7 == 0) ? std::sin(time * 40.f) * 0.5f : 0.f;

        float offset = signal * chaos.amplitude + fractal * 20.f + noise * 15.f + burst * 50.f;
        float maxOffset = 120.f;
        offset = std::max(-maxOffset, std::min(maxOffset, offset));

        static float last = 0.f;
        float smooth = 0.15f;
        offset = last + (offset - last) * smooth;
        last = offset;

        g_cl.m_cmd->m_view_angles.y = m_direction + offset;
        break;
    }

    case 14:
    {
        const float MAX_YAW_DELTA = 179.f;
        const float SWAY_NORMALIZE = 69.f;
        const int   FLICK_DURATION = 1;
        const int   FLICK_INTERVAL = 24;

        float yaw = g_cl.m_cmd->m_view_angles.y;
        int   tick = g_cl.m_tick;
        float curtime = g_csgo.m_globals->m_curtime;

        if (m_flick_ticks > 0) {
            yaw += MAX_YAW_DELTA;
            --m_flick_ticks;
            g_cl.m_cmd->m_view_angles.y = yaw;
            break;
        }

        if (tick % FLICK_INTERVAL == 0)
            m_flick_ticks = FLICK_DURATION;

        int rand_tick = rand() % 64;

        if (tick < 10)         yaw += 162.f;
        else if (m_sway == 5)       yaw += 80.f;
        else if (m_sway == 20)      yaw -= 87.f;
        else if (tick == 10)        yaw += 177.f;
        else if (tick == 12)        yaw += 170.f;
        else if (tick == 21)        yaw -= MAX_YAW_DELTA;
        else if (tick == 37)        yaw += MAX_YAW_DELTA;
        else if (tick == 63)        yaw += 177.f;
        else if (tick == rand_tick) yaw += MAX_YAW_DELTA;
        else if (curtime == 3.f || curtime == 5.f)  yaw += 177.f;
        else if (curtime == 9.f || curtime == 12.f) yaw -= 177.f;
        else if (m_sway % 5 == 0) {
            float jitter = rand() % 361 - 180.f;
            yaw += jitter + m_sway;
        }
        else {
            float t = std::clamp(static_cast<float>(m_sway) / SWAY_NORMALIZE, -1.f, 1.f);
            yaw = std::pow(static_cast<float>(m_sway), std::asin(t));
        }

        g_cl.m_cmd->m_view_angles.y = yaw;
        break;
    }

    default:
        break;
    }

    math::NormalizeAngle(g_cl.m_cmd->m_view_angles.y);
}

void HVH::AntiAim() {
    bool attack = g_cl.m_cmd->m_buttons & IN_ATTACK;
    bool attack2 = g_cl.m_cmd->m_buttons & IN_ATTACK2;

    if (false) return;

    if (g_cl.m_weapon && g_cl.m_weapon_fire) {
        bool knife = g_cl.m_weapon_type == WEAPONTYPE_KNIFE && g_cl.m_weapon_id != ZEUS;
        bool revolver = g_cl.m_weapon_id == REVOLVER;

        if (attack || (attack2 && (knife || revolver)))
            return;
    }

    if (g_csgo.m_gamerules->m_bFreezePeriod() || (g_cl.m_flags & FL_FROZEN) || (g_cl.m_cmd->m_buttons & IN_USE))
        return;

    if (g_cl.m_weapon_type == WEAPONTYPE_GRENADE
        && (!g_cl.m_weapon->m_bPinPulled() || attack || attack2)
        && g_cl.m_weapon->m_fThrowTime() > 0.f
        && g_cl.m_weapon->m_fThrowTime() <= g_csgo.m_globals->m_curtime)
        return;

    // Queued flip.
    if (m_should_flip && g_csgo.m_globals->m_curtime >= m_flip_time) {
        m_flip = !m_flip;
        m_should_flip = false;
    }

    m_mode = AntiAimMode::STAND;
    if ((g_cl.m_buttons & IN_JUMP) || !(g_cl.m_flags & FL_ONGROUND)) m_mode = AntiAimMode::AIR;
    else if (g_cl.m_speed > 0.1f)                                      m_mode = AntiAimMode::WALK;

    if (m_mode == AntiAimMode::STAND) {
        m_pitch = g_menu.main.antiaim.pitch_stand.get();
        m_yaw = g_menu.main.antiaim.yaw_stand.get();
        m_jitter_range = g_menu.main.antiaim.jitter_range_stand.get();
        m_rot_range = g_menu.main.antiaim.rot_range_stand.get();
        m_rot_speed = g_menu.main.antiaim.rot_speed_stand.get();
        m_rand_update = g_menu.main.antiaim.rand_update_stand.get();
        m_dir = g_menu.main.antiaim.dir_stand.get();
        m_dir_custom = g_menu.main.antiaim.dir_custom_stand.get();
        m_base_angle = g_menu.main.antiaim.base_angle_stand.get();
        m_auto_time = 5.f;
    }
    else if (m_mode == AntiAimMode::WALK) {
        m_pitch = g_menu.main.antiaim.pitch_walk.get();
        m_yaw = g_menu.main.antiaim.yaw_walk.get();
        m_jitter_range = g_menu.main.antiaim.jitter_range_walk.get();
        m_rot_range = g_menu.main.antiaim.rot_range_walk.get();
        m_rot_speed = g_menu.main.antiaim.rot_speed_walk.get();
        m_rand_update = g_menu.main.antiaim.rand_update_walk.get();
        m_dir = g_menu.main.antiaim.dir_walk.get();
        m_dir_custom = g_menu.main.antiaim.dir_custom_walk.get();
        m_base_angle = g_menu.main.antiaim.base_angle_walk.get();
        m_auto_time = 5.f;
    }
    else if (m_mode == AntiAimMode::AIR) {
        m_pitch = g_menu.main.antiaim.pitch_air.get();
        m_yaw = g_menu.main.antiaim.yaw_air.get();
        m_jitter_range = g_menu.main.antiaim.jitter_range_air.get();
        m_rot_range = g_menu.main.antiaim.rot_range_air.get();
        m_rot_speed = g_menu.main.antiaim.rot_speed_air.get();
        m_rand_update = g_menu.main.antiaim.rand_update_air.get();
        m_dir = g_menu.main.antiaim.dir_air.get();
        m_dir_custom = g_menu.main.antiaim.dir_custom_air.get();
        m_base_angle = g_menu.main.antiaim.base_angle_air.get();
        m_auto_time = 5.f;
    }

    AntiAimPitch();

    if (m_yaw > 0)
        GetAntiAimDirection();
    else if (g_menu.main.antiaim.fake_yaw.get() > 0)
        m_direction = g_cl.m_cmd->m_view_angles.y;

    if (g_menu.main.antiaim.fake_yaw.get()) {
        if (*g_cl.m_packet && g_cl.m_old_packet)
            *g_cl.m_packet = false;

        if (!*g_cl.m_packet || !*g_cl.m_final_packet)
            DoRealAntiAim();
        else
            DoFakeAntiAim();
    }
    else {
        DoRealAntiAim();
    }
}

void HVH::SendPacket() {
    if (!*g_cl.m_final_packet)
        *g_cl.m_packet = false;

    if (g_menu.main.antiaim.lag_enable.get() && !g_csgo.m_gamerules->m_bFreezePeriod() && !(g_cl.m_flags & FL_FROZEN)) {
        int variance = std::clamp((int)g_menu.main.antiaim.lag_limit.get(), 1, 100);
        int limit = std::min((int)g_menu.main.antiaim.lag_limit.get(), g_cl.m_max_lag);
        if (g_cl.m_weapon_id == REVOLVER && !GetAsyncKeyState(VK_SPACE))
            limit = std::min(6, g_cl.m_max_lag);

        bool active{ };

        vec3_t cur = g_cl.m_local->m_vecOrigin();
        vec3_t prev = g_cl.m_net_pos.empty() ? g_cl.m_local->m_vecOrigin() : g_cl.m_net_pos.front().m_pos;
        float  delta = (cur - prev).length_sqr();

        const auto& lag_active = g_menu.main.antiaim.lag_active.GetActiveIndices();
        bool is_active_0 = std::find(lag_active.begin(), lag_active.end(), 0) != lag_active.end();
        bool is_active_1 = std::find(lag_active.begin(), lag_active.end(), 1) != lag_active.end();
        bool is_active_2 = std::find(lag_active.begin(), lag_active.end(), 2) != lag_active.end();
        bool is_active_3 = std::find(lag_active.begin(), lag_active.end(), 3) != lag_active.end();
        bool is_active_4 = std::find(lag_active.begin(), lag_active.end(), 4) != lag_active.end();
        bool is_active_5 = std::find(lag_active.begin(), lag_active.end(), 5) != lag_active.end();

        if (is_active_4 && delta < 0.1f && g_cl.m_speed < 0.1f && (!(g_cl.m_buttons & IN_JUMP) && (g_cl.m_flags & FL_ONGROUND))) active = true;
        else if (is_active_0 && delta > 0.1f && g_cl.m_speed > 0.1f)                                                                   active = true;
        else if (is_active_1 && ((g_cl.m_buttons & IN_JUMP) || !(g_cl.m_flags & FL_ONGROUND)))                                         active = true;
        else if (is_active_2 && g_cl.m_local->m_bDucking())                                                                             active = true;
        else if (is_active_5 && g_cl.m_speed > 278.f)                                                                                  active = true;

        if (active) {
            int mode = g_menu.main.antiaim.lag_mode.get();

            if (mode == 1) *g_cl.m_packet = false;
            else if (mode == 2 && delta <= 4096.f) *g_cl.m_packet = false;
            else if (mode == 0) {
                if (m_step_switch) { if (delta <= 4096.f) *g_cl.m_packet = false; }
                else *g_cl.m_packet = false;
            }
            else if (mode == 3) {
                if (g_cl.m_cmd->m_command_number % variance >= limit) limit = 1;
                *g_cl.m_packet = false;
            }

            if (g_cl.m_lag >= limit)
                *g_cl.m_packet = true;
        }
    }

    if (g_menu.main.antiaim.lag_land.get()) {
        if (g_cl.m_local->m_PlayerAnimState())
            if (false && (g_cl.m_flags & FL_ONGROUND))
                *g_cl.m_packet = true;
    }

    if (false) {
        vec3_t start = g_cl.m_local->m_vecOrigin(), end = start, vel = g_cl.m_local->m_vecVelocity();
        CTraceFilterWorldOnly filter;
        CGameTrace trace;

        vel.z -= (g_csgo.sv_gravity->GetFloat() * g_csgo.m_globals->m_interval);
        end += (vel * g_csgo.m_globals->m_interval);
        end.z -= 2.f;

        g_csgo.m_engine_trace->TraceRay(Ray(start, end), MASK_SOLID, &filter, &trace);

        if (trace.m_fraction != 1.f && trace.m_plane.m_normal.z > 0.7f && !(g_cl.m_flags & FL_ONGROUND))
            *g_cl.m_packet = true;
    }

    if (GetAsyncKeyState(VK_SPACE) && false)
        *g_cl.m_packet = false;

    if (g_cl.m_old_shot && !false)
        *g_cl.m_packet = true;

    if (false && false)
        *g_cl.m_packet = true;

    bool lm_break_active =
        (m_mode == AntiAimMode::WALK || m_mode == AntiAimMode::AIR) &&
        g_menu.main.antiaim.lag_enable.get() &&
        g_cl.m_lag < g_cl.m_max_lag;

    if (lm_break_active && (rand() % 4) == 0)
        *g_cl.m_packet = false;

    if (g_cl.m_lag >= g_cl.m_max_lag) {
        *g_cl.m_packet = true;
        g_cl.m_weapon_fire = false;
    }
}