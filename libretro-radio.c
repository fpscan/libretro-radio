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

#include <libretro.h>

#define DR_MP3_IMPLEMENTATION
#include <dr_mp3.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define VIS_SIZE 256
#define AUDIO_FRAMES_PER_TICK 735
#define MAX_STATIONS 256


enum core_status_t {
   STATUS_IDLE = 0,
   STATUS_CONNECTING,
   STATUS_BUFFERING,
   STATUS_PLAYING,
   STATUS_ERROR
};


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

static ring_buffer_t rb;
static drmp3 mp3_decoder;
static bool drmp3_initialized = false;

static pthread_t conn_thread;
static volatile bool cancel_thread = false;
static volatile bool thread_active = false;
static volatile int sockfd = -1;
static char target_url[512] = "";

static float vis_history[VIS_SIZE];
static int vis_history_index = 0;
static float vis_bars[VIS_SIZE / 2];
static float vis_ordered[VIS_SIZE];
static float fft_window[VIS_SIZE];

static int last_option_station_idx = -1;
static int last_option_visualizer_mode = -1;
static float last_option_volume = -1.0f;

static uint32_t idle_frames = 0;
static uint32_t autohide_limit_frames = 0;
static bool ui_hidden = false;

static bool last_input_state[16] = {false};


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


static size_t drmp3_read_cb(void *pUserData, void *pBuffer, size_t bytesToRead) {
   ring_buffer_t *r = (ring_buffer_t *)pUserData;
   size_t read_bytes = 0;

   pthread_mutex_lock(&r->mutex);
   size_t avail = ring_buffer_read_avail(r);
   if (avail > 0) {
      size_t to_read = (bytesToRead < avail) ? bytesToRead : avail;
      ring_buffer_read(r, (uint8_t *)pBuffer, to_read);
      read_bytes = to_read;
   }
   pthread_mutex_unlock(&r->mutex);

   return read_bytes;
}


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
      sprintf(port_str, "%d", port);

      if (getaddrinfo(host, port_str, &hints, &res) != 0) {
         break;
      }

      int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
      if (s < 0) {
         freeaddrinfo(res);
         break;
      }
      sockfd = s;

      
      struct timeval timeout;
      timeout.tv_sec = 4;
      timeout.tv_usec = 0;
      setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
      setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

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
               "User-Agent: RetroArch-Radio/1.0\r\n"
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
      sscanf(header_buf, "HTTP/%*d.%*d %d", &status_code);

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
      if (sockfd >= 0) close(sockfd);
      sockfd = -1;
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

      int r = recv(sockfd, temp_buf, sizeof(temp_buf), 0);
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

   if (sockfd >= 0) close(sockfd);
   sockfd = -1;

   if (!cancel_thread) {
      core_status = STATUS_ERROR;
   }

   return NULL;
}

static void stop_current_thread(void) {
   if (thread_active) {
      cancel_thread = true;
      if (sockfd >= 0) {
         close(sockfd);
         sockfd = -1;
      }
      pthread_join(conn_thread, NULL);
      thread_active = false;
   }
   if (drmp3_initialized) {
      drmp3_uninit(&mp3_decoder);
      drmp3_initialized = false;
   }
   pthread_mutex_lock(&rb.mutex);
   ring_buffer_clear(&rb);
   pthread_mutex_unlock(&rb.mutex);

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

static void reconnect_current_station(void) {
   start_connection_thread(stations[current_station_idx].url);
}

static void pause_current_station(void) {
   stop_current_thread();
}

static void change_station(int dir) {
   current_station_idx = (current_station_idx + dir + num_stations) % num_stations;
   start_connection_thread(stations[current_station_idx].url);
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
               fprintf(stderr, "[Radio] Station preset changed via options to Preset %d (Idx: %d)\n", sel_idx, target_idx);
               last_option_station_idx = target_idx;
               current_station_idx = target_idx;
               start_connection_thread(stations[current_station_idx].url);
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
      }
      if (last_option_visualizer_mode != mode) {
         fprintf(stderr, "[Radio] Visualizer mode changed to %s\n", var.value);
         last_option_visualizer_mode = mode;
         visualizer_mode = mode;
      }
   }

   var.key = "radio_volume";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
      int vol_pct = atoi(var.value);
      float vol = (float)vol_pct / 100.0f;
      if (last_option_volume != vol) {
         fprintf(stderr, "[Radio] Volume changed to %s\n", var.value);
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
         fprintf(stderr, "[Radio] UI Autohide changed to %s\n", var.value);
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
         fprintf(stderr, "[Radio] UI Theme changed to %s\n", var.value);
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
         fprintf(stderr, "[Radio] Visualizer Palette changed to %s\n", var.value);
         active_vis_palette = palette;
      }
   }
}


