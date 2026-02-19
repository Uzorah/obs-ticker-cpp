obs = obslua

-- Global variables
source_name = ""
hotkey_id = obs.OBS_INVALID_HOTKEY_ID

-- Description displayed in the Scripts window
function script_description()
	return "Control the 'Text Ticker' source (obs-ticker-cpp) with a hotkey to Go Live/Stop.\n\n" ..
	       "Select the Ticker source below and set a hotkey in Settings -> Hotkeys."
end

-- Called when the script usage is updated (properties changed)
function script_update(settings)
	source_name = obs.obs_data_get_string(settings, "source")
end

-- Check properties (dropdown for sources)
function script_properties()
	local props = obs.obs_properties_create()
	local p = obs.obs_properties_add_list(props, "source", "Ticker Source", obs.OBS_COMBO_TYPE_EDITABLE, obs.OBS_COMBO_FORMAT_STRING)
	
	local sources = obs.obs_enum_sources()
	if sources ~= nil then
		for _, source in ipairs(sources) do
			local name = obs.obs_source_get_name(source)
			local id = obs.obs_source_get_unversioned_id(source)
			if id == "obs_ticker_cpp" then
				obs.obs_property_list_add_string(p, name, name)
			end
		end
		obs.source_list_release(sources)
	end
	
	return props
end

-- Hotkey callback
function toggle_ticker(pressed)
	if not pressed then return end

	local source = obs.obs_get_source_by_name(source_name)
	if source ~= nil then
		local settings = obs.obs_source_get_settings(source)
		local current_state = obs.obs_data_get_bool(settings, "is_live")
		
		-- Toggle state
		obs.obs_data_set_bool(settings, "is_live", not current_state)
		
		-- Apply update
		obs.obs_source_update(source, settings)
		
		obs.obs_data_release(settings)
		obs.obs_source_release(source)
	else
		obs.script_log(obs.LOG_WARNING, "Ticker source '" .. source_name .. "' not found!")
	end
end

-- Script load
function script_load(settings)
	hotkey_id = obs.obs_hotkey_register_frontend("ticker_hotkey_toggle", "Toggle Ticker Live", toggle_ticker)
	local hotkey_save_array = obs.obs_data_get_array(settings, "ticker_hotkey")
	obs.obs_hotkey_load(hotkey_id, hotkey_save_array)
	obs.obs_data_array_release(hotkey_save_array)
	
	source_name = obs.obs_data_get_string(settings, "source")
end

-- Script save (for hotkeys)
function script_save(settings)
	local hotkey_save_array = obs.obs_hotkey_save(hotkey_id)
	obs.obs_data_set_array(settings, "ticker_hotkey", hotkey_save_array)
	obs.obs_data_array_release(hotkey_save_array)
end
