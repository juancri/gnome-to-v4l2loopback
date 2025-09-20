#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <locale.h>
#include <signal.h>
#include <linux/videodev2.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/props.h>
#include <spa/param/buffers.h>
#include <spa/buffer/buffer.h>
#include <spa/buffer/alloc.h>
#include <spa/pod/builder.h>
#include <spa/pod/parser.h>
#include <libyuv.h>
#include <drm/drm_fourcc.h>
#include <spa/param/video/format.h>
#include "portal.h"
#include "gl_handler.h"

#define DEFAULT_V4L2_DEVICE "/dev/video0"

// Global debug flag
static bool debug_enabled = false;

// Global app data for signal handling
static struct app_data *global_app_data = NULL;

#define DEBUG_PRINT(...) do { \
    if (debug_enabled) { \
        printf(__VA_ARGS__); \
    } \
} while(0)

struct app_data {
    struct pw_main_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_stream *stream;
    struct spa_hook stream_listener;
    PortalSession *portal_session;
    int v4l2_fd;
    uint32_t width;
    uint32_t height;
    uint32_t stride;  // Stride in bytes (may be larger than width * bytes_per_pixel due to padding)
    uint32_t node_id;
    uint32_t spa_format;
    uint32_t v4l2_format;
    bool portal_ready;
    bool stream_ready;
    bool format_set;
    bool color_bars_mode;
    int frame_skip_count;
    uint8_t *conversion_buffer;
    size_t conversion_buffer_size;
    gl_context *gl_ctx;  // OpenGL context for DMA buffer handling
    uint8_t *gl_buffer;  // Buffer for OpenGL readback
    size_t gl_buffer_size;
};

// Signal handler for graceful shutdown
static void signal_handler(int sig) {
    printf("\nReceived signal %d, shutting down gracefully...\n", sig);

    if (global_app_data) {
        if (global_app_data->loop) {
            pw_main_loop_quit(global_app_data->loop);
        }

        if (global_app_data->portal_session) {
            portal_quit_main_loop(global_app_data->portal_session);
        }
    }
}

// Session closed callback
static void on_portal_session_closed(PortalSession *session, void *user_data) {
    struct app_data *data = (struct app_data*)user_data;
    (void)session; // Mark parameter as intentionally unused

    DEBUG_PRINT("DEBUG: on_portal_session_closed callback invoked\n");
    printf("Screen sharing stopped from GNOME UI, shutting down...\n");

    if (data->loop) {
        DEBUG_PRINT("DEBUG: Quitting PipeWire main loop\n");
        pw_main_loop_quit(data->loop);
    } else {
        DEBUG_PRINT("DEBUG: No PipeWire loop to quit\n");
    }
}

// Helper function to copy frame data line by line, removing stride padding
static void copy_frame_data_with_stride(uint8_t *dst, const uint8_t *src,
                                        uint32_t width, uint32_t height,
                                        uint32_t src_stride, uint32_t dst_stride) {
    uint32_t copy_width = width < dst_stride ? width : dst_stride;

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *src_line = src + (y * src_stride);
        uint8_t *dst_line = dst + (y * dst_stride);
        memcpy(dst_line, src_line, copy_width);
    }
}

// Helper to create a packed buffer from strided data
static uint8_t* create_packed_buffer(const uint8_t *src, uint32_t width, uint32_t height,
                                     uint32_t src_stride, uint32_t bytes_per_pixel) {
    uint32_t packed_stride = width * bytes_per_pixel;
    uint32_t packed_size = packed_stride * height;
    uint8_t *packed = malloc(packed_size);

    if (!packed) {
        printf("ERROR: Failed to allocate packed buffer\n");
        return NULL;
    }

    // Copy line by line to remove padding
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *src_line = src + (y * src_stride);
        uint8_t *dst_line = packed + (y * packed_stride);
        memcpy(dst_line, src_line, packed_stride);
    }

    return packed;
}

static uint32_t spa_to_v4l2_format(uint32_t spa_format) {
    switch (spa_format) {
        case 7: // SPA_VIDEO_FORMAT_RGBx
            return V4L2_PIX_FMT_RGB32;
        case 8: // SPA_VIDEO_FORMAT_BGRx
            return V4L2_PIX_FMT_BGR32;
        case 9: // SPA_VIDEO_FORMAT_xRGB
            return V4L2_PIX_FMT_XRGB32;
        case 10: // SPA_VIDEO_FORMAT_xBGR
            return V4L2_PIX_FMT_XBGR32;
        case 11: // SPA_VIDEO_FORMAT_RGBA
            return V4L2_PIX_FMT_RGBA32;
        case 12: // SPA_VIDEO_FORMAT_BGRA
            return V4L2_PIX_FMT_BGRA32;
        case 13: // SPA_VIDEO_FORMAT_ARGB
            return V4L2_PIX_FMT_ARGB32;
        case 14: // SPA_VIDEO_FORMAT_ABGR
            return V4L2_PIX_FMT_ABGR32;
        case 15: // SPA_VIDEO_FORMAT_RGB
            return V4L2_PIX_FMT_RGB24;
        case 16: // SPA_VIDEO_FORMAT_BGR
            return V4L2_PIX_FMT_BGR24;
        default:
            printf("WARNING: Unsupported SPA format %u, defaulting to RGB24\n", spa_format);
            return V4L2_PIX_FMT_RGB24;
    }
}

static void generate_color_bars_yuyv(uint8_t *dst, int width, int height) {
    // Generate SMPTE color bars in YUYV format
    // Colors: White, Yellow, Cyan, Green, Magenta, Red, Blue, Black
    uint8_t colors[8][4] = {
        {235, 128, 235, 128}, // White   (Y, U, Y, V)
        {210, 16,  210, 146}, // Yellow
        {170, 166, 170, 16},  // Cyan
        {145, 54,  145, 34},  // Green
        {106, 202, 106, 222}, // Magenta
        {81,  90,  81,  240},  // Red
        {41,  240, 41,  110},  // Blue
        {16,  128, 16,  128}   // Black
    };

    int bar_width = width / 8;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x += 2) {
            int bar = x / bar_width;
            if (bar >= 8) bar = 7;

            int dst_idx = (y * width + x) * 2;
            dst[dst_idx + 0] = colors[bar][0]; // Y0
            dst[dst_idx + 1] = colors[bar][1]; // U
            dst[dst_idx + 2] = colors[bar][2]; // Y1
            dst[dst_idx + 3] = colors[bar][3]; // V
        }
    }
}

static void generate_color_bars_xrgb32(uint8_t *dst, int width, int height) {
    // Generate SMPTE color bars in XRGB32 format
    // Colors: White, Yellow, Cyan, Green, Magenta, Red, Blue, Black
    uint32_t colors[8] = {
        0xFFFFFFFF, // White
        0xFFFFFF00, // Yellow
        0xFF00FFFF, // Cyan
        0xFF00FF00, // Green
        0xFFFF00FF, // Magenta
        0xFFFF0000, // Red
        0xFF0000FF, // Blue
        0xFF000000  // Black
    };

    int bar_width = width / 8;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int bar = x / bar_width;
            if (bar >= 8) bar = 7;

            uint32_t *pixel = (uint32_t*)(dst + (y * width + x) * 4);
            *pixel = colors[bar];
        }
    }
}

static void convert_rgb24_to_yuyv(const uint8_t *src, uint8_t *dst, int width, int height, uint32_t src_stride) {
    // RGB24 format: [R][G][B]
    // Need to convert to ARGB format first (add alpha), then to YUY2

    uint8_t *temp_argb = malloc(width * height * 4);
    if (!temp_argb) {
        printf("ERROR: Failed to allocate temporary buffer for RGB24 conversion\n");
        return;
    }

    // Convert RGB24 to ARGB
    int result1 = RGB24ToARGB(src, src_stride,          // source: use actual stride
                              temp_argb, width * 4,     // destination: stride = width * 4 bytes
                              width, height);

    if (result1 != 0) {
        printf("ERROR: RGB24ToARGB conversion failed with result %d\n", result1);
        free(temp_argb);
        return;
    }

    // Convert ARGB to YUY2
    int result2 = ARGBToYUY2(temp_argb, width * 4,     // source
                             dst, width * 2,            // destination
                             width, height);

    if (result2 != 0) {
        printf("ERROR: ARGBToYUY2 conversion failed with result %d\n", result2);
    }

    free(temp_argb);
}

