#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <sys/select.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define close closesocket
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#else
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <strings.h>
#endif

#include "libretro.h"
#include "mad.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define VIS_SIZE 256
#define OUTPUT_SAMPLE_RATE 44100
#define AUDIO_FRAMES_PER_TICK 735
#define MAX_STATIONS 256
#define MAD_INPUT_BUF_SIZE 65536
#define MAD_BUFFER_GUARD 8
#define PCM_QUEUE_SIZE 32768
#define NUM_SPECTRUM_BARS 64

enum core_status_t {
   STATUS_IDLE = 0,
   STATUS_CONNECTING,
   STATUS_BUFFERING,
   STATUS_PLAYING,
   STATUS_ERROR,
   STATUS_EOF
};

typedef enum {
   PLAYBACK_RADIO = 0,
   PLAYBACK_FILE_MP3,
   PLAYBACK_FILE_WAV
} playback_mode_t;

typedef struct {
   const char *name;
   const char *url;
   float frequency;
} radio_station_t;

typedef struct {
   uint8_t *data;
   size_t size;
   size_t write_ptr;
   size_t read_ptr;
   pthread_mutex_t mutex;
} ring_buffer_t;

typedef struct {
   int16_t *buffer;
   size_t size;
   size_t write_ptr;
   size_t read_ptr;
   size_t count;
} pcm_fifo_t;

