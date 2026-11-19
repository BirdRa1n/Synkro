#include "platform/windows/DiskHealth.h"

#include "platform/windows/Win32.h"

#include <objbase.h>
#include <oleauto.h>
#include <wbemidl.h>

#include <cstdio>
#include <cwchar>
#include <map>

namespace synkro::win {

const char* nameOf(HealthStatus s) noexcept
{
    switch (s) {
    case HealthStatus::Healthy:   return "healthy";
    case HealthStatus::Warning:   return "warning";
    case HealthStatus::Unhealthy: return "unhealthy";
    case HealthStatus::Unknown:   break;
    }
    return "unknown";
}

namespace {

template <typename T>
struct Com {
    T* p = nullptr;
    ~Com()
    {
        if (p)
            p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
};

class ComInit {
public:
    ComInit() { m_owns = SUCCEEDED(::CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
    ~ComInit()
    {
        if (m_owns)
            ::CoUninitialize();
    }

private:
    bool m_owns = false;
};

std::optional<long long> intProp(IWbemClassObject* o, const wchar_t* name)
{
    VARIANT v;
    ::VariantInit(&v);
    std::optional<long long> out;
    if (SUCCEEDED(o->Get(name, 0, &v, nullptr, nullptr)) && v.vt != VT_NULL && v.vt != VT_EMPTY) {
        VARIANT c;
        ::VariantInit(&c);
        if (v.vt == VT_BSTR) {
            out = std::wcstoll(v.bstrVal, nullptr, 10);
        } else if (SUCCEEDED(::VariantChangeType(&c, &v, 0, VT_I8))) {
            out = c.llVal;
        }
        ::VariantClear(&c);
    }
    ::VariantClear(&v);
    return out;
}

std::string strProp(IWbemClassObject* o, const wchar_t* name)
{
    VARIANT v;
    ::VariantInit(&v);
    std::string out;
    if (SUCCEEDED(o->Get(name, 0, &v, nullptr, nullptr)) && v.vt == VT_BSTR && v.bstrVal)
        out = toUtf8(v.bstrVal);
    ::VariantClear(&v);
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

std::vector<int> arrayProp(IWbemClassObject* o, const wchar_t* name)
{
    VARIANT v;
    ::VariantInit(&v);
    std::vector<int> out;
    if (SUCCEEDED(o->Get(name, 0, &v, nullptr, nullptr)) && (v.vt & VT_ARRAY) && v.parray) {
        LONG lo = 0, hi = -1;
        ::SafeArrayGetLBound(v.parray, 1, &lo);
        ::SafeArrayGetUBound(v.parray, 1, &hi);
        for (LONG i = lo; i <= hi; ++i) {
            VARIANT item;
            ::VariantInit(&item);
            unsigned short u = 0;
            if ((v.vt & VT_TYPEMASK) == VT_I4) {
                LONG x = 0;
                ::SafeArrayGetElement(v.parray, &i, &x);
                out.push_back(static_cast<int>(x));
            } else {
                ::SafeArrayGetElement(v.parray, &i, &u);
                out.push_back(u);
            }
        }
    }
    ::VariantClear(&v);
    return out;
}

const char* busName(long long b)
{
    switch (b) {
    case 7:  return "USB";
    case 11: return "SATA";
    case 17: return "NVMe";
    case 10: return "SAS";
    case 8:  return "RAID";
    case 3:  return "ATA";
    case 1:  return "SCSI";
    default: return "outro";
    }
}

std::string operationalProblem(int code)
{
    switch (code) {
    case 2:     return {};  // OK
    case 3:     return "degradado";
    case 5:     return "falha iminente prevista (SMART)";
    case 6:     return "erro";
    case 7:     return "sem recuperação";
    case 10:    return "parado";
    case 13:    return "comunicação perdida";
    case 0xD00A: return "desempenho reduzido";
    case 0xD00C: return "falha de E/S";
    case 0xD00E: return "transiente";
    default:    return code == 0 ? "desconhecido" : "estado " + std::to_string(code);
    }
}

} // namespace

Result<std::vector<DiskHealth>> queryDiskHealth()
{
    ComInit com;
    // Pode já ter sido chamado; RPC_E_TOO_LATE é aceitável.
    ::CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr,
                           EOAC_NONE, nullptr);

    Com<IWbemLocator> locator;
    HRESULT h = ::CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator,
                                   reinterpret_cast<void**>(&locator));
    if (FAILED(h))
        return Error::make(ErrorCode::Unsupported, "WMI indisponível", static_cast<unsigned long>(h));

