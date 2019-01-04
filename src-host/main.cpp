#include <iostream>
#include <fstream>
#include <vector>
#include <thread>
#include <future>
#include <cmath>
#include <regex>
#include <getopt.h>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/geometric.hpp>
#include <glm/gtx/quaternion.hpp>

#include "vk_manager.hpp"
#include "concurrentqueue.hpp"
#include "semaphore.hpp"
#include "sun_seq.hpp"
#include "shadow_processor.hpp"
#include "mesh_tools.hpp"

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

Vec3 to_vec(AngularPosition pos,
	const Vec3& unit_north, const Vec3& unit_up, const Vec3& unit_east)
{
	return glm::rotate(glm::angleAxis(float(-pos.az), unit_up) *
		glm::angleAxis(float(pos.alt), unit_east), unit_north);
}

static std::vector<Vec3>
calculate_yearly_incidence(real latitude, real longitude, real altitude,
	const Vec3& unit_north, const Vec3& unit_up, const Vec3& unit_east,
	std::vector<std::unique_ptr<ShadowProcessor>> &processors
)
{
	std::vector<Vec3> direct_incidence;
	direct_incidence.reserve(60000);

	// The generator of Sun's position:
	SunSequence ss{latitude, longitude, altitude};

	// Each generated position will be inserted into the queue
	// and signaled on the semaphore.
	moodycamel::ConcurrentQueue<InstantaneousData> queue;
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
			InstantaneousData val;

			for(;;) {
				sem.wait();

				if(!queue.try_dequeue_from_producer(t, val)) {
					break;
				}

				// Transforms the angular position into a unit
				// vector pointing to the sun.
				const Vec3 suns_direction = to_vec(val.pos,
					unit_north, unit_up, unit_east);
				p->process(suns_direction, val);

				// Store the calculated solar data for later reuse.
				direct_incidence.push_back(
					float(val.coefficient * val.direct_power)
					* suns_direction
				);
			}
		}));
	}

	// Produce the positions and notify the processors.
	InstantaneousData val;
	while(ss.next(val)) {
		queue.enqueue(t, val);
		sem.signal();
	}

	// Notify the processors without producing anything.
	sem.signal(jobs.size());

	for(auto &t: jobs) {
		t.join();
	}

	direct_incidence.shrink_to_fit();
	return direct_incidence;
}

struct NoComputeQueueFamily: public std::exception {};

static std::unique_ptr<ShadowProcessor>
create_if_has_graphics(
	VkPhysicalDevice pd,
	const Mesh &shadow_mesh, const std::vector<VertexData>& test_set)
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
		// Queue family is not for graphics, skip.
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
		throw NoComputeQueueFamily{};
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

		// It seems there is no performance benefit of using
		// more than one queue, so just create one.
		// TODO: Maybe remove support for more than one queue
		// in the same family?
		queues.emplace_back();
		vkGetDeviceQueue(d.get(), qf.queueFamilyIndex, 0, &queues.back());
	}

	VkPhysicalDeviceProperties pd_props;
	vkGetPhysicalDeviceProperties(pd, &pd_props);

	auto ret = std::make_unique<ShadowProcessor>(
		pd, pd_props, std::move(d), std::move(qfs),
		shadow_mesh, test_set
	);

	return ret;
}

