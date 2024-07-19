/*
 * Copyright 2024 Tom Clabault. GNU GPL3 license.
 * GNU GPL3 license copy: https://www.gnu.org/licenses/gpl-3.0.txt
 */

#ifndef RENDER_WINDOW_H
#define RENDER_WINDOW_H

#include "OpenGL/OpenGLProgram.h"
#include "Renderer/OpenImageDenoiser.h"
#include "Renderer/GPURenderer.h"
#include "UI/ApplicationSettings.h"
#include "UI/DisplayTextureType.h"
#include "UI/DisplayViewEnum.h"
#include "UI/DisplayViewSystem.h"
#include "UI/ImGuiRenderer.h"
#include "UI/PerformanceMetricsComputer.h"
#include "UI/RenderWindowKeyboardInteractor.h"
#include "UI/RenderWindowMouseInteractor.h"
#include "UI/Screenshoter.h"
#include "Utils/CommandlineArguments.h"

#include "GL/glew.h"
#include "GLFW/glfw3.h"

class RenderWindow
{
public:
	RenderWindow(int width, int height, std::shared_ptr<HIPRTOrochiCtx> hiprt_oro_ctx);
	~RenderWindow();

	void init_glfw(int width, int height);
	void init_gl(int width, int height);
	void init_imgui();

	static void APIENTRY gl_debug_output_callback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam);
	void resize_frame(int pixels_width, int pixels_height);
	void change_resolution_scaling(float new_scaling);

	int get_width();
	int get_height();
	bool is_interacting();

	RenderWindowKeyboardInteractor& get_keyboard_interactor();
	std::shared_ptr<RenderWindowMouseInteractor> get_mouse_interactor();

	std::shared_ptr<ApplicationSettings> get_application_settings();
	std::shared_ptr<GPURenderer> get_renderer();
	std::shared_ptr<OpenImageDenoiser> get_denoiser();
	std::shared_ptr<PerformanceMetricsComputer> get_performance_metrics();
	std::shared_ptr<Screenshoter> get_screenshoter();
	std::shared_ptr<ImGuiRenderer> get_imgui_renderer();

	std::shared_ptr<DisplayViewSystem> get_display_view_system();

	/**
	 * Translates the camera along its X and Y axis by translation_x and translation_y respectively.
	 * 
	 * If scale_translation is true, translation_x and translation_y are scaled by the delta time of
	 * the application and the camera speed before the translation is applied to the camera. You may
	 * want to set scale_translation to false when translation the camera with a mouse pan and scale it
	 * manually by a predefined arbitrary factor
 	 */
	void update_renderer_view_translation(float translation_x, float translation_y, bool scale_translation);
	void update_renderer_view_zoom(float offset, bool scale_delta_time);
	void update_renderer_view_rotation(float offset_x, float offset_y);

	void increment_sample_number();
	/**
	 * Returns true if the renderer is not sampling the image anymore. 
	 * This can be the case if all pixels have converged according to
	 * adaptive sampling or if the maximum number of samples specified by
	 * the user has been reached, etc...
	 */
	bool is_rendering_done();
	void reset_render();
	void set_render_dirty(bool render_dirty);

	float get_current_render_time();
	float get_samples_per_second();
	float compute_samples_per_second();

	void run();
	void render();
	/**
	 * Denoises the color framebuffer if necessary (according to ImGui
	 * parameters such as denoiser sample skip, only denoise when rendering done, ...)
	 * 
	 * Returns true if denoising occured and if the application needs to display the denoised data.
	 * False otherwise.
	 * 
	 * That return value can be usedf to decide whether or not we need to upload to denoised data
	 * to OpenGL or not (we need to upload it if something new was denoised AND the application
	 * wants to display the denoised data. This condition corresponds exactly to the returned value)
	 */
	bool denoise();
	void quit();

	// TODO: why is this not in linux mouse interactor only and why is it in renderwindow ?
	std::pair<float, float> get_cursor_position()
	{
		return m_cursor_position;
	}

	void set_cursor_position(std::pair<float, float> new_cursor_position)
	{
		m_cursor_position = new_cursor_position;
	}

private:
	int m_viewport_width, m_viewport_height;

	// How long the current render has been running for in milliseconds
	float m_last_delta_time_ms = 0.0f;
	float m_current_render_time_ms = 0.0f;
	float m_samples_per_second = 0.0f;

	std::shared_ptr<ApplicationSettings> m_application_settings;

	// Set to true if some settings of the render changed and we need
	// to restart rendering from sample 0
	bool m_render_dirty = true;
	// If true, this means that the first frame (after a call to reset_render()) is
	// still being rendered and we should not queue another first frame.
	// If we keep queuing "first frames" (for example when launching the mouse wheel
	// really fast) then we'll never display anything because the CUDA/HIP stream will
	// always have work to do and will never be "ready" for displaying the frame so
	// that's why we only allow one "first frame"
	bool m_first_frame_still_queued = false;

	std::shared_ptr<GPURenderer> m_renderer;
	std::shared_ptr<OpenImageDenoiser> m_denoiser;
	std::shared_ptr<PerformanceMetricsComputer> m_perf_metrics;
	std::shared_ptr<Screenshoter> m_screenshoter;

	// Encapsulates the handling of display views
	std::shared_ptr<DisplayViewSystem> m_display_view_system;

	GLFWwindow* m_glfw_window;
	// Needs to be a pointer (using unique_ptr here) because we're using polymorphism for the Linux/Windows implementation here
	std::shared_ptr<RenderWindowMouseInteractor> m_mouse_interactor;
	RenderWindowKeyboardInteractor m_keyboard_interactor;
	std::shared_ptr<ImGuiRenderer> m_imgui_renderer;

	std::pair<float, float> m_cursor_position;
};

#endif
