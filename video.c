#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#if USE_DRM
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gbm.h>
#include <libdrm/drm.h>
#include <xf86drmMode.h>
#include <xf86drm.h>
#else
#include <GLFW/glfw3.h>
#endif
#include <png.h>

#define GL_CHECK(x) \
    x; \
    { \
        GLenum glError = glGetError(); \
        if(glError != GL_NO_ERROR) { \
            printf("glGetError() = %i (0x%.8x) at %s:%i\n", glError, glError, __FILE__, __LINE__); \
            exit(1); \
        } \
    }
extern bool pausing;

#define EGL_PLATFORM_GBM_KHR 0x31D7

GLFWwindow* g_window;

struct gbm_device *g_gbm_dev = NULL;
struct gbm_surface *g_gbm_surface  = NULL;
struct gbm_bo *g_bo = NULL;
struct gbm_bo *g_next_bo = NULL;
unsigned int bo_fb_id;
unsigned int next_bo_fb_id;
#if	0 
int g_drm_fd;
int g_crtc_id;
int g_prev_crtc_id;
int g_connector_id;
struct pollfd g_drm_fds;
drmModeConnector *g_drm_connector     = NULL;
drmModeModeInfo *g_drm_mode = NULL;
drmModeRes *g_drm_resources    = NULL;
drmModeEncoder *g_drm_encoder = NULL;
drmModeCrtcPtr prev_crtc = NULL;

EGLDisplay g_display;
EGLConfig g_config;
EGLContext g_context;
EGLSurface g_surface;
#endif

GLuint g_FBO[2] = {0,0};
GLuint g_FBOTex[2] = {0,0};

#if USE_DRM
EGLint configAttributes[] =
{
	EGL_SAMPLES,             4,
	EGL_ALPHA_SIZE,          0,
	EGL_RED_SIZE,            8,
	EGL_GREEN_SIZE,          8,
	EGL_BLUE_SIZE,           8,
	EGL_BUFFER_SIZE,         32,
	EGL_STENCIL_SIZE,        0,
	EGL_RENDERABLE_TYPE,     EGL_OPENGL_ES2_BIT,
	EGL_SURFACE_TYPE,        EGL_WINDOW_BIT, 
	EGL_DEPTH_SIZE,          16,

	EGL_NONE
};

EGLint contextAttributes[] =
{
	EGL_CONTEXT_MAJOR_VERSION, 2,
	EGL_NONE, EGL_NONE,
	EGL_NONE
};

EGLint windowAttributes[] =
{
	EGL_NONE
};
#endif

GLuint g_width;
GLuint g_height;
GLuint g_pixfmt;
GLuint g_pixtype; 
GLuint g_bpp;
GLuint glsl_buffers;

struct shader_t{
    GLuint vao;
    GLuint vbo;
    GLuint program;
    GLuint program_overlay;

    GLint i_pos;
    GLint i_coord;
    GLint u_tex;
    GLint u_tex2;

} g_shader = {0};

const char *g_vshader_src =
	"precision highp float;\n"
    "attribute vec4 i_pos;\n"
    "varying vec2 o_coord;\n"
    "void main() {\n"
        "o_coord = i_pos.zw;\n"
        "gl_Position = vec4(i_pos.xy, 0.5, 1.0);\n"
    "}";

const char *g_fshader_src =
	"precision highp float;\n"
    "varying vec2 o_coord;\n"
    "uniform sampler2D u_tex;\n"
    "uniform sampler2D u_tex2;\n"
    "void main() {\n"
        "gl_FragColor = texture2D(u_tex, o_coord);\n"
    "}";

png_byte color_type;
png_byte bit_depth;
png_bytep *row_pointers;
int pngwidth, pngheight;
char buffer1[1280 * 720 * 4];

