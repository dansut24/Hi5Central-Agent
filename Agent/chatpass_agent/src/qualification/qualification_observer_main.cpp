#include <windows.h>
#include <tlhelp32.h>
#include <wbemidl.h>
#include <shlobj.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
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
std::vector<std::wstring> gInstalledIds;
std::vector<std::string> gHistoryIds;
std::string gSelectedId;
int gSelectedPane=0; // 0=none, 1=installed, 2=history
std::map<std::string,std::chrono::steady_clock::time_point> gRecentlyRemoved;
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
json AppJson(const AppEntry&e){
  return{{"id",Narrow(e.id)},{"scope",Narrow(e.scope)},{"registryKey",Narrow(e.key)},{"displayName",Narrow(e.name)},{"version",Narrow(e.version)},
    {"publisher",Narrow(e.publisher)},{"productCode",Narrow(e.productCode)},{"windowsInstaller",e.windowsInstaller},
    {"systemComponent",e.systemComponent},{"parentKeyName",Narrow(e.parentKeyName)},{"testable",IsTestableApplication(e)},
    {"uninstallString",Narrow(e.uninstallString)},{"quietUninstallString",Narrow(e.quietUninstallString)},{"installLocation",Narrow(e.installLocation)}};
}
void AppendLog(const std::wstring& text,const json& ev){
  std::wstring line=L"["+NowLocal()+L"] "+text+L"\r\n";
  if(gEvents){int n=GetWindowTextLengthW(gEvents);SendMessageW(gEvents,EM_SETSEL,n,n);SendMessageW(gEvents,EM_REPLACESEL,FALSE,(LPARAM)line.c_str());SendMessageW(gEvents,EM_SCROLLCARET,0,0);}
  if(gLog.is_open()){gLog<<line;gLog.flush();} if(gJsonl.is_open()){gJsonl<<ev.dump()<<"\n";gJsonl.flush();}
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
  SetWindowTextW(gDetails,s.str().c_str());
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
  const auto observed=Widen(s0.value("observedUninstallCommand",std::string()));if(!observed.empty())s<<L"Observed process command: "<<observed<<L"\r\n";
  s<<L"Removal confirmed: "<<Widen(s0.value("lastUninstalledAt",std::string()))<<L"\r\n";
  s<<L"Recipe status: "<<Widen(s0.value("uninstallObservationStatus",std::string("observed")) )<<L"\r\n";
  if(s0.contains("observedUninstallProcesses")&&s0["observedUninstallProcesses"].is_array()){
    s<<L"\r\nObserved uninstall process commands:\r\n";
    for(const auto&p:s0["observedUninstallProcesses"]){
      const auto cmd=Widen(p.value("commandLine",std::string()));const auto name=Widen(p.value("name",std::string()));
      s<<L"  "<<name;if(!cmd.empty())s<<L" | "<<cmd;s<<L"\r\n";
    }
  }
  SetWindowTextW(gDetails,s.str().c_str());
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
  SetWindowTextW(gDetails,s.str().c_str());
}
void Tick(bool manual){
  auto apps=CaptureApps();
  for(const auto&[id,e]:apps){
    auto old=gApps.find(id);
    if(old==gApps.end()){
      RecordAppState(e,true,"installed");
      AppendLog(L"INSTALLED + "+e.name+L" "+e.version,{{"timestamp",NowIso()},{"type","installed"},{"entry",AppJson(e)}});
      continue;
    }
    const bool versionChanged=old->second.version!=e.version;
    const bool identityChanged=versionChanged||old->second.publisher!=e.publisher||old->second.uninstallString!=e.uninstallString||
      old->second.quietUninstallString!=e.quietUninstallString||old->second.installLocation!=e.installLocation;
    if(identityChanged){
      RecordAppState(e,true,versionChanged?"version_changed":"identity_changed");
      std::wstring label=versionChanged
        ? L"VERSION * "+e.name+L" "+old->second.version+L" -> "+e.version
        : L"IDENTITY * "+e.name+L" "+e.version;
      AppendLog(label,{{"timestamp",NowIso()},{"type",versionChanged?"version_changed":"identity_changed"},{"before",AppJson(old->second)},{"after",AppJson(e)}});
    }
  }
  for(const auto&[id,e]:gApps)if(apps.find(id)==apps.end()){
    RecordAppState(e,false,"uninstalled");
    gRecentlyRemoved[Narrow(id)]=std::chrono::steady_clock::now();
    AppendLog(L"UNINSTALLED - "+e.name+L" "+e.version,{{"timestamp",NowIso()},{"type","uninstalled"},{"entry",AppJson(e)}});
    const auto recipe=RegisteredRecipe(e);
    if(!recipe.empty())AppendLog(
      L"LEARNED UNINSTALL RECIPE ["+UninstallTechnology(e)+L"] "+recipe,
      {{"timestamp",NowIso()},{"type","learned_uninstall_recipe"},{"technology",Narrow(UninstallTechnology(e))},{"command",Narrow(recipe)},{"entry",AppJson(e)}});
    const auto candidate=CandidateSilentRecipe(e);
    if(!candidate.empty()&&candidate!=recipe)AppendLog(
      L"CANDIDATE SILENT RECIPE "+candidate,
      {{"timestamp",NowIso()},{"type","candidate_silent_uninstall_recipe"},{"command",Narrow(candidate)},{"entry",AppJson(e)}});
  }
  gApps=std::move(apps);

  auto procs=CaptureProcs();
  for(const auto&[pid,p]:procs)if(gProcs.find(pid)==gProcs.end()){
    std::wstring detail=L"PROCESS + "+std::to_wstring(pid)+L" ppid="+std::to_wstring(p.parentPid)+L" "+p.name;
    if(!p.commandLine.empty())detail+=L" | "+p.commandLine;
    const json processEvent={{"timestamp",NowIso()},{"type","process_started"},{"pid",pid},{"parentPid",p.parentPid},{"name",Narrow(p.name)},{"path",Narrow(p.path)},{"commandLine",Narrow(p.commandLine)}};
    AppendLog(detail,processEvent);

    const auto now=std::chrono::steady_clock::now();
    for(auto it=gRecentlyRemoved.begin();it!=gRecentlyRemoved.end();){
      if(now-it->second>std::chrono::seconds(120)){it=gRecentlyRemoved.erase(it);continue;}
      auto stateIt=gState["apps"].find(it->first);
      if(stateIt==gState["apps"].end()){++it;continue;}
      auto& appState=*stateIt;
      const auto reg=Lower(Widen(appState.value("registeredUninstallCommand",std::string())));
      const auto quiet=Lower(Widen(appState.value("registeredQuietUninstallCommand",std::string())));
      const auto procName=Lower(p.name);
      const auto procPath=Lower(p.path);
      bool related=(!procName.empty()&&(reg.find(procName)!=std::wstring::npos||quiet.find(procName)!=std::wstring::npos));
      if(!related&&!procPath.empty())related=reg.find(procPath)!=std::wstring::npos||quiet.find(procPath)!=std::wstring::npos;
      if(related){
        if(!appState.contains("observedUninstallProcesses")||!appState["observedUninstallProcesses"].is_array())appState["observedUninstallProcesses"]=json::array();
        appState["observedUninstallProcesses"].push_back(processEvent);
        while(appState["observedUninstallProcesses"].size()>20)appState["observedUninstallProcesses"].erase(appState["observedUninstallProcesses"].begin());
        if(!p.commandLine.empty()){
          appState["observedUninstallCommand"]=Narrow(p.commandLine);
          appState["uninstallObservationStatus"]="process_command_observed_removal_confirmed";
          AppendLog(L"UNINSTALL PROCESS CORRELATED -> "+p.commandLine,{{"timestamp",NowIso()},{"type","uninstall_process_correlated"},{"applicationId",it->first},{"process",processEvent}});
        }
        SaveState();
      }
      ++it;
    }
  }
  for(const auto&[pid,p]:gProcs)if(procs.find(pid)==procs.end())AppendLog(L"PROCESS - "+std::to_wstring(pid)+L" "+p.name,{{"timestamp",NowIso()},{"type","process_exited"},{"pid",pid},{"name",Narrow(p.name)},{"path",Narrow(p.path)},{"commandLine",Narrow(p.commandLine)}});
  gProcs=std::move(procs);
  PopulateLists();

  bool restored=false;
  if(gSelectedPane==1&&!gSelectedId.empty()){
    for(size_t i=0;i<gInstalledIds.size();++i){
      if(Narrow(gInstalledIds[i])==gSelectedId){ShowInstalledDetails(i);restored=true;break;}
    }
  } else if(gSelectedPane==2&&!gSelectedId.empty()){
    for(size_t i=0;i<gHistoryIds.size();++i){
      if(gHistoryIds[i]==gSelectedId){ShowHistoryDetails(i);restored=true;break;}
    }
  }
  if(!restored)RefreshDetails();

  std::wstringstream st;st<<L"Installed: "<<SendMessageW(gInstalled,LB_GETCOUNT,0,0)<<L"   Confirmed removals: "<<SendMessageW(gHistory,LB_GETCOUNT,0,0)<<L"   Observed identities: "<<gApps.size()<<L"   Phase: "<<gOpt.phase;
  if(manual)st<<L"   Snapshot saved";SetWindowTextW(gStatus,st.str().c_str());
}
void ExportEvidence(){
  json installed=json::array();for(const auto&[_,e]:gApps)installed.push_back(AppJson(e));
  json out={{"schemaVersion",1},{"observerVersion","0.1.3"},{"jobId",Narrow(gOpt.jobId)},{"application",Narrow(gOpt.application)},{"phase",Narrow(gOpt.phase)},{"exportedAt",NowIso()},{"installed",installed},{"state",gState}};
  std::ofstream f(gOpt.root/L"jobs"/gOpt.jobId/L"summary.json",std::ios::binary|std::ios::trunc);f<<out.dump(2);
}
void Layout(HWND w){
  RECT r{};GetClientRect(w,&r);
  const int W=r.right,H=r.bottom,m=16,gap=10;
  const int head=62,bannerH=34,labelH=24,listsH=190,detailH=176,btn=34,status=24;

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
  if(msg==WM_CLOSE){gStopping=true;KillTimer(w,kTimerId);Tick(false);ExportEvidence();DestroyWindow(w);return 0;}
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
  gOpt=ParseOptions();std::error_code ec;std::filesystem::create_directories(gOpt.root/L"jobs"/gOpt.jobId,ec);LoadState();BackfillStateRecipes();
  gLog.open(gOpt.root/L"jobs"/gOpt.jobId/L"observer.log",std::ios::app);gJsonl.open(gOpt.root/L"jobs"/gOpt.jobId/L"events.jsonl",std::ios::binary|std::ios::app);

  gWindowBrush=CreateSolidBrush(kWindowBg);
  gPanelBrush=CreateSolidBrush(kPanelBg);
  gBannerBrush=CreateSolidBrush(kBannerBg);

  WNDCLASSEXW wc{};wc.cbSize=sizeof(wc);wc.hInstance=h;wc.lpfnWndProc=Proc;wc.lpszClassName=kClassName;wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.hIcon=LoadIcon(nullptr,IDI_APPLICATION);wc.hbrBackground=gWindowBrush;RegisterClassExW(&wc);
  std::wstring title=L"Hi5Central Qualification Observer - "+gOpt.application;
  HWND w=CreateWindowExW(0,kClassName,title.c_str(),WS_OVERLAPPEDWINDOW|WS_VISIBLE,CW_USEDEFAULT,CW_USEDEFAULT,1220,900,nullptr,nullptr,h,nullptr);if(!w)return 2;

  HFONT titleF=Font(21,true,L"Segoe UI Variable Display");
  HFONT sectionF=Font(14,true,L"Segoe UI Variable Text");
  HFONT uiF=Font(15,false,L"Segoe UI Variable Text");
  HFONT smallF=Font(13,false,L"Segoe UI Variable Text");
  HFONT mono=Font(14,false,L"Cascadia Mono");

  std::wstringstream head;
  head<<L"Hi5Central Qualification Observer\r\n"<<gOpt.application<<L"   •   "<<gOpt.phase<<L"   •   "<<gOpt.jobId;
  gHeader=CreateWindowExW(0,L"STATIC",head.str().c_str(),WS_CHILD|WS_VISIBLE|SS_LEFT,0,0,0,0,w,nullptr,h,nullptr);
  SendMessageW(gHeader,WM_SETFONT,(WPARAM)titleF,TRUE);

  gSafetyBanner=CreateWindowExW(0,L"STATIC",L"  OBSERVER MODE   •   Selecting an application is read-only. This window never uninstalls software on selection.",WS_CHILD|WS_VISIBLE|SS_CENTERIMAGE,0,0,0,0,w,(HMENU)1205,h,nullptr);
  SendMessageW(gSafetyBanner,WM_SETFONT,(WPARAM)smallF,TRUE);

  gInstalledLabel=CreateWindowExW(0,L"STATIC",L"INSTALLED / AVAILABLE TO TEST",WS_CHILD|WS_VISIBLE|SS_LEFT,0,0,0,0,w,(HMENU)1201,h,nullptr);
  gHistoryLabel=CreateWindowExW(0,L"STATIC",L"CONFIRMED REMOVAL HISTORY",WS_CHILD|WS_VISIBLE|SS_LEFT,0,0,0,0,w,(HMENU)1202,h,nullptr);
  gEventsLabel=CreateWindowExW(0,L"STATIC",L"LIVE ACTIVITY",WS_CHILD|WS_VISIBLE|SS_LEFT,0,0,0,0,w,(HMENU)1203,h,nullptr);
  gDetailsLabel=CreateWindowExW(0,L"STATIC",L"APPLICATION DETAILS / LEARNED RECIPE",WS_CHILD|WS_VISIBLE|SS_LEFT,0,0,0,0,w,(HMENU)1204,h,nullptr);
  for(HWND label:{gInstalledLabel,gHistoryLabel,gEventsLabel,gDetailsLabel})SendMessageW(label,WM_SETFONT,(WPARAM)sectionF,TRUE);

  gInstalled=CreateWindowExW(WS_EX_STATICEDGE,L"LISTBOX",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,0,0,0,0,w,(HMENU)1101,h,nullptr);
  gHistory=CreateWindowExW(WS_EX_STATICEDGE,L"LISTBOX",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,0,0,0,0,w,(HMENU)1102,h,nullptr);
  SendMessageW(gInstalled,WM_SETFONT,(WPARAM)uiF,TRUE);SendMessageW(gHistory,WM_SETFONT,(WPARAM)uiF,TRUE);

  gEvents=CreateWindowExW(WS_EX_STATICEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY,0,0,0,0,w,nullptr,h,nullptr);
  gDetails=CreateWindowExW(WS_EX_STATICEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY,0,0,0,0,w,nullptr,h,nullptr);
  SendMessageW(gEvents,WM_SETFONT,(WPARAM)mono,TRUE);SendMessageW(gDetails,WM_SETFONT,(WPARAM)mono,TRUE);
  SendMessageW(gEvents,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELPARAM(10,10));
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
  gProcs=CaptureProcs();PopulateLists();RefreshDetails();AppendLog(L"Observer started. Installed applications are the available test set; removed applications remain in History.",{{"timestamp",NowIso()},{"type","observer_started"},{"jobId",Narrow(gOpt.jobId)},{"application",Narrow(gOpt.application)},{"phase",Narrow(gOpt.phase)}});
  SetTimer(w,kTimerId,kTimerMs,nullptr);
  ShowWindow(w,SW_SHOW);
  SetForegroundWindow(w);
  UpdateWindow(w);
  MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
  if(singleton)CloseHandle(singleton);if(SUCCEEDED(com))CoUninitialize();return(int)msg.wParam;
}
