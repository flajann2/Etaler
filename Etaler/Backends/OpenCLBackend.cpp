#include "OpenCLBackend.hpp"

#include <map>
#include <sstream>

using namespace et;

//Helper functions

static std::string readAll(const std::string& path)
{
	std::ifstream in(path);
	if(!in)
		throw EtError("Cannot open file " + path + ". " + strerror(errno));
	std::string str((std::istreambuf_iterator<char>(in)),
		std::istreambuf_iterator<char>());
	return str;
}

inline size_t selectWorkSize(size_t max, size_t mul_of, size_t size)
{
	auto round = [mul_of](auto v){return ((v/mul_of)*mul_of) + (v%mul_of == 0 ? 0 : mul_of);};
	return std::min((size_t)max, round(size));
}

inline std::string hash_string(const std::string& str)
{
	auto hash = std::hash<std::string>()(str);
	std::stringstream ss;
	ss << std::hex << hash;
	return ss.str();
}

OpenCLBackend::OpenCLBackend()
{
	std::vector<cl::Platform> platforms;
	cl_int err = cl::Platform::get(&platforms);
	if(err != CL_SUCCESS)
		throw EtError("Failed to get OpenCL platforms. Error: " + std::to_string(err));
	if(platforms.size() == 0)
		throw EtError("No OpenCL platform found.");
	auto& platform = platforms[0];

	std::vector<cl::Device> devices;
	platform.getDevices(CL_DEVICE_TYPE_ALL, &devices);
	if(devices.size() == 0)
		throw EtError("No OpenCL device found in platorm " + platform.getInfo<CL_PLATFORM_NAME>());
	auto& device = devices[0];
	if(device.getInfo<CL_DEVICE_COMPILER_AVAILABLE>() == CL_FALSE)
		throw EtError("Compiler for " + device.getInfo<CL_DEVICE_NAME>() + " is not avliable. (Devices like Altera/Xilinx FPGAs not supported"
			" in the OpenCL backend.)");

	platform_ = platform;
	device_ = device;
	context_ = cl::Context(device, nullptr, nullptr, nullptr, &err);
	if(err != CL_SUCCESS)
		throw EtError("Failed to create OpenCL context. Error " + std::to_string(err));

	//I trust these won't fail
	queue_ = cl::CommandQueue(context_);
	kernel_manager_ = KernelManager(device, context_);
	kernel_manager_.compileKernel("kernel void __etaler_dummy__(global int* p){p[get_global_id(0)] = 0;}", "__etaler_dummy__", "__etaler_dummy__");
}

std::shared_ptr<TensorImpl> OpenCLBackend::createTensor(const Shape& shape, DType dtype, const void* data)
{
	et_assert(dtype != DType::Unknown);
	size_t buf_size = shape.volume()*dtypeToSize(dtype);
	cl::Buffer buf = allocBuffer(buf_size);

	if(data != nullptr) {
		cl_int err;
		void* ocl_ptr = queue_.enqueueMapBuffer(buf, CL_TRUE, CL_MAP_WRITE, 0, buf_size, nullptr, nullptr, &err);
		if(err != CL_SUCCESS)
			throw EtError("OpenCL buffer map failed. Error: " + std::to_string(err) + ", map size " + std::to_string(buf_size));
		memcpy(ocl_ptr, data, buf_size);
		err = queue_.enqueueUnmapMemObject(buf, ocl_ptr, nullptr, nullptr);
		if(err != CL_SUCCESS)
			throw EtError("OpenCL buffer write failed. Error: " + std::to_string(err) + ", write size " + std::to_string(buf_size));
	}

	OpenCLTensor* ptr = new OpenCLTensor(shape, dtype, buf, shared_from_this());
	return std::shared_ptr<OpenCLTensor>(ptr, [this](TensorImpl* ptr){releaseTensor(ptr);});
}

void OpenCLBackend::releaseTensor(TensorImpl* pimpl)
{
	et_assert(dynamic_cast<OpenCLTensor*>(pimpl) != nullptr);
	delete pimpl;
}

void OpenCLBackend::copyToHost(const TensorImpl* pimpl, void* dest)
{
	const OpenCLTensor* t = dynamic_cast<const OpenCLTensor*>(pimpl);
	if(t == nullptr)
		throw EtError("Cannot copy to host memory: Tensor/Backend mismach");
	cl_int err = queue_.enqueueReadBuffer(t->buffer(), CL_TRUE, 0, t->size()*dtypeToSize(t->dtype()), dest);
	if(err != CL_SUCCESS)
		throw EtError("OpenCL buffer readback failed. Error: " + std::to_string(err));
}

