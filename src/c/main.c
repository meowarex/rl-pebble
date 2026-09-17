#include <pebble.h>

// Protocol, mirrored in WatchProtocol.kt and emu_driver.py

enum {
  CMD_TRACK = 1,
  CMD_LINES = 2,
  CMD_STATE = 3,
  CMD_STATUS = 4,
  CMD_SYNC = 5,

  CMD_HELLO = 20,
  CMD_PLAY_PAUSE = 21,
  CMD_NEXT = 22,
  CMD_PREV = 23,
  CMD_SYNC_REQ = 24,
  CMD_SETTINGS = 25,
};

enum {
  STATUS_IDLE = 0,
  STATUS_LOADING = 1,
  STATUS_READY = 2,
  STATUS_NO_LYRICS = 3,
  STATUS_ERROR = 4,
};

#define SEG_BG (1 << 0)
#define SEG_SPACE (1 << 1)
#define LINE_RIGHT (1 << 0)

// Chunk: u32 start, u32 hold, u8 nseg, u8 flags, then segments
// Segment: u16 rel, u16 dur, u8 flags, u8 len, text

typedef struct {
  uint16_t rel;
  uint16_t dur;
  uint16_t off;
  int16_t x;
  int16_t y;
  int16_t w;
  uint8_t len;
  uint8_t flags;
} Seg;

typedef struct {
  uint32_t start;
  uint32_t end;
  Seg *segs;
  char *text;
  int16_t height;
  uint16_t layout_gen;
  uint8_t nseg;
  uint8_t lflags;
  bool loaded;
  bool lead;
  bool big;
  uint8_t main_rows;
  uint8_t bg_rows;
} Line;

static struct {
  uint32_t track_id;
  char title[64];
  char artist[64];
  uint32_t duration;
  uint8_t status;
  Line *lines;
  uint16_t count;
  bool connected;

  bool playing;
  int32_t anchor_pos;
  uint32_t anchor_phone;
  int64_t anchor_ms;
  bool anchor_synced;
} s;

// Settings

enum { MODE_LINE = 0, MODE_WORD = 1, MODE_SYLLABLE = 2 };
enum { STYLE_NORMAL = 0, STYLE_CENTERED = 1 };
enum { BACKLIGHT_DEFAULT = 0, BACKLIGHT_ALWAYS = 1, BACKLIGHT_TAP_TOGGLE = 2 };

// Only append fields, older saves keep new defaults
typedef struct {
  uint8_t mode;
  uint8_t text_size;
  uint8_t style;
  uint8_t backlight;
  bool bg_vocals;
  bool upcoming;
  bool progress;
  bool auto_open;
  int16_t sync_offset;
  uint32_t accent;
  uint32_t background;
} Settings;

#define SETTINGS_KEY 1
#define SETTINGS_VERSION_KEY 2
#define SETTINGS_VERSION 2

static Settings s_settings = {
  .mode = MODE_SYLLABLE,
  .text_size = 2,
  .style = STYLE_NORMAL,
  .backlight = BACKLIGHT_DEFAULT,
  .bg_vocals = true,
  .upcoming = true,
  .progress = true,
  .auto_open = true,
  .sync_offset = 0,
  .accent = 0xFFFFFF,
  .background = 0x000000,
};

static GColor prv_bg_color(void) {
  return PBL_IF_COLOR_ELSE(GColorFromHEX(s_settings.background), GColorBlack);
}

static GColor prv_lit_color(void) {
  return PBL_IF_COLOR_ELSE(GColorFromHEX(s_settings.accent), GColorWhite);
}

static Window *s_window;
static Layer *s_root;
static Layer *s_lyrics;
static AppTimer *s_timer;
static uint32_t s_inbox_size;
static uint16_t s_layout_gen = 1;

static int64_t prv_now_ms(void) {
  time_t sec;
  uint16_t ms;
  time_ms(&sec, &ms);
  return (int64_t)sec * 1000 + ms;
}

// Clock sync with the phone

#define SYNC_SLOTS 12
#define SYNC_BURST 8
#define SYNC_BURST_MS 300
#define SYNC_MINI_BURST 3
#define SYNC_PERIOD_MS 60000
#define SYNC_MAX_AGE_MS (5 * 60 * 1000)
#define UNSYNCED_LATENCY_MS 120

typedef struct {
  uint32_t offset;
  int32_t rtt;
  int64_t at;
} SyncSample;

static struct {
  SyncSample samples[SYNC_SLOTS];
  uint8_t count;
  uint8_t next;
  uint8_t burst_left;
  bool valid;
  uint32_t offset;
  AppTimer *timer;
} s_clock;

static void prv_send_cmd(uint8_t cmd);

// Lowest round trip wins
static void prv_sync_pick(void) {
  int64_t now = prv_now_ms();
  int best = -1;
  for (int i = 0; i < s_clock.count; i++) {
    if (now - s_clock.samples[i].at > SYNC_MAX_AGE_MS) continue;
    if (best < 0 || s_clock.samples[i].rtt < s_clock.samples[best].rtt) best = i;
  }
  if (best >= 0) {
    s_clock.valid = true;
    s_clock.offset = s_clock.samples[best].offset;
  }
}

static void prv_sync_timer(void *ctx) {
  s_clock.timer = NULL;
  uint32_t wait;
  if (s_clock.burst_left > 0) {
    // Faster radio keeps round trips short
    app_comm_set_sniff_interval(SNIFF_INTERVAL_REDUCED);
    prv_send_cmd(CMD_SYNC_REQ);
    s_clock.burst_left--;
    wait = SYNC_BURST_MS;
  } else {
    app_comm_set_sniff_interval(SNIFF_INTERVAL_NORMAL);
    s_clock.burst_left = SYNC_MINI_BURST;
    wait = SYNC_PERIOD_MS;
  }
  s_clock.timer = app_timer_register(wait, prv_sync_timer, NULL);
}

static void prv_sync_start_burst(void) {
  s_clock.burst_left = SYNC_BURST;
  if (s_clock.timer) app_timer_cancel(s_clock.timer);
  s_clock.timer = app_timer_register(50, prv_sync_timer, NULL);
}

static void prv_sync_reply(uint32_t t0, uint32_t t1, uint32_t t2) {
  uint32_t t3 = (uint32_t)prv_now_ms();
  int32_t rtt = (int32_t)(t3 - t0) - (int32_t)(t2 - t1);
  if (rtt < 0 || rtt > 10000) return;
  SyncSample *slot = &s_clock.samples[s_clock.next];
  // Assume symmetric latency
  slot->offset = (t1 - t0) - (uint32_t)(rtt / 2);
  slot->rtt = rtt;
  slot->at = prv_now_ms();
  s_clock.next = (s_clock.next + 1) % SYNC_SLOTS;
  if (s_clock.count < SYNC_SLOTS) s_clock.count++;
  prv_sync_pick();
}