static void fft(float *real, float *imag, int n) {
   int i, j;
   for (i = 0, j = 0; i < n; i++) {
      if (j > i) {
         float temp = real[i]; real[i] = real[j]; real[j] = temp;
         temp = imag[i]; imag[i] = imag[j]; imag[j] = temp;
      }
      int m = n / 2;
      while (m >= 1 && j >= m) {
         j -= m;
         m /= 2;
      }
      j += m;
   }
   for (int len = 2; len <= n; len <<= 1) {
      float angle = -2.0f * M_PI / len;
      float wreal = cosf(angle);
      float wimag = sinf(angle);
      for (i = 0; i < n; i += len) {
         float ur = 1.0f;
         float ui = 0.0f;
         for (j = 0; j < len / 2; j++) {
            int idx1 = i + j;
            int idx2 = i + j + len / 2;
            float tr = real[idx2] * ur - imag[idx2] * ui;
            float ti = real[idx2] * ui + imag[idx2] * ur;
            real[idx2] = real[idx1] - tr;
            imag[idx2] = imag[idx1] - ti;
            real[idx1] = real[idx1] + tr;
            imag[idx1] = imag[idx1] + ti;
            float next_ur = ur * wreal - ui * wimag;
            float next_ui = ur * wimag + ui * wreal;
            ur = next_ur;
            ui = next_ui;
         }
      }
   }
}