static void convert_rgba32_to_yuyv(const uint8_t *src, uint8_t *dst, int width, int height, uint32_t src_stride) {
    // RGBA format: [R][G][B][A]
    // Need to convert to ARGB format first, then to YUY2

    uint8_t *temp_argb = malloc(width * height * 4);
    if (!temp_argb) {
        printf("ERROR: Failed to allocate temporary buffer for RGBA conversion\n");
        return;
    }

    // Convert RGBA to ARGB
    int result1 = RGBAToARGB(src, src_stride,           // source: use actual stride
                             temp_argb, width * 4,      // destination
                             width, height);

    if (result1 != 0) {
        printf("ERROR: RGBAToARGB conversion failed with result %d\n", result1);
        free(temp_argb);
        return;
    }

    // Convert ARGB to YUY2
    int result2 = ARGBToYUY2(temp_argb, width * 4,     // source
                             dst, width * 2,            // destination
                             width, height);

    if (result2 != 0) {
        printf("ERROR: ARGBToYUY2 conversion failed with result %d\n", result2);
    }

    free(temp_argb);
}

static void convert_bgrx_to_yuyv(const uint8_t *src, uint8_t *dst, int width, int height, uint32_t src_stride) {
    // BGRx format: [B][G][R][X] (X is padding, typically 0xFF)
    // Direct conversion to YUYV using ITU-R BT.601 coefficients
    // This avoids any issues with alpha channel interpretation

    for (int y = 0; y < height; y++) {
        const uint8_t *src_row = src + (y * src_stride);
        uint8_t *dst_row = dst + (y * width * 2);  // YUYV has 2 bytes per pixel

        for (int x = 0; x < width; x += 2) {
            // Process two pixels at a time for YUYV format
            // YUYV format: [Y0][U][Y1][V]

            // First pixel
            const uint8_t *px0 = src_row + (x * 4);
            uint8_t b0 = px0[0];
            uint8_t g0 = px0[1];
            uint8_t r0 = px0[2];
            // px0[3] is X (padding), ignore it

            // Second pixel (or repeat first if at edge)
            uint8_t b1, g1, r1;
            if (x + 1 < width) {
                const uint8_t *px1 = src_row + ((x + 1) * 4);
                b1 = px1[0];
                g1 = px1[1];
                r1 = px1[2];
            } else {
                b1 = b0;
                g1 = g0;
                r1 = r0;
            }

            // Convert to YUV using ITU-R BT.601 coefficients
            // Y = 0.299*R + 0.587*G + 0.114*B
            // U = -0.147*R - 0.289*G + 0.436*B + 128
            // V = 0.615*R - 0.515*G - 0.100*B + 128

            // Using integer math (scaled by 256 for precision)
            int y0 = (77 * r0 + 150 * g0 + 29 * b0) >> 8;
            int y1 = (77 * r1 + 150 * g1 + 29 * b1) >> 8;

            // Average the two pixels for chroma
            int ravg = (r0 + r1) / 2;
            int gavg = (g0 + g1) / 2;
            int bavg = (b0 + b1) / 2;

            int u = ((-38 * ravg - 74 * gavg + 112 * bavg) >> 8) + 128;
            int v = ((112 * ravg - 94 * gavg - 18 * bavg) >> 8) + 128;

            // Clamp values to valid range
            y0 = (y0 < 0) ? 0 : ((y0 > 255) ? 255 : y0);
            y1 = (y1 < 0) ? 0 : ((y1 > 255) ? 255 : y1);
            u = (u < 0) ? 0 : ((u > 255) ? 255 : u);
            v = (v < 0) ? 0 : ((v > 255) ? 255 : v);

            // Write YUYV
            *dst_row++ = y0;
            *dst_row++ = u;
            *dst_row++ = y1;
            *dst_row++ = v;
        }
    }
}

static void convert_xrgb_to_yuyv(const uint8_t *src, uint8_t *dst, int width, int height, uint32_t src_stride) {
    // xRGB format: [X][R][G][B] (X is padding)
    // This is actually ARGB layout with X as alpha, so we can use ARGBToYUY2 directly

    // Convert ARGB to YUY2 directly (xRGB has same memory layout as ARGB)
    int result = ARGBToYUY2(src, src_stride,           // source: use actual stride
                           dst, width * 2,              // destination
                           width, height);

    if (result != 0) {
        printf("ERROR: ARGBToYUY2 conversion failed with result %d\n", result);
    }
}

static void convert_bgra_to_yuyv(const uint8_t *src, uint8_t *dst, int width, int height, uint32_t src_stride) {
    // BGRA format: [B][G][R][A]
    // Use libyuv's BGRAToARGB then ARGBToYUY2

    uint8_t *temp_argb = malloc(width * height * 4);
    if (!temp_argb) {
        printf("ERROR: Failed to allocate temporary buffer for BGRA conversion\n");
        return;
    }

    // Convert BGRA to ARGB
    int result1 = BGRAToARGB(src, src_stride,           // source: use actual stride
                             temp_argb, width * 4,       // destination: stride = width * 4 bytes
                             width, height);

    if (result1 != 0) {
        printf("ERROR: BGRAToARGB conversion failed with result %d\n", result1);
        free(temp_argb);
        return;
    }

    // Convert ARGB to YUY2 (which is the same as YUYV)
    int result2 = ARGBToYUY2(temp_argb, width * 4,     // source
                            dst, width * 2,             // destination
                            width, height);

    if (result2 != 0) {
        printf("ERROR: ARGBToYUY2 conversion failed with result %d\n", result2);
    }

    free(temp_argb);
}

static void convert_argb_to_yuyv(const uint8_t *src, uint8_t *dst, int width, int height, uint32_t src_stride) {
    // ARGB format: [A][R][G][B]
    // Can use ARGBToYUY2 directly

    int result = ARGBToYUY2(src, src_stride,           // source: use actual stride
                           dst, width * 2,              // destination
                           width, height);

    if (result != 0) {
        printf("ERROR: ARGBToYUY2 conversion failed with result %d\n", result);
    }
}

static bool validate_frame_data(const uint8_t *data, int width, int height, uint32_t spa_format, uint32_t stride) {
    // Count non-black pixels to validate frame data
    int non_black_count = 0;
    int total_pixels_checked = 0;
    int bytes_per_pixel = (spa_format == 15 || spa_format == 16) ? 3 : 4;

    // Sample pixels across the image (up to 1000 pixels)
    int y_step = height > 100 ? height / 100 : 1;
    int x_step = width > 10 ? width / 10 : 1;

    for (int y = 0; y < height && total_pixels_checked < 1000; y += y_step) {
        for (int x = 0; x < width && total_pixels_checked < 1000; x += x_step) {
            const uint8_t *pixel = data + (y * stride + x * bytes_per_pixel);
            // Check if any color component is non-zero
            if (pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0) {
                non_black_count++;
            }
            total_pixels_checked++;
        }
    }

    double non_black_ratio = (double)non_black_count / total_pixels_checked;
    DEBUG_PRINT("DEBUG: Frame validation: %d/%d non-black pixels (%.1f%%)\n",
           non_black_count, total_pixels_checked, non_black_ratio * 100);

    // Consider frame valid if at least 1% of pixels are non-black
    return non_black_ratio > 0.01;
}




