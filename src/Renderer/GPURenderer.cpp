/*
 * Copyright 2024 Tom Clabault. GNU GPL3 license.
 * GNU GPL3 license copy: https://www.gnu.org/licenses/gpl-3.0.txt
 */

#include "Renderer/GPURenderer.h"
#include "Threads/ThreadFunctions.h"
#include "Threads/ThreadManager.h"
#include "UI/ApplicationSettings.h"

#include <Orochi/OrochiUtils.h>

const std::string GPURenderer::PATH_TRACING_KERNEL = "PathTracerKernel";
const std::vector<std::string> GPURenderer::COMMON_ADDITIONAL_KERNEL_INCLUDE_DIRS = { KERNEL_COMPILER_ADDITIONAL_INCLUDE, DEVICE_INCLUDES_DIRECTORY, OROCHI_INCLUDES_DIRECTORY, "./"};

GPURenderer::GPURenderer(std::shared_ptr<HIPRTOrochiCtx> hiprt_oro_ctx)
{
	// Creating buffers
	m_framebuffer = std::make_shared<OpenGLInteropBuffer<ColorRGB32F>>();
	m_denoised_framebuffer = std::make_shared<OpenGLInteropBuffer<ColorRGB32F>>();
	m_normals_AOV_buffer = std::make_shared<OpenGLInteropBuffer<float3>>();
	m_albedo_AOV_buffer = std::make_shared<OpenGLInteropBuffer<ColorRGB32F>>();
	m_pixels_sample_count_buffer = std::make_shared<OpenGLInteropBuffer<int>>();
	m_hiprt_orochi_ctx = hiprt_oro_ctx;	

	// Adding hardware acceleration by default if supported
	if (device_supports_hardware_acceleration() == HardwareAccelerationSupport::SUPPORTED)
		m_path_trace_kernel.get_compiler_options().set_macro("__USE_HWI__", 1);
	else
		m_path_trace_kernel.get_compiler_options().remove_macro("__USE_HWI__");

	// Compiling the kernels
	m_path_trace_kernel.set_kernel_file_path(DEVICE_KERNELS_DIRECTORY "/PathTracerKernel.h");
	m_path_trace_kernel.set_kernel_function_name(GPURenderer::PATH_TRACING_KERNEL);
	m_path_trace_kernel.get_compiler_options().set_additional_include_directories(GPURenderer::COMMON_ADDITIONAL_KERNEL_INCLUDE_DIRS);
	ThreadManager::start_thread(ThreadManager::COMPILE_KERNEL_THREAD_KEY, ThreadFunctions::compile_kernel, std::ref(m_path_trace_kernel), std::ref(m_hiprt_orochi_ctx->hiprt_ctx));

	OROCHI_CHECK_ERROR(oroStreamCreate(&m_main_stream));

	// Buffer that keeps track of whether at least one ray is still alive or not
	unsigned char true_data = 1;
	m_still_one_ray_active_buffer.resize(1);
	m_still_one_ray_active_buffer.upload_data(&true_data);
	m_pixels_converged_count_buffer.resize(1);

	OROCHI_CHECK_ERROR(oroEventCreate(&m_frame_start_event));
	OROCHI_CHECK_ERROR(oroEventCreate(&m_frame_stop_event));
}

void GPURenderer::update()
{
	internal_update_clear_device_status_buffers();
	internal_update_adaptive_sampling_buffers();
}

void GPURenderer::copy_status_buffers()
{
	OROCHI_CHECK_ERROR(oroMemcpy(&m_status_buffers_values.one_ray_active, m_still_one_ray_active_buffer.get_device_pointer(), sizeof(unsigned char), oroMemcpyDeviceToHost));
	OROCHI_CHECK_ERROR(oroMemcpy(&m_status_buffers_values.pixel_converged_count, m_pixels_converged_count_buffer.get_device_pointer(), sizeof(unsigned int), oroMemcpyDeviceToHost));
}

void GPURenderer::internal_update_clear_device_status_buffers()
{
	unsigned char false_data = false;
	unsigned int zero_data = 0;
	// Uploading false to reset the flag
	m_still_one_ray_active_buffer.upload_data(&false_data);
	// Resetting the counter of pixels converged to 0
	m_pixels_converged_count_buffer.upload_data(&zero_data);
}