std::string OpenCLBackend::deviceInfo() const
{
	std::map<int, std::string> cache_type;
	cache_type[CL_LOCAL] = "Local";
	cache_type[CL_GLOBAL] = "Global";
	cache_type[CL_NONE] = "None";
	std::string res;
	res += "Platform: " + platform_.getInfo<CL_PLATFORM_NAME>() + "\n";
	res += "Device name: " + device_.getInfo<CL_DEVICE_NAME>() + "\n";
	res += "Global memory size: " + std::to_string((float)device_.getInfo<CL_DEVICE_GLOBAL_MEM_SIZE>()/1024/1024) + " GB\n";
	res += "Max allocatable memory: " + std::to_string((float)device_.getInfo<CL_DEVICE_MAX_MEM_ALLOC_SIZE>()/1024/1024) + " GB\n";
	res += "Local memory size: " + std::to_string(device_.getInfo<CL_DEVICE_LOCAL_MEM_SIZE>()/1024) + " KB\n";
	res += "Prefered work group size: " + std::to_string(kernel_manager_.kernel("__etaler_dummy__").getWorkGroupInfo<CL_KERNEL_PREFERRED_WORK_GROUP_SIZE_MULTIPLE>(device_)) + "\n";
	return res;
}

cl::Kernel KernelManager::compileKernel(const std::string& src, const std::string& program_name, const std::string& kernel_name
	, bool force_override, const std::string& flags)
{
	compileKernel(src, program_name, std::vector<std::string>{kernel_name}, force_override, flags);
	assert(exists(program_name, kernel_name)!=false);
	return kernel(program_name, kernel_name);
}

void KernelManager::compileKernel(const std::string& src, const std::string& program_name, const std::vector<std::string>& kernel_names
	, bool force_override, const std::string& flags)
{
	compileKernel(std::vector<std::string>{src}, program_name, kernel_names, force_override, flags);
}

void KernelManager::compileKernel(const std::vector<std::string>& srcs, const std::string& program_name, const std::vector<std::string>& kernel_names
	, bool force_override, const std::string& flags)
{
	if(apps_.find(program_name) != apps_.end() && force_override == false)
		return;
	cl::Program::Sources sources;
	for(const auto& src : srcs)
		sources.push_back({src.c_str(), src.size()});

	cl::Program program(context_,sources);
	cl_int err = program.build({device_}, flags.c_str());
	if(err != CL_SUCCESS) {
		throw EtError("Error building OpenCL program: " + program_name
			+ ", Error:" + std::to_string(err) +"\n" + program.getBuildInfo<CL_PROGRAM_BUILD_LOG>(device_));
	}

	auto& app = apps_[program_name];
	for(const auto& name : kernel_names) {
		cl_int err;
		cl::Kernel k(program, name.c_str(), &err);
		if(err != CL_SUCCESS)
			throw EtError("Kernel " + name + " not found in program " + program_name);
		app.kernels[name] = k;
	}
}

void KernelManager::compileFromFile(const std::string& path, const std::string& program_name, const std::vector<std::string>& kernel_names
	, bool force_override, const std::string& flags)
{
	compileFromFile(std::vector<std::string>{path}, program_name, kernel_names, force_override, flags);
}

void KernelManager::compileFromFile(const std::vector<std::string>& paths, const std::string& program_name, const std::vector<std::string>& kernel_names
	, bool force_override, const std::string& flags)
{
	std::vector<std::string> sources;
	for(const auto& path : paths)
		sources.emplace_back(readAll(path));
	compileKernel(sources, program_name, kernel_names, force_override, flags);
}


