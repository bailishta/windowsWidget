    void hide_manager() {
        if(!manager)return;
        manager.AppWindow().Hide();log(L"manager_hidden");
    }
    void apply_tray_visibility() {
        if(!coordinator()||options.self_test||options.runtime_test||!icon.cbSize)return;
        if(hide_tray_icon) {
            if(!tray_visible)return;
            bool success=Shell_NotifyIconW(NIM_DELETE,&icon)!=FALSE;tray_visible=false;
            JsonObject result;result.SetNamedValue(L"visible",JsonValue::CreateBooleanValue(false));result.SetNamedValue(L"success",JsonValue::CreateBooleanValue(success));log(L"tray_visibility_changed",result);
        } else if(!tray_visible) {
            tray_visible=Shell_NotifyIconW(NIM_ADD,&icon)!=FALSE;
            JsonObject result;result.SetNamedValue(L"visible",JsonValue::CreateBooleanValue(tray_visible));result.SetNamedValue(L"success",JsonValue::CreateBooleanValue(tray_visible));log(L"tray_visibility_changed",result);
            if(!tray_visible)log(L"tray_unavailable");
        }
    }
    void refresh_tray_controls() {
        if(!hide_tray_switch)return;
        updating_tray_controls=true;hide_tray_switch.IsOn(hide_tray_icon);hide_tray_switch.IsEnabled(!manager_read_only);updating_tray_controls=false;
    }
    void set_hide_tray_icon(bool hidden) {
        if(manager_read_only||hidden==hide_tray_icon){refresh_tray_controls();return;}
        bool previous=hide_tray_icon;hide_tray_icon=hidden;
        try {save_manager();}
        catch(...) {
            hide_tray_icon=previous;refresh_tray_controls();log(L"tray_preference_save_failed");
            if(manager_hint)manager_hint.Text(tr(L"托盘图标设置未能保存。",L"Could not save tray icon settings."));return;
        }
        apply_tray_visibility();refresh_tray_controls();
    }
    void startup_setting_error(hresult_error const& error) {
        JsonObject j;field(j,L"message",error.message());j.SetNamedValue(L"hresult",JsonValue::CreateNumberValue(error.code()));log(L"startup_setting_failed",j);
        if(!options.self_test&&!options.runtime_test){show_manager();manager_hint.Text(tr(L"启动设置未能保存：",L"Could not save startup settings: ")+std::wstring(error.message()));}
    }
    void refresh_startup_controls() {
        updating_startup_controls=true;
        if(auto_start_switch) {
            try {auto_start_switch.IsOn(startup_registration.enabled());auto_start_switch.IsEnabled(true);}
            catch(...) {auto_start_switch.IsOn(false);auto_start_switch.IsEnabled(false);}
        }
        if(silent_start_switch){silent_start_switch.IsOn(startup_preferences.silent);silent_start_switch.IsEnabled(!startup_preferences.read_only);}
        updating_startup_controls=false;
    }
    void set_auto_start(bool enabled) {
        try {
            auto command=enabled?companion::startup_command(companion::executable_path(),options.custom_data?options.root:fs::path{},options.english):L"";
            startup_registration.set(enabled,command);
            JsonObject j;j.SetNamedValue(L"enabled",JsonValue::CreateBooleanValue(enabled));log(L"autostart_changed",j);
        }catch(hresult_error const& error){startup_setting_error(error);}
        catch(std::exception const& error){startup_setting_error(hresult_error(E_FAIL,to_hstring(error.what())));}
        refresh_startup_controls();
    }
    void set_silent_start(bool enabled) {
        try {
            startup_preferences.set_silent(enabled);
            JsonObject j;j.SetNamedValue(L"enabled",JsonValue::CreateBooleanValue(enabled));log(L"silent_start_changed",j);
        }catch(hresult_error const& error){startup_setting_error(error);}
        catch(std::exception const& error){startup_setting_error(hresult_error(E_FAIL,to_hstring(error.what())));}
        refresh_startup_controls();
    }
