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
HWND gSnapshot=nullptr,gExport=nullptr,gStop=nullptr;
std::map<std::wstring,AppEntry> gApps;
std::map<DWORD,ProcEntry> gProcs;
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
void RecordAppState(const AppEntry&e,bool installed,const char* eventType){
  const std::string id=Narrow(e.id),now=NowIso(); auto& s=gState["apps"][id];
  if(!s.is_object())s=json::object();
  if(!s.contains("firstSeenAt"))s["firstSeenAt"]=now;
  s["lastSeenAt"]=now;s["currentlyInstalled"]=installed;s["displayName"]=Narrow(e.name);s["version"]=Narrow(e.version);s["publisher"]=Narrow(e.publisher);
  s["scope"]=Narrow(e.scope);s["registryKey"]=Narrow(e.key);s["productCode"]=Narrow(e.productCode);s["uninstallString"]=Narrow(e.uninstallString);
  s["quietUninstallString"]=Narrow(e.quietUninstallString);s["installLocation"]=Narrow(e.installLocation);
  s["systemComponent"]=e.systemComponent;s["parentKeyName"]=Narrow(e.parentKeyName);s["testable"]=IsTestableApplication(e);
  if(!s.contains("history")||!s["history"].is_array())s["history"]=json::array();
  s["history"].push_back({{"timestamp",now},{"event",eventType},{"jobId",Narrow(gOpt.jobId)},{"application",Narrow(gOpt.application)},{"phase",Narrow(gOpt.phase)},{"snapshot",AppJson(e)}});
  SaveState();
}
void PopulateLists(){
  SendMessageW(gInstalled,LB_RESETCONTENT,0,0); SendMessageW(gHistory,LB_RESETCONTENT,0,0);
  for(const auto&[_,e]:gApps){
    if(!IsTestableApplication(e))continue;
    std::wstring row=e.name+(e.version.empty()?L"":L"  ["+e.version+L"]")+L"  ("+e.scope+L")";
    SendMessageW(gInstalled,LB_ADDSTRING,0,(LPARAM)row.c_str());
  }
  std::vector<std::wstring> hist;
  for(auto it=gState["apps"].begin();it!=gState["apps"].end();++it){
    const auto&s=it.value();
    if(s.value("currentlyInstalled",false)||!s.value("testable",false))continue;
    const auto display=Widen(s.value("displayName",std::string()));
    if(display.empty())continue;
    std::wstring row=display+L"  ["+Widen(s.value("version",std::string()))+L"]  removed";
    hist.push_back(row);
  }
  std::sort(hist.begin(),hist.end()); for(auto&r:hist)SendMessageW(gHistory,LB_ADDSTRING,0,(LPARAM)r.c_str());
}
void RefreshDetails(){
  std::wstringstream s;s<<L"CURRENT TARGET / LEARNED IDENTITY\r\n============================================================\r\n";
  const auto target=Lower(gOpt.application);bool any=false;
  for(const auto&[_,e]:gApps){auto n=Lower(e.name);if(target.empty()||n.empty()||(n.find(target)==std::wstring::npos&&target.find(n)==std::wstring::npos))continue;any=true;
    s<<L"DisplayName: "<<e.name<<L"\r\nVersion: "<<e.version<<L"\r\nPublisher: "<<e.publisher<<L"\r\nScope: "<<e.scope<<L"\r\n";
    if(!e.productCode.empty())s<<L"MSI ProductCode: "<<e.productCode<<L"\r\n";if(!e.uninstallString.empty())s<<L"UninstallString: "<<e.uninstallString<<L"\r\n";
    if(!e.quietUninstallString.empty())s<<L"QuietUninstallString: "<<e.quietUninstallString<<L"\r\n";if(!e.installLocation.empty())s<<L"InstallLocation: "<<e.installLocation<<L"\r\n";
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
  for(const auto&[id,e]:gApps)if(apps.find(id)==apps.end()){RecordAppState(e,false,"uninstalled");AppendLog(L"UNINSTALLED - "+e.name+L" "+e.version,{{"timestamp",NowIso()},{"type","uninstalled"},{"entry",AppJson(e)}});}
  gApps=std::move(apps);

  auto procs=CaptureProcs();
  for(const auto&[pid,p]:procs)if(gProcs.find(pid)==gProcs.end()){
    std::wstring detail=L"PROCESS + "+std::to_wstring(pid)+L" ppid="+std::to_wstring(p.parentPid)+L" "+p.name;
    if(!p.commandLine.empty())detail+=L" | "+p.commandLine;
    AppendLog(detail,{{"timestamp",NowIso()},{"type","process_started"},{"pid",pid},{"parentPid",p.parentPid},{"name",Narrow(p.name)},{"path",Narrow(p.path)},{"commandLine",Narrow(p.commandLine)}});
  }
  for(const auto&[pid,p]:gProcs)if(procs.find(pid)==procs.end())AppendLog(L"PROCESS - "+std::to_wstring(pid)+L" "+p.name,{{"timestamp",NowIso()},{"type","process_exited"},{"pid",pid},{"name",Narrow(p.name)},{"path",Narrow(p.path)},{"commandLine",Narrow(p.commandLine)}});
  gProcs=std::move(procs); PopulateLists();RefreshDetails();
  std::wstringstream st;st<<L"Installed / available to test: "<<SendMessageW(gInstalled,LB_GETCOUNT,0,0)<<L"   History: "<<SendMessageW(gHistory,LB_GETCOUNT,0,0)<<L"   Observed registry entries: "<<gApps.size()<<L"   Tracking phase: "<<gOpt.phase;
  if(manual)st<<L"   Snapshot saved";SetWindowTextW(gStatus,st.str().c_str());
}
void ExportEvidence(){
  json installed=json::array();for(const auto&[_,e]:gApps)installed.push_back(AppJson(e));
  json out={{"schemaVersion",1},{"observerVersion","0.1.1"},{"jobId",Narrow(gOpt.jobId)},{"application",Narrow(gOpt.application)},{"phase",Narrow(gOpt.phase)},{"exportedAt",NowIso()},{"installed",installed},{"state",gState}};
  std::ofstream f(gOpt.root/L"jobs"/gOpt.jobId/L"summary.json",std::ios::binary|std::ios::trunc);f<<out.dump(2);
}
void Layout(HWND w){
  RECT r{};GetClientRect(w,&r);int W=r.right,H=r.bottom,m=10,head=70,btn=30,status=24,gap=8,listsH=210,detailH=165;
  MoveWindow(gHeader,m,m,W-2*m,head,TRUE);int y=m+head+gap;int half=(W-3*m)/2;MoveWindow(gInstalled,m,y,half,listsH,TRUE);MoveWindow(gHistory,m*2+half,y,half,listsH,TRUE);
  y+=listsH+gap;int eventH=H-y-detailH-btn-status-gap*3-m;MoveWindow(gEvents,m,y,W-2*m,std::max(120,eventH),TRUE);y+=std::max(120,eventH)+gap;
  MoveWindow(gDetails,m,y,W-2*m,detailH,TRUE);y+=detailH+gap;MoveWindow(gSnapshot,m,y,150,btn,TRUE);MoveWindow(gExport,m+158,y,150,btn,TRUE);MoveWindow(gStop,W-m-150,y,150,btn,TRUE);
  MoveWindow(gStatus,m,y+btn+2,W-2*m,status,TRUE);
}
LRESULT CALLBACK Proc(HWND w,UINT msg,WPARAM wp,LPARAM lp){
  if(msg==WM_SIZE){Layout(w);return 0;}if(msg==WM_TIMER&&wp==kTimerId&&!gStopping){Tick(false);return 0;}
  if(msg==WM_COMMAND){if(LOWORD(wp)==1001){Tick(true);ExportEvidence();return 0;}if(LOWORD(wp)==1002){ExportEvidence();AppendLog(L"Evidence exported.",{{"timestamp",NowIso()},{"type","export"}});return 0;}if(LOWORD(wp)==1003){SendMessageW(w,WM_CLOSE,0,0);return 0;}}
  if(msg==WM_CLOSE){gStopping=true;KillTimer(w,kTimerId);Tick(false);ExportEvidence();DestroyWindow(w);return 0;}if(msg==WM_DESTROY){PostQuitMessage(0);return 0;}return DefWindowProcW(w,msg,wp,lp);
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
  gOpt=ParseOptions();std::error_code ec;std::filesystem::create_directories(gOpt.root/L"jobs"/gOpt.jobId,ec);LoadState();
  gLog.open(gOpt.root/L"jobs"/gOpt.jobId/L"observer.log",std::ios::app);gJsonl.open(gOpt.root/L"jobs"/gOpt.jobId/L"events.jsonl",std::ios::binary|std::ios::app);
  WNDCLASSEXW wc{};wc.cbSize=sizeof(wc);wc.hInstance=h;wc.lpfnWndProc=Proc;wc.lpszClassName=kClassName;wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.hIcon=LoadIcon(nullptr,IDI_APPLICATION);wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);RegisterClassExW(&wc);
  std::wstring title=L"Hi5Central Qualification Observer - "+gOpt.application;HWND w=CreateWindowExW(0,kClassName,title.c_str(),WS_OVERLAPPEDWINDOW|WS_VISIBLE,CW_USEDEFAULT,CW_USEDEFAULT,1180,860,nullptr,nullptr,h,nullptr);if(!w)return 2;
  HFONT headF=Font(18,true),uiF=Font(15),mono=Font(14,false,L"Consolas");std::wstringstream head;head<<L"Hi5Central Qualification Observer\r\nApplication: "<<gOpt.application<<L"    Phase: "<<gOpt.phase<<L"    Job: "<<gOpt.jobId<<L"\r\nInstalled / available to test (left)                         History / removed (right)";
  gHeader=CreateWindowExW(0,L"STATIC",head.str().c_str(),WS_CHILD|WS_VISIBLE|SS_LEFT,0,0,0,0,w,nullptr,h,nullptr);SendMessageW(gHeader,WM_SETFONT,(WPARAM)headF,TRUE);
  gInstalled=CreateWindowExW(WS_EX_CLIENTEDGE,L"LISTBOX",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|LBS_NOTIFY,0,0,0,0,w,nullptr,h,nullptr);SendMessageW(gInstalled,WM_SETFONT,(WPARAM)uiF,TRUE);
  gHistory=CreateWindowExW(WS_EX_CLIENTEDGE,L"LISTBOX",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|LBS_NOTIFY,0,0,0,0,w,nullptr,h,nullptr);SendMessageW(gHistory,WM_SETFONT,(WPARAM)uiF,TRUE);
  gEvents=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY,0,0,0,0,w,nullptr,h,nullptr);SendMessageW(gEvents,WM_SETFONT,(WPARAM)mono,TRUE);
  gDetails=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY,0,0,0,0,w,nullptr,h,nullptr);SendMessageW(gDetails,WM_SETFONT,(WPARAM)mono,TRUE);
  gSnapshot=CreateWindowExW(0,L"BUTTON",L"Capture snapshot",WS_CHILD|WS_VISIBLE,0,0,0,0,w,(HMENU)1001,h,nullptr);gExport=CreateWindowExW(0,L"BUTTON",L"Export evidence",WS_CHILD|WS_VISIBLE,0,0,0,0,w,(HMENU)1002,h,nullptr);gStop=CreateWindowExW(0,L"BUTTON",L"Stop observer",WS_CHILD|WS_VISIBLE,0,0,0,0,w,(HMENU)1003,h,nullptr);
  SendMessageW(gSnapshot,WM_SETFONT,(WPARAM)uiF,TRUE);SendMessageW(gExport,WM_SETFONT,(WPARAM)uiF,TRUE);SendMessageW(gStop,WM_SETFONT,(WPARAM)uiF,TRUE);
  gStatus=CreateWindowExW(0,L"STATIC",L"Starting...",WS_CHILD|WS_VISIBLE,0,0,0,0,w,nullptr,h,nullptr);SendMessageW(gStatus,WM_SETFONT,(WPARAM)uiF,TRUE);
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
