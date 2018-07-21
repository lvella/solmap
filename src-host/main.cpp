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
#include "scene_loader.hpp"

template <typename F>
constexpr F to_deg(F rad)
{
	return rad * 180.0 / M_PI;
}

template <typename F>
constexpr F to_rad(F deg)
{
	return deg * M_PI / 180.0;
}

const Vec3 norm{0.0, 0.5*std::sqrt(2.0), -0.5*std::sqrt(2.0)};

void
calculate_yearly_incidence(
	real latitude, real longitude, real altitude,
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

static void
create_if_has_graphics(
	VkPhysicalDevice pd,
	const VkPhysicalDeviceProperties &pd_props,
	std::vector<ShadowProcessor>& procs,
	const Mesh &mesh)
{
	// Query queue capabilities:
	uint32_t num_qf;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &num_qf, nullptr);

	std::vector<VkQueueFamilyProperties> qfp(num_qf);
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &num_qf, qfp.data());

	// Select which queue families to use in the device:
	std::vector<VkDeviceQueueCreateInfo> used_qf;
	used_qf.reserve(num_qf);

	std::vector<float> priorities;
	for(uint32_t i = 0; i < num_qf; ++i) {
		// Queue family is not for compute, skip.
		if(!(qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
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
	}

	// Set pQueuePriorities:
	for(auto& qf: used_qf) {
		qf.pQueuePriorities = priorities.data();
	}

	// Create only if there is any usable queue family.
	if(used_qf.empty()) {
		return;
	}

	UVkDevice d{VkDeviceCreateInfo{
			VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
			nullptr,
			0,
			(uint32_t)used_qf.size(), used_qf.data(),
			0, nullptr,
			0, nullptr,
			nullptr
		}, pd
	};

	// Retrieve que requested queues from the newly created device:
	std::vector<std::pair<uint32_t, std::vector<VkQueue>>> qfs;
	qfs.reserve(num_qf);
	for(auto& qf: used_qf) {
		qfs.emplace_back();
		qfs.back().first = qf.queueFamilyIndex;
		auto& queues = qfs.back().second;

		for(uint32_t i = 0; i < qf.queueCount; ++i) {
			queues.emplace_back();
			vkGetDeviceQueue(d.get(), qf.queueFamilyIndex, i, &queues.back());
		}
	}

	procs.emplace_back(pd, pd_props, std::move(d), std::move(qfs), mesh);

	std::cout << " - " << procs.size() - 1 << ": "
		<< pd_props.deviceName << '\n';
}

UVkInstance initialize_vulkan()
{
	const char *layers[] = {
		"VK_LAYER_LUNARG_standard_validation"
	};
	UVkInstance vk{VkInstanceCreateInfo{
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
	};
	return vk;
}

static std::vector<ShadowProcessor>
create_procs_from_devices(VkInstance vk, const Mesh &mesh)
{
	std::vector<ShadowProcessor> processors;

	uint32_t dcount;
	chk_vk(vkEnumeratePhysicalDevices(vk, &dcount, nullptr));

	std::vector<VkPhysicalDevice> pds(dcount);
	chk_vk(vkEnumeratePhysicalDevices(vk, &dcount, pds.data()));

	std::cout << "Suitable Vulkan devices found:\n";
	for(auto &pd: pds) {
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties(pd, &props);
		create_if_has_graphics(pd, props, processors, mesh);

		// TODO: temporary
		break;
	}

	return processors;
}

int main(int argc, char *argv[])
{
	if(argc < 4) {
		std::cout << "Please provide latitude, longitude and 3D model file.\n";
		exit(1);
	}
	real lat = atof(argv[1]);
	real lon = atof(argv[2]);

	UVkInstance vk = initialize_vulkan();

	std::vector<ShadowProcessor> ps;
	size_t num_points;
	{
		auto scene_mesh = load_scene(argv[3]);
		num_points = scene_mesh.vertices.size();
		ps = create_procs_from_devices(vk.get(),
			std::move(scene_mesh));
	}

	calculate_yearly_incidence(lat, lon, 0, ps);

	// Get results:
	std::vector<double> result(num_points, 0.0f);
	Vec3 total{0.0, 0.0, 0.0};
	size_t count = 0;
	for(auto &p: ps) {
		total += p.get_sum();
		count += p.get_process_count();
		p.accumulate_result(result.data());
	}

	// Divide result by the total number of executions:
	double icount = 1.0 / count;
	for(double &r: result) {
		r *= icount;
	}
	ps[0].dump_vtk("incidence.vtk", result.data());

	std::cout << "\nTotal positions considered: " << count
		<< "\n\nWorkload distribution:\n";
	for(size_t i = 0; i < ps.size(); ++i) {
		const size_t lc =  ps[i].get_process_count();
		std::cout << " - Device " << i << ": " << lc
			<< '/' << count << " (" << lc * icount * 100.0
			<< "%)\n";
	}

	std::cout << "Total positions considered: " << count << '\n';

	Vec3 best_dir = glm::normalize(total);

	real best_alt = std::acos(best_dir.y);

	// Project total to the surface plane, to calculate azimuth:
	Vec3 plane_dir{total.x, 0.0, total.z};
	real best_az = std::acos(glm::dot(Vec3{0, 0, -1}, plane_dir)
		/ glm::length(plane_dir));

	std::cout << "\nBest placement for latitude "
		<< lat << " and longitude " << lon
		<< " is:\n"
		"Altitude: " << to_deg(best_alt) << "°\n"
		"Azimuth: " << to_deg(best_az) << "°\n\n"
		"At that position, the mean daytime power over a year is:\n"
		<< 1000.0 * glm::dot(total, best_dir) * icount << " watts/m²\n";
}
