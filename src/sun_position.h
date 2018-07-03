#pragma once

#include <stdbool.h>

typedef struct {
	double alt;
	double az;
} AngularPosition;

void *create_pos_over_year(
	double latitude, double longitude, double elevation
);

void destroy_pos_over_year(void *iter);

bool next_pos_over_year(void *iter, AngularPosition *ret);