void OpenCLBackend::overlapScore(const TensorImpl* x, const TensorImpl* connections,
		const TensorImpl* permeances, float connected_permeance, size_t active_threshold, TensorImpl* y, bool has_unconnected_synapse)
{
	et_assert(points_to<const OpenCLTensor>(x));
	et_assert(points_to<const OpenCLTensor>(connections));
	et_assert(points_to<const OpenCLTensor>(permeances));
	et_assert(points_to<OpenCLTensor>(y));

	et_assert(x->dtype() == DType::Bool);
	et_assert(connections->dtype() == DType::Int32);
	et_assert(permeances->dtype() == DType::Float);
	et_assert(y->dtype() == DType::Int32);
	et_assert(connections->shape() == permeances->shape());

	auto str = [](auto s){return std::to_string(s);};

	auto args = "-DINPUT_SIZE="+str(x->size())+" -DMAX_SYNAPSE_PER_CELL="+str(connections->shape().back())+" -DNO_UNUSED_SYNAPSE=" + str(!has_unconnected_synapse);
	auto hash = hash_string(args);
	auto program_name = "overlapScore"+hash;

	kernel_manager_.compileFromFile(kernel_root_+"overlapScore.cl", program_name, {"overlapScore"}, false, args);
	cl::Kernel k = kernel_manager_.kernel(program_name, "overlapScore");

	k.setArg(0, reinterpret_cast<const OpenCLTensor*>(x)->buffer());
	k.setArg(1, reinterpret_cast<const OpenCLTensor*>(connections)->buffer());
	k.setArg(2, reinterpret_cast<const OpenCLTensor*>(permeances)->buffer());
	k.setArg(3, reinterpret_cast<OpenCLTensor*>(y)->buffer());
	k.setArg(4, (float)connected_permeance);
	k.setArg(5, (int)active_threshold);
	k.setArg(6, (int)y->size());

	size_t local_size = 64;
	cl_int err = queue_.enqueueNDRangeKernel(k, cl::NullRange, cl::NDRange(selectWorkSize(8152, local_size, x->size())), cl::NDRange(local_size));

	if(err != CL_SUCCESS)
		throw EtError("OpenCL kernel execution failed. Code " + str(err));
}

void OpenCLBackend::globalInhibition(const TensorImpl* x, TensorImpl* y, float fraction)
{
	et_assert(points_to<OpenCLTensor>(x));
	et_assert(points_to<OpenCLTensor>(y));

	et_assert(x->dtype() == DType::Int32);
	et_assert(y->dtype() == DType::Bool);
	et_assert(x->shape() == y->shape());

	auto str = [](auto s){return std::to_string(s);};

	auto args = "-DINPUT_SIZE="+str(x->size())+" -DMAX_INPUT_VALUE="+str(2000);
	auto hash = hash_string(args);
	auto program_name = "overlapScore"+hash;

	cl::Kernel topKKernel, thresholdKernel;
	kernel_manager_.compileFromFile(kernel_root_+"globalInhibition.cl", program_name, {"fastTopK", "threshold"}, false, args);

	topKKernel = kernel_manager_.kernel(program_name, "fastTopK");
	thresholdKernel = kernel_manager_.kernel(program_name, "threshold");

	cl::Buffer threshold = allocBuffer(sizeof(int));

	topKKernel.setArg(0, reinterpret_cast<const OpenCLTensor*>(x)->buffer());
	topKKernel.setArg(1, threshold);
	topKKernel.setArg(2, (int)(x->size()*fraction));

	queue_.enqueueNDRangeKernel(topKKernel, cl::NullRange, cl::NDRange(256), cl::NDRange(256));

	thresholdKernel.setArg(0, reinterpret_cast<const OpenCLTensor*>(x)->buffer());
	thresholdKernel.setArg(1, reinterpret_cast<const OpenCLTensor*>(y)->buffer());
	thresholdKernel.setArg(2, threshold);
	queue_.enqueueNDRangeKernel(thresholdKernel, cl::NullRange, cl::NDRange(1024), cl::NDRange(32));
}

std::shared_ptr<TensorImpl> OpenCLBackend::cast(const TensorImpl* x, DType toType)
{
	et_assert(points_to<OpenCLTensor>(x));
	auto args = "-DInType="+to_ctype_string(x->dtype())+" -DOutType="+to_ctype_string(toType);
	auto hash = hash_string(args);
	auto program_name = "cast"+hash;
	kernel_manager_.compileFromFile(kernel_root_+"cast.cl", program_name, {"cast"}, false, args);
	cl::Kernel k = kernel_manager_.kernel(program_name, "cast");

	auto res = createTensor(x->shape(), toType);
	k.setArg(0, reinterpret_cast<const OpenCLTensor*>(x)->buffer());
	k.setArg(1, reinterpret_cast<OpenCLTensor*>(res.get())->buffer());
	k.setArg(2, (int)x->size());
	queue_.enqueueNDRangeKernel(k, cl::NullRange, cl::NDRange(1024), cl::NDRange(32));

	return res;
}

void OpenCLBackend::sync() const
{
	cl_int err = queue_.finish();
	if(err != CL_SUCCESS)
		throw EtError("Waiting for OpenCL queue failed. Error: " + std::to_string(err));
}