/* 8x8 bitmap font (ASCII 32 to 127) */
static const uint8_t font_8x8[96][8] = {
   {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 
   {0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00}, 
   {0x6C, 0x6C, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00}, 
   {0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00}, 
   {0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00}, 
   {0x00, 0x66, 0x30, 0x18, 0x0C, 0x06, 0x66, 0x00}, 
   {0x38, 0x6C, 0x38, 0x76, 0x6C, 0x6C, 0x3A, 0x00}, 
   {0x18, 0x18, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00}, 
   {0x0C, 0x18, 0x30, 0x30, 0x30, 0x18, 0x0C, 0x00}, 
   {0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x18, 0x30, 0x00}, 
   {0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00}, 
   {0x00, 0x18, 0x18, 0x7E, 0x18, 0x18, 0x00, 0x00}, 
   {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x30}, 
   {0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00}, 
   {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00}, 
   {0x00, 0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x00}, 
   {0x3C, 0x66, 0x6E, 0x7E, 0x76, 0x66, 0x3C, 0x00}, 
   {0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00}, 
   {0x3C, 0x66, 0x06, 0x0C, 0x30, 0x60, 0x7E, 0x00}, 
   {0x3C, 0x66, 0x06, 0x1C, 0x06, 0x66, 0x3C, 0x00}, 
   {0x06, 0x0E, 0x1E, 0x66, 0x7F, 0x06, 0x06, 0x00}, 
   {0x7E, 0x60, 0x7C, 0x06, 0x06, 0x66, 0x3C, 0x00}, 
   {0x3C, 0x66, 0x60, 0x7C, 0x66, 0x66, 0x3C, 0x00}, 
   {0x7E, 0x66, 0x0C, 0x18, 0x18, 0x18, 0x18, 0x00}, 
   {0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00}, 
   {0x3C, 0x66, 0x66, 0x3E, 0x06, 0x66, 0x3C, 0x00}, 
   {0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00}, 
   {0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x30, 0x00}, 
   {0x0C, 0x18, 0x30, 0x60, 0x30, 0x18, 0x0C, 0x00}, 
   {0x00, 0x00, 0x7E, 0x00, 0x7E, 0x00, 0x00, 0x00}, 
   {0x30, 0x18, 0x0C, 0x06, 0x0C, 0x18, 0x30, 0x00}, 
   {0x3C, 0x66, 0x06, 0x0C, 0x18, 0x00, 0x18, 0x00}, 
   {0x3C, 0x66, 0x6E, 0x7A, 0x72, 0x60, 0x3E, 0x00}, 
   {0x18, 0x3C, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x00}, 
   {0x7C, 0x66, 0x66, 0x7C, 0x66, 0x66, 0x7C, 0x00}, 
   {0x3C, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3C, 0x00}, 
   {0x78, 0x6C, 0x66, 0x66, 0x66, 0x6C, 0x78, 0x00}, 
   {0x7F, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x7F, 0x00}, 
   {0x7F, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x60, 0x00}, 
   {0x3C, 0x66, 0x60, 0x6E, 0x66, 0x66, 0x3C, 0x00}, 
   {0x66, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00}, 
   {0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00}, 
   {0x06, 0x06, 0x06, 0x06, 0x06, 0x66, 0x3C, 0x00}, 
   {0x66, 0x6C, 0x78, 0x70, 0x78, 0x6C, 0x66, 0x00}, 
   {0x60, 0x60, 0x60, 0x60, 0x60, 0x66, 0x7F, 0x00}, 
   {0x63, 0x77, 0x7F, 0x6B, 0x63, 0x63, 0x63, 0x00}, 
   {0x66, 0x66, 0x76, 0x7E, 0x6E, 0x66, 0x66, 0x00}, 
   {0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00}, 
   {0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x00}, 
   {0x3C, 0x66, 0x66, 0x66, 0x6E, 0x7C, 0x0E, 0x00}, 
   {0x7C, 0x66, 0x66, 0x7C, 0x78, 0x6C, 0x66, 0x00}, 
   {0x3C, 0x66, 0x30, 0x18, 0x0C, 0x66, 0x3C, 0x00}, 
   {0x7E, 0x5A, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00}, 
   {0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00}, 
   {0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00}, 
   {0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00}, 
   {0x66, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0x66, 0x00}, 
   {0x66, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x18, 0x00}, 
   {0x7F, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x7F, 0x00}, 
   {0x3C, 0x30, 0x30, 0x30, 0x30, 0x30, 0x3C, 0x00}, 
   {0x00, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x00}, 
   {0x3C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x3C, 0x00}, 
   {0x18, 0x3C, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00}, 
   {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00}, 
   {0x18, 0x18, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00}, 
   {0x00, 0x00, 0x3C, 0x06, 0x3E, 0x66, 0x3E, 0x00}, 
   {0x60, 0x60, 0x7C, 0x66, 0x66, 0x66, 0x7C, 0x00}, 
   {0x00, 0x00, 0x3C, 0x60, 0x60, 0x66, 0x3C, 0x00}, 
   {0x06, 0x06, 0x3E, 0x66, 0x66, 0x66, 0x3E, 0x00}, 
   {0x00, 0x00, 0x3C, 0x66, 0x7E, 0x60, 0x3C, 0x00}, 
   {0x1C, 0x30, 0x78, 0x30, 0x30, 0x30, 0x30, 0x00}, 
   {0x00, 0x00, 0x3E, 0x66, 0x66, 0x3E, 0x06, 0x3C}, 
   {0x60, 0x60, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x00}, 
   {0x18, 0x00, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00}, 
   {0x0C, 0x00, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x38}, 
   {0x60, 0x60, 0x66, 0x6C, 0x78, 0x6C, 0x66, 0x00}, 
   {0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x1C, 0x00}, 
   {0x00, 0x00, 0x76, 0x7F, 0x6B, 0x63, 0x63, 0x00}, 
   {0x00, 0x00, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x00}, 
   {0x00, 0x00, 0x3C, 0x66, 0x66, 0x66, 0x3C, 0x00}, 
   {0x00, 0x00, 0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60}, 
   {0x00, 0x00, 0x3E, 0x66, 0x66, 0x3E, 0x06, 0x06}, 
   {0x00, 0x00, 0x7C, 0x66, 0x60, 0x60, 0x60, 0x00}, 
   {0x00, 0x00, 0x3E, 0x60, 0x3C, 0x06, 0x7C, 0x00}, 
   {0x30, 0x30, 0x7C, 0x30, 0x30, 0x30, 0x1C, 0x00}, 
   {0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x3E, 0x00}, 
   {0x00, 0x00, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00}, 
   {0x00, 0x00, 0x63, 0x6B, 0x7F, 0x36, 0x22, 0x00}, 
   {0x00, 0x00, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0x00}, 
   {0x00, 0x00, 0x66, 0x66, 0x66, 0x3E, 0x06, 0x3C}, 
   {0x00, 0x00, 0x7E, 0x0C, 0x18, 0x30, 0x7E, 0x00}, 
   {0x0C, 0x18, 0x18, 0x30, 0x18, 0x18, 0x0C, 0x00}, 
   {0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00}, 
   {0x30, 0x18, 0x18, 0x0C, 0x18, 0x18, 0x30, 0x00}, 
   {0x76, 0xDC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 
};

static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t environ_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

static radio_station_t stations[MAX_STATIONS];
static int num_stations = 0;
static int current_station_idx = 0;
static bool stations_is_dynamic = false;

static uint32_t *frame_buf = NULL;
static const int frame_buf_width = 640;
static const int frame_buf_height = 360;

static volatile enum core_status_t core_status = STATUS_IDLE;
static float current_volume = 0.8f;
static int visualizer_mode = 0; 
static uint32_t time_counter = 0;

static playback_mode_t playback_mode = PLAYBACK_RADIO;
static char file_path[1024] = "";
static char file_display_name[256] = "";
static uint64_t file_total_bytes = 0;
static uint64_t file_bytes_read = 0;
static uint32_t wav_sample_rate = 44100;
static uint8_t wav_channels = 2;
static uint8_t wav_bits = 16;
static uint16_t wav_audio_format = 1;
static uint16_t wav_block_align = 4;
static volatile bool file_seek_requested = false;
static uint64_t file_seek_target_bytes = 0;
static uint32_t mp3_bitrate_bps = 128000;
static uint32_t mp3_sample_rate = 44100;
static uint64_t pcm_samples_played = 0;
static uint64_t mp3_total_seconds = 0;

static ring_buffer_t rb;
static pcm_fifo_t pcm_queue;

static struct mad_stream mad_stream_state;
static struct mad_frame mad_frame_state;
static struct mad_synth mad_synth_state;
static bool mad_initialized = false;

static uint8_t mad_input_buf[MAD_INPUT_BUF_SIZE + MAD_BUFFER_GUARD];

static pthread_t conn_thread;
static volatile bool cancel_thread = false;
static volatile bool thread_active = false;
static volatile int sockfd = -1;
static char target_url[512] = "";

static float vis_history[VIS_SIZE];
static int vis_history_index = 0;
static float vis_bars[NUM_SPECTRUM_BARS];
static float vis_peaks[NUM_SPECTRUM_BARS];
static int vis_peak_hold[NUM_SPECTRUM_BARS];
static float fft_window[VIS_SIZE];
static int bit_rev_lut[VIS_SIZE];
static float twiddle_cos[VIS_SIZE / 2];
static float twiddle_sin[VIS_SIZE / 2];
static int bar_bin_start[NUM_SPECTRUM_BARS];
static int bar_bin_end[NUM_SPECTRUM_BARS];
static float bar_weight[NUM_SPECTRUM_BARS];

static int last_option_station_idx = -1;
static int last_option_visualizer_mode = -1;
static float last_option_volume = -1.0f;

static uint32_t idle_frames = 0;
static uint32_t autohide_limit_frames = 0;
static bool ui_hidden = false;

static bool last_input_state[16] = {false};

static double resample_phase = 0.0;
static float vu_meter_l = 0.0f;
static float vu_meter_r = 0.0f;

enum ui_theme_idx {
   THEME_CLASSIC_BLUE = 0,
   THEME_MATRIX_GREEN,
   THEME_RETRO_AMBER,
   THEME_SYNTHWAVE_PINK,
   THEME_CYBERPUNK_ORANGE,
   THEME_COUNT
};

typedef struct {
   uint32_t bg_top;
   uint32_t bg_bot;
   uint32_t text_primary;
   uint32_t text_secondary;
   uint32_t border;
   uint32_t dial_accent;
} ui_theme_t;

static const ui_theme_t ui_themes[THEME_COUNT] = {
   { 0x050c1e, 0x00030a, 0xFFFFFF, 0x88CCFF, 0x334466, 0x00FFCC },
   { 0x000a00, 0x000200, 0x33FF33, 0x008800, 0x113311, 0x00FF00 },
   { 0x0c0800, 0x030200, 0xFFB300, 0xB37D00, 0x4d3300, 0xFF8F00 },
   { 0x14051a, 0x06010a, 0xFF00AA, 0x00FFCC, 0x4a0e5c, 0xFF55AA },
   { 0x140a00, 0x050200, 0xFF7700, 0xFFCC00, 0x5c2b00, 0xFF5500 }
};

static int active_ui_theme = THEME_CLASSIC_BLUE;

enum vis_palette_idx {
   PALETTE_CYAN_PINK = 0,
   PALETTE_ACID_GREEN,
   PALETTE_FIRE_BRIMSTONE,
   PALETTE_RAINBOW_RAVE,
   PALETTE_DEEP_SPACE_PURPLE,
   PALETTE_COUNT
};

static int active_vis_palette = PALETTE_CYAN_PINK;

static uint32_t get_vis_color(float t, uint32_t time) {
   if (t < 0.0f) t = 0.0f;
   else if (t > 1.0f) t = 1.0f;
   switch (active_vis_palette) {
      case PALETTE_ACID_GREEN: {
         int r = (int)(t * 128.0f);
         int g = 200 + (int)(t * 55.0f);
         int b = (int)((1.0f - t) * 30.0f);
         return (r << 16) | (g << 8) | b;
      }
      case PALETTE_FIRE_BRIMSTONE: {
         if (t < 0.5f) {
            float f = t * 2.0f;
            int r = 180 + (int)(f * 75.0f);
            int g = (int)(f * 100.0f);
            return (r << 16) | (g << 8);
         } else {
            float f = (t - 0.5f) * 2.0f;
            int r = 255;
            int g = 100 + (int)(f * 120.0f);
            int b = (int)(f * 100.0f);
            return (r << 16) | (g << 8) | b;
         }
      }
      case PALETTE_RAINBOW_RAVE: {
         float hue = fmodf(t * 360.0f + time * 0.5f, 360.0f);
         float c = 1.0f;
         float x = c * (1.0f - fabsf(fmodf(hue / 60.0f, 2.0f) - 1.0f));
         float r1 = 0, g1 = 0, b1 = 0;
         if (hue < 60.0f) { r1 = c; g1 = x; }
         else if (hue < 120.0f) { r1 = x; g1 = c; }
         else if (hue < 180.0f) { g1 = c; b1 = x; }
         else if (hue < 240.0f) { g1 = x; b1 = c; }
         else if (hue < 300.0f) { r1 = x; b1 = c; }
         else { r1 = c; b1 = x; }
         return (((int)(r1 * 255.0f)) << 16) | (((int)(g1 * 255.0f)) << 8) | ((int)(b1 * 255.0f));
      }
      case PALETTE_DEEP_SPACE_PURPLE: {
         int r = 80 + (int)(t * 175.0f);
         int g = 10 + (int)(t * 30.0f);
         int b = 150 + (int)((1.0f - t) * 105.0f);
         return (r << 16) | (g << 8) | b;
      }
      case PALETTE_CYAN_PINK:
      default: {
         int r = (int)(t * 255.0f);
         int g = 255 - (int)(t * 170.0f);
         int b = 204 - (int)(t * 34.0f);
         return (r << 16) | (g << 8) | b;
      }
   }
}

static float pharmacy_phase_lut[21][21];
static float cross_anim_pos = 0.0f;

/* Forward declarations */
static void stop_current_thread(void);
static void play_current_station(void);
static void check_variables(void);

/* Ring buffer implementation */
static void ring_buffer_init(ring_buffer_t *r, size_t size) {
   r->data = (uint8_t *)malloc(size);
   r->size = size;
   r->write_ptr = 0;
   r->read_ptr = 0;
   pthread_mutex_init(&r->mutex, NULL);
}

static void ring_buffer_free(ring_buffer_t *r) {
   if (r->data) {
      free(r->data);
      r->data = NULL;
   }
   pthread_mutex_destroy(&r->mutex);
}

static void ring_buffer_clear(ring_buffer_t *r) {
   r->write_ptr = 0;
   r->read_ptr = 0;
}

static size_t ring_buffer_write_space(ring_buffer_t *r) {
   size_t rp = r->read_ptr;
   size_t wp = r->write_ptr;
   if (wp >= rp) {
      return r->size - 1 - (wp - rp);
   } else {
      return rp - wp - 1;
   }
}

static size_t ring_buffer_read_avail(ring_buffer_t *r) {
   size_t rp = r->read_ptr;
   size_t wp = r->write_ptr;
   if (wp >= rp) {
      return wp - rp;
   } else {
      return r->size - (rp - wp);
   }
}

static void ring_buffer_write(ring_buffer_t *r, const uint8_t *src, size_t len) {
   size_t wp = r->write_ptr;
   size_t space_to_end = r->size - wp;
   if (len <= space_to_end) {
      memcpy(r->data + wp, src, len);
      r->write_ptr = (wp + len) % r->size;
   } else {
      memcpy(r->data + wp, src, space_to_end);
      memcpy(r->data, src + space_to_end, len - space_to_end);
      r->write_ptr = len - space_to_end;
   }
}

static void ring_buffer_read(ring_buffer_t *r, uint8_t *dest, size_t len) {
   size_t rp = r->read_ptr;
   size_t data_to_end = r->size - rp;
   if (len <= data_to_end) {
      memcpy(dest, r->data + rp, len);
      r->read_ptr = (rp + len) % r->size;
   } else {
      memcpy(dest, r->data + rp, data_to_end);
      memcpy(dest + data_to_end, r->data, len - data_to_end);
      r->read_ptr = len - data_to_end;
   }
}

/* Circular PCM FIFO (stores stereo interleaved int16 samples) */
static void pcm_fifo_init(pcm_fifo_t *q, size_t size_in_frames) {
   q->size = size_in_frames;
   q->buffer = (int16_t *)malloc(q->size * 2 * sizeof(int16_t));
   q->write_ptr = 0;
   q->read_ptr = 0;
   q->count = 0;
}

static void pcm_fifo_free(pcm_fifo_t *q) {
   if (q->buffer) {
      free(q->buffer);
      q->buffer = NULL;
   }
   q->count = 0;
}

static void pcm_fifo_clear(pcm_fifo_t *q) {
   q->write_ptr = 0;
   q->read_ptr = 0;
   q->count = 0;
}

static size_t pcm_fifo_avail(pcm_fifo_t *q) {
   return q->count;
}

static size_t pcm_fifo_space(pcm_fifo_t *q) {
   return q->size - q->count;
}

static void pcm_fifo_write(pcm_fifo_t *q, const int16_t *frames, size_t num_frames) {
   if (num_frames > pcm_fifo_space(q)) {
      num_frames = pcm_fifo_space(q);
   }
   if (num_frames == 0) return;

   size_t space_to_end = q->size - q->write_ptr;
   if (num_frames <= space_to_end) {
      memcpy(q->buffer + q->write_ptr * 2, frames, num_frames * 2 * sizeof(int16_t));
      q->write_ptr = (q->write_ptr + num_frames) % q->size;
   } else {
      memcpy(q->buffer + q->write_ptr * 2, frames, space_to_end * 2 * sizeof(int16_t));
      memcpy(q->buffer, frames + space_to_end * 2, (num_frames - space_to_end) * 2 * sizeof(int16_t));
      q->write_ptr = num_frames - space_to_end;
   }
   q->count += num_frames;
}

static void pcm_fifo_peek_at(pcm_fifo_t *q, size_t offset, int16_t *out_l, int16_t *out_r) {
   if (offset >= q->count) {
      *out_l = 0;
      *out_r = 0;
      return;
   }
   size_t idx = (q->read_ptr + offset) % q->size;
   *out_l = q->buffer[idx * 2];
   *out_r = q->buffer[idx * 2 + 1];
}

static void pcm_fifo_consume(pcm_fifo_t *q, size_t num_frames) {
   if (num_frames > q->count) {
      num_frames = q->count;
   }
   q->read_ptr = (q->read_ptr + num_frames) % q->size;
   q->count -= num_frames;
}

/* High-fidelity mad_fixed to int16 rounding & clipping conversion */
static inline int16_t mad_fixed_to_int16(mad_fixed_t sample) {
   sample += (1L << (MAD_F_FRACBITS - 16));
   if (sample >= MAD_F_ONE)
      sample = MAD_F_ONE - 1;
   else if (sample < -MAD_F_ONE)
      sample = -MAD_F_ONE;
   return (int16_t)(sample >> (MAD_F_FRACBITS - 15));
}

static void mad_reset_decoder(void) {
   if (mad_initialized) {
      mad_synth_finish(&mad_synth_state);
      mad_frame_finish(&mad_frame_state);
      mad_stream_finish(&mad_stream_state);
      mad_initialized = false;
   }
   pcm_fifo_clear(&pcm_queue);
   resample_phase = 0.0;
}

/* URL and Header parsing */
static void parse_url(const char *url, char *host, int *port, char *path) {
   *port = 80;
   strcpy(path, "/");

   const char *p = url;
   if (strncmp(p, "http://", 7) == 0) {
      p += 7;
   } else if (strncmp(p, "https://", 8) == 0) {
      p += 8;
   }

   const char *slash = strchr(p, '/');
   char host_port[256];
   if (slash) {
      size_t host_len = slash - p;
      if (host_len > 255) host_len = 255;
      strncpy(host_port, p, host_len);
      host_port[host_len] = '\0';
      strcpy(path, slash);
   } else {
      strncpy(host_port, p, 255);
      host_port[255] = '\0';
   }

   char *colon = strchr(host_port, ':');
   if (colon) {
      *colon = '\0';
      *port = atoi(colon + 1);
   }
   strcpy(host, host_port);
}

static const char *find_header_case_insensitive(const char *headers, const char *name) {
   size_t name_len = strlen(name);
   const char *p = headers;
   while (*p) {
      if (strncasecmp(p, name, name_len) == 0) {
         return p;
      }
      p = strchr(p, '\n');
      if (!p) break;
      p++;
   }
   return NULL;
}

/* Network streaming worker thread */
static void *connection_thread_func(void *arg) {
   char current_url[512];
   strncpy(current_url, target_url, sizeof(current_url) - 1);
   current_url[sizeof(current_url) - 1] = '\0';

   int redirect_count = 0;
   bool connected = false;

   while (redirect_count < 5 && !cancel_thread) {
      char host[256];
      int port = 80;
      char path[256];
      parse_url(current_url, host, &port, path);

      struct addrinfo hints, *res = NULL;
      memset(&hints, 0, sizeof(hints));
      hints.ai_family = AF_INET;
      hints.ai_socktype = SOCK_STREAM;

      char port_str[16];
      snprintf(port_str, sizeof(port_str), "%d", port);

      if (getaddrinfo(host, port_str, &hints, &res) != 0) {
         break;
      }

      int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
      if (s < 0) {
         freeaddrinfo(res);
         break;
      }
      sockfd = s;

#ifdef _WIN32
      DWORD timeout_ms = 4000;
      setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
      setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
#else
      struct timeval timeout;
      timeout.tv_sec = 4;
      timeout.tv_usec = 0;
      setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const void*)&timeout, sizeof(timeout));
      setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (const void*)&timeout, sizeof(timeout));
#endif

      if (connect(sockfd, res->ai_addr, res->ai_addrlen) < 0) {
         close(sockfd);
         sockfd = -1;
         freeaddrinfo(res);
         break;
      }

      freeaddrinfo(res);

      if (cancel_thread) {
         close(sockfd);
         sockfd = -1;
         break;
      }

      char req[1024];
      snprintf(req, sizeof(req),
               "GET %s HTTP/1.0\r\n"
               "Host: %s\r\n"
               "User-Agent: RetroArch-Radio/1.1\r\n"
               "Accept: */*\r\n"
               "Icy-MetaData: 0\r\n"
               "Connection: close\r\n\r\n",
               path, host);

      send(sockfd, req, strlen(req), 0);

      char header_buf[4096];
      int header_len = 0;
      char c;
      bool found_end = false;
      while (header_len < (int)sizeof(header_buf) - 1 && !cancel_thread) {
         int r = recv(sockfd, &c, 1, 0);
         if (r <= 0) break;
         header_buf[header_len++] = c;
         header_buf[header_len] = '\0';
         if (header_len >= 4 && strcmp(header_buf + header_len - 4, "\r\n\r\n") == 0) {
            found_end = true;
            break;
         }
      }

      if (cancel_thread || !found_end) {
         close(sockfd);
         sockfd = -1;
         break;
      }

      int status_code = 0;
      if (sscanf(header_buf, "HTTP/%*d.%*d %d", &status_code) != 1) {
         if (sscanf(header_buf, "ICY %d", &status_code) != 1) {
            status_code = 0;
         }
      }

      if (status_code == 301 || status_code == 302 || status_code == 307 || status_code == 308) {
         const char *loc = find_header_case_insensitive(header_buf, "Location:");
         if (loc) {
            loc += 9;
            while (*loc == ' ' || *loc == '\t') loc++;
            const char *end = strchr(loc, '\r');
            if (!end) end = strchr(loc, '\n');
            if (end) {
               size_t len = end - loc;
               if (len < sizeof(current_url)) {
                  strncpy(current_url, loc, len);
                  current_url[len] = '\0';
               }
            }
         }
         close(sockfd);
         sockfd = -1;
         redirect_count++;
         continue;
      } else if (status_code == 200) {
         connected = true;
         break;
      } else {
         close(sockfd);
         sockfd = -1;
         break;
      }
   }

   if (!connected || cancel_thread) {
      if (sockfd >= 0) {
         close(sockfd);
         sockfd = -1;
      }
      if (!cancel_thread) {
         core_status = STATUS_ERROR;
      }
      return NULL;
   }

   core_status = STATUS_BUFFERING;

   uint8_t temp_buf[4096];
   while (!cancel_thread) {
      pthread_mutex_lock(&rb.mutex);
      size_t space = ring_buffer_write_space(&rb);
      pthread_mutex_unlock(&rb.mutex);

      if (space < 4096) {
         usleep(8000); 
         continue;
      }

      int r = recv(sockfd, (char *)temp_buf, sizeof(temp_buf), 0);
      if (cancel_thread) break;
      if (r < 0) {
         if (errno == EAGAIN || errno == EWOULDBLOCK) {
            continue;
         }
         break;
      } else if (r == 0) {
         break;
      }

      pthread_mutex_lock(&rb.mutex);
      ring_buffer_write(&rb, temp_buf, r);
      pthread_mutex_unlock(&rb.mutex);
   }

   if (sockfd >= 0) {
      close(sockfd);
      sockfd = -1;
   }

   if (!cancel_thread) {
      core_status = STATUS_ERROR;
   }

   return NULL;
}

