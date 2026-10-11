// SPDX-License-Identifier: MIT
/*
 * Offscreen GPU check: renders a red triangle on a blue background through
 * GBM + EGL + GLES2 on a DRM render node and verifies the pixels.
 * Usage: tb3-gpu-test [/dev/dri/renderD128]
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define W 64
#define H 64

static const char *vs_src =
	"attribute vec2 pos;\n"
	"void main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *fs_src =
	"precision mediump float;\n"
	"void main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }\n";

static GLuint shader(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	GLint ok;

	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];

		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		fprintf(stderr, "shader: %s\n", log);
	}
	return s;
}

int main(int argc, char **argv)
{
	const char *node = argc > 1 ? argv[1] : "/dev/dri/renderD128";
	static const GLfloat tri[] = { -0.8f, -0.8f, 0.8f, -0.8f, 0.0f, 0.8f };
	EGLint cfg_attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
			      EGL_BLUE_SIZE, 8, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
	EGLint pb_attr[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
	EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	unsigned char px[W * H * 4];
	struct gbm_device *gbm;
	struct timespec t0, t1;
	EGLDisplay dpy;
	EGLConfig cfg;
	EGLSurface surf;
	EGLContext ctx;
	GLuint prog;
	EGLint n;
	int fd, i, centre, corner;

	fd = open(node, O_RDWR);
	if (fd < 0) {
		perror(node);
		return 1;
	}
	gbm = gbm_create_device(fd);
	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display(EGL_PLATFORM_GBM_KHR, gbm, NULL);
	if (!eglInitialize(dpy, NULL, NULL) || !eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
		fprintf(stderr, "EGL init failed: 0x%x\n", eglGetError());
		return 1;
	}
	eglBindAPI(EGL_OPENGL_ES_API);
	surf = eglCreatePbufferSurface(dpy, cfg, pb_attr);
	ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
	if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
		fprintf(stderr, "eglMakeCurrent failed: 0x%x\n", eglGetError());
		return 1;
	}
	printf("GL_VENDOR   %s\nGL_RENDERER %s\nGL_VERSION  %s\n", glGetString(GL_VENDOR),
	       glGetString(GL_RENDERER), glGetString(GL_VERSION));

	prog = glCreateProgram();
	glAttachShader(prog, shader(GL_VERTEX_SHADER, vs_src));
	glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs_src));
	glBindAttribLocation(prog, 0, "pos");
	glLinkProgram(prog);
	glUseProgram(prog);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
	glEnableVertexAttribArray(0);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (i = 0; i < 100; i++) {
		glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLES, 0, 3);
	}
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	clock_gettime(CLOCK_MONOTONIC, &t1);

	centre = ((H / 2) * W + W / 2) * 4;
	corner = ((H - 1) * W + 0) * 4;
	printf("centre RGBA %u %u %u %u, corner RGBA %u %u %u %u, 100 frames in %.1f ms\n",
	       px[centre], px[centre + 1], px[centre + 2], px[centre + 3],
	       px[corner], px[corner + 1], px[corner + 2], px[corner + 3],
	       (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
	if (px[centre] > 200 && px[centre + 2] < 50 && px[corner + 2] > 200 && px[corner] < 50) {
		printf("PASS\n");
		return 0;
	}
	printf("FAIL\n");
	return 1;
}
