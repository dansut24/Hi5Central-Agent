#include <windows.h>
#include <tlhelp32.h>
#include <evntrace.h>
#include <tdh.h>
#include <winevt.h>
#include <ntsecapi.h>
#include <wbemidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using json=nlohmann::json;

namespace {
constexpr wchar_t kClassName[]=L"Hi5CentralQualificationObserverWindow";
constexpr UINT_PTR kTimerId=1;
constexpr UINT kTimerMs=750;

struct Options{
  std::wstring jobId;
  std::wstring application=L"Manual observation";
  std::wstring phase=L"manual";
  std::filesystem::path root;
};

struct AppEntry{
  std::wstring id,scope,key,name,version,publisher,productCode,parentKeyName;
  std::wstring uninstallString,quietUninstallString,installLocation;
  bool windowsInstaller=false;
  bool systemComponent=false;
};

struct ProcEntry{
  DWORD pid=0,parentPid=0;
  std::wstring name;
  std::wstring path;
  std::wstring commandLine;
};

struct ProcessObservation{
  std::chrono::steady_clock::time_point seen;
  std::string timestamp;
  ProcEntry process;
  std::string captureMethod;
};

struct ProcessAuditRestoreState{
  bool policyCaptured=false;
  ULONG previousAuditingInformation=0;
  bool registryCaptured=false;
  bool registryValueExisted=false;
  DWORD previousRegistryValue=0;
};

Options gOpt;
HWND gHeader=nullptr,gInstalled=nullptr,gHistory=nullptr,gEvents=nullptr,gDetails=nullptr,gStatus=nullptr;
HWND gInstalledLabel=nullptr,gHistoryLabel=nullptr,gEventsLabel=nullptr,gDetailsLabel=nullptr,gSafetyBanner=nullptr;
HWND gSnapshot=nullptr,gExport=nullptr,gStop=nullptr;
HBRUSH gWindowBrush=nullptr,gPanelBrush=nullptr,gBannerBrush=nullptr;
constexpr COLORREF kWindowBg=RGB(246,248,251);
constexpr COLORREF kPanelBg=RGB(255,255,255);
constexpr COLORREF kText=RGB(28,35,45);
constexpr COLORREF kMuted=RGB(90,101,116);
constexpr COLORREF kBannerBg=RGB(235,245,255);
constexpr COLORREF kBannerText=RGB(24,87,145);
std::map<std::wstring,AppEntry> gApps;
std::map<DWORD,ProcEntry> gProcs;
std::deque<ProcessObservation> gProcessJournal;
std::deque<ProcessObservation> gPendingProcessStarts;
std::mutex gProcessEventMutex;
std::atomic<bool> gProcessWatcherStop{false};
std::thread gProcessWatcher;
std::thread gEtwProcessWatcher;
std::atomic<TRACEHANDLE> gEtwSessionHandle{0};
std::atomic<bool> gEtwProcessReady{false};
std::atomic<ULONG> gEtwProcessError{ERROR_SUCCESS};
constexpr wchar_t kEtwProcessSessionName[]=L"Hi5Central Qualification ETW Process";
const GUID kEtwProcessSessionGuid={
  0x7a5b69e1,0xc8cb,0x4d02,{0x93,0x2d,0xd8,0xb3,0x26,0x88,0x34,0x51}
};
EVT_HANDLE gSecuritySubscription=nullptr;
ProcessAuditRestoreState gAuditRestore;
bool gProcessAuditActive=false;
const GUID kAuditProcessCreation={
  0x0cce922b,0x69ae,0x11d9,{0xbe,0xd3,0x50,0x50,0x54,0x50,0x30,0x30}
};
std::deque<std::wstring> gActivityLines;
std::vector<std::wstring> gInstalledIds;
std::vector<std::string> gHistoryIds;
std::string gSelectedId;
int gSelectedPane=0; // 0=none, 1=installed, 2=history
json gState={{"schemaVersion",1},{"apps",json::object()}};
std::ofstream gJsonl;
std::wofstream gLog;
bool gStopping=false;

std::string Narrow(const std::wstring& v){
  if(v.empty()) return {};
  int n=WideCharToMultiByte(CP_UTF8,0,v.c_str(),(int)v.size(),nullptr,0,nullptr,nullptr);
  std::string out(n,'\0');
  WideCharToMultiByte(CP_UTF8,0,v.c_str(),(int)v.size(),out.data(),n,nullptr,nullptr);
  return out;
}
std::wstring Widen(const std::string& v){
  if(v.empty()) return {};
  int n=MultiByteToWideChar(CP_UTF8,0,v.c_str(),(int)v.size(),nullptr,0);
  std::wstring out(n,L'\0');
  MultiByteToWideChar(CP_UTF8,0,v.c_str(),(int)v.size(),out.data(),n);
  return out;
}
std::wstring Lower(std::wstring v){
  std::transform(v.begin(),v.end(),v.begin(),[](wchar_t c){return towlower(c);});
  return v;
}
std::wstring Trim(std::wstring v){
  while(!v.empty()&&iswspace(v.front()))v.erase(v.begin());
  while(!v.empty()&&iswspace(v.back()))v.pop_back();
  return v;
}
std::string NowIso(){
  SYSTEMTIME s{}; GetSystemTime(&s); char b[64]{};
  sprintf_s(b,"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",s.wYear,s.wMonth,s.wDay,s.wHour,s.wMinute,s.wSecond,s.wMilliseconds);
  return b;
}
std::wstring NowLocal(){
  SYSTEMTIME s{}; GetLocalTime(&s); wchar_t b[64]{};
  swprintf_s(b,L"%02u:%02u:%02u",s.wHour,s.wMinute,s.wSecond);
  return b;
}
std::wstring Arg(const std::vector<std::wstring>& args,const std::wstring& key){
  for(size_t i=0;i+1<args.size();++i) if(_wcsicmp(args[i].c_str(),key.c_str())==0) return args[i+1];
  return {};
}
std::filesystem::path ProgramDataRoot(){
  wchar_t p[MAX_PATH]{};
  if(SUCCEEDED(SHGetFolderPathW(nullptr,CSIDL_COMMON_APPDATA,nullptr,SHGFP_TYPE_CURRENT,p)))
    return std::filesystem::path(p)/L"Hi5Central"/L"Agent"/L"Qualification";
  return L"C:\\ProgramData\\Hi5Central\\Agent\\Qualification";
}
std::wstring DefaultJob(){
  SYSTEMTIME s{}; GetLocalTime(&s); wchar_t b[96]{};
  swprintf_s(b,L"manual-%04u%02u%02u-%02u%02u%02u",s.wYear,s.wMonth,s.wDay,s.wHour,s.wMinute,s.wSecond);
  return b;
}
Options ParseOptions(){
  int argc=0; LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc); std::vector<std::wstring> args;
  if(argv){for(int i=0;i<argc;++i)args.emplace_back(argv[i]);LocalFree(argv);}
  Options o; o.jobId=Arg(args,L"--job-id"); if(o.jobId.empty())o.jobId=DefaultJob();
  if(auto v=Arg(args,L"--application");!v.empty())o.application=v;
  if(auto v=Arg(args,L"--phase");!v.empty())o.phase=v;
  auto root=Arg(args,L"--root"); o.root=root.empty()?ProgramDataRoot():std::filesystem::path(root);
  return o;
}

std::filesystem::path AuditRestorePath(const std::filesystem::path& root){
  return root/L"process-audit-restore.json";
}

bool EnablePrivilege(const wchar_t* privilege){
  HANDLE token=nullptr;
  if(!OpenProcessToken(
    GetCurrentProcess(),
    TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,
    &token))return false;

  LUID luid{};
  if(!LookupPrivilegeValueW(nullptr,privilege,&luid)){
    CloseHandle(token);
    return false;
  }

  TOKEN_PRIVILEGES tp{};
  tp.PrivilegeCount=1;
  tp.Privileges[0].Luid=luid;
  tp.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;

  SetLastError(ERROR_SUCCESS);
  const BOOL ok=AdjustTokenPrivileges(
    token,FALSE,&tp,sizeof(tp),nullptr,nullptr);
  const DWORD err=GetLastError();
  CloseHandle(token);
  return ok&&err==ERROR_SUCCESS;
}

bool SaveAuditRestoreState(
  const std::filesystem::path& root,
  const ProcessAuditRestoreState& state){
  std::error_code ec;
  std::filesystem::create_directories(root,ec);
  std::ofstream out(
    AuditRestorePath(root),
    std::ios::binary|std::ios::trunc);
  if(!out)return false;
  out<<json{
    {"schemaVersion",1},
    {"capturedAt",NowIso()},
    {"policyCaptured",state.policyCaptured},
    {"previousAuditingInformation",state.previousAuditingInformation},
    {"registryCaptured",state.registryCaptured},
    {"registryValueExisted",state.registryValueExisted},
    {"previousRegistryValue",state.previousRegistryValue}
  }.dump(2);
  return (bool)out;
}

bool LoadAuditRestoreState(
  const std::filesystem::path& root,
  ProcessAuditRestoreState& state){
  std::ifstream in(AuditRestorePath(root),std::ios::binary);
  if(!in)return false;
  try{
    json value;in>>value;
    state.policyCaptured=value.value("policyCaptured",false);
    state.previousAuditingInformation=
      value.value("previousAuditingInformation",0UL);
    state.registryCaptured=value.value("registryCaptured",false);
    state.registryValueExisted=value.value("registryValueExisted",false);
    state.previousRegistryValue=value.value("previousRegistryValue",0UL);
    return true;
  }catch(...){
    return false;
  }
}

bool RestoreProcessAuditSettings(
  const std::filesystem::path& root,
  const ProcessAuditRestoreState& state,
  bool removeRecoveryFile=true){
  bool ok=true;

  if(state.policyCaptured){
    AUDIT_POLICY_INFORMATION policy{};
    policy.AuditSubCategoryGuid=kAuditProcessCreation;
    policy.AuditingInformation=state.previousAuditingInformation;
    if(!AuditSetSystemPolicy(&policy,1))ok=false;
  }

  if(state.registryCaptured){
    constexpr wchar_t keyPath[]=
      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System\\Audit";
    constexpr wchar_t valueName[]=
      L"ProcessCreationIncludeCmdLine_Enabled";

    HKEY key=nullptr;
    DWORD disposition=0;
    if(RegCreateKeyExW(
      HKEY_LOCAL_MACHINE,
      keyPath,
      0,
      nullptr,
      REG_OPTION_NON_VOLATILE,
      KEY_QUERY_VALUE|KEY_SET_VALUE,
      nullptr,
      &key,
      &disposition)!=ERROR_SUCCESS){
      ok=false;
    }else{
      if(state.registryValueExisted){
        const DWORD value=state.previousRegistryValue;
        if(RegSetValueExW(
          key,
          valueName,
          0,
          REG_DWORD,
          reinterpret_cast<const BYTE*>(&value),
          sizeof(value))!=ERROR_SUCCESS)ok=false;
      }else{
        const LONG rc=RegDeleteValueW(key,valueName);
        if(rc!=ERROR_SUCCESS&&rc!=ERROR_FILE_NOT_FOUND)ok=false;
      }
      RegCloseKey(key);
    }
  }

  if(ok&&removeRecoveryFile){
    std::error_code ec;
    std::filesystem::remove(AuditRestorePath(root),ec);
  }
  return ok;
}

bool RecoverStaleProcessAuditSettings(
  const std::filesystem::path& root){
  ProcessAuditRestoreState stale;
  if(!LoadAuditRestoreState(root,stale))return true;
  EnablePrivilege(L"SeSecurityPrivilege");
  return RestoreProcessAuditSettings(root,stale,true);
}

