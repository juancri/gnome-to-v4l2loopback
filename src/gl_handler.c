#include "gl_handler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm/drm_fourcc.h>

// EGL extension function pointers
typedef EGLImageKHR (*PFNEGLCREATEIMAGEKHRPROC)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
typedef EGLBoolean (*PFNEGLDESTROYIMAGEKHRPROC)(EGLDisplay, EGLImageKHR);
typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum, GLeglImageOES);

// GL context structure
struct gl_context {
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
    EGLConfig egl_config;

    // Extension functions
    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;

    // Framebuffer for rendering
    GLuint framebuffer;
    GLuint renderbuffer;

    // Check for extension support
    bool has_dma_buf_import;
};

static bool check_egl_extension(EGLDisplay display, const char *extension) {
    const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
    if (!extensions) {
        return false;
    }
    return strstr(extensions, extension) != NULL;
}

static bool check_gl_extension(const char *extension) {
    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
    if (!extensions) {
        return false;
    }
    return strstr(extensions, extension) != NULL;
}

gl_context* gl_context_create(void) {
    gl_context *ctx = calloc(1, sizeof(gl_context));
    if (!ctx) {
        fprintf(stderr, "Failed to allocate GL context\n");
        return NULL;
    }

    // Initialize EGL
    ctx->egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (ctx->egl_display == EGL_NO_DISPLAY) {
        fprintf(stderr, "Failed to get EGL display\n");
        free(ctx);
        return NULL;
    }

    EGLint major, minor;
    if (!eglInitialize(ctx->egl_display, &major, &minor)) {
        fprintf(stderr, "Failed to initialize EGL\n");
        free(ctx);
        return NULL;
    }

    printf("EGL version: %d.%d\n", major, minor);

    // Check for required extensions
    ctx->has_dma_buf_import = check_egl_extension(ctx->egl_display, "EGL_EXT_image_dma_buf_import");
    if (!ctx->has_dma_buf_import) {
        fprintf(stderr, "Warning: EGL_EXT_image_dma_buf_import not supported\n");
    }

    // Configure EGL
    static const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };

    EGLint num_configs;
    if (!eglChooseConfig(ctx->egl_display, config_attribs, &ctx->egl_config, 1, &num_configs) || num_configs == 0) {
        fprintf(stderr, "Failed to choose EGL config\n");
        eglTerminate(ctx->egl_display);
        free(ctx);
        return NULL;
    }

    // Create a pbuffer surface (1x1 pixel, we don't actually render to screen)
    static const EGLint pbuffer_attribs[] = {
        EGL_WIDTH, 1,
        EGL_HEIGHT, 1,
        EGL_NONE
    };

    ctx->egl_surface = eglCreatePbufferSurface(ctx->egl_display, ctx->egl_config, pbuffer_attribs);
    if (ctx->egl_surface == EGL_NO_SURFACE) {
        fprintf(stderr, "Failed to create EGL pbuffer surface\n");
        eglTerminate(ctx->egl_display);
        free(ctx);
        return NULL;
    }

    // Create OpenGL ES 2.0 context
    static const EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    ctx->egl_context = eglCreateContext(ctx->egl_display, ctx->egl_config, EGL_NO_CONTEXT, context_attribs);
    if (ctx->egl_context == EGL_NO_CONTEXT) {
        fprintf(stderr, "Failed to create EGL context\n");
        eglDestroySurface(ctx->egl_display, ctx->egl_surface);
        eglTerminate(ctx->egl_display);
        free(ctx);
        return NULL;
    }

    // Make context current
    if (!eglMakeCurrent(ctx->egl_display, ctx->egl_surface, ctx->egl_surface, ctx->egl_context)) {
        fprintf(stderr, "Failed to make EGL context current\n");
        eglDestroyContext(ctx->egl_display, ctx->egl_context);
        eglDestroySurface(ctx->egl_display, ctx->egl_surface);
        eglTerminate(ctx->egl_display);
        free(ctx);
        return NULL;
    }

    // Get extension functions
    ctx->eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    ctx->eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    ctx->glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    if (!ctx->eglCreateImageKHR || !ctx->eglDestroyImageKHR || !ctx->glEGLImageTargetTexture2DOES) {
        fprintf(stderr, "Failed to get required EGL/GL extension functions\n");
        eglMakeCurrent(ctx->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(ctx->egl_display, ctx->egl_context);
        eglDestroySurface(ctx->egl_display, ctx->egl_surface);
        eglTerminate(ctx->egl_display);
        free(ctx);
        return NULL;
    }

    // Check for GL_OES_EGL_image extension
    if (!check_gl_extension("GL_OES_EGL_image")) {
        fprintf(stderr, "Warning: GL_OES_EGL_image not supported\n");
    }

    // Create framebuffer for rendering
    glGenFramebuffers(1, &ctx->framebuffer);

    printf("OpenGL ES vendor: %s\n", glGetString(GL_VENDOR));
    printf("OpenGL ES renderer: %s\n", glGetString(GL_RENDERER));
    printf("OpenGL ES version: %s\n", glGetString(GL_VERSION));

    return ctx;
}