static void debug_pixel_data(const uint8_t *data, int width, int height, uint32_t spa_format, uint32_t stride) {
    DEBUG_PRINT("DEBUG: Analyzing pixel data for format %u\n", spa_format);
    int bytes_per_pixel = (spa_format == 15 || spa_format == 16) ? 3 : 4;

    // Print first few pixels in hex to understand the byte order
    DEBUG_PRINT("DEBUG: First 32 bytes (8 pixels for 32-bit formats): ");
    for (int i = 0; i < 32 && i < stride; i++) {
        DEBUG_PRINT("%02X ", data[i]);
        if ((i + 1) % 16 == 0) DEBUG_PRINT("\n                                                  ");
    }
    DEBUG_PRINT("\n");

    // Interpret first 4 pixels assuming different formats
    if (width >= 4 && height >= 1) {
        DEBUG_PRINT("DEBUG: First 4 pixels interpreted as:\n");
        for (int p = 0; p < 4; p++) {
            const uint8_t *pixel = data + (p * bytes_per_pixel);
            DEBUG_PRINT("DEBUG:   Pixel %d: [%02X %02X %02X %02X]", p, pixel[0], pixel[1], pixel[2], pixel[3]);
            DEBUG_PRINT(" -> as BGRx: B=%02X G=%02X R=%02X X=%02X", pixel[0], pixel[1], pixel[2], pixel[3]);
            DEBUG_PRINT(" -> as RGBx: R=%02X G=%02X B=%02X X=%02X\n", pixel[0], pixel[1], pixel[2], pixel[3]);
        }
    }

    // Check line boundaries to detect stride issues
    DEBUG_PRINT("DEBUG: Checking for stride issues:\n");
    DEBUG_PRINT("DEBUG: Stride: %u bytes, width * bytes_per_pixel: %d bytes\n", stride, width * bytes_per_pixel);
    if (height >= 2) {
        // Compare end of first line with start of second line
        DEBUG_PRINT("DEBUG: End of line 0 (last 4 pixels): ");
        for (int p = width-4; p < width && p >= 0; p++) {
            const uint8_t *pixel = data + (p * bytes_per_pixel);
            DEBUG_PRINT("[%02X %02X %02X %02X] ", pixel[0], pixel[1], pixel[2], pixel[3]);
        }
        DEBUG_PRINT("\n");

        DEBUG_PRINT("DEBUG: Start of line 1 (first 4 pixels): ");
        for (int p = 0; p < 4 && p < width; p++) {
            const uint8_t *pixel = data + (stride + p * bytes_per_pixel);
            DEBUG_PRINT("[%02X %02X %02X %02X] ", pixel[0], pixel[1], pixel[2], pixel[3]);
        }
        DEBUG_PRINT("\n");
    }

    // Check if data looks like it has reasonable RGB values
    int suspicious_count = 0;
    int pixels_to_check = (width * height < 1000) ? width * height : 1000;

    for (int i = 0; i < pixels_to_check; i++) {
        int y = i / width;
        int x = i % width;
        const uint8_t *pixel = data + (y * stride + x * bytes_per_pixel);
        // Check if RGB components look reasonable (any format, check all bytes)
        if (pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0) suspicious_count++;
    }
    DEBUG_PRINT("DEBUG: Suspicious (all-zero color) pixels in first 1000: %d\n", suspicious_count);

    // If we're getting all-black frames, something is wrong with the capture
    if (suspicious_count >= 950) { // Allow some tolerance
        DEBUG_PRINT("DEBUG: *** ALL-BLACK FRAME DETECTED ***\n");
        DEBUG_PRINT("DEBUG: This usually means:\n");
        DEBUG_PRINT("DEBUG: 1. Stream hasn't started yet (try waiting longer)\n");
        DEBUG_PRINT("DEBUG: 2. Wrong screen/window selected in portal\n");
        DEBUG_PRINT("DEBUG: 3. Display is off or screensaver is active\n");
        DEBUG_PRINT("DEBUG: 4. Buffer offset issue or wrong memory region\n");
    }
}

static void convert_xbgr_to_yuyv(const uint8_t *src, uint8_t *dst, int width, int height, uint32_t src_stride) {
    // xBGR format: [X][B][G][R] (X is padding)
    // This actually matches ABGR layout when we treat the padding as alpha
    // Use libyuv for reliable conversion

    // Allocate temporary buffer for ARGB conversion
    uint8_t *temp_argb = malloc(width * height * 4);
    if (!temp_argb) {
        printf("ERROR: Failed to allocate temporary buffer for conversion\n");
        return;
    }

    // Convert ABGR (our xBGR) to ARGB
    int result1 = ABGRToARGB(src, src_stride,           // source: use actual stride
                             temp_argb, width * 4,      // destination: stride = width * 4 bytes
                             width, height);

    if (result1 != 0) {
        printf("ERROR: ABGRToARGB conversion failed with result %d\n", result1);
        free(temp_argb);
        return;
    }

    // Convert ARGB to YUY2 (which is the same as YUYV)
    int result2 = ARGBToYUY2(temp_argb, width * 4,     // source: stride = width * 4 bytes
                             dst, width * 2,            // destination: stride = width * 2 bytes
                             width, height);

    if (result2 != 0) {
        printf("ERROR: ARGBToYUY2 conversion failed with result %d\n", result2);
    }

    free(temp_argb);
}

