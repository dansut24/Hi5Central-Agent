from pathlib import Path

def rep(path, old, new):
    p=Path(path); s=p.read_text(encoding='utf-8')
    if old not in s: raise RuntimeError(f'block missing: {path}')
    p.write_text(s.replace(old,new,1),encoding='utf-8')

svc='Agent/chatpass_agent/src/platform/windows/windows_service.cpp'
chat='Agent/chatpass_agent/src/ui/native_chat_window.cpp'
head='Agent/chatpass_agent/src/ui/native_chat_window.h'
view='Viewer/chatpass_viewer/src/app/viewer_app.cpp'
files='Viewer/chatpass_viewer/web/file_browser.html'

rep(svc,'                        EnsureChatOverlay(ctx.sessionId, ctx.sessionJob,','                        ChatOverlayState* overlay = EnsureChatOverlay(ctx.sessionId, ctx.sessionJob,')
rep(svc,'                            });\n                    }\n\n                    if (ConsumePresenceBannerAction(ctx.sessionId, false)) {','                            });\n                        if (overlay) {\n                            {\n                                std::lock_guard<std::mutex> lock(overlay->mu);\n                                overlay->pendingToOverlay.push_back(json{{"type", "show"}, {"session_id", ctx.sessionId}}.dump());\n                            }\n                            FlushQueuedChatToOverlay(overlay);\n                        }\n                    }\n\n                    if (ConsumePresenceBannerAction(ctx.sessionId, false)) {')
rep(chat,'    void NativeChatWindow::UiThreadMain() {\n        uiThreadId_ = GetCurrentThreadId();','    void NativeChatWindow::UiThreadMain() {\n        uiThreadId_ = GetCurrentThreadId();\n        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);')
rep(chat,'            440,\n            420,','            420,\n            560,')
rep(chat,'        const int margin = 12;\n        const int composerHeight = 32;\n        const int buttonWidth = 84;','        const float scale = std::max(1.0f, static_cast<float>(GetDpiForWindow(hwnd_)) / 96.0f);\n        const auto px = [scale](int dip) { return std::max(1, static_cast<int>(dip * scale)); };\n        const int margin = px(12);\n        const int composerHeight = px(40);\n        const int buttonWidth = px(82);')
rep(chat,'                if (type == "chat_message") {','                if (type == "show") { window.Show(); continue; }\n                if (type == "chat_message") {')
rep(view,'                    chat.set_size(380, 560, WEBVIEW_HINT_NONE);','                    chat.set_size(420, 620, WEBVIEW_HINT_NONE);')
rep(view,"try{if(window.hi5ChatSend)await window.hi5ChatSend(body);add({sender:'tech',display_name:'Technician',body});}catch(e){console.error(e)}","try{if(window.hi5ChatSend)await window.hi5ChatSend(body);}catch(e){console.error(e)}")
rep(view,'                        const int width = 380;\n                        const int height = 560;','                        const int workWidth = static_cast<int>(work.right - work.left);\n                        const int workHeight = static_cast<int>(work.bottom - work.top);\n                        const int width = std::min(420, std::max(300, workWidth - 24));\n                        const int height = std::min(620, std::max(320, workHeight - 24));')
rep(view,'                        const int maxWidth = std::max<int>(860, workWidth);\n                        const int maxHeight = std::max<int>(560, workHeight);\n                        const int width = std::min<int>(1120, maxWidth);\n                        const int height = std::min<int>(740, maxHeight);\n                        file.set_size(900, 600, WEBVIEW_HINT_MIN);','                        const int width = std::min<int>(1120, std::max<int>(320, workWidth - 24));\n                        const int height = std::min<int>(740, std::max<int>(300, workHeight - 24));\n                        file.set_size(std::min<int>(420, width), std::min<int>(360, height), WEBVIEW_HINT_MIN);')

p=Path(files); s=p.read_text(encoding='utf-8')
s=s.replace('<title>Hi5Tech File Browser</title>','<title>Hi5Central File Browser</title>',1)
s=s.replace('.shell{height:100vh;min-width:860px;','.shell{height:100vh;min-width:0;',1)
s=s.replace('grid-template-columns:minmax(380px,1fr) minmax(380px,1fr);','grid-template-columns:minmax(0,1fr) minmax(0,1fr);',1)
s=s.replace('</style>','@media(max-width:840px){.layout{grid-template-columns:1fr;overflow:auto}.pane{min-height:340px}.pathRow{flex-wrap:wrap}.pathInput{flex-basis:100%}.pathRow .btn{flex:1}.sub{display:none}}@media(max-width:520px){.layout{padding:6px;gap:6px}.pane{min-height:300px}.quick{overflow:auto;flex-wrap:nowrap}.quick button{flex:0 0 auto}.meta{display:none}.header{padding:8px}.status{font-size:10px}}</style>',1)
p.write_text(s,encoding='utf-8')
print('UI changes applied')