bool EnableQualificationProcessAudit(
  const std::filesystem::path& root){
  EnablePrivilege(L"SeSecurityPrivilege");

  ProcessAuditRestoreState state;
  PAUDIT_POLICY_INFORMATION current=nullptr;
  if(!AuditQuerySystemPolicy(
    &kAuditProcessCreation,
    1,
    &current)||
    !current){
    return false;
  }
  state.policyCaptured=true;
  state.previousAuditingInformation=current[0].AuditingInformation;
  AuditFree(current);

  constexpr wchar_t keyPath[]=
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System\\Audit";
  constexpr wchar_t valueName[]=
    L"ProcessCreationIncludeCmdLine_Enabled";

  HKEY key=nullptr;
  DWORD disposition=0;
  if(RegCreateKeyExW(
    HKEY_LOCAL_MACHINE,
    keyPath,
    0,
    nullptr,
    REG_OPTION_NON_VOLATILE,
    KEY_QUERY_VALUE|KEY_SET_VALUE,
    nullptr,
    &key,
    &disposition)!=ERROR_SUCCESS){
    return false;
  }

  state.registryCaptured=true;
  DWORD type=0,value=0,size=sizeof(value);
  const LONG query=RegQueryValueExW(
    key,
    valueName,
    nullptr,
    &type,
    reinterpret_cast<BYTE*>(&value),
    &size);
  if(query==ERROR_SUCCESS&&type==REG_DWORD){
    state.registryValueExisted=true;
    state.previousRegistryValue=value;
  }else if(query!=ERROR_FILE_NOT_FOUND){
    RegCloseKey(key);
    return false;
  }

  if(!SaveAuditRestoreState(root,state)){
    RegCloseKey(key);
    return false;
  }

  AUDIT_POLICY_INFORMATION desired{};
  desired.AuditSubCategoryGuid=kAuditProcessCreation;
  desired.AuditingInformation=
    (state.previousAuditingInformation&~POLICY_AUDIT_EVENT_NONE)|
    POLICY_AUDIT_EVENT_SUCCESS;

  bool ok=AuditSetSystemPolicy(&desired,1)!=FALSE;
  const DWORD enabled=1;
  if(RegSetValueExW(
    key,
    valueName,
    0,
    REG_DWORD,
    reinterpret_cast<const BYTE*>(&enabled),
    sizeof(enabled))!=ERROR_SUCCESS){
    ok=false;
  }
  RegCloseKey(key);

  if(!ok){
    RestoreProcessAuditSettings(root,state,true);
    return false;
  }

  gAuditRestore=state;
  gProcessAuditActive=true;
  return true;
}

void DisableQualificationProcessAudit(){
  if(!gProcessAuditActive)return;
  RestoreProcessAuditSettings(gOpt.root,gAuditRestore,true);
  gProcessAuditActive=false;
}

std::wstring RegString(HKEY h,const wchar_t* n){
  DWORD t=0,b=0; if(RegQueryValueExW(h,n,nullptr,&t,nullptr,&b)!=ERROR_SUCCESS||(t!=REG_SZ&&t!=REG_EXPAND_SZ)||b<2)return {};
  std::vector<wchar_t> buf(b/2+2,L'\0'); if(RegQueryValueExW(h,n,nullptr,&t,(LPBYTE)buf.data(),&b)!=ERROR_SUCCESS)return {};
  return Trim(buf.data());
}
DWORD RegDword(HKEY h,const wchar_t* n){
  DWORD t=0,v=0,b=sizeof(v); return RegQueryValueExW(h,n,nullptr,&t,(LPBYTE)&v,&b)==ERROR_SUCCESS&&t==REG_DWORD?v:0;
}
bool LooksGuid(const std::wstring& v){
  if(v.size()!=38||v.front()!=L'{'||v.back()!=L'}')return false;
  std::set<size_t>d{9,14,19,24}; for(size_t i=1;i+1<v.size();++i){if(d.count(i)?v[i]!=L'-':!iswxdigit(v[i]))return false;} return true;
}
void ReadUninstallRoot(HKEY hive,const wchar_t* path,REGSAM view,const std::wstring& scope,std::map<std::wstring,AppEntry>& out){
  HKEY root=nullptr; if(RegOpenKeyExW(hive,path,0,KEY_READ|view,&root)!=ERROR_SUCCESS)return;
  for(DWORD i=0;;++i){
    wchar_t sub[512]{}; DWORD len=(DWORD)std::size(sub); LONG rc=RegEnumKeyExW(root,i,sub,&len,nullptr,nullptr,nullptr,nullptr);
    if(rc==ERROR_NO_MORE_ITEMS)break; if(rc!=ERROR_SUCCESS)continue;
    HKEY a=nullptr; if(RegOpenKeyExW(root,sub,0,KEY_READ|view,&a)!=ERROR_SUCCESS)continue;
    AppEntry e; e.scope=scope;e.key=sub;e.name=RegString(a,L"DisplayName");e.version=RegString(a,L"DisplayVersion");e.publisher=RegString(a,L"Publisher");
    e.uninstallString=RegString(a,L"UninstallString");e.quietUninstallString=RegString(a,L"QuietUninstallString");e.installLocation=RegString(a,L"InstallLocation");
    e.parentKeyName=RegString(a,L"ParentKeyName");
    e.windowsInstaller=RegDword(a,L"WindowsInstaller")==1;
    e.systemComponent=RegDword(a,L"SystemComponent")==1;
    if(LooksGuid(e.key))e.productCode=e.key; RegCloseKey(a);
    if(e.name.empty()&&e.uninstallString.empty()&&e.quietUninstallString.empty())continue;
    e.id=Lower(e.scope+L"|"+e.key+L"|"+e.name); out[e.id]=std::move(e);
  }
  RegCloseKey(root);
}
void ReadLoadedUserUninstallRoots(std::map<std::wstring,AppEntry>& out){
  HKEY users=nullptr;if(RegOpenKeyExW(HKEY_USERS,L"",0,KEY_READ,&users)!=ERROR_SUCCESS)return;
  for(DWORD i=0;;++i){
    wchar_t sid[256]{};DWORD len=(DWORD)std::size(sid);LONG rc=RegEnumKeyExW(users,i,sid,&len,nullptr,nullptr,nullptr,nullptr);
    if(rc==ERROR_NO_MORE_ITEMS)break;if(rc!=ERROR_SUCCESS)continue;
    std::wstring s=sid;
    if(s.rfind(L"S-1-5-21-",0)!=0)continue;
    const std::wstring path=s+L"\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    ReadUninstallRoot(HKEY_USERS,path.c_str(),KEY_WOW64_64KEY,L"user:"+s+L":64",out);
    ReadUninstallRoot(HKEY_USERS,path.c_str(),KEY_WOW64_32KEY,L"user:"+s+L":32",out);
  }
  RegCloseKey(users);
}
std::map<std::wstring,AppEntry> CaptureApps(){
  std::map<std::wstring,AppEntry> out; constexpr wchar_t path[]=L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
  ReadUninstallRoot(HKEY_LOCAL_MACHINE,path,KEY_WOW64_64KEY,L"machine64",out);
  ReadUninstallRoot(HKEY_LOCAL_MACHINE,path,KEY_WOW64_32KEY,L"machine32",out);
  ReadLoadedUserUninstallRoots(out);
  return out;
}
bool IsTestableApplication(const AppEntry& e){
  if(e.name.empty()||e.systemComponent||!e.parentKeyName.empty())return false;
  if(e.uninstallString.empty()&&e.quietUninstallString.empty())return false;
  const std::wstring n=Lower(e.name);
  static const wchar_t* blocked[]={
    L"application verifier",L"diagnosticshub",L"icecap_",L"intellitrace",L"kits configuration installer",
    L"microsoft .net host",L"microsoft .net runtime",L"microsoft .net targeting pack",
    L"microsoft asp.net core",L"windows desktop runtime",L"windows software development kit",
    L"windows sdk",L"microsoft visual c++",L"vc++ redistributable"
  };
  for(const auto* token:blocked)if(n.find(token)!=std::wstring::npos)return false;
  return true;
}
std::wstring UninstallTechnology(const AppEntry& e){
  const std::wstring u=Lower(e.uninstallString+L" "+e.quietUninstallString);
  if(e.windowsInstaller||!e.productCode.empty()||u.find(L"msiexec")!=std::wstring::npos)return L"MSI";
  if(u.find(L"officeclicktorun.exe")!=std::wstring::npos)return L"Microsoft Office Click-to-Run";
  if(u.find(L"rundll32")!=std::wstring::npos)return L"Rundll32";
  if(u.find(L"unins")!=std::wstring::npos)return L"Inno Setup / EXE";
  if(u.find(L"uninstall.exe")!=std::wstring::npos||u.find(L"uninstaller.exe")!=std::wstring::npos)return L"Vendor EXE";
  return L"Registered EXE/command";
}
std::wstring RegisteredRecipe(const AppEntry& e){
  return !e.quietUninstallString.empty()?e.quietUninstallString:e.uninstallString;
}
std::wstring CandidateSilentRecipe(const AppEntry& e){
  if(!e.quietUninstallString.empty())return e.quietUninstallString;
  if(!e.productCode.empty())return L"msiexec.exe /x "+e.productCode+L" /qn /norestart";
  return {};
}
std::wstring VariantString(VARIANT& v){
  if(v.vt==VT_BSTR&&v.bstrVal)return v.bstrVal;
  if(v.vt==VT_NULL||v.vt==VT_EMPTY)return {};
  VARIANT tmp{};VariantInit(&tmp);
  if(FAILED(VariantChangeType(&tmp,&v,0,VT_BSTR))){VariantClear(&tmp);return {};}
  std::wstring out=tmp.bstrVal?std::wstring(tmp.bstrVal):std::wstring();VariantClear(&tmp);return out;
}
DWORD VariantDword(VARIANT& v){
  if(v.vt==VT_I4||v.vt==VT_INT)return (DWORD)v.lVal;
  if(v.vt==VT_UI4||v.vt==VT_UINT)return (DWORD)v.ulVal;
  return 0;
}
std::map<DWORD,ProcEntry> CaptureProcs(){
  std::map<DWORD,ProcEntry> out;
  IWbemLocator* locator=nullptr;IWbemServices* services=nullptr;IEnumWbemClassObject* rows=nullptr;
  HRESULT hr=CoCreateInstance(CLSID_WbemLocator,nullptr,CLSCTX_INPROC_SERVER,IID_IWbemLocator,(void**)&locator);
  if(FAILED(hr)||!locator)return out;
  BSTR ns=SysAllocString(L"ROOT\\CIMV2");
  hr=locator->ConnectServer(ns,nullptr,nullptr,nullptr,0,nullptr,nullptr,&services);SysFreeString(ns);
  if(FAILED(hr)||!services){locator->Release();return out;}
  CoSetProxyBlanket(services,RPC_C_AUTHN_WINNT,RPC_C_AUTHZ_NONE,nullptr,RPC_C_AUTHN_LEVEL_CALL,RPC_C_IMP_LEVEL_IMPERSONATE,nullptr,EOAC_NONE);
  BSTR lang=SysAllocString(L"WQL");
  BSTR query=SysAllocString(L"SELECT ProcessId,ParentProcessId,Name,ExecutablePath,CommandLine FROM Win32_Process");
  hr=services->ExecQuery(lang,query,WBEM_FLAG_FORWARD_ONLY|WBEM_FLAG_RETURN_IMMEDIATELY,nullptr,&rows);
  SysFreeString(lang);SysFreeString(query);
  if(SUCCEEDED(hr)&&rows){
    for(;;){
      IWbemClassObject* obj=nullptr;ULONG n=0;hr=rows->Next(1200,1,&obj,&n);if(FAILED(hr)||n==0||!obj)break;
      VARIANT a{},b{},c{},d{},e{};VariantInit(&a);VariantInit(&b);VariantInit(&c);VariantInit(&d);VariantInit(&e);
      obj->Get(L"ProcessId",0,&a,nullptr,nullptr);obj->Get(L"ParentProcessId",0,&b,nullptr,nullptr);
      obj->Get(L"Name",0,&c,nullptr,nullptr);obj->Get(L"ExecutablePath",0,&d,nullptr,nullptr);obj->Get(L"CommandLine",0,&e,nullptr,nullptr);
      ProcEntry p;p.pid=VariantDword(a);p.parentPid=VariantDword(b);p.name=VariantString(c);p.path=VariantString(d);p.commandLine=VariantString(e);
      if(p.pid)out[p.pid]=std::move(p);
      VariantClear(&a);VariantClear(&b);VariantClear(&c);VariantClear(&d);VariantClear(&e);obj->Release();
    }
  }
  if(rows)rows->Release();services->Release();locator->Release();return out;
}

ProcEntry CaptureProcessDetails(IWbemServices* services,DWORD pid,DWORD parentPid,const std::wstring& nameHint){
  ProcEntry p;p.pid=pid;p.parentPid=parentPid;p.name=nameHint;
  if(!services||!pid)return p;

  wchar_t queryText[160]{};
  swprintf_s(queryText,L"SELECT Name,ExecutablePath,CommandLine FROM Win32_Process WHERE ProcessId=%lu",pid);
  BSTR lang=SysAllocString(L"WQL");
  BSTR query=SysAllocString(queryText);
  IEnumWbemClassObject* rows=nullptr;
  HRESULT hr=services->ExecQuery(lang,query,WBEM_FLAG_FORWARD_ONLY|WBEM_FLAG_RETURN_IMMEDIATELY,nullptr,&rows);
  SysFreeString(lang);SysFreeString(query);

  if(SUCCEEDED(hr)&&rows){
    IWbemClassObject* obj=nullptr;ULONG count=0;
    hr=rows->Next(400,1,&obj,&count);
    if(SUCCEEDED(hr)&&count&&obj){
      VARIANT a{},b{},c{};VariantInit(&a);VariantInit(&b);VariantInit(&c);
      obj->Get(L"Name",0,&a,nullptr,nullptr);
      obj->Get(L"ExecutablePath",0,&b,nullptr,nullptr);
      obj->Get(L"CommandLine",0,&c,nullptr,nullptr);
      const auto n=VariantString(a);if(!n.empty())p.name=n;
      p.path=VariantString(b);p.commandLine=VariantString(c);
      VariantClear(&a);VariantClear(&b);VariantClear(&c);obj->Release();
    }
    rows->Release();
  }

  if(p.path.empty()){
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(process){
      wchar_t path[32768]{};DWORD size=(DWORD)std::size(path);
      if(QueryFullProcessImageNameW(process,0,path,&size))p.path.assign(path,size);
      CloseHandle(process);
    }
  }
  return p;
}

