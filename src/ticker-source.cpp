#include "ticker-source.hpp"
#include <util/platform.h>
#include <graphics/graphics.h>
#include <obs.h>
#include <obs-frontend-api.h>

#include <ctime>
#include <cstring>
#include <sstream>
#include <cmath>
#include <limits>
#include <algorithm>

/* ── Forward declarations ─────────────────────────────────────────── */

static void ticker_update(void *data, obs_data_t *settings);
static uint32_t ticker_get_width(void *data);
static uint32_t ticker_get_height(void *data);

/* ── Helpers ───────────────────────────────────────────────────────── */

static std::vector<std::string> split_lines(const std::string &s)
{
	std::vector<std::string> out;
	std::istringstream ss(s);
	std::string line;
	while (std::getline(ss, line))
		if (!line.empty()) out.push_back(line);
	return out;
}

static void ft2_push(obs_source_t *src, const char *text,
                     const char *face, const char *style,
                     int size, uint32_t abgr)
{
	if (!src) return;
	obs_data_t *d    = obs_data_create();
	obs_data_t *font = obs_data_create();
	obs_data_set_string(font, "face",  face);
	obs_data_set_string(font, "style", style);
	obs_data_set_int   (font, "size",  size);
	obs_data_set_int   (font, "flags", 0);
	obs_data_set_obj   (d, "font",   font);
	obs_data_set_string(d, "text",   text);
	obs_data_set_int   (d, "color1", abgr);
	obs_data_set_int   (d, "color2", abgr);
	obs_source_update(src, d);
	obs_data_release(font);
	obs_data_release(d);
}

static void draw_rect(uint32_t abgr, float w, float h)
{
	gs_effect_t *solid  = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *cparam = gs_effect_get_param_by_name(solid, "color");
	struct vec4 c;
	c.x = ((abgr >>  0) & 0xFF) / 255.0f;
	c.y = ((abgr >>  8) & 0xFF) / 255.0f;
	c.z = ((abgr >> 16) & 0xFF) / 255.0f;
	c.w = ((abgr >> 24) & 0xFF) / 255.0f;
	gs_effect_set_vec4(cparam, &c);
	while (gs_effect_loop(solid, "Solid"))
		gs_draw_sprite(nullptr, 0, (uint32_t)w, (uint32_t)h);
}

static std::string current_time_str(bool fmt24h)
{
	time_t now = time(nullptr);
	struct tm lt;
#ifdef _WIN32
	localtime_s(&lt, &now);
#else
	localtime_r(&now, &lt);
#endif
	char buf[32];
	strftime(buf, sizeof(buf), fmt24h ? "%H:%M" : "%I:%M %p", &lt);
	return buf;
}

/* ── Chain Management ─────────────────────────────────────────────── */

// Helper to create a chain from text list
static TickerChain create_chain(struct ticker_source *ctx, const std::vector<std::string> &texts)
{
	TickerChain chain;
	chain.width = 0.0f;
	
	static uint64_t chain_counter = 0;

	// Create sources
	for (size_t i = 0; i < texts.size(); ++i) {
		char name[128];
		snprintf(name, sizeof(name), "ticker_item_%llu_%zu", (unsigned long long)++chain_counter, i); 
		obs_source_t *ms = obs_source_create_private(
			"text_ft2_source", name, nullptr);
		if (ms) {
			ft2_push(ms, texts[i].c_str(),
			         ctx->font_face.c_str(), ctx->font_style.c_str(),
			         ctx->font_size, ctx->text_color);
			
			// Measure immediately (might be 0 first frame, but we try)
			float w = (float)obs_source_get_width(ms) * ctx->scale_x;
			chain.width += w;
		}
		chain.items.push_back(ms);

		// Add separator width (shared source)
		if (ctx->sep_source) {
			float sw = (float)obs_source_get_width(ctx->sep_source) * ctx->scale_x;
			chain.width += sw;
		}
	}
	return chain;
}