void GPURenderer::internal_clear_m_status_buffers()
{
	m_status_buffers_values.one_ray_active = true;
	m_status_buffers_values.pixel_converged_count = 0;
}

void GPURenderer::internal_update_adaptive_sampling_buffers()
{
	bool buffers_needed = m_render_settings.has_access_to_adaptive_sampling_buffers();

	if (buffers_needed)
	{
		bool pixels_squared_luminance_needs_resize = m_pixels_squared_luminance_buffer.get_element_count() == 0;
		bool pixels_sample_count_needs_resize = m_pixels_sample_count_buffer->get_element_count() == 0;

		if (pixels_squared_luminance_needs_resize || pixels_sample_count_needs_resize)
			// If one of the two buffers is going to be resized, synchronizing because we don't want
			// to resize the buffers if we're currently rendering a frame
			synchronize_kernel();

		if (pixels_squared_luminance_needs_resize)
			// Only allocating if it isn't already
			m_pixels_squared_luminance_buffer.resize(m_render_width * m_render_height);

		if (pixels_sample_count_needs_resize)
			// Only allocating if it isn't already
			m_pixels_sample_count_buffer->resize(m_render_width * m_render_height);
	}
	else
	{
		if (m_pixels_squared_luminance_buffer.get_element_count() > 0 || m_pixels_sample_count_buffer->get_element_count() > 0)
			// If one of the buffers isn't freed already, we're going to free it. In this case, we need to synchronize to avoid
			// freeing a buffer that the renderer is actively using in the frame it is rendering right now
			synchronize_kernel();

		m_pixels_squared_luminance_buffer.free();
		m_pixels_sample_count_buffer->free();
	}
}

void GPURenderer::render()
{
	// Making sure kernels are compiled
	ThreadManager::join_threads(ThreadManager::COMPILE_KERNEL_THREAD_KEY);

	int tile_size_x = 8;
	int tile_size_y = 8;

	hiprtInt2 nb_groups;
	nb_groups.x = std::ceil(m_render_width / (float)tile_size_x);
	nb_groups.y = std::ceil(m_render_height / (float)tile_size_y);

	hiprtInt2 resolution = make_hiprtInt2(m_render_width, m_render_height);

	HIPRTCamera hiprt_cam = m_camera.to_hiprt();
	HIPRTRenderData render_data = get_render_data();
	void* launch_args[] = { &render_data, &resolution, &hiprt_cam};

	OROCHI_CHECK_ERROR(oroEventRecord(m_frame_start_event, m_main_stream));
	m_path_trace_kernel.launch_timed_asynchronous(tile_size_x, tile_size_y, resolution.x, resolution.y, launch_args, m_main_stream);
	OROCHI_CHECK_ERROR(oroEventRecord(m_frame_stop_event, m_main_stream));

	HIPKernel::ComputeElapsedTimeCallbackData* elapsed_time_data = new HIPKernel::ComputeElapsedTimeCallbackData;
	elapsed_time_data->start = m_frame_start_event;
	elapsed_time_data->end = m_frame_stop_event;
	elapsed_time_data->elapsed_time_out = &m_last_frame_time;

	OROCHI_CHECK_ERROR(oroLaunchHostFunc(m_main_stream, HIPKernel::compute_elapsed_time_callback, elapsed_time_data));

	m_was_last_frame_low_resolution = m_render_settings.render_low_resolution;
}

void GPURenderer::synchronize_kernel()
{
	OROCHI_CHECK_ERROR(oroStreamSynchronize(m_main_stream));
}

bool GPURenderer::frame_render_done()
{
	return oroStreamQuery(m_main_stream) == oroSuccess;
}

bool GPURenderer::was_last_frame_low_resolution()
{
	return m_was_last_frame_low_resolution;
}

void GPURenderer::resize(int new_width, int new_height)
{
	m_render_width = new_width;
	m_render_height = new_height;

	unmap_buffers();

	m_framebuffer->resize(new_width * new_height);
	m_denoised_framebuffer->resize(new_width * new_height);
	m_normals_AOV_buffer->resize(new_width * new_height);
	m_albedo_AOV_buffer->resize(new_width * new_height);

	if (m_render_settings.has_access_to_adaptive_sampling_buffers())
	{
		m_pixels_sample_count_buffer->resize(new_width * new_height);
		m_pixels_squared_luminance_buffer.resize(new_width * new_height);
	}

	// Recomputing the perspective projection matrix since the aspect ratio
	// may have changed
	float new_aspect = (float)new_width / new_height;
	m_camera.projection_matrix = glm::transpose(glm::perspective(m_camera.vertical_fov, new_aspect, m_camera.near_plane, m_camera.far_plane));
}