/* Comprehensive RIFF WAVE header parser */
static bool parse_wav_header(FILE *f) {
   uint8_t riff[12];
   if (fread(riff, 1, 12, f) != 12) return false;
   if (memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) return false;
   
   bool fmt_found = false;
   bool data_found = false;
   
   while (!data_found) {
      uint8_t chunk_header[8];
      if (fread(chunk_header, 1, 8, f) != 8) break;
      uint32_t chunk_size = chunk_header[4] | (chunk_header[5] << 8) | 
                            (chunk_header[6] << 16) | (chunk_header[7] << 24);
      
      if (memcmp(chunk_header, "fmt ", 4) == 0) {
         uint8_t fmt_data[40];
         size_t read_len = (chunk_size < sizeof(fmt_data)) ? chunk_size : sizeof(fmt_data);
         if (fread(fmt_data, 1, read_len, f) != read_len) return false;
         
         wav_audio_format = fmt_data[0] | (fmt_data[1] << 8);
         wav_channels = fmt_data[2] | (fmt_data[3] << 8);
         wav_sample_rate = fmt_data[4] | (fmt_data[5] << 8) | 
                           (fmt_data[6] << 16) | (fmt_data[7] << 24);
         wav_block_align = fmt_data[12] | (fmt_data[13] << 8);
         wav_bits = fmt_data[14] | (fmt_data[15] << 8);

         if (wav_audio_format == 0xFFFE && read_len >= 26) {
            uint16_t sub_format = fmt_data[24] | (fmt_data[25] << 8);
            wav_audio_format = sub_format;
         }

         if (wav_audio_format != 1 && wav_audio_format != 3) {
            return false;
         }
         if (wav_channels == 0 || wav_sample_rate == 0) {
            return false;
         }

         if (chunk_size > read_len) {
            fseek(f, (chunk_size - read_len + 1) & ~1, SEEK_CUR);
         }
         fmt_found = true;
      } else if (memcmp(chunk_header, "data", 4) == 0) {
         file_total_bytes = chunk_size;
         data_found = true;
      } else {
         fseek(f, (chunk_size + 1) & ~1, SEEK_CUR);
      }
   }
   
   return fmt_found && data_found;
}

/* Local file reader thread */
static void *file_reader_thread_func(void *arg) {
   FILE *f = fopen(file_path, "rb");
   if (!f) {
      core_status = STATUS_ERROR;
      return NULL;
   }

   if (playback_mode == PLAYBACK_FILE_WAV) {
      if (!parse_wav_header(f)) {
         fclose(f);
         core_status = STATUS_ERROR;
         return NULL;
      }
   } else {
      fseek(f, 0, SEEK_END);
      file_total_bytes = ftell(f);
      rewind(f);
   }

   core_status = STATUS_BUFFERING;
   uint8_t temp_buf[4096];
   long data_start_offset = ftell(f);

   while (!cancel_thread) {
      if (file_seek_requested) {
         long target_offset = data_start_offset + file_seek_target_bytes;
         if (target_offset > data_start_offset + (long)file_total_bytes) {
            target_offset = data_start_offset + (long)file_total_bytes;
         }
         fseek(f, target_offset, SEEK_SET);
         file_bytes_read = target_offset - data_start_offset;
         file_seek_requested = false;
      }

      pthread_mutex_lock(&rb.mutex);
      size_t space = ring_buffer_write_space(&rb);
      pthread_mutex_unlock(&rb.mutex);

      if (space < 4096) {
         usleep(8000);
         continue;
      }

      size_t to_read = 4096;
      if (file_bytes_read + to_read > file_total_bytes) {
         to_read = file_total_bytes - file_bytes_read;
      }

      if (to_read == 0) {
         break;
      }

      size_t r = fread(temp_buf, 1, to_read, f);
      if (r == 0) {
         break;
      }

      file_bytes_read += r;

      pthread_mutex_lock(&rb.mutex);
      ring_buffer_write(&rb, temp_buf, r);
      pthread_mutex_unlock(&rb.mutex);
   }

   fclose(f);

   if (!cancel_thread) {
      core_status = STATUS_EOF;
   }

   return NULL;
}

static void start_file_thread(const char *path) {
   stop_current_thread();
   cancel_thread = false;
   
   strncpy(file_path, path, sizeof(file_path) - 1);
   file_path[sizeof(file_path) - 1] = '\0';
   
   const char *base = strrchr(file_path, '/');
   if (!base) base = strrchr(file_path, '\\');
   if (base) base++; else base = file_path;
   
   strncpy(file_display_name, base, sizeof(file_display_name) - 1);
   file_display_name[sizeof(file_display_name) - 1] = '\0';

   const char *ext = strrchr(file_path, '.');
   if (ext && strcasecmp(ext, ".wav") == 0) {
      playback_mode = PLAYBACK_FILE_WAV;
   } else {
      playback_mode = PLAYBACK_FILE_MP3;
   }
   
   file_bytes_read = 0;
   file_total_bytes = 0;
   file_seek_requested = false;
   file_seek_target_bytes = 0;
   mp3_bitrate_bps = 128000;
   mp3_sample_rate = 44100;
   pcm_samples_played = 0;
   mp3_total_seconds = 0;

   core_status = STATUS_CONNECTING;

   if (pthread_create(&conn_thread, NULL, file_reader_thread_func, NULL) == 0) {
      thread_active = true;
   } else {
      core_status = STATUS_ERROR;
   }
}

static void stop_current_thread(void) {
   if (thread_active) {
      cancel_thread = true;
      if (sockfd >= 0) {
#ifdef _WIN32
         shutdown(sockfd, SD_BOTH);
#else
         shutdown(sockfd, SHUT_RDWR);
#endif
         close(sockfd);
         sockfd = -1;
      }
      pthread_join(conn_thread, NULL);
      thread_active = false;
   }
   mad_reset_decoder();
   pthread_mutex_lock(&rb.mutex);
   ring_buffer_clear(&rb);
   pthread_mutex_unlock(&rb.mutex);
   playback_mode = PLAYBACK_RADIO;
   pcm_samples_played = 0;
   core_status = STATUS_IDLE;
}

static void start_connection_thread(const char *url) {
   stop_current_thread();

   cancel_thread = false;
   strncpy(target_url, url, sizeof(target_url) - 1);
   target_url[sizeof(target_url) - 1] = '\0';

   core_status = STATUS_CONNECTING;

   if (pthread_create(&conn_thread, NULL, connection_thread_func, NULL) == 0) {
      thread_active = true;
   } else {
      core_status = STATUS_ERROR;
   }
}

static void play_current_station(void) {
   if (num_stations <= 0) return;
   const char *url = stations[current_station_idx].url;
   if (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0) {
      start_connection_thread(url);
   } else {
      start_file_thread(url);
   }
}

static void reconnect_current_station(void) {
   play_current_station();
}

static void pause_current_station(void) {
   stop_current_thread();
}

static void change_station(int dir) {
   if (num_stations > 1) {
      current_station_idx = (current_station_idx + dir + num_stations) % num_stations;
      play_current_station();
   }
}

static void check_variables(void) {
   struct retro_variable var = {0};

   var.key = "radio_station";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
      int sel_idx = -1;
      if (sscanf(var.value, "Preset %d", &sel_idx) == 1) {
         int target_idx = sel_idx - 1;
         if (target_idx >= 0 && target_idx < num_stations) {
            if (last_option_station_idx != target_idx) {
               last_option_station_idx = target_idx;
               current_station_idx = target_idx;
               play_current_station();
            }
         }
      }
   }

   var.key = "radio_visualizer";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
      int mode = 0;
      if (strcmp(var.value, "FFT Spectrum Bars") == 0) {
         mode = 0;
      } else if (strcmp(var.value, "LED Pharmacy Grid") == 0) {
         mode = 1;
      } else if (strcmp(var.value, "Phosphor Oscilloscope") == 0) {
         mode = 2;
      } else if (strcmp(var.value, "Circular Audio Radar") == 0) {
         mode = 3;
      } else if (strcmp(var.value, "Analog VU Meters") == 0) {
         mode = 4;
      }
      if (last_option_visualizer_mode != mode) {
         last_option_visualizer_mode = mode;
         visualizer_mode = mode;
      }
   }

   var.key = "radio_volume";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
      int vol_pct = atoi(var.value);
      float vol = (float)vol_pct / 100.0f;
      if (last_option_volume != vol) {
         last_option_volume = vol;
         current_volume = vol;
      }
   }

   static int last_option_ui_autohide = -1;
   var.key = "radio_ui_autohide";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
      int limit = 0;
      int option_idx = 0;
      if (strcmp(var.value, "10 seconds") == 0) {
         limit = 600;
         option_idx = 1;
      } else if (strcmp(var.value, "30 seconds") == 0) {
         limit = 1800;
         option_idx = 2;
      } else if (strcmp(var.value, "1 minute") == 0) {
         limit = 3600;
         option_idx = 3;
      } else if (strcmp(var.value, "5 minutes") == 0) {
         limit = 18000;
         option_idx = 4;
      } else {
         limit = 0; 
         option_idx = 0;
      }
      if (last_option_ui_autohide != option_idx) {
         last_option_ui_autohide = option_idx;
         autohide_limit_frames = limit;
         idle_frames = 0;
      }
   }

   var.key = "radio_ui_theme";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
      int theme = THEME_CLASSIC_BLUE;
      if (strcmp(var.value, "Classic Blue") == 0) theme = THEME_CLASSIC_BLUE;
      else if (strcmp(var.value, "Matrix Green") == 0) theme = THEME_MATRIX_GREEN;
      else if (strcmp(var.value, "Retro Amber") == 0) theme = THEME_RETRO_AMBER;
      else if (strcmp(var.value, "Synthwave Pink") == 0) theme = THEME_SYNTHWAVE_PINK;
      else if (strcmp(var.value, "Cyberpunk Orange") == 0) theme = THEME_CYBERPUNK_ORANGE;

      if (active_ui_theme != theme) {
         active_ui_theme = theme;
      }
   }

   var.key = "radio_vis_palette";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
      int palette = PALETTE_CYAN_PINK;
      if (strcmp(var.value, "Neon Cyan-Pink") == 0) palette = PALETTE_CYAN_PINK;
      else if (strcmp(var.value, "Acid Green") == 0) palette = PALETTE_ACID_GREEN;
      else if (strcmp(var.value, "Fire & Brimstone") == 0) palette = PALETTE_FIRE_BRIMSTONE;
      else if (strcmp(var.value, "Rainbow Rave") == 0) palette = PALETTE_RAINBOW_RAVE;
      else if (strcmp(var.value, "Deep Space Purple") == 0) palette = PALETTE_DEEP_SPACE_PURPLE;

      if (active_vis_palette != palette) {
         active_vis_palette = palette;
      }
   }
}

