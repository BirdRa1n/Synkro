#include "platform/windows/TaskScheduler.h"

#include "platform/windows/Win32.h"

#include <objbase.h>
#include <oleauto.h>
#include <taskschd.h>

#include <cstdio>

namespace synkro::win {

namespace {

std::wstring xmlEscape(const std::wstring& s)
{
    std::wstring o;
    for (wchar_t c : s) {
        switch (c) {
        case L'&': o += L"&amp;"; break;
        case L'<': o += L"&lt;"; break;
        case L'>': o += L"&gt;"; break;
        case L'"': o += L"&quot;"; break;
        default: o += c;
        }
    }
    return o;
}

std::wstring startBoundary(int minutes)
{
    wchar_t buf[32];
    std::swprintf(buf, 32, L"2026-01-01T%02d:%02d:00", minutes / 60, minutes % 60);
    return buf;
}

std::wstring daysOfWeek(const std::vector<int>& days)
{
    static const wchar_t* names[] = {L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday", L"Sunday"};
    std::wstring o = L"<DaysOfWeek>";
    for (int d : days)
        o += L"<" + std::wstring(names[d - 1]) + L"/>";
    return o + L"</DaysOfWeek>";
}

std::wstring triggerXml(const config::ScheduleTrigger& t)
{
    using config::TriggerKind;
    switch (t.kind) {
    case TriggerKind::Daily:
        return L"<CalendarTrigger><StartBoundary>" + startBoundary(t.atMinutes)
               + L"</StartBoundary><ScheduleByDay><DaysInterval>1</DaysInterval></ScheduleByDay></CalendarTrigger>";
    case TriggerKind::Weekly:
        return L"<CalendarTrigger><StartBoundary>" + startBoundary(t.atMinutes)
               + L"</StartBoundary><ScheduleByWeek><WeeksInterval>1</WeeksInterval>" + daysOfWeek(t.days)
               + L"</ScheduleByWeek></CalendarTrigger>";
    case TriggerKind::Monthly: {
        std::wstring day = t.dayOfMonth < 0 ? L"Last" : std::to_wstring(t.dayOfMonth);
        return L"<CalendarTrigger><StartBoundary>" + startBoundary(t.atMinutes)
               + L"</StartBoundary><ScheduleByMonth><DaysOfMonth><Day>" + day
               + L"</Day></DaysOfMonth><Months><January/><February/><March/><April/><May/><June/><July/><August/>"
                 L"<September/><October/><November/><December/></Months></ScheduleByMonth></CalendarTrigger>";
    }
    case TriggerKind::Every: {
        const int start = t.betweenStart >= 0 ? t.betweenStart : 0;
        const std::int64_t window = t.betweenStart >= 0 ? static_cast<std::int64_t>(t.betweenEnd - t.betweenStart) * 60 : 86400;
        const std::wstring repetition = L"<Repetition><Interval>" + isoDuration(t.intervalSeconds) + L"</Interval><Duration>"
                                        + isoDuration(window) + L"</Duration><StopAtDurationEnd>false</StopAtDurationEnd></Repetition>";
        const std::wstring schedule = t.days.empty()
                                          ? L"<ScheduleByDay><DaysInterval>1</DaysInterval></ScheduleByDay>"
                                          : L"<ScheduleByWeek><WeeksInterval>1</WeeksInterval>" + daysOfWeek(t.days) + L"</ScheduleByWeek>";
        return L"<CalendarTrigger>" + repetition + L"<StartBoundary>" + startBoundary(start) + L"</StartBoundary>" + schedule
               + L"</CalendarTrigger>";
    }
    case TriggerKind::OnStartup:
        return L"<BootTrigger>" + (t.delaySeconds > 0 ? L"<Delay>" + isoDuration(t.delaySeconds) + L"</Delay>" : std::wstring())
               + L"</BootTrigger>";
    case TriggerKind::OnLogon:
        return L"<LogonTrigger>" + (t.delaySeconds > 0 ? L"<Delay>" + isoDuration(t.delaySeconds) + L"</Delay>" : std::wstring())
               + L"</LogonTrigger>";
    case TriggerKind::Manual:
        return {};
    }
    return {};
}

class ComScope {
public:
    ComScope()
    {
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        m_owns = SUCCEEDED(hr);
    }
    ~ComScope()
    {
        if (m_owns)
            ::CoUninitialize();
    }

private:
    bool m_owns = false;
};

Error hr(HRESULT code, const char* what)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(code));
    return Error::make(code == E_ACCESSDENIED ? ErrorCode::AccessDenied : ErrorCode::IoError,
                       std::string("Agendador de Tarefas: ") + what + " falhou (" + buf + ")"
                           + (code == E_ACCESSDENIED ? "; execute como administrador" : ""),
                       static_cast<unsigned long>(code));
}

template <typename T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr()
    {
        if (p)
            p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
};

Status openFolder(ComPtr<ITaskService>& service, ComPtr<ITaskFolder>& folder, bool create)
{
    HRESULT h = ::CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService,
                                   reinterpret_cast<void**>(&service));
    if (FAILED(h))
        return hr(h, "conectar");
    VARIANT empty;
    ::VariantInit(&empty);
    if (FAILED(h = service->Connect(empty, empty, empty, empty)))
        return hr(h, "conectar");
    BSTR name = ::SysAllocString(L"\\Synkro");
    h = service->GetFolder(name, &folder);
    if (FAILED(h) && create) {
        ComPtr<ITaskFolder> root;
        BSTR rootName = ::SysAllocString(L"\\");
        h = service->GetFolder(rootName, &root);
        ::SysFreeString(rootName);
        if (SUCCEEDED(h)) {
            VARIANT sddl;
            ::VariantInit(&sddl);
            h = root->CreateFolder(name, sddl, &folder);
        }
    }
    ::SysFreeString(name);
    if (FAILED(h))
        return hr(h, "abrir a pasta \\Synkro");
    return Status::success();
}

} // namespace