void ProcessStartWatcherLoop(){
  const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
  IWbemLocator* locator=nullptr;IWbemServices* services=nullptr;IEnumWbemClassObject* events=nullptr;

  HRESULT hr=CoCreateInstance(CLSID_WbemLocator,nullptr,CLSCTX_INPROC_SERVER,IID_IWbemLocator,(void**)&locator);
  if(SUCCEEDED(hr)&&locator){
    BSTR ns=SysAllocString(L"ROOT\\CIMV2");
    hr=locator->ConnectServer(ns,nullptr,nullptr,nullptr,0,nullptr,nullptr,&services);
    SysFreeString(ns);
  }
  if(SUCCEEDED(hr)&&services){
    CoSetProxyBlanket(services,RPC_C_AUTHN_WINNT,RPC_C_AUTHZ_NONE,nullptr,RPC_C_AUTHN_LEVEL_CALL,RPC_C_IMP_LEVEL_IMPERSONATE,nullptr,EOAC_NONE);
    BSTR lang=SysAllocString(L"WQL");
    BSTR query=SysAllocString(L"SELECT * FROM Win32_ProcessStartTrace");
    hr=services->ExecNotificationQuery(lang,query,WBEM_FLAG_FORWARD_ONLY|WBEM_FLAG_RETURN_IMMEDIATELY,nullptr,&events);
    SysFreeString(lang);SysFreeString(query);
  }

  while(!gProcessWatcherStop.load()&&events){
    IWbemClassObject* obj=nullptr;ULONG count=0;
    hr=events->Next(500,1,&obj,&count);
    if(count&&obj){
      VARIANT a{},b{},c{};VariantInit(&a);VariantInit(&b);VariantInit(&c);
      obj->Get(L"ProcessID",0,&a,nullptr,nullptr);
      obj->Get(L"ParentProcessID",0,&b,nullptr,nullptr);
      obj->Get(L"ProcessName",0,&c,nullptr,nullptr);
      const DWORD pid=VariantDword(a),parentPid=VariantDword(b);
      const std::wstring name=VariantString(c);
      VariantClear(&a);VariantClear(&b);VariantClear(&c);obj->Release();

      if(pid){
        ProcessObservation observation{
          std::chrono::steady_clock::now(),
          NowIso(),
          CaptureProcessDetails(services,pid,parentPid,name),
          "Win32_ProcessStartTrace"
        };
        std::lock_guard<std::mutex> guard(gProcessEventMutex);
        gPendingProcessStarts.push_back(std::move(observation));
        while(gPendingProcessStarts.size()>4000)gPendingProcessStarts.pop_front();
      }
    }
  }

  if(events)events->Release();
  if(services)services->Release();
  if(locator)locator->Release();
  if(SUCCEEDED(com))CoUninitialize();
}

std::vector<BYTE> EtwPropertyBytes(
  PEVENT_RECORD event,
  const wchar_t* propertyName){
  std::vector<BYTE> out;
  if(!event||!propertyName||!*propertyName)return out;

  PROPERTY_DATA_DESCRIPTOR descriptor{};
  descriptor.PropertyName=
    reinterpret_cast<ULONGLONG>(propertyName);
  descriptor.ArrayIndex=ULONG_MAX;

  ULONG size=0;
  ULONG status=TdhGetPropertySize(
    event,
    0,
    nullptr,
    1,
    &descriptor,
    &size);
  if(status!=ERROR_SUCCESS||!size)return out;

  out.resize(size);
  status=TdhGetProperty(
    event,
    0,
    nullptr,
    1,
    &descriptor,
    size,
    out.data());
  if(status!=ERROR_SUCCESS)out.clear();
  return out;
}

DWORD EtwDwordProperty(
  PEVENT_RECORD event,
  const wchar_t* propertyName){
  const auto bytes=EtwPropertyBytes(event,propertyName);
  if(bytes.size()<sizeof(DWORD))return 0;
  DWORD value=0;
  memcpy(&value,bytes.data(),sizeof(value));
  return value;
}

std::wstring EtwWideProperty(
  PEVENT_RECORD event,
  const wchar_t* propertyName){
  const auto bytes=EtwPropertyBytes(event,propertyName);
  if(bytes.size()<sizeof(wchar_t))return {};
  const auto* value=
    reinterpret_cast<const wchar_t*>(bytes.data());
  size_t count=bytes.size()/sizeof(wchar_t);
  while(count&&value[count-1]==L'\0')--count;
  return std::wstring(value,count);
}

std::wstring EtwAnsiProperty(
  PEVENT_RECORD event,
  const wchar_t* propertyName){
  const auto bytes=EtwPropertyBytes(event,propertyName);
  if(bytes.empty())return {};
  size_t count=bytes.size();
  while(count&&bytes[count-1]==0)--count;
  if(!count)return {};
  const char* value=
    reinterpret_cast<const char*>(bytes.data());
  const int needed=MultiByteToWideChar(
    CP_ACP,
    0,
    value,
    static_cast<int>(count),
    nullptr,
    0);
  if(needed<=0)return {};
  std::wstring out(needed,L'\0');
  MultiByteToWideChar(
    CP_ACP,
    0,
    value,
    static_cast<int>(count),
    out.data(),
    needed);
  return out;
}

void WINAPI KernelProcessEtwCallback(
  PEVENT_RECORD event){
  if(!event||gProcessWatcherStop.load())return;
  if(event->EventHeader.EventDescriptor.Opcode!=
     EVENT_TRACE_TYPE_START)return;

  const DWORD pid=
    EtwDwordProperty(event,L"ProcessId");
  if(!pid)return;

  const DWORD parentPid=
    EtwDwordProperty(event,L"ParentId");
  const std::wstring image=
    EtwAnsiProperty(event,L"ImageFileName");
  const std::wstring command=
    EtwWideProperty(event,L"CommandLine");

  ProcEntry process;
  process.pid=pid;
  process.parentPid=parentPid;
  process.path=image;
  process.commandLine=command;

  if(!image.empty()){
    try{
      process.name=
        std::filesystem::path(image).filename().wstring();
    }catch(...){
      process.name=image;
    }
  }else if(!command.empty()){
    std::wstring executable=Trim(command);
    if(!executable.empty()&&executable.front()==L'"'){
      const auto end=executable.find(L'"',1);
      executable=end==std::wstring::npos
        ? executable.substr(1)
        : executable.substr(1,end-1);
    }else{
      const auto end=executable.find_first_of(L" \t");
      if(end!=std::wstring::npos)
        executable=executable.substr(0,end);
    }
    process.path=executable;
    try{
      process.name=
        std::filesystem::path(executable).filename().wstring();
    }catch(...){
      process.name=executable;
    }
  }

  ProcessObservation observation{
    std::chrono::steady_clock::now(),
    NowIso(),
    std::move(process),
    "KernelETW_ProcessStart"
  };

  std::lock_guard<std::mutex> guard(gProcessEventMutex);
  gPendingProcessStarts.push_back(std::move(observation));
  while(gPendingProcessStarts.size()>4000)
    gPendingProcessStarts.pop_front();
}

std::vector<BYTE> KernelEtwPropertiesBuffer(){
  const size_t nameChars=wcslen(kEtwProcessSessionName)+1;
  const size_t bytes=
    sizeof(EVENT_TRACE_PROPERTIES)+
    nameChars*sizeof(wchar_t);
  std::vector<BYTE> buffer(bytes,0);
  auto* properties=
    reinterpret_cast<EVENT_TRACE_PROPERTIES*>(
      buffer.data());
  properties->Wnode.BufferSize=
    static_cast<ULONG>(bytes);
  properties->Wnode.Flags=WNODE_FLAG_TRACED_GUID;
  properties->Wnode.Guid=kEtwProcessSessionGuid;
  properties->Wnode.ClientContext=1;
  properties->BufferSize=64;
  properties->MinimumBuffers=4;
  properties->MaximumBuffers=32;
  properties->LogFileMode=
    EVENT_TRACE_REAL_TIME_MODE|
    EVENT_TRACE_SYSTEM_LOGGER_MODE;
  properties->EnableFlags=EVENT_TRACE_FLAG_PROCESS;
  properties->LoggerNameOffset=
    sizeof(EVENT_TRACE_PROPERTIES);

  auto* name=reinterpret_cast<wchar_t*>(
    buffer.data()+properties->LoggerNameOffset);
  wcscpy_s(name,nameChars,kEtwProcessSessionName);
  return buffer;
}

void StopKernelProcessEtwSession(){
  auto buffer=KernelEtwPropertiesBuffer();
  auto* properties=
    reinterpret_cast<EVENT_TRACE_PROPERTIES*>(
      buffer.data());
  TRACEHANDLE session=gEtwSessionHandle.exchange(0);
  ControlTraceW(
    session,
    kEtwProcessSessionName,
    properties,
    EVENT_TRACE_CONTROL_STOP);
  gEtwProcessReady.store(false);
}

void KernelProcessEtwWatcherLoop(){
  EnablePrivilege(L"SeSystemProfilePrivilege");

  // Recover a stale private logger if the previous observer was killed.
  {
    auto stale=KernelEtwPropertiesBuffer();
    ControlTraceW(
      0,
      kEtwProcessSessionName,
      reinterpret_cast<EVENT_TRACE_PROPERTIES*>(
        stale.data()),
      EVENT_TRACE_CONTROL_STOP);
  }

  auto buffer=KernelEtwPropertiesBuffer();
  auto* properties=
    reinterpret_cast<EVENT_TRACE_PROPERTIES*>(
      buffer.data());

  TRACEHANDLE session=0;
  ULONG status=StartTraceW(
    &session,
    kEtwProcessSessionName,
    properties);
  if(status==ERROR_ALREADY_EXISTS){
    ControlTraceW(
      0,
      kEtwProcessSessionName,
      properties,
      EVENT_TRACE_CONTROL_STOP);
    buffer=KernelEtwPropertiesBuffer();
    properties=
      reinterpret_cast<EVENT_TRACE_PROPERTIES*>(
        buffer.data());
    status=StartTraceW(
      &session,
      kEtwProcessSessionName,
      properties);
  }
  if(status!=ERROR_SUCCESS){
    gEtwProcessError.store(status);
    gEtwProcessReady.store(false);
    return;
  }

  gEtwSessionHandle.store(session);

  EVENT_TRACE_LOGFILEW logfile{};
  logfile.LoggerName=
    const_cast<LPWSTR>(kEtwProcessSessionName);
  logfile.ProcessTraceMode=
    PROCESS_TRACE_MODE_REAL_TIME|
    PROCESS_TRACE_MODE_EVENT_RECORD;
  logfile.EventRecordCallback=
    KernelProcessEtwCallback;

  TRACEHANDLE trace=OpenTraceW(&logfile);
  if(trace==INVALID_PROCESSTRACE_HANDLE){
    gEtwProcessError.store(GetLastError());
    StopKernelProcessEtwSession();
    return;
  }

  gEtwProcessError.store(ERROR_SUCCESS);
  gEtwProcessReady.store(true);
  const ULONG processStatus=
    ProcessTrace(&trace,1,nullptr,nullptr);
  gEtwProcessReady.store(false);

  CloseTrace(trace);

  TRACEHANDLE active=
    gEtwSessionHandle.exchange(0);
  if(active){
    auto stopBuffer=KernelEtwPropertiesBuffer();
    ControlTraceW(
      active,
      kEtwProcessSessionName,
      reinterpret_cast<EVENT_TRACE_PROPERTIES*>(
        stopBuffer.data()),
      EVENT_TRACE_CONTROL_STOP);
  }

  if(processStatus!=ERROR_SUCCESS&&
     processStatus!=ERROR_CANCELLED)
    gEtwProcessError.store(processStatus);
}

std::wstring ReplaceAll(
  std::wstring value,
  const std::wstring& from,
  const std::wstring& to){
  if(from.empty())return value;
  size_t pos=0;
  while((pos=value.find(from,pos))!=std::wstring::npos){
    value.replace(pos,from.size(),to);
    pos+=to.size();
  }
  return value;
}