/* Fast Table-Driven Radix-2 FFT */
static void fast_fft(float *real, float *imag, int n) {
   for (int i = 0; i < n; i++) {
      int j = bit_rev_lut[i];
      if (j > i) {
         float tr = real[i]; real[i] = real[j]; real[j] = tr;
         float ti = imag[i]; imag[i] = imag[j]; imag[j] = ti;
      }
   }

   for (int len = 2; len <= n; len <<= 1) {
      int half = len >> 1;
      int step = n / len;
      for (int i = 0; i < n; i += len) {
         for (int j = 0; j < half; j++) {
            int k = j * step;
            float ur = twiddle_cos[k];
            float ui = twiddle_sin[k];
            int idx1 = i + j;
            int idx2 = i + j + half;
            float tr = real[idx2] * ur - imag[idx2] * ui;
            float ti = real[idx2] * ui + imag[idx2] * ur;
            real[idx2] = real[idx1] - tr;
            imag[idx2] = imag[idx1] - ti;
            real[idx1] += tr;
            imag[idx1] += ti;
         }
      }
   }
}

/* Fast Software Drawing Primitives */
static void draw_char(uint32_t *fb, int fb_w, int fb_h, char c, int x, int y, uint32_t color) {
   if (c < 32 || c > 127) return;
   if (x + 8 <= 0 || x >= fb_w || y + 8 <= 0 || y >= fb_h) return;

   int idx = c - 32;
   for (int row = 0; row < 8; row++) {
      int draw_y = y + row;
      if (draw_y < 0 || draw_y >= fb_h) continue;
      uint8_t bits = font_8x8[idx][row];
      uint32_t *row_ptr = fb + draw_y * fb_w;
      for (int col = 0; col < 8; col++) {
         int draw_x = x + col;
         if (draw_x >= 0 && draw_x < fb_w && (bits & (0x80 >> col))) {
            row_ptr[draw_x] = color;
         }
      }
   }
}

static void draw_string(uint32_t *fb, int fb_w, int fb_h, const char *str, int x, int y, uint32_t color) {
   int start_x = x;
   while (*str) {
      if (*str == '\n') {
         y += 10;
         x = start_x;
      } else {
         draw_char(fb, fb_w, fb_h, *str, x, y, color);
         x += 8;
      }
      str++;
   }
}

static void draw_string_with_shadow(uint32_t *fb, int fb_w, int fb_h, const char *str, int x, int y, uint32_t color, uint32_t shadow_color) {
   draw_string(fb, fb_w, fb_h, str, x + 1, y + 1, shadow_color);
   draw_string(fb, fb_w, fb_h, str, x, y, color);
}

static void draw_rect(uint32_t *fb, int fb_w, int fb_h, int rx, int ry, int rw, int rh, uint32_t color) {
   int x_start = rx < 0 ? 0 : rx;
   int x_end = rx + rw;
   if (x_end > fb_w) x_end = fb_w;
   int y_start = ry < 0 ? 0 : ry;
   int y_end = ry + rh;
   if (y_end > fb_h) y_end = fb_h;

   int span = x_end - x_start;
   if (span <= 0 || y_end <= y_start) return;

   for (int y = y_start; y < y_end; y++) {
      uint32_t *row = fb + y * fb_w + x_start;
      for (int x = 0; x < span; x++) {
         row[x] = color;
      }
   }
}

static void draw_rect_gradient(uint32_t *fb, int fb_w, int fb_h, int rx, int ry, int rw, int rh, uint32_t color_top, uint32_t color_bottom) {
   int x_start = rx < 0 ? 0 : rx;
   int x_end = rx + rw;
   if (x_end > fb_w) x_end = fb_w;
   int y_start = ry < 0 ? 0 : ry;
   int y_end = ry + rh;
   if (y_end > fb_h) y_end = fb_h;

   int span = x_end - x_start;
   if (span <= 0 || y_end <= y_start || rh <= 0) return;

   int r_top = (color_top >> 16) & 0xFF;
   int g_top = (color_top >> 8) & 0xFF;
   int b_top = color_top & 0xFF;
   int r_bot = (color_bottom >> 16) & 0xFF;
   int g_bot = (color_bottom >> 8) & 0xFF;
   int b_bot = color_bottom & 0xFF;

   for (int y = y_start; y < y_end; y++) {
      float t = (float)(y - ry) / (float)rh;
      int r = r_top + (int)((r_bot - r_top) * t);
      int g = g_top + (int)((g_bot - g_top) * t);
      int b = b_top + (int)((b_bot - b_top) * t);
      uint32_t color = (r << 16) | (g << 8) | b;

      uint32_t *row = fb + y * fb_w + x_start;
      for (int x = 0; x < span; x++) {
         row[x] = color;
      }
   }
}

static void draw_line(uint32_t *fb, int fb_w, int fb_h, int x0, int y0, int x1, int y1, uint32_t color) {
   int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
   int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
   int err = dx + dy;

   for (;;) {
      if (x0 >= 0 && x0 < fb_w && y0 >= 0 && y0 < fb_h) {
         fb[y0 * fb_w + x0] = color;
      }
      if (x0 == x1 && y0 == y1) break;
      int e2 = 2 * err;
      if (e2 >= dy) { err += dy; x0 += sx; }
      if (e2 <= dx) { err += dx; y0 += sy; }
   }
}

/* Audio Resampling and Decoding Pipeline */
static void decode_audio_into_queue(void) {
   if (playback_mode == PLAYBACK_FILE_WAV) {
      if (wav_channels == 0 || wav_sample_rate == 0) return;
      size_t bytes_per_sample = (wav_bits + 7) / 8;
      size_t bytes_per_src_frame = wav_channels * bytes_per_sample;
      if (bytes_per_src_frame == 0) return;

      while (pcm_fifo_space(&pcm_queue) >= 1024) {
         pthread_mutex_lock(&rb.mutex);
         size_t avail = ring_buffer_read_avail(&rb);
         size_t frames_avail = avail / bytes_per_src_frame;
         size_t frames_to_read = pcm_fifo_space(&pcm_queue);
         if (frames_to_read > frames_avail) frames_to_read = frames_avail;
         if (frames_to_read > 512) frames_to_read = 512;

         if (frames_to_read == 0) {
            pthread_mutex_unlock(&rb.mutex);
            break;
         }

         uint8_t raw[512 * 8];
         ring_buffer_read(&rb, raw, frames_to_read * bytes_per_src_frame);
         pthread_mutex_unlock(&rb.mutex);

         int16_t decoded[512 * 2];
         for (size_t i = 0; i < frames_to_read; i++) {
            int16_t l = 0, r = 0;
            const uint8_t *p = raw + i * bytes_per_src_frame;

            if (wav_audio_format == 3 && wav_bits == 32) {
               float *fp = (float *)p;
               float fl = fp[0];
               float fr = (wav_channels > 1) ? fp[1] : fl;
               if (fl > 1.0f) fl = 1.0f; else if (fl < -1.0f) fl = -1.0f;
               if (fr > 1.0f) fr = 1.0f; else if (fr < -1.0f) fr = -1.0f;
               l = (int16_t)(fl * 32767.0f);
               r = (int16_t)(fr * 32767.0f);
            } else if (wav_bits == 16) {
               const int16_t *ip = (const int16_t *)p;
               l = ip[0];
               r = (wav_channels > 1) ? ip[1] : l;
            } else if (wav_bits == 24) {
               int32_t val_l = ((int32_t)(p[0] | (p[1] << 8) | (p[2] << 16))) << 8;
               l = (int16_t)(val_l >> 16);
               if (wav_channels > 1) {
                  int32_t val_r = ((int32_t)(p[3] | (p[4] << 8) | (p[5] << 16))) << 8;
                  r = (int16_t)(val_r >> 16);
               } else {
                  r = l;
               }
            } else if (wav_bits == 8) {
               l = (int16_t)((p[0] - 128) * 256);
               r = (wav_channels > 1) ? (int16_t)((p[1] - 128) * 256) : l;
            }
            decoded[i * 2] = l;
            decoded[i * 2 + 1] = r;
         }
         pcm_fifo_write(&pcm_queue, decoded, frames_to_read);
      }
   } else {
      if (!mad_initialized) {
         mad_stream_init(&mad_stream_state);
         mad_frame_init(&mad_frame_state);
         mad_synth_init(&mad_synth_state);
         mad_initialized = true;
      }

      size_t unconsumed = 0;
      if (mad_stream_state.next_frame != NULL &&
          mad_stream_state.bufend > mad_stream_state.next_frame) {
         unconsumed = (size_t)(mad_stream_state.bufend - mad_stream_state.next_frame);
         if (unconsumed > 0 && unconsumed <= MAD_INPUT_BUF_SIZE)
            memmove(mad_input_buf, mad_stream_state.next_frame, unconsumed);
         else
            unconsumed = 0;
      }

      size_t space = MAD_INPUT_BUF_SIZE - unconsumed;
      if (space > 0) {
         pthread_mutex_lock(&rb.mutex);
         size_t rb_avail = ring_buffer_read_avail(&rb);
         size_t to_read = rb_avail < space ? rb_avail : space;
         if (to_read > 0)
            ring_buffer_read(&rb, mad_input_buf + unconsumed, to_read);
         pthread_mutex_unlock(&rb.mutex);
         unconsumed += to_read;
      }

      if (unconsumed > 0) {
         memset(mad_input_buf + unconsumed, 0, MAD_BUFFER_GUARD);
         mad_stream_buffer(&mad_stream_state, mad_input_buf, unconsumed + MAD_BUFFER_GUARD);
      }

      while (pcm_fifo_space(&pcm_queue) >= 1152) {
         if (mad_frame_decode(&mad_frame_state, &mad_stream_state) == -1) {
            if (MAD_RECOVERABLE(mad_stream_state.error))
               continue;
            break;
         }
         if (mad_frame_state.header.bitrate > 0)
            mp3_bitrate_bps = mad_frame_state.header.bitrate;
         if (mad_frame_state.header.samplerate > 0)
            mp3_sample_rate = mad_frame_state.header.samplerate;
         if (mp3_total_seconds == 0 && mp3_bitrate_bps > 0 && file_total_bytes > 0)
            mp3_total_seconds = (file_total_bytes * 8) / mp3_bitrate_bps;

         mad_synth_frame(&mad_synth_state, &mad_frame_state);
         struct mad_pcm *pcm = &mad_synth_state.pcm;

         int16_t decoded[1152 * 2];
         for (unsigned s = 0; s < pcm->length; s++) {
            mad_fixed_t l = pcm->samples[0][s];
            mad_fixed_t r = (pcm->channels > 1) ? pcm->samples[1][s] : l;
            decoded[s * 2] = mad_fixed_to_int16(l);
            decoded[s * 2 + 1] = mad_fixed_to_int16(r);
         }
         pcm_fifo_write(&pcm_queue, decoded, pcm->length);
      }
   }
}

/* Linear Resampler converting variable source sample rates to native 44.1 kHz */
static void resample_to_output(int16_t *out_buf, size_t out_frames, uint32_t src_rate) {
   if (src_rate == 0) src_rate = OUTPUT_SAMPLE_RATE;
   double ratio = (double)src_rate / (double)OUTPUT_SAMPLE_RATE;

   for (size_t i = 0; i < out_frames; i++) {
      size_t idx0 = (size_t)resample_phase;
      double frac = resample_phase - (double)idx0;

      int16_t s0_l, s0_r, s1_l, s1_r;
      pcm_fifo_peek_at(&pcm_queue, idx0, &s0_l, &s0_r);
      pcm_fifo_peek_at(&pcm_queue, idx0 + 1, &s1_l, &s1_r);

      float l = (float)s0_l + (float)frac * ((float)s1_l - (float)s0_l);
      float r = (float)s0_r + (float)frac * ((float)s1_r - (float)s0_r);

      l *= current_volume;
      r *= current_volume;

      if (l > 32767.0f) l = 32767.0f; else if (l < -32768.0f) l = -32768.0f;
      if (r > 32767.0f) r = 32767.0f; else if (r < -32768.0f) r = -32768.0f;

      out_buf[i * 2] = (int16_t)l;
      out_buf[i * 2 + 1] = (int16_t)r;

      float mono = (l + r) * (0.5f / 32767.0f);
      vis_history[vis_history_index] = mono;
      vis_history_index = (vis_history_index + 1) % VIS_SIZE;

      resample_phase += ratio;
   }

   size_t consume_count = (size_t)resample_phase;
   pcm_fifo_consume(&pcm_queue, consume_count);
   resample_phase -= (double)consume_count;
}