void read_png_file(char *filename) {
  FILE *fp = fopen(filename, "rb");
	if (!fp) {
		printf("Failed to open %s\n", filename);
		abort();
	}

  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if(!png) abort();

  png_infop info = png_create_info_struct(png);
  if(!info) abort();

  if(setjmp(png_jmpbuf(png))) abort();

  png_init_io(png, fp);

  png_read_info(png, info);

  pngwidth      = png_get_image_width(png, info);
  pngheight     = png_get_image_height(png, info);
  color_type = png_get_color_type(png, info);
  bit_depth  = png_get_bit_depth(png, info);
  printf("%s %d - %d %d\n", __FUNCTION__, __LINE__, pngwidth, pngheight);

  // Read any color_type into 8bit depth, RGBA format.
  // See http://www.libpng.org/pub/png/libpng-manual.txt

  if(bit_depth == 16)
    png_set_strip_16(png);

  if(color_type == PNG_COLOR_TYPE_PALETTE)
    png_set_palette_to_rgb(png);

  // PNG_COLOR_TYPE_GRAY_ALPHA is always 8 or 16bit depth.
  if(color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
    png_set_expand_gray_1_2_4_to_8(png);

  if(png_get_valid(png, info, PNG_INFO_tRNS))
    png_set_tRNS_to_alpha(png);

  // These color_type don't have an alpha channel then fill it with 0xff.
  if(color_type == PNG_COLOR_TYPE_RGB ||
     color_type == PNG_COLOR_TYPE_GRAY ||
     color_type == PNG_COLOR_TYPE_PALETTE)
    png_set_filler(png, 0xFF, PNG_FILLER_AFTER);

  if(color_type == PNG_COLOR_TYPE_GRAY ||
     color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
    png_set_gray_to_rgb(png);

  png_read_update_info(png, info);

  row_pointers = (png_bytep*)malloc(sizeof(png_bytep) * pngheight);
  for(int y = 0; y < pngheight; y++) {
    row_pointers[y] = (png_byte*)malloc(png_get_rowbytes(png,info));
  }

  png_read_image(png, row_pointers);

  fclose(fp);
}

void process_png_file() {
	unsigned short s, *ps;
  for(int y = 0; y < pngheight; y++) {
    png_bytep row = row_pointers[y];

    if (g_bpp == sizeof(int)) {
    	memcpy(buffer1 + y*1280*g_bpp, row, pngwidth*4);
    }
 	else {
	    ps = (unsigned short*) (buffer1 + y*1280*g_bpp);
	    for(int x = 0; x < pngwidth; x++) {
	      png_bytep px = &(row[x * 4]);
	      
	      // Do something awesome for each pixel here...
	      //printf("%4d, %4d = RGBA(%3d, %3d, %3d, %3d)\n", x, y, px[0], px[1], px[2], px[3]);
	      s = ((px[2] >> 3) & 0x1f) + ((px[1] >> 2)<<5) + ((px[0] >> 3) << 11);
	      *ps++ = s;
	    }
	}
    
  }
}

#if USE_DRM
unsigned int get_drm_fb(struct gbm_bo *bo)
{
	int ret;
	unsigned width, height, stride, handle;
	unsigned int id;

	width  = gbm_bo_get_width(bo);
	height = gbm_bo_get_height(bo);
	stride = gbm_bo_get_stride(bo);
	handle = gbm_bo_get_handle(bo).u32;
   	if(drmModeAddFB(g_drm_fd, width, height, 24, 32,
         stride, handle, &id)) {
		printf("drmModeAddFB failed\n");
		return -1;
	}
	return id;
}

int initVideo()
{
	int ret;
	EGLint numberOfConfigs = 0;
    EGLConfig *configsArray = NULL;
	EGLint err, redSize, greenSize, blueSize;
    EGLint attributeValue = 0;
	bool matchFound = false;
	int matchingConfig = -1;
	int i;

	g_drm_fd = open("/dev/dri/card0", O_RDWR);
	if(g_drm_fd < 0)
		printf("open DRM device failed\n");
	
	g_drm_fds.fd = g_drm_fd;
	g_drm_fds.events = POLLIN;

	drmSetMaster(g_drm_fd);

	g_gbm_dev = gbm_create_device(g_drm_fd);
	if(!g_gbm_dev)
		printf("couldn't create GBM device\n");
   
	g_drm_resources = drmModeGetResources(g_drm_fd);
	if(!g_drm_resources)
		printf("drmModeGetResources failed\n");
	for(i = 0; i < g_drm_resources->count_connectors; i++) {
		g_drm_connector = drmModeGetConnector(g_drm_fd, g_drm_resources->connectors[0]);
		if(g_drm_connector != NULL &&
				g_drm_connector->connection == DRM_MODE_CONNECTED &&
				g_drm_connector->count_modes > 0) {
    		g_drm_mode = &g_drm_connector->modes[0];//TODO
			break;
		}
		drmModeFreeConnector(g_drm_connector);
		g_drm_connector = NULL;
	}

	g_connector_id = g_drm_connector->connector_id;

	//printf("mode-> width %d, height %d, refresh rate: %d\n", g_drm_mode->hdisplay, g_drm_mode->vdisplay, g_drm_mode->vrefresh);

	for(i = 0; i < g_drm_resources->count_encoders; i++) {
		g_drm_encoder = drmModeGetEncoder(g_drm_fd, g_drm_resources->encoders[i]);
		if (g_drm_encoder->encoder_id == g_drm_connector->encoder_id)
			break;
		drmModeFreeEncoder(g_drm_encoder);
		g_drm_encoder = NULL;
	}

	if (g_drm_encoder)
		g_crtc_id = g_drm_encoder->crtc_id;


	g_gbm_surface = gbm_surface_create(
		g_gbm_dev,
		g_drm_mode->hdisplay,
		g_drm_mode->vdisplay,
		GBM_FORMAT_XRGB8888,
		GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);

	if (!g_gbm_surface)
	{
	  printf("couldn't create GBM surface\n");
      goto error;
	}

    PFNEGLGETPLATFORMDISPLAYEXTPROC ptr_eglGetPlatformDisplayEXT;

	ptr_eglGetPlatformDisplayEXT = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
   	  eglGetProcAddress("eglGetPlatformDisplayEXT");
	if (ptr_eglGetPlatformDisplayEXT != NULL)
	{
		g_display = ptr_eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, g_gbm_dev, NULL);
		if (g_display == EGL_NO_DISPLAY) {
			printf("failed to get egl display\n");
			goto error;
		}
	}
    
	ret = eglInitialize(g_display, NULL, NULL);
  	if(ret != EGL_TRUE)
    {
        err = eglGetError();
        printf("eglGetError(): (0x%.4x)\n", (int)err);
        printf("Failed to initialize EGL at %s:%i\n", __FILE__, __LINE__);
		goto error;
    }

	ret = eglChooseConfig(g_display, configAttributes, NULL, 0, &numberOfConfigs);
    if(ret != EGL_TRUE)
    {
        err = eglGetError();
        printf("eglGetError(): %i (0x%.4x)\n", (int)err, (int)err);
        printf("Failed to enumerate EGL configs at %s:%i\n", __FILE__, __LINE__);
		goto error;
    }

	configsArray = (EGLConfig *)calloc(numberOfConfigs, sizeof(EGLConfig));
	if(configsArray == NULL)
	{
    	printf("Out of memory at %s:%i\n", __FILE__, __LINE__);
		goto error;
	}
	ret = eglChooseConfig(g_display, configAttributes, configsArray, numberOfConfigs, &numberOfConfigs);
	if(ret != EGL_TRUE)
	{
    	err = eglGetError();
    	printf("eglGetError(): %i (0x%.4x)\n", (int)err, (int)err);
    	printf("Failed to enumerate EGL configs at %s:%i\n", __FILE__, __LINE__);
		goto error;
	}

	redSize = configAttributes[5];
	greenSize = configAttributes[7];
	blueSize = configAttributes[9];

	for(int configsIndex = 0; (configsIndex < numberOfConfigs) && !matchFound; configsIndex++)
	{
		ret = eglGetConfigAttrib(g_display, configsArray[configsIndex], EGL_RED_SIZE, &attributeValue);
        if(ret != EGL_TRUE)
        {
			err = eglGetError();
			printf("eglGetError(): %i (0x%.4x)\n", (int)err, (int)err);
			printf("Failed to get EGL attribute at %s:%i\n", __FILE__, __LINE__);
			goto error;
        }

		if(attributeValue == redSize)
		{
			ret = eglGetConfigAttrib(g_display, configsArray[configsIndex], EGL_GREEN_SIZE, &attributeValue);
			if(ret != EGL_TRUE)
			{
    			err = eglGetError();
                printf("eglGetError(): %i (0x%.4x)\n", (int)err, (int)err);
                printf("Failed to get EGL attribute at %s:%i\n", __FILE__, __LINE__);
				goto error;
        	}

        	if(attributeValue == greenSize)
            {
            	ret = eglGetConfigAttrib(g_display, configsArray[configsIndex], EGL_BLUE_SIZE, &attributeValue);
                if(ret != EGL_TRUE)
                {
                    err = eglGetError();
                    printf("eglGetError(): %i (0x%.4x)\n", (int)err, (int)err);
                	printf("Failed to get EGL attribute at %s:%i\n", __FILE__, __LINE__);
					goto error;
                }

                if(attributeValue == blueSize) 
                {
                	matchFound = true;
                    matchingConfig = configsIndex;
                }
            }
        }
    }
    
	if(!matchFound)
    {
        printf("Failed to find matching EGL config at %s:%i\n", __FILE__, __LINE__);
		goto error;
    }

    g_config = configsArray[matchingConfig];
	free(configsArray);
	configsArray = NULL;

    g_surface = eglCreateWindowSurface(g_display, g_config, (EGLNativeWindowType)(g_gbm_surface), windowAttributes);
    if(g_surface == EGL_NO_SURFACE)
    {
		err = eglGetError();
		printf("eglGetError(): %i (0x%.4x)\n", (int)err, (int)err);
        printf("Failed to create EGL surface at %s:%i\n", __FILE__, __LINE__);
		goto error;
    }

    /* Unconditionally bind to OpenGL ES API as we exit this function, since it's the default. */
    eglBindAPI(EGL_OPENGL_ES_API);

    g_context = eglCreateContext(g_display, g_config, EGL_NO_CONTEXT, contextAttributes);
    if(g_context == EGL_NO_CONTEXT)
    {
        err = eglGetError();
        printf("eglGetError(): %i (0x%.4x)\n", (int)err, (int)err);
        printf("Failed to create EGL context at %s:%i\n", __FILE__, __LINE__);
		goto error;
    }

	ret = eglMakeCurrent(g_display, g_surface, g_surface, g_context);
	if(ret != EGL_TRUE)
    {
        err = eglGetError();
        printf("eglGetError(): (0x%.4x)\n", (int)err);
        printf("Failed to EGL MakeCurrent at %s:%i\n", __FILE__, __LINE__);
    }

	if(eglSwapBuffers(g_display, g_surface) != EGL_TRUE) {
		err = eglGetError();
        printf("eglGetError(): %i (0x%.4x)\n", (int)err, (int)err);
		printf("eglSwapBuffers failed\n");
	}

	g_bo = gbm_surface_lock_front_buffer(g_gbm_surface);
	if(!g_bo)
		printf("gbm_surface_lock_front_buffer failed\n");

	bo_fb_id = get_drm_fb(g_bo);
	if(bo_fb_id == -1)
		printf("get_drm_fb failed\n");

	prev_crtc = drmModeGetCrtc(g_drm_fd, g_crtc_id);
	/* set mode: */
	ret = drmModeSetCrtc(g_drm_fd, g_crtc_id, bo_fb_id, 0, 0,
	    &g_connector_id, 1, g_drm_mode);
	if (ret) {
		printf("failed to set mode\n");
	}


	return 0;

error:
	if(g_gbm_dev)
		gbm_device_destroy(g_gbm_dev);

	return 1;
}
#else
int initVideo()
{
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    g_window = glfwCreateWindow(1280, 720, __FILE__, NULL, NULL);
    glfwMakeContextCurrent(g_window);

    printf("GL_VERSION  : %s\n", glGetString(GL_VERSION) );
    printf("GL_RENDERER : %s\n", glGetString(GL_RENDERER) );
}
#endif

#if USE_DRM
void page_flip_handler
(int fd, unsigned int frame,
 unsigned int sec, unsigned int usec,
 void * data)
{
	int *waiting_for_flip = data;
	*waiting_for_flip = 0;
}

drmEventContext g_evctx = {
	.version = DRM_EVENT_CONTEXT_VERSION,
	.page_flip_handler = page_flip_handler,
};

int waiting_for_flip = 0;

bool drm_wait_flip(int timeout)
{
   g_drm_fds.revents = 0;

   if (poll(&g_drm_fds, 1, timeout) < 0)
      return false;

   if (g_drm_fds.revents & (POLLHUP | POLLERR))
      return false;

   if (g_drm_fds.revents & POLLIN)
   {
      drmHandleEvent(g_drm_fd, &g_evctx);
      return true;
   }

   return false;
}
bool wait_flip(bool block)
{
	int poll_state;
	int timeout = 0;

	if (!waiting_for_flip)
		return false;

	if(block)
		timeout = -1;

	while (waiting_for_flip) {
      if (!drm_wait_flip(timeout))
         break;
	}

	if(waiting_for_flip)
		return true;


	gbm_surface_release_buffer(g_gbm_surface, g_bo);
	g_bo = g_next_bo;

	return false;
}
#endif

void swapBuffers()
{
#if USE_DRM
	EGLint err;

	if(eglSwapBuffers(g_display, g_surface) != EGL_TRUE) {
		err = eglGetError();
        printf("eglGetError(): %i (0x%.4x)\n", (int)err, (int)err);
		printf("eglSwapBuffers failed\n");
	}

	if (wait_flip(false))
		return;

	g_next_bo = gbm_surface_lock_front_buffer(g_gbm_surface);
	if(!g_next_bo)
		printf("gbm_surface_lock_front_buffer failed\n");

	next_bo_fb_id = get_drm_fb(g_next_bo);
	if(next_bo_fb_id == -1) {
		printf("get_drm_fb failed\n");
		return;
	}

	if (drmModePageFlip(g_drm_fd, g_crtc_id, next_bo_fb_id, 
		DRM_MODE_PAGE_FLIP_EVENT, &waiting_for_flip) == 0)
		waiting_for_flip = 1;
	else {
		printf("drmModePageFlip failed\n");
		return;
	}

	wait_flip(true);
#else
	glfwSwapBuffers(g_window);
}
#endif

int initGL()
{
	//int width, height;
	int i;

    /* Initialize OpenGL ES. */
    GL_CHECK(glEnable(GL_CULL_FACE));
    GL_CHECK(glCullFace(GL_BACK));
    GL_CHECK(glEnable(GL_DEPTH_TEST));
    GL_CHECK(glEnable(GL_BLEND));
    /* Should do src * (src alpha) + dest * (1-src alpha). */
    GL_CHECK(glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

	
	/* Initialize FBO texture. */
	GL_CHECK(glGenTextures(2, g_FBOTex));
	for (i=0; i<2; i++) {
	    GL_CHECK(glBindTexture(GL_TEXTURE_2D, g_FBOTex[i]));
	    /* Set filtering. */
	    GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
	    GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
	    if (i==0) {
	    	GL_CHECK(glTexImage2D(GL_TEXTURE_2D, 0, g_pixtype, g_width, g_height, 0, g_pixtype, g_pixfmt, NULL));
			GL_CHECK(glPixelStorei(GL_UNPACK_ALIGNMENT, g_bpp));
		} else {
			GL_CHECK(glTexImage2D(GL_TEXTURE_2D, 0, g_pixtype, g_width, g_height, 0, g_pixtype, g_pixfmt, NULL));
			GL_CHECK(glPixelStorei(GL_UNPACK_ALIGNMENT, g_bpp));
		}
	    
	    GL_CHECK(glGenerateMipmap(GL_TEXTURE_2D));
	    GL_CHECK(glBindTexture(GL_TEXTURE_2D, 0));
		
		glViewport(160, 0, 960, 720);
		//printf("%s %d - %d %d %d %d\n", __FUNCTION__, __LINE__, g_drm_mode->hdisplay, g_drm_mode->vdisplay, g_width, g_height);
	}
	 
    return true;
}

GLuint compile_shader(unsigned type, unsigned count, const char **strings) {
    GLuint shader = glCreateShader(type);
    GL_CHECK(glShaderSource(shader, count, strings, NULL));
    GL_CHECK(glCompileShader(shader));

    GLint status;
    GL_CHECK(glGetShaderiv(shader, GL_COMPILE_STATUS, &status));

    if (status == GL_FALSE) {
        char buffer[4096];
        glGetShaderInfoLog(shader, sizeof(buffer), NULL, buffer);
        printf("Failed to compile %s shader: %s", type == GL_VERTEX_SHADER ? "vertex" : "fragment", buffer);
    }

    return shader;
}

void init_shaders(const char *frag) {
    GLuint vshader = compile_shader(GL_VERTEX_SHADER, 1, &g_vshader_src);
    GLuint fshader = compile_shader(GL_FRAGMENT_SHADER, 1, &frag);
    GLuint program = glCreateProgram();

    GL_CHECK(glAttachShader(program, vshader));
    GL_CHECK(glAttachShader(program, fshader));
    GL_CHECK(glLinkProgram(program));

    GL_CHECK(glDeleteShader(vshader));
    GL_CHECK(glDeleteShader(fshader));

    GL_CHECK(glValidateProgram(program));

	GL_CHECK(glBindAttribLocation(program, 0, "i_pos"));

    GLint status;
    GL_CHECK(glGetProgramiv(program, GL_LINK_STATUS, &status));

    if(status == GL_FALSE) {
        char buffer[4096];
        glGetProgramInfoLog(program, sizeof(buffer), NULL, buffer);
        printf("Failed to link shader program: %s", buffer);
    }

    g_shader.program = program;
    g_shader.u_tex   = glGetUniformLocation(program, "u_tex");
    g_shader.u_tex2   = glGetUniformLocation(program, "u_tex2");

    GL_CHECK(glUseProgram(g_shader.program));

    GL_CHECK(glUseProgram(0));
}

void init_vertex_data() {
	GL_CHECK(glGenBuffers(1, &glsl_buffers));
	GL_CHECK(glBindBuffer(GL_ARRAY_BUFFER, glsl_buffers));

	GLfloat vertex[16] = {
	  /*  x  y  s  t */
		-1, 1, 0, 0,
		-1, -1, 0, 1,
		1, 1, 1, 0,
		1, -1, 1, 1
	};


	GL_CHECK(glBufferData(
		GL_ARRAY_BUFFER, 16*sizeof(GLfloat), vertex, GL_STATIC_DRAW
	));
    GL_CHECK(glBindBuffer(GL_ARRAY_BUFFER, 0));
}

void renderFrame(void *data, GLuint width, GLuint height, GLuint pixfmt, GLuint pixtype, GLuint pitch)
{
	GL_CHECK(glClear( GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT ));
	GL_CHECK(glClearColor(0.0f, 0.0f, 0.0f, 1.0f));


	GL_CHECK(glActiveTexture(GL_TEXTURE0));

    GL_CHECK(glBindTexture(GL_TEXTURE_2D, g_FBOTex[0]));
   
	printf("g_width %d, g_height %d\n", g_width, g_height);
	printf("width %d, height %d, pixtype %d, pixfmt %d\n", width, height, pixtype, pixfmt);
	GL_CHECK(glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, pixtype, pixfmt, data));

	GL_CHECK(glUseProgram(g_shader.program));
    
	GL_CHECK(glEnableVertexAttribArray(0));
	GL_CHECK(glBindBuffer(GL_ARRAY_BUFFER, glsl_buffers));
	GL_CHECK(glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(GLfloat)*4, (uint8_t *) 0));

	GL_CHECK(glDrawArrays(GL_TRIANGLE_STRIP, 0, 4));
	GL_CHECK(glUniform1i(g_shader.u_tex, 0));
    GL_CHECK(glUniform1i(g_shader.u_tex2, 1));
	GL_CHECK(glUseProgram(0));
}

void draw(void *data, GLuint width, GLuint height, GLuint pitch)
{
	renderFrame(data, width, height, g_pixfmt, g_pixtype, pitch);
	swapBuffers();
}

int setupGraphics(int width, int height)
{
	g_width = width;
	g_height = height;

	g_pixfmt  = GL_UNSIGNED_BYTE;
	g_pixtype = GL_BGRA_EXT;
	g_bpp = sizeof(uint32_t);

	initVideo();
	initGL();
	init_shaders(g_fshader_src);
	init_vertex_data();

	return 0;
}

int main(int argc, char *argv[]) {

	setupGraphics(1280, 720);
	read_png_file("./testpattern-hd-720.png");
	process_png_file();
	draw(buffer1, 1280, 720, 1280*4);
    
	while (1);

    return EXIT_SUCCESS;
}