std::wstring XmlDecode(std::wstring value){
  value=ReplaceAll(std::move(value),L"&quot;",L"\"");
  value=ReplaceAll(std::move(value),L"&apos;",L"'");
  value=ReplaceAll(std::move(value),L"&lt;",L"<");
  value=ReplaceAll(std::move(value),L"&gt;",L">");
  value=ReplaceAll(std::move(value),L"&amp;",L"&");
  return value;
}

std::wstring EventDataValue(
  const std::wstring& xml,
  const std::wstring& name){
  const std::wstring doubleMarker=L"<Data Name=\""+name+L"\">";
  const std::wstring singleMarker=L"<Data Name='"+name+L"'>";
  size_t start=xml.find(doubleMarker);
  size_t markerSize=doubleMarker.size();
  if(start==std::wstring::npos){
    start=xml.find(singleMarker);
    markerSize=singleMarker.size();
  }
  if(start==std::wstring::npos)return {};
  start+=markerSize;
  const size_t end=xml.find(L"</Data>",start);
  if(end==std::wstring::npos)return {};
  return XmlDecode(xml.substr(start,end-start));
}

DWORD ParseEventProcessId(const std::wstring& value){
  if(value.empty())return 0;
  wchar_t* end=nullptr;
  const unsigned long long parsed=wcstoull(value.c_str(),&end,0);
  if(end==value.c_str())return 0;
  return (DWORD)parsed;
}

DWORD WINAPI SecurityProcessEventCallback(
  EVT_SUBSCRIBE_NOTIFY_ACTION action,
  PVOID,
  EVT_HANDLE event){
  if(action!=EvtSubscribeActionDeliver||!event||gProcessWatcherStop.load())
    return ERROR_SUCCESS;

  DWORD used=0,count=0;
  if(!EvtRender(
    nullptr,
    event,
    EvtRenderEventXml,
    0,
    nullptr,
    &used,
    &count)&&
    GetLastError()!=ERROR_INSUFFICIENT_BUFFER){
    return ERROR_SUCCESS;
  }
  if(!used)return ERROR_SUCCESS;

  std::vector<wchar_t> buffer(
    used/sizeof(wchar_t)+2,
    L'\0');
  if(!EvtRender(
    nullptr,
    event,
    EvtRenderEventXml,
    used,
    buffer.data(),
    &used,
    &count)){
    return ERROR_SUCCESS;
  }

  const std::wstring xml=buffer.data();
  const DWORD pid=ParseEventProcessId(
    EventDataValue(xml,L"NewProcessId"));
  if(!pid)return ERROR_SUCCESS;

  const DWORD parentPid=ParseEventProcessId(
    EventDataValue(xml,L"ProcessId"));
  const std::wstring image=EventDataValue(
    xml,L"NewProcessName");
  std::wstring command=EventDataValue(
    xml,L"CommandLine");
  if(Trim(command)==L"-")command.clear();

  ProcEntry process;
  process.pid=pid;
  process.parentPid=parentPid;
  process.path=image;
  process.commandLine=command;
  if(!image.empty()){
    try{
      process.name=std::filesystem::path(image).filename().wstring();
    }catch(...){
      process.name=image;
    }
  }

  ProcessObservation observation{
    std::chrono::steady_clock::now(),
    NowIso(),
    std::move(process),
    "Security4688_CommandLineAudit"
  };

  std::lock_guard<std::mutex> guard(gProcessEventMutex);
  gPendingProcessStarts.push_back(std::move(observation));
  while(gPendingProcessStarts.size()>4000)
    gPendingProcessStarts.pop_front();
  return ERROR_SUCCESS;
}

bool StartSecurityProcessSubscription(){
  if(gSecuritySubscription)return true;
  gSecuritySubscription=EvtSubscribe(
    nullptr,
    nullptr,
    L"Security",
    L"*[System[(EventID=4688)]]",
    nullptr,
    nullptr,
    SecurityProcessEventCallback,
    EvtSubscribeToFutureEvents);
  return gSecuritySubscription!=nullptr;
}

void StopSecurityProcessSubscription(){
  EVT_HANDLE handle=gSecuritySubscription;
  gSecuritySubscription=nullptr;
  if(handle)EvtClose(handle);
}

std::vector<ProcessObservation> DrainProcessStartEvents(){
  std::vector<ProcessObservation> out;
  std::lock_guard<std::mutex> guard(gProcessEventMutex);
  out.reserve(gPendingProcessStarts.size());
  while(!gPendingProcessStarts.empty()){
    out.push_back(std::move(gPendingProcessStarts.front()));
    gPendingProcessStarts.pop_front();
  }
  return out;
}

