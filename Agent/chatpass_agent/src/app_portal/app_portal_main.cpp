#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <wrl.h>
#include <WebView2.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "dwmapi.lib")

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using json = nlohmann::json;

namespace {

constexpr UINT WM_PORTAL_RESULT = WM_APP + 301;
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\Hi5CentralAppPortal";

struct AsyncResult {
    std::string operation;
    json payload;
    std::string error;
};

struct PortalState {
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    std::filesystem::path userDataFolder;
    std::atomic<bool> busy{false};
    bool webReady = false;
};

PortalState g_state;

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int needed = MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), needed);
    return out;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

std::filesystem::path PortalUserDataFolder() {
    PWSTR raw = nullptr;
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw)) && raw) {
        result = std::filesystem::path(raw) / L"Hi5Central" / L"AppPortal" / L"WebView2";
        CoTaskMemFree(raw);
    }
    if (result.empty()) {
        wchar_t temp[MAX_PATH]{};
        const DWORD chars = GetTempPathW(MAX_PATH, temp);
        if (chars > 0 && chars < MAX_PATH) {
            result = std::filesystem::path(temp) / L"Hi5Central" / L"AppPortal" / L"WebView2";
        }
    }
    std::error_code ec;
    if (!result.empty()) std::filesystem::create_directories(result, ec);
    return result;
}

