    void test_startup_settings() {
        auto before=passed;
        using companion::manager_on_launch;using companion::reopen_existing_manager;
        expect(manager_on_launch(false,false,false,true),L"Manual launch opens the manager even when silent logon is selected");
        expect(!manager_on_launch(true,false,false,true),L"Silent logon keeps the manager hidden");
        expect(manager_on_launch(true,false,false,false),L"Non-silent logon opens the manager");
        expect(!manager_on_launch(false,true,false,false),L"Explicit background launch stays hidden");
        expect(!manager_on_launch(true,true,false,false),L"Background mode takes precedence over logon preferences");
        expect(!manager_on_launch(false,false,true,false),L"Runtime tests never show the manager");
        expect(reopen_existing_manager(false,false),L"A second manual launch reopens the existing manager");
        expect(!reopen_existing_manager(true,false)&&!reopen_existing_manager(false,true),L"Repeated logon and background launches do not reopen a hidden manager");

        auto rejects=[](auto action){try{action();return false;}catch(...){return true;}};
        auto executable=fs::path(L"C:\\Program Files\\随行组件\\WindowsWidget.exe");
        auto data=fs::path(L"C:\\Users\\tester\\启动 数据\\");
        auto command=companion::startup_command(executable,data,true);
        int count=0;auto args=CommandLineToArgvW(command.c_str(),&count);check_bool(args!=nullptr);
        bool roundtrip=count==5&&args[0]==executable.wstring()&&std::wstring_view(args[1])==L"--autostart"&&
            std::wstring_view(args[2])==L"--data-dir"&&args[3]==data.wstring()&&std::wstring_view(args[4])==L"--en";
        LocalFree(args);expect(roundtrip,L"Run command preserves spaces, Unicode, language and a trailing path separator");
        std::wstring quoted=L"embedded\"quote\\";
        auto escaped=L"test.exe "+companion::startup_argument(quoted);args=CommandLineToArgvW(escaped.c_str(),&count);check_bool(args!=nullptr);
        bool quote_ok=count==2&&args[1]==quoted;LocalFree(args);expect(quote_ok,L"Windows argument escaping round-trips quotes and trailing backslashes");
        expect(companion::startup_command(executable)==companion::startup_argument(executable.wstring())+L" --autostart",L"Default logon command contains no preview or custom data flags");
        expect(rejects([&]{companion::startup_command(L"WindowsWidget.exe");}),L"Startup rejects a relative executable path");
        expect(rejects([&]{companion::startup_command(fs::path(L"C:\\"+std::wstring(270,L'x')+L".exe"));}),L"Startup rejects commands exceeding the documented Run limit");

        auto directory=options.root/L"startup-tests";fs::create_directories(directory);
        companion::StartupPreferences preferences;preferences.load(directory);
        expect(preferences.silent&&!preferences.read_only&&!fs::exists(directory/L"startup.json"),L"Missing preferences default to silent without writing a file");
        preferences.set_silent(false);companion::StartupPreferences restored;restored.load(directory);
        expect(!restored.silent&&!restored.read_only,L"Silent startup disabled survives reload");
        restored.set_silent(true);preferences.load(directory);
        expect(preferences.silent,L"Silent startup enabled survives reload");
        auto document=JsonObject::Parse(to_hstring(companion::read_plugin_file(directory/L"startup.json")));
        field(document,L"future_field",L"preserved");companion::write_json_atomic(directory/L"startup.json",document);
        preferences.load(directory);preferences.set_silent(false);
        expect(JsonObject::Parse(to_hstring(companion::read_plugin_file(directory/L"startup.json"))).GetNamedString(L"future_field")==L"preserved",L"Preference updates preserve unrecognized fields");

        auto damaged=directory/L"damaged";fs::create_directories(damaged);
        {std::ofstream stream(damaged/L"startup.json",std::ios::binary);stream<<"{interrupted";}
        auto original=companion::read_plugin_file(damaged/L"startup.json");preferences.load(damaged);
        expect(preferences.read_only&&preferences.silent,L"Damaged preferences use a safe silent default");
        expect(rejects([&]{preferences.set_silent(false);})&&companion::read_plugin_file(damaged/L"startup.json")==original,L"Damaged preferences cannot be silently overwritten");
        auto future=directory/L"future";document=JsonObject();document.SetNamedValue(L"schema",JsonValue::CreateNumberValue(2));
        companion::write_json_atomic(future/L"startup.json",document);original=companion::read_plugin_file(future/L"startup.json");preferences.load(future);
        expect(preferences.read_only&&rejects([&]{preferences.set_silent(false);})&&companion::read_plugin_file(future/L"startup.json")==original,L"Unknown preference versions are preserved");
        auto blocked=directory/L"blocked";preferences.load(blocked);{std::ofstream stream(blocked);stream<<"parent is a file";}
        expect(rejects([&]{preferences.set_silent(false);})&&preferences.silent,L"Failed preference writes leave the previous in-memory setting intact");
        expect(!fs::exists(directory/L"startup.json.tmp"),L"Successful atomic saves consume the temporary file");

        // Exercise the real registry API in a unique volatile leaf only.
        // The user's Windows Run key is never written by automated tests.
        struct TemporaryRegistry {
            std::wstring path;HKEY key=nullptr;
            ~TemporaryRegistry(){if(key)RegCloseKey(key);RegDeleteKeyW(HKEY_CURRENT_USER,path.c_str());}
        } temporary{L"Software\\WindowsWidget.StartupTest."+companion::unique_token()};
        auto checked_registry=[](LSTATUS result){if(result!=ERROR_SUCCESS)throw hresult_error(HRESULT_FROM_WIN32(result),L"Isolated startup registry test");};
        checked_registry(RegCreateKeyExW(HKEY_CURRENT_USER,temporary.path.c_str(),0,nullptr,REG_OPTION_VOLATILE,
            KEY_QUERY_VALUE|KEY_SET_VALUE,nullptr,&temporary.key,nullptr));
        DWORD sentinel=42;checked_registry(RegSetValueExW(temporary.key,L"Unrelated",0,REG_DWORD,reinterpret_cast<BYTE const*>(&sentinel),sizeof(sentinel)));
        companion::StartupRegistration registration(HKEY_CURRENT_USER,temporary.path,REG_OPTION_VOLATILE);
        expect(!registration.enabled(),L"Absent startup registration reads as disabled");
        registration.set(true,command);expect(registration.enabled()&&registration.command()==command,L"Enabling startup stores the exact REG_SZ command");
        auto new_command=companion::startup_command(L"C:\\New Location\\WindowsWidget.exe");
        registration.set(true,new_command);expect(registration.command()==new_command,L"Re-enabling startup updates the executable path");
        expect(rejects([&]{registration.set(true,L"");})&&registration.command()==new_command,L"An empty startup command is rejected without changing the registration");
        expect(rejects([&]{registration.set(true,std::wstring(261,L'x'));})&&registration.command()==new_command,L"Rejected startup registration does not alter the existing command");
        registration.set(false);expect(!registration.enabled(),L"Disabling startup removes its own registry value");
        DWORD size=sizeof(sentinel);sentinel=0;
        expect(RegGetValueW(temporary.key,nullptr,L"Unrelated",RRF_RT_REG_DWORD,nullptr,&sentinel,&size)==ERROR_SUCCESS&&sentinel==42,L"Disabling startup preserves unrelated registry values");
        registration.set(false);expect(!registration.enabled(),L"Disabling absent startup registration is idempotent");
        // Drive the main-window switches through their actual event handlers using the isolated key.
        auto saved_registration=startup_registration;auto saved_preferences=startup_preferences;
        startup_registration=registration;startup_preferences.load(directory/L"main-controls");
        build_control_center();
        expect(auto_start_switch&&!auto_start_switch.IsOn()&&silent_start_switch.IsOn(),L"Main startup controls display disabled autostart and default silent logon");
        expect(!registration.enabled()&&!fs::exists(directory/L"main-controls"/L"startup.json"),L"Opening startup controls changes neither registration nor preferences");
        auto_start_switch.IsOn(true);
        expect(registration.enabled()&&auto_start_switch.IsOn(),L"Main autostart switch registers the executable through its event handler");
        auto registered_command=registration.command();build_control_center();
        expect(auto_start_switch.IsOn()&&registration.command()==registered_command,L"Rebuilding the main window preserves existing startup registration");
        silent_start_switch.IsOn(false);companion::StartupPreferences main_restored;main_restored.load(directory/L"main-controls");
        expect(!main_restored.silent&&!silent_start_switch.IsOn(),L"Main silent startup switch persists the selected preference");
        auto_start_switch.IsOn(false);
        expect(!registration.enabled()&&!auto_start_switch.IsOn(),L"Main autostart switch removes only the isolated registration");
        startup_preferences.load(damaged);refresh_startup_controls();
        expect(!silent_start_switch.IsEnabled()&&silent_start_switch.IsOn(),L"Main controls disable writes to damaged startup preferences");
        startup_registration=saved_registration;startup_preferences=saved_preferences;refresh_startup_controls();
        JsonObject result;result.SetNamedValue(L"assertions",JsonValue::CreateNumberValue(passed-before));log(L"startup_settings_test_pass",result);
    }

    void test_hidden_manager() {
        auto before=passed;build_control_center();
        auto manager_handle=manager_hwnd;auto content=manager_page;
        bool preview=session.preview;auto active=active_shell,dismissed=dismissed_shell;
        std::vector<DWORD> pids;for(auto const& p:plugins)pids.push_back(p->pid);
        hide_manager();
        expect(IsWindow(manager_handle)&&!IsWindowVisible(manager_handle),L"Hiding the manager retains its live hidden window");
        expect(manager_hwnd==manager_handle&&manager_page==content,L"Hiding retains the manager contents for reopening");
        expect(session.preview==preview&&active_shell==active&&dismissed_shell==dismissed,L"Hiding the manager does not change the Shell follow session");
        for(size_t i=0;i<plugins.size();++i)expect(plugins[i]->pid==pids[i]&&plugins[i]->panel()!=nullptr,L"Hiding the manager keeps each component process running");
        manager.Close();
        expect(IsWindow(manager_handle)&&!IsWindowVisible(manager_handle)&&!closing,L"The manager close button hides it without exiting the coordinator");
        JsonObject result;result.SetNamedValue(L"assertions",JsonValue::CreateNumberValue(passed-before));log(L"manager_hide_test_pass",result);
    }
