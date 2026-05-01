#pragma once

// Global flag: true while applying values from a config file.
// Used to suppress element callbacks to avoid re-entrancy/crashes.
extern bool g_config_loading;

class Config {
public:
	void init( );
	void LoadHotkeys( );
	void SaveHotkeys( );
	void load( const Form* form, const std::string& name );
	void save( const Form* form, const std::string& name );

private:
	bool m_init;
	std::string m_path;
};

extern Config g_config;