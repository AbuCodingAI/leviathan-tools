#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <sys/wait.h>

#if __has_include("webview.h")
#include "webview.h"
#define DADA_HAS_WEBVIEW 1
#elif __has_include("third_party/webview/webview.h")
#include "third_party/webview/webview.h"
#define DADA_HAS_WEBVIEW 1
#else
#define DADA_HAS_WEBVIEW 0
#endif

namespace {
#if DADA_HAS_WEBVIEW

std::string ShellEscape(const std::string& s) {
  std::string o; o.reserve(s.size() + 8); o.push_back('\'');
  for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
  o.push_back('\''); return o;
}

struct CmdResult { int exit_code = 0; std::string out; };

CmdResult RunCmd(const std::vector<std::string>& a) {
  if (a.empty()) return {127, "no command"};
  std::ostringstream cmd;
  for (size_t i = 0; i < a.size(); ++i) { if (i) cmd << " "; cmd << ShellEscape(a[i]); }
  cmd << " 2>&1";
  FILE* p = popen(cmd.str().c_str(), "r");
  if (!p) return {127, "popen failed"};
  std::string out; char buf[4096];
  while (fgets(buf, sizeof(buf), p)) out += buf;
  int rc = pclose(p);
  int ec = WIFEXITED(rc) ? WEXITSTATUS(rc) : (WIFSIGNALED(rc) ? 128 + WTERMSIG(rc) : 1);
  return {ec, out};
}

std::string JE(const std::string& s) {
  std::string o; o.reserve(s.size() + 16);
  for (char c : s) {
    switch (c) {
      case '\\': o += "\\\\"; break; case '"': o += "\\\""; break;
      case '\n': o += "\\n";  break; case '\r': o += "\\r";  break;
      case '\t': o += "\\t";  break; default:   o += c;
    }
  }
  return o;
}

std::string ToJson(const CmdResult& r) {
  return "{\"exit_code\":" + std::to_string(r.exit_code) + ",\"out\":\"" + JE(r.out) + "\"}";
}

// Extract a string or numeric value from the flat JSON object in req.
std::string ExtractParam(const std::string& req, const std::string& key) {
  auto kp = req.find("\"" + key + "\"");
  if (kp == std::string::npos) return "";
  auto cp = req.find(':', kp + key.size() + 2);
  if (cp == std::string::npos) return "";
  while (++cp < req.size() && req[cp] == ' ');
  if (cp >= req.size()) return "";
  if (req[cp] == '"') {
    auto ep = req.find('"', cp + 1);
    return ep == std::string::npos ? "" : req.substr(cp + 1, ep - cp - 1);
  }
  auto ep = req.find_first_of(",}]", cp);
  return ep == std::string::npos ? req.substr(cp) : req.substr(cp, ep - cp);
}

bool IsNum(const std::string& s) {
  return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos;
}
bool SafePort(const std::string& s) {
  if (s.empty() || s.size() > 32) return false;
  for (char c : s) if (!isalnum(c) && c != ':' && c != '-') return false;
  return true;
}
bool SafeProto(const std::string& s)  { return s == "tcp" || s == "udp" || s == "any"; }
bool SafePolicy(const std::string& s) { return s == "allow" || s == "deny"; }

#endif
} // namespace

int main() {
#if !DADA_HAS_WEBVIEW
  std::cerr << "DADA requires webview.h.\n"
            << "Install deps: sudo apt-get install -y g++ pkg-config libgtk-4-dev libwebkitgtk-6.0-dev\n";
  return 2;
#else
  webview::webview w(true, nullptr);
  w.set_title("DADA — Defense Against the Dark Arts");
  w.set_size(1120, 720, WEBVIEW_HINT_NONE);

  const char* html = R"HTML(
<!doctype html><html><head>
<meta charset="utf-8"/><title>DADA</title>
<style>
:root {
  --bg:#0b0f16; --sidebar:#0d1219; --card:rgba(18,26,39,.92);
  --muted:#7a8fa8; --text:#e9f0ff; --accent:#7dd3fc;
  --good:#34d399; --warn:#fbbf24; --bad:#fb7185;
  --border:rgba(255,255,255,.07); --sh:rgba(0,0,0,.4);
}
*{box-sizing:border-box;margin:0;padding:0;}
body{display:flex;height:100vh;overflow:hidden;
  font-family:ui-sans-serif,system-ui,-apple-system,sans-serif;
  background:var(--bg);color:var(--text);font-size:13px;}

/* ── Sidebar ── */
#sidebar{width:188px;min-width:188px;background:var(--sidebar);
  border-right:1px solid var(--border);display:flex;flex-direction:column;}
.brand{padding:18px 16px 14px;border-bottom:1px solid var(--border);}
.brand-name{font-size:15px;font-weight:700;letter-spacing:.3px;}
.brand-sub{font-size:10px;color:var(--muted);margin-top:2px;}
nav{flex:1;padding:8px;overflow-y:auto;}
.ni{display:flex;align-items:center;gap:9px;padding:9px 10px;border-radius:9px;
  cursor:pointer;color:var(--muted);transition:all .12s;user-select:none;
  border:1px solid transparent;margin-bottom:2px;font-size:12.5px;}
.ni:hover{background:rgba(255,255,255,.04);color:var(--text);}
.ni.active{background:rgba(125,211,252,.1);border-color:rgba(125,211,252,.2);color:var(--accent);}
.ni-icon{font-size:13px;width:17px;text-align:center;}
.sf{padding:11px 16px;border-top:1px solid var(--border);font-size:10px;color:var(--muted);}

/* ── Content ── */
#content{flex:1;overflow-y:auto;
  background:radial-gradient(900px 600px at 10% 0%,rgba(125,211,252,.09),transparent 50%),
             radial-gradient(700px 500px at 90% 30%,rgba(52,211,153,.06),transparent 55%),
             var(--bg);}
.tab{display:none;padding:22px 24px;max-width:980px;}
.tab.active{display:block;}
.th{display:flex;align-items:center;justify-content:space-between;margin-bottom:18px;}
.th h1{font-size:17px;font-weight:700;}
.th-acts{display:flex;gap:8px;}

/* ── Cards ── */
.card{background:var(--card);border:1px solid var(--border);border-radius:14px;
  padding:16px;box-shadow:0 8px 32px var(--sh);backdrop-filter:blur(8px);margin-bottom:14px;}
