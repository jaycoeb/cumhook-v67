#pragma once

// modelled after the original valve 'developer 1' debug console
// https://github.com/LestaD/SourceEngine2007/blob/master/se2007/engine/console.cpp

class NotifyText {
public:
 struct Segment {
		std::string m_text;
		Color       m_color;
	};

	std::string           m_text;
	Color				m_color;
	std::vector< Segment > m_segments;
	float		m_time;

public:
  __forceinline NotifyText( const std::string& text, Color color, float time ) : m_text{ text }, m_color{ color }, m_segments{}, m_time{ time } {}
	__forceinline NotifyText( std::vector< Segment > segments, float time ) : m_text{}, m_color{ colors::white }, m_segments{ std::move( segments ) }, m_time{ time } {}
};

class Notify {
private:
	std::vector< std::shared_ptr< NotifyText > > m_notify_text;

public:
	__forceinline Notify( ) : m_notify_text{} {}

	__forceinline void add( const std::string& text, Color color = colors::white, float time = 8.f, bool console = true ) {
		// modelled after 'CConPanel::AddToNotify'
		m_notify_text.push_back( std::make_shared< NotifyText >( text, color, time ) );

		if( console )
		    g_cl.print( text );
	}

	__forceinline void add_segments( std::vector< NotifyText::Segment > segments, float time = 8.f, bool console = true ) {
		m_notify_text.push_back( std::make_shared< NotifyText >( std::move( segments ), time ) );

		if( console ) {
			std::string out;
			for( const auto& seg : m_notify_text.back( )->m_segments )
				out += seg.m_text;

			g_cl.print( out );
		}
	}

	// modelled after 'CConPanel::DrawNotify' and 'CConPanel::ShouldDraw'
	void think( ) {
		int		x{ 8 }, y{ 5 }, size{ render::menu_shade.m_size.m_height + 1 };
		Color	color;
		float	left;

		// update lifetimes.
		for( size_t i{}; i < m_notify_text.size( ); ++i ) {
			auto notify = m_notify_text[ i ];

			notify->m_time -= g_csgo.m_globals->m_frametime;

			if( notify->m_time <= 0.f ) {
				m_notify_text.erase( m_notify_text.begin( ) + i );
				continue;
			}
		}

		// we have nothing to draw.
		if( m_notify_text.empty( ) )
			return;

		// iterate entries.
		for( size_t i{}; i < m_notify_text.size( ); ++i ) {
			auto notify = m_notify_text[ i ];

			left = notify->m_time;
			color = notify->m_color;

			if( left < .5f ) {
				float f = left;
				math::clamp( f, 0.f, .5f );

				f /= .5f;

				color.a( ) = ( int )( f * 255.f );

				if( i == 0 && f < 0.2f )
					y -= size * ( 1.f - f / 0.2f );
			}

			else
				color.a( ) = 255;

           if( !notify->m_segments.empty( ) ) {
				int x_off = x;
               for( const auto& seg : notify->m_segments ) {
					render::menu_shade.string( x_off, y, seg.m_color, seg.m_text );
					x_off += render::menu_shade.size( seg.m_text ).m_width;
				}
			}
			else {
				render::menu_shade.string( x, y, color, notify->m_text );
			}
			y += size;
		}
	}
};

extern Notify g_notify;