void GPURenderer::unmap_buffers()
{
	m_framebuffer->unmap();
	m_normals_AOV_buffer->unmap();
	m_albedo_AOV_buffer->unmap();
	// TODO some unmapping here could be done only if necessary (pixel sample count for example doesn't need to be unmapped unless we're displaying the adaptive sampling map)
	m_pixels_sample_count_buffer->unmap();
}

std::shared_ptr<OpenGLInteropBuffer<ColorRGB32F>> GPURenderer::get_color_framebuffer()
{
	return m_framebuffer;
}

std::shared_ptr<OpenGLInteropBuffer<ColorRGB32F>> GPURenderer::get_denoised_framebuffer()
{
	return m_denoised_framebuffer;
}

std::shared_ptr<OpenGLInteropBuffer<float3>> GPURenderer::get_denoiser_normals_AOV_buffer()
{
	return m_normals_AOV_buffer;
}

std::shared_ptr<OpenGLInteropBuffer<ColorRGB32F>> GPURenderer::get_denoiser_albedo_AOV_buffer()
{
	return m_albedo_AOV_buffer;
}

std::shared_ptr<OpenGLInteropBuffer<int>>& GPURenderer::get_pixels_sample_count_buffer()
{
	return m_pixels_sample_count_buffer;
}

const StatusBuffersValues& GPURenderer::get_status_buffer_values() const
{
	return m_status_buffers_values;
}

HIPRTRenderSettings& GPURenderer::get_render_settings()
{
	return m_render_settings;
}

WorldSettings& GPURenderer::get_world_settings()
{
	return m_world_settings;
}

oroDeviceProp GPURenderer::get_device_properties()
{
	return m_device_properties;
}

std::string getDeviceName(oroCtx m_ctxt, oroDevice m_device)
{
	oroDeviceProp prop;
	OROCHI_CHECK_ERROR(oroCtxSetCurrent(m_ctxt));
	OROCHI_CHECK_ERROR(oroGetDeviceProperties(&prop, m_device));
	return std::string(prop.name);
}

std::string getGcnArchName(oroCtx m_ctxt, oroDevice m_device)
{
	oroDeviceProp prop;
	OROCHI_CHECK_ERROR(oroCtxSetCurrent(m_ctxt));
	OROCHI_CHECK_ERROR(oroGetDeviceProperties(&prop, m_device));
	return std::string(prop.gcnArchName);
}

uint32_t getGcnArchNumber(oroCtx m_ctxt, oroDevice m_device)
{
	oroDeviceProp prop;
	OROCHI_CHECK_ERROR(oroCtxSetCurrent(m_ctxt));
	OROCHI_CHECK_ERROR(oroGetDeviceProperties(&prop, m_device));
	return prop.gcnArch;
}

bool enableHwi(oroCtx m_ctxt, oroDevice m_device)
{
	std::string	   deviceName = getDeviceName(m_ctxt, m_device);
	const uint32_t archNumber = getGcnArchNumber(m_ctxt, m_device);
	return (archNumber >= 1030 && deviceName.find("NVIDIA") == std::string::npos);
}

HardwareAccelerationSupport GPURenderer::device_supports_hardware_acceleration()
{
	bool enabled = reinterpret_cast<hiprt::Context*>(m_hiprt_orochi_ctx->hiprt_ctx)->enableHwi();
	if (enabled)
		return HardwareAccelerationSupport::SUPPORTED;
	else
	{
		if (std::string(m_device_properties.name).find("NVIDIA") != std::string::npos)
		{
			// Not supported on NVIDIA
			return HardwareAccelerationSupport::NVIDIA_UNSUPPORTED;
		}
		else
		{
			// Not NVIDIA but hardware acceleration not supported, assuming too old AMD
			return HardwareAccelerationSupport::AMD_UNSUPPORTED;
		}
	}
}

float GPURenderer::get_last_frame_time()
{
	return m_last_frame_time;
}

