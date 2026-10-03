    bool full_card_region(HWND handle) {
        RECT bounds{};check_bool(GetWindowRect(handle,&bounds));
        int width=bounds.right-bounds.left,height=bounds.bottom-bounds.top;
        auto region=CreateRectRgn(0,0,0,0);auto type=GetWindowRgn(handle,region);
        bool full=type==COMPLEXREGION && PtInRegion(region,width/2,0) && PtInRegion(region,width/2,height-1) &&
            PtInRegion(region,0,height/2) && PtInRegion(region,width-1,height/2) &&
            !PtInRegion(region,0,0) && !PtInRegion(region,width-1,0) && !PtInRegion(region,0,height-1) && !PtInRegion(region,width-1,height-1) &&
            !PtInRegion(region,width+1,height/2) && !PtInRegion(region,width/2,height+1);
        DeleteObject(region);return full;
    }
    struct SurfaceChanges {
        unsigned positions=0,nonclient=0;
        static LRESULT CALLBACK observe(HWND handle,UINT message,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data) {
            auto& counts=*reinterpret_cast<SurfaceChanges*>(data);
            if(message==WM_WINDOWPOSCHANGED)++counts.positions;
            if(message==WM_NCCALCSIZE)++counts.nonclient;
            return DefSubclassProc(handle,message,w,l);
        }
    };
    void test_stationary_surfaces() {
        SurfaceChanges counts;
        std::vector<HWND> handles{hwnd};for(auto const& surface:widgets)handles.push_back(surface.hwnd);
        for(auto handle:handles)check_bool(SetWindowSubclass(handle,SurfaceChanges::observe,2,reinterpret_cast<DWORD_PTR>(&counts)));
        auto detach=[&]{for(auto handle:handles)RemoveWindowSubclass(handle,SurfaceChanges::observe,2);};
        try {
            RECT work{0,0,2880,1548},reserved{2340,0,2880,1530},first{};
            for(bool theme:{false,true}) {
                dark=theme;build_ui();layout_widgets(work,reserved,96,first);last_position=first;
                motion.request(true,false,companion::motion_now_ms());motion_frame();counts={};
                for(int i=0;i<120;++i){layout_widgets(work,reserved,96,first);motion_frame();}
                expect(counts.positions==0 && counts.nonclient==0,L"Settled light and dark cards receive no native position or nonclient layout changes across 120 polls");
                for(auto handle:handles) {
                    expect(!IsWindowVisible(handle),L"Stationary surface regression keeps all cards hidden");
                    expect(full_card_region(handle),L"Every stationary card keeps full content and four rounded corners without system decoration");
                }
                layout_widgets(work,reserved,144,first);last_position=first;motion_frame();
                expect(counts.positions>0,L"A DPI layout change still updates native widget geometry");
                for(size_t i=0;i<handles.size();++i) {
                    RECT actual{};check_bool(GetWindowRect(handles[i],&actual));
                    expect(EqualRect(&actual,&widget_positions[i]),L"Every resized native card reaches its new physical slot");
                    expect(full_card_region(handles[i]),L"DPI resizing restores the full rounded card region");
                }
                counts={};for(int i=0;i<120;++i){layout_widgets(work,reserved,144,first);motion_frame();}
                expect(counts.positions==0 && counts.nonclient==0,L"Cards stop changing native geometry after DPI layout settles");
            }
            // The last animated pixel can equal the settled pixel. Region
            // cleanup must still run once without requiring another move.
            motion.request(false,false,companion::motion_now_ms());
            motion.request(true,true,companion::motion_now_ms());
            animate_surface(hwnd,last_position,animated_position,animated_clipped,1,0);
            expect(animated_clipped,L"An animation installs its temporary native clipping region");
            counts={};motion.request(true,false,companion::motion_now_ms());motion_frame();
            expect(full_card_region(hwnd) && !animated_clipped,L"Animation completion restores the card shape even when the rounded position is unchanged");
            counts={};for(int i=0;i<120;++i)motion_frame();
            expect(counts.positions==0 && counts.nonclient==0,L"Repeated settled frames do not reapply region cleanup");
            motion.request(false,false,companion::motion_now_ms());finish_hide();dark=true;build_ui();detach();
        }catch(...){detach();throw;}
    }
    void test_feature_settings() {
        companion::WeatherConfig config;config.custom=true;config.url=L"https://example.com/weather?city={city}&lat={lat}&lon={lon}";config.header_value=L"Bearer test-only-secret";config.interval_minutes=15;config.latitude=22.3;config.longitude=114.2;
        config.temperature=L"now.temp";config.feels=L"now.feels";config.humidity=L"now.humidity";config.wind=L"now.wind";config.description=L"weather[0].description";config.fahrenheit=true;config.wind_unit=1;
        auto json=JsonObject::Parse(LR"({"now":{"temp":"68","feels":59,"humidity":56,"wind":"2"},"weather":[{"description":"晴间多云"}]})");
        auto reading=companion::parse_weather(json,config,L"深圳");
        expect(reading.valid&&std::abs(reading.temperature-20)<.001&&reading.has_feels&&std::abs(reading.feels-15)<.001,L"Custom API parses nested numbers and numeric strings and converts Fahrenheit");
        expect(reading.has_wind&&std::abs(reading.wind-7.2)<.001&&reading.has_humidity&&reading.humidity==56,L"Custom API converts wind units and retains humidity");
        expect(reading.description==L"晴间多云"&&reading.city==L"深圳",L"Custom API supports array paths and missing city fallback");
        auto sparse=companion::parse_weather(JsonObject::Parse(LR"({"now":{"temp":68}})"),config,L"深圳");
        expect(!sparse.has_feels&&!sparse.has_humidity&&!sparse.has_wind&&sparse.code==-1,L"Missing optional API fields are omitted rather than fabricated");
        bool rejected=false;try{companion::parse_weather(JsonObject::Parse(LR"({"now":{"temp":"68 junk"}})"),config,L"深圳");}catch(...){rejected=true;}expect(rejected,L"Malformed temperature cannot become a valid reading");
        expect(!companion::weather_field(json,L"weather[9].description")&&!companion::weather_field(json,L"now.temp."),L"Invalid or out-of-range JSON paths fail safely");
        auto url=companion::weather_url(config,L"Hong Kong&深圳");expect(url.find(L"Hong%20Kong%26%E6%B7%B1%E5%9C%B3")!=url.npos&&url.find(L"{")==url.npos,L"API placeholders encode city and replace coordinates");
        auto encrypted=companion::weather_config_json(config);expect(std::wstring(encrypted.Stringify()).find(L"test-only-secret")==std::wstring::npos,L"API credentials are not persisted as plaintext");
        auto restored=companion::weather_config_from_json(encrypted);expect(restored.header_value==config.header_value&&restored.url==config.url&&restored.interval_minutes==15,L"DPAPI encrypted credentials and custom API settings round-trip");
        expect(companion::weather_due(10,0,config)&&!companion::weather_due(900099,100,config)&&companion::weather_due(900100,100,config),L"Automatic weather update runs on startup and at the configured interval");
        auto reject_config=[&](companion::WeatherConfig const& c){bool failed=false;try{companion::validate_weather_config(c);}catch(...){failed=true;}return failed;};
        auto invalid=config;invalid.url=L"http://example.com";expect(reject_config(invalid),L"Custom API requires HTTPS");invalid=config;invalid.header_value=L"token\r\nInjected: value";expect(reject_config(invalid),L"API authentication cannot inject request headers");
        invalid=config;invalid.url=L"https://user:pass@example.com/weather";expect(reject_config(invalid),L"API URLs cannot include embedded credentials");
        auto saved_config=weather_config;auto saved_entries=quick_entries;weather_config=config;quick_entries[0]={L"我的文件夹",L"C:\\Users\\Public",L""};quick_entries[1]={L"我的应用",L"C:\\Tools\\demo.exe",L"--name \"中文参数\""};quick_entries[2]={L"",L"",L""};save_state();
        weather_config={};quick_entries=companion::default_quick_entries();tasks.clear();load_state();
        expect(weather_config.custom&&weather_config.header_value==config.header_value&&weather_config.temperature==L"now.temp",L"Custom weather settings survive a component restart");
        expect(quick_entries[0].name==L"我的文件夹"&&quick_entries[1].arguments==L"--name \"中文参数\""&&quick_entries[2].target.empty(),L"Three custom shortcuts persist Unicode, arguments and empty slots");
        render_quick_entries();expect(quick_list.Children().Size()==3&&!quick_list.Children().GetAt(2).as<Button>().IsEnabled(),L"Quick access retains exactly three slots and disables an empty target");
        expect(!companion::valid_quick_entry({L"",L"app.exe",L""})&&!companion::valid_quick_entry({L"name",L"\"app.exe\"",L""}),L"Quick access rejects missing names and quoted targets");
        open_quick_editor();expect(editor&&editor.AppWindow().TitleBar().BackgroundColor().Value().R==32,L"Quick shortcut editor uses the matching dark title bar");close_editor();editor=nullptr;
        open_weather_settings();expect(editor&&editor.AppWindow().TitleBar().BackgroundColor().Value().R==32,L"Weather API editor uses the matching dark title bar");close_editor();editor=nullptr;
        companion::PerformancePlot plot(Windows::UI::Color{255,94,180,230});for(int i=0;i<80;++i)plot.sample(i);expect(plot.size()==60&&plot.latest()==79&&plot.canvas.Height()==64,L"Compact performance chart retains one minute of samples at 64 DIP high");plot.sample(200);plot.sample(std::numeric_limits<double>::quiet_NaN());expect(plot.size()==60&&plot.latest()==100,L"Performance plots clamp percentages and ignore invalid samples");
        weather_config=saved_config;quick_entries=saved_entries;save_state();render_quick_entries();
    }
    void test_task_editor() {
        hide_completed_tasks=false;tasks={{L"待删除测试项",false},{L"保留测试项",true}};render_tasks();begin_inline_edit();
        auto original_input=task_input;auto original_content=widgets[0].window.Content();auto card=surface_handle(1);
        task_input.Text(L"尚未提交的中文草稿");
        expect(!editor&&inline_editing&&task_input_row.Visibility()==Visibility::Visible,L"Task editing lives inside the card without creating another Window");
        expect(!IsWindowVisible(card),L"Inline editing regression test stays hidden");
        expect(!(GetWindowLongPtrW(card,GWL_EXSTYLE)&WS_EX_NOACTIVATE)&&SendMessageW(card,WM_MOUSEACTIVATE,0,0)==MA_ACTIVATE,L"Only the editing card permits keyboard activation");
        auto pending_row=task_list.Children().GetAt(0);auto completed_row=task_list.Children().GetAt(1);
        auto pending_check=pending_row.as<Grid>().Children().GetAt(0).as<CheckBox>();
        set_hide_completed_tasks(true);
        expect(pending_row.Visibility()==Visibility::Visible&&completed_row.Visibility()==Visibility::Collapsed&&tasks.size()==2,L"Hide completed keeps pending rows and retains completed data");
        expect(task_input==original_input&&task_input.Text()==L"尚未提交的中文草稿",L"Filtering completed tasks preserves the inline input and draft");
        pending_check.IsChecked(true);
        expect(tasks[0].done&&pending_row.Visibility()==Visibility::Collapsed&&task_empty_message.Visibility()==Visibility::Visible,L"Completing a task immediately hides it and shows the all-completed message");
        expect(task_list.Children().GetAt(0)==pending_row&&task_list.Children().GetAt(1)==completed_row,L"Completion filtering keeps the existing task controls alive");
        hide_completed_tasks=false;tasks.clear();load_state();render_tasks();
        expect(hide_completed_tasks&&tasks.size()==2&&tasks[0].done&&tasks[1].done,L"Completed visibility preference and task data survive reload");
        set_hide_completed_tasks(false);
        expect(task_list.Children().GetAt(0).Visibility()==Visibility::Visible&&task_list.Children().GetAt(1).Visibility()==Visibility::Visible&&task_empty_message.Visibility()==Visibility::Collapsed,L"Show completed restores every completed item");
        hide_completed_tasks=true;tasks.clear();load_state();
        expect(!hide_completed_tasks,L"Showing completed also persists across reload");
        remove_inline_task(0);
        expect(!editor&&widgets[0].window.Content()==original_content&&IsWindow(card),L"Deleting a task keeps the existing component window and content alive");
        expect(task_input==original_input&&task_input.Text()==L"尚未提交的中文草稿",L"Deleting a task preserves the input control and unsaved draft");
        expect(tasks.size()==1&&tasks[0].title==L"保留测试项"&&tasks[0].done,L"Deletion preserves the remaining completion state");
        auto saved=JsonObject::Parse(to_hstring(companion::read_plugin_file(options.root/L"widgets.json")));
        expect(saved.GetNamedArray(L"tasks").Size()==1&&saved.GetNamedArray(L"tasks").GetObjectAt(0).GetNamedString(L"title")==L"保留测试项",L"Deletion is persisted without rebuilding the editor");
        edit_inline_task(0);task_input.Text(L"修改完成项");expect(submit_inline_task()&&tasks[0].done&&tasks[0].title==L"修改完成项",L"Inline rename preserves completion state and saves the task");
        task_input.Text(L"  新增中文待办  ");expect(submit_inline_task()&&tasks.size()==2&&tasks[1].title==L"新增中文待办"&&!tasks[1].done,L"Inline add trims input and creates an unfinished task");
        edit_inline_task(1);task_input.Text(L"编辑中的草稿");remove_inline_task(0);
        expect(task_edit_index==0&&task_input.Text()==L"编辑中的草稿",L"Deleting a preceding task keeps the correct edit target and draft");
        expect(submit_inline_task()&&tasks[0].title==L"编辑中的草稿",L"Saving after deleting a preceding row updates the intended task");
        edit_inline_task(0);task_input.Text(L"保留草稿");remove_inline_task(0);
        expect(tasks.empty()&&task_edit_index==-1&&task_input.Text()==L"保留草稿"&&inline_editing,L"Deleting the final edited row keeps the card editor and draft alive");
        dark=false;apply_surface_theme(widgets[0].root,false,widgets[0].backdrop.get());
        expect(widgets[0].window.Content()==original_content&&task_input==original_input&&task_input.Text()==L"保留草稿",L"Changing card theme preserves the inline input and draft");
        finish_inline_edit();
        expect(!inline_editing&&task_input_row.Visibility()==Visibility::Collapsed&&task_input.Text()==L"保留草稿",L"Leaving edit mode preserves an unsubmitted draft");
        expect((GetWindowLongPtrW(card,GWL_EXSTYLE)&WS_EX_NOACTIVATE)&&SendMessageW(card,WM_MOUSEACTIVATE,0,0)==MA_NOACTIVATE,L"Leaving edit mode restores normal nonactivating card behavior");
        begin_inline_edit();expect(task_input.Text()==L"保留草稿"&&!editor,L"Reentering inline editing restores the draft without another window");
        task_input.Text(L" \t ");expect(!submit_inline_task()&&tasks.empty()&&task_hint.Visibility()==Visibility::Visible,L"Empty tasks are rejected within the card");
        task_input.Text(L"");finish_inline_edit();dark=true;tasks.clear();save_state();render_tasks();
        auto saved_input_row=task_input_row;task_input_row=nullptr;
        plugin_definition.document=JsonObject();plugin_definition.kind=L"note";plugin_definition.name=L"便签验证";plugin_definition.height=280;
        auto note_body=custom_plugin_body();auto original_note=note_editor;
        begin_inline_edit();note_editor.Text(L"直接在组件内输入中文\n自动保存便签");
        // WinUI TextBox normalizes multiline text to CR separators.
        auto expected_note=note_editor.Text();
        expect(inline_editing&&!editor&&!note_editor.IsReadOnly()&&expected_note==L"直接在组件内输入中文\r自动保存便签",L"Note editing enables the existing card input without another Window");
        // TextChanged is dispatched asynchronously. Exercise the same deadline consumer
        // without pumping input or sleeping on the UI thread of the hidden fixture.
        note_due=GetTickCount64()+500;poll_note_autosave(note_due-1);
        expect(note_due!=0&&!fs::exists(options.root/L"note.json"),L"Note autosave waits until the debounce deadline");
        poll_note_autosave(note_due);
        expect(note_due==0&&fs::exists(options.root/L"note.json"),L"Note autosave commits after its deadline without ending inline editing");
        finish_inline_edit();auto note_state=JsonObject::Parse(to_hstring(companion::read_plugin_file(options.root/L"note.json")));
        expect(note_state.GetNamedString(L"text")==expected_note&&note_editor==original_note&&note_editor.IsReadOnly()&&note_due==0,L"Finishing note editing saves multiline text without replacing the input or creating a Window");
        note_editor=nullptr;note_edit_button=nullptr;task_input_row=saved_input_row;plugin_definition={};
    }
    void test_plugin_catalog() {
        auto directory=options.root/L"catalog-tests";fs::create_directories(directory);auto file=directory/L"widget.json";
        JsonObject j;j.SetNamedValue(L"schema",JsonValue::CreateNumberValue(1));field(j,L"id",L"user.example");field(j,L"name",L"我的中文组件");field(j,L"kind",L"text");field(j,L"text",L"独立保存，不丢中文。");
        auto read=[&](){companion::write_json_atomic(file,j);return companion::read_plugin(file);};
        auto good=read();expect(good.name==L"我的中文组件"&&good.height==240,L"Custom UTF-8 text manifest loads");
        auto reject=[&](wchar_t const* message){bool rejected=false;try{read();}catch(...){rejected=true;}expect(rejected,message);};
        auto image=directory/L"图标 #1.png";fs::copy_file(companion::bundled_icon(L"note"),image,fs::copy_options::overwrite_existing);
        field(j,L"icon",L"图标 #1.png");expect(read().icon==fs::weakly_canonical(image),L"A package-local PNG icon loads with Unicode and spaces");
        expect(companion::image_file_uri(image).Fragment().empty(),L"A literal hash in an icon filename is not treated as a URI fragment");
        field(j,L"icon",L"../escape.png");reject(L"Plugin icon cannot escape its package");field(j,L"icon",L"missing.png");reject(L"Missing declared PNG icon is rejected");j.Remove(L"icon");
        auto shell_icon=LoadImageW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(1),IMAGE_ICON,16,16,LR_SHARED);expect(shell_icon!=nullptr,L"The executable contains a loadable small application icon");
        field(j,L"id",L"../outside");reject(L"Plugin id cannot escape its data directory");field(j,L"id",L"user.example");
        field(j,L"kind",L"weather");reject(L"Domain-specific kinds are not privileged host components");field(j,L"kind",L"text");
        j.SetNamedValue(L"schema",JsonValue::CreateNumberValue(2));reject(L"Unknown plugin schema rejected");j.SetNamedValue(L"schema",JsonValue::CreateNumberValue(1));
        field(j,L"entry",L"example.exe");reject(L"Declarative components do not execute imported programs");j.Remove(L"entry");
        field(j,L"kind",L"native");field(j,L"entry",L"../example.dll");j.SetNamedValue(L"abi",JsonValue::CreateNumberValue(1));reject(L"Native entry cannot escape the mod package");
        field(j,L"entry",L"missing.dll");reject(L"Missing native DLL rejected before installation");j.SetNamedValue(L"abi",JsonValue::CreateNumberValue(9));reject(L"Unknown native ABI rejected");j.Remove(L"entry");j.Remove(L"abi");field(j,L"kind",L"text");
        field(j,L"kind",L"process");field(j,L"entry",L"../outside.exe");j.SetNamedValue(L"abi",JsonValue::CreateNumberValue(1));reject(L"Process entry cannot escape the package");
        field(j,L"entry",L"missing.exe");reject(L"A missing process entry is rejected before installation");
        field(j,L"entry",L"invalid.dll");reject(L"A process package must declare an EXE rather than a DLL");
        j.SetNamedValue(L"abi",JsonValue::CreateNumberValue(2));reject(L"An unsupported process protocol version is rejected");
        j.Remove(L"entry");j.Remove(L"abi");field(j,L"kind",L"text");
        j.SetNamedValue(L"height",JsonValue::CreateNumberValue(601));reject(L"Oversize plugin height rejected");j.Remove(L"height");
        field(j,L"kind",L"links");Windows::Data::Json::JsonArray items;JsonObject link;field(link,L"title",L"示例");field(link,L"url",L"https://example.com/");items.Append(link);j.SetNamedValue(L"items",items);
        expect(read().kind==L"links",L"HTTPS link component accepted");field(link,L"url",L"file:///C:/Windows/example.exe");reject(L"Executable local links rejected");
        field(link,L"url",L"javascript:alert(1)");reject(L"Script URLs rejected");field(link,L"url",L"https://example.com/\nunsafe");reject(L"Control characters rejected in URLs");
        auto source=directory/L"process-package";fs::create_directories(source);
        {std::ofstream stub(source/L"Example.exe",std::ios::binary);stub<<"test package entry only";}
        JsonObject package;package.SetNamedValue(L"schema",JsonValue::CreateNumberValue(1));field(package,L"id",L"user.lifecycle");field(package,L"name",L"独立组件");field(package,L"kind",L"process");field(package,L"entry",L"Example.exe");package.SetNamedValue(L"abi",JsonValue::CreateNumberValue(1));
        companion::write_json_atomic(source/L"widget.json",package);auto isolated=directory/L"lifecycle-data";
        auto installed=companion::install_component(source/L"widget.json",isolated);
        expect(installed.kind==L"process"&&fs::exists(installed.manifest.parent_path()/L"Example.exe"),L"Ordinary process package installs through the same catalog path as custom mods");
        bool duplicate=false;try{companion::install_component(source/L"widget.json",isolated);}catch(...){duplicate=true;}expect(duplicate,L"Duplicate package import keeps the existing install intact");
        auto data=isolated/L"plugin-data"/installed.id;companion::write_json_atomic(data/L"state.json",package);
        auto archive=companion::archive_component(installed,isolated);
        expect(!fs::exists(installed.manifest.parent_path())&&fs::exists(archive/L"Example.exe")&&fs::exists(data/L"state.json"),L"Uninstall archives the complete package and preserves separate data");
        installed=companion::install_component(source/L"widget.json",isolated);
        expect(fs::exists(installed.manifest)&&fs::exists(data/L"state.json"),L"An uninstalled id can be installed again without deleting user data");
        auto external=installed;external.manifest=source/L"widget.json";bool outside=false;
        try{companion::archive_component(external,isolated);}catch(...){outside=true;}expect(outside&&fs::exists(source/L"Example.exe"),L"Uninstall cannot move a package outside the validated installation root");
        auto token=L"Local\\WindowsWidget.Test."+companion::unique_token();companion::PluginChannel writer,reader;
        writer.create(token,control);reader.open(token);companion::PluginFrame frame{{-1920,-100,0,932},{-350,8,-12,920},{-700,600,-380,840},144,0,1,2};writer.publish(frame);
        companion::PluginFrame actual;expect(reader.read(actual)&&actual.dpi==144&&EqualRect(&actual.target,&frame.target),L"Cross-process wire preserves physical geometry and DPI");
        expect(actual.automatic==0&&actual.phase==2,L"Shared frame preserves layout and interaction state");
    }
    void test_component_migration() {
        auto original_root=options.root;auto original_dark=dark;auto original_automatic=auto_arrange;
        options.root=original_root/L"migration-fixture";
        JsonObject old;old.SetNamedValue(L"schema",JsonValue::CreateNumberValue(1));Windows::Data::Json::JsonArray entries;
        for(auto const* id:{L"builtin.todo",L"builtin.weather"}) {
            JsonObject e;field(e,L"id",id);e.SetNamedValue(L"enabled",JsonValue::CreateBooleanValue(false));e.SetNamedValue(L"x",JsonValue::CreateNumberValue(.25));e.SetNamedValue(L"y",JsonValue::CreateNumberValue(.75));e.SetNamedValue(L"saved",JsonValue::CreateBooleanValue(true));entries.Append(e);
        }
        old.SetNamedValue(L"plugins",entries);old.SetNamedValue(L"auto_arrange",JsonValue::CreateBooleanValue(false));
        companion::write_json_atomic(options.root/L"manager.json",old);
        JsonObject legacy;field(legacy,L"weather_city",L"旧的天气城市");companion::write_json_atomic(options.root/L"widgets.json",legacy);
        JsonObject prior;field(prior,L"weather_city",L"独立组件里已保存的城市");
        companion::write_json_atomic(options.root/L"plugin-data"/L"builtin.weather"/L"widgets.json",prior);
        for(auto const* id:{L"user.my-checklist",L"user.my-links"}) {
            JsonObject retired;retired.SetNamedValue(L"schema",JsonValue::CreateNumberValue(1));field(retired,L"id",id);field(retired,L"name",L"退役示例");field(retired,L"kind",L"text");
            companion::write_json_atomic(options.root/L"plugins"/id/L"widget.json",retired);
            companion::write_json_atomic(options.root/L"plugin-data"/id/L"preserved.json",prior);
        }
        initialize_plugins();
        expect(plugins.size()==2&&plugins[0]->definition.id==L"builtin.todo"&&plugins[1]->definition.id==L"builtin.weather",L"Upgrade migrates only the recorded components and retains their order");
        expect(!widget_enabled[0]&&!widget_enabled[1]&&!auto_arrange&&free_positions[0].saved&&free_positions[0].x==.25&&free_positions[0].y==.75,L"Upgrade preserves enabled state, free arrangement and saved positions");
        auto weather_state=JsonObject::Parse(to_hstring(companion::read_plugin_file(plugin_data(plugins[1]->definition)/L"widgets.json")));
        expect(weather_state.GetNamedString(L"weather_city")==L"独立组件里已保存的城市",L"Upgrade never overwrites a module's existing data");
        auto todo_state=JsonObject::Parse(to_hstring(companion::read_plugin_file(plugin_data(plugins[0]->definition)/L"widgets.json")));
        expect(todo_state.GetNamedString(L"weather_city")==L"旧的天气城市",L"Legacy data is copied once when a module has no private state");
        expect(!fs::exists(options.root/L"plugins"/L"user.my-checklist")&&!fs::exists(options.root/L"plugins"/L"user.my-links")&&fs::exists(options.root/L"removed-plugins"),L"Conflicting stock checklist and developer links examples are archived during upgrade");
        expect(fs::exists(options.root/L"plugin-data"/L"user.my-checklist"/L"preserved.json")&&fs::exists(options.root/L"plugin-data"/L"user.my-links"/L"preserved.json"),L"Retiring stock examples preserves their independent user data");
        auto library=available_components();expect(std::none_of(library.begin(),library.end(),[](auto const& item){return retired_example(item);}),L"The library no longer exposes the two retired example packages");
        for(auto& p:plugins)p->stop();plugins.clear();widgets.clear();widget_heights.clear();widget_enabled.clear();free_positions.clear();widget_positions.clear();
        options.root=original_root;dark=original_dark;auto_arrange=original_automatic;initialize_plugins();
        expect(plugins.empty(),L"Migration test remains isolated from the normal installed list");
    }
    void runtime_test_tick() {
        if(GetTickCount64()-started>120000)throw hresult_error(E_FAIL,L"Independent plugin test timed out");
        auto ready=[&](size_t i){return plugins[i]->panel()!=nullptr;};
        if(runtime_step==0) {
            if(plugins.empty()) {
                RECT first{};expect(widget_heights.empty()&&widgets.empty()&&!layout_widgets({0,0,1920,1032},{},96,first),L"Fresh manager starts with an empty, safe optional component list");
                test_component_migration();
                auto packages=available_components();expect(packages.size()>=5,L"Component library exposes standard and user-designed modules as options");
                for(auto const* id:{L"builtin.weather",L"builtin.todo",L"builtin.quick",L"builtin.system"}) {
                    auto found=std::find_if(packages.begin(),packages.end(),[&](auto const& item){return item.id==id;});
                    expect(found!=packages.end(),L"Former builtin is available as an ordinary installable package");
                    import_plugin_file(found->manifest);set_plugin_enabled(plugins.size()-1,true);
                }
            }
            if(!std::all_of(plugins.begin(),plugins.end(),[](auto const& p){return p->panel()!=nullptr;}))return;
            expect(plugins.size()==4,L"Four optional process components installed");std::set<DWORD> ids;
            for(auto const& p:plugins){ids.insert(p->pid);expect(p->definition.kind==L"process"&&fs::exists(companion::executable_plugin_entry(p->definition.manifest,std::wstring(p->definition.document.GetNamedString(L"entry")),true)),L"Optional module has its own packaged executable");expect(p->pid!=GetCurrentProcessId(),L"Plugin does not run inside manager");expect(!IsWindowVisible(p->panel()),L"Plugins begin hidden until the Shell opens");}
            expect(ids.size()==4,L"Every installed component owns a different process");
            test_hidden_manager();
            RECT first{};expect(layout_widgets({0,0,1920,1032},{1572,8,1908,1020},96,first),L"Independent processes participate in coordinated packing");
            JsonObject custom;custom.SetNamedValue(L"schema",JsonValue::CreateNumberValue(1));field(custom,L"id",L"test.custom-list");field(custom,L"name",L"自定义清单验证");field(custom,L"kind",L"checklist");
            Windows::Data::Json::JsonArray items;items.Append(JsonValue::CreateStringValue(L"保留中文"));items.Append(JsonValue::CreateStringValue(L"独立的数据目录"));custom.SetNamedValue(L"items",items);
            fs::copy_file(companion::bundled_icon(L"checklist"),options.root/L"test-icon.png");field(custom,L"icon",L"test-icon.png");
            auto file=options.root/L"test-widget.json";companion::write_json_atomic(file,custom);import_plugin_file(file);set_plugin_enabled(4,true);runtime_step=1;
        } else if(runtime_step==1) {
            if(!ready(4))return;
            auto state=JsonObject::Parse(to_hstring(companion::read_plugin_file(plugin_data(plugins[4]->definition)/L"widgets.json")));
            expect(state.GetNamedArray(L"tasks").Size()==2&&state.GetNamedArray(L"tasks").GetObjectAt(0).GetNamedString(L"title")==L"保留中文",L"Imported custom checklist renders and stores UTF-8 seed data");
            expect(plugins[4]->definition.icon==fs::weakly_canonical(plugins[4]->definition.manifest.parent_path()/L"test-icon.png")&&fs::exists(plugins[4]->definition.icon),L"Declarative import copies its icon and resolves it from the installed package");
            expect(plugins[4]->pid!=plugins[0]->pid&&ready(0),L"Custom component has its own process");
            for(auto const& p:plugins)runtime_pids.push_back(p->pid);
            check_bool(TerminateProcess(plugins[1]->process.get(),73));runtime_step=2;
        } else if(runtime_step==2) {
            if(plugins[1]->process)return;
            expect(plugins[1]->state.find(L"73")!=std::wstring::npos,L"Manager detects a crashed plugin with its exit code");
            for(size_t i:{0u,2u,3u,4u})expect(ready(i)&&plugins[i]->pid==runtime_pids[i],L"One plugin crash leaves peers running");
            start_plugin(1);runtime_step=3;
        } else if(runtime_step==3) {
            if(!ready(1))return;
            expect(plugins[1]->pid!=runtime_pids[1],L"Plugin restarts independently with a new PID");set_plugin_enabled(0,false);
            expect(!plugins[0]->process&&ready(1)&&ready(4),L"Disabling one component only stops its own job");
            set_plugin_enabled(0,true);runtime_step=4;
        } else if(runtime_step==4) {
            if(!ready(0))return;
            move_plugin(4,-1);expect(plugins[3]->definition.id==L"test.custom-list"&&ready(3),L"Reordering preserves plugin identity and running process");
            save_manager();auto state=JsonObject::Parse(to_hstring(companion::read_plugin_file(options.root/L"manager.json")));
            expect(state.GetNamedArray(L"plugins").GetObjectAt(3).GetNamedString(L"id")==L"test.custom-list",L"Manager persists ordering by stable plugin id");
            for(auto const& p:plugins)expect(!IsWindowVisible(p->panel()),L"Management does not leave component windows visible");
            auto sample=companion::executable_path().parent_path()/L"examples"/L"bluetooth-battery"/L"widget.json";
            import_plugin_file(sample);set_plugin_enabled(5,true);runtime_step=5;
        } else if(runtime_step==5) {
            auto snapshot=plugin_data(plugins[5]->definition)/L"last-reading.json";
            if(!ready(5)||!fs::exists(snapshot)||!InterlockedCompareExchange(&plugins[5]->channel->wire->native_renders,0,0))return;
            expect(plugins[5]->definition.kind==L"native"&&plugins[5]->pid!=GetCurrentProcessId(),L"External DLL mod runs in a separate generic plugin process");
            auto reading=JsonObject::Parse(to_hstring(companion::read_plugin_file(snapshot)));
            expect(reading.GetNamedArray(L"slots").Size()==4,L"External Bluetooth plugin reserves exactly four device slots");
            expect(fs::exists(plugins[5]->definition.manifest.parent_path()/L"BluetoothBattery.dll"),L"Import copies the independent DLL package");
            expect(InterlockedCompareExchange(&plugins[5]->channel->wire->native_renders,0,0)>0,L"External mod supplies its own valid XAML UI through the existing SDK");
            plugins[5]->command(2);runtime_step=6;fixture_started=double(GetTickCount64());
        } else if(runtime_step==6) {
            if(GetTickCount64()-fixture_started<800)return;
            expect(ready(5)&&ready(0)&&ready(3),L"Refreshing an external mod leaves manager and other components running");
            set_plugin_enabled(5,false);expect(!plugins[5]->process&&ready(0),L"External DLL unload is confined to its own process");
            auto data=plugin_data(plugins[5]->definition);auto reading=companion::read_plugin_file(data/L"last-reading.json");
            remove_custom_plugin(5);
            expect(plugins.size()==5&&!fs::exists(options.root/L"plugins"/L"mod.bluetooth-battery"),L"Uninstall removes the whole executable package from the installed directory");
            expect(companion::read_plugin_file(data/L"last-reading.json")==reading&&ready(0),L"Uninstall preserves module data and leaves other components running");
            import_plugin_file(companion::executable_path().parent_path()/L"examples"/L"bluetooth-battery"/L"widget.json");set_plugin_enabled(5,true);runtime_step=7;
        } else if(runtime_step==7) {
            if(!ready(5))return;
            expect(ready(5),L"A removed native mod can be installed again with the same id");
            auto packages=available_components();
            JsonObject marker;field(marker,L"value",L"卸载再安装仍然保留的数据");
            auto weather_data=plugin_data(plugins[0]->definition);companion::write_json_atomic(weather_data/L"preserved.json",marker);
            while(!plugins.empty())remove_custom_plugin(0);
            expect(widget_enabled.empty()&&widget_heights.empty()&&widgets.empty()&&widget_positions.empty(),L"All components, including the final module, can be uninstalled");
            auto saved=JsonObject::Parse(to_hstring(companion::read_plugin_file(options.root/L"manager.json")));
            expect(saved.GetNamedArray(L"plugins").Size()==0&&saved.GetNamedNumber(L"library_version")==1,L"An empty installed list is persisted with a completed migration marker");
            initialize_plugins();expect(plugins.empty(),L"Restart and rescan do not resurrect removed standard modules");
            rescan_plugins();expect(plugins.empty(),L"Rescan keeps an intentionally empty installed list");
            auto weather_package=std::find_if(packages.begin(),packages.end(),[](auto const& item){return item.id==L"builtin.weather";});
            import_plugin_file(weather_package->manifest);set_plugin_enabled(0,true);runtime_step=8;
        } else if(runtime_step==8) {
            if(!ready(0))return;
            expect(plugins.size()==1&&plugins[0]->definition.kind==L"process",L"A former builtin reinstalls independently as the only selected module");
            auto marker=JsonObject::Parse(to_hstring(companion::read_plugin_file(plugin_data(plugins[0]->definition)/L"preserved.json")));
            expect(marker.GetNamedString(L"value")==L"卸载再安装仍然保留的数据",L"Reinstall reuses the original per-module data directory");
            auto packages=available_components();
            for(auto const* id:{L"builtin.todo",L"user.my-note"}) {
                auto item=std::find_if(packages.begin(),packages.end(),[&](auto const& p){return p.id==id;});
                expect(item!=packages.end(),L"Todo and note remain independent optional packages");
                import_plugin_file(item->manifest);set_plugin_enabled(plugins.size()-1,true);
            }
            runtime_step=9;
        } else if(runtime_step==9) {
            if(!ready(1)||!ready(2))return;
            plugins[1]->command(1);plugins[2]->command(1);runtime_step=10;
        } else if(runtime_step==10) {
            if(!plugin_inline_editing()||!InterlockedCompareExchange(&plugins[1]->channel->wire->editing,0,0)||!InterlockedCompareExchange(&plugins[2]->channel->wire->editing,0,0))return;
            expect(!(GetWindowLongPtrW(plugins[1]->panel(),GWL_EXSTYLE)&WS_EX_NOACTIVATE)&&!(GetWindowLongPtrW(plugins[2]->panel(),GWL_EXSTYLE)&WS_EX_NOACTIVATE),L"Independent todo and note processes enable keyboard input in their existing cards");
            expect(!IsWindowVisible(plugins[1]->panel())&&!IsWindowVisible(plugins[2]->panel()),L"Hidden runtime fixtures never activate or show cards while testing inline editing");
            plugins[1]->command(3);plugins[2]->command(3);runtime_step=11;
        } else if(runtime_step==11) {
            if(plugin_inline_editing())return;
            expect((GetWindowLongPtrW(plugins[1]->panel(),GWL_EXSTYLE)&WS_EX_NOACTIVATE)&&(GetWindowLongPtrW(plugins[2]->panel(),GWL_EXSTYLE)&WS_EX_NOACTIVATE),L"Ending inline editing releases the shared display hold and restores nonactivating cards");
            expect(fs::exists(plugin_data(plugins[2]->definition)/L"note.json")&&ready(0),L"Note editing persists data independently without disrupting another module");
            JsonObject result;result.SetNamedValue(L"assertions",JsonValue::CreateNumberValue(passed));log(L"runtime_test_pass",result);shutdown();
        }
    }