json AppJson(const AppEntry&e){
  return{{"id",Narrow(e.id)},{"scope",Narrow(e.scope)},{"registryKey",Narrow(e.key)},{"displayName",Narrow(e.name)},{"version",Narrow(e.version)},
    {"publisher",Narrow(e.publisher)},{"productCode",Narrow(e.productCode)},{"windowsInstaller",e.windowsInstaller},
    {"systemComponent",e.systemComponent},{"parentKeyName",Narrow(e.parentKeyName)},{"testable",IsTestableApplication(e)},
    {"uninstallString",Narrow(e.uninstallString)},{"quietUninstallString",Narrow(e.quietUninstallString)},{"installLocation",Narrow(e.installLocation)}};
}
void RenderActivity(){
  // Live Activity is a row-based LISTBOX. Rows are appended individually in
  // AppendLog so user scrolling is never replaced by a full-control redraw.
}
void AppendActivityRow(const std::wstring& line){
  if(!gEvents)return;

  const LRESULT countBefore=SendMessageW(gEvents,LB_GETCOUNT,0,0);
  const LRESULT topBefore=SendMessageW(gEvents,LB_GETTOPINDEX,0,0);
  const LRESULT itemHeight=SendMessageW(gEvents,LB_GETITEMHEIGHT,0,0);
  RECT rect{};GetClientRect(gEvents,&rect);
  const int visibleRows=itemHeight>0?std::max(1,(rect.bottom-rect.top)/(int)itemHeight):10;
  const bool followTail=countBefore<=0||
    topBefore+visibleRows>=countBefore-1;

  SendMessageW(gEvents,WM_SETREDRAW,FALSE,0);
  SendMessageW(gEvents,LB_ADDSTRING,0,(LPARAM)line.c_str());

  HDC dc=GetDC(gEvents);
  if(dc){
    HFONT font=(HFONT)SendMessageW(gEvents,WM_GETFONT,0,0);
    HGDIOBJ oldFont=font?SelectObject(dc,font):nullptr;
    SIZE size{};
    if(GetTextExtentPoint32W(dc,line.c_str(),(int)line.size(),&size)){
      const LRESULT currentExtent=SendMessageW(gEvents,LB_GETHORIZONTALEXTENT,0,0);
      if(size.cx+24>currentExtent)
        SendMessageW(gEvents,LB_SETHORIZONTALEXTENT,size.cx+24,0);
    }
    if(oldFont)SelectObject(dc,oldFont);
    ReleaseDC(gEvents,dc);
  }

  LRESULT count=SendMessageW(gEvents,LB_GETCOUNT,0,0);
  while(count>220){
    SendMessageW(gEvents,LB_DELETESTRING,0,0);
    --count;
  }

  if(followTail){
    const int newTop=std::max(0,(int)count-visibleRows);
    SendMessageW(gEvents,LB_SETTOPINDEX,newTop,0);
  }else{
    const int adjustedTop=std::max(0,(int)topBefore-(countBefore>=220?1:0));
    SendMessageW(gEvents,LB_SETTOPINDEX,adjustedTop,0);
  }

  SendMessageW(gEvents,WM_SETREDRAW,TRUE,0);
  InvalidateRect(gEvents,nullptr,TRUE);
}
void AppendLog(const std::wstring& text,const json& ev){
  const std::wstring line=L"["+NowLocal()+L"] "+text;
  gActivityLines.push_back(line);
  while(gActivityLines.size()>220)gActivityLines.pop_front();
  AppendActivityRow(line);
  if(gLog.is_open()){gLog<<line<<L"\r\n";gLog.flush();}
  if(gJsonl.is_open()){gJsonl<<ev.dump()<<"\n";gJsonl.flush();}
}
void LoadState(){
  auto p=gOpt.root/L"observer-state.json"; std::ifstream f(p,std::ios::binary);
  if(f){try{f>>gState;}catch(...){gState={{"schemaVersion",1},{"apps",json::object()}};}}
  if(!gState.contains("apps")||!gState["apps"].is_object())gState["apps"]=json::object();
}
void SaveState(){
  std::error_code ec;std::filesystem::create_directories(gOpt.root,ec);std::ofstream f(gOpt.root/L"observer-state.json",std::ios::binary|std::ios::trunc);f<<gState.dump(2);
}
void BackfillStateRecipes(){
  for(auto it=gState["apps"].begin();it!=gState["apps"].end();++it){
    auto&s=it.value();if(!s.is_object())continue;
    AppEntry e;
    e.name=Widen(s.value("displayName",std::string()));
    e.version=Widen(s.value("version",std::string()));
    e.publisher=Widen(s.value("publisher",std::string()));
    e.scope=Widen(s.value("scope",std::string()));
    e.key=Widen(s.value("registryKey",std::string()));
    e.productCode=Widen(s.value("productCode",std::string()));
    e.uninstallString=Widen(s.value("uninstallString",s.value("registeredUninstallCommand",std::string())));
    e.quietUninstallString=Widen(s.value("quietUninstallString",s.value("registeredQuietUninstallCommand",std::string())));
    e.installLocation=Widen(s.value("installLocation",std::string()));
    e.parentKeyName=Widen(s.value("parentKeyName",std::string()));
    e.systemComponent=s.value("systemComponent",false);
    e.windowsInstaller=!e.productCode.empty()||Lower(e.uninstallString).find(L"msiexec")!=std::wstring::npos;
    s["testable"]=IsTestableApplication(e);
    s["uninstallTechnology"]=Narrow(UninstallTechnology(e));
    s["registeredUninstallCommand"]=Narrow(e.uninstallString);
    s["registeredQuietUninstallCommand"]=Narrow(e.quietUninstallString);
    s["learnedUninstallCommand"]=Narrow(RegisteredRecipe(e));
    s["candidateSilentUninstallCommand"]=Narrow(CandidateSilentRecipe(e));

    if(!s.contains("observedUninstallActionCommand")&&
       s.contains("observedUninstallProcesses")&&
       s["observedUninstallProcesses"].is_array()){
      const json* bestAction=nullptr;
      int bestScore=-1;
      for(const auto& process:s["observedUninstallProcesses"]){
        if(!process.is_object())continue;
        const auto command=process.value("commandLine",std::string());
        if(command.empty())continue;
        bool hasUninstallSemantics=false;
        if(process.contains("correlationReasons")&&process["correlationReasons"].is_array()){
          for(const auto& reason:process["correlationReasons"]){
            if(reason.is_string()&&reason.get<std::string>()=="uninstall_semantics_in_command"){
              hasUninstallSemantics=true;
              break;
            }
          }
        }
        if(!hasUninstallSemantics)continue;
        const int score=process.value("correlationScore",0);
        if(score>bestScore){bestScore=score;bestAction=&process;}
      }
      if(bestAction){
        s["observedUninstallActionCommand"]=bestAction->value("commandLine",std::string());
        s["observedUninstallActionArguments"]=bestAction->value("arguments",json::array());
        s["observedUninstallActionSwitches"]=bestAction->value("switches",json::array());
        s["observedUninstallActionCaptureMethod"]=bestAction->value("captureMethod",std::string());
        s["observedUninstallActionCorrelationScore"]=bestAction->value("correlationScore",0);
        s["observedUninstallActionCorrelationReasons"]=bestAction->value("correlationReasons",json::array());

        if(s.contains("patchingEvidence")&&s["patchingEvidence"].is_object()&&
           s["patchingEvidence"].contains("uninstall")&&s["patchingEvidence"]["uninstall"].is_object()){
          auto& portal=s["patchingEvidence"]["uninstall"];
          portal["observedActionCommand"]=s["observedUninstallActionCommand"];
          portal["observedActionArguments"]=s["observedUninstallActionArguments"];
          portal["observedActionSwitches"]=s["observedUninstallActionSwitches"];
          portal["observedActionCaptureMethod"]=s["observedUninstallActionCaptureMethod"];
          portal["observedActionCorrelationScore"]=s["observedUninstallActionCorrelationScore"];
          portal["observedActionCorrelationReasons"]=s["observedUninstallActionCorrelationReasons"];
        }
      }
    }

    if(!s.value("currentlyInstalled",false)&&!s.contains("lastUninstalledAt")&&s.contains("history")&&s["history"].is_array()){
      for(auto h=s["history"].rbegin();h!=s["history"].rend();++h){
        if(h->value("event",std::string())=="uninstalled"){
          s["lastUninstalledAt"]=h->value("timestamp",std::string());
          s["uninstallObservationStatus"]="registered_recipe_observed_removal_confirmed";
          break;
        }
      }
    }
  }
  SaveState();
}
void RecordAppState(const AppEntry&e,bool installed,const char* eventType){
  const std::string id=Narrow(e.id),now=NowIso(); auto& s=gState["apps"][id];
  if(!s.is_object())s=json::object();
  if(!s.contains("firstSeenAt"))s["firstSeenAt"]=now;
  s["lastSeenAt"]=now;s["currentlyInstalled"]=installed;s["displayName"]=Narrow(e.name);s["version"]=Narrow(e.version);s["publisher"]=Narrow(e.publisher);
  s["scope"]=Narrow(e.scope);s["registryKey"]=Narrow(e.key);s["productCode"]=Narrow(e.productCode);s["uninstallString"]=Narrow(e.uninstallString);
  s["quietUninstallString"]=Narrow(e.quietUninstallString);s["installLocation"]=Narrow(e.installLocation);
  s["systemComponent"]=e.systemComponent;s["parentKeyName"]=Narrow(e.parentKeyName);s["testable"]=IsTestableApplication(e);
  s["uninstallTechnology"]=Narrow(UninstallTechnology(e));
  s["registeredUninstallCommand"]=Narrow(e.uninstallString);
  s["registeredQuietUninstallCommand"]=Narrow(e.quietUninstallString);
  s["learnedUninstallCommand"]=Narrow(RegisteredRecipe(e));
  s["candidateSilentUninstallCommand"]=Narrow(CandidateSilentRecipe(e));
  if(std::string(eventType)=="uninstalled"){
    s["lastUninstalledAt"]=now;
    s["uninstallObservationStatus"]="registered_recipe_observed_removal_confirmed";
  }
  if(!s.contains("history")||!s["history"].is_array())s["history"]=json::array();
  s["history"].push_back({{"timestamp",now},{"event",eventType},{"jobId",Narrow(gOpt.jobId)},{"application",Narrow(gOpt.application)},{"phase",Narrow(gOpt.phase)},{"snapshot",AppJson(e)}});
  SaveState();
}
void PopulateLists(){
  SendMessageW(gInstalled,LB_RESETCONTENT,0,0); SendMessageW(gHistory,LB_RESETCONTENT,0,0);
  gInstalledIds.clear();gHistoryIds.clear();

  std::set<std::wstring> shownInstalled;
  int installedSelection=-1;
  for(const auto&[id,e]:gApps){
    if(!IsTestableApplication(e))continue;
    const std::wstring visibleKey=Lower(e.name+L"|"+e.key);
    if(!shownInstalled.insert(visibleKey).second)continue;
    std::wstring row=e.name+(e.version.empty()?L"":L"  ["+e.version+L"]");
    const int index=(int)gInstalledIds.size();
    SendMessageW(gInstalled,LB_ADDSTRING,0,(LPARAM)row.c_str());
    gInstalledIds.push_back(id);
    if(gSelectedPane==1&&Narrow(id)==gSelectedId)installedSelection=index;
  }

  std::vector<std::tuple<std::wstring,std::string,std::wstring>> hist;
  std::set<std::wstring> shownHistory;
  for(auto it=gState["apps"].begin();it!=gState["apps"].end();++it){
    const auto&s=it.value();
    if(s.value("currentlyInstalled",false)||!s.value("testable",false))continue;

    const std::string removedAt=s.value("lastUninstalledAt",std::string());
    const std::string removalStatus=s.value("uninstallObservationStatus",std::string());
    if(removedAt.empty()&&removalStatus.empty())continue;

    const auto display=Widen(s.value("displayName",std::string()));
    const auto registryKey=Widen(s.value("registryKey",std::string()));
    if(display.empty())continue;
    const std::wstring visibleKey=Lower(display+L"|"+registryKey);
    if(!shownHistory.insert(visibleKey).second)continue;

    std::wstring row=display+L"  ["+Widen(s.value("version",std::string()))+L"]  removed";
    hist.push_back({row,it.key(),visibleKey});
  }
  std::sort(hist.begin(),hist.end(),[](const auto&a,const auto&b){return std::get<0>(a)<std::get<0>(b);});

  int historySelection=-1;
  for(auto&item:hist){
    const int index=(int)gHistoryIds.size();
    SendMessageW(gHistory,LB_ADDSTRING,0,(LPARAM)std::get<0>(item).c_str());
    gHistoryIds.push_back(std::get<1>(item));
    if(gSelectedPane==2&&std::get<1>(item)==gSelectedId)historySelection=index;
  }

  if(installedSelection>=0)SendMessageW(gInstalled,LB_SETCURSEL,installedSelection,0);
  if(historySelection>=0)SendMessageW(gHistory,LB_SETCURSEL,historySelection,0);

  if(gSelectedPane==1&&installedSelection<0&&!gSelectedId.empty()){
    for(size_t i=0;i<gHistoryIds.size();++i){
      if(gHistoryIds[i]==gSelectedId){
        gSelectedPane=2;
        SendMessageW(gHistory,LB_SETCURSEL,(WPARAM)i,0);
        break;
      }
    }
  }
}
void SetDetailsText(const std::wstring& value){
  if(!gDetails)return;
  SendMessageW(gDetails,WM_SETREDRAW,FALSE,0);
  SetWindowTextW(gDetails,value.c_str());
  SendMessageW(gDetails,EM_SETSEL,0,0);
  SendMessageW(gDetails,EM_SCROLLCARET,0,0);
  SendMessageW(gDetails,WM_SETREDRAW,TRUE,0);
  RedrawWindow(gDetails,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_UPDATENOW|RDW_FRAME);
}
void ShowInstalledDetails(size_t index){
  if(index>=gInstalledIds.size())return;
  auto it=gApps.find(gInstalledIds[index]);if(it==gApps.end())return;const auto&e=it->second;
  std::wstringstream s;s<<L"INSTALLED APPLICATION / LEARNED IDENTITY\r\n============================================================\r\n";
  s<<L"DisplayName: "<<e.name<<L"\r\nVersion: "<<e.version<<L"\r\nPublisher: "<<e.publisher<<L"\r\nScope: "<<e.scope<<L"\r\n";
  s<<L"Uninstall technology: "<<UninstallTechnology(e)<<L"\r\n";
  if(!e.productCode.empty())s<<L"MSI ProductCode: "<<e.productCode<<L"\r\n";
  if(!e.uninstallString.empty())s<<L"Registered uninstall command: "<<e.uninstallString<<L"\r\n";
  if(!e.quietUninstallString.empty())s<<L"Registered quiet uninstall: "<<e.quietUninstallString<<L"\r\n";
  const auto candidate=CandidateSilentRecipe(e);if(!candidate.empty())s<<L"Candidate silent recipe: "<<candidate<<L"\r\n";
  s<<L"Recipe status: observed registration; not yet automatically verified\r\n";
  SetDetailsText(s.str());
}
void ShowHistoryDetails(size_t index){
  if(index>=gHistoryIds.size())return;
  auto it=gState["apps"].find(gHistoryIds[index]);if(it==gState["apps"].end())return;const auto&s0=*it;
  std::wstringstream s;s<<L"REMOVED APPLICATION / LEARNED UNINSTALL RECIPE\r\n============================================================\r\n";
  s<<L"DisplayName: "<<Widen(s0.value("displayName",std::string()))<<L"\r\n";
  s<<L"Version: "<<Widen(s0.value("version",std::string()))<<L"\r\n";
  s<<L"Publisher: "<<Widen(s0.value("publisher",std::string()))<<L"\r\n";
  s<<L"Uninstall technology: "<<Widen(s0.value("uninstallTechnology",std::string()))<<L"\r\n";
  const auto product=Widen(s0.value("productCode",std::string()));if(!product.empty())s<<L"MSI ProductCode: "<<product<<L"\r\n";
  const auto reg=Widen(s0.value("registeredUninstallCommand",std::string()));if(!reg.empty())s<<L"Registered uninstall command: "<<reg<<L"\r\n";
  const auto quiet=Widen(s0.value("registeredQuietUninstallCommand",std::string()));if(!quiet.empty())s<<L"Registered quiet uninstall: "<<quiet<<L"\r\n";
  const auto silent=Widen(s0.value("candidateSilentUninstallCommand",std::string()));if(!silent.empty())s<<L"Candidate silent recipe: "<<silent<<L"\r\n";
  const auto observed=Widen(s0.value("observedUninstallCommand",std::string()));
  if(!observed.empty()){
    s<<L"Observed exact uninstall command: "<<observed<<L"\r\n";
    if(s0.contains("observedUninstallSwitches")&&s0["observedUninstallSwitches"].is_array()){
      s<<L"Observed switches:";
      for(const auto& value:s0["observedUninstallSwitches"])s<<L" "<<Widen(value.get<std::string>());
      s<<L"\r\n";
    }
    if(s0.contains("observedUninstallArguments")&&s0["observedUninstallArguments"].is_array()){
      s<<L"Observed arguments:";
      for(const auto& value:s0["observedUninstallArguments"])s<<L" ["<<Widen(value.get<std::string>())<<L"]";
      s<<L"\r\n";
    }
  }

  const auto captureMethod=Widen(s0.value("observedUninstallCaptureMethod",std::string()));
  if(!captureMethod.empty())s<<L"Launcher capture method: "<<captureMethod<<L"\r\n";

  const auto action=Widen(s0.value("observedUninstallActionCommand",std::string()));
  if(!action.empty()){
    s<<L"Observed downstream uninstall action: "<<action<<L"\r\n";
    if(s0.contains("observedUninstallActionSwitches")&&s0["observedUninstallActionSwitches"].is_array()){
      s<<L"Observed action switches:";
      for(const auto& value:s0["observedUninstallActionSwitches"])s<<L" "<<Widen(value.get<std::string>());
      s<<L"\r\n";
    }
    const auto actionCapture=Widen(s0.value("observedUninstallActionCaptureMethod",std::string()));
    if(!actionCapture.empty())s<<L"Action capture method: "<<actionCapture<<L"\r\n";
    s<<L"Action note: observed during confirmed removal; not yet verified as a reusable silent recipe.\r\n";
  }

  const auto observedProcess=Widen(s0.value("observedUninstallProcessCommand",std::string()));
  const int correlationScore=s0.value("observedUninstallCorrelationScore",0);
  if(!observedProcess.empty()){
    const wchar_t* confidence=correlationScore>=120?L"high":(correlationScore>=80?L"strong":L"medium");
    s<<L"Observed process activity: "<<observedProcess<<L"\r\n";
    s<<L"Process correlation: "<<confidence<<L" ("<<correlationScore<<L")\r\n";
  }

  s<<L"Removal confirmed: "<<Widen(s0.value("lastUninstalledAt",std::string()))<<L"\r\n";
  s<<L"Recipe status: "<<Widen(s0.value("uninstallObservationStatus",std::string("observed")) )<<L"\r\n";

  if(s0.contains("observedUninstallProcesses")&&s0["observedUninstallProcesses"].is_array()&&!s0["observedUninstallProcesses"].empty()){
    s<<L"\r\nCorrelated process timeline:\r\n";
    for(const auto&p:s0["observedUninstallProcesses"]){
      const auto cmd=Widen(p.value("commandLine",std::string()));
      const auto name=Widen(p.value("name",std::string()));
      const int score=p.value("correlationScore",0);
      s<<L"  ["<<score<<L"] "<<name;
      if(!cmd.empty())s<<L" | "<<cmd;
      if(p.contains("correlationReasons")&&p["correlationReasons"].is_array()){
        s<<L" | ";
        bool first=true;
        for(const auto& reason:p["correlationReasons"]){
          if(!first)s<<L", ";
          s<<Widen(reason.get<std::string>());
          first=false;
        }
      }
      s<<L"\r\n";
    }
  }
  SetDetailsText(s.str());
}
void RefreshDetails(){
  std::wstringstream s;s<<L"CURRENT TARGET / LEARNED IDENTITY\r\n============================================================\r\n";
  const auto target=Lower(gOpt.application);bool any=false;
  for(const auto&[_,e]:gApps){auto n=Lower(e.name);if(target.empty()||n.empty()||(n.find(target)==std::wstring::npos&&target.find(n)==std::wstring::npos))continue;any=true;
    s<<L"DisplayName: "<<e.name<<L"\r\nVersion: "<<e.version<<L"\r\nPublisher: "<<e.publisher<<L"\r\nScope: "<<e.scope<<L"\r\n";
    s<<L"Uninstall technology: "<<UninstallTechnology(e)<<L"\r\n";
    if(!e.productCode.empty())s<<L"MSI ProductCode: "<<e.productCode<<L"\r\n";
    if(!e.uninstallString.empty())s<<L"Registered UninstallString: "<<e.uninstallString<<L"\r\n";
    if(!e.quietUninstallString.empty())s<<L"Registered QuietUninstallString: "<<e.quietUninstallString<<L"\r\n";
    const auto candidate=CandidateSilentRecipe(e);
    if(!candidate.empty())s<<L"Candidate silent recipe: "<<candidate<<L"\r\n";
    if(!e.installLocation.empty())s<<L"InstallLocation: "<<e.installLocation<<L"\r\n";
  }
  if(!any)s<<L"Target is not currently registered as installed.\r\nThe observer will retain it in History after removal and update this pane if it appears again.\r\n";
  SetDetailsText(s.str());
}

