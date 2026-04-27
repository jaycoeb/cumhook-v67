#pragma once

class AdaptiveAngle {
public:
	float m_yaw;
	float m_dist;

public:
	// ctor.
	__forceinline AdaptiveAngle(float yaw, float penalty = 0.f) {
		// set yaw.
		m_yaw = math::NormalizedAngle(yaw);

		// init distance.
		m_dist = 0.f;

		// remove penalty.
		m_dist -= penalty;
	}
};

enum AntiAimMode : size_t {
	STAND = 0,
	WALK,
	AIR,
};

class HVH {
public:
	size_t m_mode;
	int    m_pitch;
	int    m_yaw;
	float  m_jitter_range;
	float  m_rot_range;
	float  m_rot_speed;
	float  m_rand_update;
	int    m_dir;
	float  m_dir_custom;
	size_t m_base_angle;
	float  m_auto_time;

	bool   m_step_switch;
	int    m_random_lag;
	float  m_next_random_update;
	float  m_random_angle;
	float  m_direction;
	float  m_auto;
	float  m_auto_dist;
	float  m_auto_last;
	float  m_view;
	int    m_sway = 0;
	int	   m_flick_ticks = 0;

	//chatgpt shit
	struct ChaosState {
		float phase = 0.f;
		float amplitude = 60.f;
		float frequency = 5.f;
		float targetAmplitude = 60.f;
		float targetFrequency = 5.f;
		float blend = 0.f;
		int mode = 0;
	};

	ChaosState chaos;

	float time = 0;
	float lerpSpeed = 0;
	float signal = 0.f;
	float fractal = 0.f;
	float scale = 0.f;
	float noise = 0.f;
	float burst = 0.f;
	float offset = 0.f;
	float maxOffset = 120.f;
	float last = 0.f;
	float smooth = 0.15f;

	bool m_flip = false;
	int  m_hit_count = 0;
	int  m_missed_shots = 0; // optional if you use miss-based flip too

	float desync = 0.f;

	int side;

	//claude shit
	float MAX_YAW_DELTA = 179.f;
	float SWAY_NORMALIZE = 69.f;
	int   FLICK_DURATION = 1;
	float yaw = 0.f;
	int  tick = 0;
	float curtime = 0;
	int FLICK_INTERVAL = 24;
	int rand_tick = 0;
	float jitter = 0.f;
	float t = 0.f;

	bool   m_left, m_right, m_back;
	int direction = -1;

public:
	void UpdateHotkeys(Stage_t stage);
	void IdealPitch();
	void AntiAimPitch();
	void AutoDirection();
	void GetAntiAimDirection();
	bool DoEdgeAntiAim(Player* player, ang_t& out);
	void DoRealAntiAim();
	void DoFakeAntiAim();
	void AntiAim();
	void SendPacket();
	void OnHit();
};

extern HVH g_hvh;