void GPURenderer::reset_last_frame_time()
{
	m_last_frame_time = 0.0f;
}

void GPURenderer::reset()
{
	m_render_settings.frame_number = 0;
	m_render_settings.sample_number = 0;
	m_render_settings.samples_per_frame = 1;

	reset_last_frame_time();
	internal_clear_m_status_buffers();
}

HIPRTRenderData GPURenderer::get_render_data()
{
	HIPRTRenderData render_data;

	render_data.geom = m_hiprt_scene.geometry.m_geometry;

	render_data.buffers.pixels = m_framebuffer->map_no_error();
	render_data.buffers.triangles_indices = reinterpret_cast<int*>(m_hiprt_scene.geometry.m_mesh.triangleIndices);
	render_data.buffers.vertices_positions = reinterpret_cast<float3*>(m_hiprt_scene.geometry.m_mesh.vertices);
	render_data.buffers.has_vertex_normals = reinterpret_cast<unsigned char*>(m_hiprt_scene.has_vertex_normals.get_device_pointer());
	render_data.buffers.vertex_normals = reinterpret_cast<float3*>(m_hiprt_scene.vertex_normals.get_device_pointer());
	render_data.buffers.material_indices = reinterpret_cast<int*>(m_hiprt_scene.material_indices.get_device_pointer());
	render_data.buffers.materials_buffer = reinterpret_cast<RendererMaterial*>(m_hiprt_scene.materials_buffer.get_device_pointer());
	render_data.buffers.emissive_triangles_count = m_hiprt_scene.emissive_triangles_count;
	render_data.buffers.emissive_triangles_indices = reinterpret_cast<int*>(m_hiprt_scene.emissive_triangles_indices.get_device_pointer());

	render_data.buffers.material_textures = reinterpret_cast<oroTextureObject_t*>(m_hiprt_scene.materials_textures.get_device_pointer());
	render_data.buffers.texcoords = reinterpret_cast<float2*>(m_hiprt_scene.texcoords_buffer.get_device_pointer());
	render_data.buffers.textures_dims = reinterpret_cast<int2*>(m_hiprt_scene.textures_dims.get_device_pointer());

	render_data.aux_buffers.denoiser_normals = m_normals_AOV_buffer->map_no_error();
	render_data.aux_buffers.denoiser_albedo = m_albedo_AOV_buffer->map_no_error();
	if (m_render_settings.has_access_to_adaptive_sampling_buffers())
	{
		render_data.aux_buffers.pixel_sample_count = m_pixels_sample_count_buffer->map_no_error();
		render_data.aux_buffers.pixel_squared_luminance = m_pixels_squared_luminance_buffer.get_device_pointer();
	}
	render_data.aux_buffers.still_one_ray_active = m_still_one_ray_active_buffer.get_device_pointer();
	render_data.aux_buffers.stop_noise_threshold_count = reinterpret_cast<AtomicType<unsigned int>*>(m_pixels_converged_count_buffer.get_device_pointer());

	render_data.world_settings = m_world_settings;
	render_data.render_settings = m_render_settings;

	return render_data;
}

HIPKernel& GPURenderer::get_trace_kernel()
{
	return m_path_trace_kernel;
}

void GPURenderer::recompile_trace_kernel()
{
	// Recompiling
	m_path_trace_kernel.compile(m_hiprt_orochi_ctx->hiprt_ctx);
}