std::wstring ExecutableNameFromCommand(const std::wstring& command){
  std::wstring value=Trim(command);
  if(value.empty())return {};
  std::wstring exe;
  if(value.front()==L'"'){
    const auto end=value.find(L'"',1);
    exe=end==std::wstring::npos?value.substr(1):value.substr(1,end-1);
  }else{
    const auto end=value.find_first_of(L" \t");
    exe=end==std::wstring::npos?value:value.substr(0,end);
  }
  try{return Lower(std::filesystem::path(exe).filename().wstring());}catch(...){return Lower(exe);}
}

json CommandArgumentsJson(const std::wstring& commandLine){
  json args=json::array();
  if(commandLine.empty())return args;
  int argc=0;
  LPWSTR* argv=CommandLineToArgvW(commandLine.c_str(),&argc);
  if(!argv)return args;
  for(int i=1;i<argc;++i)args.push_back(Narrow(argv[i]));
  LocalFree(argv);
  return args;
}

json CommandSwitchesJson(const std::wstring& commandLine){
  json switches=json::array();
  if(commandLine.empty())return switches;
  int argc=0;
  LPWSTR* argv=CommandLineToArgvW(commandLine.c_str(),&argc);
  if(!argv)return switches;
  for(int i=1;i<argc;++i){
    const std::wstring token=argv[i]?argv[i]:L"";
    if(!token.empty()&&(token.front()==L'/'||token.front()==L'-'))
      switches.push_back(Narrow(token));
  }
  LocalFree(argv);
  return switches;
}

bool HasExactIdentityEvidence(const std::vector<std::string>& reasons){
  static const std::set<std::string> strong={
    "product_code_in_command",
    "registered_uninstaller_executable",
    "registered_uninstaller_path",
    "process_inside_install_location"
  };
  for(const auto& reason:reasons)if(strong.count(reason))return true;
  return false;
}

std::pair<int,std::vector<std::string>> ScoreUninstallProcess(
  const AppEntry& app,
  const ProcessObservation& observation,
  const std::set<DWORD>& strongParentPids){
  const auto& p=observation.process;
  const std::wstring name=Lower(p.name);
  const std::wstring path=Lower(p.path);
  const std::wstring command=Lower(p.commandLine);
  const std::wstring registeredExe=ExecutableNameFromCommand(
    !app.quietUninstallString.empty()?app.quietUninstallString:app.uninstallString);
  const std::wstring product=Lower(app.productCode);
  const std::wstring installLocation=Lower(app.installLocation);
  int score=0;
  std::vector<std::string> reasons;

  if(!product.empty()&&command.find(product)!=std::wstring::npos){
    score+=140;reasons.push_back("product_code_in_command");
  }
  const bool genericHost=
    registeredExe==L"msiexec.exe"||registeredExe==L"rundll32.exe"||
    registeredExe==L"powershell.exe"||registeredExe==L"pwsh.exe"||
    registeredExe==L"cmd.exe"||registeredExe==L"wscript.exe"||
    registeredExe==L"cscript.exe";

  if(!registeredExe.empty()&&name==registeredExe){
    if(genericHost){score+=15;reasons.push_back("generic_registered_host_executable");}
    else{score+=100;reasons.push_back("registered_uninstaller_executable");}
  }
  if(!registeredExe.empty()&&!path.empty()&&
     Lower(std::filesystem::path(path).filename().wstring())==registeredExe){
    if(genericHost){score+=10;reasons.push_back("generic_registered_host_path");}
    else{score+=90;reasons.push_back("registered_uninstaller_path");}
  }
  if(app.windowsInstaller&&name==L"msiexec.exe"){
    score+=35;reasons.push_back("windows_installer_process");
  }
  if(command.find(L" /x")!=std::wstring::npos||command.find(L"/uninstall")!=std::wstring::npos||
     command.find(L" uninstall")!=std::wstring::npos||command.find(L" remove")!=std::wstring::npos){
    score+=35;reasons.push_back("uninstall_semantics_in_command");
  }
  if(!installLocation.empty()&&!path.empty()&&path.rfind(installLocation,0)==0){
    score+=55;reasons.push_back("process_inside_install_location");
  }
  if(strongParentPids.count(p.parentPid)){
    score+=50;reasons.push_back("child_of_matched_uninstaller");
  }

  const auto age=std::chrono::duration_cast<std::chrono::seconds>(
    std::chrono::steady_clock::now()-observation.seen).count();
  if(age<=15){score+=20;reasons.push_back("within_15_seconds_of_removal");}
  else if(age<=60){score+=10;reasons.push_back("within_60_seconds_of_removal");}

  return {score,reasons};
}

void CorrelateRemovalWithRecentProcesses(const AppEntry& app,const std::string& appId){
  auto stateIt=gState["apps"].find(appId);
  if(stateIt==gState["apps"].end())return;
  auto& state=*stateIt;

  const auto now=std::chrono::steady_clock::now();
  while(!gProcessJournal.empty()&&now-gProcessJournal.front().seen>std::chrono::minutes(5))
    gProcessJournal.pop_front();

  std::set<DWORD> strongParentPids;
  for(const auto& observation:gProcessJournal){
    if(now-observation.seen>std::chrono::minutes(3))continue;
    const auto scored=ScoreUninstallProcess(app,observation,{});
    if(scored.first>=80)strongParentPids.insert(observation.process.pid);
  }

  struct Candidate{
    int score=0;
    ProcessObservation observation;
    std::vector<std::string> reasons;
  };
  std::vector<Candidate> candidates;
  for(const auto& observation:gProcessJournal){
    if(now-observation.seen>std::chrono::minutes(3))continue;
    auto scored=ScoreUninstallProcess(app,observation,strongParentPids);
    if(scored.first<45)continue;
    candidates.push_back({scored.first,observation,std::move(scored.second)});
  }
  std::sort(candidates.begin(),candidates.end(),[](const Candidate&a,const Candidate&b){
    if(a.score!=b.score)return a.score>b.score;
    return a.observation.seen>b.observation.seen;
  });
  if(candidates.size()>12)candidates.resize(12);

  json observed=json::array();
  for(const auto& candidate:candidates){
    json reasons=json::array();
    for(const auto& reason:candidate.reasons)reasons.push_back(reason);
    observed.push_back({
      {"timestamp",candidate.observation.timestamp},
      {"pid",candidate.observation.process.pid},
      {"parentPid",candidate.observation.process.parentPid},
      {"name",Narrow(candidate.observation.process.name)},
      {"path",Narrow(candidate.observation.process.path)},
      {"commandLine",Narrow(candidate.observation.process.commandLine)},
      {"arguments",CommandArgumentsJson(candidate.observation.process.commandLine)},
      {"switches",CommandSwitchesJson(candidate.observation.process.commandLine)},
      {"captureMethod",candidate.observation.captureMethod},
      {"correlationScore",candidate.score},
      {"correlationReasons",reasons}
    });
  }
  state["observedUninstallProcesses"]=observed;

  if(!candidates.empty()){
    const auto& best=candidates.front();
    state["observedUninstallProcessName"]=Narrow(best.observation.process.name);
    state["observedUninstallProcessPath"]=Narrow(best.observation.process.path);
    state["observedUninstallProcessCommand"]=Narrow(best.observation.process.commandLine);
    state["observedUninstallProcessArguments"]=CommandArgumentsJson(best.observation.process.commandLine);
    state["observedUninstallProcessSwitches"]=CommandSwitchesJson(best.observation.process.commandLine);
    state["observedUninstallCaptureMethod"]=best.observation.captureMethod;
    state["observedUninstallCorrelationScore"]=best.score;
    state["observedUninstallCorrelationReasons"]=best.reasons;
    state["observedUninstallCorrelationAt"]=NowIso();

    const Candidate* actionCandidate=nullptr;
    for(const auto& candidate:candidates){
      if(candidate.observation.process.commandLine.empty())continue;
      if(std::find(
        candidate.reasons.begin(),
        candidate.reasons.end(),
        "uninstall_semantics_in_command")!=candidate.reasons.end()){
        actionCandidate=&candidate;
        break;
      }
    }
    if(actionCandidate){
      state["observedUninstallActionCommand"]=
        Narrow(actionCandidate->observation.process.commandLine);
      state["observedUninstallActionArguments"]=
        CommandArgumentsJson(actionCandidate->observation.process.commandLine);
      state["observedUninstallActionSwitches"]=
        CommandSwitchesJson(actionCandidate->observation.process.commandLine);
      state["observedUninstallActionCaptureMethod"]=
        actionCandidate->observation.captureMethod;
      state["observedUninstallActionCorrelationScore"]=
        actionCandidate->score;
      state["observedUninstallActionCorrelationReasons"]=
        actionCandidate->reasons;
    }else{
      state.erase("observedUninstallActionCommand");
      state.erase("observedUninstallActionArguments");
      state.erase("observedUninstallActionSwitches");
      state.erase("observedUninstallActionCaptureMethod");
      state.erase("observedUninstallActionCorrelationScore");
      state.erase("observedUninstallActionCorrelationReasons");
    }

    const bool exactIdentity=HasExactIdentityEvidence(best.reasons);
    const bool exactCommand=
      exactIdentity&&best.score>=120&&!best.observation.process.commandLine.empty();

    if(exactCommand){
      state["observedUninstallCommand"]=Narrow(best.observation.process.commandLine);
      state["observedUninstallArguments"]=CommandArgumentsJson(best.observation.process.commandLine);
      state["observedUninstallSwitches"]=CommandSwitchesJson(best.observation.process.commandLine);
      state["uninstallObservationStatus"]="exact_process_command_observed_removal_confirmed";
    }else{
      state.erase("observedUninstallCommand");
      state.erase("observedUninstallArguments");
      state.erase("observedUninstallSwitches");
      state["uninstallObservationStatus"]="process_activity_observed_removal_confirmed";
    }

    std::wstringstream line;
    line<<L"UNINSTALL CORRELATED ["<<best.score<<L"] "<<app.name<<L" <- "<<best.observation.process.name;
    if(!best.observation.process.commandLine.empty())line<<L" | "<<best.observation.process.commandLine;
    AppendLog(line.str(),{
      {"timestamp",NowIso()},
      {"type","uninstall_process_correlated"},
      {"applicationId",appId},
      {"score",best.score},
      {"process",observed.empty()?json::object():observed.front()}
    });
  }else{
    state["uninstallObservationStatus"]="registered_recipe_observed_removal_confirmed";
  }

  json portalEvidence={
    {"schemaVersion",1},
    {"displayName",Narrow(app.name)},
    {"version",Narrow(app.version)},
    {"publisher",Narrow(app.publisher)},
    {"technology",Narrow(UninstallTechnology(app))},
    {"productCode",Narrow(app.productCode)},
    {"registeredCommand",Narrow(app.uninstallString)},
    {"registeredQuietCommand",Narrow(app.quietUninstallString)},
    {"candidateSilentCommand",Narrow(CandidateSilentRecipe(app))},
    {"removalConfirmed",true},
    {"removalConfirmedAt",state.value("lastUninstalledAt",std::string())},
    {"observationStatus",state.value("uninstallObservationStatus",std::string())},
    {"observedProcesses",observed}
  };
  if(state.contains("observedUninstallCommand"))
    portalEvidence["observedExactCommand"]=state["observedUninstallCommand"];
  if(state.contains("observedUninstallArguments"))
    portalEvidence["observedExactArguments"]=state["observedUninstallArguments"];
  if(state.contains("observedUninstallSwitches"))
    portalEvidence["observedExactSwitches"]=state["observedUninstallSwitches"];
  if(state.contains("observedUninstallCaptureMethod"))
    portalEvidence["captureMethod"]=state["observedUninstallCaptureMethod"];
  if(state.contains("observedUninstallActionCommand"))
    portalEvidence["observedActionCommand"]=state["observedUninstallActionCommand"];
  if(state.contains("observedUninstallActionArguments"))
    portalEvidence["observedActionArguments"]=state["observedUninstallActionArguments"];
  if(state.contains("observedUninstallActionSwitches"))
    portalEvidence["observedActionSwitches"]=state["observedUninstallActionSwitches"];
  if(state.contains("observedUninstallActionCaptureMethod"))
    portalEvidence["observedActionCaptureMethod"]=state["observedUninstallActionCaptureMethod"];
  if(state.contains("observedUninstallActionCorrelationScore"))
    portalEvidence["observedActionCorrelationScore"]=state["observedUninstallActionCorrelationScore"];
  if(state.contains("observedUninstallActionCorrelationReasons"))
    portalEvidence["observedActionCorrelationReasons"]=state["observedUninstallActionCorrelationReasons"];
  if(state.contains("observedUninstallCorrelationScore"))
    portalEvidence["correlationScore"]=state["observedUninstallCorrelationScore"];
  if(state.contains("observedUninstallCorrelationReasons"))
    portalEvidence["correlationReasons"]=state["observedUninstallCorrelationReasons"];

  if(!state.contains("patchingEvidence")||!state["patchingEvidence"].is_object())
    state["patchingEvidence"]=json::object();
  state["patchingEvidence"]["uninstall"]=std::move(portalEvidence);
  SaveState();
}