std::wstring isoDuration(std::int64_t seconds)
{
    if (seconds <= 0)
        return L"PT0S";
    if (seconds % 86400 == 0)
        return L"P" + std::to_wstring(seconds / 86400) + L"D";
    std::wstring o = L"PT";
    if (seconds >= 3600) {
        o += std::to_wstring(seconds / 3600) + L"H";
        seconds %= 3600;
    }
    if (seconds >= 60) {
        o += std::to_wstring(seconds / 60) + L"M";
        seconds %= 60;
    }
    if (seconds > 0)
        o += std::to_wstring(seconds) + L"S";
    return o;
}

std::wstring buildTaskXml(const TaskDefinition& def)
{
    std::wstring triggers;
    for (const auto& t : def.triggers)
        triggers += triggerXml(t);

    std::wstring x = L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n"
                     L"<Task version=\"1.4\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n"
                     L"  <RegistrationInfo><Author>Synkro</Author><Description>"
                     + xmlEscape(def.description) + L"</Description></RegistrationInfo>\n"
                     L"  <Triggers>" + triggers + L"</Triggers>\n";
    if (!def.groupSid.empty())
        x += L"  <Principals><Principal id=\"Author\"><GroupId>" + xmlEscape(def.groupSid)
             + L"</GroupId><RunLevel>HighestAvailable</RunLevel></Principal></Principals>\n";
    else if (def.runAsSystem)
        x += L"  <Principals><Principal id=\"Author\"><UserId>S-1-5-18</UserId><RunLevel>HighestAvailable</RunLevel>"
             L"</Principal></Principals>\n";
    x += L"  <Settings>"
         L"<MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
         L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
         L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
         L"<StartWhenAvailable>"
         + std::wstring(def.startWhenAvailable ? L"true" : L"false")
         + L"</StartWhenAvailable>"
           L"<RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>"
           L"<IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>"
           L"<AllowStartOnDemand>true</AllowStartOnDemand><Enabled>true</Enabled><Hidden>false</Hidden>"
           L"<ExecutionTimeLimit>"
         + isoDuration(def.executionTimeLimitSeconds)
         + L"</ExecutionTimeLimit><Priority>" + std::to_wstring(def.priority) + L"</Priority></Settings>\n"
           L"  <Actions Context=\"Author\"><Exec><Command>"
         + xmlEscape(def.executable) + L"</Command><Arguments>" + xmlEscape(def.arguments)
         + L"</Arguments></Exec></Actions>\n</Task>\n";
    return x;
}

