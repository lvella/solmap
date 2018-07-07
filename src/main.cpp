#include <iostream>
#include <vector>
#include <thread>
#include <cmath>

#include <glm/geometric.hpp>

#include "vk_manager.hpp"
#include "concurrentqueue.hpp"
#include "semaphore.hpp"
#include "sun_seq.hpp"
#include "shadow_processor.hpp"

constexpr double to_deg(double rad)
{
	return rad * 180.0 / M_PI;
}

constexpr double to_rad(double deg)
{
	return deg * M_PI / 180.0;
}

const Vec3 norm{0.0, 0.5*sqrt(2.0), -0.5*sqrt(2.0)};

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

	// Notify the processors without producing anything.
	sem.signal(jobs.size());

	for(auto &t: jobs) {
		t.join();
	}
}

void create_if_compute(VkPhysicalDevice pd, std::vector<ShadowProcessor>& procs)
{
	// Query queue capabilities:
	uint32_t num_qf;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &num_qf, nullptr);

	std::vector<VkQueueFamilyProperties> qfp(num_qf);
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &num_qf, qfp.data());

	// Select which queue families to use in the device:
	std::vector<VkDeviceQueueCreateInfo> used_qf;
	used_qf.reserve(num_qf);

	uint32_t total_queue_count = 0;
	std::vector<float> priorities;
	for(uint32_t i = 0; i < num_qf; ++i) {
		// Queue family is not for compute, skip.
		if(!(qfp[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
			continue;
		}

		// All priorities are the same: adjust priority
		// vector to the biggest number of queues, overall:
		if(priorities.size() < qfp[i].queueCount) {
			priorities.resize(qfp[i].queueCount, 1.0);
		}

		// Set this family for use:
		used_qf.push_back({
			.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
			.pNext = nullptr,
			.flags = 0,
			.queueFamilyIndex = i,
			.queueCount = qfp[i].queueCount
			// We set pQueuePriorities, because the pointer might change.
		});

		total_queue_count += qfp[i].queueCount;
	}

	// Set pQueuePriorities:
	for(auto& qf: used_qf) {
		qf.pQueuePriorities = priorities.data();
	}

	// Create only if there is any usable queue family.
	if(used_qf.empty()) {
		return;
	}

	UVkDevice d = create_vk<vkCreateDevice, vkDestroyDevice>(VkDeviceCreateInfo{
			VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
			nullptr,
			0,
			(uint32_t)used_qf.size(), used_qf.data(),
			0, nullptr,
			0, nullptr,
			nullptr
		}, pd
	);

	// Retrieve que requested queues from the newly created device:
	std::vector<VkQueue> queues;
	queues.reserve(total_queue_count);
	for(auto& qf: used_qf) {
		for(uint32_t i = 0; i < qf.queueCount; ++i) {
			queues.emplace_back();
			vkGetDeviceQueue(d.get(), qf.queueFamilyIndex, i, &queues.back());
		}
	}

	procs.emplace_back(std::move(d), std::move(queues));
}

UVkInstance initialize_vulkan()
{
	const char *layers[] = {
		"VK_LAYER_LUNARG_standard_validation"
	};
	UVkInstance vk = create_vk<vkCreateInstance, vkDestroyInstance>(
		VkInstanceCreateInfo{
			VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
			nullptr,
			0,
			nullptr,
#ifdef NDEBUG
			0,
			nullptr,
#else
			1,
			layers,
#endif
			0,
			nullptr
		}
	);
	return vk;
}

std::vector<ShadowProcessor> create_procs_from_devices(VkInstance vk)
{
	std::vector<ShadowProcessor> processors;

	uint32_t dcount;
	chk_vk(vkEnumeratePhysicalDevices(vk, &dcount, nullptr));

	std::vector<VkPhysicalDevice> pds(dcount);
	chk_vk(vkEnumeratePhysicalDevices(vk, &dcount, pds.data()));

	for(auto &pd: pds) {
		VkPhysicalDeviceProperties props;

		vkGetPhysicalDeviceProperties(pd, &props);
		std::cout << props.deviceName << ' '
			<< props.deviceType << std::endl;

		create_if_compute(pd, processors);
	}

	return processors;
}

int main(int argc, char *argv[])
{
	if(argc < 3) {
		std::cout << "Please provide latitude and longitude.\n";
		exit(1);
	}
	double lat = atof(argv[1]);
	double lon = atof(argv[2]);

	UVkInstance vk = initialize_vulkan();

	std::vector<ShadowProcessor> ps =
		create_procs_from_devices(vk.get());

	calculate_yearly_incidence(lat, lon, 0, ps);

	Vec3 total;
	size_t count = 0;
	for(auto &p: ps) {
		total += p.get_sum();
		count += p.get_count();
	}

	Vec3 best_dir = glm::normalize(total);

	double best_alt = std::acos(best_dir.y);

	// Project total to the surface plane:
	Vec3 plane_dir{total.x, 0.0, total.z};
	double best_az = std::acos(glm::dot(Vec3{0, 0, -1}, plane_dir)
		/ glm::length(plane_dir));

	std::cout << "Best placement for latitude "
		<< lat << " and longitude " << lon
		<< " is:\n"
		"Altitude: " << to_deg(best_alt) << "°\n"
		"Azimuth: " << to_deg(best_az) << "°\n\n"
		"At that position, the mean daytime power over a year is:\n"
		<< 1000.0 * glm::dot(total, best_dir) / count << " watts/m²\n";
}