.card h2{font-size:12.5px;font-weight:650;color:#dbeafe;margin-bottom:12px;}

/* ── Stat grid ── */
.sg{display:grid;grid-template-columns:repeat(4,1fr);gap:11px;margin-bottom:14px;}
.sc{background:var(--card);border:1px solid var(--border);border-radius:13px;
  padding:13px 15px;box-shadow:0 4px 20px var(--sh);}
.sl{font-size:10px;color:var(--muted);text-transform:uppercase;letter-spacing:.6px;margin-bottom:5px;}
.sv{font-size:14px;font-weight:600;}
.ss{font-size:10px;color:var(--muted);margin-top:3px;}

/* ── Pills ── */
.pills{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:14px;}
.pill{display:inline-flex;align-items:center;gap:7px;padding:6px 11px;border-radius:999px;
  border:1px solid var(--border);font-size:11.5px;color:var(--muted);background:rgba(255,255,255,.025);}
.dot{width:7px;height:7px;border-radius:50%;background:var(--warn);flex-shrink:0;}
.dot.good{background:var(--good);} .dot.bad{background:var(--bad);}

/* ── Buttons ── */
button{border:1px solid var(--border);background:rgba(255,255,255,.04);color:var(--text);
  padding:7px 12px;border-radius:9px;cursor:pointer;font-size:12px;font-weight:600;transition:all .12s;}
button:hover{border-color:rgba(125,211,252,.35);background:rgba(125,211,252,.06);}
button.primary{background:rgba(125,211,252,.14);border-color:rgba(125,211,252,.28);color:var(--accent);}
button.danger{background:rgba(251,113,133,.09);border-color:rgba(251,113,133,.22);color:var(--bad);}
button:disabled{opacity:.35;cursor:not-allowed;}

/* ── Pre ── */
pre{background:rgba(0,0,0,.28);border:1px solid var(--border);border-radius:10px;
  padding:11px;color:#c7d2fe;font-size:11.5px;line-height:1.5;
  overflow:auto;max-height:280px;white-space:pre-wrap;word-break:break-all;}
.infected{color:var(--bad);}

/* ── Tables ── */
table{width:100%;border-collapse:collapse;font-size:12px;}
th{text-align:left;padding:7px 10px;color:var(--muted);font-weight:600;border-bottom:1px solid var(--border);}
td{padding:6px 10px;border-bottom:1px solid rgba(255,255,255,.03);}
tr:hover td{background:rgba(255,255,255,.02);}
.mono{font-family:ui-monospace,monospace;font-size:11px;}

/* ── Forms ── */
.fr{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-bottom:10px;}
input[type=text],select{background:rgba(0,0,0,.28);border:1px solid var(--border);
  color:var(--text);padding:7px 10px;border-radius:9px;font-size:12px;outline:none;}
input[type=text]:focus,select:focus{border-color:rgba(125,211,252,.38);}
select option{background:#1a2233;}

/* ── Banners ── */
.banner{padding:9px 14px;border-radius:10px;margin-bottom:14px;
  font-size:12.5px;font-weight:600;display:flex;align-items:center;gap:8px;}
.banner.on{background:rgba(52,211,153,.09);border:1px solid rgba(52,211,153,.22);color:var(--good);}
.banner.off{background:rgba(251,113,133,.07);border:1px solid rgba(251,113,133,.18);color:var(--bad);}

.hint{font-size:10.5px;color:var(--muted);margin-top:8px;}
.dementor-ok{padding:12px 16px;border-radius:12px;margin-bottom:14px;
  background:rgba(52,211,153,.08);border:1px solid rgba(52,211,153,.2);
  color:var(--good);font-size:13px;font-weight:600;display:flex;align-items:center;gap:10px;}
.dementor-bad{padding:12px 16px;border-radius:12px;margin-bottom:14px;
  background:rgba(251,113,133,.08);border:1px solid rgba(251,113,133,.2);color:var(--bad);}
.dementor-bad .issues{font-size:12px;margin-bottom:6px;opacity:.85;line-height:1.7;}
.dementor-bad .label{font-size:13px;font-weight:700;}
.spin{display:inline-block;animation:spin .7s linear infinite;}
@keyframes spin{to{transform:rotate(360deg);}}
@media(max-width:860px){.sg{grid-template-columns:1fr 1fr;}}
</style></head><body>

<div id="sidebar">
  <div class="brand">
    <div class="brand-name">DADA</div>
    <div class="brand-sub">Defense Against the Dark Arts</div>
  </div>
  <nav>
    <div class="ni active" onclick="switchTab('overview',this)"><span class="ni-icon">◈</span>Overview</div>
    <div class="ni" onclick="switchTab('firewall',this)"><span class="ni-icon">◎</span>Firewall</div>
    <div class="ni" onclick="switchTab('scanner',this)"><span class="ni-icon">◉</span>Scanner</div>
    <div class="ni" onclick="switchTab('network',this)"><span class="ni-icon">⊡</span>Network</div>
    <div class="ni" onclick="switchTab('procs',this)"><span class="ni-icon">▤</span>Processes</div>
    <div class="ni" onclick="switchTab('disks',this)"><span class="ni-icon">▥</span>Disks</div>
    <div class="ni" onclick="switchTab('dcak',this)"><span class="ni-icon">◆</span>DCAK</div>
    <div class="ni" onclick="switchTab('about',this)"><span class="ni-icon">ⓘ</span>About</div>
  </nav>
  <div class="sf">LeviathanOS Pv2</div>
</div>

<div id="content">

<!-- ── OVERVIEW ── -->
<div id="tab-overview" class="tab active">
  <div class="th"><h1>Overview</h1>
    <div class="th-acts"><button class="primary" onclick="loadOverview()">↻ Refresh</button></div>
  </div>
  <div class="sg">
    <div class="sc"><div class="sl">Uptime</div><div class="sv" id="s-up">—</div></div>
    <div class="sc"><div class="sl">Memory</div><div class="sv" id="s-mu">—</div><div class="ss" id="s-mt"></div></div>
    <div class="sc"><div class="sl">Disk (/)</div><div class="sv" id="s-du">—</div><div class="ss" id="s-dp"></div></div>
    <div class="sc"><div class="sl">Load avg</div><div class="sv" id="s-la">—</div><div class="ss">1 / 5 / 15 min</div></div>
  </div>
  <div id="dementor-status"></div>
  <div class="pills">
    <div class="pill"><span class="dot" id="d-fw"></span><span id="t-fw">Firewall…</span></div>
    <div class="pill"><span class="dot" id="d-cl"></span><span id="t-cl">ClamAV…</span></div>
    <div class="pill"><span class="dot" id="d-dk"></span><span id="t-dk">DCAK…</span></div>
    <div class="pill"><span class="dot" id="d-gd"></span><span id="t-gd">Protection…</span></div>
  </div>
  <div class="card">
    <div class="th" style="margin:0 0 8px;"><h2 style="margin:0;">Protection (Guardian)</h2>
      <div class="th-acts">
        <span id="rt-badge" class="pill"><span class="dot"></span>Real-time: …</span>
        <button onclick="scanNow()">Scan now</button>
      </div>
    </div>
    <div id="rt-stats" class="hint" style="margin:0 0 8px;">Checking real-time protection…</div>
    <pre id="guardian-out" style="max-height:220px;">Loading…</pre>
    <div class="hint">Real-time protection actively watches your Downloads, Desktop, /tmp and USB drives
      and scans each new file the instant it lands. The Guardian also sweeps in the background — after
      boot, once a day, and on USB insert. You don't need to keep DADA open; this shows what it found.</div>
  </div>
  <div class="card">
    <div class="th" style="margin:0 0 8px;"><h2 style="margin:0;">Threat Log — caught Dementors</h2>
      <div class="th-acts"><button onclick="loadThreatLog()">↻ Refresh</button></div>
    </div>
    <pre id="threat-out" style="max-height:200px;">Loading…</pre>
    <div class="hint">Recent detections from real-time watch and scheduled scans. Files are detected
      (and optionally quarantined) — never silently deleted.</div>
  </div>
  <div class="card"><h2>Recent Logins</h2><pre id="logins-out">Loading…</pre></div>
</div>

<!-- ── FIREWALL ── -->
<div id="tab-firewall" class="tab">
  <div class="th"><h1>Firewall</h1>
    <div class="th-acts">
      <button onclick="fwRefresh()">↻ Refresh</button>
      <button class="primary" onclick="fwEnable()">Enable</button>
      <button class="danger" onclick="fwDisable()">Disable</button>
    </div>
  </div>
  <div id="fw-banner" class="banner off">● Status unknown</div>
  <div class="card"><h2>Rules</h2><pre id="fw-rules">Click Refresh.</pre></div>
  <div class="card">
    <h2>Add Rule</h2>
    <div class="fr">
      <input type="text" id="fw-port" placeholder="port or service  (22, 80, ssh)" style="width:230px"/>
      <select id="fw-proto"><option value="tcp">TCP</option><option value="udp">UDP</option><option value="any">Any</option></select>
      <select id="fw-pol"><option value="allow">Allow</option><option value="deny">Deny</option></select>
      <button class="primary" onclick="fwAdd()">Add</button>
    </div>
    <h2 style="margin-top:10px;">Delete Rule</h2>
    <div class="fr">
      <input type="text" id="fw-del" placeholder="rule number from list above" style="width:210px"/>
      <button class="danger" onclick="fwDel()">Delete</button>
    </div>
    <pre id="fw-out" style="margin-top:10px;max-height:120px;"></pre>
    <div class="hint">Rule numbers appear in brackets in the Rules list above.</div>
  </div>
</div>

<!-- ── SCANNER ── -->
<div id="tab-scanner" class="tab">
  <div class="th"><h1>Scanner</h1>
    <div class="th-acts"><button onclick="clamUpdate()">Update DB (freshclam)</button></div>
  </div>
  <div class="card">
    <h2>Scan</h2>
    <div class="fr">
      <button onclick="doScan('home')">Home</button>
      <button onclick="doScan('tmp')">/ tmp</button>
      <button onclick="doScan('downloads')">Downloads</button>
    </div>
    <div class="fr">
      <input type="text" id="scan-path" placeholder="custom path  (e.g. /opt/devApp)" style="width:280px"/>
      <button class="primary" onclick="doScan('custom')">Scan Path</button>
    </div>
    <div id="scan-status" style="font-size:11px;color:var(--muted);min-height:16px;margin-bottom:6px;"></div>
    <pre id="scan-out" style="max-height:360px;">No scan run yet.</pre>
    <div class="hint">Only infected files are shown. Exit 0 = clean, 1 = threats found.</div>
  </div>
  <div class="card"><h2>DB Update Output</h2><pre id="freshclam-out" style="max-height:200px;">Not run.</pre></div>
</div>

<!-- ── NETWORK ── -->
<div id="tab-network" class="tab">
  <div class="th"><h1>Network</h1>
    <div class="th-acts"><button onclick="loadNetwork()">↻ Refresh</button></div>
  </div>
  <div class="card"><h2>Listening Ports</h2><pre id="ports-out" class="mono">Click Refresh.</pre></div>
  <div class="card"><h2>Active Connections</h2><pre id="conn-out" class="mono">Click Refresh.</pre></div>
</div>

<!-- ── PROCESSES ── -->
<div id="tab-procs" class="tab">
  <div class="th"><h1>Processes</h1>
    <div class="th-acts"><button onclick="loadProcs()">↻ Refresh</button></div>
  </div>
  <div class="card">
    <h2>Top by CPU</h2>
    <div style="overflow-x:auto;">
      <table>
        <thead><tr><th>User</th><th>PID</th><th>CPU%</th><th>MEM%</th><th>Command</th><th></th></tr></thead>
        <tbody id="proc-body"><tr><td colspan="6" style="color:var(--muted)">Click Refresh.</td></tr></tbody>
      </table>
    </div>
  </div>
  <div class="card">
    <h2>Kill by PID</h2>
    <div class="fr">
      <input type="text" id="kill-pid" placeholder="PID" style="width:90px"/>
      <button class="danger" onclick="doKill(document.getElementById('kill-pid').value.trim())">Send SIGTERM</button>
    </div>
    <pre id="kill-out" style="max-height:80px;"></pre>
  </div>
</div>

<!-- ── DCAK ── -->
<div id="tab-dcak" class="tab">
  <div class="th"><h1>DCAK</h1>
    <div class="th-acts"><button onclick="loadDcak()">↻ Refresh</button></div>
  </div>
  <div class="card">
    <h2>Authorization Key</h2>
    <div class="pills" style="margin-bottom:0;">
      <div class="pill"><span class="dot" id="d-dk2"></span><span id="t-dk2">Checking…</span></div>
    </div>
    <pre id="dcak-out" style="margin-top:12px;max-height:120px;"></pre>
  </div>
  <div class="card">
    <h2>Locked Paths</h2>
    <pre id="locked-out">Loading…</pre>
    <div class="hint">Source: /etc/leviathanos/locks.d/</div>
  </div>
</div>

<!-- ── DISKS ── -->
<div id="tab-disks" class="tab">
  <div class="th"><h1>Disks</h1>
    <div class="th-acts"><button onclick="diskList()">↻ Refresh drives</button></div>
  </div>
  <div class="card">
    <h2>Your drives</h2>
    <pre id="disk-list" style="max-height:240px;">Loading…</pre>
    <div class="hint">Pick a partition below by its NAME (e.g. <b>sda3</b>, <b>nvme0n1p5</b>) — not the whole disk.</div>
  </div>
  <div class="card">
    <h2>Check &amp; repair a drive</h2>
    <div class="fr">
      <input type="text" id="disk-dev" placeholder="partition name  (e.g. sda3)" style="width:220px"/>
      <button onclick="diskDo('check')">Quick check (read-only)</button>
      <button class="primary" onclick="diskDo('repair')">Repair / clear dirty flag</button>
    </div>
    <div id="disk-status" style="font-size:11px;color:var(--muted);min-height:16px;margin:6px 0;"></div>
    <pre id="disk-out" style="max-height:320px;">No check run yet.</pre>
    <div class="hint">A <b>quick check</b> never writes — safe anytime. A <b>repair</b> writes, so the
      drive must be UNMOUNTED first (eject it in the file manager). Repair clears the "dirty"
      flag and fixes common errors: <b>ntfsfix</b> for Windows (NTFS), <b>fsck</b> for Linux/USB drives.</div>
  </div>
  <div class="card" style="border-color:#7a2222;">
    <h2 style="color:#ff6b6b;">⚠ Force-mount a refused drive</h2>
    <p style="font-size:12px;line-height:1.5;color:var(--muted);margin:2px 0 10px;">
      Leviathan refuses to write to a Windows drive that was <b>hibernated</b> or shut down
      with <b>Fast Startup</b> ON, because writing to it can <b>corrupt Windows</b>. The clean
      fix is to turn Fast Startup off in Windows (see the help page 7-IF-IT-BREAKS).<br><br>
      This button ignores that guard and mounts the drive <b>read-write anyway</b>. For a
      hibernated NTFS drive it will <b>discard the saved Windows session</b> (any unsaved work
      in Windows is lost) to make the drive writable. <b>Your drive, your call — but if it
      corrupts, that is on you, not Leviathan.</b> Run a Quick check first.
    </p>
    <div class="fr">
      <input type="text" id="disk-force-dev" placeholder="partition name  (e.g. sda3)" style="width:220px"/>
      <button style="background:#7a2222;color:#fff;" onclick="diskForce()">I accept the risk — force-mount</button>
    </div>
    <pre id="disk-force-out" style="max-height:180px;margin-top:10px;">Nothing mounted.</pre>
  </div>
</div>

<!-- ── ABOUT / LICENSE ── -->
<div id="tab-about" class="tab">
  <div class="th"><h1>About</h1></div>
  <div class="card" style="max-width:560px;">
    <h2>License</h2>
    <div style="font-size:12.5px;line-height:1.8;color:var(--text);">
      DADA — part of LeviathanOS<br/>
      Free software under the GNU General Public License, version 3.<br/>
      This program comes with ABSOLUTELY NO WARRANTY.
    </div>
    <div class="hint" style="line-height:1.8;">
      Full license: /usr/share/doc/leviathanos/LICENSE<br/>
      <a href="https://www.gnu.org/licenses/gpl-3.0.html" style="color:var(--accent);">https://www.gnu.org/licenses/gpl-3.0.html</a>
    </div>
  </div>
</div>

</div><!-- #content -->
<script>
// ── Tab router ────────────────────────────────────────────────────────────
function switchTab(name, el) {
  document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
  document.querySelectorAll('.ni').forEach(n => n.classList.remove('active'));
  document.getElementById('tab-' + name).classList.add('active');
  el.classList.add('active');
  ({overview:loadOverview, firewall:fwRefresh, scanner:()=>{},
    network:loadNetwork, procs:loadProcs, disks:diskList, dcak:loadDcak})[name]?.();
}

// ── Core RPC ─────────────────────────────────────────────────────────────
// Pass a single object so the C++ parser finds "method" in the JSON.
async function call(method, params) {
  return await window.dada({ method, ...(params || {}) });
}

function dot(did, tid, cls, txt) {
  document.getElementById(did).className = 'dot ' + (cls || '');
  document.getElementById(tid).textContent = txt;
}

// ── Overview ─────────────────────────────────────────────────────────────
async function loadOverview() {
  const [sys, logins, fw, clam, dcak, guard, rt] = await Promise.all([
    call('sys_info'), call('last_logins'),
    call('ufw_status'), call('clamav_version'), call('dcak_status'),
    call('guardian_status'), call('guardian_realtime'),
  ]);

  (sys.out || '').split('\n').forEach(ln => {
    if (ln.startsWith('UP:'))   document.getElementById('s-up').textContent = ln.slice(3).trim();
    if (ln.startsWith('MU:'))   document.getElementById('s-mu').textContent = ln.slice(3).trim();
    if (ln.startsWith('MT:'))   document.getElementById('s-mt').textContent = 'of ' + ln.slice(3).trim();
    if (ln.startsWith('DU:'))   document.getElementById('s-du').textContent = ln.slice(3).trim();
    if (ln.startsWith('DP:'))   document.getElementById('s-dp').textContent = ln.slice(3).trim() + ' used';
    if (ln.startsWith('LA:'))   document.getElementById('s-la').textContent = ln.slice(3).trim();
  });

  const fwOn  = fw.exit_code === 0 && /Status:\s+active/i.test(fw.out);
  const fwMis = /not found/i.test(fw.out);
  dot('d-fw','t-fw', fwMis?'bad':(fwOn?'good':''), fwMis?'Firewall: missing':(fwOn?'Firewall: active':'Firewall: inactive'));

  const clOk  = clam.exit_code === 0 && clam.out.trim().length > 0;
  const clMis = /not found/i.test(clam.out);
  dot('d-cl','t-cl', clMis?'bad':(clOk?'good':''), clMis?'ClamAV: missing':(clOk?'ClamAV: ready':'ClamAV: unknown'));

  const dkOk = dcak.exit_code === 0 && /initialized/i.test(dcak.out);
  dot('d-dk','t-dk', dkOk?'good':'bad', dkOk?'DCAK: initialized':'DCAK: not set');

  // Guardian (background scanner) status
  const gout = (guard.out || '').trim();
  const gstate = (gout.match(/^state:\s*(\S+)/m) || [,'unknown'])[1];
  const gMap = {
    clean:   ['good','Protection: clean'],
    scanning:['','Protection: scanning…'],
    threats: ['bad','Protection: THREATS found'],
    idle:    ['good','Protection: idle'],
    'no-engine':['bad','Protection: no engine'],
    'never-run':['','Protection: not run yet'],
    error:   ['bad','Protection: scan error'],
  };
  const g = gMap[gstate] || ['','Protection: '+gstate];
  dot('d-gd','t-gd', g[0], g[1]);
  document.getElementById('guardian-out').textContent = gout || '(the Guardian has not run yet — it will after boot / on its daily timer)';
  let issuesGuardian = null;
  if (gstate === 'no-engine') issuesGuardian = 'ClamAV engine is not installed (Guardian can’t scan)';
  else if (gstate === 'threats') issuesGuardian = 'Guardian found threats — review the Protection panel';

  // Real-time protection (inotify watch) status
  const rtOn = renderRealtime(rt);

  // Threat log (caught dementors)
  loadThreatLog();

  // Dementor status banner
  const issues = [];
  if (!rtOn) issues.push('Real-time protection is OFF');
  if (fwMis)       issues.push('Firewall (ufw) is not installed');
  else if (!fwOn)  issues.push('Firewall is inactive');
  if (clMis)       issues.push('ClamAV is not installed');
  else if (!clOk)  issues.push('ClamAV status unknown');
  if (!dkOk)       issues.push('DCAK authorization key is not initialized');
  if (issuesGuardian) issues.push(issuesGuardian);

  const banner = document.getElementById('dementor-status');
  if (issues.length === 0) {
    banner.className = 'dementor-ok';
    banner.innerHTML = '<span>✦</span> No Dementors detected';
  } else {
    banner.className = 'dementor-bad';
    banner.innerHTML =
      '<div class="issues">' + issues.map(i => '⚠ ' + i).join('<br/>') + '</div>' +
      '<div class="label">Dementor detected</div>';
  }

  document.getElementById('logins-out').textContent = logins.out.trim() || '(no history)';
}

// ── Firewall ─────────────────────────────────────────────────────────────
async function fwRefresh() {
  const [st, rules] = await Promise.all([call('ufw_status'), call('ufw_rules')]);
  const on = st.exit_code === 0 && /Status:\s+active/i.test(st.out);
  const b = document.getElementById('fw-banner');
  b.className = 'banner ' + (on ? 'on' : 'off');
  b.textContent = on ? '● Firewall is ACTIVE' : '● Firewall is INACTIVE';
  document.getElementById('fw-rules').textContent = rules.out.trim() || '(no rules)';
}
async function fwEnable()  { document.getElementById('fw-out').textContent=(await call('ufw_enable')).out;  fwRefresh(); }
async function fwDisable() { document.getElementById('fw-out').textContent=(await call('ufw_disable')).out; fwRefresh(); }
async function fwAdd() {
  const port=document.getElementById('fw-port').value.trim();
  const proto=document.getElementById('fw-proto').value;
  const pol=document.getElementById('fw-pol').value;
  if (!port){document.getElementById('fw-out').textContent='Enter a port.';return;}
  document.getElementById('fw-out').textContent=(await call('ufw_add_rule',{port,proto,policy:pol})).out;
  fwRefresh();
}
async function fwDel() {
  const n=document.getElementById('fw-del').value.trim();
  if (!n){document.getElementById('fw-out').textContent='Enter a rule number.';return;}
  document.getElementById('fw-out').textContent=(await call('ufw_delete_rule',{rule_num:n})).out;
  fwRefresh();
}

// ── Scanner ───────────────────────────────────────────────────────────────
async function doScan(target) {
  const st=document.getElementById('scan-status');
  const out=document.getElementById('scan-out');
  let method='clam_scan_home', params={};
  if      (target==='tmp')       { method='clam_scan_tmp'; }
  else if (target==='downloads') { method='clam_scan_downloads'; }
  else if (target==='custom') {
    const p=document.getElementById('scan-path').value.trim();
    if (!p){out.textContent='Enter a path.';return;}
    method='clam_scan_path'; params={path:p};
  }
  st.innerHTML='<span class="spin">⟳</span> Scanning…';
  out.textContent='';
  const r = await call(method, params);
  st.textContent = r.exit_code===0?'✓ Clean':r.exit_code===1?'⚠ Threats found':'✗ Error (exit '+r.exit_code+')';
  out.innerHTML='';
  (r.out||'').split('\n').forEach(ln=>{
    const sp=document.createElement('span');
    sp.textContent=ln+'\n';
    if (/FOUND$/i.test(ln.trim())) sp.className='infected';
    out.appendChild(sp);
  });
  if (!r.out.trim()) out.textContent='(no output)';
}
async function clamUpdate() {
  document.getElementById('freshclam-out').textContent='⟳ Running freshclam…';
  document.getElementById('freshclam-out').textContent=(await call('freshclam_update')).out||'(no output)';
}

// ── Guardian ──────────────────────────────────────────────────────────────
async function scanNow() {
  const out=document.getElementById('guardian-out');
  out.textContent='⟳ Starting a background scan…';
  const r=await call('guardian_scan_now');
  out.textContent=(r.out||'').trim()||'Scan started in the background. It runs at low priority — click ↻ Refresh in a bit to see the result.';
}

// Real-time watch: renders the ACTIVE/off badge + stats. Returns true if ACTIVE.
function renderRealtime(rt) {
  const t = (rt && rt.out) || '';
  const on = /^watch:\s*on\b/mi.test(t);
  const grab = (k) => { const m = t.match(new RegExp('^'+k+':\\s*(.+)$','mi')); return m ? m[1].trim() : ''; };
  const scanned  = grab('scanned')  || '0';
  const detected = grab('detected') || '0';
  const lastdet  = grab('lastdetection');
  const note     = grab('note');

  const badge = document.getElementById('rt-badge');
  badge.innerHTML = '<span class="dot ' + (on ? 'good' : 'bad') + '"></span>' +
    (on ? 'Real-time protection: ACTIVE' : 'Real-time protection: OFF');

  const stats = document.getElementById('rt-stats');
  let s = on
    ? ('Actively watching Downloads / Desktop / /tmp / USB — ' + scanned +
       ' file(s) scanned today, ' + detected + ' detection(s).')
    : ('Real-time watch is off' + (note ? ' — ' + note : ' (install inotify-tools + clamav to enable).'));
  if (lastdet) s += '  Last detection: ' + lastdet;
  stats.textContent = s;
  return on;
}

async function loadThreatLog() {
  const el = document.getElementById('threat-out');
  const r = await call('guardian_threatlog');
  const t = ((r && r.out) || '').trim();
  el.innerHTML = '';
  if (!t) { el.textContent = 'No detections recorded — no Dementors have been caught. ✦'; return; }
  t.split('\n').forEach(ln => {
    const sp = document.createElement('span');
    sp.textContent = ln + '\n';
    sp.className = 'infected';
    el.appendChild(sp);
  });
}

// ── Disks ─────────────────────────────────────────────────────────────────
async function diskList() {
  const el=document.getElementById('disk-list');
  el.textContent='⟳ Reading drives…';
  el.textContent=(await call('disk_list')).out||'(no drives found)';
}
async function diskDo(mode) {
  const dev=document.getElementById('disk-dev').value.trim().replace(/^\/dev\//,'');
  const st=document.getElementById('disk-status');
  const out=document.getElementById('disk-out');
  if (!dev){out.textContent='Enter a partition name first (e.g. sda3).';return;}
  st.innerHTML='<span class="spin">⟳</span> '+(mode==='repair'?'Repairing':'Checking')+' /dev/'+dev+' …';
  out.textContent='';
  const r=await call(mode==='repair'?'disk_repair':'disk_check',{dev});
  st.textContent = r.exit_code===0?'✓ Done':'✗ Finished with warnings (exit '+r.exit_code+')';
  out.textContent = (r.out||'').trim()||'(no output)';
}
async function diskForce() {
  const dev=document.getElementById('disk-force-dev').value.trim().replace(/^\/dev\//,'');
  const out=document.getElementById('disk-force-out');
  if (!dev){out.textContent='Enter a partition name first (e.g. sda3).';return;}
  if (!confirm('Force-mount /dev/'+dev+' read-write?\n\nIf it is a hibernated Windows drive, the saved Windows session will be DISCARDED. This can corrupt Windows. You accept the risk.')) return;
  out.textContent='⟳ Force-mounting /dev/'+dev+' …';
  out.textContent=(await call('disk_forcemount',{dev})).out||'(no output)';
}

// ── Network ───────────────────────────────────────────────────────────────
async function loadNetwork() {
  const [p,c]=await Promise.all([call('open_ports'),call('net_connections')]);
  document.getElementById('ports-out').textContent=p.out.trim()||'(none)';
  document.getElementById('conn-out').textContent=c.out.trim()||'(none)';
}

// ── Processes ─────────────────────────────────────────────────────────────
async function loadProcs() {
  const r=await call('top_procs');
  const tb=document.getElementById('proc-body');
  tb.innerHTML='';
  // ps aux: USER PID %CPU %MEM VSZ RSS TTY STAT START TIME COMMAND...
  (r.out||'').split('\n').slice(1).forEach(ln=>{
    const p=ln.trim().split(/\s+/);
    if (p.length<11) return;
    const [user,pid,cpu,mem]=p;
    const cmd=p.slice(10).join(' ').slice(0,56);
    const tr=document.createElement('tr');
    tr.innerHTML=`<td>${user}</td><td class="mono">${pid}</td><td>${cpu}</td><td>${mem}</td>
      <td class="mono" title="${cmd}">${cmd}</td>
      <td><button class="danger" style="padding:3px 8px;font-size:11px" onclick="doKill('${pid}')">kill</button></td>`;
    tb.appendChild(tr);
  });
  if (!tb.children.length) tb.innerHTML='<tr><td colspan="6" style="color:var(--muted)">No data.</td></tr>';
}
async function doKill(pid) {
  if (!pid) return;
  const r=await call('kill_proc',{pid});
  document.getElementById('kill-out').textContent=r.out||(r.exit_code===0?'SIGTERM sent to '+pid:'Failed');
  loadProcs();
}

// ── DCAK ──────────────────────────────────────────────────────────────────
async function loadDcak() {
  const [dk,lk]=await Promise.all([call('dcak_status'),call('locked_paths')]);
  const ok=dk.exit_code===0&&/initialized/i.test(dk.out);
  dot('d-dk2','t-dk2',ok?'good':'bad',ok?'DCAK: initialized':'DCAK: not initialized');
  document.getElementById('dcak-out').textContent=dk.out.trim()||'(no output)';
  document.getElementById('locked-out').textContent=lk.out.trim()||'(empty)';
}

loadOverview();
</script></body></html>
)HTML";

  w.bind("dada", [](std::string req) -> std::string {
    // req = JSON array of args: [{"method":"name","key":"val",...}]
    // Extract method name
    auto mp = req.find("\"method\"");
    if (mp == std::string::npos) return R"({"exit_code":1,"out":"bad request"})";
    mp = req.find(':', mp);
    if (mp == std::string::npos) return R"({"exit_code":1,"out":"bad request"})";
    mp = req.find('"', mp);
    if (mp == std::string::npos) return R"({"exit_code":1,"out":"bad request"})";
    auto me = req.find('"', mp + 1);
    if (me == std::string::npos) return R"({"exit_code":1,"out":"bad request"})";
    const std::string m = req.substr(mp + 1, me - mp - 1);

    CmdResult r;

    // ── System ──────────────────────────────────────────────────────────
    if (m == "sys_info") {
      r = RunCmd({"bash","-lc",
        "printf 'UP:'; uptime -p 2>/dev/null || uptime;"
        "printf 'MU:'; free -h 2>/dev/null | awk 'NR==2{print $3}';"
        "printf 'MT:'; free -h 2>/dev/null | awk 'NR==2{print $2}';"
        "printf 'DU:'; df -h / 2>/dev/null | awk 'NR==2{print $3}';"
        "printf 'DP:'; df -h / 2>/dev/null | awk 'NR==2{print $5}';"
        "printf 'LA:'; awk '{print $1,$2,$3}' /proc/loadavg;"});
    } else if (m == "last_logins") {
      r = RunCmd({"bash","-lc","last -n 10 -F 2>/dev/null | head -12"});

    // ── Firewall ─────────────────────────────────────────────────────────
    } else if (m == "ufw_status") {
      r = RunCmd({"bash","-lc","command -v ufw >/dev/null 2>&1 && ufw status verbose || echo 'ufw not found'"});
    } else if (m == "ufw_rules") {
      r = RunCmd({"bash","-lc","command -v ufw >/dev/null 2>&1 && ufw status numbered || echo 'ufw not found'"});
    } else if (m == "ufw_enable") {
      r = RunCmd({"bash","-lc","command -v ufw >/dev/null 2>&1 && ufw --force enable || echo 'ufw not found'"});
    } else if (m == "ufw_disable") {
      r = RunCmd({"bash","-lc","command -v ufw >/dev/null 2>&1 && ufw disable || echo 'ufw not found'"});
    } else if (m == "ufw_add_rule") {
      const std::string port   = ExtractParam(req, "port");
      const std::string proto  = ExtractParam(req, "proto");
      const std::string policy = ExtractParam(req, "policy");
      if (!SafePort(port) || !SafeProto(proto) || !SafePolicy(policy)) {
        r = {1, "invalid params"};
      } else {
        const std::string spec = port + (proto != "any" ? "/" + proto : "");
        r = RunCmd({"bash","-lc",
          "command -v ufw >/dev/null 2>&1 && ufw " + policy + " " + spec + " || echo 'ufw not found'"});
      }
    } else if (m == "ufw_delete_rule") {
      const std::string n = ExtractParam(req, "rule_num");
      if (!IsNum(n)) { r = {1, "invalid rule number"}; }
      else { r = RunCmd({"bash","-lc",
        "command -v ufw >/dev/null 2>&1 && ufw --force delete " + n + " || echo 'ufw not found'"});}

    // ── Scanner ───────────────────────────────────────────────────────────
    } else if (m == "clamav_version") {
      r = RunCmd({"bash","-lc","command -v clamscan >/dev/null 2>&1 && clamscan --version || echo 'clamscan not found'"});
    } else if (m == "clam_scan_home") {
      r = RunCmd({"bash","-lc",
        "command -v clamscan >/dev/null 2>&1 || { echo 'clamscan not found'; exit 1; };"
        "clamscan -r --infected --no-summary \"$HOME\" 2>&1 | head -n 500"});
    } else if (m == "clam_scan_tmp") {
      r = RunCmd({"bash","-lc",
        "command -v clamscan >/dev/null 2>&1 || { echo 'clamscan not found'; exit 1; };"
        "clamscan -r --infected --no-summary /tmp 2>&1 | head -n 500"});
    } else if (m == "clam_scan_downloads") {
      r = RunCmd({"bash","-lc",
        "command -v clamscan >/dev/null 2>&1 || { echo 'clamscan not found'; exit 1; };"
        "clamscan -r --infected --no-summary \"$HOME/Downloads\" 2>&1 | head -n 500"});
    } else if (m == "clam_scan_path") {
      // Path is passed as positional $1 to avoid double-escaping issues.
      const std::string path = ExtractParam(req, "path");
      if (path.empty()) { r = {1, "no path provided"}; }
      else {
        r = RunCmd({"bash","-lc",
          "command -v clamscan >/dev/null 2>&1 || { echo 'clamscan not found'; exit 1; };"
          "clamscan -r --infected --no-summary \"$1\" 2>&1 | head -n 500",
          "_", path});
      }
    } else if (m == "freshclam_update") {
      r = RunCmd({"bash","-lc","command -v freshclam >/dev/null 2>&1 && freshclam || echo 'freshclam not found'"});

    // ── Network ───────────────────────────────────────────────────────────
    } else if (m == "open_ports") {
      r = RunCmd({"bash","-lc",
        "ss -tlnp 2>/dev/null || netstat -tlnp 2>/dev/null || echo 'ss/netstat not available'"});
    } else if (m == "net_connections") {
      r = RunCmd({"bash","-lc","ss -tnp 2>/dev/null | head -40 || echo 'ss not available'"});

    // ── Processes ─────────────────────────────────────────────────────────
    } else if (m == "top_procs") {
      r = RunCmd({"bash","-lc","ps aux --sort=-%cpu 2>/dev/null | head -16"});
    } else if (m == "kill_proc") {
      const std::string pid = ExtractParam(req, "pid");
      if (!IsNum(pid)) { r = {1, "invalid PID"}; }
      else { r = RunCmd({"bash","-lc",
        "kill " + pid + " && echo 'SIGTERM sent to " + pid + "' || echo 'kill failed (check permissions)'"});}

    // ── GUARDIAN (background scanner) ─────────────────────────────────────
    } else if (m == "guardian_status") {
      r = RunCmd({"bash","-lc",
        "if command -v leviathanos-guardian >/dev/null 2>&1; then leviathanos-guardian status;"
        "elif [ -f /var/lib/leviathanos/guardian/last-scan.txt ]; then cat /var/lib/leviathanos/guardian/last-scan.txt;"
        "else echo 'state: never-run'; fi"});
    } else if (m == "guardian_scan_now") {
      // Kick a quick scan without blocking the UI. Prefer the systemd unit so
      // it runs at idle priority; fall back to a detached run.
      r = RunCmd({"bash","-lc",
        "command -v leviathanos-guardian >/dev/null 2>&1 || { echo 'Guardian is not installed.'; exit 1; };"
        "if command -v systemctl >/dev/null 2>&1 && systemctl start --no-block leviathanos-guardian.service 2>/dev/null; then"
        "  echo 'Scan started (systemd). Refresh in a bit.';"
        "else setsid leviathanos-guardian scan-quick >/dev/null 2>&1 < /dev/null & echo 'Scan started. Refresh in a bit.'; fi"});
    } else if (m == "guardian_realtime") {
      // Live real-time (inotify watch) status: watching on/off, files scanned
      // today, last detection. Read via the tool; fall back to the state file.
      r = RunCmd({"bash","-lc",
        "if command -v leviathanos-guardian >/dev/null 2>&1; then leviathanos-guardian watch-status;"
        "elif [ -f /var/lib/leviathanos/guardian/realtime.txt ]; then cat /var/lib/leviathanos/guardian/realtime.txt;"
        "else echo 'watch: off'; fi"});
    } else if (m == "guardian_threatlog") {
      // Recent detections (caught 'dementors') from real-time watch + scans.
      r = RunCmd({"bash","-lc",
        "if command -v leviathanos-guardian >/dev/null 2>&1; then leviathanos-guardian threat-log;"
        "elif [ -f /var/lib/leviathanos/guardian/guardian.log ]; then grep -E 'THREAT:| FOUND$' /var/lib/leviathanos/guardian/guardian.log | tail -n 50;"
        "else echo ''; fi"});

    // ── DISKS ─────────────────────────────────────────────────────────────
    } else if (m == "disk_list") {
      r = RunCmd({"bash","-lc",
        "lsblk -o NAME,SIZE,TYPE,FSTYPE,LABEL,MOUNTPOINT 2>/dev/null || echo 'lsblk not found'"});
    } else if (m == "disk_check" || m == "disk_repair" || m == "disk_forcemount") {
      // Partition name passed as positional $1; validated in-shell to a device name.
      const std::string dev = ExtractParam(req, "dev");
      if (dev.empty()) { r = {1, "no device provided"}; }
      else {
        // Shared preamble: sanitize name, resolve /dev path, detect fs, need root.
        std::string pre =
          "n=$1;"
          "case \"$n\" in *[!A-Za-z0-9_]*) echo 'Invalid partition name.'; exit 2;; esac;"
          "d=/dev/$n;"
          "[ -b \"$d\" ] || { echo \"Not a block device: $d  (run Refresh drives to see valid names)\"; exit 2; };"
          "t=$(lsblk -dno FSTYPE \"$d\" 2>/dev/null);"
          "mp=$(lsblk -dno MOUNTPOINT \"$d\" 2>/dev/null);"
          "SUDO=; [ \"$(id -u)\" != 0 ] && SUDO='sudo -n';";
        if (m == "disk_check") {
          r = RunCmd({"bash","-lc", pre +
            "echo \"Device : $d\"; echo \"Format : ${t:-unknown}\";"
            "[ -n \"$mp\" ] && echo \"Mounted: $mp  (this read-only check is still safe)\";"
            "echo '----------------------------------------';"
            "case \"$t\" in"
            "  ntfs) echo '[NTFS] dirty-flag / structure check (read-only):';"
            "        $SUDO ntfsfix -n \"$d\" 2>&1 || echo '(ntfsfix reported issues — a Repair may be needed)';;"
            "  ext2|ext3|ext4) echo '[ext] read-only check:'; $SUDO fsck -fn \"$d\" 2>&1 || echo '(errors found — run Repair while unmounted)';;"
            "  vfat|exfat|msdos) echo '[FAT] read-only check:'; $SUDO fsck -n \"$d\" 2>&1 || echo '(errors found — run Repair while unmounted)';;"
            "  '') echo 'No filesystem detected (empty or unknown).';;"
            "  *) echo \"No read-only checker built in for '$t'.\";;"
            "esac",
            "_", dev});
        } else if (m == "disk_repair") {
          r = RunCmd({"bash","-lc", pre +
            "if [ -n \"$mp\" ]; then echo \"REFUSED: $d is mounted at $mp.\"; echo 'Eject/unmount it in the file manager first, then repair.'; exit 1; fi;"
            "echo \"Repairing $d (${t:-unknown}) …\"; echo '----------------------------------------';"
            "case \"$t\" in"
            "  ntfs) $SUDO ntfsfix \"$d\" 2>&1;;"                     // clears dirty flag + common fixes
            "  ext2|ext3|ext4) $SUDO fsck -y \"$d\" 2>&1;;"
            "  vfat|exfat|msdos) $SUDO fsck -y \"$d\" 2>&1;;"
            "  '') echo 'No filesystem detected — nothing to repair.';;"
            "  *) echo \"No repair tool built in for '$t'.\";;"
            "esac",
            "_", dev});
        } else { // disk_forcemount
          r = RunCmd({"bash","-lc", pre +
            "if [ -n \"$mp\" ]; then echo \"Already mounted at $mp — nothing to do.\"; exit 0; fi;"
            "lbl=$(lsblk -dno LABEL \"$d\" 2>/dev/null); lbl=${lbl:-$n};"
            "mnt=/media/${SUDO_USER:-$USER}/$lbl;"
            "$SUDO mkdir -p \"$mnt\" 2>&1 || { echo 'Could not create mount point (need root?).'; exit 1; };"
            "case \"$t\" in"
            "  ntfs) $SUDO mount -t ntfs-3g -o rw,remove_hiberfile,windows_names \"$d\" \"$mnt\" 2>&1;;"
            "  *) $SUDO mount -o rw \"$d\" \"$mnt\" 2>&1;;"
            "esac;"
            "s=$?;"
            "if [ $s -eq 0 ]; then echo \"Force-mounted $d read-write at $mnt.\"; echo 'You accepted the risk. Consider a Quick check if anything looks off.';"
            "else echo \"Mount failed (exit $s). Try a Repair first, or run DADA as root.\"; fi",
            "_", dev});
        }
      }

    // ── DCAK ──────────────────────────────────────────────────────────────
    } else if (m == "dcak_status") {
      r = RunCmd({"bash","-lc",
        "f=/etc/leviathanos/dcak.hash;"
        "if [ -f \"$f\" ] && [ -s \"$f\" ]; then"
        "  echo 'DCAK initialized';"
        "  echo \"File: $f\";"
        "  echo \"Hash: $(cat $f)\";"
        "else echo 'DCAK not initialized — run: sudo leviathanos-dcak-init'; fi"});
    } else if (m == "locked_paths") {
      r = RunCmd({"bash","-lc",
        "echo '=== OS-locked ==='; cat /etc/leviathanos/locks.d/os.locked 2>/dev/null || echo '(not found)';"
        "echo ''; echo '=== User-locked ===';"
        "cat /etc/leviathanos/locks.d/user.locked 2>/dev/null || echo '(not found)';"});

    } else {
      r = {1, "unknown method: " + m};
    }

    return ToJson(r);
  });

  w.set_html(html);
  w.run();
  return 0;
#endif
}