Status registerTask(const TaskDefinition& def)
{
    ComScope com;
    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> folder;
    auto opened = openFolder(service, folder, true);
    if (!opened)
        return opened.error();

    const std::wstring xml = buildTaskXml(def);
    BSTR name = ::SysAllocString(def.name.c_str());
    BSTR body = ::SysAllocString(xml.c_str());
    VARIANT user, password, sddl;
    ::VariantInit(&user);
    ::VariantInit(&password);
    ::VariantInit(&sddl);
    TASK_LOGON_TYPE logon = TASK_LOGON_INTERACTIVE_TOKEN;
    if (!def.groupSid.empty()) {
        logon = TASK_LOGON_GROUP; // o grupo vem do <GroupId> do XML
    } else if (def.runAsSystem) {
        user.vt = VT_BSTR;
        user.bstrVal = ::SysAllocString(L"SYSTEM");
        logon = TASK_LOGON_SERVICE_ACCOUNT;
    }
    ComPtr<IRegisteredTask> task;
    const HRESULT h = folder->RegisterTask(name, body, TASK_CREATE_OR_UPDATE, user, password, logon, sddl, &task);
    ::VariantClear(&user);
    ::SysFreeString(name);
    ::SysFreeString(body);
    if (FAILED(h))
        return hr(h, "registrar a tarefa");
    return Status::success();
}

TaskDefinition desktopAutostartTask(const std::wstring& desktopExe)
{
    TaskDefinition t;
    t.name = kDesktopTaskName;
    t.description = L"Abre o Synkro Desktop na bandeja, como administrador, ao entrar no Windows.";
    t.executable = desktopExe;
    t.arguments = L"--tray";
    config::ScheduleTrigger logon;
    logon.kind = config::TriggerKind::OnLogon;
    logon.delaySeconds = 10;
    t.triggers = {logon};
    t.runAsSystem = false;
    t.groupSid = L"S-1-5-32-545"; // Usuários (inclui os administradores)
    t.startWhenAvailable = false;
    t.executionTimeLimitSeconds = 0;
    t.priority = 4;
    return t;
}

bool taskExists(const std::wstring& name)
{
    auto tasks = listTasks();
    if (!tasks)
        return false;
    for (const auto& t : tasks.value())
        if (foldCase(t) == foldCase(name))
            return true;
    return false;
}

Status deleteTask(const std::wstring& name)
{
    ComScope com;
    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> folder;
    auto opened = openFolder(service, folder, false);
    if (!opened)
        return opened.error();
    BSTR n = ::SysAllocString(name.c_str());
    const HRESULT h = folder->DeleteTask(n, 0);
    ::SysFreeString(n);
    if (FAILED(h))
        return hr(h, "remover a tarefa");
    return Status::success();
}

Result<std::vector<std::wstring>> listTasks()
{
    ComScope com;
    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> folder;
    auto opened = openFolder(service, folder, false);
    if (!opened) {
        if (opened.error().systemCode == static_cast<unsigned long>(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)))
            return std::vector<std::wstring>{};
        return opened.error();
    }
    ComPtr<IRegisteredTaskCollection> tasks;
    HRESULT h = folder->GetTasks(TASK_ENUM_HIDDEN, &tasks);
    if (FAILED(h))
        return hr(h, "listar tarefas");
    LONG count = 0;
    tasks->get_Count(&count);
    std::vector<std::wstring> out;
    for (LONG i = 1; i <= count; ++i) {
        VARIANT idx;
        ::VariantInit(&idx);
        idx.vt = VT_I4;
        idx.lVal = i;
        ComPtr<IRegisteredTask> task;
        if (SUCCEEDED(tasks->get_Item(idx, &task))) {
            BSTR name = nullptr;
            if (SUCCEEDED(task->get_Name(&name)) && name) {
                out.emplace_back(name);
                ::SysFreeString(name);
            }
        }
    }
    return out;
}

Status runTaskNow(const std::wstring& name)
{
    ComScope com;
    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> folder;
    auto opened = openFolder(service, folder, false);
    if (!opened)
        return opened.error();
    BSTR n = ::SysAllocString(name.c_str());
    ComPtr<IRegisteredTask> task;
    HRESULT h = folder->GetTask(n, &task);
    ::SysFreeString(n);
    if (FAILED(h))
        return hr(h, "localizar a tarefa");
    VARIANT params;
    ::VariantInit(&params);
    ComPtr<IRunningTask> running;
    if (FAILED(h = task->Run(params, &running)))
        return hr(h, "iniciar a tarefa");
    return Status::success();
}

} // namespace synkro::win
