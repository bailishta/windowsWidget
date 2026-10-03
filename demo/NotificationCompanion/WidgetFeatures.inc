    Grid surface_root() {
        Grid g;g.Padding(Thickness{16,16,16,16});g.RequestedTheme(dark?ElementTheme::Dark:ElementTheme::Light);
        auto outline=Markup::XamlReader::Load(L"<Border xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' CornerRadius='8' BorderThickness='1' BorderBrush='{ThemeResource SurfaceStrokeColorDefaultBrush}' IsHitTestVisible='False' Margin='-16' />").as<Border>();
        g.Children().Append(outline);return g;
    }
    void apply_surface_theme(Grid const& g, bool acrylic, companion::AcrylicBackdrop* material) {
        g.RequestedTheme(dark?ElementTheme::Dark:ElementTheme::Light);
        g.Background(SolidColorBrush(acrylic?Windows::UI::Color{0,0,0,0}:
            dark?Windows::UI::Color{255,32,32,32}:Windows::UI::Color{255,243,243,243}));
        if(material)material->configuration.Theme(dark?SB::SystemBackdropTheme::Dark:SB::SystemBackdropTheme::Light);
    }
    Grid heading(std::wstring_view title,fs::path const& image={}) {
        Grid row;
        row.Background(SolidColorBrush(Windows::UI::Color{0,0,0,0}));
        row.PointerPressed([this,weak=make_weak(row)](auto const&,Input::PointerRoutedEventArgs const& args) {
            auto target=weak.get();if(!target || auto_arrange || motion.phase!=companion::MotionPhase::Visible)return;
            auto point=args.GetCurrentPoint(target);
            if(!point.Properties().IsLeftButtonPressed())return;
            HWND handle=hwnd;
            for(auto const& surface:widgets)if(surface.root.XamlRoot()==target.XamlRoot()){handle=surface.hwnd;break;}
            if(!target.CapturePointer(args.Pointer()))return;
            if(child_channel)InterlockedExchange(&child_channel->wire->dragging,1);
            dragging=true;layout_moving=false;dragging_handle=handle;
            // PointerPressed may be dequeued after later input. Use its cached point,
            // converted from XAML DIPs to physical pixels, for the initial cursor.
            auto origin=args.GetCurrentPoint(nullptr).Position();UINT dpi=GetDpiForWindow(handle);
            dragging_start={LONG(std::lround(origin.X*dpi/96.0)),LONG(std::lround(origin.Y*dpi/96.0))};ClientToScreen(handle,&dragging_start);
            GetWindowRect(handle,&dragging_origin);args.Handled(true);log(L"widget_drag_started");
        });
        row.PointerMoved([this](auto const&,Input::PointerRoutedEventArgs const& args) {
            if(!dragging || !dragging_handle)return;
            POINT cursor{};GetCursorPos(&cursor);RECT requested=dragging_origin;
            OffsetRect(&requested,cursor.x-dragging_start.x,cursor.y-dragging_start.y);
            auto i=surface_index(dragging_handle);
            if(i>=widget_positions.size())return;
            requested=companion::constrain_widget(requested,widget_positions[i],layout_work,layout_reserved,MulDiv(8,layout_dpi,96));
            SetWindowPos(dragging_handle,nullptr,requested.left,requested.top,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
            if(child_channel)child_channel->wire->drag_position=requested;
            args.Handled(true);
        });
        row.PointerReleased([this,weak=make_weak(row)](auto const&,Input::PointerRoutedEventArgs const& args) {
            if(!dragging_handle)return;
            finish_pointer_drag();if(auto target=weak.get())target.ReleasePointerCapture(args.Pointer());args.Handled(true);
        });
        row.PointerCanceled([this](auto const&,auto const&){finish_pointer_drag();});
        row.PointerCaptureLost([this](auto const&,auto const&){finish_pointer_drag();});
        row.MinHeight(28);
        row.Children().Append(icon_label(title,14,24,image));
        return row;
    }
    void finish_pointer_drag() {
        if(!dragging_handle)return;
        auto i=surface_index(dragging_handle);RECT actual{};GetWindowRect(dragging_handle,&actual);
        dragging_handle=nullptr;dragging=false;
        if(child_channel){child_channel->wire->drag_position=actual;MemoryBarrier();InterlockedIncrement(&child_channel->wire->drag_revision);InterlockedExchange(&child_channel->wire->dragging,0);return;}
        if(i<widget_positions.size()) {
            remember_position(i,actual);widget_positions[i]=actual;
            if(i==0){last_position=actual;animated_position=actual;}
            else {widgets[i-1].target=actual;widgets[i-1].animated=actual;}
            save_state();log(L"widget_drag_saved");poll();
        }
    }
    void show_widget_menu(FrameworkElement const& anchor) {
        if(child_channel){PostMessageW(child_channel->wire->parent,WM_APP+100,0,0);return;}
        MenuFlyout menu;
        ToggleMenuFlyoutItem automatic;automatic.Text(tr(L"自动排列 · 右下角优先",L"Auto arrange · bottom right"));automatic.IsChecked(auto_arrange);
        automatic.Click([this](auto const&,auto const&){clear_dismissal();toggle_arrangement();});menu.Items().Append(automatic);
        MenuFlyoutItem hint;hint.Text(tr(L"关闭后拖动卡片标题",L"Turn off to drag card headings"));hint.IsEnabled(false);menu.Items().Append(hint);
        const wchar_t* names[]={tr(L"天气",L"Weather"),tr(L"待办事项",L"To do"),tr(L"常用入口",L"Quick access"),tr(L"系统状态",L"System status")};
        menu.Items().Append(MenuFlyoutSeparator());
        for(size_t i=0;i<widget_enabled.size();++i) {
            ToggleMenuFlyoutItem item;item.Text(names[i]);item.IsChecked(widget_enabled[i]);
            item.Click([this,i](auto const&,auto const&){
                if(!widget_enabled[i] || std::count(widget_enabled.begin(),widget_enabled.end(),true)>1)widget_enabled[i]=!widget_enabled[i];
                save_state();clear_dismissal();poll();
            });menu.Items().Append(item);
        }
        menu.Items().Append(MenuFlyoutSeparator());
        auto action=[&](const wchar_t* label,std::function<void()> execute){MenuFlyoutItem item;item.Text(label);item.Click([execute](auto const&,auto const&){execute();});menu.Items().Append(item);};
        action(tr(L"重置自由位置",L"Reset free positions"),[this]{free_positions=std::vector<FreePosition>(4);save_state();clear_dismissal();poll();});
        action(tr(L"切换深浅色",L"Toggle theme"),[this]{theme_override=true;dark=!dark;build_ui();});
        action(tr(L"收起组件",L"Hide widgets"),[this]{dismiss_following();});
        menu.ShowAt(anchor);
    }
    void load_state() {
        try {
            std::ifstream input(options.root/L"widgets.json",std::ios::binary);
            if(!input)return;
            std::string contents((std::istreambuf_iterator<char>(input)),{});
            auto state=JsonObject::Parse(to_hstring(contents));
            auto_arrange=state.GetNamedBoolean(L"auto_arrange",true);
            weather_city=state.GetNamedString(L"weather_city",L"Hong Kong");
            hide_completed_tasks=state.GetNamedBoolean(L"hide_completed_tasks",false);
            try {if(state.HasKey(L"weather_api"))weather_config=companion::weather_config_from_json(state.GetNamedObject(L"weather_api"));}catch(...){weather_config={};log(L"weather_config_load_failed");}
            try {auto entries=state.GetNamedArray(L"quick_entries");if(entries.Size()==3)for(unsigned i=0;i<3;++i){auto j=entries.GetObjectAt(i);companion::QuickEntry entry{std::wstring(j.GetNamedString(L"name")),std::wstring(j.GetNamedString(L"target")),std::wstring(j.GetNamedString(L"arguments",L""))};if(companion::valid_quick_entry(entry))quick_entries[i]=entry;}}catch(...){}
            auto task_values=state.GetNamedArray(L"tasks",Windows::Data::Json::JsonArray());
            for(auto value:task_values) {
                auto task=value.GetObject();auto title=task.GetNamedString(L"title");
                if(!title.empty() && title.size()<=200 && tasks.size()<100)tasks.push_back({std::wstring(title),task.GetNamedBoolean(L"done",false)});
            }
            auto positions=state.GetNamedArray(L"positions",Windows::Data::Json::JsonArray());
            for(unsigned i=0;i<std::min(positions.Size(),unsigned(free_positions.size()));++i) {
                auto position=positions.GetObjectAt(i);
                auto x=position.GetNamedNumber(L"x",0),y=position.GetNamedNumber(L"y",0);
                if(std::isfinite(x)&&std::isfinite(y)) free_positions[i]={std::clamp(x,0.0,1.0),std::clamp(y,0.0,1.0),position.GetNamedBoolean(L"saved",false)};
                widget_enabled[i]=position.GetNamedBoolean(L"enabled",true);
            }
            if(!widget_enabled.empty()&&std::none_of(widget_enabled.begin(),widget_enabled.end(),[](bool v){return v;}))widget_enabled[0]=true;
        }catch(...){tasks.clear();log(L"widget_state_load_failed");}
    }
    void save_state() {
        if(coordinator()){save_manager();return;}
        try {
            JsonObject state;state.SetNamedValue(L"auto_arrange",JsonValue::CreateBooleanValue(auto_arrange));field(state,L"weather_city",weather_city);
            state.SetNamedValue(L"weather_api",companion::weather_config_json(weather_config));
            Windows::Data::Json::JsonArray entries;for(auto const& entry:quick_entries){JsonObject j;field(j,L"name",entry.name);field(j,L"target",entry.target);field(j,L"arguments",entry.arguments);entries.Append(j);}state.SetNamedValue(L"quick_entries",entries);
            state.SetNamedValue(L"hide_completed_tasks",JsonValue::CreateBooleanValue(hide_completed_tasks));
            Windows::Data::Json::JsonArray values,positions;
            for(auto const& task:tasks) {JsonObject t;field(t,L"title",task.title);t.SetNamedValue(L"done",JsonValue::CreateBooleanValue(task.done));values.Append(t);}
            for(size_t i=0;i<free_positions.size();++i) {
                auto p=free_positions[i];JsonObject position;
                position.SetNamedValue(L"x",JsonValue::CreateNumberValue(p.x));position.SetNamedValue(L"y",JsonValue::CreateNumberValue(p.y));
                position.SetNamedValue(L"saved",JsonValue::CreateBooleanValue(p.saved));position.SetNamedValue(L"enabled",JsonValue::CreateBooleanValue(widget_enabled[i]));positions.Append(position);
            }
            state.SetNamedValue(L"tasks",values);state.SetNamedValue(L"positions",positions);
            fs::create_directories(options.root);auto temp=options.root/L"widgets.json.tmp",target=options.root/L"widgets.json";
            {std::ofstream output(temp,std::ios::binary|std::ios::trunc);output<<to_string(state.Stringify());output.flush();if(!output)throw std::runtime_error("save");}
            check_bool(MoveFileExW(temp.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH));
        }catch(...){log(L"widget_state_save_failed");}
    }
    void remember_position(size_t i,RECT position) {
        if(i>=free_positions.size())return;
        auto width=position.right-position.left,height=position.bottom-position.top;
        free_positions[i]={std::clamp(double(position.left-layout_work.left)/std::max(1L,layout_work.right-layout_work.left-width),0.0,1.0),
            std::clamp(double(position.top-layout_work.top)/std::max(1L,layout_work.bottom-layout_work.top-height),0.0,1.0),true};
    }
    size_t surface_index(HWND target) const {
        if(child_channel&&target==hwnd)return 0;
        if(target==surface_handle(0))return 0;
        for(size_t i=0;i<widgets.size();++i)if(widgets[i].hwnd==target)return i+1;
        return free_positions.size();
    }
    void toggle_arrangement() {
        if(auto_arrange)for(size_t i=0;i<widget_positions.size();++i)if(!IsRectEmpty(&widget_positions[i]))remember_position(i,widget_positions[i]);
        auto_arrange=!auto_arrange;save_state();log(auto_arrange?L"auto_arrange_enabled":L"free_arrange_enabled");poll();
    }
    void restore_card_region(HWND handle) {
        if(!handle || !IsWindow(handle))return;
        RECT bounds{};check_bool(GetWindowRect(handle,&bounds));
        UINT dpi=GetDpiForWindow(handle);if(!dpi)dpi=96;
        auto region=CreateRoundRectRgn(0,0,bounds.right-bounds.left+1,bounds.bottom-bounds.top+1,MulDiv(16,dpi,96),MulDiv(16,dpi,96));
        if(!region)throw hresult_error(E_OUTOFMEMORY);
        if(!SetWindowRgn(handle,region,FALSE)){DeleteObject(region);throw hresult_error(E_FAIL);}
    }
    void configure_card_shape(HWND handle) {
        // Adjacent independent Acrylic windows otherwise receive each other's
        // DWM shadows. Use a persistent card region instead of DWM decoration.
        DWMNCRENDERINGPOLICY policy=DWMNCRP_DISABLED;
        check_hresult(DwmSetWindowAttribute(handle,DWMWA_NCRENDERING_POLICY,&policy,sizeof(policy)));
        DWM_WINDOW_CORNER_PREFERENCE corners=DWMWCP_DONOTROUND;
        check_hresult(DwmSetWindowAttribute(handle,DWMWA_WINDOW_CORNER_PREFERENCE,&corners,sizeof(corners)));
        COLORREF border=DWMWA_COLOR_NONE;DwmSetWindowAttribute(handle,DWMWA_BORDER_COLOR,&border,sizeof(border));
        restore_card_region(handle);
    }
    bool resize_surface(HWND handle,RECT target) {
        if(!handle || IsRectEmpty(&target))return false;
        RECT actual{};check_bool(GetWindowRect(handle,&actual));
        int width=target.right-target.left,height=target.bottom-target.top;
        if(actual.right-actual.left==width && actual.bottom-actual.top==height)return false;
        check_bool(SetWindowPos(handle,nullptr,0,0,width,height,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE));
        restore_card_region(handle);
        return true;
    }
    bool layout_widgets(RECT work,RECT reserved,UINT dpi,RECT& first) {
        if(dragging)return !IsRectEmpty(&first);
        layout_work=work;layout_reserved=reserved;layout_dpi=dpi;
        int gap=MulDiv(8,dpi,96),width=MulDiv(320,dpi,96);
        std::vector<int> heights;std::vector<size_t> indices;
        for(size_t i=0;i<widget_heights.size();++i)if(widget_enabled[i]){heights.push_back(MulDiv(widget_heights[i],dpi,96));indices.push_back(i);}
        auto packed=companion::arrange_widgets(work,reserved,width,heights,gap);
        std::vector<RECT> next(widget_heights.size());
        for(size_t k=0;k<indices.size();++k) {
            auto i=indices[k];auto fallback=packed[k];
            if(!auto_arrange && IsRectEmpty(&fallback))fallback=companion::arrange_widgets(work,reserved,width,{heights[k]},gap)[0];
            next[i]=fallback;
            if(!auto_arrange && free_positions[i].saved) {
                int x=int(work.left+std::lround(free_positions[i].x*(work.right-work.left-width)));
                int y=int(work.top+std::lround(free_positions[i].y*(work.bottom-work.top-heights[k])));
                next[i]=companion::constrain_widget({x,y,x+width,y+heights[k]},fallback,work,reserved,gap);
            }
        }
        bool changed=next.size()!=widget_positions.size();
        if(!changed)for(size_t i=0;i<next.size();++i)changed|=!EqualRect(&next[i],&widget_positions[i]);
        if(changed && shown && motion.phase==companion::MotionPhase::Visible && ui_settings.AnimationsEnabled()) {
            layout_from=widget_positions;
            for(size_t i=0;i<layout_from.size();++i) {
                HWND handle=surface_handle(i);
                if(IsWindowVisible(handle))GetWindowRect(handle,&layout_from[i]);
            }
            layout_started=companion::motion_now_ms();layout_moving=true;motion_frames.active(true);
        }
        widget_positions=next;first=next.empty()?RECT{}:next[0];
        // Also check an unchanged slot: a restarted plugin has a new HWND and
        // starts at 96 DPI. Resize only when its actual pixel dimensions differ.
        if(resize_surface(surface_handle(0),first))animated_position={};
        for(size_t i=0;i<widgets.size();++i) {
            auto& surface=widgets[i];auto target=next[i+1];
            if(!EqualRect(&surface.target,&target)) {
                surface.target=target;surface.animated={};
            }
            if(IsRectEmpty(&target)){if(IsWindowVisible(surface.hwnd))ShowWindow(surface.hwnd,SW_HIDE);continue;}
            if(resize_surface(surface.hwnd,target))surface.animated={};
        }
        if(changed && motion.wants_visible()) {animated_position={};motion_frame();}
        return std::any_of(next.begin(),next.end(),[](RECT r){return !IsRectEmpty(&r);});
    }
    void focus_inline_input() {
        if(!inline_editing||options.self_test||!inline_edit_handle)return;
        if(GetTickCount64()>inline_focus_deadline){finish_inline_edit();log(L"inline_focus_unavailable");return;}
        if(!IsWindowVisible(inline_edit_handle))return;
        if(motion.phase!=companion::MotionPhase::Visible)return;
        if(!SetForegroundWindow(inline_edit_handle))return;
        auto input=note_editor?note_editor:task_input;
        if(input&&input.Focus(FocusState::Programmatic)){inline_focus_pending=false;log(L"inline_edit_focused");}
    }
    void begin_inline_edit() {
        if(!task_input&&!note_editor)return;
        inline_editing=true;inline_focus_pending=true;inline_focus_deadline=GetTickCount64()+3000;
        inline_edit_handle=options.self_test?surface_handle(1):hwnd;
        if(inline_edit_handle)SetWindowLongPtrW(inline_edit_handle,GWL_EXSTYLE,GetWindowLongPtrW(inline_edit_handle,GWL_EXSTYLE)&~WS_EX_NOACTIVATE);
        if(child_channel)InterlockedExchange(&child_channel->wire->editing,1);
        if(note_editor) {
            note_editor.IsReadOnly(false);note_editor.IsTabStop(true);
            if(note_edit_button){note_edit_button.Content(box_value(tr(L"完成编辑 · 自动保存",L"Done · auto saved")));note_edit_button.IsTabStop(true);}
        } else {
            task_input_row.Visibility(Visibility::Visible);task_input.IsTabStop(true);task_submit_button.IsTabStop(true);
            task_edit_button.Content(box_value(tr(L"完成编辑",L"Done")));task_edit_button.IsTabStop(true);task_visibility_button.IsTabStop(true);resize_task_scroll();render_tasks();
        }
        log(L"inline_edit_started");
        focus_inline_input();
    }
    void finish_inline_edit() {
        if(!inline_editing)return;
        // Keep an unsubmitted task draft in its existing input when focus leaves the card.
        if(note_editor){save_note();note_editor.IsReadOnly(true);note_editor.IsTabStop(false);if(note_edit_button){note_edit_button.Content(box_value(tr(L"编辑便签",L"Edit note")));note_edit_button.IsTabStop(false);}}
        if(inline_edit_handle&&!options.inspect)SetWindowLongPtrW(inline_edit_handle,GWL_EXSTYLE,GetWindowLongPtrW(inline_edit_handle,GWL_EXSTYLE)|WS_EX_NOACTIVATE);
        inline_editing=false;inline_focus_pending=false;
        if(task_input_row){task_input_row.Visibility(Visibility::Collapsed);task_hint.Visibility(Visibility::Collapsed);task_input.IsTabStop(false);task_submit_button.IsTabStop(false);task_edit_button.IsTabStop(false);task_visibility_button.IsTabStop(false);task_edit_button.Content(box_value(tr(L"＋ 添加 / 编辑",L"＋ Add / edit")));resize_task_scroll();render_tasks();}
        if(child_channel)InterlockedExchange(&child_channel->wire->editing,0);
        log(L"inline_edit_finished");
    }
    void resize_task_scroll() {
        if(task_scroll)task_scroll.Height(std::max(40.0,task_preview_height-(inline_editing?40:0)-(task_hint&&task_hint.Visibility()==Visibility::Visible?24:0)));
    }
    void task_error(wchar_t const* message) {
        task_hint.Text(message);task_hint.Visibility(Visibility::Visible);resize_task_scroll();
    }
    void update_task_submit_label() {
        task_submit_button.Content(box_value(tr(task_edit_index<0?L"添加":L"保存",task_edit_index<0?L"Add":L"Save")));
    }
    bool submit_inline_task() {
        std::wstring value(task_input.Text());auto start=value.find_first_not_of(L" \t\r\n"),end=value.find_last_not_of(L" \t\r\n");
        if(start==value.npos){task_error(tr(L"请先输入待办内容。",L"Enter a task first."));return false;}
        value=value.substr(start,end-start+1);
        if(task_edit_index>=0&&size_t(task_edit_index)<tasks.size())tasks[size_t(task_edit_index)].title=value;
        else {if(tasks.size()>=100){task_error(tr(L"最多保存 100 项待办。",L"Up to 100 tasks."));return false;}tasks.push_back({value,false});}
        task_edit_index=-1;task_input.Text(L"");update_task_submit_label();task_hint.Visibility(Visibility::Collapsed);
        save_state();render_tasks();resize_task_scroll();task_input.Focus(FocusState::Programmatic);return true;
    }
    void edit_inline_task(size_t index) {
        if(index>=tasks.size())return;
        task_edit_index=int(index);task_input.Text(tasks[index].title);update_task_submit_label();
        task_hint.Visibility(Visibility::Collapsed);resize_task_scroll();task_input.Focus(FocusState::Programmatic);task_input.SelectAll();
    }
    void remove_inline_task(size_t index) {
        if(index>=tasks.size())return;
        tasks.erase(tasks.begin()+index);
        if(task_edit_index==int(index))task_edit_index=-1;
        else if(task_edit_index>int(index))--task_edit_index;
        update_task_submit_label();save_state();render_tasks();
    }
    void append_task_controls(StackPanel const& body,double preview_height) {
        task_preview_height=preview_height;
        task_input_row=Grid();task_input_row.ColumnSpacing(6);task_input_row.Visibility(Visibility::Collapsed);
        ColumnDefinition entry;entry.Width({1,GridUnitType::Star});task_input_row.ColumnDefinitions().Append(entry);
        ColumnDefinition submit;submit.Width({1,GridUnitType::Auto});task_input_row.ColumnDefinitions().Append(submit);
        task_input=TextBox();task_input.MaxLength(200);task_input.PlaceholderText(tr(L"输入待办内容",L"Enter a task"));task_input.IsTabStop(false);
        task_input.KeyDown([this](auto const&,Input::KeyRoutedEventArgs const& args){if(args.Key()==Windows::System::VirtualKey::Enter){submit_inline_task();args.Handled(true);}else if(args.Key()==Windows::System::VirtualKey::Escape){finish_inline_edit();args.Handled(true);}});
        task_input_row.Children().Append(task_input);task_submit_button=button(tr(L"添加",L"Add"),[this]{submit_inline_task();});Grid::SetColumn(task_submit_button,1);task_input_row.Children().Append(task_submit_button);body.Children().Append(task_input_row);
        task_hint=text(L"",12);task_hint.Visibility(Visibility::Collapsed);body.Children().Append(task_hint);
        task_list=StackPanel();task_list.Spacing(2);task_list.Margin({0,0,12,0});task_scroll=ScrollViewer();task_scroll.Height(preview_height);task_scroll.Content(task_list);
        task_scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);task_scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);body.Children().Append(task_scroll);
        body.Children().Append(task_actions());render_tasks();
    }
    Grid task_actions() {
        Grid actions;actions.ColumnSpacing(8);
        for(int i=0;i<2;++i){ColumnDefinition column;column.Width({1,GridUnitType::Star});actions.ColumnDefinitions().Append(column);}
        task_edit_button=button(tr(L"＋ 添加 / 编辑",L"＋ Add / edit"),[this]{
            if(!inline_editing){begin_inline_edit();return;}
            if(std::wstring(task_input.Text()).find_first_not_of(L" \t\r\n")!=std::wstring::npos&&!submit_inline_task())return;
            finish_inline_edit();
        });actions.Children().Append(task_edit_button);
        task_visibility_button=button(L"",[this]{set_hide_completed_tasks(!hide_completed_tasks);});
        Grid::SetColumn(task_visibility_button,1);actions.Children().Append(task_visibility_button);return actions;
    }
    void set_hide_completed_tasks(bool hidden) {
        hide_completed_tasks=hidden;save_state();update_task_visibility();
    }
    void update_task_visibility() {
        if(task_list) {
            size_t visible=0;
            for(size_t i=0;i<tasks.size();++i) {
                bool show=!hide_completed_tasks||!tasks[i].done;if(show)++visible;
                if(i<task_list.Children().Size())task_list.Children().GetAt(unsigned(i)).Visibility(show?Visibility::Visible:Visibility::Collapsed);
            }
            if(task_empty_message) {
                task_empty_message.Text(tasks.empty()?tr(L"把要做的事记在这里。",L"A place for things you want to do."):tr(L"已全部完成。",L"All tasks completed."));
                task_empty_message.Visibility(visible?Visibility::Collapsed:Visibility::Visible);
            }
        }
        if(task_visibility_button)task_visibility_button.Content(box_value(tr(hide_completed_tasks?L"显示已完成":L"隐藏已完成",hide_completed_tasks?L"Show completed":L"Hide completed")));
    }
    void render_tasks() {
        if(!task_list)return;
        task_list.Children().Clear();
        for(size_t i=0;i<tasks.size();++i) {
            auto const& task=tasks[i];
            CheckBox check;check.Content(box_value(task.title));check.IsChecked(task.done);check.IsTabStop(false);
            check.HorizontalAlignment(HorizontalAlignment::Stretch);
            check.AddHandler(UIElement::PointerPressedEvent(),box_value(Input::PointerEventHandler([this](auto const&,auto const&){begin_probe();})),true);
            auto change=[this,i](auto const& sender,auto const&) {
                if(i>=tasks.size())return;
                tasks[i].done=sender.template as<CheckBox>().IsChecked().Value();save_state();update_task_count();update_task_visibility();
            };
            check.Checked(change);check.Unchecked(change);
            Grid row;row.ColumnSpacing(4);ColumnDefinition title;title.Width({1,GridUnitType::Star});row.ColumnDefinitions().Append(title);row.Children().Append(check);
            if(inline_editing) {
                for(int column=1;column<=2;++column) {
                    ColumnDefinition icon_column;icon_column.Width({28,GridUnitType::Pixel});row.ColumnDefinitions().Append(icon_column);
                    auto action=column==1?button(L"\xE70F",[this,i]{edit_inline_task(i);}):button(L"\xE74D",[this,i]{remove_inline_task(i);});
                    auto label=tr(column==1?L"编辑待办":L"删除待办",column==1?L"Edit task":L"Delete task");
                    action.FontFamily(FontFamily(L"Segoe Fluent Icons"));action.Padding({0,0,0,0});action.MinWidth(0);action.Width(28);action.Height(28);action.IsTabStop(true);
                    Automation::AutomationProperties::SetName(action,label);ToolTipService::SetToolTip(action,box_value(label));Grid::SetColumn(action,column);row.Children().Append(action);
                }
            }
            task_list.Children().Append(row);
        }
        task_empty_message=text(L"",13);task_empty_message.Opacity(.65);task_list.Children().Append(task_empty_message);
        update_task_count();update_task_visibility();
    }
    void update_task_count() {
        unsigned count=0;for(auto const& task:tasks)if(!task.done)++count;
        if(task_count)task_count.Text(options.english?std::to_wstring(count)+L" remaining":L"还有 "+std::to_wstring(count)+L" 项待办");
    }
    void refresh_weather() {
        if(options.self_test || weather.busy())return;
        weather_status.Text(tr(L"正在更新…",L"Updating…"));weather.refresh(weather_city,weather_config,weather_generation);last_weather=GetTickCount64();
    }
    void poll_features() {
        auto now=GetTickCount64();
        if(weather_status) {
            companion::WeatherReading reading;std::wstring error;
            if(weather.consume(reading,error)) {
                if(reading.requested_city!=weather_city||reading.generation!=weather_generation){last_weather=0;}
                else if(reading.valid) {
                    weather_reading=reading;weather_title.Text(reading.city);
                    weather_temp.Text(std::to_wstring(int(std::lround(reading.temperature)))+L"°C");
                    std::wstring detail;auto append=[&](std::wstring const& value){if(!detail.empty())detail+=L" · ";detail+=value;};auto rounded=[](double v){return std::to_wstring(int(std::lround(v)));};
                    if(reading.has_feels)append(std::wstring(tr(L"体感 ",L"Feels "))+rounded(reading.feels)+L"°");if(reading.has_humidity)append(std::wstring(tr(L"湿度 ",L"Humidity "))+rounded(reading.humidity)+L"%");if(reading.has_wind){if(!detail.empty())detail+=L"\n";detail+=std::wstring(tr(L"风速 ",L"Wind "))+rounded(reading.wind)+L" km/h";}weather_details.Text(detail);
                    auto code=reading.code;weather_symbol.Text(code<0?L"☁":code<=1?L"☀":code<=3?L"☁":code>=95?L"ϟ":code>=71&&code<=77?L"❄":L"☂");
                    std::wstring description=!reading.description.empty()?reading.description:code<0?tr(L"实时天气",L"Current weather"):code<=1?tr(L"晴",L"Clear"):code<=3?tr(L"多云",L"Cloudy"):code==45||code==48?tr(L"雾",L"Fog"):code>=95?tr(L"雷暴",L"Storm"):code>=71&&code<=77?tr(L"雪",L"Snow"):tr(L"雨",L"Rain");
                    weather_status.Text(description+(reading.measured.empty()?L"":L" · "+reading.measured.substr(reading.measured.size()>5?reading.measured.size()-5:0)));
                } else weather_status.Text(weather_reading.valid?tr(L"更新失败 · 保留上次数据",L"Update failed · previous reading"):tr(L"天气暂不可用，请检查城市、API 或网络",L"Weather unavailable. Check city, API or connection."));
            }
            if(!options.self_test&&companion::weather_due(now,last_weather,weather_config))refresh_weather();
        }
        if(cpu_plot && now-last_metrics>=1000) {
            last_metrics=now;MEMORYSTATUSEX memory{sizeof(memory)};bool memory_ok=GlobalMemoryStatusEx(&memory)!=FALSE;
            FILETIME idle{},kernel{},user{};
            auto ticks=[](FILETIME f){return (ULONGLONG(f.dwHighDateTime)<<32)|f.dwLowDateTime;};
            double cpu=0;bool cpu_ok=false;
            if(GetSystemTimes(&idle,&kernel,&user)) {
                auto next_idle=ticks(idle),next_kernel=ticks(kernel),next_user=ticks(user);
                auto total=(next_kernel-previous_kernel)+(next_user-previous_user);
                if(previous_kernel && total){cpu=std::clamp(100.0*(1.0-double(next_idle-previous_idle)/total),0.0,100.0);cpu_ok=true;}
                previous_idle=next_idle;previous_kernel=next_kernel;previous_user=next_user;
            }
            if(cpu_ok){cpu_plot->sample(cpu);cpu_caption.Text(L"CPU  "+std::to_wstring(int(std::lround(cpu)))+L"%");}
            if(memory_ok){memory_plot->sample(memory.dwMemoryLoad);memory_caption.Text(std::wstring(tr(L"内存  ",L"Memory  "))+std::to_wstring(memory.dwMemoryLoad)+L"%");}
        }
    }
    void build_widget_surfaces() {
        for(size_t i=1;i<widget_heights.size();++i) {
            WidgetSurface surface;surface.window=Window();surface.window.Title(i==1?L"WindowsWidget · To do":i==2?L"WindowsWidget · Quick access":L"WindowsWidget · System status");
            check_hresult(surface.window.as<IWindowNative>()->get_WindowHandle(&surface.hwnd));
            auto presenter=surface.window.AppWindow().Presenter().as<Microsoft::UI::Windowing::OverlappedPresenter>();
            presenter.SetBorderAndTitleBar(false,false);presenter.IsResizable(false);presenter.IsMaximizable(false);presenter.IsMinimizable(false);
            SetWindowLongPtrW(surface.hwnd,GWL_STYLE,(GetWindowLongPtrW(surface.hwnd,GWL_STYLE)|WS_POPUP)&~(WS_CAPTION|WS_THICKFRAME|WS_BORDER|WS_DLGFRAME));
            SetWindowLongPtrW(surface.hwnd,GWL_EXSTYLE,(GetWindowLongPtrW(surface.hwnd,GWL_EXSTYLE)|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW)&~WS_EX_APPWINDOW);
            if(options.inspect)SetWindowLongPtrW(surface.hwnd,GWL_EXSTYLE,(GetWindowLongPtrW(surface.hwnd,GWL_EXSTYLE)|WS_EX_APPWINDOW)&~(WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW));
            SetWindowSubclass(surface.hwnd,panel_proc,1,reinterpret_cast<DWORD_PTR>(this));
            SetWindowPos(surface.hwnd,nullptr,0,0,320,widget_heights[i],SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
            configure_card_shape(surface.hwnd);
            surface.root=surface_root();auto body=plugin_body(i);
            surface.root.Children().Append(body);surface.window.Content(surface.root);
            surface.backdrop=make_self<companion::AcrylicBackdrop>();bool acrylic=false;
            try {surface.window.DispatcherQueue().EnsureSystemDispatcherQueue();if(surface.backdrop->supported){surface.window.SystemBackdrop(surface.backdrop.as<Microsoft::UI::Xaml::Media::SystemBackdrop>());acrylic=true;}}
            catch(...){surface.window.SystemBackdrop(nullptr);log(L"widget_backdrop_unavailable");}
            apply_surface_theme(surface.root,acrylic,surface.backdrop.get());widgets.push_back(std::move(surface));
        }
    }
