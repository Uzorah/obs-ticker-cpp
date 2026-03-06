#include "ticker-source.hpp"
#include <util/platform.h>
#include <graphics/graphics.h>
#include <obs.h>
#include <obs-frontend-api.h>

#include <ctime>
#include <cstring>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <cstdio>

/* ── Forward declarations ─────────────────────────────────────────── */

static void ticker_update(void *data, obs_data_t *settings);
static uint32_t ticker_get_width(void *data);
static uint32_t ticker_get_height(void *data);
static void update_chain_width(struct ticker_source *ctx, TickerChain &chain);

/* ── Helpers ───────────────────────────────────────────────────────── */

static std::vector<std::string> split_lines(const std::string &s)
{
	std::vector<std::string> out;
	std::istringstream ss(s);
	std::string line;
	while (std::getline(ss, line))
		if (!line.empty())
			out.push_back(line);
	return out;
}

static void ft2_push(obs_source_t *src, const char *text, const char *face, const char *style, int size, uint32_t abgr)
{
	if (!src)
		return;
	obs_data_t *d = obs_data_create();
	obs_data_t *font = obs_data_create();
	obs_data_set_string(font, "face", face);
	obs_data_set_string(font, "style", style);
	obs_data_set_int(font, "size", size);
	obs_data_set_int(font, "flags", 0);
	obs_data_set_obj(d, "font", font);
	obs_data_set_string(d, "text", text);
	obs_data_set_int(d, "color1", abgr);
	obs_data_set_int(d, "color2", abgr);
	obs_source_update(src, d);
	obs_data_release(font);
	obs_data_release(d);
}

