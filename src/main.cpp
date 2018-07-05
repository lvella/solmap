#include <iostream>
#include <vector>
#include <thread>
#include <cmath>

#include "concurrentqueue.hpp"
#include "semaphore.hpp"
#include "sun_seq.hpp"
#include "vec3.hpp"

constexpr double to_deg(double rad)
{
	return rad * 180.0 / M_PI;
}

constexpr double to_rad(double deg)
{
	return deg * M_PI / 180.0;
}

const Vec3 norm{0.0, 0.5*sqrt(2.0), -0.5*sqrt(2.0)};

class ShadowProcessor
{
public:
	void process(const AngularPosition& p)
	{
		// Transforms into a unit vector pointing to the sun.
		// We convention Y as upwards and -z as N, thus
		// lookig from above, we have:
		//
		//        -z, N
		//           |
		//           |
		// -x, O ----+---- +x, E
		//           |
		//           |
		//        +z, S
                double c = cos(p.alt);

		Vec3 sun{
			sin(p.az) * c,
			sin(p.alt),
			-cos(p.az) * c
		};

		sum += sun;
		++count;
	}

	double test = 0;
	Vec3 sum;
	size_t count = 0;
};

void
calculate_yearly_incidence(
	double latitude, double longitude, double altitude,
	std::vector<ShadowProcessor> &processors
)
{
	// The generator of Sun's position:
	SunSequence ss{latitude, longitude, altitude};

	// Each generated position will be inserted into the queue
	// and signaled on the semaphore.
	moodycamel::ConcurrentQueue<AngularPosition> queue;
	Semaphore sem;

	// These jobs will wait on the semaphore and consume
	// the positions from the queue, simulating the result
	// and accumulating internally.
	// One job for each ShadowProcessor.
	std::vector<std::thread> jobs;
	jobs.reserve(processors.size());

	moodycamel::ProducerToken t(queue);

	// Jobs that will consume from the queue:
	for(auto &p: processors) {
		jobs.push_back(std::thread([&]() {
			AngularPosition pos;

			for(;;) {
				sem.wait();

				if(!queue.try_dequeue_from_producer(t, pos)) {
					break;
				}

				p.process(pos);
			}
		}));
	}

	// Produce the positions and notify the processors.
	AngularPosition pos;
	while(ss.next(pos)) {
		queue.enqueue(t, pos);
		sem.signal();
	}

	// Notify the processor without producing anything.
	sem.signal(jobs.size());

	for(auto &t: jobs) {
		t.join();
	}
}

int main(int argc, char *argv[])
{
	if(argc < 3) {
		std::cout << "Please provide latitude and longitude.\n";
		exit(1);
	}
	double lat = atof(argv[1]);
	double lon = atof(argv[2]);

	std::vector<ShadowProcessor> ps(7);
	calculate_yearly_incidence(lat, lon, 0, ps);

	Vec3 total;
	size_t count = 0;
	for(auto &p: ps) {
		total += p.sum;
		count += p.count;
	}

	Vec3 best_dir = total.normalized();

	double best_alt = acos(best_dir.y());

	// Project total to the surface plane:
	Vec3 plane_dir{total.x(), 0.0, total.z()};
	double best_az = acos(Vec3::dot(Vec3{0, 0, -1}, plane_dir)
		/ plane_dir.norm());

	std::cout << "Best placement for latitude "
		<< lat << " and longitude " << lon
		<< " is:\n"
		"Altitude: " << to_deg(best_alt) << "°\n"
		"Azimuth: " << to_deg(best_az) << "°\n\n"
		"At that position, the mean daytime power over a year is:\n"
		<< 1000.0 * Vec3::dot(total, best_dir) / count << " watts/m²\n";
}