/* Visualizer Rendering Modes */
static void render_visualizer_bars(uint32_t *fb, int width, int height) {
   int num_bars = NUM_SPECTRUM_BARS;
   int bar_gap = 2;
   int bar_width = (width - 80) / num_bars - bar_gap;
   if (bar_width < 2) bar_width = 2;
   int start_x = 40;
   int max_h = ui_hidden ? (height - 80) : 100;
   int base_y = ui_hidden ? (height - 40) : 300;

   for (int i = 0; i < num_bars; i++) {
      int h = (int)vis_bars[i];
      if (h > max_h) h = max_h;
      if (h < 2) h = 2;

      int x = start_x + i * (bar_width + bar_gap);
      int y = base_y - h;

      float t_bar = (float)i / (float)num_bars;
      uint32_t color_top = get_vis_color(t_bar * 0.4f, time_counter);
      uint32_t color_bot = get_vis_color(0.5f + t_bar * 0.5f, time_counter);
      draw_rect_gradient(fb, width, height, x, y, bar_width, h, color_top, color_bot);

      int peak_h = (int)vis_peaks[i];
      if (peak_h > max_h) peak_h = max_h;
      if (peak_h >= 2) {
         int peak_y = base_y - peak_h;
         uint32_t peak_color = get_vis_color(0.9f, time_counter);
         draw_rect(fb, width, height, x, peak_y, bar_width, 2, peak_color);
      }
   }
}

static void render_visualizer_pharmacy_grid(uint32_t *fb, int width, int height) {
   int cx = width / 2;
   int cy = ui_hidden ? (height / 2) : 240;

   float bass = vis_bars[0] + vis_bars[1] + vis_bars[2] + vis_bars[3];
   cross_anim_pos += 0.2f + bass * 0.04f;
   for (int i = 0; i < 3; i++) {
      float size = fmodf(cross_anim_pos + i * 25.0f, 75.0f);
      float alpha = 1.0f - (size / 75.0f);
      if (alpha > 0.0f) {
         uint32_t color = get_vis_color(size / 75.0f, time_counter);
         int r = (int)(((color >> 16) & 0xFF) * alpha * 0.25f);
         int g = (int)(((color >> 8) & 0xFF) * alpha * 0.25f);
         int b = (int)((color & 0xFF) * alpha * 0.25f);
         uint32_t bg_cross_color = (r << 16) | (g << 8) | b;
         
         int w = (int)size;
         int iw = w / 3;
         draw_line(fb, width, height, cx + iw, cy - w, cx + iw, cy - iw, bg_cross_color);
         draw_line(fb, width, height, cx + iw, cy - iw, cx + w, cy - iw, bg_cross_color);
         draw_line(fb, width, height, cx + w, cy - iw, cx + w, cy + iw, bg_cross_color);
         draw_line(fb, width, height, cx + w, cy + iw, cx + iw, cy + iw, bg_cross_color);
         draw_line(fb, width, height, cx + iw, cy + iw, cx + iw, cy + w, bg_cross_color);
         draw_line(fb, width, height, cx + iw, cy + w, cx - iw, cy + w, bg_cross_color);
         draw_line(fb, width, height, cx - iw, cy + w, cx - iw, cy + iw, bg_cross_color);
         draw_line(fb, width, height, cx - iw, cy + iw, cx - w, cy + iw, bg_cross_color);
         draw_line(fb, width, height, cx - w, cy + iw, cx - w, cy - iw, bg_cross_color);
         draw_line(fb, width, height, cx - w, cy - iw, cx - iw, cy - iw, bg_cross_color);
         draw_line(fb, width, height, cx - iw, cy - iw, cx - iw, cy - w, bg_cross_color);
         draw_line(fb, width, height, cx - iw, cy - w, cx + iw, cy - w, bg_cross_color);
      }
   }
   
   int led_size = 4;
   int led_gap = 2;
   int step = led_size + led_gap;
   
   float treble = vis_bars[40] + vis_bars[41] + vis_bars[42] + vis_bars[43];
   static float border_phase = 0.0f;
   border_phase = fmodf(border_phase + 0.015f + treble * 0.003f, 1.0f);
   
   for (int dy = -10; dy <= 10; dy++) {
      for (int dx = -10; dx <= 10; dx++) {
         int adx = abs(dx);
         int ady = abs(dy);
         
         if (adx <= 3 || ady <= 3) {
            int x = cx + dx * step - led_size / 2;
            int y = cy + dy * step - led_size / 2;
            
            bool on_border = (adx == 3 && ady >= 3 && ady <= 10) ||
                             (ady == 3 && adx >= 3 && adx <= 10) ||
                             (ady == 10 && adx <= 3) ||
                             (adx == 10 && ady <= 3);
                             
            uint32_t led_color = 0;
            if (on_border) {
               float phase = pharmacy_phase_lut[dy + 10][dx + 10];
               float diff = fabsf(phase - border_phase);
               if (diff > 0.5f) diff = 1.0f - diff;
               
               float chase_factor = 0.0f;
               if (diff < 0.08f) {
                  chase_factor = 1.0f - (diff / 0.08f);
               }
               
               uint32_t base_color = get_vis_color(phase, time_counter);
               int r = (base_color >> 16) & 0xFF;
               int g = (base_color >> 8) & 0xFF;
               int b = base_color & 0xFF;
               
               float factor = 0.15f + 0.85f * chase_factor;
               led_color = (((int)(r * factor)) << 16) | (((int)(g * factor)) << 8) | ((int)(b * factor));
            } else {
               int r_dist = adx > ady ? adx : ady;
               int bin = r_dist * 6;
               if (bin >= NUM_SPECTRUM_BARS) bin = NUM_SPECTRUM_BARS - 1;
               float val = vis_bars[bin];
               float intensity = val / 40.0f;
               if (intensity > 1.0f) intensity = 1.0f;
               else if (intensity < 0.0f) intensity = 0.0f;
               
               uint32_t base_color = get_vis_color((float)r_dist / 10.0f, time_counter);
               int r = (base_color >> 16) & 0xFF;
               int g = (base_color >> 8) & 0xFF;
               int b = base_color & 0xFF;
               
               float factor = 0.08f + 0.92f * intensity;
               led_color = (((int)(r * factor)) << 16) | (((int)(g * factor)) << 8) | ((int)(b * factor));
            }
            
            draw_rect(fb, width, height, x, y, led_size, led_size, led_color);
         }
      }
   }
}

static void render_visualizer_oscilloscope(uint32_t *fb, int width, int height) {
   int cy = ui_hidden ? (height / 2) : 240;
   int start_x = 40;
   int end_x = width - 40;
   int span_x = end_x - start_x;
   int amp = ui_hidden ? 100 : 55;

   uint32_t beam_color = get_vis_color(0.2f, time_counter);
   uint32_t halo_color = get_vis_color(0.7f, time_counter);
   int hr = ((halo_color >> 16) & 0xFF) / 3;
   int hg = ((halo_color >> 8) & 0xFF) / 3;
   int hb = (halo_color & 0xFF) / 3;
   halo_color = (hr << 16) | (hg << 8) | hb;

   draw_line(fb, width, height, start_x, cy, end_x, cy, 0x112233);

   int prev_x = start_x;
   int prev_y = cy + (int)(vis_history[vis_history_index] * amp);

   for (int i = 1; i < span_x; i++) {
      int hist_idx = (vis_history_index + (i * VIS_SIZE) / span_x) % VIS_SIZE;
      float sample = vis_history[hist_idx];
      int cur_x = start_x + i;
      int cur_y = cy + (int)(sample * amp);

      draw_line(fb, width, height, prev_x, prev_y - 1, cur_x, cur_y - 1, halo_color);
      draw_line(fb, width, height, prev_x, prev_y + 1, cur_x, cur_y + 1, halo_color);
      draw_line(fb, width, height, prev_x, prev_y, cur_x, cur_y, beam_color);

      prev_x = cur_x;
      prev_y = cur_y;
   }
}

static void render_visualizer_circular_radar(uint32_t *fb, int width, int height) {
   int cx = width / 2;
   int cy = ui_hidden ? (height / 2) : 240;
   int base_r = ui_hidden ? 60 : 40;
   int num_rays = 64;

   float bass = vis_bars[0] + vis_bars[1] + vis_bars[2] + vis_bars[3];
   float pulse_r = base_r + bass * 0.15f;

   for (int i = 0; i < num_rays; i++) {
      float angle = (float)i * (2.0f * (float)M_PI / (float)num_rays) + time_counter * 0.015f;
      float cos_a = cosf(angle);
      float sin_a = sinf(angle);

      int bin_idx = (i < num_rays / 2) ? (i * 2) : ((num_rays - 1 - i) * 2);
      if (bin_idx >= NUM_SPECTRUM_BARS) bin_idx = NUM_SPECTRUM_BARS - 1;

      float ray_len = vis_bars[bin_idx] * 0.8f;
      if (ray_len < 3.0f) ray_len = 3.0f;

      int x0 = cx + (int)(cos_a * pulse_r);
      int y0 = cy + (int)(sin_a * pulse_r);
      int x1 = cx + (int)(cos_a * (pulse_r + ray_len));
      int y1 = cy + (int)(sin_a * (pulse_r + ray_len));

      uint32_t ray_color = get_vis_color((float)i / (float)num_rays, time_counter);
      draw_line(fb, width, height, x0, y0, x1, y1, ray_color);
   }

   int inner_r = (int)(pulse_r * 0.6f);
   for (int a = 0; a < 360; a += 10) {
      float rad = (float)a * (float)M_PI / 180.0f;
      int px = cx + (int)(cosf(rad) * inner_r);
      int py = cy + (int)(sinf(rad) * inner_r);
      draw_rect(fb, width, height, px, py, 2, 2, get_vis_color(0.5f, time_counter));
   }
}

static void render_visualizer_vu_meters(uint32_t *fb, int width, int height) {
   int meter_w = 240;
   int meter_h = 90;
   int cy = ui_hidden ? (height / 2) : 240;
   int y = cy - meter_h / 2;

   int x_l = width / 2 - meter_w - 15;
   int x_r = width / 2 + 15;

   for (int m = 0; m < 2; m++) {
      int mx = (m == 0) ? x_l : x_r;
      const char *label = (m == 0) ? "CH-L (LEFT)" : "CH-R (RIGHT)";
      float meter_val = (m == 0) ? vu_meter_l : vu_meter_r;

      draw_rect(fb, width, height, mx, y, meter_w, meter_h, 0x181c24);
      draw_line(fb, width, height, mx, y, mx + meter_w, y, 0x445566);
      draw_line(fb, width, height, mx, y + meter_h, mx + meter_w, y + meter_h, 0x445566);
      draw_line(fb, width, height, mx, y, mx, y + meter_h, 0x445566);
      draw_line(fb, width, height, mx + meter_w, y, mx + meter_w, y + meter_h, 0x445566);

      draw_string(fb, width, height, label, mx + 12, y + 8, 0x8899aa);
      draw_string(fb, width, height, "-20  -10  -5  -3   0  +3", mx + 18, y + 24, 0x778899);

      int pivot_x = mx + meter_w / 2;
      int pivot_y = y + meter_h + 30;
      int needle_len = 80;

      float angle_min = -0.70f;
      float angle_max = 0.70f;
      float needle_angle = angle_min + meter_val * (angle_max - angle_min);

      int nx = pivot_x + (int)(sinf(needle_angle) * needle_len);
      int ny = pivot_y - (int)(cosf(needle_angle) * needle_len);

      draw_line(fb, width, height, pivot_x, pivot_y - 10, nx, ny, 0xFF3333);
      draw_line(fb, width, height, pivot_x - 1, pivot_y - 10, nx - 1, ny, 0xFF5555);

      uint32_t led_color = (meter_val > 0.85f) ? 0xFF0000 : 0x330000;
      draw_rect(fb, width, height, mx + meter_w - 25, y + 8, 8, 8, led_color);
      draw_string(fb, width, height, "PEAK", mx + meter_w - 55, y + 8, 0x888888);
   }
}