static void draw_rect(uint32_t abgr, float w, float h)
{
	if (w <= 0.0f || h <= 0.0f || std::isnan(w) || std::isnan(h))
		return;
	if (w > 16384.0f)
		w = 16384.0f; // Safety clamp to prevent D3D crashes
	if (h > 16384.0f)
		h = 16384.0f;

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *cparam = gs_effect_get_param_by_name(solid, "color");
	struct vec4 c;
	c.x = ((abgr >> 0) & 0xFF) / 255.0f;
	c.y = ((abgr >> 8) & 0xFF) / 255.0f;
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

static uint64_t chain_counter = 0;

static void append_texts_to_chain(struct ticker_source *ctx, TickerChain &chain, const std::vector<std::string> &texts)
{
	for (size_t i = 0; i < texts.size(); ++i) {
		std::string text_with_sep = texts[i] + ctx->sep_text;

		char name[128];
		snprintf(name, sizeof(name), "ticker_item_%llu_%zu", (unsigned long long)++chain_counter, i);
		obs_source_t *ms = obs_source_create_private("text_ft2_source", name, nullptr);
		if (ms) {
			ft2_push(ms, text_with_sep.c_str(), ctx->font_face.c_str(), ctx->font_style.c_str(),
				 ctx->font_size, ctx->text_color);

			float w = (float)obs_source_get_width(ms) * ctx->scale_x;
			chain.width += w;

			TickerItem item;
			item.source = ms;
			item.show_separator = false; // Baked into string for simplicity
			chain.items.push_back(item);
		}
	}
}

// Helper to create a chain from text list
static TickerChain create_chain(struct ticker_source *ctx, const std::vector<std::string> &texts)
{
	TickerChain chain;
	chain.width = 0.0f;
	append_texts_to_chain(ctx, chain, texts);
	return chain;
}

static void destroy_chain(TickerChain &chain)
{
	for (auto &item : chain.items) {
		if (item.source)
			obs_source_release(item.source);
	}
	chain.items.clear();
}

static void update_chain_style(struct ticker_source *ctx)
{
	std::lock_guard<std::mutex> lock(ctx->results_mutex);
	ctx->style_update_requested = true;
}

// Refresh chain layout (e.g. if scale changed or width was 0 initially)
static void update_chain_width(struct ticker_source *ctx, TickerChain &chain)
{
	chain.width = 0.0f;
	float sep_w = 0.0f;
	if (ctx->sep_source)
		sep_w = (float)obs_source_get_width(ctx->sep_source) * ctx->scale_x;

	for (auto &item : chain.items) {
		if (item.source)
			chain.width += (float)obs_source_get_width(item.source) * ctx->scale_x;
		if (item.show_separator)
			chain.width += sep_w;
	}
}

/* ── Source lifecycle ─────────────────────────────────────────────── */

static const char *ticker_get_name(void *)
{
	return "Text Ticker";
}

static void *ticker_create(obs_data_t *settings, obs_source_t *source)
{
	auto *ctx = new ticker_source;
	ctx->source = source;
	ctx->is_live = false;
	ctx->bar_height = 70;
	ctx->clock_timer = 0.0f;

	ctx->sep_source = obs_source_create_private("text_ft2_source", "ticker_sep", nullptr);
	ctx->clock_source = obs_source_create_private("text_ft2_source", "ticker_clock", nullptr);
	ctx->height_ref_source = obs_source_create_private("text_ft2_source", "ticker_h_ref", nullptr);

	ticker_update(ctx, settings);

	// FORCE OFF on startup:
	// Even if saved setting was true, we start stopped.
	ctx->is_live = false;
	ctx->prev_is_live = false;
	obs_data_set_bool(settings, "is_live", false);

	return ctx;
}

static void ticker_destroy(void *data)
{
	auto *ctx = (struct ticker_source *)data;

	for (auto &chain : ctx->active_chains) {
		destroy_chain(chain);
	}
	ctx->active_chains.clear();

	// Clear any orphaned data in queues
	{
		std::lock_guard<std::mutex> lock(ctx->results_mutex);
		ctx->pending_chain_requests.clear();
	}

	for (auto &chain : ctx->dead_chains) {
		destroy_chain(chain);
	}
	ctx->dead_chains.clear();

	if (ctx->sep_source)
		obs_source_release(ctx->sep_source);
	if (ctx->clock_source)
		obs_source_release(ctx->clock_source);
	if (ctx->height_ref_source)
		obs_source_release(ctx->height_ref_source);
	delete ctx;
}

static uint32_t ticker_get_width(void *)
{
	return 1920;
}

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
	ctx->speed = (int)obs_data_get_int(settings, "speed");
	if (ctx->speed <= 0)
		ctx->speed = 100;

	int new_height = (int)obs_data_get_int(settings, "bar_height");
	if (new_height <= 0)
		new_height = 70;

	if (new_height != ctx->bar_height) {
		// Handle scene item offset logic if needed (omitted for brevity as it was standard)
		ctx->bar_height = new_height;
	}

	ctx->vertical_offset = (int)obs_data_get_int(settings, "vertical_offset");
	ctx->bg_color = (uint32_t)obs_data_get_int(settings, "bg_color");

	std::string new_face = obs_data_get_string(settings, "font_face");
	std::string new_style = obs_data_get_string(settings, "font_style");
	int new_fsize = (int)obs_data_get_int(settings, "font_size");
	if (new_fsize > 250)
		new_fsize = 250; // Clamp tighter to avoid texture limits/crashes

	uint32_t new_tcol = (uint32_t)obs_data_get_int(settings, "text_color");
	float new_sx = (float)obs_data_get_double(settings, "scale_x");
	float new_sy = (float)obs_data_get_double(settings, "scale_y");
	if (new_sx > 10.0f)
		new_sx = 10.0f;
	if (new_sy > 10.0f)
		new_sy = 10.0f;
	std::string new_sep = obs_data_get_string(settings, "sep_text");

	bool font_dirty = (new_face != ctx->font_face) || (new_style != ctx->font_style) ||
			  (new_fsize != ctx->font_size) || (new_tcol != ctx->text_color);

	ctx->font_face = new_face;
	ctx->font_style = new_style;
	ctx->font_size = new_fsize;
	ctx->text_color = new_tcol;
	ctx->scale_x = new_sx;
	ctx->scale_y = new_sy;

	bool sep_dirty = (new_sep != ctx->sep_text) || font_dirty;
	ctx->sep_text = new_sep;

	ctx->show_clock = obs_data_get_bool(settings, "show_clock");
	bool new_24h = obs_data_get_bool(settings, "clock_24h");
	int new_cfsize = (int)obs_data_get_int(settings, "clock_font_size");
	if (new_cfsize > 250)
		new_cfsize = 250; // Clamp

	bool clock_dirty = (new_24h != ctx->clock_24h) || (new_cfsize != ctx->clock_font_size) || font_dirty;
	ctx->clock_24h = new_24h;
	ctx->clock_font_size = new_cfsize;
	ctx->clock_scale_x = (float)obs_data_get_double(settings, "clock_scale_x");
	ctx->clock_scale_y = (float)obs_data_get_double(settings, "clock_scale_y");
	ctx->clock_zone_width = (int)obs_data_get_int(settings, "clock_zone_width");
	ctx->clock_sep_color = (uint32_t)obs_data_get_int(settings, "clock_sep_color");

	const char *raw = obs_data_get_string(settings, "messages");
	if (raw) {
		ctx->pending_msg_texts = split_lines(raw);
	}

	// Update existing chains style instead of destroying them
	if (font_dirty) {
		update_chain_style(ctx);
	}

	// Signal style update on graphics thread
	if (font_dirty || sep_dirty || clock_dirty) {
		std::lock_guard<std::mutex> lock_r(ctx->results_mutex);
		ctx->style_update_requested = true;
	}
}