static std::vector<std::unique_ptr<ShadowProcessor>>
create_procs_from_devices(VkInstance vk,
	const Mesh &shadow_mesh, const std::vector<VertexData>& test_set)
{
	// Get the number of Vulkan devices in the system:
	uint32_t dcount;
	chk_vk(vkEnumeratePhysicalDevices(vk, &dcount, nullptr));

	// Get the list of VkPhysicalDevice
	std::vector<VkPhysicalDevice> pds(dcount);
	chk_vk(vkEnumeratePhysicalDevices(vk, &dcount, pds.data()));

	// Launch device setup tasks, possibly in parallel.
	std::vector<std::future<std::unique_ptr<ShadowProcessor>>> create_work;
	create_work.reserve(dcount);
	for(auto &pd: pds) {
		create_work.push_back(std::async(
			create_if_has_graphics, pd, shadow_mesh, test_set)
		);
		break;
	}

	std::vector<std::unique_ptr<ShadowProcessor>> processors;
	processors.reserve(dcount);
	std::cout << "Suitable Vulkan devices found:\n";
	for(auto &f: create_work) {
		try{
			processors.push_back(f.get());
			std::cout << " - "<< processors.back()->get_name()
				<< '\n';
		} catch(const std::exception &e) {}
	}
	if(processors.empty()) {
		std::cout << " None! Exiting due to lack of "
			"suitable Vulkan devices!\n";
		exit(1);
	}
	std::cout.flush();

	return processors;
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

void dump_vtk(const char* fname, const Mesh& mesh, real scale, double *result)
{
	std::ofstream fd(fname);

	fd << "# vtk DataFile Version 3.0\n"
		"Daylight solar incidence\n"
		"ASCII\n"
		"DATASET POLYDATA\n"
		"POINTS " << mesh.vertices.size() << " float\n";

	for(auto& p: mesh.vertices) {
		Vec3 pos = scale * p.position;
		fd << pos.x << ' ' << pos.y << ' ' << pos.z << '\n';
	}

	uint32_t face_count = mesh.indices.size() / 3;
	fd << "POLYGONS " << face_count << ' ' << face_count * 4 << '\n';
	{
		auto ptr = mesh.indices.begin();
		for(uint32_t i = 0; i < face_count; ++i) {
			fd << '3';
			for(uint8_t j = 0; j < 3; ++j) {
				fd << ' ' << *ptr++;
			}
			fd << '\n';
		}
	}

	fd << "POINT_DATA " << mesh.vertices.size() << "\n"
		"SCALARS incidence float 1\n"
		"LOOKUP_TABLE default\n";

	for(uint32_t i = 0; i < mesh.vertices.size(); ++i) {
		fd << result[i] << '\n';
	}
}

void usage(const char *cmd)
{
	std::cout << "Usage:\n"
		"    " << cmd << " [options] latitude longitude 3d-model\n"
		"\n"
		"Option:\n"
		"    -q --rotation-quaternion=<w>:<x>:<y>:<z>\n"
		"\tRotation quaternion applied to the 3-D model (default: no rotation).\n"
		"\n"
		"    -s --scale=<scalar>\n"
		"\tScale applied to the 3-D model (default: 1.0).\n"
		"\n"
		"    -f --fine-pass-filter=<cutoff>\n"
		"\tRemove triangles larger than cutoff from 3-D model.\n"
		"\tCutoff must be between 0 and 1, where 0 is the smallest\n"
		"\tmesh element and 1 is the biggest.\n"
		"\n"
		"Parameters:\n"
		"    latitude\n"
		"\tLatitde, given as degrees in decimal notation,\n"
		"\tnegative for south (e.g. -18.9118465).\n"
		"\n"
		"    longitude\n"
		"\tLongitude, given as degress in decimal notation,\n"
		"\tnegative for west (e.g. -48.2560091).\n"
		"\n"
		"    3d-model\n"
		"\t3-D model where to compute the insolation.\n"
		"\tAssumes a right-hand coordinate system.\n"
		"\tExpected alignment after transformations:\n"
		"\t+y is up; -z is north; +x is east.\n";
	exit(1);
}

static real parse_real(const char* opt, const char* cmd)
{
	char *endptr;
	real val = strtod(opt, &endptr);

	if(endptr == opt) {
		std::cout << "Invalid argument number \"" << opt << "\"." << std::endl;
		usage(cmd);
	}
	return val;
}

static Quat parse_quat(const char* opt, const char* cmd)
{
	std::regex parser{"^(.+):(.+):(.+):(.+)$"};
	std::cmatch match;

	if(!std::regex_match(opt, match, parser)) {
		usage(cmd);
	}

	Quat ret;
	ret.w = parse_real(match[1].str().c_str(), cmd);
	for(uint8_t i = 0; i < 3; ++i) {
		ret[i] = parse_real(match[2+i].str().c_str(), cmd);
	};

	return glm::normalize(ret);
}

static void parse_args(int argc, char *argv[], Quat& rotation, real& scale,
	real& lat, real& lon, std::string& mesh_name, real &filter_cutoff)
{
	const static struct option long_options[] =
	{
		{"rotation-quaternion", required_argument, nullptr, 'q'},
		{"scale",               required_argument, nullptr, 's'},
		{"fine-pass-filter",	required_argument, nullptr, 'f'},
		{nullptr, 0, nullptr, 0}
	};

	rotation = Quat(1.0, 0.0, 0.0, 0.0);
	scale = 1.0;
	filter_cutoff = 1.0;

	opterr = 0;
	for(;;) {
		int opt = getopt_long (argc, argv, "q:s:f:",
			long_options, nullptr);

		if(opt == -1) {
			break;
		}

		switch(opt) {
		case 'q':
			rotation = parse_quat(optarg, argv[0]);
			break;
		case 's':
			scale = parse_real(optarg, argv[0]);
			break;
		case 'f':
			filter_cutoff = parse_real(optarg, argv[0]);
			break;
		default:
			goto out;
		}
	}
	out:

	if(argc - optind < 3)
	{
		std::cout << "Error: Missing arguments." << std::endl;
		usage(argv[0]);
	}

	if(filter_cutoff < 0.0 || filter_cutoff > 1.0) {
		std::cout << "Error: Fine pass filter factor must be between 0 and 1." << std::endl;
		usage(argv[0]);
	}

	lat = parse_real(argv[optind], argv[0]);
	lon = parse_real(argv[optind+1], argv[0]);
	mesh_name = argv[optind+2];
}

// Golden section method, straight from Wikipedia page:
template<class F, class T>
auto maximize(const F& func, T lo, T hi, size_t iters)
{
	// Inverse golden ratio
	static const double phi = 2.0 / (1.0 + std::sqrt(5.0));

	T a = lo;
	auto af = func(a);

	T b = hi;
	auto bf = func(b);

	T c = b - (b - a) * phi;
	auto cf = func(c);

	T d = a + (b - a) * phi;
	auto df = func(d);

	for (size_t i = 0; i < iters; ++i) {
        	if(cf > df) {
			b = d;
			bf = df;

			d = c;
			df = cf;

	    		c = b - (b - a) * phi;
			cf = func(c);
		} else {
			a = c;
			af = cf;

			c = d;
			cf = df;

			d = a + (b - a) * phi;
			df = func(d);
		}
	}

	return cf > df ? std::make_pair(c, cf) : std::make_pair(d, df);
}

int main(int argc, char *argv[])
{
	std::ios_base::sync_with_stdio(false);

	real lat, lon;
	std::string mesh_name;
	Quat rotation;
	real scale;
	real filter_cutoff;

	parse_args(argc, argv, rotation, scale, lat, lon, mesh_name, filter_cutoff);

	UVkInstance vk = initialize_vulkan();

	std::vector<std::unique_ptr<ShadowProcessor>> ps;
	Mesh test_mesh = load_scene(mesh_name, rotation, scale, filter_cutoff);
	{
		auto shadow_mesh = test_mesh;
		//refine(test_mesh, 0.05);

		ps = create_procs_from_devices(vk.get(),
			shadow_mesh, test_mesh.vertices);
	}

	// TODO: take as command line input:
	const Vec3 unit_north{0, 0, -1};
	const Vec3 unit_up{0, 1, 0};
	const Vec3 unit_east{1, 0, 0};
	auto solar_data = calculate_yearly_incidence(lat, lon, 0,
		unit_north, unit_up, unit_east, ps);

	// Get results:
	std::vector<double> result(test_mesh.vertices.size(), 0.0f);
	Vec3 dir_total{0.0, 0.0, 0.0};
	double dif_total = 0.0;
	double suntime = 0.0;

	size_t count = 0;
	for(auto &p: ps) {
		dir_total += p->get_directional_sum();
		dif_total += p->get_diffuse_sum();
		suntime += p->get_time_sum();

		count += p->get_process_count();
		p->accumulate_result(result.data());
	}
	const float icount = 1.0f / count;

	// Convert from j/m² to kWh/m²
	const double j2kwh = 1.0 / 3600.0 / 1000.0;
	for(double &r: result) {
		r *= j2kwh;
	}
	dump_vtk("incidence.vtk", test_mesh, scale, result.data());

	std::cout << "Workload distribution:\n";
	for(size_t i = 0; i < ps.size(); ++i) {
		const size_t lc =  ps[i]->get_process_count();
		std::cout << " - Device " << i << ": " << lc
			<< '/' << count << " (" << lc * icount * 100.0f
			<< "%)\n";
	}

	// Find the best placement angle with a maximization method:
	auto energy_calc = [&](double alt) {
		AngularPosition pos {.az=0.0, .alt=M_PI*0.5 - alt};
		const Vec3 best = to_vec(pos, unit_north, unit_up, unit_east);

		double energy_at_best = dif_total;
		for(auto& sun: solar_data) {
			energy_at_best += std::max(0.0f, dot(sun, best));
		}

		return energy_at_best;
	};

	auto best_alt = maximize(energy_calc, -M_PI * 0.5, M_PI * 0.5, 20);
	double best_az = 0.0;
	if(best_alt.first < 0.0) {
		best_az = 180.0;
		best_alt.first = -best_alt.first;
	}

	std::cout << "\nReport:\n"
		" - Total daytime over year: " << suntime / 3600.0 << " hours\n"
		" - Best placement for latitude "
		<< lat << " and longitude " << lon
		<< " is:\n"
		"    - Altitude: " << to_deg(best_alt.first) << "°\n"
		"    - Azimuth: " << best_az << "°\n"
		" - At this orientation, the total incident energy over a year is: "
		<< best_alt.second * j2kwh << " kWh/m²"
		<< std::endl;
}
