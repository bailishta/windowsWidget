    void close_editor() {
        // Closed clears the App reference synchronously. Retain the window until Close returns.
        auto closing_window=editor;if(closing_window)closing_window.Close();if(editor==closing_window)editor=nullptr;
    }
    void show_feature_editor(StackPanel const& contents,int width,int height) {
        ScrollViewer scroll;scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);scroll.Content(contents);editor.Content(scroll);theme_titlebar(editor);
        editor.AppWindow().Resize({width,height});editor.Closed([this](auto const&,auto const&){editor=nullptr;});if(!options.self_test)editor.Activate();
    }
    void update_weather_credit() {
        if(weather_credit)weather_credit.Text(std::wstring(weather_config.custom?tr(L"自定义 API",L"Custom API"):L"Open-Meteo")+L" · "+tr(L"每 ",L"Every ")+std::to_wstring(weather_config.interval_minutes)+tr(L" 分钟自动更新",L" min automatically"));
    }
    void render_quick_entries() {
        if(!quick_list)return;quick_list.Children().Clear();
        for(auto const& entry:quick_entries){auto launch=button(entry.target.empty()?tr(L"未设置入口",L"Empty slot"):entry.name,[this,entry]{
            auto result=ShellExecuteW(hwnd,L"open",entry.target.c_str(),entry.arguments.empty()?nullptr:entry.arguments.c_str(),nullptr,SW_SHOWNORMAL);
            if(INT_PTR(result)<=32)MessageBoxW(hwnd,L"无法打开这个入口，请在控制中心检查目标地址。",L"常用入口",MB_OK|MB_ICONINFORMATION);
        });launch.IsEnabled(!entry.target.empty());quick_list.Children().Append(launch);}
    }
    void open_quick_editor() {
        if(editor){if(!options.self_test)editor.Activate();return;}clear_dismissal();editor=Window();editor.Title(tr(L"常用入口 · 设置",L"Quick access · settings"));
        StackPanel body;body.Padding({24,24,24,24});body.Spacing(14);body.Children().Append(text(tr(L"自定义三个入口",L"Customize three shortcuts"),24));
        body.Children().Append(text(tr(L"可打开程序、文件、文件夹、网址或 Windows 设置。目标留空可停用该位置。",L"Open apps, files, folders, URLs or Windows settings. Leave a target empty to disable its slot."),13));
        std::array<TextBox,3> names,targets,arguments;
        for(unsigned i=0;i<3;++i){StackPanel slot;slot.Spacing(8);slot.Children().Append(text(std::wstring(tr(L"位置 ",L"Slot "))+std::to_wstring(i+1),17));
            names[i].Header(box_value(tr(L"显示名称",L"Name")));names[i].MaxLength(40);names[i].Text(quick_entries[i].name);
            targets[i].Header(box_value(tr(L"目标",L"Target")));targets[i].MaxLength(2048);targets[i].PlaceholderText(L"C:\\Tools\\app.exe / https://example.com / ms-settings:");targets[i].Text(quick_entries[i].target);
            arguments[i].Header(box_value(tr(L"启动参数（可选）",L"Arguments (optional)")));arguments[i].MaxLength(2048);arguments[i].Text(quick_entries[i].arguments);
            slot.Children().Append(names[i]);slot.Children().Append(targets[i]);slot.Children().Append(arguments[i]);body.Children().Append(slot);
        }
        auto hint=text(L"",12);body.Children().Append(hint);body.Children().Append(button(tr(L"保存入口",L"Save shortcuts"),[this,names,targets,arguments,hint]{
            auto entries=quick_entries;for(unsigned i=0;i<3;++i){entries[i]={companion::trim(std::wstring(names[i].Text())),companion::trim(std::wstring(targets[i].Text())),std::wstring(arguments[i].Text())};
                if(!companion::valid_quick_entry(entries[i])){hint.Text(tr(L"请填写显示名称。目标不能包含换行或外层引号。",L"Provide a name. Targets cannot contain line breaks or enclosing quotes."));return;}}
            quick_entries=std::move(entries);save_state();render_quick_entries();close_editor();editor=nullptr;
        }));show_feature_editor(body,600,760);
    }
    void open_weather_settings() {
        if(editor){if(!options.self_test)editor.Activate();return;}clear_dismissal();editor=Window();editor.Title(tr(L"天气 · 城市与 API 设置",L"Weather · city and API settings"));
        StackPanel body;body.Padding({24,24,24,24});body.Spacing(14);body.Children().Append(text(tr(L"天气设置",L"Weather settings"),24));
        TextBox city;city.Header(box_value(tr(L"城市",L"City")));city.MaxLength(100);city.Text(weather_city);body.Children().Append(city);
        ComboBox weather_source;weather_source.Header(box_value(tr(L"天气来源",L"Weather source")));weather_source.HorizontalAlignment(HorizontalAlignment::Stretch);weather_source.Items().Append(box_value(L"Open-Meteo"));weather_source.Items().Append(box_value(tr(L"自定义 JSON API",L"Custom JSON API")));weather_source.SelectedIndex(weather_config.custom?1:0);body.Children().Append(weather_source);
        NumberBox interval;interval.Header(box_value(tr(L"自动更新间隔（分钟，5–1440）",L"Automatic update interval (minutes, 5–1440)")));interval.Minimum(5);interval.Maximum(1440);interval.SmallChange(5);interval.Value(weather_config.interval_minutes);interval.SpinButtonPlacementMode(NumberBoxSpinButtonPlacementMode::Inline);body.Children().Append(interval);
        StackPanel custom;custom.Spacing(12);custom.Visibility(weather_config.custom?Visibility::Visible:Visibility::Collapsed);
        TextBox url;url.Header(box_value(tr(L"HTTPS API 地址",L"HTTPS API URL")));url.MaxLength(4096);url.Text(weather_config.url);url.PlaceholderText(L"https://example.com/weather?city={city}");custom.Children().Append(url);
        custom.Children().Append(text(tr(L"使用 GET 请求读取 JSON。{city} 替换为城市；{lat}、{lon} 使用下方经纬度。",L"GET request returning JSON. {city} is the city; {lat} and {lon} use the coordinates below."),12));
        TextBox header;header.Header(box_value(tr(L"认证请求头名称（可选）",L"Authentication header (optional)")));header.Text(weather_config.header_name);header.MaxLength(80);custom.Children().Append(header);
        PasswordBox secret;secret.Header(box_value(tr(L"认证值（例如 Bearer xxx，可选）",L"Authentication value (for example Bearer xxx, optional)")));secret.MaxLength(2048);secret.Password(weather_config.header_value);custom.Children().Append(secret);
        NumberBox latitude,longitude;latitude.Header(box_value(tr(L"纬度（仅用于 {lat}）",L"Latitude (for {lat})")));latitude.Minimum(-90);latitude.Maximum(90);latitude.Value(weather_config.latitude);longitude.Header(box_value(tr(L"经度（仅用于 {lon}）",L"Longitude (for {lon})")));longitude.Minimum(-180);longitude.Maximum(180);longitude.Value(weather_config.longitude);
        Grid coordinates;coordinates.ColumnSpacing(12);for(int i=0;i<2;++i){ColumnDefinition col;col.Width({1,GridUnitType::Star});coordinates.ColumnDefinitions().Append(col);}coordinates.Children().Append(latitude);Grid::SetColumn(longitude,1);coordinates.Children().Append(longitude);custom.Children().Append(coordinates);
        custom.Children().Append(text(tr(L"JSON 字段路径：例如 now.temp、weather[0].description。只有温度必填，其余可留空。天气代码使用 WMO 编码；其他接口可只填写天气描述。",L"JSON paths such as now.temp or weather[0].description. Only temperature is required. Codes use WMO; other APIs can provide descriptions instead."),12));
        std::array<TextBox,8> paths;std::array<std::wstring,8> values{weather_config.temperature,weather_config.feels,weather_config.humidity,weather_config.wind,weather_config.code,weather_config.description,weather_config.time,weather_config.city};
        std::array<wchar_t const*,8> labels{tr(L"温度字段 *",L"Temperature *"),tr(L"体感温度字段",L"Feels-like temperature"),tr(L"湿度字段",L"Humidity"),tr(L"风速字段",L"Wind speed"),tr(L"天气代码字段（WMO）",L"Weather code (WMO)"),tr(L"天气描述字段",L"Description"),tr(L"观测时间字段",L"Observation time"),tr(L"城市名称字段",L"City name")};
        for(int row=0;row<4;++row){Grid line;line.ColumnSpacing(12);for(int col=0;col<2;++col){ColumnDefinition definition;definition.Width({1,GridUnitType::Star});line.ColumnDefinitions().Append(definition);auto i=row*2+col;paths[i].Header(box_value(labels[i]));paths[i].MaxLength(200);paths[i].Text(values[i]);Grid::SetColumn(paths[i],col);line.Children().Append(paths[i]);}custom.Children().Append(line);}
        ComboBox temperature_unit,wind_unit;temperature_unit.Header(box_value(tr(L"接口温度单位",L"API temperature unit")));temperature_unit.Items().Append(box_value(L"°C"));temperature_unit.Items().Append(box_value(L"°F"));temperature_unit.SelectedIndex(weather_config.fahrenheit?1:0);custom.Children().Append(temperature_unit);
        wind_unit.Header(box_value(tr(L"接口风速单位",L"API wind speed unit")));for(auto unit:{L"km/h",L"m/s",L"mph"})wind_unit.Items().Append(box_value(unit));wind_unit.SelectedIndex(weather_config.wind_unit);custom.Children().Append(wind_unit);body.Children().Append(custom);
        weather_source.SelectionChanged([custom](auto const& sender,auto const&){custom.Visibility(sender.template as<ComboBox>().SelectedIndex()==1?Visibility::Visible:Visibility::Collapsed);});
        auto hint=text(L"",12);body.Children().Append(hint);body.Children().Append(button(tr(L"保存天气设置",L"Save weather settings"),[this,city,weather_source,interval,url,header,secret,latitude,longitude,paths,temperature_unit,wind_unit,hint]{
            try {
                auto value=companion::trim(std::wstring(city.Text()));if(value.empty())throw std::runtime_error("请填写城市名称");auto minutes=interval.Value();if(!std::isfinite(minutes)||minutes<5||minutes>1440||minutes!=std::floor(minutes))throw std::runtime_error("更新间隔必须为 5–1440 之间的整数");
                auto config=weather_config;config.custom=weather_source.SelectedIndex()==1;config.interval_minutes=unsigned(minutes);config.url=companion::trim(std::wstring(url.Text()));config.header_name=companion::trim(std::wstring(header.Text()));config.header_value=secret.Password();config.latitude=latitude.Value();config.longitude=longitude.Value();
                config.temperature=companion::trim(std::wstring(paths[0].Text()));config.feels=companion::trim(std::wstring(paths[1].Text()));config.humidity=companion::trim(std::wstring(paths[2].Text()));config.wind=companion::trim(std::wstring(paths[3].Text()));config.code=companion::trim(std::wstring(paths[4].Text()));config.description=companion::trim(std::wstring(paths[5].Text()));config.time=companion::trim(std::wstring(paths[6].Text()));config.city=companion::trim(std::wstring(paths[7].Text()));config.fahrenheit=temperature_unit.SelectedIndex()==1;config.wind_unit=unsigned(wind_unit.SelectedIndex());companion::validate_weather_config(config);
                weather_config=std::move(config);weather_city=value;++weather_generation;weather_reading={};last_weather=0;save_state();weather_temp.Text(L"--°");weather_details.Text(L"");weather_symbol.Text(L"☁");weather_title.Text(value);weather_status.Text(tr(L"等待自动更新…",L"Waiting for automatic update…"));update_weather_credit();close_editor();editor=nullptr;
            }catch(std::exception const& error){hint.Text(to_hstring(error.what()));}catch(...){hint.Text(tr(L"无法保存天气设置，请检查输入。",L"Cannot save settings. Check your input."));}
        }));show_feature_editor(body,640,760);
    }