static uint32_t prv_phone_now(void) {
  return (uint32_t)prv_now_ms() + s_clock.offset;
}

static int32_t prv_position(void) {
  if (!s.playing) {
    return s.anchor_pos - s_settings.sync_offset;
  }
  int64_t elapsed;
  if (s.anchor_synced && s_clock.valid) {
    elapsed = (int32_t)(prv_phone_now() - s.anchor_phone);
  } else {
    elapsed = prv_now_ms() - s.anchor_ms + UNSYNCED_LATENCY_MS;
  }
  int64_t pos = s.anchor_pos + elapsed - s_settings.sync_offset;
  if (s.duration > 0 && pos > (int64_t)s.duration) {
    pos = s.duration;
  }
  return (int32_t)pos;
}

// Lines

static int16_t *s_line_y;
static uint16_t s_line_y_valid;

static void prv_invalidate_layout(void) {
  s_layout_gen++;
  s_line_y_valid = 0;
}

static void prv_free_lines(void) {
  if (s.lines) {
    for (uint16_t i = 0; i < s.count; i++) {
      free(s.lines[i].segs);
      free(s.lines[i].text);
    }
    free(s.lines);
  }
  free(s_line_y);
  s_line_y = NULL;
  s.lines = NULL;
  s.count = 0;
  prv_invalidate_layout();
}

static inline uint16_t prv_u16(const uint8_t *p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t prv_u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void prv_parse_chunk(uint16_t index, const uint8_t *data, uint16_t size) {
  const uint8_t *p = data;
  const uint8_t *end = data + size;

  while (p + 10 <= end && index < s.count) {
    uint32_t start = prv_u32(p);
    uint32_t hold = prv_u32(p + 4);
    uint8_t nseg = p[8];
    uint8_t lflags = p[9];
    p += 10;

    // Validate and size the text first
    const uint8_t *q = p;
    uint16_t text_size = 0;
    for (uint8_t i = 0; i < nseg; i++) {
      if (q + 6 > end) return;
      uint8_t len = q[5];
      if (q + 6 + len > end) return;
      text_size += len + 1;
      q += 6 + len;
    }

    Line *line = &s.lines[index];
    free(line->segs);
    free(line->text);
    line->segs = NULL;
    line->text = NULL;
    line->nseg = 0;

    Seg *segs = nseg ? malloc(sizeof(Seg) * nseg) : NULL;
    char *text = text_size ? malloc(text_size) : NULL;
    if ((nseg && !segs) || (text_size && !text)) {
      free(segs);
      free(text);
      return;
    }

    uint16_t off = 0;
    bool lead = false;
    for (uint8_t i = 0; i < nseg; i++) {
      Seg *seg = &segs[i];
      seg->rel = prv_u16(p);
      seg->dur = prv_u16(p + 2);
      seg->flags = p[4];
      seg->len = p[5];
      seg->off = off;
      memcpy(text + off, p + 6, seg->len);
      text[off + seg->len] = '\0';
      off += seg->len + 1;
      p += 6 + seg->len;
      if (!(seg->flags & SEG_BG)) lead = true;
    }

    line->start = start;
    line->end = hold;
    line->nseg = nseg;
    line->lflags = lflags;
    line->segs = segs;
    line->text = text;
    line->lead = lead;
    line->loaded = true;
    line->layout_gen = 0;
    index++;
  }
  s_line_y_valid = 0;
}

static inline bool prv_line_shown(const Line *line) {
  return line->loaded && (line->lead || s_settings.bg_vocals);
}

static inline bool prv_seg_shown(const Seg *seg) {
  return !(seg->flags & SEG_BG) || s_settings.bg_vocals;
}

// Last started line, -1 before the first
static int prv_scroll_line(int32_t pos) {
  int cur = -1;
  for (int i = 0; i < s.count; i++) {
    if (!prv_line_shown(&s.lines[i])) continue;
    if ((int32_t)s.lines[i].start <= pos) {
      cur = i;
    } else {
      break;
    }
  }
  return cur;
}

static inline bool prv_line_active(const Line *line, int32_t pos) {
  return prv_line_shown(line) && (int32_t)line->start <= pos && pos < (int32_t)line->end;
}

// Layout

#define ROUND PBL_IF_ROUND_ELSE(true, false)
#define INSET_X PBL_IF_ROUND_ELSE(24, 6)
#define LINE_GAP 8

// Lines up to here use the full size
static int s_big_through;

static const char *const FONT_STEPS[] = {
  FONT_KEY_GOTHIC_14_BOLD, FONT_KEY_GOTHIC_18_BOLD, FONT_KEY_GOTHIC_24_BOLD, FONT_KEY_GOTHIC_28_BOLD,
};
static const int16_t FONT_STEP_H[] = {16, 20, 26, 30};

static int prv_main_step(bool big) {
  uint8_t size = s_settings.text_size > 2 ? 2 : s_settings.text_size;
  return size + (big ? 1 : 0);
}

static int prv_bg_step(bool big) {
  return big ? 1 : 0;
}

static GFont prv_main_font(bool big) {
  return fonts_get_system_font(FONT_STEPS[prv_main_step(big)]);
}

static int16_t prv_main_line_h(bool big) {
  return FONT_STEP_H[prv_main_step(big)];
}

static GFont prv_bg_font(bool big) {
  return fonts_get_system_font(FONT_STEPS[prv_bg_step(big)]);
}

static int16_t prv_bg_line_h(bool big) {
  return FONT_STEP_H[prv_bg_step(big)];
}

typedef enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT } LineAlign;

static LineAlign prv_line_align(const Line *line) {
  if (ROUND || s_settings.style == STYLE_CENTERED) return ALIGN_CENTER;
  return (line->lflags & LINE_RIGHT) ? ALIGN_RIGHT : ALIGN_LEFT;
}

static int16_t prv_text_w(const char *text, GFont font) {
  return graphics_text_layout_get_content_size(text, font, GRect(0, 0, 1000, 100),
                                               GTextOverflowModeFill, GTextAlignmentLeft).w;
}

static inline bool prv_in_group(const Seg *seg, bool bg) {
  return ((seg->flags & SEG_BG) != 0) == bg;
}

static void prv_align_row(Line *line, bool bg, int from, int to, int16_t row_w, int16_t width, LineAlign align) {
  if (align == ALIGN_LEFT) return;
  int16_t shift = align == ALIGN_CENTER ? (width - row_w) / 2 : width - row_w;
  for (int k = from; k < to; k++) {
    if (prv_in_group(&line->segs[k], bg)) line->segs[k].x += shift;
  }
}

