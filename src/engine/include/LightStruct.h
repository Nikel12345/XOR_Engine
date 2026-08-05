#pragma once
#include <glm/glm.hpp>

struct LightLayout {
	float x, y, z; // position						// 12 байт
	float w; // source radius (усечённый конус)		// 16 байта
	float dir_x, dir_y, dir_z;						// 28 байта
	float angle_tan; // tangent of the angle		// 32 байт
	float r, g, b; // color							// 44 байт
	float power;									// 48 байта
	int type;										// 52 байт
	int offset;										// 56 байт
	float max_range = 0;								// 60 байт
	int padding = 0;								// 64 байт
};