/* ── Tick ─────────────────────────────────────────────────────────── */

static constexpr float SLIDE_DURATION = 0.35f;

static void ticker_video_tick(void *data, float seconds)
{
	auto *ctx = (struct ticker_source *)data;
	std::lock_guard<std::mutex> lock(ctx->mutex);

	// 0. Garbage Collection: Safely destroy sources released in previous ticks.
	// Since we are on the graphics thread and hold the mutex, this is safe.
	while (!ctx->dead_chains.empty()) {
		TickerChain c = std::move(ctx->dead_chains.front());
		ctx->dead_chains.pop_front();
		destroy_chain(c);
	}

	// 1. Process deferred data requests from worker
	{
		std::lock_guard<std::mutex> lock_r(ctx->results_mutex);

		// Clock
		if (ctx->clock_update_requested && ctx->clock_source) {
			ft2_push(ctx->clock_source, ctx->pending_clock_text.c_str(), ctx->font_face.c_str(),
				 ctx->font_style.c_str(), ctx->clock_font_size, ctx->text_color);
			ctx->clock_update_requested = false;
			ctx->clock_frames_alive = 0; // Reset for texture safety
		}

		// Style
		if (ctx->style_update_requested) {
			if (ctx->sep_source) {
				ft2_push(ctx->sep_source, ctx->sep_text.c_str(), ctx->font_face.c_str(),
					 ctx->font_style.c_str(), ctx->font_size, ctx->text_color);
			}
			if (ctx->height_ref_source) {
				ft2_push(ctx->height_ref_source, "|Tgj_0123", ctx->font_face.c_str(),
					 ctx->font_style.c_str(), ctx->font_size, ctx->text_color);
			}
			if (ctx->clock_source) {
				ft2_push(ctx->clock_source, current_time_str(ctx->clock_24h).c_str(),
					 ctx->font_face.c_str(), ctx->font_style.c_str(), ctx->clock_font_size,
					 ctx->text_color);
				ctx->clock_frames_alive = 0; // Reset
			}

			// Update existing chain items (PRESERVE TEXT!)
			for (auto &c : ctx->active_chains) {
				for (auto &item : c.items) {
					if (item.source) {
						obs_data_t *settings = obs_source_get_settings(item.source);
						const char *txt = obs_data_get_string(settings, "text");
						if (txt && txt[0] != '\0') {
							ft2_push(item.source, txt, ctx->font_face.c_str(),
								 ctx->font_style.c_str(), ctx->font_size,
								 ctx->text_color);
						}
						obs_data_release(settings);
					}
				}
				update_chain_width(ctx, c);
				c.frames_alive = 0; // Reset for texture safety
			}
			ctx->style_update_requested = false;
		}
	}

	// Get actual canvas width
	struct obs_video_info ovi;
	float base_width = 1920.0f;
	if (obs_get_video_info(&ovi)) {
		base_width = (float)ovi.base_width;
	}

	float scroll_area = ctx->show_clock ? (base_width - (float)ctx->clock_zone_width) : base_width;

	// ── Edge-detect is_live changes ──
	if (ctx->is_live && !ctx->prev_is_live) {
		if (ctx->anim_state == TickerAnim::HIDDEN || ctx->anim_state == TickerAnim::SLIDING_DOWN)
			ctx->anim_state = TickerAnim::SLIDING_UP;
		else if (ctx->anim_state == TickerAnim::DRAINING)
			ctx->anim_state = TickerAnim::RUNNING;

	} else if (!ctx->is_live && ctx->prev_is_live) {
		if (ctx->anim_state == TickerAnim::RUNNING || ctx->anim_state == TickerAnim::ENTERING) {
			ctx->anim_state = TickerAnim::DRAINING;

			// Clear ready chains to stop new content from appearing
			{
				std::lock_guard<std::mutex> lock_r(ctx->results_mutex);
				ctx->pending_chain_requests.clear();
			}

			// Purge completely off-screen tails to ensure "Once and Drain"
			auto it = ctx->active_chains.begin();
			while (it != ctx->active_chains.end()) {
				if (it->x >= scroll_area) {
					ctx->dead_chains.push_back(std::move(*it));
					it = ctx->active_chains.erase(it);
				} else {
					++it;
				}
			}

		} else if (ctx->anim_state == TickerAnim::SLIDING_UP) {
			ctx->anim_state = TickerAnim::SLIDING_DOWN;
		}
	}
	ctx->prev_is_live = ctx->is_live;

	// ... (Seamless updates handled in loop spawning and appending)

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
					ctx->current_msg_texts = ctx->pending_msg_texts; // Update snapshot
					std::lock_guard<std::mutex> lock_r(ctx->results_mutex);
					for (const auto &txt : ctx->pending_msg_texts) {
						ctx->pending_chain_requests.push_back({txt});
					}
				}
			}
		}
		break;
	case TickerAnim::SLIDING_DOWN:
		ctx->slide_t += slide_speed * seconds;
		if (ctx->slide_t >= 1.0f) {
			ctx->slide_t = 1.0f;
			ctx->anim_state = TickerAnim::HIDDEN;
			for (auto &c : ctx->active_chains) {
				ctx->dead_chains.push_back(std::move(c));
			}
			ctx->active_chains.clear();
		}
		break;
	case TickerAnim::RUNNING:
	case TickerAnim::ENTERING: // Treated same as running now
	case TickerAnim::DRAINING:

		// ── Seamless Content Update ──
		if (ctx->pending_msg_texts != ctx->current_msg_texts) {
			// 1. Unified Purge: To achieve "Once and Drain", preserve all visible content
			// and destroy the entire pre-spawned tail that hasn't entered the screen yet.
			auto it = ctx->active_chains.begin();
			while (it != ctx->active_chains.end()) {
				if (it->x >= scroll_area) {
					ctx->dead_chains.push_back(std::move(*it));
					it = ctx->active_chains.erase(it);
				} else {
					++it;
				}
			}

			bool is_pure_addition = (ctx->pending_msg_texts.size() > ctx->current_msg_texts.size());
			if (is_pure_addition) {
				for (size_t i = 0; i < ctx->current_msg_texts.size(); ++i) {
					if (ctx->pending_msg_texts[i] != ctx->current_msg_texts[i]) {
						is_pure_addition = false;
						break;
					}
				}
			}

			// Append logic: Only run if currently LIVE.
			if (is_pure_addition && ctx->is_live) {
				if (!ctx->active_chains.empty()) {
					TickerChain &back = ctx->active_chains.back();
					if (back.x + back.width > 0.0f) {
						std::vector<std::string> new_items;
						for (size_t i = ctx->current_msg_texts.size();
						     i < ctx->pending_msg_texts.size(); ++i) {
							new_items.push_back(ctx->pending_msg_texts[i]);
						}
						{
							std::lock_guard<std::mutex> lock_r(ctx->results_mutex);
							for (const auto &txt : new_items) {
								ctx->pending_chain_requests.push_back({txt});
							}
						}
						ctx->current_msg_texts = ctx->pending_msg_texts;
					}
				} else {
					ctx->current_msg_texts = ctx->pending_msg_texts;
				}
			} else {
				// Removal/Change (or addition while not live):
				// Just update snapshot; the loop will spawn the new queue naturally after current clear.
				ctx->current_msg_texts = ctx->pending_msg_texts;
			}
		}

		// Pick up ready chains from worker
		{
			std::lock_guard<std::mutex> lock_r(ctx->results_mutex);
			while (!ctx->pending_chain_requests.empty()) {
				std::vector<std::string> texts = std::move(ctx->pending_chain_requests.front());
				ctx->pending_chain_requests.pop_front();

				TickerChain chain = create_chain(ctx, texts);

				// Positioning
				if (ctx->active_chains.empty()) {
					chain.x = scroll_area;
				} else {
					TickerChain &back = ctx->active_chains.back();
					chain.x = back.x + back.width;
				}
				ctx->active_chains.push_back(std::move(chain));
			}
		}

		// Move all chains
		for (auto &c : ctx->active_chains) {
			c.x -= (float)ctx->speed * seconds;
			// Always update width to catch delayed font loading
			update_chain_width(ctx, c);
		}
		break;
	}

	// ── Chain Management ──
	if (ctx->anim_state == TickerAnim::RUNNING || ctx->anim_state == TickerAnim::DRAINING) {

		// 0. Collision Resolution (Push chains apart if they overlap due to growth)
		if (ctx->active_chains.size() > 1) {
			for (size_t i = 1; i < ctx->active_chains.size(); ++i) {
				TickerChain &prev = ctx->active_chains[i - 1];
				TickerChain &curr = ctx->active_chains[i];

				// Only enforce collision if the previous chain has a valid width.
				// If width is ~0, it's likely still loading (texture not ready),
				// so snapping to it would cause overlap.
				if (prev.width > 1.0f) {
					float min_x = prev.x + prev.width;
					if (curr.x < min_x) {
						curr.x = min_x;
					}
				} else {
					// Previous chain is loading (width ~0). Keep curr behind it to avoid overlap jump.
					// Use a larger safety gap to prevent "stacking" if multiple chains load at once.
					float safety_x = prev.x + 200.0f;
					if (curr.x < safety_x)
						curr.x = safety_x;
				}
			}
		}

		// 1. Remove off-screen chains
		if (!ctx->active_chains.empty()) {
			TickerChain &front = ctx->active_chains.front();
			if (front.x + front.width < 0.0f) {
				ctx->dead_chains.push_back(std::move(front));
				ctx->active_chains.pop_front();
			}
		}

		// 2. Add new chains if needed (only if RUNNING, not DRAINING)
		if (ctx->anim_state == TickerAnim::RUNNING && !ctx->pending_msg_texts.empty()) {
			float rightmost_edge = scroll_area; // Default start
			bool should_add = false;

			if (ctx->active_chains.empty()) {
				// If we have no chains but have pending text, start one immediately.
				should_add = true;
			} else {
				TickerChain &back = ctx->active_chains.back();

				// Ensure ALL items in the last chain are measured (width > 0)
				// This prevents spawning a new chain on top of an unmeasured one
				bool all_items_measured = true;
				for (auto &item : back.items) {
					if (item.source && obs_source_get_width(item.source) <= 1.0f) {
						all_items_measured = false;
						break;
					}
				}
				if (all_items_measured && ctx->sep_source &&
				    obs_source_get_width(ctx->sep_source) <= 1.0f) {
					all_items_measured = false;
				}

				// Extra safety: If chain width is suspiciously small (less than font size), wait.
				// This catches cases where sources exist but haven't loaded content yet.
				if (back.width < (float)ctx->font_size * 0.5f) {
					all_items_measured = false;
				}

				if (all_items_measured) {
					rightmost_edge = back.x + back.width;
					if (rightmost_edge < scroll_area) {
						should_add = true;
					}
				}
			}

			// If rightmost edge is visible (or close to entering), append next chain
			if (should_add) {
				ctx->current_msg_texts = ctx->pending_msg_texts;
				std::lock_guard<std::mutex> lock_r(ctx->results_mutex);
				for (const auto &txt : ctx->pending_msg_texts) {
					ctx->pending_chain_requests.push_back({txt});
				}
			}
		}

		// 3. Check for empty during draining -> Finish
		if (ctx->anim_state == TickerAnim::DRAINING && ctx->active_chains.empty()) {
			ctx->anim_state = TickerAnim::SLIDING_DOWN;
		}
	}

	// Clock update: offload to worker
	ctx->clock_timer += seconds;
	if (ctx->clock_timer >= 1.0f) {
		ctx->clock_timer -= 1.0f;
		if (ctx->clock_source) {
			std::string time_str = current_time_str(ctx->clock_24h);
			std::lock_guard<std::mutex> lock_r(ctx->results_mutex);
			ctx->pending_clock_text = time_str;
			ctx->clock_update_requested = true;
		}
	}

	// Report runtime state
	int current_state = 0; // HIDDEN/STOPPED
	if (ctx->anim_state == TickerAnim::DRAINING || ctx->anim_state == TickerAnim::SLIDING_DOWN) {
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

	// Increment frame counters (giving GPU time to upload textures)
	for (auto &c : ctx->active_chains) {
		c.frames_alive++;
	}
	ctx->clock_frames_alive++;
}

