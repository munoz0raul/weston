/*
 * Copyright © 2011 Benjamin Franzke
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <assert.h>
#include <signal.h>

#include <wayland-client.h>
#include <wayland-egl.h>

#include <GLES2/gl2.h>
#include <EGL/egl.h>

struct window;

struct display {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shell *shell;
	struct wl_input_device *input;
	struct {
		EGLDisplay dpy;
		EGLContext ctx;
		EGLConfig conf;
	} egl;
	uint32_t mask;
	struct window *window;
};

struct window {
	struct display *display;
	struct {
		int width, height;
	} geometry;
	struct {
		GLuint fbo;
		GLuint color_rbo;

		GLuint program;
		GLuint rotation_uniform;

		GLuint pos;
		GLuint col;
	} gl;

	struct wl_egl_window *native;
	struct wl_surface *surface;
	struct wl_shell_surface *shell_surface;
	EGLSurface egl_surface;
	struct wl_callback *callback;
	int fullscreen, configured;
};

static const char *vert_shader_text =
	"uniform mat4 rotation;\n"
	"attribute vec4 pos;\n"
	"attribute vec4 color;\n"
	"varying vec4 v_color;\n"
	"void main() {\n"
	"  gl_Position = rotation * pos;\n"
	"  v_color = color;\n"
	"}\n";

static const char *frag_shader_text =
	"precision mediump float;\n"
	"varying vec4 v_color;\n"
	"void main() {\n"
	"  gl_FragColor = v_color;\n"
	"}\n";

static void
init_egl(struct display *display, EGLint alpha_size)
{
	static const EGLint context_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};

	EGLint config_attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RED_SIZE, 1,
		EGL_GREEN_SIZE, 1,
		EGL_BLUE_SIZE, 1,
		EGL_ALPHA_SIZE, alpha_size,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NONE
	};

	EGLint major, minor, n;
	EGLBoolean ret;

	display->egl.dpy = eglGetDisplay(display->display);
	assert(display->egl.dpy);

	ret = eglInitialize(display->egl.dpy, &major, &minor);
	assert(ret == EGL_TRUE);
	ret = eglBindAPI(EGL_OPENGL_ES_API);
	assert(ret == EGL_TRUE);

	assert(eglChooseConfig(display->egl.dpy, config_attribs,
			       &display->egl.conf, 1, &n) && n == 1);

	display->egl.ctx = eglCreateContext(display->egl.dpy,
					    display->egl.conf,
					    EGL_NO_CONTEXT, context_attribs);
	assert(display->egl.ctx);

}

static void
fini_egl(struct display *display)
{
	/* Required, otherwise segfault in egl_dri2.c: dri2_make_current()
	 * on eglReleaseThread(). */
	eglMakeCurrent(display->egl.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
		       EGL_NO_CONTEXT);

	eglTerminate(display->egl.dpy);
	eglReleaseThread();
}

static GLuint
create_shader(struct window *window, const char *source, GLenum shader_type)
{
	GLuint shader;
	GLint status;

	shader = glCreateShader(shader_type);
	assert(shader != 0);

	glShaderSource(shader, 1, (const char **) &source, NULL);
	glCompileShader(shader);

	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (!status) {
		char log[1000];
		GLsizei len;
		glGetShaderInfoLog(shader, 1000, &len, log);
		fprintf(stderr, "Error: compiling %s: %*s\n",
			shader_type == GL_VERTEX_SHADER ? "vertex" : "fragment",
			len, log);
		exit(1);
	}

	return shader;
}

static void
init_gl(struct window *window)
{
	GLuint frag, vert;
	GLint status;

	glViewport(0, 0, window->geometry.width, window->geometry.height);

	frag = create_shader(window, frag_shader_text, GL_FRAGMENT_SHADER);
	vert = create_shader(window, vert_shader_text, GL_VERTEX_SHADER);

	window->gl.program = glCreateProgram();
	glAttachShader(window->gl.program, frag);
	glAttachShader(window->gl.program, vert);
	glLinkProgram(window->gl.program);

	glGetProgramiv(window->gl.program, GL_LINK_STATUS, &status);
	if (!status) {
		char log[1000];
		GLsizei len;
		glGetProgramInfoLog(window->gl.program, 1000, &len, log);
		fprintf(stderr, "Error: linking:\n%*s\n", len, log);
		exit(1);
	}

	glUseProgram(window->gl.program);
	
	window->gl.pos = 0;
	window->gl.pos = 1;

	glBindAttribLocation(window->gl.program, window->gl.pos, "pos");
	glBindAttribLocation(window->gl.program, window->gl.col, "color");
	glLinkProgram(window->gl.program);

	window->gl.rotation_uniform =
		glGetUniformLocation(window->gl.program, "rotation");
}