// Word wraps one vocal group
static int16_t prv_layout_group(Line *line, bool bg, int16_t y0, int16_t width) {
  if (bg && !s_settings.bg_vocals) return 0;
  LineAlign align = prv_line_align(line);
  GFont font = bg ? prv_bg_font(line->big) : prv_main_font(line->big);
  int16_t line_h = bg ? prv_bg_line_h(line->big) : prv_main_line_h(line->big);
  int16_t sw = prv_text_w("a a", font) - prv_text_w("aa", font);
  if (sw < 3) sw = 4;
  // Wrap at full size so growing keeps rows
  GFont wrap_font = bg ? prv_bg_font(true) : prv_main_font(true);
  bool same_font = wrap_font == font;
  int16_t wrap_sw = same_font ? sw : prv_text_w("a a", wrap_font) - prv_text_w("aa", wrap_font);
  if (wrap_sw < 3) wrap_sw = 4;

  int16_t x = 0;
  int16_t wrap_x = 0;
  int16_t y = y0;
  int row_first = -1;
  int rows = 0;
  bool trailing_space = false;
  int n = line->nseg;

  int i = 0;
  while (i < n) {
    if (!prv_in_group(&line->segs[i], bg)) {
      i++;
      continue;
    }
    int j = i;
    int16_t wrap_w = 0;
    while (j < n) {
      Seg *seg = &line->segs[j];
      if (!prv_in_group(seg, bg)) break;
      seg->w = prv_text_w(line->text + seg->off, font);
      wrap_w += same_font ? seg->w : prv_text_w(line->text + seg->off, wrap_font);
      j++;
      if (seg->flags & SEG_SPACE) break;
    }

    if (wrap_x > 0 && wrap_x + wrap_w > width) {
      if (row_first >= 0) prv_align_row(line, bg, row_first, i, x - (trailing_space ? sw : 0), width, align);
      x = 0;
      wrap_x = 0;
      y += line_h;
      row_first = -1;
    }
    if (row_first < 0) {
      row_first = i;
      rows++;
    }
    for (int k = i; k < j; k++) {
      line->segs[k].x = x;
      line->segs[k].y = y;
      x += line->segs[k].w;
    }
    wrap_x += wrap_w;
    trailing_space = (line->segs[j - 1].flags & SEG_SPACE) != 0;
    if (trailing_space) {
      x += sw;
      wrap_x += wrap_sw;
    }
    i = j;
  }

  if (row_first >= 0) prv_align_row(line, bg, row_first, n, x - (trailing_space ? sw : 0), width, align);
  return rows * line_h;
}

static int16_t prv_content_width(void) {
  return layer_get_bounds(s_lyrics).size.w - 2 * INSET_X;
}

static void prv_layout_line(Line *line, bool big) {
  if (line->layout_gen == s_layout_gen && line->big == big) return;
  line->layout_gen = s_layout_gen;
  line->big = big;
  if (!line->loaded) {
    line->height = prv_main_line_h(big);
    return;
  }
  if (!prv_line_shown(line)) {
    line->height = 0;
    line->main_rows = line->bg_rows = 0;
    return;
  }
  int16_t width = prv_content_width();
  int16_t main_h = prv_layout_group(line, false, 0, width);
  int16_t bg_h = prv_layout_group(line, true, main_h, width);
  line->height = main_h + bg_h;
  line->main_rows = main_h / prv_main_line_h(big);
  line->bg_rows = bg_h / prv_bg_line_h(big);
}

// Content y of a line, laid out lazily
static int16_t prv_line_y(int i) {
  if (!s_line_y) {
    s_line_y = malloc(sizeof(int16_t) * (s.count + 1));
    if (!s_line_y) return 0;
    s_line_y_valid = 0;
  }
  if (s_line_y_valid == 0) {
    s_line_y[0] = 0;
    s_line_y_valid = 1;
  }
  while (s_line_y_valid <= i) {
    int k = s_line_y_valid - 1;
    prv_layout_line(&s.lines[k], k <= s_big_through);
    s_line_y[k + 1] = s_line_y[k] + s.lines[k].height + (s.lines[k].height ? LINE_GAP : 0);
    s_line_y_valid++;
  }
  return s_line_y[i];
}

// Scroll animation

#define SCROLL_MS 600

static struct {
  int line;
  int32_t from;
  int32_t to;
  int64_t started;
  uint32_t track_id;
  uint16_t gen;
} s_scroll = {.line = -2};

static struct {
  int line;
  int64_t started;
  int32_t width_scale;
} s_grow = {.line = -1};

// Cubic ease in out, per mille
static int32_t prv_ease(int32_t t) {
  if (t <= 0) return 0;
  if (t >= 1000) return 1000;
  if (t < 500) {
    return 4 * t * t / 1000 * t / 1000;
  }
  int32_t u = 1000 - t;
  return 1000 - 4 * u * u / 1000 * u / 1000;
}

static int32_t prv_scroll_value(int64_t now) {
  int32_t t = (int32_t)((now - s_scroll.started) * 1000 / SCROLL_MS);
  return s_scroll.from + (s_scroll.to - s_scroll.from) * prv_ease(t) / 1000;
}

static bool prv_scroll_animating(int64_t now) {
  return (now - s_scroll.started < SCROLL_MS && s_scroll.from != s_scroll.to) ||
         (s_grow.line >= 0 && now - s_grow.started < SCROLL_MS);
}

// Resting scroll for the current line
static int32_t prv_scroll_target(int anchor) {
  int32_t y = prv_line_y(anchor);
  if (s_settings.style != STYLE_CENTERED || !s_lyrics) return y;
  prv_line_y(anchor + 1);
  int16_t view_h = layer_get_bounds(s_lyrics).size.h;
  return y - (view_h * 2 / 5 - s.lines[anchor].height / 2);
}

static void prv_update_scroll(int32_t pos) {
  int target = prv_scroll_line(pos);
  int anchor = target < 0 ? 0 : target;
  int64_t now = prv_now_ms();

  if (s_scroll.track_id != s.track_id) {
    s_grow.line = -1;
    s_scroll.track_id = s.track_id;
    s_scroll.line = anchor;
    s_big_through = anchor;
    s_line_y_valid = 0;
    s_scroll.from = s_scroll.to = prv_scroll_target(anchor);
    s_scroll.started = 0;
    s_scroll.gen = s_layout_gen;
    return;
  }
  if (s_scroll.gen != s_layout_gen || s_line_y_valid == 0) {
    // Layout changed, re-anchor in place
    s_scroll.gen = s_layout_gen;
    s_scroll.from = s_scroll.to = prv_scroll_target(s_scroll.line < 0 ? 0 : s_scroll.line);
    s_scroll.started = 0;
  }
  if (anchor != s_scroll.line) {
    if (anchor > s_scroll.line) {
      s_grow.line = anchor;
      s_grow.started = now;
      s_grow.width_scale = 0;
    }
    s_scroll.from = prv_scroll_value(now);
    s_big_through = anchor;
    s_line_y_valid = 0;
    s_scroll.to = prv_scroll_target(anchor);
    s_scroll.line = anchor;
    s_scroll.started = now;
  }
}