static void render_clock(struct ticker_source *ctx, float scroll_w, float bar_h)
{
	if (!ctx->clock_source)
		return;

	// Texture safety: Wait at least 1 frame after update
	if (ctx->clock_frames_alive < 1)
		return;

	uint32_t raw_cw = obs_source_get_width(ctx->clock_source);
	uint32_t raw_ch = obs_source_get_height(ctx->clock_source);

	// Only render if texture is ready (> 1px)
	if (raw_cw <= 1 || raw_ch <= 1)
		return;

	float cw = (float)raw_cw * ctx->clock_scale_x;
	float ch = (float)raw_ch * ctx->clock_scale_y;
	float cx = scroll_w + 6.0f + ((float)ctx->clock_zone_width - 6.0f - cw) * 0.5f;
	float visual_adj = ch * 0.12f;
	float cy = (bar_h - ch) * 0.5f + (float)ctx->vertical_offset - visual_adj;

	gs_matrix_push();
	gs_matrix_translate3f(cx, cy, 0.0f);
	gs_matrix_scale3f(ctx->clock_scale_x, ctx->clock_scale_y, 1.0f);
	obs_source_video_render(ctx->clock_source);
	gs_matrix_pop();
}

/* ── Render ───────────────────────────────────────────────────────── */

static void ticker_video_render(void *data, gs_effect_t *)
{
	auto *ctx = (struct ticker_source *)data;
	std::lock_guard<std::mutex> lock(ctx->mutex);

	struct obs_video_info ovi;
	float base_width = 1920.0f;
	if (obs_get_video_info(&ovi)) {
		base_width = (float)ovi.base_width;
	}

	float bar_h = (float)ctx->bar_height;
	float scroll_w = ctx->show_clock ? (base_width - (float)ctx->clock_zone_width) : base_width;

	if (ctx->anim_state == TickerAnim::HIDDEN && ctx->slide_t <= 0.0f)
		return;

	// Clock zone
	if (ctx->show_clock) {
		gs_matrix_push();
		gs_matrix_translate3f(scroll_w, 0.0f, 0.0f);
		draw_rect(ctx->bg_color, (float)ctx->clock_zone_width, bar_h);
		draw_rect(ctx->clock_sep_color, 6.0f, bar_h);
		gs_matrix_pop();

		render_clock(ctx, scroll_w, bar_h);
	}

	if (ctx->anim_state == TickerAnim::HIDDEN)
		return;

	float slide_y = ctx->slide_t * bar_h;
	gs_matrix_push();
	gs_matrix_translate3f(0.0f, slide_y, 0.0f);

	// Background
	draw_rect(ctx->bg_color, scroll_w, bar_h);

	// Render chains
	for (auto &chain : ctx->active_chains) {
		// Texture safety: Wait 1 frame for GPU upload
		if (chain.frames_alive < 1)
			continue;

		if (chain.x > scroll_w || chain.x + chain.width < 0)
			continue;

		float x = chain.x;
		for (auto &item : chain.items) {
			if (item.source) {
				float w = (float)obs_source_get_width(item.source);
				float h = (float)obs_source_get_height(item.source);

				// Stable height reference calculation
				float h_ref = (float)ctx->font_size * 1.3f; // Default based on font size
				if (ctx->height_ref_source) {
					float hr = (float)obs_source_get_height(ctx->height_ref_source);
					if (hr > 1.0f)
						h_ref = hr;
				}

				if (w > 1.0f && h > 1.0f) {
					// Use h_ref for deterministic vertical positioning across chunks and chains.
					// This aligns them to the same "bounding box center", which for a fixed font
					// and reference string |Tgj_0123 effectively aligns baselines.
					float visual_adj = h_ref * ctx->scale_y * 0.12f;
					float text_y = (bar_h - h_ref * ctx->scale_y) * 0.5f +
						       (float)ctx->vertical_offset - visual_adj;

					gs_matrix_push();
					gs_matrix_translate3f(x, text_y, 0.0f);
					gs_matrix_scale3f(ctx->scale_x, ctx->scale_y, 1.0f);
					obs_source_video_render(item.source);
					gs_matrix_pop();
				}
				x += w * ctx->scale_x;
			}
		}
	}

	gs_matrix_pop(); // slide_y

	// Re-draw clock zone on top to mask text
	if (ctx->show_clock) {
		struct obs_video_info ovi;
		float base_width = 1920.0f;
		if (obs_get_video_info(&ovi)) {
			base_width = (float)ovi.base_width;
		}
		float scroll_w = base_width - (float)ctx->clock_zone_width;

		gs_matrix_push();
		gs_matrix_translate3f(scroll_w, 0.0f, 0.0f);
		draw_rect(ctx->bg_color, (float)ctx->clock_zone_width, bar_h);
		draw_rect(ctx->clock_sep_color, 6.0f, bar_h);
		gs_matrix_pop();

		render_clock(ctx, scroll_w, bar_h);
	}
}