std::shared_ptr<TensorImpl> OpenCLBackend::copy(const TensorImpl* x)
{
	et_assert(points_to<OpenCLTensor>(x));
	size_t buf_size = x->size()*dtypeToSize(x->dtype());
	cl::Buffer buf = allocBuffer(buf_size);
	const cl::Buffer& src = reinterpret_cast<const OpenCLTensor*>(x)->buffer();
	cl_int err = queue_.enqueueCopyBuffer(src, buf, 0, 0, buf_size);
	if(err != CL_SUCCESS)
		throw EtError("Data copy enqueuing failed. Error " + std::to_string(err));

	OpenCLTensor* ptr = new OpenCLTensor(x->shape(), x->dtype(), buf, shared_from_this());
	return std::shared_ptr<OpenCLTensor>(ptr, [this](TensorImpl* ptr){releaseTensor(ptr);});
}

void OpenCLBackend::learnCorrilation(const TensorImpl* x, const TensorImpl* learn, const TensorImpl* connections,
	TensorImpl* permeances, float perm_inc, float perm_dec)
{
	et_assert(points_to<OpenCLTensor>(x));
	et_assert(points_to<OpenCLTensor>(connections));
	et_assert(points_to<OpenCLTensor>(permeances));
	et_assert(points_to<OpenCLTensor>(learn));

	et_assert(connections->shape() == permeances->shape());
	et_assert(x->shape() == learn->shape());
	et_assert(x->dtype() == DType::Bool);
	et_assert(learn->dtype() == DType::Bool);
	et_assert(connections->dtype() == DType::Int32);
	et_assert(permeances->dtype() == DType::Float);

	auto str = [](auto s){return std::to_string(s);};
	auto args = "-DINPUT_SIZE="+str(x->size())+" -DMAX_SYNAPSE_PER_CELL="+str(connections->shape().back())+" -DNO_UNUSED_SYNAPSE=true";
	auto hash = hash_string(args);
	auto program_name = "learnCorrilation"+hash;

	kernel_manager_.compileFromFile(kernel_root_+"learnCorrilation.cl", program_name, {"learnCorrilation"}, false, args);
	cl::Kernel k = kernel_manager_.kernel(program_name, "learnCorrilation");

	k.setArg(0, reinterpret_cast<const OpenCLTensor*>(x)->buffer());
	k.setArg(1, reinterpret_cast<const OpenCLTensor*>(learn)->buffer());
	k.setArg(2, reinterpret_cast<const OpenCLTensor*>(connections)->buffer());
	k.setArg(3, reinterpret_cast<OpenCLTensor*>(permeances)->buffer());
	k.setArg(4, (float)perm_inc);
	k.setArg(5, (float)perm_dec);

	size_t local_size = 128;

	cl_int err = queue_.enqueueNDRangeKernel(k, cl::NullRange, cl::NDRange(selectWorkSize(4096, local_size, x->size())), cl::NDRange(local_size));

	if(err != CL_SUCCESS)
		throw EtError("OpenCL kernel execution failed. Code " + str(err));
}

void OpenCLBackend::sortSynapse(TensorImpl* connections, TensorImpl* permeances)
{
	et_assert(connections->shape() == permeances->shape());
	et_assert(points_to<OpenCLTensor>(connections));
	et_assert(points_to<OpenCLTensor>(permeances));
	et_assert(connections->dtype() == DType::Int32);
	et_assert(permeances->dtype() == DType::Float);

	auto str = [](auto s){return std::to_string(s);};
	auto args = "-DMAX_SYNAPSE_PER_CELL="+str(connections->shape().back());
	auto hash = hash_string(args);
	auto program_name = "sortSynapse"+hash;
	kernel_manager_.compileFromFile(kernel_root_+"sort.cl", program_name, {"sortSynapse"}, false, args);
	cl::Kernel k = kernel_manager_.kernel(program_name, "sortSynapse");

	int num_cells = connections->size()/connections->shape().back();

	cl::Buffer aux_buffer1 = allocBuffer(connections->size()*sizeof(int));
	cl::Buffer aux_buffer2 = allocBuffer(permeances->size()*sizeof(float));

	k.setArg(0, reinterpret_cast<const OpenCLTensor*>(connections)->buffer());
	k.setArg(1, reinterpret_cast<const OpenCLTensor*>(permeances)->buffer());
	k.setArg(2, num_cells);
	k.setArg(3, aux_buffer1);
	k.setArg(4, aux_buffer2);
	size_t local_size = 128;

	cl_int err = queue_.enqueueNDRangeKernel(k, cl::NullRange, cl::NDRange(selectWorkSize(4096, local_size, num_cells)), cl::NDRange(local_size));
	if(err != CL_SUCCESS)
		throw EtError("OpenCL kernel execution failed. Code " + str(err));
}