// Rendering

// Recolour text pixels, or dither them on 1-bit
static void prv_fb_fill(GContext *ctx, GRect rect, GRect clip, GColor lit) {
  int16_t x_min = rect.origin.x > clip.origin.x ? rect.origin.x : clip.origin.x;
  int16_t x_max = rect.origin.x + rect.size.w;
  if (x_max > clip.origin.x + clip.size.w) x_max = clip.origin.x + clip.size.w;
  int16_t y_min = rect.origin.y > clip.origin.y ? rect.origin.y : clip.origin.y;
  int16_t y_max = rect.origin.y + rect.size.h;
  if (y_max > clip.origin.y + clip.size.h) y_max = clip.origin.y + clip.size.h;
  if (x_min >= x_max || y_min >= y_max) return;

  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (!fb) return;
#if defined(PBL_COLOR)
  const uint8_t bg = prv_bg_color().argb;
#endif
  for (int16_t y = y_min; y < y_max; y++) {
    GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, y);
    int16_t x0 = x_min < row.min_x ? row.min_x : x_min;
    int16_t x1 = x_max - 1 > row.max_x ? row.max_x : x_max - 1;
    for (int16_t x = x0; x <= x1; x++) {
#if defined(PBL_COLOR)
      if (row.data[x] != bg) row.data[x] = lit.argb;
#else
      (void)lit;
      if ((x + y) & 1) row.data[x / 8] &= ~(1 << (x % 8));
#endif
    }
  }
  graphics_release_frame_buffer(ctx, fb);
}

typedef enum { LIT_NONE, LIT_FULL, LIT_PARTIAL } LitState;

static LitState prv_seg_lit(const Line *line, int i, int32_t pos, int16_t *fill_px) {
  const Seg *seg = &line->segs[i];
  int32_t st = (int32_t)line->start + seg->rel;

  if (s_settings.mode == MODE_SYLLABLE) {
    int32_t en = st + seg->dur;
    if (pos < st) return LIT_NONE;
    if (pos >= en || seg->dur == 0) return LIT_FULL;
    *fill_px = (int16_t)((int32_t)seg->w * (pos - st) / seg->dur);
    return LIT_PARTIAL;
  }

  if (s_settings.mode == MODE_WORD) {
    // Only the current word is lit
    bool bg = (seg->flags & SEG_BG) != 0;
    int ws = i;
    for (int k = i - 1; k >= 0; k--) {
      if (!prv_in_group(&line->segs[k], bg)) continue;
      if (line->segs[k].flags & SEG_SPACE) break;
      ws = k;
    }
    int we = i;
    for (int k = i; k < line->nseg; k++) {
      if (!prv_in_group(&line->segs[k], bg)) continue;
      we = k;
      if (line->segs[k].flags & SEG_SPACE) break;
    }
    int32_t word_start = (int32_t)line->start + line->segs[ws].rel;
    if (pos < word_start) return LIT_NONE;
    int32_t until = word_start + 800;
    for (int k = we + 1; k < line->nseg; k++) {
      if (prv_in_group(&line->segs[k], bg)) {
        until = (int32_t)line->start + line->segs[k].rel;
        break;
      }
    }
    return pos < until ? LIT_FULL : LIT_NONE;
  }

  return LIT_FULL;
}

static void prv_draw_line(GContext *ctx, Line *line, int16_t ox, int16_t oy, int32_t pos, GRect clip) {
  if (!line->loaded) return;

  bool active = prv_line_active(line, pos);
  GFont main_font = prv_main_font(line->big);
  GFont bg_font = prv_bg_font(line->big);
  int16_t main_h = prv_main_line_h(line->big);
  int16_t bg_h = prv_bg_line_h(line->big);
  GColor lit = prv_lit_color();
  // Keep unsung text visible on dark grey
  GColor dim = PBL_IF_COLOR_ELSE(
      gcolor_equal(prv_bg_color(), GColorDarkGray) ? GColorLightGray : GColorDarkGray, GColorWhite);

  for (int i = 0; i < line->nseg; i++) {
    const Seg *seg = &line->segs[i];
    if (!prv_seg_shown(seg)) continue;
    bool bg = (seg->flags & SEG_BG) != 0;
    int16_t h = bg ? bg_h : main_h;
    GRect rect = GRect(ox + seg->x, oy + seg->y - h / 5, seg->w + 2, h + 8);
    if (rect.origin.y > clip.size.h || rect.origin.y + rect.size.h < 0) continue;

    int16_t fill = 0;
    LitState state = active ? prv_seg_lit(line, i, pos, &fill) : LIT_NONE;

#if defined(PBL_COLOR)
    graphics_context_set_text_color(ctx, state == LIT_FULL ? lit : dim);
    graphics_draw_text(ctx, line->text + seg->off, bg ? bg_font : main_font, rect,
                       GTextOverflowModeFill, GTextAlignmentLeft, NULL);
    if (state == LIT_PARTIAL) {
      GRect abs = GRect(clip.origin.x + rect.origin.x, clip.origin.y + rect.origin.y, fill, rect.size.h);
      prv_fb_fill(ctx, abs, clip, lit);
    }
#else
    (void)dim;
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, line->text + seg->off, bg ? bg_font : main_font, rect,
                       GTextOverflowModeFill, GTextAlignmentLeft, NULL);
    if (state != LIT_FULL) {
      GRect abs = GRect(clip.origin.x + rect.origin.x + fill, clip.origin.y + rect.origin.y,
                        rect.size.w - fill, rect.size.h);
      prv_fb_fill(ctx, abs, clip, lit);
    }
#endif
  }
}

#if defined(PBL_COLOR)
static uint8_t *s_scale_buf;
static size_t s_scale_cap;

static void prv_release_scale_buf(void) {
  free(s_scale_buf);
  s_scale_buf = NULL;
  s_scale_cap = 0;
}