/* Master Frame Rendering */
static void render_frame(uint32_t *fb, int width, int height) {
   float energy = 0.0f;
   if (core_status == STATUS_PLAYING) {
      float sum = 0.0f;
      for (int i = 0; i < VIS_SIZE; i++) {
         sum += vis_history[i] * vis_history[i];
      }
      energy = sqrtf(sum / (float)VIS_SIZE);
      if (energy > 1.0f) energy = 1.0f;
   }

   time_counter++;

   const ui_theme_t *theme = &ui_themes[active_ui_theme];

   int bg_top_r = (theme->bg_top >> 16) & 0xFF;
   int bg_top_g = (theme->bg_top >> 8) & 0xFF;
   int bg_top_b = theme->bg_top & 0xFF;
   
   int bg_bot_r = (theme->bg_bot >> 16) & 0xFF;
   int bg_bot_g = (theme->bg_bot >> 8) & 0xFF;
   int bg_bot_b = theme->bg_bot & 0xFF;

   float pulse_mod = energy * 15.0f + 5.0f * sinf(time_counter * 0.02f);
   bg_top_r = (int)(bg_top_r + pulse_mod);
   bg_top_g = (int)(bg_top_g + pulse_mod * 0.8f);
   bg_top_b = (int)(bg_top_b + pulse_mod * 1.2f);
   if (bg_top_r < 0) bg_top_r = 0; else if (bg_top_r > 255) bg_top_r = 255;
   if (bg_top_g < 0) bg_top_g = 0; else if (bg_top_g > 255) bg_top_g = 255;
   if (bg_top_b < 0) bg_top_b = 0; else if (bg_top_b > 255) bg_top_b = 255;

   bg_bot_r = (int)(bg_bot_r + pulse_mod * 0.5f);
   bg_bot_g = (int)(bg_bot_g + pulse_mod * 0.4f);
   bg_bot_b = (int)(bg_bot_b + pulse_mod * 0.6f);
   if (bg_bot_r < 0) bg_bot_r = 0; else if (bg_bot_r > 255) bg_bot_r = 255;
   if (bg_bot_g < 0) bg_bot_g = 0; else if (bg_bot_g > 255) bg_bot_g = 255;
   if (bg_bot_b < 0) bg_bot_b = 0; else if (bg_bot_b > 255) bg_bot_b = 255;

   uint32_t cur_bg_top = (bg_top_r << 16) | (bg_top_g << 8) | bg_top_b;
   uint32_t cur_bg_bot = (bg_bot_r << 16) | (bg_bot_g << 8) | bg_bot_b;

   draw_rect_gradient(fb, width, height, 0, 0, width, height, cur_bg_top, cur_bg_bot);

   if (!ui_hidden) {
      draw_line(fb, width, height, 20, 20, width - 20, 20, theme->border);
      draw_line(fb, width, height, 20, height - 20, width - 20, height - 20, theme->border);
      draw_line(fb, width, height, 20, 20, 20, height - 20, theme->border);
      draw_line(fb, width, height, width - 20, 20, width - 20, height - 20, theme->border);

      draw_line(fb, width, height, 30, 145, width - 30, 145, theme->border);

      if (playback_mode == PLAYBACK_RADIO) {
         draw_line(fb, width, height, 50, 95, width - 50, 95, theme->border);
         for (float f = 88.0f; f <= 108.0f; f += 0.2f) {
            int x = 50 + (int)((f - 88.0f) * 27.0f);
            int tick_h = 5;
            uint32_t t_color = theme->text_secondary;
            if (fmodf(f, 1.0f) < 0.01f || fmodf(f, 1.0f) > 0.99f) {
               tick_h = 10;
               t_color = theme->text_primary;
               char label[16];
               snprintf(label, sizeof(label), "%.0f", f);
               draw_string(fb, width, height, label, x - 4, 105, theme->text_secondary);
            }
            draw_line(fb, width, height, x, 95 - tick_h, x, 95, t_color);
         }

         float target_freq = (num_stations > 0) ? stations[current_station_idx].frequency : 98.0f;
         static float current_freq_pos = 98.0f;
         current_freq_pos += (target_freq - current_freq_pos) * 0.08f;

         int needle_x = 50 + (int)((current_freq_pos - 88.0f) * 27.0f);
         draw_line(fb, width, height, needle_x, 75, needle_x, 98, 0xFF3333);
         draw_line(fb, width, height, needle_x - 1, 75, needle_x - 1, 98, 0xFF3333);
         draw_line(fb, width, height, needle_x + 1, 75, needle_x + 1, 98, 0xFF3333);
         draw_rect(fb, width, height, needle_x - 2, 99, 5, 2, 0xFF5555);

         char title_str[64];
         snprintf(title_str, sizeof(title_str), "RADIO PRESET %d/%d", current_station_idx + 1, num_stations);
         draw_string_with_shadow(fb, width, height, title_str, 40, 35, theme->text_secondary, 0x000000);

         const char *name_str = (num_stations > 0 && stations[current_station_idx].name) ? 
                                stations[current_station_idx].name : "No Station";
         draw_string_with_shadow(fb, width, height, name_str, 40, 50, theme->text_primary, 0x000000);

         char freq_str[32];
         snprintf(freq_str, sizeof(freq_str), "%.1f FM MHz", target_freq);
         draw_string_with_shadow(fb, width, height, freq_str, width - 150, 35, theme->dial_accent, 0x000000);

         char url_display[80];
         const char *st_url = (num_stations > 0 && stations[current_station_idx].url) ?
                              stations[current_station_idx].url : "";
         snprintf(url_display, sizeof(url_display), "URL: %s", st_url);
         if (strlen(url_display) > 70) {
            url_display[67] = '.'; url_display[68] = '.'; url_display[69] = '.'; url_display[70] = '\0';
         }
         draw_string(fb, width, height, url_display, 40, 125, theme->text_secondary);
      } else {
         int bar_width = width - 100;
         int bar_x = 50;
         int bar_y = 83;
         draw_rect(fb, width, height, bar_x, bar_y, bar_width, 8, theme->border);

         uint32_t play_rate = (playback_mode == PLAYBACK_FILE_WAV) ? wav_sample_rate : mp3_sample_rate;
         uint64_t cur_sec = (play_rate > 0) ? pcm_samples_played / play_rate : 0;

         uint64_t tot_sec = 0;
         float progress = 0.0f;
         if (playback_mode == PLAYBACK_FILE_WAV && wav_sample_rate > 0 && wav_channels > 0 && wav_bits > 0) {
            uint64_t total_samples = file_total_bytes / ((uint32_t)wav_channels * ((wav_bits + 7) / 8));
            tot_sec = wav_sample_rate > 0 ? total_samples / wav_sample_rate : 0;
            progress = total_samples > 0 ? (float)pcm_samples_played / (float)total_samples : 0.0f;
         } else {
            tot_sec = mp3_total_seconds;
            progress = tot_sec > 0 ? (float)cur_sec / (float)tot_sec : 0.0f;
         }
         if (progress > 1.0f) progress = 1.0f;

         draw_rect(fb, width, height, bar_x, bar_y, (int)(bar_width * progress), 8, theme->dial_accent);

         char time_str[40];
         snprintf(time_str, sizeof(time_str), "%02llu:%02llu / %02llu:%02llu",
                  (unsigned long long)(cur_sec / 60), (unsigned long long)(cur_sec % 60),
                  (unsigned long long)(tot_sec / 60), (unsigned long long)(tot_sec % 60));

         char format_str[32];
         if (playback_mode == PLAYBACK_FILE_WAV)
            snprintf(format_str, sizeof(format_str), "WAV %uHz %dbit", wav_sample_rate, wav_bits);
         else
            snprintf(format_str, sizeof(format_str), "MP3 %ukbps %uHz", mp3_bitrate_bps / 1000, mp3_sample_rate);

         draw_string(fb, width, height, time_str, bar_x, bar_y + 12, theme->text_secondary);
         draw_string_with_shadow(fb, width, height, "LOCAL FILE PLAYBACK", 40, 35, theme->text_secondary, 0x000000);
         draw_string_with_shadow(fb, width, height, file_display_name, 40, 50, theme->text_primary, 0x000000);
         draw_string_with_shadow(fb, width, height, format_str, width - 180, 35, theme->dial_accent, 0x000000);

         char url_display[80];
         if (stations_is_dynamic && num_stations > 1) {
            snprintf(url_display, sizeof(url_display), "TRACK %d/%d: %s", current_station_idx + 1, num_stations, file_path);
         } else {
            snprintf(url_display, sizeof(url_display), "FILE: %s", file_path);
         }
         if (strlen(url_display) > 70) {
            url_display[67] = '.'; url_display[68] = '.'; url_display[69] = '.'; url_display[70] = '\0';
         }
         draw_string(fb, width, height, url_display, 40, 125, theme->text_secondary);
      }

      const char *status_text = "UNKNOWN";
      uint32_t status_color = 0xCCCCCC;
      switch (core_status) {
         case STATUS_IDLE:
            status_text = "PAUSED / IDLE";
            status_color = 0x33CCFF;
            break;
         case STATUS_CONNECTING:
            status_text = "CONNECTING...";
            status_color = 0xFF9900;
            break;
         case STATUS_BUFFERING:
            status_text = "BUFFERING...";
            status_color = 0xFFFF00;
            break;
         case STATUS_PLAYING:
            status_text = (playback_mode == PLAYBACK_RADIO) ? "PLAYING ONLINE" : "PLAYING FILE";
            status_color = 0x33FF33;
            break;
         case STATUS_ERROR:
            status_text = (playback_mode == PLAYBACK_RADIO) ? "CONNECTION ERROR / OFFLINE" : "FILE ERROR";
            status_color = 0xFF3333;
            break;
         case STATUS_EOF:
            status_text = "FILE FINISHED";
            status_color = 0x888888;
            break;
      }
      draw_string_with_shadow(fb, width, height, "STATUS:", 40, 160, theme->text_secondary, 0x000000);
      draw_string_with_shadow(fb, width, height, status_text, 100, 160, status_color, 0x000000);

      draw_string(fb, width, height, "VOLUME:", width - 180, 160, theme->text_secondary);
      int vol_bar_w = 80;
      draw_rect(fb, width, height, width - 120, 162, vol_bar_w, 4, theme->border);
      draw_rect(fb, width, height, width - 120, 162, (int)(vol_bar_w * current_volume), 4, theme->dial_accent);
   }

   /* Calculate Logarithmic Spectrum FFT */
   float fft_real[VIS_SIZE];
   float fft_imag[VIS_SIZE];
   for (int i = 0; i < VIS_SIZE; i++) {
      int idx = (vis_history_index + i) % VIS_SIZE;
      fft_real[i] = vis_history[idx] * fft_window[i];
      fft_imag[i] = 0.0f;
   }

   fast_fft(fft_real, fft_imag, VIS_SIZE);

   float mag[VIS_SIZE / 2];
   for (int i = 0; i < VIS_SIZE / 2; i++) {
      mag[i] = sqrtf(fft_real[i] * fft_real[i] + fft_imag[i] * fft_imag[i]);
   }

   for (int i = 0; i < NUM_SPECTRUM_BARS; i++) {
      int s = bar_bin_start[i];
      int e = bar_bin_end[i];
      float sum_val = 0.0f;
      for (int b = s; b <= e; b++) {
         sum_val += mag[b];
      }
      float avg_val = sum_val / (float)(e - s + 1);
      float target = avg_val * 70.0f * bar_weight[i];

      if (target > vis_bars[i]) {
         vis_bars[i] = target;
      } else {
         vis_bars[i] = vis_bars[i] * 0.78f + target * 0.22f;
      }

      if (vis_bars[i] > vis_peaks[i]) {
         vis_peaks[i] = vis_bars[i];
         vis_peak_hold[i] = 16;
      } else if (vis_peak_hold[i] > 0) {
         vis_peak_hold[i]--;
      } else {
         vis_peaks[i] -= 1.4f;
         if (vis_peaks[i] < 0.0f) vis_peaks[i] = 0.0f;
      }
   }

   /* Render chosen visualizer */
   switch (visualizer_mode) {
      case 1:
         render_visualizer_pharmacy_grid(fb, width, height);
         break;
      case 2:
         render_visualizer_oscilloscope(fb, width, height);
         break;
      case 3:
         render_visualizer_circular_radar(fb, width, height);
         break;
      case 4:
         render_visualizer_vu_meters(fb, width, height);
         break;
      case 0:
      default:
         render_visualizer_bars(fb, width, height);
         break;
   }

   if (!ui_hidden) {
      draw_line(fb, width, height, 30, height - 44, width - 30, height - 44, theme->border);
      const char *hint1 = (playback_mode == PLAYBACK_RADIO) ?
         "UP/DOWN: Select Preset  |  LEFT/RIGHT: Volume  |  SEL: Mode" :
         "UP/DOWN: Select Track   |  LEFT/RIGHT: Seek    |  SEL: Mode";
      draw_string(fb, width, height, hint1, 40, height - 34, theme->text_secondary);
      draw_string(fb, width, height, "A: Play/Restart  |  B: Pause/Stop  |  L1/R1: Vol  |  X: Toggle UI", 40, height - 20, theme->text_secondary);
   }
}