void GPURenderer::set_hiprt_scene_from_scene(const Scene& scene)
{
	HIPRTScene& hiprt_scene = m_hiprt_scene;
	HIPRTGeometry& geometry = hiprt_scene.geometry;
	
	geometry.m_hiprt_ctx = m_hiprt_orochi_ctx->hiprt_ctx;
	geometry.upload_indices(scene.triangle_indices);
	geometry.upload_vertices(scene.vertices_positions);
	geometry.build_bvh();

	hiprt_scene.has_vertex_normals.resize(scene.has_vertex_normals.size());
	hiprt_scene.has_vertex_normals.upload_data(scene.has_vertex_normals.data());

	hiprt_scene.vertex_normals.resize(scene.vertex_normals.size());
	hiprt_scene.vertex_normals.upload_data(scene.vertex_normals.data());

	hiprt_scene.material_indices.resize(scene.material_indices.size());
	hiprt_scene.material_indices.upload_data(scene.material_indices.data());

	hiprt_scene.materials_buffer.resize(scene.materials.size());
	hiprt_scene.materials_buffer.upload_data(scene.materials.data());

	hiprt_scene.emissive_triangles_count = scene.emissive_triangle_indices.size();
	if (hiprt_scene.emissive_triangles_count > 0)
	{
		hiprt_scene.emissive_triangles_indices.resize(scene.emissive_triangle_indices.size());
		hiprt_scene.emissive_triangles_indices.upload_data(scene.emissive_triangle_indices.data());
	}

	hiprt_scene.texcoords_buffer.resize(scene.texcoords.size());
	hiprt_scene.texcoords_buffer.upload_data(scene.texcoords.data());

	// We're joining the threads that were loading the scene textures in the background
	// at the last moment so that they had the maximum amount of time to load the textures
	// while the main thread was doing something else
	ThreadManager::join_threads(ThreadManager::TEXTURE_THREADS_KEY);

	if (scene.textures.size() > 0)
	{
		std::vector<oroTextureObject_t> oro_textures(scene.textures.size());
		m_materials_textures.reserve(scene.textures.size());
		for (int i = 0; i < scene.textures.size(); i++)
		{
			// We need to keep the texture alive so they are not destroyed when returning from 
			// this function so we're adding them to a member buffer
			m_materials_textures.push_back(OrochiTexture(scene.textures[i]));

			oro_textures[i] = m_materials_textures.back().get_device_texture();
		}

		hiprt_scene.materials_textures.resize(oro_textures.size());
		hiprt_scene.materials_textures.upload_data(oro_textures.data());

		hiprt_scene.textures_dims.resize(scene.textures_dims.size());
		hiprt_scene.textures_dims.upload_data(scene.textures_dims.data());
	}
}

void GPURenderer::set_scene(const Scene& scene)
{
	set_hiprt_scene_from_scene(scene);

	m_materials = scene.materials;
	m_material_names = scene.material_names;
}

void GPURenderer::set_envmap(Image32Bit& envmap_image)
{
	ThreadManager::join_threads(ThreadManager::ENVMAP_LOAD_THREAD_KEY);

	if (envmap_image.width == 0 || envmap_image.height == 0)
	{
		m_world_settings.ambient_light_type = AmbientLightType::UNIFORM;

		std::cerr << "Empty envmap set on the GPURenderer..." << std::endl;

		return;
	}

	m_envmap.init_from_image(envmap_image);
	m_envmap.compute_cdf(envmap_image);

	m_world_settings.envmap = m_envmap.get_device_texture();
	m_world_settings.envmap_width = m_envmap.width;
	m_world_settings.envmap_height = m_envmap.height;
	m_world_settings.envmap_cdf = m_envmap.get_cdf_device_pointer();
}

bool GPURenderer::has_envmap()
{
	return m_world_settings.envmap_height != 0 && m_world_settings.envmap_width != 0;
}

const std::vector<RendererMaterial>& GPURenderer::get_materials()
{
	return m_materials;
}

const std::vector<std::string>& GPURenderer::get_material_names()
{
	return m_material_names;
}

void GPURenderer::update_materials(std::vector<RendererMaterial>& materials)
{
	m_materials = materials;
	m_hiprt_scene.materials_buffer.upload_data(materials.data());
}

const Camera& GPURenderer::get_camera() const
{
	return m_camera;
}

void GPURenderer::set_camera(const Camera& camera)
{
	m_camera = camera;
}

void GPURenderer::translate_camera_view(glm::vec3 translation)
{
	m_camera.translation = m_camera.translation + translation * glm::conjugate(m_camera.rotation);
}

void GPURenderer::rotate_camera_view(glm::vec3 rotation_angles)
{
	glm::quat qx = glm::angleAxis(rotation_angles.y, glm::vec3(1.0f, 0.0f, 0.0f));
	glm::quat qy = glm::angleAxis(rotation_angles.x, glm::vec3(0.0f, 1.0f, 0.0f));

	glm::quat orientation = glm::normalize(qy * m_camera.rotation * qx);
	m_camera.rotation = orientation;
}

void GPURenderer::zoom_camera_view(float offset)
{
	glm::vec3 translation(0, 0, offset);
	m_camera.translation = m_camera.translation + translation * glm::conjugate(m_camera.rotation);
}