// Fonts can't scale, so resample the framebuffer
static void prv_draw_line_scaled(GContext *ctx, Line *line, int16_t ox, int16_t oy, int32_t pos, GRect clip,
                                 int32_t scale_x, int32_t scale_y) {
  prv_draw_line(ctx, line, ox, oy, pos, clip);
  if (scale_x >= 998 && scale_y >= 998) return;

  const int16_t pad = 8;
  int16_t top = clip.origin.y + oy - pad;
  int16_t bottom = clip.origin.y + oy + line->height + pad;
  if (top < clip.origin.y) top = clip.origin.y;
  if (bottom > clip.origin.y + clip.size.h) bottom = clip.origin.y + clip.size.h;
  int16_t left = clip.origin.x;
  int16_t width = clip.size.w;
  int16_t rows = bottom - top;
  if (rows <= 0 || width <= 0 || width > 260) return;

  size_t need = (size_t)rows * width;
  if (need > s_scale_cap) {
    prv_release_scale_buf();
    s_scale_buf = malloc(need);
    if (!s_scale_buf) return;
    s_scale_cap = need;
  }

  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (!fb) return;
  const uint8_t bg = prv_bg_color().argb;

  for (int16_t r = 0; r < rows; r++) {
    GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, top + r);
    for (int16_t c = 0; c < width; c++) {
      int16_t x = left + c;
      s_scale_buf[r * width + c] = (x >= row.min_x && x <= row.max_x) ? row.data[x] : bg;
    }
  }

  LineAlign align = prv_line_align(line);
  int16_t anchor_x = align == ALIGN_CENTER ? left + ox + prv_content_width() / 2
                   : align == ALIGN_RIGHT  ? left + ox + prv_content_width()
                                           : left + ox;
  int16_t anchor_y = clip.origin.y + oy;

  // Source column per x, 24.8 fixed point
  static int32_t col_src[260];
  for (int16_t c = 0; c < width; c++) {
    col_src[c] = ((int32_t)(left + c - anchor_x) * 256000) / scale_x + (int32_t)(anchor_x - left) * 256;
  }

  for (int16_t r = 0; r < rows; r++) {
    int16_t y = top + r;
    GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, y);
    int32_t t8 = ((int32_t)(y - anchor_y) * 256000) / scale_y + (int32_t)(anchor_y - top) * 256;
    int32_t sy = t8 >> 8;
    int32_t fy = t8 & 0xFF;
    const uint8_t *row0 = (sy >= 0 && sy < rows) ? &s_scale_buf[sy * width] : NULL;
    const uint8_t *row1 = (sy + 1 >= 0 && sy + 1 < rows) ? &s_scale_buf[(sy + 1) * width] : NULL;

    for (int16_t x = row.min_x < left ? left : row.min_x; x <= row.max_x && x < left + width; x++) {
      int32_t s8 = col_src[x - left];
      int32_t sx = s8 >> 8;
      int32_t fx = s8 & 0xFF;
      bool in0 = sx >= 0 && sx < width;
      bool in1 = sx + 1 >= 0 && sx + 1 < width;
      uint8_t p00 = (row0 && in0) ? row0[sx] : bg;
      uint8_t p10 = (row0 && in1) ? row0[sx + 1] : bg;
      uint8_t p01 = (row1 && in0) ? row1[sx] : bg;
      uint8_t p11 = (row1 && in1) ? row1[sx + 1] : bg;
      if (p00 == p10 && p00 == p01 && p00 == p11) {
        row.data[x] = p00;
        continue;
      }
      // Bilinear blend of 2-bit channels
      int32_t w00 = (256 - fx) * (256 - fy);
      int32_t w10 = fx * (256 - fy);
      int32_t w01 = (256 - fx) * fy;
      int32_t w11 = fx * fy;
      uint8_t out = 0xC0;
      for (int shift = 0; shift <= 4; shift += 2) {
        int32_t v = ((p00 >> shift) & 3) * w00 + ((p10 >> shift) & 3) * w10 + ((p01 >> shift) & 3) * w01 +
                    ((p11 >> shift) & 3) * w11;
        out |= ((v + 32768) >> 16) << shift;
      }
      row.data[x] = out;
    }
  }
  graphics_release_frame_buffer(ctx, fb);
}

static int16_t prv_small_height(const Line *line) {
  return line->main_rows * prv_main_line_h(false) + line->bg_rows * prv_bg_line_h(false);
}

// Small to big text width ratio
static int32_t prv_measure_width_scale(const Line *line) {
  int32_t big_w = 0;
  int32_t small_w = 0;
  GFont small_font = prv_main_font(false);
  for (int i = 0; i < line->nseg; i++) {
    const Seg *seg = &line->segs[i];
    if (seg->flags & SEG_BG) continue;
    big_w += seg->w;
    small_w += prv_text_w(line->text + seg->off, small_font);
  }
  if (big_w <= 0) return prv_main_line_h(false) * 1000 / prv_main_line_h(true);
  int32_t scale = small_w * 1000 / big_w;
  return scale < 500 ? 500 : (scale > 1000 ? 1000 : scale);
}
#endif

static void prv_draw_centered(GContext *ctx, const char *text, GFont font, GRect rect, GColor color) {
  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(ctx, text, font, rect, GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentCenter, NULL);
}

#define HEADER_H PBL_IF_ROUND_ELSE(38, 20)
#define FOOTER_H PBL_IF_ROUND_ELSE(30, 12)

static void prv_root_update(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  int16_t w = bounds.size.w;
  int16_t h = bounds.size.h;
  graphics_context_set_fill_color(ctx, prv_bg_color());
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  GFont tiny = fonts_get_system_font(FONT_KEY_GOTHIC_14);
  GColor grey = PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite);

  if (s.title[0]) {
    prv_draw_centered(ctx, s.title, tiny,
                      GRect(INSET_X + 8, PBL_IF_ROUND_ELSE(16, 1), w - 2 * INSET_X - 16, 18), grey);
  }

  int32_t pos = prv_position();
  if (s_settings.progress && s.duration > 0 && s.status != STATUS_IDLE) {
    int16_t bar_w = w - 2 * INSET_X - PBL_IF_ROUND_ELSE(40, 16);
    int16_t bar_x = (w - bar_w) / 2;
    int16_t bar_y = h - PBL_IF_ROUND_ELSE(22, 7);
    if (!s.playing) {
      graphics_context_set_fill_color(ctx, GColorWhite);
      graphics_fill_rect(ctx, GRect(bar_x, bar_y - 3, 2, 9), 0, GCornerNone);
      graphics_fill_rect(ctx, GRect(bar_x + 4, bar_y - 3, 2, 9), 0, GCornerNone);
      bar_x += 11;
      bar_w -= 11;
    }
    graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorDarkGray, GColorWhite));
#if defined(PBL_COLOR)
    graphics_fill_rect(ctx, GRect(bar_x, bar_y, bar_w, 3), 1, GCornersAll);
#else
    graphics_draw_line(ctx, GPoint(bar_x, bar_y + 1), GPoint(bar_x + bar_w, bar_y + 1));