/* ── Child source enumeration ─────────────────────────────────────── */

static void ticker_enum_active_sources(void *data, obs_source_enum_proc_t cb, void *param)
{
	auto *ctx = (struct ticker_source *)data;
	std::lock_guard<std::mutex> lock(ctx->mutex);

	for (auto &chain : ctx->active_chains) {
		for (auto &item : chain.items) {
			if (item.source)
				cb(ctx->source, item.source, param);
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
	obs_data_set_default_int(settings, "vertical_offset", 0);
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
	ticker_source_info.id = "obs_ticker_cpp";
	ticker_source_info.type = OBS_SOURCE_TYPE_INPUT;
	ticker_source_info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	ticker_source_info.get_name = ticker_get_name;
	ticker_source_info.create = ticker_create;
	ticker_source_info.destroy = ticker_destroy;
	ticker_source_info.update = ticker_update;
	ticker_source_info.get_defaults = ticker_get_defaults;
	ticker_source_info.video_render = ticker_video_render;
	ticker_source_info.video_tick = ticker_video_tick;
	ticker_source_info.get_width = ticker_get_width;
	ticker_source_info.get_height = ticker_get_height;
	ticker_source_info.enum_active_sources = ticker_enum_active_sources;
	obs_register_source(&ticker_source_info);
}