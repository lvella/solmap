#pragma once

#include <stdbool.h>

typedef struct {
	double az;
	double alt;
} AngularPosition;

typedef struct {
	AngularPosition pos;
	double coefficient;
	double direct_power;
	double indirect_power;
} InstantaneousData;

void *create_pos_over_year(
	double latitude, double longitude, double elevation, double max_dt
);

void destroy_pos_over_year(void *iter);

bool next_pos_over_year(void *iter, InstantaneousData *ret);