#endif
    int16_t filled = (int16_t)((int64_t)bar_w * (pos < 0 ? 0 : pos) / s.duration);
    if (filled > bar_w) filled = bar_w;
    graphics_context_set_fill_color(ctx, prv_lit_color());
    graphics_fill_rect(ctx, GRect(bar_x, bar_y, filled, 3), 1, GCornersAll);
  }

  if (s.status != STATUS_READY || s.count == 0) {
    const char *msg;
    switch (s.status) {
      case STATUS_IDLE: msg = s.connected ? "Play something in TIDAL" : "Waiting for RL Manager..."; break;
      case STATUS_LOADING: msg = "Loading lyrics..."; break;
      case STATUS_NO_LYRICS: msg = "No synced lyrics for this track"; break;
      case STATUS_ERROR: msg = "Couldn't load lyrics"; break;
      default: msg = ""; break;
    }
    int16_t cy = h / 2;
    int16_t cw = w - 2 * INSET_X;
    GFont small = fonts_get_system_font(FONT_KEY_GOTHIC_18);
    if (s.status != STATUS_IDLE && s.title[0]) {
      prv_draw_centered(ctx, s.title, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
                        GRect(INSET_X, cy - 56, cw, 56), GColorWhite);
      prv_draw_centered(ctx, s.artist, small, GRect(INSET_X, cy, cw, 22), grey);
      prv_draw_centered(ctx, msg, tiny, GRect(INSET_X, cy + 26, cw, 36),
                        PBL_IF_COLOR_ELSE(GColorDarkGray, GColorWhite));
    } else {
      prv_draw_centered(ctx, msg, small, GRect(INSET_X, cy - 24, cw, 48), grey);
    }
  }
}

static void prv_lyrics_update(Layer *layer, GContext *ctx) {
  if (s.status != STATUS_READY || s.count == 0) return;

  GRect clip = layer_get_frame(layer);
  int32_t pos = prv_position();
  int64_t now = prv_now_ms();
  prv_update_scroll(pos);

  int32_t scroll = prv_scroll_value(now);
  const int16_t top = 2;

  // New current line grows to full size
  int grow_line = -1;
  int16_t grow_shift = 0;
#if defined(PBL_COLOR)
  int32_t grow_scale = 1000;
  int32_t grow_scale_x = 1000;
  if (s_grow.line >= 0 && s_grow.line < s.count) {
    int32_t t = (int32_t)((now - s_grow.started) * 1000 / SCROLL_MS);
    if (t < 1000) {
      prv_line_y(s_grow.line + 1);
      const Line *g = &s.lines[s_grow.line];
      int32_t p = prv_ease(t);
      int32_t small_scale = prv_main_line_h(false) * 1000 / prv_main_line_h(true);
      if (!s_grow.width_scale) s_grow.width_scale = prv_measure_width_scale(g);
      grow_line = s_grow.line;
      grow_scale = small_scale + (1000 - small_scale) * p / 1000;
      grow_scale_x = s_grow.width_scale + (1000 - s_grow.width_scale) * p / 1000;
      grow_shift = (int16_t)((g->height - prv_small_height(g)) * (1000 - p) / 1000);
    } else {
      s_grow.line = -1;
      prv_release_scale_buf();
    }
  }
#else
  s_grow.line = -1;
#endif

  for (int i = 0; i < s.count; i++) {
    int16_t y = top + prv_line_y(i) - scroll - (grow_line >= 0 && i > grow_line ? grow_shift : 0);
    if (y > clip.size.h) break;
    Line *line = &s.lines[i];
    prv_line_y(i + 1);
    if (y + line->height < 0) continue;
    if (!s_settings.upcoming && i > s_scroll.line && !prv_line_active(line, pos)) break;
#if defined(PBL_COLOR)
    if (i == grow_line) {
      prv_draw_line_scaled(ctx, line, INSET_X, y, pos, clip, grow_scale_x, grow_scale);
      continue;
    }
#endif
    prv_draw_line(ctx, line, INSET_X, y, pos, clip);
  }
}

// Frame scheduling, wake only on visible change

static void prv_schedule(void);

static void prv_timer_cb(void *ctx) {
  s_timer = NULL;
  layer_mark_dirty(s_root);
  prv_schedule();
}

static void prv_consider(int32_t *wait, int32_t at, int32_t pos) {
  int32_t d = at - pos;
  if (d > 0 && d < *wait) *wait = d;
}

static void prv_schedule(void) {
  if (s_timer) {
    app_timer_cancel(s_timer);
    s_timer = NULL;
  }
  int64_t now = prv_now_ms();
  if (s.status == STATUS_READY && s.count > 0 && s_lyrics) prv_update_scroll(prv_position());
  bool animating = prv_scroll_animating(now);
  int32_t wait = animating ? 33 : 1000;

  if (s.playing && s.status == STATUS_READY && s.count > 0) {
    int32_t pos = prv_position();
    int cur = prv_scroll_line(pos);
    int from = cur < 0 ? 0 : cur;
    // Nearby lines may overlap
    int lo = from - 2 < 0 ? 0 : from - 2;
    int hi = from + 2 >= s.count ? s.count - 1 : from + 2;
    for (int li = lo; li <= hi; li++) {
      const Line *line = &s.lines[li];
      if (!line->loaded) continue;
      prv_consider(&wait, (int32_t)line->start, pos);
      prv_consider(&wait, (int32_t)line->end, pos);
      if (!prv_line_active(line, pos)) continue;
      for (int i = 0; i < line->nseg; i++) {
        int32_t st = (int32_t)line->start + line->segs[i].rel;
        int32_t en = st + line->segs[i].dur;
        prv_consider(&wait, st, pos);
        if (s_settings.mode == MODE_SYLLABLE) {
          if (pos >= st && pos < en && wait > 40) wait = 40;
          prv_consider(&wait, en, pos);
        } else if (s_settings.mode == MODE_WORD) {
          prv_consider(&wait, st + 800, pos);
        }
      }
    }
  } else if (!animating && !(s.playing && s_settings.progress)) {
    return;
  }

  if (wait < 15) wait = 15;
  s_timer = app_timer_register(wait, prv_timer_cb, NULL);
}

// Backlight tap toggle

#define TAP_MAX_MS 300
#define DOUBLE_TAP_GAP_MS 300
#define TAP_SLOP_PX 30
#define LIGHT_REASSERT_MS 400

static bool s_touch_subscribed;
static bool s_accel_subscribed;
static bool s_tap_light;
static AppTimer *s_light_timer;
static AppTimer *s_single_tap_timer;
static uint8_t s_light_reasserts;

static struct {
  int64_t down_at;
  GPoint down;
} s_taps;

