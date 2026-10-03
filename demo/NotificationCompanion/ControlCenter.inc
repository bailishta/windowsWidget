    bool coordinator() const {return !options.self_test && options.channel.empty();}
    void theme_titlebar(Window const& target) {
        if(!target)return;
        auto background=SolidColorBrush(dark?Windows::UI::Color{255,32,32,32}:Windows::UI::Color{255,243,243,243});
        if(auto page=target.Content().try_as<FrameworkElement>()) {
            page.RequestedTheme(dark?ElementTheme::Dark:ElementTheme::Light);
            if(auto panel=page.try_as<Panel>())panel.Background(background);
            if(auto scroll=page.try_as<ScrollViewer>()) {
                scroll.Background(background);
                if(auto content=scroll.Content().try_as<FrameworkElement>()) {
                    content.RequestedTheme(dark?ElementTheme::Dark:ElementTheme::Light);
                    if(auto panel=content.try_as<Panel>())panel.Background(background);
                }
            }
        }
        auto bar=target.AppWindow().TitleBar();
        if(Microsoft::UI::Windowing::AppWindowTitleBar::IsCustomizationSupported()) {
            auto bg=dark?Windows::UI::Color{255,32,32,32}:Windows::UI::Color{255,243,243,243};
            auto fg=dark?Windows::UI::Color{255,245,245,245}:Windows::UI::Color{255,25,25,25};
            auto dim=dark?Windows::UI::Color{255,160,160,160}:Windows::UI::Color{255,100,100,100};
            auto hover=dark?Windows::UI::Color{255,52,52,52}:Windows::UI::Color{255,225,225,225};
            bar.BackgroundColor(bg);bar.ForegroundColor(fg);bar.InactiveBackgroundColor(bg);bar.InactiveForegroundColor(dim);
            bar.ButtonBackgroundColor(bg);bar.ButtonForegroundColor(fg);bar.ButtonInactiveBackgroundColor(bg);bar.ButtonInactiveForegroundColor(dim);
            bar.ButtonHoverBackgroundColor(hover);bar.ButtonHoverForegroundColor(fg);bar.ButtonPressedBackgroundColor(bg);bar.ButtonPressedForegroundColor(dim);
        }
        HWND handle=nullptr;target.as<IWindowNative>()->get_WindowHandle(&handle);BOOL value=dark;
        DwmSetWindowAttribute(handle,DWMWA_USE_IMMERSIVE_DARK_MODE,&value,sizeof(value));
    }
    HWND surface_handle(size_t i) const {
        if(coordinator())return i<plugins.size()?plugins[i]->panel():nullptr;
        return i==0?hwnd:(i-1<widgets.size()?widgets[i-1].hwnd:nullptr);
    }
    fs::path plugin_data(companion::PluginDefinition const& p) const {return options.root/L"plugin-data"/p.id;}
    void save_manager() {
        if(manager_read_only)return;
        JsonObject j;j.SetNamedValue(L"schema",JsonValue::CreateNumberValue(1));j.SetNamedValue(L"library_version",JsonValue::CreateNumberValue(1));Windows::Data::Json::JsonArray entries;
        for(size_t i=0;i<plugins.size();++i) {
            JsonObject e;field(e,L"id",plugins[i]->definition.id);e.SetNamedValue(L"enabled",JsonValue::CreateBooleanValue(widget_enabled[i]));
            e.SetNamedValue(L"x",JsonValue::CreateNumberValue(free_positions[i].x));e.SetNamedValue(L"y",JsonValue::CreateNumberValue(free_positions[i].y));
            e.SetNamedValue(L"saved",JsonValue::CreateBooleanValue(free_positions[i].saved));entries.Append(e);
        }
        j.SetNamedValue(L"plugins",entries);j.SetNamedValue(L"filter",JsonValue::CreateNumberValue(filter));
        j.SetNamedValue(L"auto_arrange",JsonValue::CreateBooleanValue(auto_arrange));j.SetNamedValue(L"theme_override",JsonValue::CreateBooleanValue(theme_override));
        j.SetNamedValue(L"hide_tray_icon",JsonValue::CreateBooleanValue(hide_tray_icon));
        j.SetNamedValue(L"dark",JsonValue::CreateBooleanValue(dark));companion::write_json_atomic(options.root/L"manager.json",j);
    }
    void start_plugin(size_t i) {
        auto& p=*plugins[i];
        if(!p.definition.error.empty()){p.state=p.definition.error;return;}
        try {p.start(plugin_data(p.definition),control,options.english,options.inspect);}
        catch(hresult_error const& e){p.stop();p.state=L"启动失败："+std::wstring(e.message());}
        catch(std::exception const& e){p.stop();p.state=std::wstring(to_hstring(e.what()));}
    }
    std::vector<companion::PluginDefinition> available_components() {
        std::vector<companion::PluginDefinition> result;
        auto base=companion::executable_path().parent_path();
        for(auto const* folder:{L"components",L"examples"}) {
            if(!fs::exists(base/folder))continue;
            for(auto const& entry:fs::directory_iterator(base/folder))if(entry.is_directory()&&fs::exists(entry.path()/L"widget.json")) {
                try {auto item=companion::read_plugin(entry.path()/L"widget.json");
                    if(retired_example(item))continue;
                    if(std::none_of(result.begin(),result.end(),[&](auto const& other){return other.id==item.id;}))result.push_back(std::move(item));
                }catch(...){catalog_errors+=entry.path().filename().wstring()+L"：组件库文件无效\n";}
            }
        }
        return result;
    }
    static bool retired_example(companion::PluginDefinition const& item) {
        return item.id==L"user.my-checklist"||item.id==L"user.my-links";
    }
    void initialize_plugins() {
        auto guides=companion::executable_path().parent_path()/L"plugins";
        fs::create_directories(options.root/L"plugins");
        for(auto const* name:{L"README.md",L"WidgetSdk.h",L"PluginProcess.h",L"PluginCatalog.h",L"widget.schema.json"}) {
            std::error_code ignored;fs::copy_file(guides/name,options.root/L"plugins"/name,fs::copy_options::skip_existing,ignored);
        }
        auto old_enabled=widget_enabled;
        JsonObject state{nullptr};bool legacy=false;
        if(fs::exists(options.root/L"manager.json")) {
            try {
                state=JsonObject::Parse(to_hstring(companion::read_plugin_file(options.root/L"manager.json")));
                if(state.GetNamedNumber(L"schema",1)!=1)throw std::runtime_error("Unsupported manager schema");
                // Validate the entire saved list before copying packages or changing manager state.
                for(auto const& value:state.GetNamedArray(L"plugins",Windows::Data::Json::JsonArray())) {
                    auto e=value.GetObject();e.GetNamedString(L"id");e.GetNamedBoolean(L"enabled",true);
                    e.GetNamedNumber(L"x",0);e.GetNamedNumber(L"y",0);e.GetNamedBoolean(L"saved",false);
                }
                state.GetNamedBoolean(L"auto_arrange",true);state.GetNamedNumber(L"filter",0);
                state.GetNamedBoolean(L"theme_override",false);state.GetNamedBoolean(L"dark",true);state.GetNamedBoolean(L"hide_tray_icon",false);
                legacy=state.GetNamedNumber(L"library_version",0)<1;
            }catch(...){state=nullptr;manager_read_only=true;catalog_errors+=L"管理设置损坏或版本不兼容：原文件已保留。\n";}
        }else legacy=fs::exists(options.root/L"widgets.json");
        if(legacy&&!manager_read_only) {
            auto entries=state?state.GetNamedArray(L"plugins",Windows::Data::Json::JsonArray()):Windows::Data::Json::JsonArray();
            auto packages=available_components();
            for(auto const& definition:packages)if(definition.document.GetNamedBoolean(L"legacy_component",false)) {
                bool listed=!state;
                for(auto const& value:entries)listed|=value.GetObject().GetNamedString(L"id")==definition.id;
                if(!listed)continue;
                auto manifest=options.root/L"plugins"/definition.id/L"widget.json";
                try {
                    if(!fs::exists(manifest))companion::install_component(definition.manifest,options.root);
                    auto data=plugin_data(definition);fs::create_directories(data);
                    if(!fs::exists(data/L"widgets.json")&&fs::exists(options.root/L"widgets.json"))
                        fs::copy_file(options.root/L"widgets.json",data/L"widgets.json");
                }catch(...){catalog_errors+=definition.name+L"：旧组件迁移失败，可在组件库重新安装。\n";}
            }
        }
        for(auto const& dir:fs::directory_iterator(options.root/L"plugins"))if(dir.is_directory()&&fs::exists(dir.path()/L"widget.json")) {
            try {
                auto definition=companion::read_plugin(dir.path()/L"widget.json");
                if(retired_example(definition)&&!manager_read_only) {
                    companion::archive_component(definition,options.root);
                    log(L"retired_example_archived");continue;
                }
                if(std::any_of(plugins.begin(),plugins.end(),[&](auto const& p){return p->definition.id==definition.id;}))throw std::runtime_error("重复的组件 id");
                auto p=std::make_unique<companion::PluginProcess>();p->definition=std::move(definition);p->enabled=false;plugins.push_back(std::move(p));
            }catch(...){catalog_errors+=dir.path().filename().wstring()+L"：描述文件无效\n";}
        }
        widget_heights.clear();widget_enabled.assign(plugins.size(),false);free_positions.assign(plugins.size(),{});hide_tray_icon=false;
        if(state) {
            auto entries=state.GetNamedArray(L"plugins",Windows::Data::Json::JsonArray());size_t next=0;
            for(auto const& value:entries) {
                auto e=value.GetObject();auto id=e.GetNamedString(L"id");
                auto found=std::find_if(plugins.begin()+next,plugins.end(),[&](auto const& p){return p->definition.id==id;});
                if(found==plugins.end())continue;
                std::iter_swap(plugins.begin()+next,found);plugins[next]->enabled=e.GetNamedBoolean(L"enabled",true);
                auto x=e.GetNamedNumber(L"x",0),y=e.GetNamedNumber(L"y",0);
                if(std::isfinite(x)&&std::isfinite(y))free_positions[next]={std::clamp(x,0.0,1.0),std::clamp(y,0.0,1.0),e.GetNamedBoolean(L"saved",false)};
                ++next;
            }
            auto_arrange=state.GetNamedBoolean(L"auto_arrange",true);filter=unsigned(std::clamp(state.GetNamedNumber(L"filter",0),0.0,2.0));
            theme_override=state.GetNamedBoolean(L"theme_override",false);if(theme_override)dark=state.GetNamedBoolean(L"dark",dark);
            hide_tray_icon=state.GetNamedBoolean(L"hide_tray_icon",false);
        }
        for(size_t i=0;i<plugins.size();++i) {
            auto& p=*plugins[i];widget_heights.push_back(p.definition.height);
            if(legacy&&!state) {
                auto index=p.definition.document.GetNamedNumber(L"legacy_index",-1);
                if(index>=0&&index<old_enabled.size())p.enabled=old_enabled[size_t(index)];
            }
            widget_enabled[i]=p.enabled;fs::create_directories(plugin_data(p.definition));
        }
        widgets.resize(plugins.empty()?0:plugins.size()-1);widget_positions.resize(plugins.size());
        for(size_t i=0;i<plugins.size();++i)if(widget_enabled[i])start_plugin(i);
        save_manager();
    }
    void service_plugins() {
        if(!coordinator())return;
        bool changed=false,any_dragging=false;
        for(size_t i=0;i<plugins.size();++i) {
            auto& p=*plugins[i];changed|=p.update();auto handle=p.panel();
            if(i&&widgets[i-1].hwnd!=handle){widgets[i-1].hwnd=handle;widgets[i-1].animated={};widgets[i-1].clipped=false;changed=true;}
            if(p.channel) {
                auto* wire=p.channel->wire;
                if(InterlockedCompareExchange(&wire->dragging,0,0)){any_dragging=true;layout_moving=false;}
                auto revision=InterlockedCompareExchange(&wire->drag_revision,0,0);
                if(revision!=p.drag_revision) {
                    p.drag_revision=revision;dragging=false;MemoryBarrier();RECT actual=wire->drag_position;
                    if(!IsRectEmpty(&actual)){remember_position(i,actual);widget_positions[i]=actual;if(i==0){last_position=actual;animated_position=actual;}else {widgets[i-1].target=actual;widgets[i-1].animated=actual;}save_manager();log(L"widget_drag_saved");}
                }
                companion::PluginFrame frame{layout_work,layout_reserved,i<widget_positions.size()?widget_positions[i]:RECT{},layout_dpi,
                    auto_arrange?1L:0L,dark?1L:0L,LONG(motion.phase)};
                p.channel->publish(frame);
            }
        }
        dragging=any_dragging;
        if(changed){animated_position={};update_plugin_status();if(shown)motion_frame();}
    }
    bool plugin_inline_editing() const {
        return std::any_of(plugins.begin(),plugins.end(),[](auto const& item){
            return item->panel()&&item->channel&&InterlockedCompareExchange(&item->channel->wire->editing,0,0)!=0;
        });
    }
    void update_plugin_status() {
        for(size_t i=0;i<std::min(plugin_status.size(),plugins.size());++i) {
            auto& p=*plugins[i];plugin_status[i].Text(p.state+(p.pid?L" · 独立运行":L""));
        }
    }
    void set_plugin_enabled(size_t i,bool enabled) {
        if(enabled&&!plugins[i]->definition.error.empty()){manager_hint.Text(plugins[i]->definition.error);return;}
        widget_enabled[i]=enabled;plugins[i]->enabled=enabled;
        if(enabled)start_plugin(i);else {plugins[i]->stop();dragging=false;}
        save_manager();clear_dismissal();poll();update_plugin_status();
    }
    void plugin_settings(size_t i) {
        if(!plugins[i]->definition.document.GetNamedBoolean(L"has_settings",true))return;
        if(!widget_enabled[i]){set_plugin_enabled(i,true);}
        // Explicit management actions allow the child card to focus its in-place input.
        if(plugins[i]->pid)AllowSetForegroundWindow(plugins[i]->pid);
        plugins[i]->command(1);
    }
    void move_plugin(size_t i,int delta) {
        int next=int(i)+delta;if(next<0||next>=int(plugins.size()))return;
        std::swap(plugins[i],plugins[next]);std::swap(widget_heights[i],widget_heights[next]);
        bool enabled=widget_enabled[i];widget_enabled[i]=widget_enabled[next];widget_enabled[next]=enabled;
        std::swap(free_positions[i],free_positions[next]);
        widget_positions.assign(plugins.size(),{});layout_from.clear();layout_moving=false;
        for(auto& w:widgets){w.target={};w.animated={};w.hwnd=nullptr;}
        last_position={};save_manager();service_plugins();poll();build_control_center();
    }
    void import_plugin_file(fs::path file) {
        auto definition=companion::read_plugin(file);
        if(std::any_of(plugins.begin(),plugins.end(),[&](auto const& p){return p->definition.id==definition.id;}))throw std::runtime_error("已有相同 id 的组件，请先卸载或使用不同的 id");
        definition=companion::install_component(file,options.root);
        auto p=std::make_unique<companion::PluginProcess>();p->definition=std::move(definition);p->enabled=false;plugins.push_back(std::move(p));
        widget_enabled.push_back(false);widget_heights.push_back(plugins.back()->definition.height);free_positions.push_back({});
        widgets.resize(plugins.size()-1);widget_positions.resize(plugins.size());
        save_manager();build_control_center();manager_hint.Text(L"已安装组件。点击启用即可显示。");
    }
    void pick_plugin_file() {
        try {
            com_ptr<IFileOpenDialog> dialog;check_hresult(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(dialog.put())));
            DWORD flags=0;dialog->GetOptions(&flags);dialog->SetOptions(flags|FOS_FILEMUSTEXIST|FOS_FORCEFILESYSTEM);
            COMDLG_FILTERSPEC filter_spec{L"组件描述文件 (*.json)",L"*.json"};dialog->SetFileTypes(1,&filter_spec);dialog->SetTitle(L"添加自定义组件 — 选择 widget.json");
            auto result=dialog->Show(manager_hwnd);if(result==HRESULT_FROM_WIN32(ERROR_CANCELLED))return;check_hresult(result);
            com_ptr<IShellItem> selected;check_hresult(dialog->GetResult(selected.put()));PWSTR path=nullptr;check_hresult(selected->GetDisplayName(SIGDN_FILESYSPATH,&path));
            fs::path file(path);CoTaskMemFree(path);import_plugin_file(file);
        }catch(std::exception const& e){manager_hint.Text(to_hstring(e.what()));}
        catch(hresult_error const& e){manager_hint.Text(L"添加失败："+std::wstring(e.message()));}
    }
    void remove_custom_plugin(size_t i) {
        if(i>=plugins.size())return;
        auto& p=*plugins[i];bool enabled=widget_enabled[i];p.stop();
        try {companion::archive_component(p.definition,options.root);}
        catch(...) {if(enabled)start_plugin(i);throw;}
        plugins.erase(plugins.begin()+i);
        widget_enabled.erase(widget_enabled.begin()+i);widget_heights.erase(widget_heights.begin()+i);free_positions.erase(free_positions.begin()+i);
        dragging=false;dragging_handle=nullptr;layout_from.clear();layout_moving=false;
        widgets.resize(plugins.empty()?0:plugins.size()-1);for(auto& w:widgets){w.target={};w.animated={};w.hwnd=nullptr;}
        widget_positions.assign(plugins.size(),{});last_position={};animated_position={};save_manager();service_plugins();poll();build_control_center();
        manager_hint.Text(L"已卸载组件，配置和数据已保留。需要时可在组件库重新安装。");
    }
    void rescan_plugins() {
        catalog_errors.clear();
        for(auto const& dir:fs::directory_iterator(options.root/L"plugins"))if(dir.is_directory()&&fs::exists(dir.path()/L"widget.json")) {
            try {
                auto p=companion::read_plugin(dir.path()/L"widget.json");
                auto existing=std::find_if(plugins.begin(),plugins.end(),[&](auto const& item){return item->definition.id==p.id;});
                if(existing!=plugins.end()) {
                    auto i=size_t(existing-plugins.begin());(*existing)->definition=std::move(p);widget_heights[i]=(*existing)->definition.height;
                    if(widget_enabled[i])start_plugin(i);
                } else {
                    auto item=std::make_unique<companion::PluginProcess>();item->definition=std::move(p);item->enabled=false;plugins.push_back(std::move(item));
                    widget_enabled.push_back(false);widget_heights.push_back(plugins.back()->definition.height);free_positions.push_back({});widgets.resize(plugins.size()-1);widget_positions.resize(plugins.size());
                }
            }catch(std::exception const& e){catalog_errors+=dir.path().filename().wstring()+L"："+std::wstring(to_hstring(e.what()))+L"\n";}
            catch(...){catalog_errors+=dir.path().filename().wstring()+L"：组件描述文件无效\n";}
        }
        save_manager();service_plugins();poll();build_control_center();if(catalog_errors.empty())manager_hint.Text(L"组件目录已重新扫描。");
    }
    void build_control_center() {
        if(!manager) {
            manager=Window();manager.Title(L"随行组件 · 控制中心");check_hresult(manager.as<IWindowNative>()->get_WindowHandle(&manager_hwnd));
            companion::set_window_icon(manager,companion::bundled_icon(L"app",true));
            UINT dpi=GetDpiForWindow(manager_hwnd);MONITORINFO info{sizeof(info)};GetMonitorInfoW(MonitorFromWindow(manager_hwnd,MONITOR_DEFAULTTONEAREST),&info);
            manager.AppWindow().Resize({std::min(MulDiv(960,dpi,96),int(info.rcWork.right-info.rcWork.left)-32),std::min(MulDiv(900,dpi,96),int(info.rcWork.bottom-info.rcWork.top)-32)});manager.SystemBackdrop(Media::MicaBackdrop());
            manager.Closed([this](auto const&,WindowEventArgs const& args){if(!closing){args.Handled(true);hide_manager();}});
        }
        theme_titlebar(manager);
        StackPanel page;page.Spacing(14);StackPanel listing;listing.Spacing(14);
        // WinUI scroll indicators overlay their viewport. Reserve a content-free
        // lane wide enough for the expanded scrollbar in each independent pane.
        page.Margin({0,0,20,0});listing.Margin({0,0,20,0});
        StackPanel header;header.Spacing(6);header.Children().Append(icon_label(L"随行组件",30,44,companion::bundled_icon(L"app")));header.Children().Append(text(L"统一管理你的组件，随 Windows 通知中心或快速设置出现。",14));
        page.Children().Append(text(L"管理与显示",20));StackPanel controls;controls.Spacing(10);
        controls.Children().Append(button(L"预览 / 收起组件",[this]{toggle_preview();}));
        controls.Children().Append(button(L"添加组件文件",[this]{pick_plugin_file();}));
        controls.Children().Append(button(L"已安装组件",[this]{library_view=false;build_control_center();}));
        controls.Children().Append(button(L"组件库 / 自定义组件",[this]{library_view=true;build_control_center();}));
        page.Children().Append(controls);
        StackPanel startup;startup.Spacing(12);startup.Children().Append(text(tr(L"启动与托盘",L"Startup and tray"),18));
        auto_start_switch=ToggleSwitch();auto_start_switch.Header(box_value(tr(L"开机自启动",L"Start with Windows")));
        silent_start_switch=ToggleSwitch();silent_start_switch.Header(box_value(tr(L"开机静默启动",L"Start silently at logon")));
        // Populate before subscribing: opening the manager must never change the Run key or startup.json.
        refresh_startup_controls();
        auto_start_switch.Toggled([this](auto const& sender,auto const&){if(!updating_startup_controls)set_auto_start(sender.template as<ToggleSwitch>().IsOn());});
        silent_start_switch.Toggled([this](auto const& sender,auto const&){if(!updating_startup_controls)set_silent_start(sender.template as<ToggleSwitch>().IsOn());});
        startup.Children().Append(auto_start_switch);startup.Children().Append(silent_start_switch);
        startup.Children().Append(text(tr(L"静默启动只影响开机登录，双击程序仍打开控制中心。",L"Silent startup applies at logon. Opening the app normally still shows the control center."),12));
        hide_tray_switch=ToggleSwitch();hide_tray_switch.Header(box_value(tr(L"隐藏托盘图标",L"Hide tray icon")));
        hide_tray_switch.OnContent(box_value(tr(L"已隐藏",L"Hidden")));hide_tray_switch.OffContent(box_value(tr(L"显示",L"Visible")));
        refresh_tray_controls();
        hide_tray_switch.Toggled([this](auto const& sender,auto const&){if(!updating_tray_controls)set_hide_tray_icon(sender.template as<ToggleSwitch>().IsOn());});
        startup.Children().Append(hide_tray_switch);
        startup.Children().Append(text(tr(L"隐藏图标后程序仍运行，双击主程序可重新打开设置。",L"The app keeps running with its icon hidden. Open the executable again to return to settings."),12));
        page.Children().Append(card(startup));
        StackPanel files;files.Spacing(10);
        files.Children().Append(button(L"打开组件目录",[this]{ShellExecuteW(manager_hwnd,L"open",(options.root/L"plugins").c_str(),nullptr,nullptr,SW_SHOWNORMAL);}));
        files.Children().Append(button(L"重新扫描",[this]{rescan_plugins();}));page.Children().Append(files);
        StackPanel layout;layout.Spacing(12);ToggleSwitch automatic;automatic.Header(box_value(L"自动排列 · 右下角优先"));automatic.IsOn(auto_arrange);
        automatic.Toggled([this](auto const& sender,auto const&){bool next=sender.template as<ToggleSwitch>().IsOn();if(next!=auto_arrange){clear_dismissal();toggle_arrangement();save_manager();}});layout.Children().Append(automatic);
        layout.Children().Append(text(L"关闭后可拖动组件标题。默认靠右排列，避开系统面板。",12));
        StackPanel settings;settings.Spacing(12);
        ComboBox follow;follow.Header(box_value(L"跟随面板"));for(auto label:{L"通知中心和快速设置",L"仅通知中心",L"仅快速设置"})follow.Items().Append(box_value(label));follow.SelectedIndex(int(filter));
        follow.SelectionChanged([this](auto const& sender,auto const&){auto box=sender.template as<ComboBox>();if(box.SelectedIndex()<0)return;filter=unsigned(box.SelectedIndex());session.preview=false;clear_dismissal();save_manager();poll();});settings.Children().Append(follow);
        ComboBox theme;theme.Header(box_value(L"外观"));for(auto label:{L"跟随 Windows",L"深色",L"浅色"})theme.Items().Append(box_value(label));theme.SelectedIndex(theme_override?(dark?1:2):0);
        theme.SelectionChanged([this](auto const& sender,auto const&){int i=sender.template as<ComboBox>().SelectedIndex();if(i<0)return;theme_override=i!=0;dark=i==0?shell_dark_theme():i==1;build_ui();theme_titlebar(manager);theme_titlebar(editor);save_manager();service_plugins();});settings.Children().Append(theme);
        layout.Children().Append(settings);layout.Children().Append(button(L"重置摆放位置",[this]{free_positions=std::vector<FreePosition>(plugins.size());save_manager();clear_dismissal();poll();}));page.Children().Append(card(layout));
        manager_hint=text(catalog_errors,13);manager_hint.Visibility(catalog_errors.empty()?Visibility::Collapsed:Visibility::Visible);
        manager_hint.RegisterPropertyChangedCallback(TextBlock::TextProperty(),[](auto const& sender,auto const&){
            auto label=sender.template as<TextBlock>();label.Visibility(label.Text().empty()?Visibility::Collapsed:Visibility::Visible);
        });listing.Children().Append(manager_hint);
        StackPanel label;label.Orientation(Orientation::Horizontal);label.Spacing(14);label.Children().Append(text(L"我的组件",22));
        label.Children().Append(button(L"全部启用",[this]{for(size_t i=0;i<plugins.size();++i)if(!widget_enabled[i])set_plugin_enabled(i,true);build_control_center();}));
        label.Children().Append(button(L"全部停用",[this]{for(size_t i=0;i<plugins.size();++i)if(widget_enabled[i])set_plugin_enabled(i,false);build_control_center();}));listing.Children().Append(label);
        plugin_status.clear();
        if(library_view) {
            listing.Children().Clear();listing.Children().Append(text(L"组件库 / 自定义组件",22));
            listing.Children().Append(text(L"所有组件均可按需安装、启用和卸载。卸载后保留配置与数据，重新安装可继续使用。",13));
            listing.Children().Append(manager_hint);
            for(auto const& definition:available_components()) {
                bool installed=std::any_of(plugins.begin(),plugins.end(),[&](auto const& p){return p->definition.id==definition.id;});
                StackPanel body;body.Spacing(10);body.Children().Append(icon_label(definition.name,18,32,companion::component_icon(definition)));
                if(!definition.description.empty())body.Children().Append(text(definition.description,13));
                auto file=definition.manifest;auto add=button(installed?L"已安装":L"安装组件",[this,file]{
                    try{import_plugin_file(file);set_plugin_enabled(plugins.size()-1,true);build_control_center();}
                    catch(std::exception const& e){manager_hint.Text(to_hstring(e.what()));}catch(hresult_error const& e){manager_hint.Text(e.message());}
                });add.IsEnabled(!installed);body.Children().Append(add);listing.Children().Append(card(body));
            }
        }
        if(!library_view&&plugins.empty())listing.Children().Append(text(L"尚未安装组件。打开“组件库 / 自定义组件”选择需要的组件，也可导入自己的组件包。",14));
        if(!library_view)for(size_t i=0;i<plugins.size();++i) {
            auto& p=*plugins[i];StackPanel body;body.Spacing(8);Grid top;
            auto title=icon_label(p.definition.name,18,32,companion::component_icon(p.definition));title.Margin({0,0,150,0});top.Children().Append(title);ToggleSwitch enabled;enabled.IsOn(widget_enabled[i]);enabled.HorizontalAlignment(HorizontalAlignment::Right);enabled.OnContent(box_value(L"已启用"));enabled.OffContent(box_value(L"已停用"));
            enabled.Toggled([this,i](auto const& sender,auto const&){set_plugin_enabled(i,sender.template as<ToggleSwitch>().IsOn());});top.Children().Append(enabled);body.Children().Append(top);
            if(!p.definition.description.empty())body.Children().Append(text(p.definition.description,12));
            auto status_label=text(L"",12);status_label.Opacity(.7);plugin_status.push_back(status_label);body.Children().Append(status_label);
            StackPanel actions;actions.Orientation(Orientation::Horizontal);actions.Spacing(10);
            if(p.definition.document.GetNamedBoolean(L"has_settings",true))actions.Children().Append(button(p.definition.document.GetNamedString(L"settings_label",L"组件设置"),[this,i]{
                auto kind=plugins[i]->definition.kind;
                if(kind==L"text"||kind==L"links")ShellExecuteW(manager_hwnd,L"open",plugins[i]->definition.manifest.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
                else plugin_settings(i);
            }));
            actions.Children().Append(button(L"重启组件",[this,i]{if(!widget_enabled[i])set_plugin_enabled(i,true);else start_plugin(i);update_plugin_status();}));
            auto up=button(L"上移",[this,i]{move_plugin(i,-1);});up.IsEnabled(i>0);actions.Children().Append(up);
            auto down=button(L"下移",[this,i]{move_plugin(i,1);});down.IsEnabled(i+1<plugins.size());actions.Children().Append(down);
            actions.Children().Append(button(L"卸载组件",[this,i]{try{remove_custom_plugin(i);}catch(std::exception const& e){manager_hint.Text(to_hstring(e.what()));}catch(hresult_error const& e){manager_hint.Text(e.message());}}));
            body.Children().Append(actions);listing.Children().Append(card(body));
        }
        page.Children().Append(button(L"退出主程序和所有组件",[this]{shutdown();}));
        Grid frame;frame.Padding({24,20,24,24});frame.RowSpacing(20);frame.ColumnSpacing(24);frame.RequestedTheme(dark?ElementTheme::Dark:ElementTheme::Light);manager_page=frame;
        frame.Background(SolidColorBrush(dark?Windows::UI::Color{255,32,32,32}:Windows::UI::Color{255,243,243,243}));
        RowDefinition header_row;header_row.Height(GridLength{1,GridUnitType::Auto});frame.RowDefinitions().Append(header_row);
        RowDefinition body_row;body_row.Height(GridLength{1,GridUnitType::Star});frame.RowDefinitions().Append(body_row);
        ColumnDefinition left;left.Width(GridLength{252,GridUnitType::Pixel});frame.ColumnDefinitions().Append(left);
        ColumnDefinition right;right.Width(GridLength{1,GridUnitType::Star});frame.ColumnDefinitions().Append(right);
        Grid::SetColumnSpan(header,2);frame.Children().Append(header);
        ScrollViewer navigation;navigation.Content(page);navigation.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);navigation.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);Grid::SetRow(navigation,1);frame.Children().Append(navigation);
        ScrollViewer list;list.Content(listing);list.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);list.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);Grid::SetRow(list,1);Grid::SetColumn(list,1);frame.Children().Append(list);
        manager.Content(frame);theme_titlebar(manager);update_plugin_status();
    }
    void show_manager(){if(!coordinator())return;build_control_center();manager.Activate();}