void gl_context_destroy(gl_context *ctx) {
    if (!ctx) {
        return;
    }

    // Make context current before destroying GL objects
    eglMakeCurrent(ctx->egl_display, ctx->egl_surface, ctx->egl_surface, ctx->egl_context);

    // Delete framebuffer
    if (ctx->framebuffer) {
        glDeleteFramebuffers(1, &ctx->framebuffer);
    }

    // Clean up EGL
    eglMakeCurrent(ctx->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(ctx->egl_display, ctx->egl_context);
    eglDestroySurface(ctx->egl_display, ctx->egl_surface);
    eglTerminate(ctx->egl_display);

    free(ctx);
}

bool gl_import_dma_buffer(gl_context *ctx,
                          int dma_fd,
                          uint32_t width,
                          uint32_t height,
                          uint32_t stride,
                          uint32_t offset,
                          uint32_t fourcc,
                          uint8_t *out_buffer,
                          size_t out_buffer_size) {
    if (!ctx || !ctx->has_dma_buf_import || dma_fd < 0 || !out_buffer) {
        return false;
    }

    // Make context current
    if (!eglMakeCurrent(ctx->egl_display, ctx->egl_surface, ctx->egl_surface, ctx->egl_context)) {
        fprintf(stderr, "Failed to make EGL context current\n");
        return false;
    }

    // Create EGLImage from DMA buffer
    EGLint attribs[] = {
        EGL_WIDTH, (EGLint)width,
        EGL_HEIGHT, (EGLint)height,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)fourcc,
        EGL_DMA_BUF_PLANE0_FD_EXT, dma_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride,
        EGL_NONE
    };

    EGLImageKHR egl_image = ctx->eglCreateImageKHR(ctx->egl_display,
                                                    EGL_NO_CONTEXT,
                                                    EGL_LINUX_DMA_BUF_EXT,
                                                    (EGLClientBuffer)NULL,
                                                    attribs);

    if (egl_image == EGL_NO_IMAGE_KHR) {
        EGLint error = eglGetError();
        fprintf(stderr, "Failed to create EGLImage from DMA buffer: 0x%x\n", error);
        return false;
    }

    // Create texture from EGLImage
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Bind EGLImage to texture
    ctx->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)egl_image);

    GLenum gl_error = glGetError();
    if (gl_error != GL_NO_ERROR) {
        fprintf(stderr, "Failed to bind EGLImage to texture: 0x%x\n", gl_error);
        glDeleteTextures(1, &texture);
        ctx->eglDestroyImageKHR(ctx->egl_display, egl_image);
        return false;
    }

    // Create a framebuffer and attach the texture
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

    GLenum fb_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fb_status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "Framebuffer incomplete: 0x%x\n", fb_status);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteTextures(1, &texture);
        ctx->eglDestroyImageKHR(ctx->egl_display, egl_image);
        return false;
    }

    // Set viewport to match texture size
    glViewport(0, 0, width, height);

    // Read pixels from framebuffer
    size_t expected_size = width * height * 4; // RGBA
    if (out_buffer_size < expected_size) {
        fprintf(stderr, "Output buffer too small: %zu < %zu\n", out_buffer_size, expected_size);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteTextures(1, &texture);
        ctx->eglDestroyImageKHR(ctx->egl_display, egl_image);
        return false;
    }

    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, out_buffer);

    gl_error = glGetError();
    if (gl_error != GL_NO_ERROR) {
        fprintf(stderr, "glReadPixels failed: 0x%x\n", gl_error);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteTextures(1, &texture);
        ctx->eglDestroyImageKHR(ctx->egl_display, egl_image);
        return false;
    }

    // Clean up
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteTextures(1, &texture);
    ctx->eglDestroyImageKHR(ctx->egl_display, egl_image);

    return true;
}

bool gl_has_dma_buf_import_support(gl_context *ctx) {
    return ctx && ctx->has_dma_buf_import;
}