// Beat the system's timed tap light
static void prv_light_reassert(void *ctx) {
  s_light_timer = NULL;
  light_enable(s_tap_light);
  if (s_light_reasserts > 0) {
    s_light_reasserts--;
    s_light_timer = app_timer_register(LIGHT_REASSERT_MS, prv_light_reassert, NULL);
  }
}

static void prv_toggle_light(void) {
  s_tap_light = !s_tap_light;
  light_enable(s_tap_light);
  if (s_light_timer) app_timer_cancel(s_light_timer);
  s_light_reasserts = 2;
  s_light_timer = app_timer_register(LIGHT_REASSERT_MS, prv_light_reassert, NULL);
}

static void prv_single_tap_confirmed(void *ctx) {
  s_single_tap_timer = NULL;
  prv_toggle_light();
}

static inline bool prv_near(GPoint a, GPoint b) {
  int dx = a.x - b.x;
  int dy = a.y - b.y;
  return dx * dx + dy * dy <= TAP_SLOP_PX * TAP_SLOP_PX;
}

// Single tap toggles, double tap stays system
__attribute__((unused)) static void prv_touch_handler(const TouchEvent *event, void *context) {
  int64_t now = prv_now_ms();
  GPoint at = GPoint(event->x, event->y);
  switch (event->type) {
    case TouchEvent_Touchdown:
      s_taps.down_at = now;
      s_taps.down = at;
      if (s_single_tap_timer) {
        app_timer_cancel(s_single_tap_timer);
        s_single_tap_timer = NULL;
        s_taps.down_at = 0;
      }
      break;
    case TouchEvent_Liftoff:
      if (s_taps.down_at && now - s_taps.down_at <= TAP_MAX_MS && prv_near(at, s_taps.down)) {
        s_single_tap_timer = app_timer_register(DOUBLE_TAP_GAP_MS, prv_single_tap_confirmed, NULL);
      }
      break;
    default:
      break;
  }
}

// No touchscreen, use the motion gesture
static void prv_accel_tap_handler(AccelAxisType axis, int32_t direction) {
  prv_toggle_light();
}

static void prv_apply_backlight(void) {
  bool tap = s_settings.backlight == BACKLIGHT_TAP_TOGGLE;
  bool touch = tap && touch_service_is_enabled();
  if (touch && !s_touch_subscribed) {
    (void)touch_service_subscribe(prv_touch_handler, NULL);
    s_touch_subscribed = true;
  } else if (!touch && s_touch_subscribed) {
    (void)touch_service_unsubscribe();
    s_touch_subscribed = false;
  }
  bool accel = tap && !touch;
  if (accel && !s_accel_subscribed) {
    accel_tap_service_subscribe(prv_accel_tap_handler);
    s_accel_subscribed = true;
  } else if (!accel && s_accel_subscribed) {
    accel_tap_service_unsubscribe();
    s_accel_subscribed = false;
  }
  if (!tap) {
    s_tap_light = false;
    if (s_single_tap_timer) {
      app_timer_cancel(s_single_tap_timer);
      s_single_tap_timer = NULL;
    }
  }
  light_enable(s_settings.backlight == BACKLIGHT_ALWAYS || (tap && s_tap_light));
}

// Settings storage and messages

static void prv_apply_settings(void) {
  prv_apply_backlight();
  prv_invalidate_layout();
}

static void prv_load_settings(void) {
  int size = persist_get_size(SETTINGS_KEY);
  if (persist_read_int(SETTINGS_VERSION_KEY) == SETTINGS_VERSION && size > 0 && size <= (int)sizeof(Settings)) {
    persist_read_data(SETTINGS_KEY, &s_settings, size);
  }
  prv_apply_settings();
}

// Reads ints, including Clay's string values
static bool prv_read(DictionaryIterator *iter, uint32_t key, int32_t *out) {
  Tuple *t = dict_find(iter, key);
  if (!t) return false;
  if (t->type == TUPLE_CSTRING) {
    const char *c = t->value->cstring;
    int32_t sign = 1;
    int32_t v = 0;
    if (*c == '-') {
      sign = -1;
      c++;
    }
    while (*c >= '0' && *c <= '9') v = v * 10 + (*c++ - '0');
    *out = v * sign;
    return true;
  }
  switch (t->length) {
    case 1: *out = t->type == TUPLE_INT ? t->value->int8 : t->value->uint8; break;
    case 2: *out = t->type == TUPLE_INT ? t->value->int16 : t->value->uint16; break;
    default: *out = t->type == TUPLE_INT ? t->value->int32 : (int32_t)t->value->uint32; break;
  }
  return true;
}

static uint32_t prv_get(DictionaryIterator *iter, uint32_t key, uint32_t fallback) {
  int32_t v;
  return prv_read(iter, key, &v) ? (uint32_t)v : fallback;
}

static inline int32_t prv_clamp(int32_t v, int32_t lo, int32_t hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

static bool prv_receive_settings(DictionaryIterator *iter) {
  bool changed = false;
  int32_t v;
  if (prv_read(iter, MESSAGE_KEY_SetMode, &v)) { s_settings.mode = prv_clamp(v, 0, 2); changed = true; }
  if (prv_read(iter, MESSAGE_KEY_SetTextSize, &v)) { s_settings.text_size = prv_clamp(v, 0, 2); changed = true; }
  if (prv_read(iter, MESSAGE_KEY_SetStyle, &v)) { s_settings.style = prv_clamp(v, 0, 1); changed = true; }
  if (prv_read(iter, MESSAGE_KEY_SetBacklight, &v)) { s_settings.backlight = prv_clamp(v, 0, 2); changed = true; }
  if (prv_read(iter, MESSAGE_KEY_SetBgVocals, &v)) { s_settings.bg_vocals = v != 0; changed = true; }
  if (prv_read(iter, MESSAGE_KEY_SetUpcoming, &v)) { s_settings.upcoming = v != 0; changed = true; }
  if (prv_read(iter, MESSAGE_KEY_SetProgress, &v)) { s_settings.progress = v != 0; changed = true; }
  if (prv_read(iter, MESSAGE_KEY_SetAutoOpen, &v)) { s_settings.auto_open = v != 0; changed = true; }
  if (prv_read(iter, MESSAGE_KEY_SetSyncOffset, &v)) { s_settings.sync_offset = prv_clamp(v, -2000, 2000); changed = true; }
  if (prv_read(iter, MESSAGE_KEY_SetAccent, &v)) { s_settings.accent = (uint32_t)v & 0xFFFFFF; changed = true; }
  if (prv_read(iter, MESSAGE_KEY_SetBgColor, &v)) { s_settings.background = (uint32_t)v & 0xFFFFFF; changed = true; }
  if (changed) {
    persist_write_int(SETTINGS_VERSION_KEY, SETTINGS_VERSION);
    persist_write_data(SETTINGS_KEY, &s_settings, sizeof(Settings));
    prv_apply_settings();
  }
  return changed;
}

// AppMessage

static void prv_send_cmd(uint8_t cmd) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) return;
  dict_write_uint8(iter, MESSAGE_KEY_Cmd, cmd);
  if (cmd == CMD_SYNC_REQ) {
    dict_write_uint32(iter, MESSAGE_KEY_WatchTime, (uint32_t)prv_now_ms());
  }
  if (cmd == CMD_HELLO) {
    dict_write_uint32(iter, MESSAGE_KEY_InboxSize, s_inbox_size);
  }
  if (cmd == CMD_HELLO || cmd == CMD_SETTINGS) {
    dict_write_uint8(iter, MESSAGE_KEY_SetAutoOpen, s_settings.auto_open ? 1 : 0);
  }
  app_message_outbox_send();
}