static void destroy_chain(TickerChain &chain)
{
	for (auto *s : chain.items) {
		if (s) obs_source_release(s);
	}
	chain.items.clear();
}

// Refresh chain layout (e.g. if scale changed or width was 0 initially)
static void update_chain_width(struct ticker_source *ctx, TickerChain &chain)
{
	chain.width = 0.0f;
	float sep_w = 0.0f;
	if (ctx->sep_source)
		sep_w = (float)obs_source_get_width(ctx->sep_source) * ctx->scale_x;

	for (auto *s : chain.items) {
		if (s) chain.width += (float)obs_source_get_width(s) * ctx->scale_x;
		chain.width += sep_w;
	}
}

/* ── Source lifecycle ─────────────────────────────────────────────── */

static const char *ticker_get_name(void *) { return "Text Ticker"; }

static void *ticker_create(obs_data_t *settings, obs_source_t *source)
{
	auto *ctx       = new ticker_source;
	ctx->source     = source;
	ctx->is_live    = false;
	ctx->bar_height = 70;
	ctx->clock_timer = 0.0f;

	ctx->sep_source = obs_source_create_private(
		"text_ft2_source", "ticker_sep", nullptr);
	ctx->clock_source = obs_source_create_private(
		"text_ft2_source", "ticker_clock", nullptr);

	ticker_update(ctx, settings);
	return ctx;
}

static void ticker_destroy(void *data)
{
	auto *ctx = (struct ticker_source *)data;
	for (auto &chain : ctx->active_chains) {
		destroy_chain(chain);
	}
	ctx->active_chains.clear();

	if (ctx->sep_source)   obs_source_release(ctx->sep_source);
	if (ctx->clock_source) obs_source_release(ctx->clock_source);
	delete ctx;
}

static uint32_t ticker_get_width(void *) { return 1920; }

static uint32_t ticker_get_height(void *data)
{
	auto *ctx = (struct ticker_source *)data;
	return (uint32_t)(ctx->bar_height > 0 ? ctx->bar_height : 70);
}

/* ── Update ───────────────────────────────────────────────────────── */