static void
handle_ping(void *data, struct wl_shell_surface *shell_surface,
	    uint32_t serial)
{
	wl_shell_surface_pong(shell_surface, serial);
}

static void
handle_configure(void *data, struct wl_shell_surface *shell_surface,
		 uint32_t edges, int32_t width, int32_t height)
{
	struct window *window = data;

	window->geometry.width = width;
	window->geometry.height = height;
	window->configured = 1;
}

static void
handle_popup_done(void *data, struct wl_shell_surface *shell_surface)
{
}

static const struct wl_shell_surface_listener shell_surface_listener = {
	handle_ping,
	handle_configure,
	handle_popup_done
};

static void
create_surface(struct window *window)
{
	struct display *display = window->display;
	EGLBoolean ret;
	
	window->surface = wl_compositor_create_surface(display->compositor);
	window->shell_surface = wl_shell_get_shell_surface(display->shell,
							   window->surface);

	wl_shell_surface_add_listener(window->shell_surface,
				      &shell_surface_listener, window);

	if (window->fullscreen) {
		window->configured = 0;
		wl_shell_surface_set_fullscreen(window->shell_surface,
						WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT,
						0, NULL);

		while (!window->configured)
			wl_display_iterate(display->display, display->mask);
	}
	else
		wl_shell_surface_set_toplevel(window->shell_surface);

	window->native =
		wl_egl_window_create(window->surface,
				     window->geometry.width,
				     window->geometry.height);
	window->egl_surface =
		eglCreateWindowSurface(display->egl.dpy,
				       display->egl.conf,
				       window->native, NULL);

	ret = eglMakeCurrent(window->display->egl.dpy, window->egl_surface,
			     window->egl_surface, window->display->egl.ctx);
	assert(ret == EGL_TRUE);
}

static void
destroy_surface(struct window *window)
{
	wl_egl_window_destroy(window->native);

	wl_shell_surface_destroy(window->shell_surface);
	wl_surface_destroy(window->surface);

	if (window->callback)
		wl_callback_destroy(window->callback);
}

static const struct wl_callback_listener frame_listener;

static void
redraw(void *data, struct wl_callback *callback, uint32_t time)
{
	struct window *window = data;
	static const GLfloat verts[3][2] = {
		{ -0.5, -0.5 },
		{  0.5, -0.5 },
		{  0,    0.5 }
	};
	static const GLfloat colors[3][3] = {
		{ 1, 0, 0 },
		{ 0, 1, 0 },
		{ 0, 0, 1 }
	};
	GLfloat angle;
	GLfloat rotation[4][4] = {
		{ 1, 0, 0, 0 },
		{ 0, 1, 0, 0 },
		{ 0, 0, 1, 0 },
		{ 0, 0, 0, 1 }
	};
	static const int32_t speed_div = 5;
	static uint32_t start_time = 0;

	if (start_time == 0)
		start_time = time;

	angle = ((time-start_time) / speed_div) % 360 * M_PI / 180.0;
	rotation[0][0] =  cos(angle);
	rotation[0][2] =  sin(angle);
	rotation[2][0] = -sin(angle);
	rotation[2][2] =  cos(angle);

	glUniformMatrix4fv(window->gl.rotation_uniform, 1, GL_FALSE,
			   (GLfloat *) rotation);

	glClearColor(0.0, 0.0, 0.0, 0.5);
	glClear(GL_COLOR_BUFFER_BIT);

	glVertexAttribPointer(window->gl.pos, 2, GL_FLOAT, GL_FALSE, 0, verts);
	glVertexAttribPointer(window->gl.col, 3, GL_FLOAT, GL_FALSE, 0, colors);
	glEnableVertexAttribArray(window->gl.pos);
	glEnableVertexAttribArray(window->gl.col);

	glDrawArrays(GL_TRIANGLES, 0, 3);

	glDisableVertexAttribArray(window->gl.pos);
	glDisableVertexAttribArray(window->gl.col);

	glFlush();

	eglSwapBuffers(window->display->egl.dpy, window->egl_surface);
	if (callback)
		wl_callback_destroy(callback);

	window->callback = wl_surface_frame(window->surface);
	wl_callback_add_listener(window->callback, &frame_listener, window);
}

static const struct wl_callback_listener frame_listener = {
	redraw
};

static void
input_handle_motion(void *data, struct wl_input_device *input_device,
		    uint32_t time, int32_t sx, int32_t sy)
{
}

static void
input_handle_button(void *data,
		    struct wl_input_device *input_device, uint32_t serial,
		    uint32_t time, uint32_t button, uint32_t state)
{
}

