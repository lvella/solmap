#include <iostream>
#include <vector>
#include <chrono>

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

int main()
{
	auto ss = SunSequence(-18, -48);

	std::vector<AngularPosition> v;

	AngularPosition p;

	auto start = std::chrono::system_clock::now();
	while(ss.next(p)) {
		v.push_back(p);
	}
	auto end = std::chrono::system_clock::now();

	for(const auto& e: v) {
		std::cout << e.az << ", " << e.alt << std::endl;
	}

	std::cout << "### duration: " <<
		std::chrono::duration_cast<std::chrono::nanoseconds>
                             (end-start).count() * 1e-9
	<< std::endl;
}