    Com<IWbemServices> svc;
    BSTR ns = ::SysAllocString(L"ROOT\\Microsoft\\Windows\\Storage");
    h = locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc);
    ::SysFreeString(ns);
    if (FAILED(h))
        return Error::make(ErrorCode::Unsupported, "namespace de armazenamento do WMI indisponível", static_cast<unsigned long>(h));
    ::CoSetProxyBlanket(svc.p, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                        RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);

    auto query = [&](const wchar_t* wql, auto&& fn) -> HRESULT {
        BSTR lang = ::SysAllocString(L"WQL");
        BSTR q = ::SysAllocString(wql);
        Com<IEnumWbemClassObject> e;
        const HRESULT r = svc->ExecQuery(lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &e);
        ::SysFreeString(lang);
        ::SysFreeString(q);
        if (FAILED(r))
            return r;
        for (;;) {
            IWbemClassObject* obj = nullptr;
            ULONG n = 0;
            if (FAILED(e->Next(static_cast<LONG>(-1), 1, &obj, &n)) || n == 0)
                break;
            fn(obj);
            obj->Release();
        }
        return S_OK;
    };

    std::map<std::string, DiskHealth> byObjectId;
    std::vector<DiskHealth> disks;
    h = query(L"SELECT DeviceId, FriendlyName, HealthStatus, OperationalStatus, MediaType, BusType, ObjectId FROM MSFT_PhysicalDisk",
              [&](IWbemClassObject* o) {
                  DiskHealth d;
                  d.diskNumber = static_cast<std::uint32_t>(intProp(o, L"DeviceId").value_or(-1));
                  d.model = strProp(o, L"FriendlyName");
                  d.busType = busName(intProp(o, L"BusType").value_or(0));
                  const auto media = intProp(o, L"MediaType").value_or(0);
                  d.mediaType = media == 3 ? "HDD" : media == 4 ? "SSD" : media == 5 ? "SCM" : "";
                  switch (intProp(o, L"HealthStatus").value_or(5)) {
                  case 0: d.status = HealthStatus::Healthy; break;
                  case 1: d.status = HealthStatus::Warning; break;
                  case 2: d.status = HealthStatus::Unhealthy; break;
                  default: d.status = HealthStatus::Unknown;
                  }
                  for (int code : arrayProp(o, L"OperationalStatus")) {
                      const std::string p = operationalProblem(code);
                      if (!p.empty())
                          d.problems.push_back(p);
                  }
                  disks.push_back(d);
              });
    if (FAILED(h))
        return Error::make(ErrorCode::Unsupported, "consulta de discos no WMI falhou", static_cast<unsigned long>(h));

    // Contadores de confiabilidade: só com administrador (falha é ignorada).
    query(L"SELECT DeviceId, Temperature, Wear, ReadErrorsUncorrected FROM MSFT_StorageReliabilityCounter",
          [&](IWbemClassObject* o) {
              const auto id = intProp(o, L"DeviceId");
              for (auto& d : disks) {
                  if (id && static_cast<std::uint32_t>(*id) == d.diskNumber) {
                      if (auto t = intProp(o, L"Temperature"); t && *t > 0)
                          d.temperatureC = static_cast<int>(*t);
                      if (auto w = intProp(o, L"Wear"); w)
                          d.wearPercent = static_cast<int>(*w);
                      if (auto e = intProp(o, L"ReadErrorsUncorrected"); e)
                          d.readErrorsUncorrected = static_cast<std::uint64_t>(*e);
                  }
              }
          });
    return disks;
}

std::optional<DiskHealth> healthOfDisk(std::uint32_t n)
{
    auto all = queryDiskHealth();
    if (!all)
        return std::nullopt;
    for (auto& d : all.value()) {
        if (d.diskNumber == n)
            return d;
    }
    return std::nullopt;
}

std::string describeHealth(const DiskHealth& h)
{
    std::string s;
    switch (h.status) {
    case HealthStatus::Healthy:   s = "Saudável"; break;
    case HealthStatus::Warning:   s = "Atenção"; break;
    case HealthStatus::Unhealthy: s = "Com defeito"; break;
    case HealthStatus::Unknown:   s = "Desconhecida"; break;
    }
    if (!h.mediaType.empty())
        s += " · " + h.mediaType;
    if (!h.busType.empty())
        s += " " + h.busType;
    if (h.temperatureC)
        s += " · " + std::to_string(*h.temperatureC) + " °C";
    if (h.wearPercent && *h.wearPercent > 0)
        s += " · desgaste " + std::to_string(*h.wearPercent) + "%";
    for (const auto& p : h.problems)
        s += " · " + p;
    return s;
}

} // namespace synkro::win
