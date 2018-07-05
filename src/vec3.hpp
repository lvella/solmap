#pragma once

#include <cstdint>
#include <cmath>

struct Vec3
{
	Vec3(double x=0.0, double y=0.0, double z=0.0):
		d{x, y, z}
	{}

	Vec3 operator+(const Vec3& o) const {
		Vec3 ret{*this};
		ret += o;
		return ret;
	}

	Vec3& operator+=(const Vec3& o) {
		for(uint8_t i = 0; i < 3; ++i) {
			d[i] += o.d[i];
		}
		return *this;
	}

	Vec3 operator*(double s) const {
		Vec3 ret{*this};
		ret *= s;
		return ret;
	}

	Vec3& operator*=(double s) {
		for(uint8_t i = 0; i < 3; ++i) {
			d[i] *= s;
		}
		return *this;
	}

	Vec3 normalized() const {
		return *this * (1.0 / norm());
	}

	double norm() const {
		return sqrt(squared_norm());
	}

	double squared_norm() const {
		return dot(*this, *this);
	}

	double& x()
	{
		return d[0];
	}

	double& y()
	{
		return d[1];
	}

	double& z()
	{
		return d[2];
	}

	double x() const
	{
		return d[0];
	}

	double y() const
	{
		return d[1];
	}

	double z() const
	{
		return d[2];
	}

	static double dot(const Vec3& a, const Vec3& b)
	{
		double ret = 0.0;
		for(uint8_t i = 0; i < 3; ++i) {
			ret += a.d[i] * b.d[i];
		}
		return ret;
	}

	double d[3];
};
