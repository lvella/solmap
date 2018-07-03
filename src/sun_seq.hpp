#pragma once

extern "C" {
#include "sun_position.h"
}

class SunSequence
{
public:
	SunSequence(double latitude, double longitude, double elevation=0)
	{
		poy = create_pos_over_year(latitude, longitude, elevation);
	}

	~SunSequence()
	{
		destroy_pos_over_year(poy);
	}

	bool next(AngularPosition &val)
	{
		return next_pos_over_year(poy, &val);
	}

private:
	void *poy;
};

