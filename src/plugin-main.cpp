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

// Forward declaration
static void on_frontend_finished_loading(void);

bool obs_module_load(void)
{
    // Initialize and register the ticker source
    ticker_source_init();
    obs_register_source(&ticker_source_info);
    
    // Register frontend callback to create dock after OBS UI loads
    obs_frontend_add_event_callback(
        [](enum obs_frontend_event event, void *) {
            if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
                on_frontend_finished_loading();
            }
        },
        nullptr
    );
    
    obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
    return true;
}

void obs_module_unload(void)
{
    obs_log(LOG_INFO, "plugin unloaded");
}

static void on_frontend_finished_loading(void)
{
    // Create and register the ticker dock
    QWidget *mainWindow = (QWidget *)obs_frontend_get_main_window();
    if (mainWindow) {
        TickerDock *dock = new TickerDock(mainWindow);
        
        // Add dock to OBS using the current API
        obs_frontend_add_dock_by_id("ticker_control_dock",
                                    "Ticker Control",
                                    dock);
        
        blog(LOG_INFO, "[TICKER] Dock widget created and registered");
    } else {
        blog(LOG_ERROR, "[TICKER] Failed to get OBS main window!");
    }
}