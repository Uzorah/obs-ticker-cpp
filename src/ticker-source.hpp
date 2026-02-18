#pragma once

#include <obs-module.h>
#include <deque>
#include <vector>
#include <string>
#include <mutex>
#include <obs-module.h>

enum class TickerAnim {
    HIDDEN,        // Bar off-screen below
    SLIDING_UP,    // Bar rising into view
    ENTERING,      // Bar at rest, text scrolling in from right (single pass)
    RUNNING,       // Bar at rest, text looping seamlessly
    DRAINING,      // is_live turned off, text finishing scroll-out
    SLIDING_DOWN,  // Bar descending out of view
};

struct TickerChain {
    std::vector<obs_source_t *> items; // Includes text sources and separators interweaved
    float width = 0.0f;
    float x = 0.0f; // Current position relative to screen 0
};

struct ticker_source {
    obs_source_t *source;
    std::mutex    mutex;

    /* Animation state machine */
    TickerAnim anim_state   = TickerAnim::HIDDEN;
    float      slide_t      = 1.0f;   // 0 = fully visible, 1 = fully hidden (below)
    bool       prev_is_live = false;

    /* Live state */
    bool  is_live = false;

    /* Scroll Deque */
    std::deque<TickerChain> active_chains;
    int   speed     = 100;
    
    // Config for NEW chains
    std::vector<std::string> pending_msg_texts;
    std::vector<std::string> current_msg_texts; // The texts used by current "template"

    /* Layout */
    int      bar_height = 70;
    uint32_t bg_color   = 0xFF000000;  // ABGR

    /* Typography */
    std::string font_face  = "Arial";
    std::string font_style = "Regular";
    int         font_size  = 24;
    uint32_t    text_color = 0xFFFFFFFF;
    float       scale_x    = 1.0f;
    float       scale_y    = 1.0f;

    /* Separator */
    std::string  sep_text    = "    \u2022    ";
    
    // We keep a "template" separator source just to measure/copy properties, 
    // but actual chains have their own separator instances (or we reuse this one if we can render multiple times? 
    // Yes, obs_source_video_render can be called multiple times per frame for same source!)
    // WAIT. If we change text of 'sep_source', it changes everywhere.
    // Content changes (A -> B) require new sources.
    // Separator usually doesn't change content often.
    // Text items DO change content (removal/addition).
    // So we need unique sources for text items. 
    // But separators are identical. We can share one 'sep_source' for all chains?
    // YES. We can reuse 'sep_source' for all separators in all chains to save memory.
    obs_source_t *sep_source = nullptr;

    /* Clock */
    obs_source_t *clock_source     = nullptr;
    bool          show_clock       = true;
    bool          clock_24h        = true;
    int           clock_font_size  = 24;
    float         clock_scale_x    = 1.0f;
    float         clock_scale_y    = 1.0f;
    float         clock_timer      = 0.0f;
    int           clock_zone_width = 170;

    /* Clock divider */
    uint32_t clock_sep_color = 0xFFFFFFFF;
    int      clock_sep_width = 6;

    /* State reporting */
    int last_reported_state = -1;
};

extern struct obs_source_info ticker_source_info;
void ticker_source_init();