void Tick(bool manual){
  const auto tickNow=std::chrono::steady_clock::now();

  // Process creation is event-driven. The 750ms loop only drains the event
  // queue and takes a fallback snapshot for anything the watcher could not
  // resolve.
  while(!gProcessJournal.empty()&&tickNow-gProcessJournal.front().seen>std::chrono::minutes(5))
    gProcessJournal.pop_front();

  auto observationQuality=[](const ProcessObservation& observation){
    int quality=0;
    if(!observation.process.commandLine.empty())quality+=100;
    if(!observation.process.path.empty())quality+=20;
    if(observation.captureMethod=="KernelETW_ProcessStart")quality+=80;
    else if(observation.captureMethod=="Security4688_CommandLineAudit")quality+=60;
    else if(observation.captureMethod=="Win32_ProcessStartTrace")quality+=30;
    else if(observation.captureMethod=="snapshot_fallback")quality+=10;
    return quality;
  };

  std::map<DWORD,ProcessObservation> mergedStarts;
  auto eventStarts=DrainProcessStartEvents();
  for(auto& observation:eventStarts){
    const DWORD pid=observation.process.pid;
    if(!pid)continue;
    auto existing=mergedStarts.find(pid);
    if(existing==mergedStarts.end()||
       observationQuality(observation)>observationQuality(existing->second)){
      mergedStarts[pid]=std::move(observation);
    }
  }

  std::set<DWORD> eventPids;
  for(auto&[pid,observation]:mergedStarts){
    eventPids.insert(pid);
    gProcessJournal.push_back(observation);
    while(gProcessJournal.size()>1500)gProcessJournal.pop_front();

    const auto& p=observation.process;
    std::wstring detail=L"PROCESS START ["+Widen(observation.captureMethod)+L"] "+
      std::to_wstring(p.pid)+L" ppid="+std::to_wstring(p.parentPid)+L" "+p.name;
    if(!p.commandLine.empty())detail+=L" | "+p.commandLine;
    AppendLog(detail,{
      {"timestamp",observation.timestamp},
      {"type","process_started"},
      {"captureMethod",observation.captureMethod},
      {"pid",p.pid},
      {"parentPid",p.parentPid},
      {"name",Narrow(p.name)},
      {"path",Narrow(p.path)},
      {"commandLine",Narrow(p.commandLine)},
      {"arguments",CommandArgumentsJson(p.commandLine)},
      {"switches",CommandSwitchesJson(p.commandLine)}
    });
  }

  auto procs=CaptureProcs();
  for(const auto&[pid,p]:procs){
    if(gProcs.find(pid)!=gProcs.end()||eventPids.count(pid))continue;
    const std::string timestamp=NowIso();
    ProcessObservation observation{tickNow,timestamp,p,"snapshot_fallback"};
    gProcessJournal.push_back(observation);
    while(gProcessJournal.size()>1500)gProcessJournal.pop_front();

    std::wstring detail=L"PROCESS START [snapshot fallback] "+std::to_wstring(pid)+L" ppid="+std::to_wstring(p.parentPid)+L" "+p.name;
    if(!p.commandLine.empty())detail+=L" | "+p.commandLine;
    AppendLog(detail,{
      {"timestamp",timestamp},
      {"type","process_started"},
      {"captureMethod","snapshot_fallback"},
      {"pid",pid},
      {"parentPid",p.parentPid},
      {"name",Narrow(p.name)},
      {"path",Narrow(p.path)},
      {"commandLine",Narrow(p.commandLine)}
    });
  }

  for(const auto&[pid,p]:gProcs)if(procs.find(pid)==procs.end()){
    AppendLog(L"PROCESS EXIT "+std::to_wstring(pid)+L" "+p.name,{
      {"timestamp",NowIso()},
      {"type","process_exited"},
      {"pid",pid},
      {"name",Narrow(p.name)},
      {"path",Narrow(p.path)},
      {"commandLine",Narrow(p.commandLine)}
    });
  }
  gProcs=std::move(procs);

  auto apps=CaptureApps();
  for(const auto&[id,e]:apps){
    auto old=gApps.find(id);
    if(old==gApps.end()){
      RecordAppState(e,true,"installed");
      AppendLog(L"INSTALLED + "+e.name+L" "+e.version,{
        {"timestamp",NowIso()},{"type","installed"},{"entry",AppJson(e)}
      });
      continue;
    }
    const bool versionChanged=old->second.version!=e.version;
    const bool identityChanged=versionChanged||old->second.publisher!=e.publisher||
      old->second.uninstallString!=e.uninstallString||
      old->second.quietUninstallString!=e.quietUninstallString||
      old->second.installLocation!=e.installLocation;
    if(identityChanged){
      RecordAppState(e,true,versionChanged?"version_changed":"identity_changed");
      std::wstring label=versionChanged
        ? L"VERSION * "+e.name+L" "+old->second.version+L" -> "+e.version
        : L"IDENTITY * "+e.name+L" "+e.version;
      AppendLog(label,{
        {"timestamp",NowIso()},
        {"type",versionChanged?"version_changed":"identity_changed"},
        {"before",AppJson(old->second)},
        {"after",AppJson(e)}
      });
    }
  }

  for(const auto&[id,e]:gApps)if(apps.find(id)==apps.end()){
    const std::string appId=Narrow(id);
    RecordAppState(e,false,"uninstalled");
    AppendLog(L"UNINSTALLED - "+e.name+L" "+e.version,{
      {"timestamp",NowIso()},{"type","uninstalled"},{"entry",AppJson(e)}
    });

    const auto recipe=RegisteredRecipe(e);
    if(!recipe.empty())AppendLog(
      L"REGISTERED UNINSTALL RECIPE ["+UninstallTechnology(e)+L"] "+recipe,
      {{"timestamp",NowIso()},{"type","learned_uninstall_recipe"},{"technology",Narrow(UninstallTechnology(e))},{"command",Narrow(recipe)},{"entry",AppJson(e)}});

    const auto candidate=CandidateSilentRecipe(e);
    if(!candidate.empty()&&candidate!=recipe)AppendLog(
      L"CANDIDATE SILENT RECIPE "+candidate,
      {{"timestamp",NowIso()},{"type","candidate_silent_uninstall_recipe"},{"command",Narrow(candidate)},{"entry",AppJson(e)}});

    CorrelateRemovalWithRecentProcesses(e,appId);
  }
  gApps=std::move(apps);

  const int paneBeforeRefresh=gSelectedPane;
  const std::string selectedBeforeRefresh=gSelectedId;
  PopulateLists();

  bool selectionStillPresent=false;
  if(gSelectedPane==1&&!gSelectedId.empty()){
    for(size_t i=0;i<gInstalledIds.size();++i){
      if(Narrow(gInstalledIds[i])==gSelectedId){selectionStillPresent=true;break;}
    }
  }else if(gSelectedPane==2&&!gSelectedId.empty()){
    for(size_t i=0;i<gHistoryIds.size();++i){
      if(gHistoryIds[i]==gSelectedId){selectionStillPresent=true;break;}
    }
  }

  if(!selectionStillPresent){
    gSelectedPane=0;
    gSelectedId.clear();
    RefreshDetails();
  }else if(paneBeforeRefresh!=gSelectedPane||selectedBeforeRefresh!=gSelectedId){
    if(gSelectedPane==1){
      for(size_t i=0;i<gInstalledIds.size();++i)
        if(Narrow(gInstalledIds[i])==gSelectedId){ShowInstalledDetails(i);break;}
    }else if(gSelectedPane==2){
      for(size_t i=0;i<gHistoryIds.size();++i)
        if(gHistoryIds[i]==gSelectedId){ShowHistoryDetails(i);break;}
    }
  }

  RenderActivity();

  std::wstringstream st;
  st<<L"Installed: "<<SendMessageW(gInstalled,LB_GETCOUNT,0,0)
    <<L"   Confirmed removals: "<<SendMessageW(gHistory,LB_GETCOUNT,0,0)
    <<L"   Observed identities: "<<gApps.size()
    <<L"   Process journal: "<<gProcessJournal.size()
    <<L"   Phase: "<<gOpt.phase;
  if(manual)st<<L"   Snapshot saved";
  SetWindowTextW(gStatus,st.str().c_str());
}
void ExportEvidence(){
  json installed=json::array();for(const auto&[_,e]:gApps)installed.push_back(AppJson(e));
  json out={{"schemaVersion",1},{"observerVersion","0.1.9"},{"jobId",Narrow(gOpt.jobId)},{"application",Narrow(gOpt.application)},{"phase",Narrow(gOpt.phase)},{"exportedAt",NowIso()},{"installed",installed},{"state",gState}};
  std::ofstream f(gOpt.root/L"jobs"/gOpt.jobId/L"summary.json",std::ios::binary|std::ios::trunc);f<<out.dump(2);
}
void Layout(HWND w){
  RECT r{};GetClientRect(w,&r);
  const int W=r.right,H=r.bottom,m=16,gap=10;
  const int head=62,bannerH=34,labelH=24,listsH=180,detailH=210,btn=34,status=24;

  MoveWindow(gHeader,m,m,W-2*m,head,TRUE);
  int y=m+head+8;
  MoveWindow(gSafetyBanner,m,y,W-2*m,bannerH,TRUE);
  y+=bannerH+12;

  const int half=(W-3*m)/2;
  MoveWindow(gInstalledLabel,m,y,half,labelH,TRUE);
  MoveWindow(gHistoryLabel,m*2+half,y,half,labelH,TRUE);
  y+=labelH+4;
  MoveWindow(gInstalled,m,y,half,listsH,TRUE);
  MoveWindow(gHistory,m*2+half,y,half,listsH,TRUE);
  y+=listsH+12;

  MoveWindow(gEventsLabel,m,y,W-2*m,labelH,TRUE);
  y+=labelH+4;
  const int eventH=std::max(105,H-y-detailH-labelH-btn-status-gap*4-m);
  MoveWindow(gEvents,m,y,W-2*m,eventH,TRUE);
  y+=eventH+12;

  MoveWindow(gDetailsLabel,m,y,W-2*m,labelH,TRUE);
  y+=labelH+4;
  MoveWindow(gDetails,m,y,W-2*m,detailH,TRUE);
  y+=detailH+12;

  MoveWindow(gSnapshot,m,y,156,btn,TRUE);
  MoveWindow(gExport,m+166,y,156,btn,TRUE);
  MoveWindow(gStop,W-m-146,y,146,btn,TRUE);
  MoveWindow(gStatus,m,y+btn+4,W-2*m,status,TRUE);
}
LRESULT CALLBACK Proc(HWND w,UINT msg,WPARAM wp,LPARAM lp){
  if(msg==WM_SIZE){Layout(w);return 0;}if(msg==WM_TIMER&&wp==kTimerId&&!gStopping){Tick(false);return 0;}
  if(msg==WM_COMMAND){
    if(LOWORD(wp)==1101&&HIWORD(wp)==LBN_SELCHANGE){
      const LRESULT i=SendMessageW(gInstalled,LB_GETCURSEL,0,0);
      if(i!=LB_ERR&&(size_t)i<gInstalledIds.size()){
        gSelectedPane=1;gSelectedId=Narrow(gInstalledIds[(size_t)i]);
        SendMessageW(gHistory,LB_SETCURSEL,(WPARAM)-1,0);
        ShowInstalledDetails((size_t)i);
      }
      return 0;
    }
    if(LOWORD(wp)==1102&&HIWORD(wp)==LBN_SELCHANGE){
      const LRESULT i=SendMessageW(gHistory,LB_GETCURSEL,0,0);
      if(i!=LB_ERR&&(size_t)i<gHistoryIds.size()){
        gSelectedPane=2;gSelectedId=gHistoryIds[(size_t)i];
        SendMessageW(gInstalled,LB_SETCURSEL,(WPARAM)-1,0);
        ShowHistoryDetails((size_t)i);
      }
      return 0;
    }
    if(LOWORD(wp)==1001){Tick(true);ExportEvidence();return 0;}
    if(LOWORD(wp)==1002){ExportEvidence();AppendLog(L"Evidence exported.",{{"timestamp",NowIso()},{"type","export"}});return 0;}
    if(LOWORD(wp)==1003){SendMessageW(w,WM_CLOSE,0,0);return 0;}
  }
  if(msg==WM_CTLCOLORSTATIC){
    HDC dc=(HDC)wp;HWND ctrl=(HWND)lp;SetTextColor(dc,kText);SetBkMode(dc,TRANSPARENT);
    if(ctrl==gSafetyBanner){SetTextColor(dc,kBannerText);SetBkMode(dc,OPAQUE);SetBkColor(dc,kBannerBg);return (LRESULT)gBannerBrush;}
    return (LRESULT)gWindowBrush;
  }
  if(msg==WM_CTLCOLOREDIT||msg==WM_CTLCOLORLISTBOX){
    HDC dc=(HDC)wp;SetTextColor(dc,kText);SetBkColor(dc,kPanelBg);return (LRESULT)gPanelBrush;
  }
  if(msg==WM_CLOSE){
    gStopping=true;
    gProcessWatcherStop.store(true);
    StopKernelProcessEtwSession();
    StopSecurityProcessSubscription();
    DisableQualificationProcessAudit();
    KillTimer(w,kTimerId);
    Tick(false);
    ExportEvidence();
    DestroyWindow(w);
    return 0;
  }
  if(msg==WM_DESTROY){PostQuitMessage(0);return 0;}
  return DefWindowProcW(w,msg,wp,lp);
}
HFONT Font(int sz,bool bold=false,const wchar_t*face=L"Segoe UI"){return CreateFontW(-sz,0,0,0,bold?FW_SEMIBOLD:FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,face);}
}