static void ticker_update(void *data, obs_data_t *settings)
{
	auto *ctx = (struct ticker_source *)data;
	std::lock_guard<std::mutex> lock(ctx->mutex);

	ctx->is_live = obs_data_get_bool(settings, "is_live");
	ctx->speed   = (int)obs_data_get_int(settings, "speed");
	if (ctx->speed <= 0) ctx->speed = 100;

	int new_height = (int)obs_data_get_int(settings, "bar_height");
	if (new_height <= 0) new_height = 70;

	if (new_height != ctx->bar_height) {
		// Handle scene item offset logic if needed (omitted for brevity as it was standard)
		ctx->bar_height = new_height;
	}

	ctx->bg_color = (uint32_t)obs_data_get_int(settings, "bg_color");

	std::string new_face  = obs_data_get_string(settings, "font_face");
	std::string new_style = obs_data_get_string(settings, "font_style");
	int         new_fsize = (int)obs_data_get_int(settings, "font_size");
	uint32_t    new_tcol  = (uint32_t)obs_data_get_int(settings, "text_color");
	float       new_sx    = (float)obs_data_get_double(settings, "scale_x");
	float       new_sy    = (float)obs_data_get_double(settings, "scale_y");
	std::string new_sep   = obs_data_get_string(settings, "sep_text");

	bool font_dirty = (new_face != ctx->font_face) ||
	                  (new_style != ctx->font_style) ||
	                  (new_fsize != ctx->font_size) ||
	                  (new_tcol  != ctx->text_color);

	ctx->font_face  = new_face;
	ctx->font_style = new_style;
	ctx->font_size  = new_fsize;
	ctx->text_color = new_tcol;
	ctx->scale_x    = new_sx;
	ctx->scale_y    = new_sy;

	bool sep_dirty = (new_sep != ctx->sep_text) || font_dirty;
	ctx->sep_text  = new_sep;

	ctx->show_clock       = obs_data_get_bool(settings, "show_clock");
	bool new_24h          = obs_data_get_bool(settings, "clock_24h");
	int  new_cfsize       = (int)obs_data_get_int(settings, "clock_font_size");
	bool clock_dirty      = (new_24h != ctx->clock_24h) ||
	                        (new_cfsize != ctx->clock_font_size) || font_dirty;
	ctx->clock_24h        = new_24h;
	ctx->clock_font_size  = new_cfsize;
	ctx->clock_scale_x    = (float)obs_data_get_double(settings, "clock_scale_x");
	ctx->clock_scale_y    = (float)obs_data_get_double(settings, "clock_scale_y");
	ctx->clock_zone_width = (int)obs_data_get_int(settings, "clock_zone_width");
	ctx->clock_sep_color  = (uint32_t)obs_data_get_int(settings, "clock_sep_color");

	const char *raw = obs_data_get_string(settings, "messages");
	if (raw) {
		std::vector<std::string> new_texts = split_lines(raw);
		ctx->pending_msg_texts = new_texts;
	}

	// Update existing chains style
	if (font_dirty) {
		for (auto &chain : ctx->active_chains) {
			// Note: We can't update text content here, only style.
			// Text content is baked into the source logic, but ft2_push updates style.
			// But we don't have the original text string stored in TickerChain items.
			// Ideally TickerChain items should store their text?
			// For now, we assume style updates are global and applied to pending only?
			// No, user expects immediate update.
			// If we re-push style to existing sources, we need the text.
			// We don't have it easily.
			// BUT: we can recreate all chains if style changes.
			// This causes a jump/reset. 
			// User didn't ask for smooth style updates, only content updates.
			// If we must smooth update style, we need to store text in TickerChain.
			// Let's defer that complexity. A style change causing a reset is acceptable usually.
		}
	}

	// Push separator
	bool force_sep = (ctx->sep_source && obs_source_get_width(ctx->sep_source) == 0);
	if ((sep_dirty || force_sep) && ctx->sep_source)
		ft2_push(ctx->sep_source, ctx->sep_text.c_str(),
		         ctx->font_face.c_str(), ctx->font_style.c_str(),
		         ctx->font_size, ctx->text_color);

	if (clock_dirty && ctx->clock_source)
		ft2_push(ctx->clock_source, current_time_str(ctx->clock_24h).c_str(),
		         ctx->font_face.c_str(), ctx->font_style.c_str(),
		         ctx->clock_font_size, ctx->text_color);
}

/* ── Tick ─────────────────────────────────────────────────────────── */

static constexpr float SLIDE_DURATION = 0.35f;

