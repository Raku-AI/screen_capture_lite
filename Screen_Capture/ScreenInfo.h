#pragma once
#include <vector>

namespace SL {
	namespace Screen_Capture {
		struct ScreenInfo {
			int Width = 0;//width in pixels of the screen
			int Height = 0;//Height in pixels of the screen
			int Depth = 0;//Depth in pixels of the screen, i.e. 32 bit
			char Device[32];//name of the screen
			int Offsetx = 0;//distance in pixels from the MOST left screen. This can be negative because the primary monitor starts at 0, but this screen could be layed out to the left of the primary, in which case the offset is negative
			int Offsety = 0;//distance in pixels from the TOP MOST screen
			int Index = 0;//Index of the screen from LEFT to right of the physical monitors
		};
		//monitors are returned pre-sorted left to right
		std::vector<ScreenInfo> GetMoitors();
		void Reorder(std::vector<SL::Screen_Capture::ScreenInfo>& screens);

	}
}