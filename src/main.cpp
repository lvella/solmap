#include <iostream>
#include <vector>
#include <thread>
#include <cmath>

#include "concurrentqueue.hpp"
#include "semaphore.hpp"
#include "sun_seq.hpp"

constexpr double to_deg(double rad)
{
	return rad * 180.0 / M_PI;
}

constexpr double to_rad(double deg)
{
	return deg * M_PI / 180.0;
}

class ShadowProcessor
{
public:
	void process(const AngularPosition& p)
	{
		std::cout << to_deg(p.alt) << ' ' << to_deg(p.az) << std::endl;

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
                sum[1] += sin(p.alt);
                double c = cos(p.alt);
                sum[0] += sin(p.az) * c;
                sum[2] += -cos(p.az) * c;

		++count;
	}

	double sum[3] = {0};
	size_t count;
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

int main()
{
	std::vector<ShadowProcessor> ps(7);
	calculate_yearly_incidence(-18.9132819, -48.2584852, 863, ps);

	double total[3] = {0.0};
	size_t count = 0;
	for(auto &p: ps) {
		total[0] += p.sum[0];
		total[1] += p.sum[1];
		total[2] += p.sum[2];

		count += p.count;
	}

	printf("%g %g %g, %zu\n",
		total[0], total[1], total[2], count);
}