static void draw_char(uint32_t *fb, int fb_w, int fb_h, char c, int x, int y, uint32_t color) {
   if (c < 32 || c > 127) return;
   int idx = c - 32;
   for (int row = 0; row < 8; row++) {
      uint8_t bits = font_8x8[idx][row];
      int draw_y = y + row;
      if (draw_y < 0 || draw_y >= fb_h) continue;
      for (int col = 0; col < 8; col++) {
         int draw_x = x + col;
         if (draw_x < 0 || draw_x >= fb_w) continue;
         if (bits & (0x80 >> col)) {
            fb[draw_y * fb_w + draw_x] = color;
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

   for (int y = y_start; y < y_end; y++) {
      uint32_t *row = fb + y * fb_w;
      for (int x = x_start; x < x_end; x++) {
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

   int r_top = (color_top >> 16) & 0xFF;
   int g_top = (color_top >> 8) & 0xFF;
   int b_top = color_top & 0xFF;
   int r_bot = (color_bottom >> 16) & 0xFF;
   int g_bot = (color_bottom >> 8) & 0xFF;
   int b_bot = color_bottom & 0xFF;

   for (int y = y_start; y < y_end; y++) {
      float t = (float)(y - ry) / rh;
      int r = r_top + (int)((r_bot - r_top) * t);
      int g = g_top + (int)((g_bot - g_top) * t);
      int b = b_top + (int)((b_bot - b_top) * t);
      uint32_t color = (r << 16) | (g << 8) | b;

      uint32_t *row = fb + y * fb_w;
      for (int x = x_start; x < x_end; x++) {
         row[x] = color;
      }
   }
}

static void draw_line(uint32_t *fb, int fb_w, int fb_h, int x0, int y0, int x1, int y1, uint32_t color) {
   int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
   int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
   int err = dx + dy, e2;

   for (;;) {
      if (x0 >= 0 && x0 < fb_w && y0 >= 0 && y0 < fb_h) {
         fb[y0 * fb_w + x0] = color;
      }
      if (x0 == x1 && y0 == y1) break;
      e2 = 2 * err;
      if (e2 >= dy) { err += dy; x0 += sx; }
      if (e2 <= dx) { err += dx; y0 += sy; }
   }
}


static void render_frame(uint32_t *fb, int width, int height) {
   float energy = 0.0f;
   if (core_status == STATUS_PLAYING) {
      float sum = 0.0f;
      for (int i = 0; i < VIS_SIZE; i++) {
         sum += vis_ordered[i] * vis_ordered[i];
      }
      energy = sqrtf(sum / VIS_SIZE);
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

      draw_line(fb, width, height, 50, 95, width - 50, 95, theme->border);
      for (float f = 88.0f; f <= 108.0f; f += 0.2f) {
         int x = 50 + (int)((f - 88.0f) * 27.0f);
         int tick_h = 5;
         uint32_t t_color = theme->text_secondary;
         if (fmodf(f, 1.0f) < 0.01f || fmodf(f, 1.0f) > 0.99f) {
            tick_h = 10;
            t_color = theme->text_primary;
            char label[16];
            sprintf(label, "%.0f", f);
            draw_string(fb, width, height, label, x - 4, 105, theme->text_secondary);
         }
         draw_line(fb, width, height, x, 95 - tick_h, x, 95, t_color);
      }

      float target_freq = stations[current_station_idx].frequency;
      static float current_freq_pos = 98.0f;
      current_freq_pos += (target_freq - current_freq_pos) * 0.08f;

      int needle_x = 50 + (int)((current_freq_pos - 88.0f) * 27.0f);
      draw_line(fb, width, height, needle_x, 75, needle_x, 98, 0xFF3333);
      draw_line(fb, width, height, needle_x - 1, 75, needle_x - 1, 98, 0xFF3333);
      draw_line(fb, width, height, needle_x + 1, 75, needle_x + 1, 98, 0xFF3333);

      draw_rect(fb, width, height, needle_x - 2, 99, 5, 2, 0xFF5555);

      char title_str[64];
      sprintf(title_str, "RADIO PRESET %d/%d", current_station_idx + 1, num_stations);
      draw_string_with_shadow(fb, width, height, title_str, 40, 35, theme->text_secondary, 0x000000);

      draw_string_with_shadow(fb, width, height, stations[current_station_idx].name, 40, 50, theme->text_primary, 0x000000);

      char freq_str[32];
      sprintf(freq_str, "%.1f FM MHz", stations[current_station_idx].frequency);
      draw_string_with_shadow(fb, width, height, freq_str, width - 150, 35, theme->dial_accent, 0x000000);

      char url_display[80];
      snprintf(url_display, sizeof(url_display), "URL: %s", stations[current_station_idx].url);
      if (strlen(stations[current_station_idx].url) > 70) {
         url_display[67] = '.';
         url_display[68] = '.';
         url_display[69] = '.';
         url_display[70] = '\0';
      }
      draw_string(fb, width, height, url_display, 40, 125, theme->text_secondary);

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
            status_text = "PLAYING ONLINE";
            status_color = 0x33FF33;
            break;
         case STATUS_ERROR:
            status_text = "CONNECTION ERROR / OFFLINE";
            status_color = 0xFF3333;
            break;
      }
      draw_string_with_shadow(fb, width, height, "STATUS:", 40, 160, theme->text_secondary, 0x000000);
      draw_string_with_shadow(fb, width, height, status_text, 100, 160, status_color, 0x000000);

      draw_string(fb, width, height, "VOLUME:", width - 180, 160, theme->text_secondary);
      int vol_bar_w = 80;
      draw_rect(fb, width, height, width - 120, 162, vol_bar_w, 4, theme->border);
      draw_rect(fb, width, height, width - 120, 162, (int)(vol_bar_w * current_volume), 4, theme->dial_accent);
   }

   int cy = ui_hidden ? (height / 2) : 250;

   float vis_ordered_local[VIS_SIZE];
   for (int i = 0; i < VIS_SIZE; i++) {
      vis_ordered_local[i] = vis_history[(vis_history_index + i) % VIS_SIZE];
   }

   float fft_real[VIS_SIZE];
   float fft_imag[VIS_SIZE];
   for (int i = 0; i < VIS_SIZE; i++) {
      fft_real[i] = vis_ordered_local[i] * fft_window[i];
      fft_imag[i] = 0.0f;
   }

   fft(fft_real, fft_imag, VIS_SIZE);

   float mag[VIS_SIZE / 2];
   for (int i = 0; i < VIS_SIZE / 2; i++) {
      mag[i] = sqrtf(fft_real[i]*fft_real[i] + fft_imag[i]*fft_imag[i]);
   }

   for (int i = 0; i < VIS_SIZE / 2; i++) {
      float target = mag[i] * 65.0f;
      if (target > vis_bars[i]) {
         vis_bars[i] = target;
      } else {
         vis_bars[i] = vis_bars[i] * 0.82f + target * 0.18f;
      }
   }

   if (visualizer_mode == 0) {
      int num_bars = 60;
      int bar_gap = 2;
      int bar_width = (width - 80) / num_bars - bar_gap;
      int start_x = 40;
      int max_h = ui_hidden ? 160 : 100;
      int base_y = ui_hidden ? (height - 40) : 300;

      for (int i = 0; i < num_bars; i++) {
         int bin_idx = (i * 80) / num_bars;
         int h = (int)vis_bars[bin_idx];
         if (h > max_h) h = max_h;
         if (h < 2) h = 2;

         int x = start_x + i * (bar_width + bar_gap);
         int y = base_y - h;

         float t_bar = (float)i / num_bars;
         uint32_t color_top = get_vis_color(t_bar * 0.4f, time_counter);
         uint32_t color_bot = get_vis_color(0.5f + t_bar * 0.5f, time_counter);
         draw_rect_gradient(fb, width, height, x, y, bar_width, h, color_top, color_bot);
      }
   } else if (visualizer_mode == 1) {
      int cx = width / 2;
      float bass = vis_bars[0] + vis_bars[1] + vis_bars[2] + vis_bars[3];
      cross_anim_pos += 0.2f + bass * 0.05f;
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
      
      float treble = vis_bars[30] + vis_bars[31] + vis_bars[32] + vis_bars[33];
      static float border_phase = 0.0f;
      border_phase = fmodf(border_phase + 0.015f + treble * 0.003f, 1.0f);
      
      for (int dy = -10; dy <= 10; dy++) {
         for (int dx = -10; dx <= 10; dx++) {
            int adx = abs(dx);
            int ady = abs(dy);
            
            if (adx <= 3 || ady <= 3) {
               int x = cx + dx * step - led_size/2;
               int y = cy + dy * step - led_size/2;
               
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
                  int bin = r_dist * 5;
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

   if (!ui_hidden) {
      draw_line(fb, width, height, 30, height - 40, width - 30, height - 40, theme->border);
      draw_string(fb, width, height, "UP/DOWN: Select Preset  |  LEFT/RIGHT: Volume  |  L1/R1: Visualizer Mode", 40, height - 32, theme->text_secondary);
      draw_string(fb, width, height, "A: Connect/Play  |  B: Pause/Stop", 40, height - 22, theme->text_secondary);
   }
}

void retro_init(void) {
   frame_buf = (uint32_t *)calloc(frame_buf_width * frame_buf_height, sizeof(uint32_t));
   ring_buffer_init(&rb, 524288);
   memset(vis_history, 0, sizeof(vis_history));
   memset(vis_bars, 0, sizeof(vis_bars));

   for (int i = 0; i < VIS_SIZE; i++) {
      fft_window[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (VIS_SIZE - 1)));
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
   info->library_version  = "1.0";
   info->need_fullpath    = true;
   info->valid_extensions = "m3u|txt";
}

void retro_get_system_av_info(struct retro_system_av_info *info) {
   info->timing.fps = 60.0;
   info->timing.sample_rate = 44100.0;

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
      { "radio_visualizer", "Visualizer Mode; FFT Spectrum Bars|LED Pharmacy Grid" },
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

   input_poll_cb();

   bool press_up = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP);
   bool press_down = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN);
   bool press_left = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT);
   bool press_right = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT);
   bool press_l = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L);
   bool press_r = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R);
   bool press_a = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A);
   bool press_b = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B);

   if (press_up || press_down || press_left || press_right || press_l || press_r || press_a || press_b) {
      idle_frames = 0;
   } else {
      idle_frames++;
   }

   ui_hidden = (autohide_limit_frames > 0 && idle_frames > autohide_limit_frames);

   if (press_up && !last_input_state[RETRO_DEVICE_ID_JOYPAD_UP]) {
      change_station(-1);
   }
   if (press_down && !last_input_state[RETRO_DEVICE_ID_JOYPAD_DOWN]) {
      change_station(1);
   }
   if (press_l && !last_input_state[RETRO_DEVICE_ID_JOYPAD_L]) {
      visualizer_mode = (visualizer_mode + 1) % 2;
   }
   if (press_r && !last_input_state[RETRO_DEVICE_ID_JOYPAD_R]) {
      visualizer_mode = (visualizer_mode + 1) % 2;
   }
   if (press_a && !last_input_state[RETRO_DEVICE_ID_JOYPAD_A]) {
      reconnect_current_station();
   }
   if (press_b && !last_input_state[RETRO_DEVICE_ID_JOYPAD_B]) {
      pause_current_station();
   }

   if (press_left) {
      current_volume -= 0.015f;
      if (current_volume < 0.0f) current_volume = 0.0f;
   }
   if (press_right) {
      current_volume += 0.015f;
      if (current_volume > 1.0f) current_volume = 1.0f;
   }

   last_input_state[RETRO_DEVICE_ID_JOYPAD_UP] = press_up;
   last_input_state[RETRO_DEVICE_ID_JOYPAD_DOWN] = press_down;
   last_input_state[RETRO_DEVICE_ID_JOYPAD_LEFT] = press_left;
   last_input_state[RETRO_DEVICE_ID_JOYPAD_RIGHT] = press_right;
   last_input_state[RETRO_DEVICE_ID_JOYPAD_L] = press_l;
   last_input_state[RETRO_DEVICE_ID_JOYPAD_R] = press_r;
   last_input_state[RETRO_DEVICE_ID_JOYPAD_A] = press_a;
   last_input_state[RETRO_DEVICE_ID_JOYPAD_B] = press_b;

   
   size_t avail = 0;
   pthread_mutex_lock(&rb.mutex);
   avail = ring_buffer_read_avail(&rb);
   pthread_mutex_unlock(&rb.mutex);

   if (core_status == STATUS_BUFFERING) {
      if (avail >= 65536) { 
         core_status = STATUS_PLAYING;
      }
   } else if (core_status == STATUS_PLAYING) {
      if (avail < 1024) { 
         core_status = STATUS_BUFFERING;
      }
   }

   int16_t audio_out_buf[AUDIO_FRAMES_PER_TICK * 2];
   float audio_float_buf[AUDIO_FRAMES_PER_TICK * 2];

   if (core_status == STATUS_PLAYING) {
      if (!drmp3_initialized) {
         if (drmp3_init(&mp3_decoder, drmp3_read_cb, NULL, &rb, NULL)) {
            drmp3_initialized = true;
         }
      }

      size_t decoded = 0;
      if (drmp3_initialized) {
         decoded = (size_t)drmp3_read_f32(&mp3_decoder, AUDIO_FRAMES_PER_TICK, audio_float_buf);
      }

      if (decoded < AUDIO_FRAMES_PER_TICK) {
         memset(audio_float_buf + decoded * 2, 0, (AUDIO_FRAMES_PER_TICK - decoded) * 2 * sizeof(float));
      }

      for (size_t i = 0; i < AUDIO_FRAMES_PER_TICK; i++) {
         float fl = audio_float_buf[i * 2];
         float fr = audio_float_buf[i * 2 + 1];

         fl *= current_volume;
         fr *= current_volume;

         if (fl > 1.0f) fl = 1.0f;
         else if (fl < -1.0f) fl = -1.0f;
         if (fr > 1.0f) fr = 1.0f;
         else if (fr < -1.0f) fr = -1.0f;

         audio_out_buf[i * 2] = (int16_t)(fl * 32767.0f);
         audio_out_buf[i * 2 + 1] = (int16_t)(fr * 32767.0f);

         float mono = (fl + fr) * 0.5f;
         vis_history[vis_history_index] = mono;
         vis_history_index = (vis_history_index + 1) % VIS_SIZE;
      }

      audio_batch_cb(audio_out_buf, AUDIO_FRAMES_PER_TICK);
   } else {
      
      memset(audio_out_buf, 0, AUDIO_FRAMES_PER_TICK * 2 * sizeof(int16_t));
      audio_batch_cb(audio_out_buf, AUDIO_FRAMES_PER_TICK);

      for (size_t i = 0; i < AUDIO_FRAMES_PER_TICK / 8; i++) {
         vis_history[vis_history_index] = 0.0f;
         vis_history_index = (vis_history_index + 1) % VIS_SIZE;
      }
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
      if (pending_name[0] != '\0') {
         stations[num_stations].name = strdup(pending_name);
      } else {
         char host[256];
         int port;
         char path_url[256];
         parse_url(ptr, host, &port, path_url);
         char auto_name[256];
         sprintf(auto_name, "Station: %s", host);
         stations[num_stations].name = strdup(auto_name);
      }

      char url_buf[512];
      if (strstr(ptr, "://") == NULL) {
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

   fprintf(stderr, "[Radio] Loading playlist file: %s\n", path);

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
   sprintf(port_str, "%d", port);

   fprintf(stderr, "[Radio] Resolving API host: %s:%d\n", host, port);
   if (getaddrinfo(host, port_str, &hints, &res) != 0) {
      fprintf(stderr, "[Radio] Failed to resolve API host: %s\n", host);
      return false;
   }

   int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
   if (s < 0) {
      freeaddrinfo(res);
      return false;
   }

   struct timeval timeout;
   timeout.tv_sec = 2;
   timeout.tv_usec = 0;
   setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
   setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

   fprintf(stderr, "[Radio] Connecting to API server...\n");
   if (connect(s, res->ai_addr, res->ai_addrlen) < 0) {
      fprintf(stderr, "[Radio] Connection to API server failed\n");
      close(s);
      freeaddrinfo(res);
      return false;
   }
   freeaddrinfo(res);

   char req[512];
   snprintf(req, sizeof(req),
            "GET %s HTTP/1.0\r\n"
            "Host: %s\r\n"
            "User-Agent: RetroArch-RadioCore/1.0\r\n"
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
   sscanf(header_buf, "HTTP/%*d.%*d %d", &status_code);
   if (status_code != 200) {
      fprintf(stderr, "[Radio] API server returned HTTP status %d\n", status_code);
      close(s);
      return false;
   }

   fprintf(stderr, "[Radio] API response 200 OK. Parsing M3U data stream...\n");

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
   fprintf(stderr, "[Radio] Loaded %d stations from API.\n", num_stations);
   return num_stations > 0;
}

static bool load_radio_browser_api(void) {
   fprintf(stderr, "[Radio] Querying Radio Garden Style API...\n");
   if (fetch_remote_playlist("de1.api.radio-browser.info", 80, "/m3u/stations/topclick/100")) {
      return true;
   }
   if (fetch_remote_playlist("nl1.api.radio-browser.info", 80, "/m3u/stations/topclick/100")) {
      return true;
   }
   if (fetch_remote_playlist("at1.api.radio-browser.info", 80, "/m3u/stations/topclick/100")) {
      return true;
   }
   return false;
}

bool retro_load_game(const struct retro_game_info *game) {
   num_stations = 0;
   stations_is_dynamic = false;

   
   if (game && game->path && game->path[0] != '\0') {
      if (load_playlist_file(game->path)) {
         stations_is_dynamic = true;
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
      fprintf(stderr, "[Radio] No playlist found and API offline. Loading default SomaFM stations.\n");
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
   start_connection_thread(stations[current_station_idx].url);

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
