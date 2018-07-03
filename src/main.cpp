#include <iostream>
#include <vector>
#include <chrono>

#include "sun_seq.hpp"

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