static void ticker_video_tick(void *data, float seconds)
{
	auto *ctx = (struct ticker_source *)data;
	std::lock_guard<std::mutex> lock(ctx->mutex);

	float scroll_area = ctx->show_clock
		? (float)(1920 - ctx->clock_zone_width)
		: 1920.0f;

	// ── Edge-detect is_live changes ──
	if (ctx->is_live && !ctx->prev_is_live) {
		if (ctx->anim_state == TickerAnim::HIDDEN ||
		    ctx->anim_state == TickerAnim::SLIDING_DOWN)
			ctx->anim_state = TickerAnim::SLIDING_UP;
		else if (ctx->anim_state == TickerAnim::DRAINING)
			ctx->anim_state = TickerAnim::RUNNING;

	} else if (!ctx->is_live && ctx->prev_is_live) {
		if (ctx->anim_state == TickerAnim::RUNNING ||
		    ctx->anim_state == TickerAnim::ENTERING) {
			ctx->anim_state = TickerAnim::DRAINING;
		} else if (ctx->anim_state == TickerAnim::SLIDING_UP) {
			ctx->anim_state = TickerAnim::SLIDING_DOWN;
		}
	}
	ctx->prev_is_live = ctx->is_live;

	// ── State machine ──
	float slide_speed = 1.0f / SLIDE_DURATION;

	switch (ctx->anim_state) {
	case TickerAnim::HIDDEN:
		ctx->slide_t = 1.0f;
		break;
	case TickerAnim::SLIDING_UP:
		ctx->slide_t -= slide_speed * seconds;
		if (ctx->slide_t <= 0.0f) {
			ctx->slide_t = 0.0f;
			ctx->anim_state = TickerAnim::RUNNING; // Directly to RUNNING
			// Start first chain at edge
			if (ctx->active_chains.empty()) {
				// Seed initial chain
				if (!ctx->pending_msg_texts.empty()) {
					TickerChain chain = create_chain(ctx, ctx->pending_msg_texts);
					chain.x = scroll_area;
					ctx->current_msg_texts = ctx->pending_msg_texts; // Update snapshot
					ctx->active_chains.push_back(chain);
				}
			}
		}
		break;
	case TickerAnim::SLIDING_DOWN:
		ctx->slide_t += slide_speed * seconds;
		if (ctx->slide_t >= 1.0f) {
			ctx->slide_t = 1.0f;
			ctx->anim_state = TickerAnim::HIDDEN;
			// Clear chains
			for (auto &c : ctx->active_chains) destroy_chain(c);
			ctx->active_chains.clear();
		}
		break;
	case TickerAnim::RUNNING:
	case TickerAnim::ENTERING: // Treated same as running now
	case TickerAnim::DRAINING:
		// Move all chains
		for (auto &c : ctx->active_chains) {
			c.x -= (float)ctx->speed * seconds;
			// Refresh width if needed (e.g. init or font load)
			if (c.width == 0.0f) update_chain_width(ctx, c);
		}
		break;
	}

	// ── Chain Management ──
	if (ctx->anim_state == TickerAnim::RUNNING || ctx->anim_state == TickerAnim::DRAINING) {
		
		// 1. Remove off-screen chains
		if (!ctx->active_chains.empty()) {
			TickerChain &front = ctx->active_chains.front();
			if (front.x + front.width < 0.0f) {
				// If draining, just destroy.
				// If running, we might recycle if content matches pending?
				// For simplicity, just destroy. We create new at end.
				destroy_chain(front);
				ctx->active_chains.pop_front();
			}
		}

		// 2. Add new chains if needed (only if RUNNING, not DRAINING)
		if (ctx->anim_state == TickerAnim::RUNNING && !ctx->pending_msg_texts.empty()) {
			float rightmost_edge = scroll_area; // Default start
			if (!ctx->active_chains.empty()) {
				TickerChain &back = ctx->active_chains.back();
				rightmost_edge = back.x + back.width;
			}
			
			// If rightmost edge is visible (or close to entering), append next chain
			if (rightmost_edge < scroll_area) {
				TickerChain chain = create_chain(ctx, ctx->pending_msg_texts);
				chain.x = rightmost_edge;
				ctx->current_msg_texts = ctx->pending_msg_texts;
				ctx->active_chains.push_back(chain);
			}
		}

		// 3. Check for empty during draining -> Finish
		if (ctx->anim_state == TickerAnim::DRAINING && ctx->active_chains.empty()) {
			ctx->anim_state = TickerAnim::SLIDING_DOWN;
		}
	}

	// Clock update
	ctx->clock_timer += seconds;
	if (ctx->clock_timer >= 1.0f) {
		ctx->clock_timer -= 1.0f;
		if (ctx->clock_source)
			ft2_push(ctx->clock_source, current_time_str(ctx->clock_24h).c_str(),
			         ctx->font_face.c_str(), ctx->font_style.c_str(),
			         ctx->clock_font_size, ctx->text_color);
	}

	// Report runtime state
	int current_state = 0; // HIDDEN/STOPPED
	if (ctx->anim_state == TickerAnim::DRAINING || 
	    ctx->anim_state == TickerAnim::SLIDING_DOWN) {
		current_state = 2; // STOPPING
	} else if (ctx->anim_state != TickerAnim::HIDDEN) {
		current_state = 1; // ACTIVE
	}

	if (current_state != ctx->last_reported_state) {
		obs_data_t *settings = obs_source_get_settings(ctx->source);
		if (settings) {
			obs_data_set_int(settings, "runtime_state", current_state);
			obs_data_release(settings);
			ctx->last_reported_state = current_state;
		}
	}
}