static void
input_handle_axis(void *data,
		    struct wl_input_device *input_device,
		    uint32_t time, uint32_t axis, int32_t value)
{
}

static void
input_handle_key(void *data, struct wl_input_device *input_device,
		 uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
}

static void
input_handle_pointer_enter(void *data,
			   struct wl_input_device *input_device,
			   uint32_t serial, struct wl_surface *surface,
			   int32_t sx, int32_t sy)
{
	struct display *display = data;

	if (display->window->fullscreen)
		wl_input_device_attach(input_device, serial, NULL, 0, 0);
}

static void
input_handle_pointer_leave(void *data,
			   struct wl_input_device *input_device,
			   uint32_t serial, struct wl_surface *surface)
{
}

static void
input_handle_keyboard_enter(void *data,
			    struct wl_input_device *input_device,
			    uint32_t serial,
			    struct wl_surface *surface,
			    struct wl_array *keys)
{
}

static void
input_handle_keyboard_leave(void *data,
			    struct wl_input_device *input_device,
			    uint32_t serial,
			    struct wl_surface *surface)
{
}

static void
input_handle_touch_down(void *data,
			struct wl_input_device *wl_input_device,
			uint32_t serial, uint32_t time,
			struct wl_surface *surface,
			int32_t id, int32_t x, int32_t y)
{
}

static void
input_handle_touch_up(void *data,
		      struct wl_input_device *wl_input_device,
		      uint32_t serial, uint32_t time, int32_t id)
{
}

static void
input_handle_touch_motion(void *data,
			  struct wl_input_device *wl_input_device,
			  uint32_t time, int32_t id, int32_t x, int32_t y)
{
}

static void
input_handle_touch_frame(void *data,
			 struct wl_input_device *wl_input_device)
{
}

static void
input_handle_touch_cancel(void *data,
			  struct wl_input_device *wl_input_device)
{
}

static const struct wl_input_device_listener input_listener = {
	input_handle_motion,
	input_handle_button,
	input_handle_axis,
	input_handle_key,
	input_handle_pointer_enter,
	input_handle_pointer_leave,
	input_handle_keyboard_enter,
	input_handle_keyboard_leave,
	input_handle_touch_down,
	input_handle_touch_up,
	input_handle_touch_motion,
	input_handle_touch_frame,
	input_handle_touch_cancel,
};

static void
display_handle_global(struct wl_display *display, uint32_t id,
		      const char *interface, uint32_t version, void *data)
{
	struct display *d = data;

	if (strcmp(interface, "wl_compositor") == 0) {
		d->compositor =
			wl_display_bind(display, id, &wl_compositor_interface);
	} else if (strcmp(interface, "wl_shell") == 0) {
		d->shell = wl_display_bind(display, id, &wl_shell_interface);
	} else if (strcmp(interface, "wl_input_device") == 0) {
		d->input = wl_display_bind(display, id, &wl_input_device_interface);
		wl_input_device_add_listener(d->input, &input_listener, d);
	}
}

static int
event_mask_update(uint32_t mask, void *data)
{
	struct display *d = data;

	d->mask = mask;

	return 0;
}

static int running = 1;

static void
signal_int(int signum)
{
	running = 0;
}

int
main(int argc, char **argv)
{
	struct sigaction sigint;
	struct display display = { 0 };
	struct window  window  = { 0 };

	window.display = &display;
	display.window = &window;
	window.geometry.width  = 250;
	window.geometry.height = 250;

	if (argc >= 2 && strcmp("-f", argv[0]))
		window.fullscreen = 1;

	display.display = wl_display_connect(NULL);
	assert(display.display);

	wl_display_add_global_listener(display.display,
				       display_handle_global, &display);

	wl_display_get_fd(display.display, event_mask_update, &display);
	wl_display_iterate(display.display, WL_DISPLAY_READABLE);

	init_egl(&display, window.fullscreen ? 0 : 1);
	create_surface(&window);
	init_gl(&window);

	sigint.sa_handler = signal_int;
	sigemptyset(&sigint.sa_mask);
	sigint.sa_flags = SA_RESETHAND;
	sigaction(SIGINT, &sigint, NULL);

	redraw(&window, NULL, 0);

	while (running)
		wl_display_iterate(display.display, display.mask);

	fprintf(stderr, "simple-egl exiting\n");

	destroy_surface(&window);
	fini_egl(&display);

	if (display.shell)
		wl_shell_destroy(display.shell);

	if (display.compositor)
		wl_compositor_destroy(display.compositor);

	wl_display_flush(display.display);
	wl_display_disconnect(display.display);

	return 0;
}