static void on_stream_param_changed(void *userdata, uint32_t id,
                                   const struct spa_pod *param) {
    struct app_data *data = userdata;

    printf("Stream param changed: id=%u\n", id);

    if (param == NULL) {
        DEBUG_PRINT("DEBUG: param is NULL for id=%u\n", id);
        return;
    }

    // Debug: dump the param pod
    if (id == SPA_PARAM_EnumFormat || id == SPA_PARAM_Format) {
        struct spa_pod_parser parser;
        const struct spa_pod_prop *prop;

        spa_pod_parser_pod(&parser, param);
        DEBUG_PRINT("DEBUG: Param type=0x%x, size=%zu\n",
               SPA_POD_TYPE(param), (size_t)SPA_POD_SIZE(param));

        // Try to parse as object
        if (SPA_POD_TYPE(param) == SPA_TYPE_Object) {
            struct spa_pod_frame frame;
            if (spa_pod_parser_push_object(&parser, &frame,
                                          SPA_TYPE_OBJECT_Format, NULL) == 0) {
                while (true) {
                    uint32_t key;
                    const struct spa_pod *value = NULL;
                    if (spa_pod_parser_get(&parser,
                                          SPA_POD_Id(&key),
                                          SPA_POD_Pod(&value),
                                          NULL) < 0)
                        break;

                    DEBUG_PRINT("DEBUG: Property key=0x%x\n", key);

                    if (key == SPA_FORMAT_VIDEO_format && value) {
                        // Extract format values
                        if (SPA_POD_TYPE(value) == SPA_TYPE_Id) {
                            uint32_t format = SPA_POD_VALUE(struct spa_pod_id, value);
                            DEBUG_PRINT("DEBUG: Single format offered: %u\n", format);
                        } else if (SPA_POD_TYPE(value) == SPA_TYPE_Choice) {
                            const struct spa_pod_choice *choice =
                                (const struct spa_pod_choice *)value;
                            const uint32_t *vals = (const uint32_t *)SPA_POD_CHOICE_VALUES(choice);
                            uint32_t n_vals = SPA_POD_CHOICE_N_VALUES(choice);
                            DEBUG_PRINT("DEBUG: Choice of %u formats offered:\n", n_vals);
                            for (uint32_t i = 0; i < n_vals; i++) {
                                DEBUG_PRINT("DEBUG:   Format[%u]: %u\n", i, vals[i]);
                            }
                        }
                    }
                }
                spa_pod_parser_pop(&parser, &frame);
            }
        }
    }

    // Handle buffer parameter negotiation
    if (id == SPA_PARAM_Buffers) {
        printf("Buffer parameters negotiation received\n");
        // Accept the buffer configuration offered by the portal
        // We support both DMA buffers (via OpenGL) and shared memory buffers
        uint32_t n_buffers;

        if (spa_pod_fixate((struct spa_pod *)param) == 0) {
            if (spa_pod_parse_object(param,
                SPA_TYPE_OBJECT_ParamBuffers, NULL,
                SPA_PARAM_BUFFERS_buffers, SPA_POD_OPT_Int(&n_buffers)) == 0) {
                DEBUG_PRINT("Buffer negotiation: %u buffers requested\n", n_buffers);
            }
        }

        // Accept whatever the portal offers
        return;
    }

    if (id != SPA_PARAM_Format)
        return;

    struct spa_video_info_raw info;
    if (spa_format_video_raw_parse(param, &info) < 0) {
        printf("Failed to parse video format\n");
        return;
    }

    const char* format_name = "UNKNOWN";
    switch (info.format) {
        case 7: format_name = "RGBx"; break; // SPA_VIDEO_FORMAT_RGBx
        case 8: format_name = "BGRx"; break; // SPA_VIDEO_FORMAT_BGRx
        case 9: format_name = "xRGB"; break; // SPA_VIDEO_FORMAT_xRGB
        case 10: format_name = "xBGR"; break; // SPA_VIDEO_FORMAT_xBGR
        case 11: format_name = "RGBA"; break; // SPA_VIDEO_FORMAT_RGBA
        case 12: format_name = "BGRA"; break; // SPA_VIDEO_FORMAT_BGRA
        case 13: format_name = "ARGB"; break; // SPA_VIDEO_FORMAT_ARGB
        case 14: format_name = "ABGR"; break; // SPA_VIDEO_FORMAT_ABGR
        case 15: format_name = "RGB"; break; // SPA_VIDEO_FORMAT_RGB
        case 16: format_name = "BGR"; break; // SPA_VIDEO_FORMAT_BGR
        default: format_name = "UNKNOWN"; break;
    }
    printf("Stream format negotiated: %ux%u, format=%u (%s)\n",
           info.size.width, info.size.height, info.format, format_name);

    // Check if dimensions have changed
    bool dimensions_changed = (data->width != info.size.width || data->height != info.size.height);

    // Update our stored dimensions and format
    data->width = info.size.width;
    data->height = info.size.height;
    data->spa_format = info.format;
    data->v4l2_format = spa_to_v4l2_format(info.format);

    // Calculate stride - for 32-bit formats it's typically aligned
    // For now, assume no padding (will be corrected in on_stream_process if needed)
    int bytes_per_pixel = (info.format == 15 || info.format == 16) ? 3 : 4; // RGB/BGR are 24-bit
    data->stride = data->width * bytes_per_pixel;

    printf("Initial stride estimate: %u bytes (width * bytes_per_pixel)\n", data->stride);

    // Update V4L2 device format if this is the first time we're setting it or dimensions changed
    if ((!data->format_set || dimensions_changed) && data->v4l2_fd >= 0) {
        struct v4l2_format fmt = {0};
        fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        fmt.fmt.pix.width = data->width;
        fmt.fmt.pix.height = data->height;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;

        // Always use YUYV format for consistency (same as color bars)
        uint32_t target_format = V4L2_PIX_FMT_YUYV;
        size_t bytes_per_pixel = 2;

        fmt.fmt.pix.pixelformat = target_format;
        fmt.fmt.pix.bytesperline = data->width * bytes_per_pixel;
        fmt.fmt.pix.sizeimage = data->width * data->height * bytes_per_pixel;

        if (ioctl(data->v4l2_fd, VIDIOC_S_FMT, &fmt) < 0) {
            perror("Failed to update V4L2 format");
            return;
        }

        // Store the actual format that was set
        data->v4l2_format = fmt.fmt.pix.pixelformat;

        printf("V4L2 format updated: %dx%d, YUYV\n", data->width, data->height);
        data->format_set = true;

        // Reset frame skip counter when format changes
        data->frame_skip_count = 0;

        // Allocate conversion buffer
        data->conversion_buffer_size = data->width * data->height * bytes_per_pixel;
        data->conversion_buffer = realloc(data->conversion_buffer, data->conversion_buffer_size);
        if (!data->conversion_buffer) {
            fprintf(stderr, "Failed to allocate conversion buffer\n");
        }
        DEBUG_PRINT("Conversion buffer allocated: %zu bytes\n", data->conversion_buffer_size);
    }
}

static void on_stream_state_changed(void *userdata, enum pw_stream_state old,
                                   enum pw_stream_state state, const char *error) {
    struct app_data *data = userdata;

    printf("Stream state changed: %s -> %s\n",
           pw_stream_state_as_string(old),
           pw_stream_state_as_string(state));

    if (error) {
        printf("Stream error: %s\n", error);

        // Handle format negotiation errors
        if (strstr(error, "no more input formats") != NULL) {
            printf("ERROR: Format negotiation failed. The portal may be offering formats we don't support.\n");
            printf("This can happen when:\n");
            printf("  1. The screen capture source uses an incompatible pixel format\n");
            printf("  2. Buffer type negotiation failed\n");
            printf("  3. The compositor is using hardware-specific formats\n");
            printf("Please try selecting a different monitor or window in the portal dialog.\n");
        }
    }

    // Mark stream as ready when in streaming state
    if (state == PW_STREAM_STATE_STREAMING) {
        data->stream_ready = true;
        printf("Stream is now ready for processing\n");
    }
}