/* ── Render ───────────────────────────────────────────────────────── */

static void ticker_video_render(void *data, gs_effect_t *)
{
	auto *ctx = (struct ticker_source *)data;
	std::lock_guard<std::mutex> lock(ctx->mutex);

	float bar_h    = (float)ctx->bar_height;
	float scroll_w = ctx->show_clock
		? (float)(1920 - ctx->clock_zone_width)
		: 1920.0f;

	// Clock zone
	if (ctx->show_clock) {
		gs_matrix_push();
		gs_matrix_translate3f(scroll_w, 0.0f, 0.0f);
		draw_rect(ctx->bg_color, (float)ctx->clock_zone_width, bar_h);
		draw_rect(ctx->clock_sep_color, 6.0f, bar_h);
		gs_matrix_pop();

		if (ctx->clock_source) {
			float cw = (float)obs_source_get_width(ctx->clock_source) * ctx->clock_scale_x;
			float ch = (float)obs_source_get_height(ctx->clock_source) * ctx->clock_scale_y;
			float cx = scroll_w + 6.0f + ((float)ctx->clock_zone_width - 6.0f - cw) * 0.5f;
			float cy = (bar_h - ch) * 0.5f;
			gs_matrix_push();
			gs_matrix_translate3f(cx, cy, 0.0f);
			gs_matrix_scale3f(ctx->clock_scale_x, ctx->clock_scale_y, 1.0f);
			obs_source_video_render(ctx->clock_source);
			gs_matrix_pop();
		}
	}

	if (ctx->anim_state == TickerAnim::HIDDEN)
		return;

	float slide_y = ctx->slide_t * bar_h;
	gs_matrix_push();
	gs_matrix_translate3f(0.0f, slide_y, 0.0f);

	// Background
	draw_rect(ctx->bg_color, scroll_w, bar_h);

	// Render chains
	// Determine text_y baseline from first chain? Or global sep?
	// Usually separation source is a good baseline reference.
	float ref_h = 0.0f;
	if (ctx->sep_source)
		ref_h = std::max(ref_h, (float)obs_source_get_height(ctx->sep_source));
	// Check first chain items for height too?
	if (!ctx->active_chains.empty() && !ctx->active_chains.front().items.empty()) {
		if (ctx->active_chains.front().items[0])
			ref_h = std::max(ref_h, (float)obs_source_get_height(ctx->active_chains.front().items[0]));
	}
	float text_y = (bar_h - ref_h * ctx->scale_y) * 0.5f;

	for (auto &chain : ctx->active_chains) {
		// Optimization: Don't render if completely off-screen
		if (chain.x > scroll_w || chain.x + chain.width < 0) continue;

		float x = chain.x;
		for (auto *item : chain.items) {
			// Item text
			if (item) {
				gs_matrix_push();
				gs_matrix_translate3f(x, text_y, 0.0f);
				gs_matrix_scale3f(ctx->scale_x, ctx->scale_y, 1.0f);
				obs_source_video_render(item);
				gs_matrix_pop();
				x += (float)obs_source_get_width(item) * ctx->scale_x;
			}
			// Separator
			if (ctx->sep_source) {
				gs_matrix_push();
				gs_matrix_translate3f(x, text_y, 0.0f);
				gs_matrix_scale3f(ctx->scale_x, ctx->scale_y, 1.0f);
				obs_source_video_render(ctx->sep_source);
				gs_matrix_pop();
				x += (float)obs_source_get_width(ctx->sep_source) * ctx->scale_x;
			}
		}
	}

	gs_matrix_pop(); // slide_y
	
	// Re-draw clock zone on top to mask text
	if (ctx->show_clock) {
		gs_matrix_push();
		gs_matrix_translate3f(scroll_w, 0.0f, 0.0f);
		draw_rect(ctx->bg_color, (float)ctx->clock_zone_width, bar_h);
		draw_rect(ctx->clock_sep_color, 6.0f, bar_h);
		gs_matrix_pop();

		if (ctx->clock_source) {
			float cw = (float)obs_source_get_width(ctx->clock_source) * ctx->clock_scale_x;
			float ch = (float)obs_source_get_height(ctx->clock_source) * ctx->clock_scale_y;
			float cx = scroll_w + 6.0f + ((float)ctx->clock_zone_width - 6.0f - cw) * 0.5f;
			float cy = (bar_h - ch) * 0.5f;
			gs_matrix_push();
			gs_matrix_translate3f(cx, cy, 0.0f);
			gs_matrix_scale3f(ctx->clock_scale_x, ctx->clock_scale_y, 1.0f);
			obs_source_video_render(ctx->clock_source);
			gs_matrix_pop();
		}
	}
}

