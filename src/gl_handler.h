#ifndef GL_HANDLER_H
#define GL_HANDLER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Forward declaration to avoid circular dependencies
typedef struct gl_context gl_context;

// Initialize the OpenGL/EGL context
// Returns NULL if initialization fails (e.g., extensions not available)
gl_context* gl_context_create(void);

// Destroy the OpenGL/EGL context and free resources
void gl_context_destroy(gl_context *ctx);

// Import a DMA buffer and convert it to linear RGB data
// Parameters:
//   ctx: The GL context
//   dma_fd: File descriptor of the DMA buffer
//   width: Width of the buffer in pixels
//   height: Height of the buffer in pixels
//   stride: Stride of the buffer in bytes
//   offset: Offset in the DMA buffer
//   fourcc: DRM fourcc format code (e.g., DRM_FORMAT_XRGB8888)
//   out_buffer: Pre-allocated buffer to store the linear RGB data
//   out_buffer_size: Size of the output buffer
// Returns: true on success, false on failure
bool gl_import_dma_buffer(gl_context *ctx,
                          int dma_fd,
                          uint32_t width,
                          uint32_t height,
                          uint32_t stride,
                          uint32_t offset,
                          uint32_t fourcc,
                          uint8_t *out_buffer,
                          size_t out_buffer_size);

// Check if EGL DMA buffer import extension is available
bool gl_has_dma_buf_import_support(gl_context *ctx);

#endif // GL_HANDLER_H