static void on_stream_process(void *userdata) {
    struct app_data *data = userdata;
    struct pw_buffer *b;
    struct spa_buffer *buf;
    struct spa_data *d;
    void *frame_data = NULL;
    static int write_error_count = 0;

    if ((b = pw_stream_dequeue_buffer(data->stream)) == NULL) {
        DEBUG_PRINT("DEBUG: No buffer available\n");
        return;
    }

    buf = b->buffer;

    // Debug: Check for multiple data planes
    DEBUG_PRINT("DEBUG: Buffer has %u data planes (n_datas)\n", buf->n_datas);
    if (buf->n_datas > 1) {
        DEBUG_PRINT("DEBUG: WARNING: Multiple data planes detected! This might indicate tiled or planar format.\n");
        for (uint32_t i = 0; i < buf->n_datas; i++) {
            DEBUG_PRINT("DEBUG: Data plane %u: type=%d, fd=%ld, maxsize=%u\n",
                   i, buf->datas[i].type, (long)buf->datas[i].fd, buf->datas[i].maxsize);
        }
    }

    d = &buf->datas[0];

    void *mapped_data = NULL;

    if (d->type == SPA_DATA_MemFd || d->type == SPA_DATA_DmaBuf) {
        DEBUG_PRINT("DEBUG: Mapping buffer: fd=%ld, maxsize=%u, mapoffset=%u, chunk offset=%u\n",
               (long)d->fd, d->maxsize, d->mapoffset, d->chunk->offset);

        // Handle DMA buffer tiling issues
        if (d->type == SPA_DATA_DmaBuf) {
            DEBUG_PRINT("DEBUG: DMA buffer detected. Checking if GPU processing is needed...\n");

            // Try to use OpenGL to handle the DMA buffer if available
            if (data->gl_ctx && gl_has_dma_buf_import_support(data->gl_ctx)) {
                DEBUG_PRINT("DEBUG: Using OpenGL to import DMA buffer\n");

                // Ensure GL buffer is allocated
                size_t required_size = data->width * data->height * 4; // RGBA
                if (!data->gl_buffer || data->gl_buffer_size < required_size) {
                    data->gl_buffer = realloc(data->gl_buffer, required_size);
                    data->gl_buffer_size = required_size;
                    if (!data->gl_buffer) {
                        DEBUG_PRINT("ERROR: Failed to allocate GL buffer\n");
                        goto done;
                    }
                }

                // Import DMA buffer and read it back as linear RGBA
                uint32_t stride = d->chunk->stride > 0 ? d->chunk->stride : data->width * 4;
                uint32_t fourcc = DRM_FORMAT_XRGB8888; // Default format, adjust based on spa_format

                // Map SPA format to DRM fourcc
                switch (data->spa_format) {
                    case 7: // SPA_VIDEO_FORMAT_RGBx
                        fourcc = DRM_FORMAT_RGBX8888;
                        break;
                    case 8: // SPA_VIDEO_FORMAT_BGRx
                        fourcc = DRM_FORMAT_BGRX8888;
                        break;
                    case 9: // SPA_VIDEO_FORMAT_xRGB
                        fourcc = DRM_FORMAT_XRGB8888;
                        break;
                    case 10: // SPA_VIDEO_FORMAT_xBGR
                        fourcc = DRM_FORMAT_XBGR8888;
                        break;
                    case 11: // SPA_VIDEO_FORMAT_RGBA
                        fourcc = DRM_FORMAT_RGBA8888;
                        break;
                    case 12: // SPA_VIDEO_FORMAT_BGRA
                        fourcc = DRM_FORMAT_BGRA8888;
                        break;
                    case 13: // SPA_VIDEO_FORMAT_ARGB
                        fourcc = DRM_FORMAT_ARGB8888;
                        break;
                    case 14: // SPA_VIDEO_FORMAT_ABGR
                        fourcc = DRM_FORMAT_ABGR8888;
                        break;
                    default:
                        DEBUG_PRINT("DEBUG: Unknown SPA format %u, using XRGB8888\n", data->spa_format);
                        break;
                }

                if (gl_import_dma_buffer(data->gl_ctx, d->fd, data->width, data->height,
                                         stride, d->mapoffset, fourcc,
                                         data->gl_buffer, data->gl_buffer_size)) {
                    // Success! Use the GL buffer as frame data
                    frame_data = data->gl_buffer;
                    mapped_data = NULL; // No mmap needed
                    DEBUG_PRINT("DEBUG: Successfully imported DMA buffer via OpenGL\n");
                } else {
                    DEBUG_PRINT("ERROR: Failed to import DMA buffer via OpenGL, trying fallback...\n");
                    // Fall through to try mmap
                }
            }

            // If OpenGL import failed or is not available, try direct mapping
            if (!frame_data) {
                // Check if MAPPABLE flag is set
                if (!(d->flags & SPA_DATA_FLAG_MAPPABLE)) {
                    DEBUG_PRINT("ERROR: DMA buffer is not mappable and OpenGL import failed/unavailable\n");
                    DEBUG_PRINT("ERROR: Cannot process tiled DMA buffers. Skipping frame.\n");
                    goto done;  // Skip this frame entirely
                }

                DEBUG_PRINT("DEBUG: DMA buffer has MAPPABLE flag, attempting direct mmap...\n");
            }
        }

        // If we haven't successfully imported via OpenGL, try mmap
        if (!frame_data) {
            mapped_data = mmap(NULL, d->maxsize, PROT_READ, MAP_PRIVATE, d->fd, d->mapoffset);
            if (mapped_data == MAP_FAILED) {
                DEBUG_PRINT("DEBUG: Failed to map buffer\n");
                goto done;
            }
            DEBUG_PRINT("DEBUG: Buffer mapped successfully at %p\n", mapped_data);

            // Apply chunk offset to get actual frame data
            frame_data = (uint8_t*)mapped_data + d->chunk->offset;
            DEBUG_PRINT("DEBUG: Frame data at %p (mapped + %u offset)\n", frame_data, d->chunk->offset);
        }

    } else if (d->type == SPA_DATA_MemPtr) {
        // For memory pointers, chunk offset should already be applied
        frame_data = (uint8_t*)d->data + d->chunk->offset;
        DEBUG_PRINT("DEBUG: Using direct memory pointer: %p + offset %u = %p\n",
               d->data, d->chunk->offset, frame_data);
    } else {
        DEBUG_PRINT("DEBUG: Unsupported buffer type: %d\n", d->type);
        goto done;
    }

    if (frame_data == NULL) {
        DEBUG_PRINT("DEBUG: Frame data is NULL\n");
        goto cleanup_map;
    }

    // Get stride from the chunk structure if available
    uint32_t actual_stride;
    int bytes_per_pixel = (data->spa_format == 15 || data->spa_format == 16) ? 3 : 4;
    uint32_t min_stride = data->width * bytes_per_pixel;

    if (d->chunk->stride > 0) {
        actual_stride = d->chunk->stride;
        DEBUG_PRINT("DEBUG: Using stride from chunk: %u bytes (chunk->stride)\n", actual_stride);
    } else if (data->height > 0 && d->chunk->size > 0) {
        // Fallback: Calculate stride from chunk size (like PipeWire video-play.c does)
        actual_stride = d->chunk->size / data->height;
        DEBUG_PRINT("DEBUG: chunk->stride is 0, calculated stride from size/height: %u bytes\n", actual_stride);
    } else {
        actual_stride = min_stride;
        DEBUG_PRINT("DEBUG: Using minimum stride (width * bytes_per_pixel): %u bytes\n", actual_stride);
    }

    // Validate stride
    if (actual_stride < min_stride) {
        DEBUG_PRINT("DEBUG: WARNING: Stride %u is less than minimum %u, using minimum\n", actual_stride, min_stride);
        actual_stride = min_stride;
    }

    // Update stored stride if different
    if (data->stride != actual_stride) {
        DEBUG_PRINT("DEBUG: Updating stored stride from %u to %u\n", data->stride, actual_stride);
        data->stride = actual_stride;
    }

    DEBUG_PRINT("DEBUG: Processing frame: %u bytes, type=%d, spa_format=%u\n", d->chunk->size, d->type, data->spa_format);
    DEBUG_PRINT("DEBUG: Frame dimensions: %ux%u, stride: %d bytes (chunk->stride=%d)\n",
           data->width, data->height, actual_stride, d->chunk->stride);
    DEBUG_PRINT("DEBUG: Buffer maxsize: %u, chunk offset: %u, chunk size: %u\n",
           d->maxsize, d->chunk->offset, d->chunk->size);

    // Check if size matches expectations with stride
    size_t expected_size = (size_t)actual_stride * data->height;
    if (d->chunk->size != expected_size) {
        DEBUG_PRINT("DEBUG: *** SIZE MISMATCH *** chunk size %u != expected %zu (stride * height)\n", d->chunk->size, expected_size);
    } else {
        DEBUG_PRINT("DEBUG: Size matches expectations (stride * height)\n");
    }

    // Skip the first few frames as they may be uninitialized
    if (data->frame_skip_count < 5) {
        data->frame_skip_count++;
        DEBUG_PRINT("DEBUG: Skipping frame %d (waiting for stream to stabilize)\n", data->frame_skip_count);
        goto cleanup_map;
    }

    if (data->v4l2_fd >= 0) {
        // Ensure conversion buffer is allocated if needed
        if (!data->conversion_buffer && !data->color_bars_mode) {
            data->conversion_buffer_size = data->width * data->height * 2; // YUYV
            data->conversion_buffer = malloc(data->conversion_buffer_size);
            if (!data->conversion_buffer) {
                DEBUG_PRINT("DEBUG: Failed to allocate conversion buffer on-demand\n");
                goto cleanup_map;
            }
            DEBUG_PRINT("DEBUG: Allocated conversion buffer on-demand: %zu bytes\n", data->conversion_buffer_size);
        }

        if (data->color_bars_mode) {
            // Generate color bars test pattern
            generate_color_bars_yuyv(data->conversion_buffer, data->width, data->height);
            ssize_t written = write(data->v4l2_fd, data->conversion_buffer, data->conversion_buffer_size);
            if (written < 0) {
                perror("Failed to write to V4L2 device");
            } else {
                DEBUG_PRINT("DEBUG: Wrote %zd color bars bytes to V4L2 device\n", written);
            }
        } else {
            // Validate the frame data
            bool frame_valid = validate_frame_data((const uint8_t*)frame_data, data->width, data->height, data->spa_format, actual_stride);

            // Debug: Analyze the incoming pixel data
            static int debug_frame_count = 0;
            static time_t last_color_sample = 0;
            time_t current_time = time(NULL);

            if (debug_frame_count < 3) { // Only debug first 3 frames to avoid spam
                debug_pixel_data((const uint8_t*)frame_data, data->width, data->height, data->spa_format, actual_stride);
                debug_frame_count++;
            }


            // Skip invalid frames (all-black or mostly black)
            if (!frame_valid) {
                DEBUG_PRINT("DEBUG: Skipping invalid frame (mostly black pixels)\n");
                goto cleanup_map;
            }

            // Sample colors every second
            if (current_time - last_color_sample >= 1) {
                const uint8_t *sample_data = (const uint8_t*)frame_data;

                // Sample from center of frame
                int center_x = data->width / 2;
                int center_y = data->height / 2;
                int center_idx = center_y * actual_stride + center_x * 4;

                // Sample from a few different locations
                DEBUG_PRINT("COLOR SAMPLE: Center pixel [%02X %02X %02X %02X] -> BGRx(B=%02X G=%02X R=%02X)\n",
                       sample_data[center_idx], sample_data[center_idx+1],
                       sample_data[center_idx+2], sample_data[center_idx+3],
                       sample_data[center_idx], sample_data[center_idx+1], sample_data[center_idx+2]);

                // Sample from corners to see if there's variation
                int corners[4][2] = {{10, 10}, {data->width-10, 10}, {10, data->height-10}, {data->width-10, data->height-10}};
                for (int c = 0; c < 4; c++) {
                    int corner_idx = corners[c][1] * actual_stride + corners[c][0] * 4;
                    DEBUG_PRINT("COLOR SAMPLE: Corner %d [%02X %02X %02X %02X]\n", c,
                           sample_data[corner_idx], sample_data[corner_idx+1],
                           sample_data[corner_idx+2], sample_data[corner_idx+3]);
                }

                // Count different color patterns
                int black_pixels = 0, white_pixels = 0, red_pixels = 0, other_pixels = 0;
                for (int i = 0; i < 100 && i < data->width; i++) {  // Sample first line
                    const uint8_t *pixel = sample_data + (i * 4);
                    if (pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0) {
                        black_pixels++;
                    } else if (pixel[0] == 0xFF && pixel[1] == 0xFF && pixel[2] == 0xFF) {
                        white_pixels++;
                    } else if (pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0xFF) {
                        red_pixels++;
                    } else {
                        other_pixels++;
                    }
                }
                DEBUG_PRINT("COLOR SAMPLE: Black=%d White=%d Red=%d Other=%d (out of 100)\n",
                       black_pixels, white_pixels, red_pixels, other_pixels);

                last_color_sample = current_time;
            }

            // Check if we need to create a packed buffer (stride has padding)
            uint8_t *conversion_src = (uint8_t*)frame_data;
            uint8_t *packed_buffer = NULL;
            uint32_t conversion_stride = actual_stride;

            int local_bytes_per_pixel = (data->spa_format == 15 || data->spa_format == 16) ? 3 : 4;
            uint32_t expected_stride = data->width * local_bytes_per_pixel;

            if (actual_stride > expected_stride) {
                DEBUG_PRINT("DEBUG: Stride has padding (%u > %u), creating packed buffer\n", actual_stride, expected_stride);
                packed_buffer = create_packed_buffer((const uint8_t*)frame_data, data->width, data->height,
                                                    actual_stride, local_bytes_per_pixel);
                if (packed_buffer) {
                    conversion_src = packed_buffer;
                    conversion_stride = expected_stride;
                } else {
                    DEBUG_PRINT("ERROR: Failed to create packed buffer, using original with stride\n");
                }
            }

            // Always convert to YUYV format
            ssize_t written = -1;
            switch (data->spa_format) {
                case 7: // SPA_VIDEO_FORMAT_RGBx - [R][G][B][X]
                case 11: // SPA_VIDEO_FORMAT_RGBA - [R][G][B][A]
                    convert_rgba32_to_yuyv((const uint8_t*)conversion_src, data->conversion_buffer,
                                          data->width, data->height, conversion_stride);
                    written = write(data->v4l2_fd, data->conversion_buffer, data->conversion_buffer_size);
                    break;
                case 8: // SPA_VIDEO_FORMAT_BGRx - [B][G][R][X]
                    // Debug: Let's verify the actual format by checking sample pixels
                    {
                        const uint8_t *debug_src = (const uint8_t*)conversion_src;
                        DEBUG_PRINT("DEBUG BGRx: First pixel bytes: [%02X %02X %02X %02X]\n",
                               debug_src[0], debug_src[1], debug_src[2], debug_src[3]);
                        DEBUG_PRINT("DEBUG BGRx: Interpreting as BGRx: B=%d G=%d R=%d\n",
                               debug_src[0], debug_src[1], debug_src[2]);
                        // Check a non-black pixel if available
                        for (int i = 0; i < 100 && i < data->width; i++) {
                            const uint8_t *px = debug_src + i * 4;
                            if (px[0] != 0 || px[1] != 0 || px[2] != 0) {
                                DEBUG_PRINT("DEBUG BGRx: Non-black pixel at %d: [%02X %02X %02X %02X] -> RGB(%d,%d,%d)\n",
                                       i, px[0], px[1], px[2], px[3], px[2], px[1], px[0]);
                                break;
                            }
                        }
                    }
                    convert_bgrx_to_yuyv((const uint8_t*)conversion_src, data->conversion_buffer,
                                        data->width, data->height, conversion_stride);
                    // Debug: Check the YUV output
                    {
                        uint8_t *yuv = data->conversion_buffer;
                        DEBUG_PRINT("DEBUG YUV: First 8 bytes (2 pixels): [%02X %02X %02X %02X %02X %02X %02X %02X]\n",
                               yuv[0], yuv[1], yuv[2], yuv[3], yuv[4], yuv[5], yuv[6], yuv[7]);
                        DEBUG_PRINT("DEBUG YUV: Pixel 0: Y0=%d U=%d, Pixel 1: Y1=%d V=%d\n",
                               yuv[0], yuv[1], yuv[2], yuv[3]);
                    }
                    written = write(data->v4l2_fd, data->conversion_buffer, data->conversion_buffer_size);
                    break;
                case 9: // SPA_VIDEO_FORMAT_xRGB - [X][R][G][B]
                    convert_xrgb_to_yuyv((const uint8_t*)conversion_src, data->conversion_buffer,
                                        data->width, data->height, conversion_stride);
                    written = write(data->v4l2_fd, data->conversion_buffer, data->conversion_buffer_size);
                    break;
                case 10: // SPA_VIDEO_FORMAT_xBGR - [X][B][G][R]
                    convert_xbgr_to_yuyv((const uint8_t*)conversion_src, data->conversion_buffer,
                                        data->width, data->height, conversion_stride);
                    written = write(data->v4l2_fd, data->conversion_buffer, data->conversion_buffer_size);
                    break;
                case 12: // SPA_VIDEO_FORMAT_BGRA - [B][G][R][A]
                    convert_bgra_to_yuyv((const uint8_t*)conversion_src, data->conversion_buffer,
                                        data->width, data->height, conversion_stride);
                    written = write(data->v4l2_fd, data->conversion_buffer, data->conversion_buffer_size);
                    break;
                case 13: // SPA_VIDEO_FORMAT_ARGB - [A][R][G][B]
                    convert_argb_to_yuyv((const uint8_t*)conversion_src, data->conversion_buffer,
                                        data->width, data->height, conversion_stride);
                    written = write(data->v4l2_fd, data->conversion_buffer, data->conversion_buffer_size);
                    break;
                case 14: // SPA_VIDEO_FORMAT_ABGR - [A][B][G][R]
                    // ABGR uses the xBGR conversion (same as convert_xbgr_to_yuyv handles ABGR)
                    convert_xbgr_to_yuyv((const uint8_t*)conversion_src, data->conversion_buffer,
                                        data->width, data->height, conversion_stride);
                    written = write(data->v4l2_fd, data->conversion_buffer, data->conversion_buffer_size);
                    break;
                case 15: // SPA_VIDEO_FORMAT_RGB - [R][G][B]
                case 16: // SPA_VIDEO_FORMAT_BGR - [B][G][R]
                    convert_rgb24_to_yuyv((const uint8_t*)conversion_src, data->conversion_buffer,
                                         data->width, data->height, conversion_stride);
                    written = write(data->v4l2_fd, data->conversion_buffer, data->conversion_buffer_size);
                    break;
                default:
                    DEBUG_PRINT("DEBUG: Unsupported format %u for conversion\n", data->spa_format);
                    break;
            }

            // Clean up packed buffer if we created one
            if (packed_buffer) {
                free(packed_buffer);
            }

            if (written < 0) {
                perror("Failed to write to V4L2 device");
                write_error_count++;

                // Check if the portal session is still active
                if (data->portal_session && !data->portal_session->session_active) {
                    printf("Portal session is no longer active, stopping stream...\n");
                    if (data->loop) {
                        pw_main_loop_quit(data->loop);
                    }
                    goto cleanup_map;
                }

                // If we get multiple consecutive write errors, assume sharing has stopped
                if (write_error_count >= 5) {
                    printf("Multiple V4L2 write failures detected, assuming sharing stopped. Exiting...\n");
                    if (data->loop) {
                        pw_main_loop_quit(data->loop);
                    }
                    goto cleanup_map;
                }
            } else if (written > 0) {
                // Reset error count on successful write
                write_error_count = 0;
                DEBUG_PRINT("DEBUG: Wrote %zd converted bytes to V4L2 device (format %u)\n", written, data->spa_format);
            }
        }
    }

cleanup_map:
    if (mapped_data && (d->type == SPA_DATA_MemFd || d->type == SPA_DATA_DmaBuf)) {
        munmap(mapped_data, d->maxsize);
    }

done:
    pw_stream_queue_buffer(data->stream, b);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed,
    .param_changed = on_stream_param_changed,
    .process = on_stream_process,
};