json PipeRequest(const json& request) {
    if (!WaitNamedPipeW(kPipeName, 5000)) {
        throw std::runtime_error("Hi5Central Agent App Portal service is not available.");
    }

    HANDLE pipe = CreateFileW(
        kPipeName,
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("Could not connect to the Hi5Central Agent.");
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

    const std::string encoded = request.dump();
    DWORD written = 0;
    if (!WriteFile(
            pipe,
            encoded.data(),
            static_cast<DWORD>(encoded.size()),
            &written,
            nullptr)
        || written != encoded.size()) {
        CloseHandle(pipe);
        throw std::runtime_error("Could not send the App Portal request.");
    }

    std::vector<char> buffer(64 * 1024);
    std::string response;
    for (;;) {
        DWORD read = 0;
        const BOOL ok = ReadFile(
            pipe,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &read,
            nullptr);
        if (read > 0) response.append(buffer.data(), read);
        if (response.size() > 2 * 1024 * 1024) {
            CloseHandle(pipe);
            throw std::runtime_error("The App Portal response was too large.");
        }
        if (ok) break;
        if (GetLastError() == ERROR_MORE_DATA) continue;
        CloseHandle(pipe);
        throw std::runtime_error("Could not read the App Portal response.");
    }
    CloseHandle(pipe);

    const json payload = json::parse(response, nullptr, false);
    if (payload.is_discarded() || !payload.is_object()) {
        throw std::runtime_error("Hi5Central returned an invalid App Portal response.");
    }
    return payload;
}

void PostWebJson(const json& payload) {
    if (!g_state.webReady || !g_state.webview) return;
    const std::wstring encoded = Utf8ToWide(payload.dump());
    if (!encoded.empty()) g_state.webview->PostWebMessageAsJson(encoded.c_str());
}

void SetBusy(bool busy) {
    g_state.busy.store(busy);
    PostWebJson({{"type", "busy"}, {"busy", busy}});
}

void RunAsync(const std::string& operation, json request) {
    if (g_state.busy.exchange(true)) return;
    PostWebJson({{"type", "busy"}, {"busy", true}});

    std::thread([operation, request = std::move(request)]() mutable {
        auto result = std::make_unique<AsyncResult>();
        result->operation = operation;
        try {
            result->payload = PipeRequest(request);
            if (result->payload.value("success", false) != true) {
                result->error = result->payload.value(
                    "error", std::string("Hi5Central could not complete this request."));
            }
        } catch (const std::exception& ex) {
            result->error = ex.what();
        }

        HWND target = g_state.window;
        if (target) {
            PostMessageW(
                target,
                WM_PORTAL_RESULT,
                0,
                reinterpret_cast<LPARAM>(result.release()));
        }
    }).detach();
}

void RefreshCatalogue() {
    RunAsync("catalogue", {{"type", "catalogue"}});
}

void InstallApp(const std::string& appId) {
    if (appId.empty() || appId.size() > 80) {
        PostWebJson({
            {"type", "error"},
            {"message", "The selected application is invalid."}
        });
        return;
    }
    RunAsync("install", {{"type", "install"}, {"appId", appId}});
}

std::wstring HtmlPage() {
    return LR"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
:root{
  color-scheme:light;
  --navy:#0b1730;
  --navy2:#13284f;
  --ink:#122039;
  --muted:#66758f;
  --line:#dfe6f1;
  --soft:#f4f7fb;
  --card:#ffffff;
  --accent:#f0a315;
  --accent2:#ffc85b;
  --blue:#2962ff;
  --blueSoft:#edf3ff;
  --green:#14865f;
  --greenSoft:#eaf8f2;
  --red:#c84755;
  --redSoft:#fff0f2;
  --warning:#a86908;
  --warningSoft:#fff7e8;
  --shadow:0 18px 50px rgba(15,35,70,.10);
}
*{box-sizing:border-box}
html,body{margin:0;width:100%;height:100%;font-family:"Segoe UI",system-ui,sans-serif;background:var(--soft);color:var(--ink);overflow:hidden}
button,input,select{font:inherit}
button{cursor:pointer}
.shell{height:100vh;display:grid;grid-template-rows:auto auto minmax(0,1fr)}
.hero{
  position:relative;
  padding:28px 34px 26px;
  background:
    radial-gradient(circle at 85% 10%,rgba(240,163,21,.25),transparent 26%),
    linear-gradient(135deg,var(--navy),var(--navy2));
  color:white;
  overflow:hidden;
}
.hero:after{
  content:"";
  position:absolute;right:-80px;top:-115px;width:300px;height:300px;border-radius:50%;
  border:1px solid rgba(255,255,255,.08);
}
.hero-top{display:flex;align-items:center;justify-content:space-between;gap:20px}
.brand{display:flex;align-items:center;gap:13px}
.logo{
  width:46px;height:46px;border-radius:14px;display:grid;place-items:center;
  background:linear-gradient(135deg,var(--accent2),var(--accent));
  color:#18233a;font-size:18px;font-weight:900;box-shadow:0 10px 28px rgba(240,163,21,.25)
}
.brand-copy strong{display:block;font-size:15px;letter-spacing:.01em}
.brand-copy span{display:block;margin-top:2px;color:#aebbd2;font-size:12px}
.hero-main{display:flex;align-items:flex-end;justify-content:space-between;gap:24px;margin-top:30px}
.hero-copy h1{margin:0;font-size:30px;line-height:1.08;letter-spacing:-.035em}
.hero-copy p{max-width:650px;margin:9px 0 0;color:#c9d3e5;font-size:13px;line-height:1.55}
.hero-stat{min-width:150px;text-align:right}
.hero-stat strong{display:block;font-size:28px;line-height:1}
.hero-stat span{display:block;margin-top:5px;color:#b7c4d9;font-size:11px}
.refresh{
  height:38px;display:inline-flex;align-items:center;gap:7px;border:1px solid rgba(255,255,255,.18);
  border-radius:10px;padding:0 13px;background:rgba(255,255,255,.10);color:white;font-weight:700;
  backdrop-filter:blur(10px)
}
.refresh:hover{background:rgba(255,255,255,.16)}
.refresh:disabled{opacity:.55;cursor:default}
.toolbar{
  display:grid;grid-template-columns:minmax(240px,1fr) 190px auto;gap:10px;
  padding:16px 34px;border-bottom:1px solid var(--line);background:#fff;
}
.search-wrap{position:relative}
.search-wrap svg{position:absolute;left:13px;top:11px;width:17px;height:17px;color:#8897af}
.search{
  width:100%;height:40px;border:1px solid var(--line);border-radius:10px;padding:0 14px 0 39px;
  background:#f8fafc;color:var(--ink);outline:none;transition:.16s
}
.search:focus,.category:focus{border-color:#9cb8ff;box-shadow:0 0 0 3px rgba(41,98,255,.09);background:white}
.category{
  height:40px;border:1px solid var(--line);border-radius:10px;padding:0 11px;background:#f8fafc;color:var(--ink);outline:none
}
.chips{display:flex;align-items:center;gap:6px}
.chip{
  height:36px;border:1px solid var(--line);border-radius:999px;padding:0 12px;background:white;color:var(--muted);
  font-size:11px;font-weight:700
}
.chip.active{border-color:#c5d5ff;background:var(--blueSoft);color:var(--blue)}
.content{min-height:0;overflow:auto;padding:24px 34px 34px}
.content::-webkit-scrollbar{width:10px}
.content::-webkit-scrollbar-thumb{background:#cad4e3;border-radius:99px;border:3px solid var(--soft)}
.section-head{display:flex;align-items:center;justify-content:space-between;gap:14px;margin-bottom:13px}
.section-head h2{margin:0;font-size:16px;letter-spacing:-.01em}
.section-head span{color:var(--muted);font-size:11px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(290px,1fr));gap:14px}
.card{
  min-height:225px;display:flex;flex-direction:column;border:1px solid var(--line);border-radius:16px;padding:17px;
  background:var(--card);box-shadow:0 3px 14px rgba(18,32,57,.035);transition:.16s
}
.card:hover{transform:translateY(-1px);border-color:#cbd8ea;box-shadow:var(--shadow)}
.card-top{display:flex;align-items:flex-start;gap:12px}
.app-icon{
  flex:0 0 auto;width:49px;height:49px;border-radius:13px;display:grid;place-items:center;
  background:linear-gradient(135deg,#e9efff,#fff5df);border:1px solid #dfe7f6;color:#243c71;font-size:16px;font-weight:900
}
.app-title{min-width:0;flex:1}
.app-title h3{margin:1px 0 4px;font-size:14px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.app-title p{margin:0;color:var(--muted);font-size:10px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.pill{
  display:inline-flex;align-items:center;gap:5px;border-radius:999px;padding:5px 8px;font-size:9px;font-weight:800;white-space:nowrap
}
.pill.available{background:var(--blueSoft);color:var(--blue)}
.pill.installing{background:var(--warningSoft);color:var(--warning)}
.pill.installed{background:var(--greenSoft);color:var(--green)}
.pill.failed{background:var(--redSoft);color:var(--red)}
.pill.approval{background:#f5efff;color:#6d46ad}
.desc{
  min-height:49px;margin:14px 0 12px;color:#53637b;font-size:11px;line-height:1.5;
  display:-webkit-box;-webkit-line-clamp:3;-webkit-box-orient:vertical;overflow:hidden
}
.meta{display:flex;align-items:center;flex-wrap:wrap;gap:7px;margin-top:auto}
.meta span{border-radius:7px;padding:5px 7px;background:#f5f7fa;color:#66758f;font-size:9px}
.card-actions{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-top:14px;padding-top:13px;border-top:1px solid #edf1f6}
.scope{min-width:0;color:#7a879c;font-size:9px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.install{
  min-width:92px;height:34px;border:0;border-radius:9px;padding:0 13px;background:var(--blue);color:white;font-size:10px;font-weight:800;
  box-shadow:0 6px 16px rgba(41,98,255,.18)
}
.install:hover{background:#174fe0}
.install.secondary{background:#f0f4fa;color:#4e607b;box-shadow:none}
.install.approval{background:#7652b9}
.install:disabled{opacity:.5;cursor:default;box-shadow:none}
.empty{
  min-height:310px;display:flex;flex-direction:column;align-items:center;justify-content:center;text-align:center;
  border:1px dashed #ccd7e6;border-radius:18px;background:rgba(255,255,255,.62);padding:30px
}
.empty-icon{
  width:66px;height:66px;border-radius:20px;display:grid;place-items:center;margin-bottom:15px;
  background:linear-gradient(135deg,#eaf0ff,#fff4de);color:#284d9b;font-size:23px;font-weight:900
}
.empty h3{margin:0;font-size:16px}
.empty p{max-width:460px;margin:7px 0 0;color:var(--muted);font-size:11px;line-height:1.55}
.empty button{margin-top:14px;height:35px;border:1px solid var(--line);border-radius:9px;padding:0 13px;background:white;color:var(--blue);font-weight:800;font-size:10px}
.loading{display:grid;grid-template-columns:repeat(auto-fill,minmax(290px,1fr));gap:14px}
.skeleton{height:225px;border-radius:16px;border:1px solid var(--line);background:linear-gradient(90deg,#f5f7fb 25%,#fff 50%,#f5f7fb 75%);background-size:200% 100%;animation:shine 1.15s infinite}
@keyframes shine{to{background-position:-200% 0}}
.toast{
  position:fixed;right:24px;bottom:24px;max-width:420px;display:flex;align-items:flex-start;gap:10px;
  border:1px solid #d9e3f1;border-radius:13px;padding:12px 14px;background:#fff;box-shadow:0 18px 48px rgba(12,31,64,.18);
  transform:translateY(20px);opacity:0;pointer-events:none;transition:.2s;z-index:20
}
.toast.show{transform:translateY(0);opacity:1}
.toast.error{border-color:#f1ccd1}
.toast-mark{width:25px;height:25px;display:grid;place-items:center;border-radius:8px;background:var(--greenSoft);color:var(--green);font-weight:900}
.toast.error .toast-mark{background:var(--redSoft);color:var(--red)}
.toast strong{display:block;font-size:11px}
.toast p{margin:3px 0 0;color:var(--muted);font-size:10px;line-height:1.4}
@media(max-width:760px){
  .hero{padding:22px}
  .hero-main{align-items:flex-start}
  .hero-stat{display:none}
  .toolbar{grid-template-columns:1fr 150px;padding:13px 22px}
  .chips{grid-column:1/-1;overflow:auto}
  .content{padding:20px 22px 28px}
}
</style>
</head>
<body>
<div class="shell">
  <header class="hero">
    <div class="hero-top">
      <div class="brand">
        <div class="logo">H5</div>
        <div class="brand-copy"><strong>Hi5Central App Portal</strong><span>Secure self-service software</span></div>
      </div>
      <button id="refresh" class="refresh" type="button"><span>&#8635;</span><span>Refresh</span></button>
    </div>
    <div class="hero-main">
      <div class="hero-copy">
        <h1>Company software, ready when you need it.</h1>
        <p>Install applications approved for you or this device. Hi5Central handles elevation and installation securely in the background.</p>
      </div>
      <div class="hero-stat"><strong id="availableCount">--</strong><span>applications available</span></div>
    </div>
  </header>

  <section class="toolbar">
    <div class="search-wrap">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="11" cy="11" r="7"/><path d="m20 20-3.6-3.6"/></svg>
      <input id="search" class="search" type="search" placeholder="Search applications or publishers..." autocomplete="off">
    </div>
    <select id="category" class="category"><option value="all">All categories</option></select>
    <div class="chips" id="chips">
      <button class="chip active" data-filter="all" type="button">All</button>
      <button class="chip" data-filter="available" type="button">Available</button>
      <button class="chip" data-filter="installing" type="button">Installing</button>
      <button class="chip" data-filter="installed" type="button">Installed</button>
    </div>
  </section>

  <main class="content">
    <div class="section-head"><h2>Applications</h2><span id="resultCount">Loading catalogue...</span></div>
    <div id="body" class="loading"><div class="skeleton"></div><div class="skeleton"></div><div class="skeleton"></div></div>
  </main>
</div>

<div id="toast" class="toast">
  <div class="toast-mark">OK</div>
  <div><strong id="toastTitle">Done</strong><p id="toastMessage"></p></div>
</div>

<script>
const state={apps:[],installations:[],busy:true,query:'',category:'all',filter:'all'};
const body=document.getElementById('body');
const search=document.getElementById('search');
const category=document.getElementById('category');
const chips=document.getElementById('chips');
const refresh=document.getElementById('refresh');
const availableCount=document.getElementById('availableCount');
const resultCount=document.getElementById('resultCount');
const toast=document.getElementById('toast');
let toastTimer=null;

function post(message){
  try{
    if(window.chrome&&window.chrome.webview){
      window.chrome.webview.postMessage(message);
      return true;
    }
  }catch(_){}
  return false;
}
function escapeHtml(value){
  return String(value??'').replace(/[&<>"']/g,ch=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[ch]));
}
function initials(name){
  const parts=String(name||'App').trim().split(/\s+/).filter(Boolean);
  return (parts.slice(0,2).map(p=>p[0]).join('')||'AP').toUpperCase();
}
function installationFor(appId){
  return state.installations.find(item=>item.app_id===appId)||null;
}
function appState(app){
  const install=installationFor(app.id);
  const job=String(install?.job_status||'').toLowerCase();
  const lifecycle=String(install?.installation_status||'').toLowerCase();
  if(job==='queued'||job==='claimed'||lifecycle==='queued'||lifecycle==='running'||lifecycle==='requested') return {key:'installing',label:'Installing'};
  if(job==='completed'||lifecycle==='succeeded') return {key:'installed',label:'Installed'};
  if(job==='failed'||job==='cancelled'||lifecycle==='failed'||lifecycle==='cancelled') return {key:'failed',label:'Failed'};
  if(app.intent==='approval_required') return {key:'approval',label:'Approval required'};
  if(app.intent==='required') return {key:'available',label:'Required'};
  return {key:'available',label:'Available'};
}
function scopeLabel(app){
  const scope=app.scope||{};
  if(!scope.type) return 'Assigned by your organisation';
  if(scope.type==='Estate') return 'Available to your organisation';
  return 'Assigned via '+scope.type+(scope.name?' - '+scope.name:'');
}
function actionFor(app,status){
  if(status.key==='installing') return {label:'Installing...',disabled:true,cls:'secondary'};
  if(status.key==='installed') return {label:'Installed',disabled:true,cls:'secondary'};
  if(app.intent==='approval_required') return {label:'Request approval',disabled:false,cls:'approval'};
  if(status.key==='failed') return {label:'Retry',disabled:false,cls:''};
  return {label:'Install',disabled:false,cls:''};
}
function filteredApps(){
  const q=state.query.trim().toLowerCase();
  return state.apps.filter(app=>{
    const status=appState(app);
    const matchesFilter=state.filter==='all'||status.key===state.filter;
    const matchesCategory=state.category==='all'||String(app.category||'Company software')===state.category;
    const hay=(String(app.name||'')+' '+String(app.publisher||'')+' '+String(app.description||'')).toLowerCase();
    return matchesFilter&&matchesCategory&&(!q||hay.includes(q));
  });
}
function buildCategories(){
  const values=[...new Set(state.apps.map(app=>String(app.category||'Company software')).filter(Boolean))].sort((a,b)=>a.localeCompare(b));
  const current=state.category;
  category.innerHTML='<option value="all">All categories</option>'+values.map(v=>'<option value="'+escapeHtml(v)+'">'+escapeHtml(v)+'</option>').join('');
  category.value=values.includes(current)?current:'all';
  state.category=category.value;
}
function render(){
  availableCount.textContent=state.apps.length;
  const apps=filteredApps();
  resultCount.textContent=apps.length+' of '+state.apps.length+' applications';
  refresh.disabled=state.busy;

  if(!state.apps.length){
    body.className='';
    body.innerHTML='<div class="empty"><div class="empty-icon">H5</div><h3>No software has been assigned yet</h3><p>Your IT team can publish Hi5Central catalogue applications or company-specific packages to this portal. Once assigned, they will appear here automatically.</p><button type="button" onclick="requestRefresh()">Check again</button></div>';
    return;
  }
  if(!apps.length){
    body.className='';
    body.innerHTML='<div class="empty"><div class="empty-icon">S</div><h3>No matching applications</h3><p>Try a different search term, category or status filter.</p><button type="button" onclick="clearFilters()">Clear filters</button></div>';
    return;
  }

  body.className='grid';
  body.innerHTML=apps.map(app=>{
    const status=appState(app);
    const action=actionFor(app,status);
    const description=app.description||'Approved company software available through Hi5Central.';
    const version=app.version||'Current release';
    const publisher=app.publisher||'Company application';
    const categoryName=app.category||'Company software';
    return '<article class="card">'
      +'<div class="card-top"><div class="app-icon">'+escapeHtml(initials(app.name))+'</div>'
      +'<div class="app-title"><h3>'+escapeHtml(app.name||'Application')+'</h3><p>'+escapeHtml(publisher)+'</p></div>'
      +'<span class="pill '+escapeHtml(status.key)+'">'+escapeHtml(status.label)+'</span></div>'
      +'<p class="desc">'+escapeHtml(description)+'</p>'
      +'<div class="meta"><span>'+escapeHtml(version)+'</span><span>'+escapeHtml(categoryName)+'</span></div>'
      +'<div class="card-actions"><span class="scope">'+escapeHtml(scopeLabel(app))+'</span>'
      +'<button class="install '+escapeHtml(action.cls)+'" type="button" '+(action.disabled?'disabled':'')+' data-install="'+escapeHtml(app.id)+'">'+escapeHtml(action.label)+'</button></div>'
      +'</article>';
  }).join('');

  body.querySelectorAll('[data-install]').forEach(button=>{
    button.addEventListener('click',()=>{
      const id=button.getAttribute('data-install');
      if(!id||state.busy)return;
      button.disabled=true;
      post({type:'install',appId:id});
    });
  });
}
function setBusy(busy){
  state.busy=!!busy;
  refresh.disabled=state.busy;
}
function showToast(title,message,error=false){
  clearTimeout(toastTimer);
  toast.classList.toggle('error',!!error);
  toast.querySelector('.toast-mark').textContent=error?'!':'OK';
  document.getElementById('toastTitle').textContent=title;
  document.getElementById('toastMessage').textContent=message||'';
  toast.classList.add('show');
  toastTimer=setTimeout(()=>toast.classList.remove('show'),5000);
}
function requestRefresh(){
  if(state.busy)return;
  post({type:'refresh'});
}
function clearFilters(){
  state.query='';
  state.category='all';
  state.filter='all';
  search.value='';
  category.value='all';
  chips.querySelectorAll('.chip').forEach(button=>button.classList.toggle('active',button.dataset.filter==='all'));
  render();
}
refresh.addEventListener('click',requestRefresh);
search.addEventListener('input',()=>{state.query=search.value;render();});
category.addEventListener('change',()=>{state.category=category.value;render();});
chips.addEventListener('click',event=>{
  const button=event.target.closest('.chip');
  if(!button)return;
  state.filter=button.dataset.filter||'all';
  chips.querySelectorAll('.chip').forEach(item=>item.classList.toggle('active',item===button));
  render();
});

if(window.chrome&&window.chrome.webview){
  window.chrome.webview.addEventListener('message',event=>{
    const message=event.data||{};
    if(message.type==='busy'){
      setBusy(message.busy);
    }else if(message.type==='catalogue'){
      state.apps=Array.isArray(message.apps)?message.apps:[];
      state.installations=Array.isArray(message.installations)?message.installations:[];
      setBusy(false);
      buildCategories();
      render();
    }else if(message.type==='install_result'){
      setBusy(false);
      if(message.approvalRequired) showToast('Request sent','Your approval request has been sent to IT.');
      else showToast('Installation started','Hi5Central is installing the application in the background.');
    }else if(message.type==='error'){
      setBusy(false);
      showToast('Something went wrong',message.message||'Please try again.',true);
      if(!state.apps.length){
        body.className='';
        body.innerHTML='<div class="empty"><div class="empty-icon">!</div><h3>Unable to load company software</h3><p>'+escapeHtml(message.message||'Please check your connection and try again.')+'</p><button type="button" onclick="requestRefresh()">Try again</button></div>';
        resultCount.textContent='Catalogue unavailable';
      }
    }
  });
}
post({type:'web_ready'});
</script>
</body>
</html>
)HTML";
}

void ResizeWebView() {
    if (!g_state.window || !g_state.controller) return;
    RECT bounds{};
    GetClientRect(g_state.window, &bounds);
    g_state.controller->put_Bounds(bounds);
}

void HandleWebMessage(ICoreWebView2WebMessageReceivedEventArgs* args) {
    if (!args) return;
    LPWSTR raw = nullptr;
    if (FAILED(args->get_WebMessageAsJson(&raw)) || !raw) {
        if (raw) CoTaskMemFree(raw);
        return;
    }

    std::wstring wide(raw);
    CoTaskMemFree(raw);
    const json message = json::parse(WideToUtf8(wide), nullptr, false);
    if (message.is_discarded() || !message.is_object()) return;

    const std::string type = message.value("type", std::string());
    if (type == "web_ready") {
        g_state.webReady = true;
        RefreshCatalogue();
    } else if (type == "refresh") {
        if (!g_state.busy.load()) RefreshCatalogue();
    } else if (type == "install") {
        if (!g_state.busy.load()) InstallApp(message.value("appId", std::string()));
    }
}

void InitWebView2() {
    const std::wstring folder = g_state.userDataFolder.wstring();
    const HRESULT start = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        folder.empty() ? nullptr : folder.c_str(),
        nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                if (FAILED(result) || !environment) {
                    MessageBoxW(
                        g_state.window,
                        L"Microsoft Edge WebView2 Runtime is required to use Hi5Central App Portal.",
                        L"Hi5Central App Portal",
                        MB_OK | MB_ICONERROR);
                    PostMessageW(g_state.window, WM_CLOSE, 0, 0);
                    return S_OK;
                }

                environment->CreateCoreWebView2Controller(
                    g_state.window,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT controllerResult, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(controllerResult) || !controller) {
                                MessageBoxW(
                                    g_state.window,
                                    L"Hi5Central could not start the App Portal user interface.",
                                    L"Hi5Central App Portal",
                                    MB_OK | MB_ICONERROR);
                                PostMessageW(g_state.window, WM_CLOSE, 0, 0);
                                return S_OK;
                            }

                            g_state.controller = controller;
                            controller->get_CoreWebView2(&g_state.webview);
                            ResizeWebView();

                            ComPtr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(g_state.webview->get_Settings(&settings)) && settings) {
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_AreDevToolsEnabled(FALSE);
                                settings->put_IsStatusBarEnabled(FALSE);
                                settings->put_IsZoomControlEnabled(FALSE);
                            }

                            EventRegistrationToken token{};
                            g_state.webview->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        HandleWebMessage(args);
                                        return S_OK;
                                    }).Get(),
                                &token);

                            const std::wstring page = HtmlPage();
                            g_state.webview->NavigateToString(page.c_str());
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());

    if (FAILED(start)) {
        MessageBoxW(
            g_state.window,
            L"Hi5Central could not initialise the App Portal user interface.",
            L"Hi5Central App Portal",
            MB_OK | MB_ICONERROR);
        PostMessageW(g_state.window, WM_CLOSE, 0, 0);
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        g_state.window = hwnd;
        return 0;

    case WM_SIZE:
        ResizeWebView();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        if (info) {
            info->ptMinTrackSize.x = 760;
            info->ptMinTrackSize.y = 560;
        }
        return 0;
    }

    case WM_PORTAL_RESULT: {
        std::unique_ptr<AsyncResult> result(reinterpret_cast<AsyncResult*>(lParam));
        g_state.busy.store(false);
        if (!result) return 0;

        if (!result->error.empty()) {
            PostWebJson({
                {"type", "error"},
                {"message", result->error}
            });
            return 0;
        }

        if (result->operation == "catalogue") {
            json message = result->payload;
            message["type"] = "catalogue";
            PostWebJson(message);
        } else if (result->operation == "install") {
            json message = result->payload;
            message["type"] = "install_result";
            PostWebJson(message);
            RefreshCatalogue();
        }
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        g_state.window = nullptr;
        g_state.webview.Reset();
        g_state.controller.Reset();
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(co)) return 2;

    g_state.instance = instance;
    g_state.userDataFolder = PortalUserDataFolder();

    const wchar_t className[] = L"Hi5CentralAppPortalWindowV2";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = className;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        CoUninitialize();
        return 3;
    }

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int width = std::min(1180, std::max(900, static_cast<int>((work.right - work.left) * 0.78)));
    const int height = std::min(820, std::max(640, static_cast<int>((work.bottom - work.top) * 0.82)));
    const int x = work.left + std::max(20, ((work.right - work.left) - width) / 2);
    const int y = work.top + std::max(20, ((work.bottom - work.top) - height) / 2);

    HWND window = CreateWindowExW(
        0,
        className,
        L"Hi5Central App Portal",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        x,
        y,
        width,
        height,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (!window) {
        CoUninitialize();
        return 4;
    }

    BOOL darkTitle = TRUE;
    DwmSetWindowAttribute(window, 20, &darkTitle, sizeof(darkTitle));

    ShowWindow(window, show == SW_HIDE ? SW_SHOWNORMAL : show);
    UpdateWindow(window);
    InitWebView2();

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    g_state.webview.Reset();
    g_state.controller.Reset();
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