void retro_init(void) {
   frame_buf = (uint32_t *)calloc(frame_buf_width * frame_buf_height, sizeof(uint32_t));
   ring_buffer_init(&rb, 524288);
   pcm_fifo_init(&pcm_queue, PCM_QUEUE_SIZE);

   memset(vis_history, 0, sizeof(vis_history));
   memset(vis_bars, 0, sizeof(vis_bars));
   memset(vis_peaks, 0, sizeof(vis_peaks));
   memset(vis_peak_hold, 0, sizeof(vis_peak_hold));

   for (int i = 0; i < VIS_SIZE; i++) {
      fft_window[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i / (float)(VIS_SIZE - 1)));
      
      int rev = 0;
      for (int b = 0; b < 8; b++) {
         if (i & (1 << b)) rev |= (1 << (7 - b));
      }
      bit_rev_lut[i] = rev;
   }

   for (int k = 0; k < VIS_SIZE / 2; k++) {
      twiddle_cos[k] = cosf(-2.0f * (float)M_PI * (float)k / (float)VIS_SIZE);
      twiddle_sin[k] = sinf(-2.0f * (float)M_PI * (float)k / (float)VIS_SIZE);
   }

   for (int i = 0; i < NUM_SPECTRUM_BARS; i++) {
      float f0 = 35.0f * powf(16000.0f / 35.0f, (float)i / (float)NUM_SPECTRUM_BARS);
      float f1 = 35.0f * powf(16000.0f / 35.0f, (float)(i + 1) / (float)NUM_SPECTRUM_BARS);

      int b0 = (int)(f0 / ((float)OUTPUT_SAMPLE_RATE / (float)VIS_SIZE));
      int b1 = (int)(f1 / ((float)OUTPUT_SAMPLE_RATE / (float)VIS_SIZE));

      if (b0 < 1) b0 = 1;
      if (b1 < b0) b1 = b0;
      if (b1 >= VIS_SIZE / 2) b1 = VIS_SIZE / 2 - 1;

      bar_bin_start[i] = b0;
      bar_bin_end[i] = b1;
      bar_weight[i] = 1.0f + 2.8f * ((float)i / (float)NUM_SPECTRUM_BARS);
   }

   for (int dy = -10; dy <= 10; dy++) {
      for (int dx = -10; dx <= 10; dx++) {
         float angle = atan2f((float)dy, (float)dx);
         pharmacy_phase_lut[dy + 10][dx + 10] = (angle + (float)M_PI) / (2.0f * (float)M_PI);
      }
   }
}

void retro_deinit(void) {
   stop_current_thread();
   ring_buffer_free(&rb);
   pcm_fifo_free(&pcm_queue);

   if (stations_is_dynamic) {
      for (int i = 0; i < num_stations; i++) {
         free((void*)stations[i].name);
         free((void*)stations[i].url);
      }
      stations_is_dynamic = false;
   }
   num_stations = 0;

   if (frame_buf) {
      free(frame_buf);
      frame_buf = NULL;
   }
}

unsigned retro_api_version(void) {
   return RETRO_API_VERSION;
}

void retro_set_controller_port_device(unsigned port, unsigned device) {}

void retro_get_system_info(struct retro_system_info *info) {
   memset(info, 0, sizeof(*info));
   info->library_name     = "Radio";
   info->library_version  = "1.1";
   info->need_fullpath    = true;
   info->valid_extensions = "m3u|txt|mp3|wav";
}

void retro_get_system_av_info(struct retro_system_av_info *info) {
   info->timing.fps = 60.0;
   info->timing.sample_rate = (double)OUTPUT_SAMPLE_RATE;

   info->geometry.base_width = frame_buf_width;
   info->geometry.base_height = frame_buf_height;
   info->geometry.max_width = frame_buf_width;
   info->geometry.max_height = frame_buf_height;
   info->geometry.aspect_ratio = 16.0f / 9.0f;
}

void retro_set_environment(retro_environment_t cb) {
   environ_cb = cb;
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);

   static const struct retro_variable vars[] = {
      { "radio_station", "Active Preset; Preset 1|Preset 2|Preset 3|Preset 4|Preset 5|Preset 6|Preset 7|Preset 8|Preset 9|Preset 10|Preset 11|Preset 12|Preset 13|Preset 14|Preset 15|Preset 16|Preset 17|Preset 18|Preset 19|Preset 20|Preset 21|Preset 22|Preset 23|Preset 24|Preset 25|Preset 26|Preset 27|Preset 28|Preset 29|Preset 30|Preset 31|Preset 32|Preset 33|Preset 34|Preset 35|Preset 36|Preset 37|Preset 38|Preset 39|Preset 40|Preset 41|Preset 42|Preset 43|Preset 44|Preset 45|Preset 46|Preset 47|Preset 48|Preset 49|Preset 50|Preset 51|Preset 52|Preset 53|Preset 54|Preset 55|Preset 56|Preset 57|Preset 58|Preset 59|Preset 60|Preset 61|Preset 62|Preset 63|Preset 64|Preset 65|Preset 66|Preset 67|Preset 68|Preset 69|Preset 70|Preset 71|Preset 72|Preset 73|Preset 74|Preset 75|Preset 76|Preset 77|Preset 78|Preset 79|Preset 80|Preset 81|Preset 82|Preset 83|Preset 84|Preset 85|Preset 86|Preset 87|Preset 88|Preset 89|Preset 90|Preset 91|Preset 92|Preset 93|Preset 94|Preset 95|Preset 96|Preset 97|Preset 98|Preset 99|Preset 100" },
      { "radio_visualizer", "Visualizer Mode; FFT Spectrum Bars|LED Pharmacy Grid|Phosphor Oscilloscope|Circular Audio Radar|Analog VU Meters" },
      { "radio_volume", "Volume Level; 80%|100%|90%|70%|60%|50%|40%|30%|20%|10%|0%" },
      { "radio_ui_autohide", "Auto-Hide UI; Never|10 seconds|30 seconds|1 minute|5 minutes" },
      { "radio_ui_theme", "UI Color Theme; Classic Blue|Matrix Green|Retro Amber|Synthwave Pink|Cyberpunk Orange" },
      { "radio_vis_palette", "Visualizer Palette; Neon Cyan-Pink|Acid Green|Fire & Brimstone|Rainbow Rave|Deep Space Purple" },
      { NULL, NULL }
   };
   environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);
}

void retro_set_video_refresh(retro_video_refresh_t cb) {
   video_cb = cb;
}

void retro_set_audio_sample(retro_audio_sample_t cb) {}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) {
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb) {
   input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb) {
   input_state_cb = cb;
}

void retro_reset(void) {}

void retro_run(void) {
   bool updated = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
      check_variables();
   }

   static int var_check_counter = 0;
   var_check_counter++;
   if (var_check_counter >= 30) {
      var_check_counter = 0;
      check_variables();
   }

   if (core_status == STATUS_EOF) {
      if (stations_is_dynamic && num_stations > 1) {
         change_station(1);
      } else {
         stop_current_thread();
      }
   }

   input_poll_cb();

   bool press_up = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP);
   bool press_down = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN);
   bool press_left = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT);
   bool press_right = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT);
   bool press_l = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L);
   bool press_r = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R);
   bool press_select = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT);
   bool press_a = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A);
   bool press_b = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B);
   bool press_x = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X);

   if (press_up || press_down || press_left || press_right || press_l || press_r || 
       press_select || press_a || press_b || press_x) {
      idle_frames = 0;
   } else {
      idle_frames++;
   }

   if (press_x && !last_input_state[RETRO_DEVICE_ID_JOYPAD_X]) {
      ui_hidden = !ui_hidden;
   } else if (autohide_limit_frames > 0) {
      ui_hidden = (idle_frames > autohide_limit_frames);
   }

   if (press_up && !last_input_state[RETRO_DEVICE_ID_JOYPAD_UP]) {
      change_station(-1);
   }
   if (press_down && !last_input_state[RETRO_DEVICE_ID_JOYPAD_DOWN]) {
      change_station(1);
   }
   if (press_select && !last_input_state[RETRO_DEVICE_ID_JOYPAD_SELECT]) {
      visualizer_mode = (visualizer_mode + 1) % 5;
   }
   if (press_a && !last_input_state[RETRO_DEVICE_ID_JOYPAD_A]) {
      reconnect_current_station();
   }
   if (press_b && !last_input_state[RETRO_DEVICE_ID_JOYPAD_B]) {
      pause_current_station();
   }

   if (press_l) {
      current_volume -= 0.015f;
      if (current_volume < 0.0f) current_volume = 0.0f;
   }
   if (press_r) {
      current_volume += 0.015f;
      if (current_volume > 1.0f) current_volume = 1.0f;
   }

   if (playback_mode == PLAYBACK_RADIO) {
      if (press_left) {
         current_volume -= 0.015f;
         if (current_volume < 0.0f) current_volume = 0.0f;
      }
      if (press_right) {
         current_volume += 0.015f;
         if (current_volume > 1.0f) current_volume = 1.0f;
      }
   } else if (file_total_bytes > 0) {
      uint64_t seek_amt = file_total_bytes / 20;
      size_t align = (playback_mode == PLAYBACK_FILE_WAV) ? wav_block_align : 1;
      if (align == 0) align = 4;

      if (press_left && !last_input_state[RETRO_DEVICE_ID_JOYPAD_LEFT]) {
         uint64_t actual_pos = file_bytes_read;
         uint64_t target = (actual_pos > seek_amt) ? actual_pos - seek_amt : 0;
         target = (target / align) * align;
         file_seek_target_bytes = target;
         file_seek_requested = true;
         mad_reset_decoder();
         pthread_mutex_lock(&rb.mutex);
         ring_buffer_clear(&rb);
         pthread_mutex_unlock(&rb.mutex);
         
         if (playback_mode == PLAYBACK_FILE_WAV && wav_channels > 0 && wav_bits > 0) {
            pcm_samples_played = target / ((uint32_t)wav_channels * ((wav_bits + 7) / 8));
         } else if (mp3_bitrate_bps > 0 && mp3_sample_rate > 0) {
            pcm_samples_played = (target * 8 / mp3_bitrate_bps) * mp3_sample_rate;
         }
      }
      if (press_right && !last_input_state[RETRO_DEVICE_ID_JOYPAD_RIGHT]) {
         uint64_t actual_pos = file_bytes_read;
         uint64_t target = actual_pos + seek_amt;
         if (target > file_total_bytes) target = file_total_bytes;
         target = (target / align) * align;
         file_seek_target_bytes = target;
         file_seek_requested = true;
         mad_reset_decoder();
         pthread_mutex_lock(&rb.mutex);
         ring_buffer_clear(&rb);
         pthread_mutex_unlock(&rb.mutex);

         if (playback_mode == PLAYBACK_FILE_WAV && wav_channels > 0 && wav_bits > 0) {
            pcm_samples_played = target / ((uint32_t)wav_channels * ((wav_bits + 7) / 8));
         } else if (mp3_bitrate_bps > 0 && mp3_sample_rate > 0) {
            pcm_samples_played = (target * 8 / mp3_bitrate_bps) * mp3_sample_rate;
         }
      }
   }

   last_input_state[RETRO_DEVICE_ID_JOYPAD_UP] = press_up;
   last_input_state[RETRO_DEVICE_ID_JOYPAD_DOWN] = press_down;
   last_input_state[RETRO_DEVICE_ID_JOYPAD_LEFT] = press_left;
   last_input_state[RETRO_DEVICE_ID_JOYPAD_RIGHT] = press_right;
   last_input_state[RETRO_DEVICE_ID_JOYPAD_L] = press_l;
   last_input_state[RETRO_DEVICE_ID_JOYPAD_R] = press_r;
   last_input_state[RETRO_DEVICE_ID_JOYPAD_SELECT] = press_select;
   last_input_state[RETRO_DEVICE_ID_JOYPAD_A] = press_a;
   last_input_state[RETRO_DEVICE_ID_JOYPAD_B] = press_b;
   last_input_state[RETRO_DEVICE_ID_JOYPAD_X] = press_x;

   pthread_mutex_lock(&rb.mutex);
   size_t avail = ring_buffer_read_avail(&rb);
   pthread_mutex_unlock(&rb.mutex);

   size_t mad_unconsumed = 0;
   if (mad_initialized && mad_stream_state.next_frame != NULL &&
       mad_stream_state.bufend > mad_stream_state.next_frame) {
      mad_unconsumed = (size_t)(mad_stream_state.bufend - mad_stream_state.next_frame);
   }
   size_t pipeline_bytes = avail + mad_unconsumed + (pcm_fifo_avail(&pcm_queue) * 4);

   if (core_status == STATUS_BUFFERING) {
      size_t start_threshold = (playback_mode == PLAYBACK_RADIO) ? 49152 : 4096;
      if (avail >= start_threshold) {
         core_status = STATUS_PLAYING;
      }
   } else if (core_status == STATUS_PLAYING) {
      if (pipeline_bytes < 2048) {
         core_status = STATUS_BUFFERING;
      }
   }

   int16_t audio_out_buf[AUDIO_FRAMES_PER_TICK * 2];

   if (core_status == STATUS_PLAYING) {
      decode_audio_into_queue();

      uint32_t src_rate = (playback_mode == PLAYBACK_FILE_WAV) ? wav_sample_rate : mp3_sample_rate;
      if (src_rate == 0) src_rate = OUTPUT_SAMPLE_RATE;

      resample_to_output(audio_out_buf, AUDIO_FRAMES_PER_TICK, src_rate);

      audio_batch_cb(audio_out_buf, AUDIO_FRAMES_PER_TICK);
      if (playback_mode != PLAYBACK_RADIO)
         pcm_samples_played += (uint64_t)((double)AUDIO_FRAMES_PER_TICK * ((double)src_rate / (double)OUTPUT_SAMPLE_RATE));

      float sum_sq_l = 0.0f;
      float sum_sq_r = 0.0f;
      for (size_t i = 0; i < AUDIO_FRAMES_PER_TICK; i++) {
         float sl = (float)audio_out_buf[i * 2] / 32768.0f;
         float sr = (float)audio_out_buf[i * 2 + 1] / 32768.0f;
         sum_sq_l += sl * sl;
         sum_sq_r += sr * sr;
      }
      float rms_l = sqrtf(sum_sq_l / (float)AUDIO_FRAMES_PER_TICK);
      float rms_r = sqrtf(sum_sq_r / (float)AUDIO_FRAMES_PER_TICK);
      vu_meter_l = vu_meter_l * 0.80f + rms_l * 0.20f;
      vu_meter_r = vu_meter_r * 0.80f + rms_r * 0.20f;
   } else {
      memset(audio_out_buf, 0, AUDIO_FRAMES_PER_TICK * 2 * sizeof(int16_t));
      audio_batch_cb(audio_out_buf, AUDIO_FRAMES_PER_TICK);

      for (size_t i = 0; i < AUDIO_FRAMES_PER_TICK / 8; i++) {
         vis_history[vis_history_index] = 0.0f;
         vis_history_index = (vis_history_index + 1) % VIS_SIZE;
      }
      vu_meter_l *= 0.85f;
      vu_meter_r *= 0.85f;
   }

   if (frame_buf) {
      render_frame(frame_buf, frame_buf_width, frame_buf_height);
      video_cb(frame_buf, frame_buf_width, frame_buf_height, frame_buf_width * sizeof(uint32_t));
   }
}

