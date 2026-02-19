/*
Plugin Main Entry Point
*/

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include "ticker-source.hpp"
#include "ticker-dock.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

bool obs_module_load(void)
{
    // Initialize and register the ticker source
    ticker_source_init();
    obs_register_source(&ticker_source_info);
    
    // Register the dock immediately so OBS can restore its layout state
    // We pass nullptr as parent; OBS will manage the dock widget's hierarchy.
    TickerDock *dock = new TickerDock(nullptr);
    obs_frontend_add_dock_by_id("ticker_control_dock",
                                "Ticker Control",
                                dock);
    
    obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
    return true;
}

void obs_module_unload(void)
{
    obs_log(LOG_INFO, "plugin unloaded");
}