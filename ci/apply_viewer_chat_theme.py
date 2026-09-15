from pathlib import Path

p = Path('Viewer/chatpass_viewer/src/app/viewer_app.cpp')
s = p.read_text(encoding='utf-8')
repls = [
    ('body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Arial,sans-serif;background:#0b1020;color:#eef2ff}', 'body{font-family:"Segoe UI Variable","Segoe UI",Arial,sans-serif;background:#f7f9fc;color:#111827}'),
    ('.shell{height:100vh;display:flex;flex-direction:column;background:linear-gradient(180deg,#111827 0%,#0b1020 100%)}', '.shell{height:100vh;display:flex;flex-direction:column;background:#f7f9fc}'),
    ('.head{height:72px;display:flex;align-items:center;gap:12px;padding:14px 16px;border-bottom:1px solid rgba(255,255,255,.08);background:rgba(15,23,42,.94)}', '.head{height:72px;display:flex;align-items:center;gap:12px;padding:14px 16px;border-bottom:1px solid #dbe3ec;background:#f8fafc}'),
    ('.sub{font-size:12px;color:rgba(238,242,255,.58);', '.sub{font-size:12px;color:#64748b;'),
    ('.x{border:0;background:rgba(255,255,255,.08);color:#e5e7eb;', '.x{border:1px solid #dbe3ec;background:#fff;color:#64748b;'),
    ('.empty{height:100%;display:flex;align-items:center;justify-content:center;text-align:center;color:rgba(238,242,255,.48);', '.empty{height:100%;display:flex;align-items:center;justify-content:center;text-align:center;color:#64748b;'),
    ('.meta{font-size:11px;color:rgba(238,242,255,.48);', '.meta{font-size:11px;color:#64748b;'),
    ('.user .bubble{background:rgba(255,255,255,.09);color:#eef2ff;border:1px solid rgba(255,255,255,.08);', '.user .bubble{background:#fff;color:#111827;border:1px solid #dbe3ec;'),
    ('.compose{border-top:1px solid rgba(255,255,255,.08);background:rgba(15,23,42,.92);', '.compose{border-top:1px solid #dbe3ec;background:#f8fafc;'),
    ('textarea{flex:1;min-height:42px;max-height:108px;resize:none;border:1px solid rgba(255,255,255,.12);background:rgba(255,255,255,.07);color:#f8fafc;', 'textarea{flex:1;min-height:42px;max-height:108px;resize:none;border:1px solid #dbe3ec;background:#fff;color:#111827;'),
    ('<div class="title">Hi5Central Support Chat</div><div class="sub">Technician chat</div>', '<div class="title">Hi5Central Remote Support</div><div class="sub">Connected · Technician chat</div>'),
]
for old, new in repls:
    if old not in s:
        raise RuntimeError('viewer chat theme block missing: ' + old[:70])
    s = s.replace(old, new, 1)
p.write_text(s, encoding='utf-8')
print('Technician Chat theme applied')