static int setup_v4l2_device(const char *device) {
    int fd = open(device, O_RDWR);
    if (fd < 0) {
        perror("Failed to open V4L2 device");
        return -1;
    }

    printf("V4L2 device opened: %s\n", device);
    return fd;
}

// Portal callback functions
static void on_session_created(PortalSession *session, bool success, void *user_data);
static void on_sources_selected(PortalSession *session, bool success, void *user_data);
static void on_session_started(PortalSession *session, uint32_t node_id, int pipewire_fd, void *user_data);
static void on_pipewire_ready(PortalSession *session, uint32_t node_id, int pipewire_fd, void *user_data);

static int setup_pipewire_via_portal(struct app_data *data) {
    // Create portal session
    data->portal_session = portal_session_new();
    if (!data->portal_session) {
        fprintf(stderr, "Failed to create portal session\n");
        return -1;
    }

    // Register session closed callback
    portal_set_session_closed_callback(data->portal_session, on_portal_session_closed, data);

    printf("Starting portal-based screen capture...\n");
    printf("A dialog will appear asking you to select which monitor to capture.\n");

    // Start the portal workflow
    if (!portal_create_session(data->portal_session, on_session_created, data)) {
        fprintf(stderr, "Failed to create portal session\n");
        return -1;
    }

    return 0;
}

