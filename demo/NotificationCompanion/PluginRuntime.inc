    StackPanel plugin_body(size_t builtin_index) {
        StackPanel body;body.Spacing(12);
        if(builtin_index==0) {
            body.Spacing(10);body.Children().Append(heading(tr(L"天气",L"Weather"),child_channel?companion::component_icon(plugin_definition):companion::bundled_icon(L"weather")));
            weather_title=text(weather_city,13);body.Children().Append(weather_title);
            Grid reading;weather_temp=text(L"--°",38);reading.Children().Append(weather_temp);
            weather_symbol=text(L"☁",38);weather_symbol.HorizontalAlignment(HorizontalAlignment::Right);reading.Children().Append(weather_symbol);body.Children().Append(reading);
            weather_details=text(tr(L"选择城市，查看实时天气",L"Choose a city for current weather"),12);body.Children().Append(weather_details);
            weather_status=text(tr(L"尚未更新",L"Not updated yet"),12);weather_status.Opacity(0.65);body.Children().Append(weather_status);
            weather_credit=text(L"",11);weather_credit.Opacity(.6);body.Children().Append(weather_credit);update_weather_credit();
        } else if(builtin_index==1) {
            body.Spacing(8);
            body.Children().Append(heading(tr(L"待办事项",L"To do"),child_channel?companion::component_icon(plugin_definition):companion::bundled_icon(L"todo")));task_count=text(L"",12);task_count.Opacity(.65);body.Children().Append(task_count);
            append_task_controls(body,140);
        } else if(builtin_index==2) {
            body.Children().Append(heading(tr(L"常用入口",L"Quick access"),child_channel?companion::component_icon(plugin_definition):companion::bundled_icon(L"quick")));
            quick_list=StackPanel();quick_list.Spacing(10);body.Children().Append(quick_list);render_quick_entries();
        } else if(builtin_index==3) {
            body.Children().Append(heading(tr(L"系统状态",L"System status"),child_channel?companion::component_icon(plugin_definition):companion::bundled_icon(L"system")));Grid charts;charts.ColumnSpacing(12);for(int i=0;i<2;++i){ColumnDefinition column;column.Width({1,GridUnitType::Star});charts.ColumnDefinitions().Append(column);}
            cpu_plot=std::make_unique<companion::PerformancePlot>(Windows::UI::Color{255,94,180,230});memory_plot=std::make_unique<companion::PerformancePlot>(Windows::UI::Color{255,186,140,230});
            cpu_caption=text(L"CPU  --%",12);memory_caption=text(tr(L"内存  --%",L"Memory  --%"),12);
            for(int i=0;i<2;++i){StackPanel chart;chart.Spacing(6);chart.Children().Append(i?memory_caption:cpu_caption);chart.Children().Append(i?memory_plot->canvas:cpu_plot->canvas);Grid::SetColumn(chart,i);charts.Children().Append(chart);}body.Children().Append(charts);
            body.Children().Append(button(tr(L"打开任务管理器",L"Open Task Manager"),[]{ShellExecuteW(nullptr,L"open",L"taskmgr.exe",nullptr,nullptr,SW_SHOWNORMAL);}));
        }
        return body;
    }
    StackPanel custom_plugin_body() {
        StackPanel body;body.Spacing(12);body.Children().Append(heading(plugin_definition.name,companion::component_icon(plugin_definition)));
        auto const& j=plugin_definition.document;auto kind=plugin_definition.kind;
        if(kind==L"native") {
            native_body=ContentControl();native_body.HorizontalContentAlignment(HorizontalAlignment::Stretch);native_body.VerticalContentAlignment(VerticalAlignment::Stretch);
            native_body.Height(plugin_definition.height-72);body.Children().Append(native_body);
        } else if(kind==L"note") {
            TextBox note;note.AcceptsReturn(true);note.TextWrapping(TextWrapping::Wrap);note.Height(plugin_definition.height-125);note.PlaceholderText(L"写下你的想法…");note.MaxLength(4000);note.IsReadOnly(true);
            std::wstring value(j.GetNamedString(L"text",L""));
            try{auto state=JsonObject::Parse(to_hstring(companion::read_plugin_file(options.root/L"note.json")));value=state.GetNamedString(L"text",value);}catch(...){}
            note.Text(value);note_editor=note;
            note.TextChanged([this](auto const&,auto const&){note_due=GetTickCount64()+500;});body.Children().Append(note);
            note.IsTabStop(false);
            note_edit_button=button(tr(L"编辑便签",L"Edit note"),[this]{if(inline_editing)finish_inline_edit();else begin_inline_edit();});body.Children().Append(note_edit_button);
        } else if(kind==L"checklist") {
            if(!fs::exists(options.root/L"widgets.json")) {
                tasks.clear();for(auto const& item:j.GetNamedArray(L"items",Windows::Data::Json::JsonArray()))tasks.push_back({std::wstring(item.GetString()),false});save_state();
            }
            body.Spacing(8);task_count=text(L"",12);body.Children().Append(task_count);
            append_task_controls(body,std::max(40,plugin_definition.height-140));
        } else {
            StackPanel content;content.Spacing(10);
            auto description=j.GetNamedString(L"text",L"");if(!description.empty())content.Children().Append(text(description,14));
            if(kind==L"links")for(auto const& item:j.GetNamedArray(L"items",Windows::Data::Json::JsonArray())) {
                auto link=item.GetObject();std::wstring url(link.GetNamedString(L"url"));
                content.Children().Append(button(link.GetNamedString(L"title"),[url]{ShellExecuteW(nullptr,L"open",url.c_str(),nullptr,nullptr,SW_SHOWNORMAL);}));
            }
            ScrollViewer scroll;scroll.MaxHeight(plugin_definition.height-70);scroll.Content(content);body.Children().Append(scroll);
        }
        return body;
    }
    void save_note() {
        if(!note_editor)return;JsonObject state;field(state,L"text",note_editor.Text());companion::write_json_atomic(options.root/L"note.json",state);note_due=0;
    }
    void poll_note_autosave(uint64_t now) {
        if(note_due&&now>=note_due)save_note();
    }
    void plugin_tick() {
        if(!child_channel||!child_channel->wire)return;
        if(acrylic_attached&&backdrop&&FAILED(backdrop->last_error)) {
            window.SystemBackdrop(nullptr);acrylic_attached=false;apply_surface_theme(root,false,backdrop.get());log(L"plugin_backdrop_fallback");
        }
        auto* wire=child_channel->wire;
        if(!IsWindow(wire->parent)){shutdown();return;}
        companion::PluginFrame frame;
        if(child_channel->read(frame)) {
            layout_work=frame.work;layout_reserved=frame.reserved;layout_dpi=frame.dpi;auto_arrange=frame.automatic!=0;
            widget_positions={frame.target};last_position=frame.target;motion.phase=companion::MotionPhase(frame.phase);
            root.IsHitTestVisible(motion.phase!=companion::MotionPhase::Exiting);
            if(dark!=(frame.dark!=0)){dark=frame.dark!=0;apply_surface_theme(root,acrylic_attached,backdrop.get());theme_titlebar(editor);}
        }
        LONG command=InterlockedExchange(&wire->command,0);
        if(native_plugin){native_plugin->update(hwnd,dark);if(command)native_plugin->event(command==1?"settings":"refresh");InterlockedExchange(&wire->native_renders,LONG(native_plugin->render_count()));return;}
        if(command==1) {
            auto kind=plugin_definition.kind;
#ifdef WIDGET_COMPONENT_KIND
            constexpr wchar_t const* module_kinds[]={L"weather",L"todo",L"quick",L"system"};
            kind=module_kinds[WIDGET_COMPONENT_KIND];
#endif
            if(kind==L"weather")open_weather_settings();
            else if(kind==L"quick")open_quick_editor();
            else if(kind==L"todo"||kind==L"checklist"||kind==L"note")begin_inline_edit();
            else if(kind==L"system")ShellExecuteW(nullptr,L"open",L"taskmgr.exe",nullptr,nullptr,SW_SHOWNORMAL);
        }
        if(command==3)finish_inline_edit();
        if(inline_focus_pending)focus_inline_input();
        poll_features();poll_note_autosave(GetTickCount64());
    }
