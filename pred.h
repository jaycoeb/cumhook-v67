#pragma once

class InputPrediction {
public:
	float m_curtime;
	float m_frametime;
	bool m_in_prediction;
	int m_tickbase;
	vec3_t m_velocity;
	bool m_first_time_predicted;

public:
	void update( );
	void run( );
	void restore( );
};

extern InputPrediction g_inputpred;