static int setup_pipewire_stream(struct app_data *data, int pipewire_fd, uint32_t node_id) {
    const struct spa_pod *params[1];
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    // Create PipeWire context and connect using the portal's file descriptor
    data->context = pw_context_new(pw_main_loop_get_loop(data->loop), NULL, 0);
    if (!data->context) {
        fprintf(stderr, "Failed to create PipeWire context\n");
        return -1;
    }

    // Connect to PipeWire using the file descriptor from the portal
    struct pw_properties *props = pw_properties_new(
        PW_KEY_REMOTE_NAME, "portal-screencast",
        NULL);

    data->core = pw_context_connect_fd(data->context, pipewire_fd, props, 0);
    if (!data->core) {
        fprintf(stderr, "Failed to connect to PipeWire via portal\n");
        return -1;
    }

    // Create stream
    data->stream = pw_stream_new(data->core, "gnome-screen-capture",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Video",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Screen",
            NULL));

    if (!data->stream) {
        fprintf(stderr, "Failed to create PipeWire stream\n");
        return -1;
    }

    pw_stream_add_listener(data->stream, &data->stream_listener, &stream_events, data);

    // Setup format parameters - no specific format constraints
    // Let PipeWire negotiate the format based on what the portal offers
    params[0] = spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw));

    // Connect to the specific node ID provided by the portal
    // Don't force buffer mapping since we support DMA buffers via OpenGL
    if (pw_stream_connect(data->stream,
                         PW_DIRECTION_INPUT,
                         node_id,
                         PW_STREAM_FLAG_AUTOCONNECT |
                         PW_STREAM_FLAG_RT_PROCESS,
                         params, 1) < 0) {
        fprintf(stderr, "Failed to connect PipeWire stream to node %u\n", node_id);
        return -1;
    }

    printf("PipeWire stream connected to portal node %u\n", node_id);
    data->stream_ready = true;
    return 0;
}

// Portal callback implementations
static void on_session_created(PortalSession *session, bool success, void *user_data) {
    struct app_data *data = (struct app_data*)user_data;

    if (!success) {
        fprintf(stderr, "Failed to create portal session\n");
        portal_quit_main_loop(session);
        return;
    }

    printf("Portal session created successfully\n");

    // Proceed to source selection
    if (!portal_select_sources(session, on_sources_selected, user_data)) {
        fprintf(stderr, "Failed to start source selection\n");
        portal_quit_main_loop(session);
    }
}

static void on_sources_selected(PortalSession *session, bool success, void *user_data) {
    struct app_data *data = (struct app_data*)user_data;

    if (!success) {
        fprintf(stderr, "Failed to select sources\n");
        portal_quit_main_loop(session);
        return;
    }

    printf("Sources selected successfully\n");

    // Start the session
    if (!portal_start_session(session, on_session_started, user_data)) {
        fprintf(stderr, "Failed to start session\n");
        portal_quit_main_loop(session);
    }
}

static void on_session_started(PortalSession *session, uint32_t node_id, int pipewire_fd, void *user_data) {
    struct app_data *data = (struct app_data*)user_data;

    printf("Session started with node ID: %u\n", node_id);
    data->node_id = node_id;

    // Open PipeWire remote to get file descriptor
    if (!portal_open_pipewire_remote(session, on_pipewire_ready, user_data)) {
        fprintf(stderr, "Failed to open PipeWire remote\n");
        portal_quit_main_loop(session);
    }
}

static void on_pipewire_ready(PortalSession *session, uint32_t node_id, int pipewire_fd, void *user_data) {
    struct app_data *data = (struct app_data*)user_data;

    printf("PipeWire remote ready with fd: %d\n", pipewire_fd);

    // Create PipeWire main loop now that portal is ready
    data->loop = pw_main_loop_new(NULL);
    if (!data->loop) {
        fprintf(stderr, "Failed to create PipeWire main loop\n");
        portal_quit_main_loop(session);
        return;
    }

    // Setup PipeWire stream with the portal's file descriptor
    if (setup_pipewire_stream(data, pipewire_fd, node_id) < 0) {
        fprintf(stderr, "Failed to setup PipeWire stream\n");
        portal_quit_main_loop(session);
        return;
    }

    data->portal_ready = true;
    printf("Portal setup complete. Screen capture is now active.\n");

    // Exit the portal main loop so we can switch to PipeWire main loop
    portal_quit_main_loop(session);
}

