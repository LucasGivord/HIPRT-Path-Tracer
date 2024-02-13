#ifndef RENDERER_H
#define RENDERER_H

#include "glm/gtc/matrix_transform.hpp"
#include "HIPRT-Orochi/orochi_buffer.h"
#include "Image/color.h"
#include "Kernels/includes/hiprt_render_data.h"
#include "Scene/camera.h"
#include "Scene/scene_parser.h"

#include <vector>

class Renderer
{
public:
	struct HIPRTOrochiCtx
	{
		void init(int device_index)
		{
			OROCHI_CHECK_ERROR(static_cast<oroError>(oroInitialize((oroApi)(ORO_API_HIP | ORO_API_CUDA), 0)));

			OROCHI_CHECK_ERROR(oroInit(0));
			OROCHI_CHECK_ERROR(oroDeviceGet(&orochi_device, device_index));
			OROCHI_CHECK_ERROR(oroCtxCreate(&orochi_ctx, 0, orochi_device));

			oroDeviceProp props;
			OROCHI_CHECK_ERROR(oroGetDeviceProperties(&props, orochi_device));

			std::cout << "hiprt ver." << HIPRT_VERSION_STR << std::endl;
			std::cout << "Executing on '" << props.name << "'" << std::endl;
			if (std::string(props.name).find("NVIDIA") != std::string::npos)
				hiprt_ctx_input.deviceType = hiprtDeviceNVIDIA;
			else
				hiprt_ctx_input.deviceType = hiprtDeviceAMD;

			hiprt_ctx_input.ctxt = oroGetRawCtx(orochi_ctx);
			hiprt_ctx_input.device = oroGetRawDevice(orochi_device);
			hiprtSetLogLevel(hiprtLogLevelInfo);

			HIPRT_CHECK_ERROR(hiprtCreateContext(HIPRT_API_VERSION, hiprt_ctx_input, hiprt_ctx));
		}

		hiprtContextCreationInput hiprt_ctx_input;
		oroCtx					  orochi_ctx;
		oroDevice				  orochi_device;

		hiprtContext hiprt_ctx;
	};

	struct HIPRTScene
	{
		HIPRTScene(hiprtContext ctx) : hiprt_ctx(ctx)
		{
			mesh.vertices = nullptr;
			mesh.triangleIndices = nullptr;
			geometry = nullptr;
		}

		~HIPRTScene()
		{
			if (mesh.triangleIndices)
				OROCHI_CHECK_ERROR(oroFree(reinterpret_cast<oroDeviceptr>(mesh.triangleIndices)));

			if (mesh.vertices)
				OROCHI_CHECK_ERROR(oroFree(reinterpret_cast<oroDeviceptr>(mesh.vertices)));

			if (geometry)
				HIPRT_CHECK_ERROR(hiprtDestroyGeometry(hiprt_ctx, geometry));

			if (material_indices)
				OROCHI_CHECK_ERROR(oroFree(reinterpret_cast<oroDeviceptr>(material_indices)));

			if (materials_buffer)
				OROCHI_CHECK_ERROR(oroFree(reinterpret_cast<oroDeviceptr>(materials_buffer)));
		}

		hiprtContext hiprt_ctx = nullptr;
		hiprtTriangleMeshPrimitive mesh;
		hiprtGeometry geometry = nullptr;

		hiprtDevicePtr material_indices;
		hiprtDevicePtr materials_buffer;
	};

	Renderer(int width, int height, HIPRTOrochiCtx* hiprt_orochi_ctx) : 
		m_framebuffer_width(width), m_framebuffer_height(height),
		m_framebuffer(width* height), m_hiprt_orochi_ctx(hiprt_orochi_ctx),
		m_trace_kernel(nullptr)
	{
		m_scene.get()->hiprt_ctx = hiprt_orochi_ctx->hiprt_ctx;
	}

	Renderer() : m_scene(nullptr) {}

	void render();
	void resize_frame(int new_width, int new_height);

	OrochiBuffer<float>& get_orochi_framebuffer();

	void init_ctx(int device_index);
	void compile_trace_kernel(const char* kernel_file_path, const char* kernel_function_name);
	void launch_kernel(int tile_size_x, int tile_size_y, int res_x, int res_y, void** launch_args);

	std::shared_ptr<HIPRTScene> create_hiprt_scene_from_scene(Scene& scene);
	void set_hiprt_scene(std::shared_ptr<HIPRTScene>);

	void set_camera(const Camera& camera);
	void translate_camera_view(glm::vec3 translation);
	void rotate_camera_view(glm::vec3 rotation_angles);
	void zoom_camera_view(float offset);


	int m_framebuffer_width, m_framebuffer_height;

	Camera m_camera;

private:
	OrochiBuffer<float> m_framebuffer;

	std::shared_ptr<HIPRTOrochiCtx> m_hiprt_orochi_ctx;
	oroFunction m_trace_kernel;
	std::shared_ptr<HIPRTScene> m_scene;
	HIPRTRenderData m_scene_data;
};

#endif