static void parse_playlist_line(char *line, float *next_freq, char *pending_name) {
   size_t len = strlen(line);
   while (len > 0 && (line[len-1] == '\r' || line[len-1] == '\n' || line[len-1] == ' ' || line[len-1] == '\t')) {
      line[len-1] = '\0';
      len--;
   }

   char *ptr = line;
   while (*ptr == ' ' || *ptr == '\t') ptr++;

   if (ptr[0] == '\0') return;

   if (strncmp(ptr, "#EXTINF:", 8) == 0) {
      char *comma = strrchr(ptr, ',');
      if (comma) {
         strncpy(pending_name, comma + 1, 255);
         pending_name[255] = '\0';
      }
   } else if (ptr[0] != '#') {
      const char *ext = strrchr(ptr, '.');
      bool is_local_file = (ptr[0] == '/') || (ptr[1] == ':') || 
                           (ext && (strcasecmp(ext, ".mp3") == 0 || strcasecmp(ext, ".wav") == 0));
      
      if (pending_name[0] != '\0') {
         stations[num_stations].name = strdup(pending_name);
      } else if (is_local_file) {
         const char *base = strrchr(ptr, '/');
         if (!base) base = strrchr(ptr, '\\');
         if (base) base++; else base = ptr;
         stations[num_stations].name = strdup(base);
      } else {
         char host[256];
         int port;
         char path_url[256];
         parse_url(ptr, host, &port, path_url);
         char auto_name[256];
         snprintf(auto_name, sizeof(auto_name), "Station: %s", host);
         stations[num_stations].name = strdup(auto_name);
      }

      char url_buf[512];
      if (strstr(ptr, "://") == NULL && !is_local_file) {
         snprintf(url_buf, sizeof(url_buf), "http://%s", ptr);
      } else {
         snprintf(url_buf, sizeof(url_buf), "%s", ptr);
      }
      stations[num_stations].url = strdup(url_buf);
      stations[num_stations].frequency = *next_freq;

      *next_freq += 1.2f;
      if (*next_freq > 107.9f) *next_freq = 88.1f;

      num_stations++;
      pending_name[0] = '\0';
   }
}

static bool load_playlist_file(const char *path) {
   FILE *f = fopen(path, "r");
   if (!f) return false;

   char line[512];
   char pending_name[256] = "";
   float next_freq = 88.1f;

   while (fgets(line, sizeof(line), f) && num_stations < MAX_STATIONS) {
      parse_playlist_line(line, &next_freq, pending_name);
   }
   fclose(f);
   return num_stations > 0;
}

static bool fetch_remote_playlist(const char *host, int port, const char *path) {
   struct addrinfo hints, *res = NULL;
   memset(&hints, 0, sizeof(hints));
   hints.ai_family = AF_INET;
   hints.ai_socktype = SOCK_STREAM;

   char port_str[16];
   snprintf(port_str, sizeof(port_str), "%d", port);

   if (getaddrinfo(host, port_str, &hints, &res) != 0) {
      return false;
   }

   int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
   if (s < 0) {
      freeaddrinfo(res);
      return false;
   }

#ifdef _WIN32
   DWORD timeout_ms = 3000;
   setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
   setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
#else
   struct timeval timeout;
   timeout.tv_sec = 3;
   timeout.tv_usec = 0;
   setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const void*)&timeout, sizeof(timeout));
   setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const void*)&timeout, sizeof(timeout));
#endif

   if (connect(s, res->ai_addr, res->ai_addrlen) < 0) {
      close(s);
      freeaddrinfo(res);
      return false;
   }
   freeaddrinfo(res);

   char req[512];
   snprintf(req, sizeof(req),
            "GET %s HTTP/1.0\r\n"
            "Host: %s\r\n"
            "User-Agent: RetroArch-RadioCore/1.1\r\n"
            "Accept: */*\r\n"
            "Connection: close\r\n\r\n",
            path, host);
   send(s, req, strlen(req), 0);

   char header_buf[4096];
   int header_len = 0;
   char c;
   bool found_end = false;
   while (header_len < (int)sizeof(header_buf) - 1) {
      int r = recv(s, &c, 1, 0);
      if (r <= 0) break;
      header_buf[header_len++] = c;
      header_buf[header_len] = '\0';
      if (header_len >= 4 && strcmp(header_buf + header_len - 4, "\r\n\r\n") == 0) {
         found_end = true;
         break;
      }
   }

   if (!found_end) {
      close(s);
      return false;
   }

   int status_code = 0;
   if (sscanf(header_buf, "HTTP/%*d.%*d %d", &status_code) != 1) {
      if (sscanf(header_buf, "ICY %d", &status_code) != 1) {
         status_code = 0;
      }
   }
   if (status_code != 200) {
      close(s);
      return false;
   }

   char line[512];
   int line_len = 0;
   char pending_name[256] = "";
   float next_freq = 88.1f;

   while (num_stations < MAX_STATIONS) {
      int r = recv(s, &c, 1, 0);
      if (r <= 0) {
         if (line_len > 0) {
            line[line_len] = '\0';
            parse_playlist_line(line, &next_freq, pending_name);
         }
         break;
      }

      if (c == '\n') {
         line[line_len] = '\0';
         parse_playlist_line(line, &next_freq, pending_name);
         line_len = 0;
      } else {
         if (line_len < (int)sizeof(line) - 1) {
            line[line_len++] = c;
         }
      }
   }

   close(s);
   return num_stations > 0;
}

static bool load_radio_browser_api(void) {
   if (fetch_remote_playlist("de1.api.radio-browser.info", 80, "/m3u/stations/topclick/100")) return true;
   if (fetch_remote_playlist("nl1.api.radio-browser.info", 80, "/m3u/stations/topclick/100")) return true;
   if (fetch_remote_playlist("at1.api.radio-browser.info", 80, "/m3u/stations/topclick/100")) return true;
   return false;
}

bool retro_load_game(const struct retro_game_info *game) {
   if (stations_is_dynamic) {
      for (int i = 0; i < num_stations; i++) {
         free((void*)stations[i].name);
         free((void*)stations[i].url);
      }
      stations_is_dynamic = false;
   }
   num_stations = 0;

   if (game && game->path && game->path[0] != '\0') {
      const char *ext = strrchr(game->path, '.');
      if (ext && (strcasecmp(ext, ".mp3") == 0 || strcasecmp(ext, ".wav") == 0)) {
         stations[0].url = strdup(game->path);
         const char *base = strrchr(game->path, '/');
         if (!base) base = strrchr(game->path, '\\');
         if (base) base++; else base = game->path;
         stations[0].name = strdup(base);
         stations[0].frequency = 88.1f;
         num_stations = 1;
         stations_is_dynamic = true;
      } else {
         if (load_playlist_file(game->path)) {
            stations_is_dynamic = true;
         }
      }
   }

   if (num_stations == 0) {
      char path[1024];
      const char *system_dir = NULL;
      if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir) && system_dir) {
         snprintf(path, sizeof(path), "%s/radio_stations.m3u", system_dir);
         if (load_playlist_file(path)) {
            stations_is_dynamic = true;
         }
      }
      
      if (num_stations == 0) {
         const char *save_dir = NULL;
         if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir) && save_dir) {
            snprintf(path, sizeof(path), "%s/radio_stations.m3u", save_dir);
            if (load_playlist_file(path)) {
               stations_is_dynamic = true;
            }
         }
      }
      
      if (num_stations == 0) {
         if (load_playlist_file("radio_stations.m3u")) {
            stations_is_dynamic = true;
         }
      }
   }

   if (num_stations == 0) {
      if (load_radio_browser_api()) {
         stations_is_dynamic = true;
      }
   }

   if (num_stations == 0) {
      stations[0] = (radio_station_t){"SomaFM Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3", 89.1f};
      stations[1] = (radio_station_t){"SomaFM Drone Zone", "http://ice1.somafm.com/dronezone-128-mp3", 92.3f};
      stations[2] = (radio_station_t){"SomaFM Space Station Soma", "http://ice1.somafm.com/spacestation-128-mp3", 95.5f};
      stations[3] = (radio_station_t){"SomaFM Indie Pop Rocks!", "http://ice1.somafm.com/indiepop-128-mp3", 98.7f};
      stations[4] = (radio_station_t){"SomaFM Secret Agent", "http://ice1.somafm.com/secretagent-128-mp3", 101.9f};
      stations[5] = (radio_station_t){"SomaFM Def Con Radio", "http://ice1.somafm.com/defcon-128-mp3", 104.3f};
      stations[6] = (radio_station_t){"SomaFM Boot Liquor", "http://ice1.somafm.com/bootliquor-128-mp3", 106.1f};
      stations[7] = (radio_station_t){"SomaFM Deep Space One", "http://ice1.somafm.com/deepspaceone-128-mp3", 107.9f};
      num_stations = 8;
      stations_is_dynamic = false;
   }

   current_station_idx = 0;
   check_variables();
   play_current_station();

   return true;
}

void retro_unload_game(void) {
   stop_current_thread();

   if (stations_is_dynamic) {
      for (int i = 0; i < num_stations; i++) {
         free((void*)stations[i].name);
         free((void*)stations[i].url);
      }
      stations_is_dynamic = false;
   }
   num_stations = 0;
}

unsigned retro_get_region(void) {
   return RETRO_REGION_NTSC;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num) {
   return false;
}

size_t retro_serialize_size(void) {
   return 0;
}

bool retro_serialize(void *data, size_t len) {
   return false;
}

bool retro_unserialize(const void *data, size_t len) {
   return false;
}

void *retro_get_memory_data(unsigned id) {
   return NULL;
}

size_t retro_get_memory_size(unsigned id) {
   return 0;
}

void retro_cheat_reset(void) {}

void retro_cheat_set(unsigned idx, bool enabled, const char *code) {}