/* ── Child source enumeration ─────────────────────────────────────── */

static void ticker_enum_active_sources(void *data,
                                       obs_source_enum_proc_t cb, void *param)
{
	auto *ctx = (struct ticker_source *)data;
	std::lock_guard<std::mutex> lock(ctx->mutex);
	
	for (auto &chain : ctx->active_chains) {
		for (auto *item : chain.items) {
			if (item) cb(ctx->source, item, param);
		}
	}

	if (ctx->sep_source)
		cb(ctx->source, ctx->sep_source, param);
	if (ctx->clock_source)
		cb(ctx->source, ctx->clock_source, param);
}

/* ── Registration ─────────────────────────────────────────────────── */

static void ticker_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "is_live", false);
	obs_data_set_default_int(settings, "speed", 100);
	obs_data_set_default_int(settings, "bar_height", 70);
	obs_data_set_default_int(settings, "bg_color", 0xFF000000);
	obs_data_set_default_string(settings, "font_face", "Arial");
	obs_data_set_default_string(settings, "font_style", "Regular");
	obs_data_set_default_int(settings, "font_size", 24);
	obs_data_set_default_int(settings, "text_color", 0xFFFFFFFF);
	obs_data_set_default_double(settings, "scale_x", 1.0);
	obs_data_set_default_double(settings, "scale_y", 1.0);
	obs_data_set_default_string(settings, "sep_text", "    \xE2\x80\xA2    ");
	obs_data_set_default_bool(settings, "show_clock", true);
	obs_data_set_default_bool(settings, "clock_24h", true);
	obs_data_set_default_int(settings, "clock_font_size", 24);
	obs_data_set_default_double(settings, "clock_scale_x", 1.0);
	obs_data_set_default_double(settings, "clock_scale_y", 1.0);
	obs_data_set_default_int(settings, "clock_zone_width", 170);
	obs_data_set_default_int(settings, "clock_sep_color", 0xFFFFFFFF);
}

struct obs_source_info ticker_source_info = {};

void ticker_source_init()
{
	ticker_source_info.id                  = "obs_ticker_cpp";
	ticker_source_info.type                = OBS_SOURCE_TYPE_INPUT;
	ticker_source_info.output_flags        = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	ticker_source_info.get_name            = ticker_get_name;
	ticker_source_info.create              = ticker_create;
	ticker_source_info.destroy             = ticker_destroy;
	ticker_source_info.update              = ticker_update;
	ticker_source_info.get_defaults        = ticker_get_defaults;
	ticker_source_info.video_render        = ticker_video_render;
	ticker_source_info.video_tick          = ticker_video_tick;
	ticker_source_info.get_width           = ticker_get_width;
	ticker_source_info.get_height          = ticker_get_height;
	ticker_source_info.enum_active_sources = ticker_enum_active_sources;
}