int main(int argc, char *argv[]) {
    struct app_data data = {0};
    const char *v4l2_device = DEFAULT_V4L2_DEVICE;

    // Set up global reference for signal handling
    global_app_data = &data;

    // Set up signal handlers for graceful shutdown
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("Failed to set up SIGINT handler");
    }
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("Failed to set up SIGTERM handler");
    }

    // Check for debug mode via environment variable
    if (getenv("DEBUG") || getenv("GNOME_V4L2_DEBUG")) {
        debug_enabled = true;
    }

    DEBUG_PRINT("DEBUG: Starting main function\n");
    DEBUG_PRINT("DEBUG: Initializing data structure\n");

    data.width = 0;  // Will be set by PipeWire stream
    data.height = 0; // Will be set by PipeWire stream
    data.stride = 0; // Will be set by PipeWire stream
    data.color_bars_mode = false;

    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--color-bars") == 0 || strcmp(argv[i], "-c") == 0) {
            data.color_bars_mode = true;
            printf("Color bars mode enabled\n");
        } else if (strcmp(argv[i], "--debug") == 0 || strcmp(argv[i], "-v") == 0) {
            debug_enabled = true;
            printf("Debug mode enabled\n");
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [options] [/dev/videoN]\n", argv[0]);
            printf("Options:\n");
            printf("  -c, --color-bars         Generate SMPTE color bars test pattern\n");
            printf("  -v, --debug              Enable debug logging\n");
            printf("  -h, --help               Show this help message\n");
            printf("\nIf no device is specified, %s is used by default.\n", DEFAULT_V4L2_DEVICE);
            printf("\nDebug mode can also be enabled by setting DEBUG=1 or GNOME_V4L2_DEBUG=1 environment variable.\n");
            return 0;
        } else if (argv[i][0] != '-') {
            v4l2_device = argv[i];
        } else {
            printf("Unknown option: %s\n", argv[i]);
            printf("Use --help for usage information.\n");
            return 1;
        }
    }

    printf("Starting GNOME to V4L2 loopback\n");
    printf("V4L2 device: %s\n", v4l2_device);

    // Initialize OpenGL context for DMA buffer handling
    printf("Initializing OpenGL/EGL context for DMA buffer support...\n");
    data.gl_ctx = gl_context_create();
    if (data.gl_ctx) {
        if (gl_has_dma_buf_import_support(data.gl_ctx)) {
            printf("OpenGL DMA buffer import support is available\n");
        } else {
            printf("Warning: OpenGL context created but DMA buffer import not supported\n");
            printf("Will fall back to direct memory mapping when possible\n");
        }
    } else {
        printf("Warning: Failed to create OpenGL context\n");
        printf("DMA buffer handling will be limited - may fail on tiled buffers\n");
    }

    if (data.color_bars_mode) {
        // For color bars mode, use default resolution
        data.width = 1280;
        data.height = 720;
        data.stride = data.width * 4;  // No padding for color bars
        printf("Resolution: %dx%d\n", data.width, data.height);
        printf("Mode: Color bars test pattern\n");
    } else {
        printf("Mode: Screen capture (resolution will be determined by PipeWire)\n");
    }

    data.v4l2_fd = setup_v4l2_device(v4l2_device);
    if (data.v4l2_fd < 0) {
        fprintf(stderr, "Failed to setup V4L2 device\n");
        goto cleanup;
    }

    if (data.color_bars_mode) {
        // First check device capabilities
        struct v4l2_capability cap;
        if (ioctl(data.v4l2_fd, VIDIOC_QUERYCAP, &cap) < 0) {
            perror("Failed to query device capabilities");
            goto cleanup;
        }

        printf("Device capabilities: 0x%x\n", cap.capabilities);
        printf("Device supports: %s%s%s\n",
               (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ? "CAPTURE " : "",
               (cap.capabilities & V4L2_CAP_VIDEO_OUTPUT) ? "OUTPUT " : "",
               (cap.capabilities & V4L2_CAP_READWRITE) ? "READWRITE" : "");

        // Set up V4L2 format for color bars - try both capture and output types
        struct v4l2_format fmt = {0};
        fmt.type = (cap.capabilities & V4L2_CAP_VIDEO_OUTPUT) ? V4L2_BUF_TYPE_VIDEO_OUTPUT : V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = data.width;
        fmt.fmt.pix.height = data.height;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
        fmt.fmt.pix.bytesperline = data.width * 2;
        fmt.fmt.pix.sizeimage = data.width * data.height * 2;

        if (ioctl(data.v4l2_fd, VIDIOC_S_FMT, &fmt) < 0) {
            // Try with XR24 format if YUYV fails
            fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_XRGB32;
            fmt.fmt.pix.bytesperline = data.width * 4;
            fmt.fmt.pix.sizeimage = data.width * data.height * 4;

            if (ioctl(data.v4l2_fd, VIDIOC_S_FMT, &fmt) < 0) {
                perror("Failed to set V4L2 format for color bars");
                printf("Trying to write to device without setting format...\n");
            } else {
                printf("V4L2 format set: %dx%d, XRGB32\n", data.width, data.height);
            }
        } else {
            printf("V4L2 format set: %dx%d, YUYV\n", data.width, data.height);
        }

        // Allocate buffer for color bars - determine size based on format
        bool using_yuyv = (fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV);
        data.conversion_buffer_size = using_yuyv ?
            data.width * data.height * 2 : data.width * data.height * 4;
        data.conversion_buffer = malloc(data.conversion_buffer_size);
        if (!data.conversion_buffer) {
            fprintf(stderr, "Failed to allocate color bars buffer\n");
            goto cleanup;
        }

        // Generate and write color bars continuously
        printf("Generating color bars... Press Ctrl+C to stop.\n");
        while (1) {
            if (using_yuyv) {
                generate_color_bars_yuyv(data.conversion_buffer, data.width, data.height);
            } else {
                generate_color_bars_xrgb32(data.conversion_buffer, data.width, data.height);
            }
            ssize_t written = write(data.v4l2_fd, data.conversion_buffer, data.conversion_buffer_size);
            if (written < 0) {
                perror("Failed to write color bars to V4L2 device");
                break;
            }
            usleep(33333); // ~30 FPS
        }
    } else {
        DEBUG_PRINT("DEBUG: Initializing PipeWire\n");
        pw_init(&argc, &argv);
        DEBUG_PRINT("DEBUG: PipeWire initialized\n");

        if (setup_pipewire_via_portal(&data) < 0) {
            fprintf(stderr, "Failed to setup PipeWire via portal\n");
            goto cleanup;
        }

        printf("Starting main loop...\n");
        // The portal setup will run its main loop until ready,
        // then we'll switch to the PipeWire main loop
        portal_run_main_loop(data.portal_session);

        if (data.portal_ready && data.loop) {
            printf("Portal ready, starting PipeWire main loop...\n");
            pw_main_loop_run(data.loop);
        }
    }

cleanup:
    // Clear global reference
    global_app_data = NULL;

    if (data.stream)
        pw_stream_destroy(data.stream);
    if (data.core)
        pw_core_disconnect(data.core);
    if (data.context)
        pw_context_destroy(data.context);
    if (data.portal_session)
        portal_session_free(data.portal_session);
    if (data.v4l2_fd >= 0)
        close(data.v4l2_fd);
    if (data.conversion_buffer)
        free(data.conversion_buffer);
    if (data.gl_buffer)
        free(data.gl_buffer);
    if (data.gl_ctx)
        gl_context_destroy(data.gl_ctx);
    if (data.loop)
        pw_main_loop_destroy(data.loop);

    if (!data.color_bars_mode) {
        pw_deinit();
    }

    printf("Application shutdown complete.\n");
    return 0;
}