static void prv_inbox_received(DictionaryIterator *iter, void *context) {
  if (prv_receive_settings(iter)) {
    // The phone opens the app, it needs this
    prv_send_cmd(CMD_SETTINGS);
    layer_mark_dirty(s_root);
    prv_schedule();
  }
  if (!dict_find(iter, MESSAGE_KEY_Cmd)) return;
  uint8_t cmd = (uint8_t)prv_get(iter, MESSAGE_KEY_Cmd, 0);
  uint32_t track_id = prv_get(iter, MESSAGE_KEY_TrackId, 0);
  s.connected = true;

  switch (cmd) {
    case CMD_TRACK: {
      uint16_t count = (uint16_t)prv_get(iter, MESSAGE_KEY_LineCount, 0);
      if (track_id != s.track_id || !s.lines || count != s.count) {
        prv_free_lines();
        if (count > 0) {
          s.lines = calloc(count, sizeof(Line));
          s.count = s.lines ? count : 0;
        }
      }
      s.track_id = track_id;
      Tuple *t;
      if ((t = dict_find(iter, MESSAGE_KEY_Title))) strncpy(s.title, t->value->cstring, sizeof(s.title) - 1);
      if ((t = dict_find(iter, MESSAGE_KEY_Artist))) strncpy(s.artist, t->value->cstring, sizeof(s.artist) - 1);
      s.duration = prv_get(iter, MESSAGE_KEY_Duration, s.duration);
      s.status = (uint8_t)prv_get(iter, MESSAGE_KEY_Status, s.status);
      break;
    }
    case CMD_LINES: {
      if (track_id != s.track_id || !s.lines) break;
      Tuple *chunk = dict_find(iter, MESSAGE_KEY_Chunk);
      if (!chunk) break;
      prv_parse_chunk((uint16_t)prv_get(iter, MESSAGE_KEY_ChunkIndex, 0), chunk->value->data, chunk->length);
      break;
    }
    case CMD_STATE: {
      if (track_id != s.track_id) break;
      s.playing = prv_get(iter, MESSAGE_KEY_Playing, 0) != 0;
      s.anchor_pos = (int32_t)prv_get(iter, MESSAGE_KEY_Position, 0);
      s.anchor_ms = prv_now_ms();
      s.anchor_synced = dict_find(iter, MESSAGE_KEY_PhoneTime) != NULL;
      s.anchor_phone = prv_get(iter, MESSAGE_KEY_PhoneTime, 0);
      break;
    }
    case CMD_SYNC: {
      prv_sync_reply(prv_get(iter, MESSAGE_KEY_WatchTime, 0), prv_get(iter, MESSAGE_KEY_PhoneTime, 0),
                     prv_get(iter, MESSAGE_KEY_PhoneTime2, 0));
      break;
    }
    case CMD_STATUS: {
      uint8_t status = (uint8_t)prv_get(iter, MESSAGE_KEY_Status, s.status);
      if (track_id != s.track_id && status != STATUS_IDLE) break;
      s.status = status;
      if (status == STATUS_IDLE) {
        prv_free_lines();
        s.track_id = 0;
        s.title[0] = s.artist[0] = '\0';
        s.playing = false;
      }
      break;
    }
    default:
      break;
  }

  layer_mark_dirty(s_root);
  prv_schedule();
}

// Buttons

static void prv_select_click(ClickRecognizerRef rec, void *ctx) {
  // Pause locally, phone confirms
  s.anchor_pos = prv_position() + s_settings.sync_offset;
  s.anchor_ms = prv_now_ms();
  s.anchor_phone = prv_phone_now();
  s.anchor_synced = s_clock.valid;
  s.playing = !s.playing;
  layer_mark_dirty(s_root);
  prv_schedule();
  prv_send_cmd(CMD_PLAY_PAUSE);
}

static void prv_up_click(ClickRecognizerRef rec, void *ctx) {
  prv_send_cmd(CMD_PREV);
}

static void prv_down_click(ClickRecognizerRef rec, void *ctx) {
  prv_send_cmd(CMD_NEXT);
}

static void prv_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click);
  window_single_click_subscribe(BUTTON_ID_UP, prv_up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_down_click);
}

// App lifecycle

static void prv_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  s_root = layer_create(bounds);
  layer_set_update_proc(s_root, prv_root_update);
  layer_add_child(root, s_root);

  s_lyrics = layer_create(GRect(0, HEADER_H, bounds.size.w, bounds.size.h - HEADER_H - FOOTER_H));
  layer_set_update_proc(s_lyrics, prv_lyrics_update);
  layer_add_child(s_root, s_lyrics);
}

static void prv_window_unload(Window *window) {
  layer_destroy(s_lyrics);
  layer_destroy(s_root);
}

static void prv_init(void) {
  s_window = window_create();
  window_set_click_config_provider(s_window, prv_click_config);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = prv_window_load,
    .unload = prv_window_unload,
  });
  window_stack_push(s_window, true);
  prv_load_settings();

  app_message_register_inbox_received(prv_inbox_received);
  uint32_t inbox = app_message_inbox_size_maximum();
  if (inbox > 4096) inbox = 4096;
  s_inbox_size = inbox;
  app_message_open(inbox, 64);
  prv_send_cmd(CMD_HELLO);
  prv_sync_start_burst();
}

static void prv_deinit(void) {
  if (s_timer) app_timer_cancel(s_timer);
  if (s_clock.timer) app_timer_cancel(s_clock.timer);
  if (s_light_timer) app_timer_cancel(s_light_timer);
  if (s_single_tap_timer) app_timer_cancel(s_single_tap_timer);
  if (s_touch_subscribed) (void)touch_service_unsubscribe();
  if (s_accel_subscribed) accel_tap_service_unsubscribe();
  app_comm_set_sniff_interval(SNIFF_INTERVAL_NORMAL);
  light_enable(false);
  window_destroy(s_window);
  prv_free_lines();
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