int WINAPI wWinMain(HINSTANCE h,HINSTANCE,PWSTR,int show){
  HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
  if(SUCCEEDED(com))CoInitializeSecurity(nullptr,-1,nullptr,nullptr,RPC_C_AUTHN_LEVEL_DEFAULT,RPC_C_IMP_LEVEL_IMPERSONATE,nullptr,EOAC_NONE,nullptr);
  HANDLE singleton=CreateMutexW(nullptr,TRUE,L"Local\\Hi5CentralQualificationObserver");
  if(singleton&&GetLastError()==ERROR_ALREADY_EXISTS){
    MessageBoxW(nullptr,L"Hi5Central Qualification Observer is already running in this desktop session.",L"Hi5Central Qualification Observer",MB_OK|MB_ICONINFORMATION);
    CloseHandle(singleton);if(SUCCEEDED(com))CoUninitialize();return 3;
  }
  gOpt=ParseOptions();
  std::error_code ec;
  std::filesystem::create_directories(gOpt.root/L"jobs"/gOpt.jobId,ec);
  const bool auditRecoveryOk=RecoverStaleProcessAuditSettings(gOpt.root);
  LoadState();
  BackfillStateRecipes();
  gLog.open(gOpt.root/L"jobs"/gOpt.jobId/L"observer.log",std::ios::app);
  gJsonl.open(gOpt.root/L"jobs"/gOpt.jobId/L"events.jsonl",std::ios::binary|std::ios::app);

  gWindowBrush=CreateSolidBrush(kWindowBg);
  gPanelBrush=CreateSolidBrush(kPanelBg);
  gBannerBrush=CreateSolidBrush(kBannerBg);

  WNDCLASSEXW wc{};wc.cbSize=sizeof(wc);wc.hInstance=h;wc.lpfnWndProc=Proc;wc.lpszClassName=kClassName;wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.hIcon=LoadIcon(nullptr,IDI_APPLICATION);wc.hbrBackground=gWindowBrush;RegisterClassExW(&wc);
  std::wstring title=L"Hi5Central Qualification Observer - "+gOpt.application;
  HWND w=CreateWindowExW(0,kClassName,title.c_str(),WS_OVERLAPPEDWINDOW|WS_VISIBLE,CW_USEDEFAULT,CW_USEDEFAULT,1220,900,nullptr,nullptr,h,nullptr);if(!w)return 2;

  HFONT titleF=Font(21,true,L"Segoe UI");
  HFONT sectionF=Font(14,true,L"Segoe UI");
  HFONT uiF=Font(15,false,L"Segoe UI");
  HFONT smallF=Font(13,false,L"Segoe UI");
  HFONT mono=Font(13,false,L"Consolas");

  std::wstringstream head;
  head<<L"Hi5Central Qualification Observer\r\n"<<gOpt.application<<L"   |   "<<gOpt.phase<<L"   |   "<<gOpt.jobId;
  gHeader=CreateWindowExW(0,L"STATIC",head.str().c_str(),WS_CHILD|WS_VISIBLE|SS_LEFT,0,0,0,0,w,nullptr,h,nullptr);
  SendMessageW(gHeader,WM_SETFONT,(WPARAM)titleF,TRUE);

  gSafetyBanner=CreateWindowExW(0,L"STATIC",L"  OBSERVER MODE   |   Selecting an application is read-only. This window never uninstalls software on selection.",WS_CHILD|WS_VISIBLE|SS_CENTERIMAGE,0,0,0,0,w,(HMENU)1205,h,nullptr);
  SendMessageW(gSafetyBanner,WM_SETFONT,(WPARAM)smallF,TRUE);

  gInstalledLabel=CreateWindowExW(0,L"STATIC",L"INSTALLED / AVAILABLE TO TEST",WS_CHILD|WS_VISIBLE|SS_LEFT,0,0,0,0,w,(HMENU)1201,h,nullptr);
  gHistoryLabel=CreateWindowExW(0,L"STATIC",L"CONFIRMED REMOVAL HISTORY",WS_CHILD|WS_VISIBLE|SS_LEFT,0,0,0,0,w,(HMENU)1202,h,nullptr);
  gEventsLabel=CreateWindowExW(0,L"STATIC",L"LIVE ACTIVITY",WS_CHILD|WS_VISIBLE|SS_LEFT,0,0,0,0,w,(HMENU)1203,h,nullptr);
  gDetailsLabel=CreateWindowExW(0,L"STATIC",L"APPLICATION DETAILS / LEARNED RECIPE",WS_CHILD|WS_VISIBLE|SS_LEFT,0,0,0,0,w,(HMENU)1204,h,nullptr);
  for(HWND label:{gInstalledLabel,gHistoryLabel,gEventsLabel,gDetailsLabel})SendMessageW(label,WM_SETFONT,(WPARAM)sectionF,TRUE);

  gInstalled=CreateWindowExW(WS_EX_STATICEDGE,L"LISTBOX",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,0,0,0,0,w,(HMENU)1101,h,nullptr);
  gHistory=CreateWindowExW(WS_EX_STATICEDGE,L"LISTBOX",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,0,0,0,0,w,(HMENU)1102,h,nullptr);
  SendMessageW(gInstalled,WM_SETFONT,(WPARAM)uiF,TRUE);SendMessageW(gHistory,WM_SETFONT,(WPARAM)uiF,TRUE);

  gEvents=CreateWindowExW(WS_EX_STATICEDGE,L"LISTBOX",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|WS_HSCROLL|LBS_NOINTEGRALHEIGHT|LBS_NOSEL,0,0,0,0,w,nullptr,h,nullptr);
  gDetails=CreateWindowExW(WS_EX_STATICEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY,0,0,0,0,w,nullptr,h,nullptr);
  SendMessageW(gEvents,WM_SETFONT,(WPARAM)mono,TRUE);SendMessageW(gDetails,WM_SETFONT,(WPARAM)mono,TRUE);
  SendMessageW(gDetails,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELPARAM(10,10));

  gSnapshot=CreateWindowExW(0,L"BUTTON",L"Capture snapshot",WS_CHILD|WS_VISIBLE|BS_FLAT,0,0,0,0,w,(HMENU)1001,h,nullptr);
  gExport=CreateWindowExW(0,L"BUTTON",L"Export evidence",WS_CHILD|WS_VISIBLE|BS_FLAT,0,0,0,0,w,(HMENU)1002,h,nullptr);
  gStop=CreateWindowExW(0,L"BUTTON",L"Stop observer",WS_CHILD|WS_VISIBLE|BS_FLAT,0,0,0,0,w,(HMENU)1003,h,nullptr);
  SendMessageW(gSnapshot,WM_SETFONT,(WPARAM)uiF,TRUE);SendMessageW(gExport,WM_SETFONT,(WPARAM)uiF,TRUE);SendMessageW(gStop,WM_SETFONT,(WPARAM)uiF,TRUE);

  gStatus=CreateWindowExW(0,L"STATIC",L"Starting observer...",WS_CHILD|WS_VISIBLE|SS_LEFT,0,0,0,0,w,nullptr,h,nullptr);
  SendMessageW(gStatus,WM_SETFONT,(WPARAM)smallF,TRUE);
  Layout(w);
  for(auto it=gState["apps"].begin();it!=gState["apps"].end();++it)if(it.value().is_object())it.value()["currentlyInstalled"]=false;
  gApps=CaptureApps();for(const auto&[_,e]:gApps){
    auto id=Narrow(e.id);auto&s=gState["apps"][id];
    if(!s.is_object()){RecordAppState(e,true,"baseline_present");}
    else{
      s["currentlyInstalled"]=true;s["lastSeenAt"]=NowIso();s["displayName"]=Narrow(e.name);s["version"]=Narrow(e.version);s["publisher"]=Narrow(e.publisher);
      s["scope"]=Narrow(e.scope);s["registryKey"]=Narrow(e.key);s["productCode"]=Narrow(e.productCode);s["uninstallString"]=Narrow(e.uninstallString);
      s["quietUninstallString"]=Narrow(e.quietUninstallString);s["installLocation"]=Narrow(e.installLocation);s["systemComponent"]=e.systemComponent;
      s["parentKeyName"]=Narrow(e.parentKeyName);s["testable"]=IsTestableApplication(e);
    }
  }SaveState();

  gProcessWatcherStop.store(false);
  gEtwProcessReady.store(false);
  gEtwProcessError.store(ERROR_SUCCESS);
  gEtwProcessWatcher=std::thread(KernelProcessEtwWatcherLoop);

  for(int i=0;i<40&&!gEtwProcessReady.load()&&
      gEtwProcessError.load()==ERROR_SUCCESS;++i){
    Sleep(25);
  }
  const bool kernelEtwReady=gEtwProcessReady.load();

  bool security4688Ready=false;
  if(!kernelEtwReady&&auditRecoveryOk&&
     EnableQualificationProcessAudit(gOpt.root)){
    security4688Ready=StartSecurityProcessSubscription();
    if(!security4688Ready)DisableQualificationProcessAudit();
  }

  gProcs=CaptureProcs();
  gProcessWatcher=std::thread(ProcessStartWatcherLoop);
  PopulateLists();
  RefreshDetails();

  json captureSources=json::array();
  if(kernelEtwReady)captureSources.push_back("KernelETW_ProcessStart");
  if(security4688Ready)captureSources.push_back("Security4688_CommandLineAudit");
  captureSources.push_back("Win32_ProcessStartTrace");
  captureSources.push_back("snapshot_fallback");

  std::wstring startMessage;
  if(kernelEtwReady){
    startMessage=
      L"Observer started. Exact command-line capture uses kernel ETW with WMI/snapshot fallback.";
  }else if(security4688Ready){
    startMessage=
      L"Observer started. Kernel ETW is unavailable; Security 4688 command-line capture is active with WMI/snapshot fallback.";
  }else{
    startMessage=
      L"Observer started. Kernel ETW and Security 4688 are unavailable; WMI/snapshot fallback is active.";
  }

  AppendLog(
    startMessage,
    {{"timestamp",NowIso()},
     {"type","observer_started"},
     {"jobId",Narrow(gOpt.jobId)},
     {"application",Narrow(gOpt.application)},
     {"phase",Narrow(gOpt.phase)},
     {"processCaptureSources",captureSources},
     {"kernelEtwReady",kernelEtwReady},
     {"kernelEtwError",gEtwProcessError.load()},
     {"security4688Ready",security4688Ready},
     {"auditRecoveryOk",auditRecoveryOk}});
  SetTimer(w,kTimerId,kTimerMs,nullptr);
  ShowWindow(w,SW_SHOW);
  SetForegroundWindow(w);
  UpdateWindow(w);
  MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
  gProcessWatcherStop.store(true);
  StopKernelProcessEtwSession();
  StopSecurityProcessSubscription();
  DisableQualificationProcessAudit();
  if(gEtwProcessWatcher.joinable())gEtwProcessWatcher.join();
  if(gProcessWatcher.joinable())gProcessWatcher.join();
  if(singleton)CloseHandle(singleton);
  if(SUCCEEDED(com))CoUninitialize();
  return(int)msg.wParam;
}
