// Leviathan Helm — System Control Center
//
// The machine-side counterpart to DADA (which owns SECURITY: firewall, AV,
// network, DCAK, Guardian, disk repair). Helm owns HARDWARE & SYSTEM control:
// hardware inventory, live sensors/temps, systemd services (start/stop/
// enable/disable), startup apps, package updates, storage breakdown, system
// logs, boot info, and a read-only resource monitor.
//
// Stack: bundled webview.h + GTK3 + WebKit2GTK-4.1. Backends are REAL shell
// commands run through RunCommandCapture(), mirroring DADA's RunCmd pattern.
//
// Build (from repo root; do NOT use the repo Makefile):
//   g++ -std=c++20 -O2 -Ithird_party/webview src/legacy_dashboard/main.cpp
//     -o src/legacy_dashboard/legacy_dashboard
//     $(pkg-config --cflags --libs gtk+-3.0 webkit2gtk-4.1)

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <sys/wait.h>

#if __has_include("webview.h")
#include "webview.h"
#define LEVI_HAS_WEBVIEW 1
#elif __has_include("third_party/webview/webview.h")
#include "third_party/webview/webview.h"
#define LEVI_HAS_WEBVIEW 1
#else
#define LEVI_HAS_WEBVIEW 0
#endif

namespace {

#if LEVI_HAS_WEBVIEW
std::string ShellEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  out.push_back('\'');
  for (char c : s) {
    if (c == '\'') out += "'\\''";
    else out.push_back(c);
  }
  out.push_back('\'');
  return out;
}

struct CmdResult {
  int exit_code = 0;
  std::string out;
};

CmdResult RunCommandCapture(const std::vector<std::string>& args) {
  if (args.empty()) return {.exit_code = 127, .out = "no command"};
  std::ostringstream cmd;
  for (size_t i = 0; i < args.size(); ++i) {
    if (i) cmd << " ";
    cmd << ShellEscape(args[i]);
  }
  cmd << " 2>&1";

  FILE* pipe = popen(cmd.str().c_str(), "r");
  if (!pipe) return {.exit_code = 127, .out = "popen failed"};
  std::string out;
  char buf[4096];
  while (fgets(buf, sizeof(buf), pipe)) out += buf;
  int rc = pclose(pipe);
  int exit_code = 1;
  if (WIFEXITED(rc)) exit_code = WEXITSTATUS(rc);
  else if (WIFSIGNALED(rc)) exit_code = 128 + WTERMSIG(rc);
  return {.exit_code = exit_code, .out = out};
}

// Run `inner` with a SUDO shell function available.
//   - With a password: SUDO pipes it to `sudo -S` (works for repeated calls).
//   - Without one:      SUDO uses `sudo -n` (non-interactive; fails cleanly if
//                       no cached credentials / passwordless rule).
// The password is passed as positional $1 so it never lands in the command
// string that gets logged by the shell.
CmdResult RunPrivileged(const std::string& inner, const std::string& password) {
  if (!password.empty()) {
    std::string script =
        "PW=$1; SUDO() { printf '%s\\n' \"$PW\" | command sudo -S -p '' \"$@\"; }; " + inner;
    return RunCommandCapture({"bash", "-lc", script, "_", password});
  }
  std::string script = "SUDO() { command sudo -n \"$@\"; }; " + inner;
  return RunCommandCapture({"bash", "-lc", script});
}

std::string JsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 16);
  for (char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c; break;
    }
  }
  return out;
}

std::string ToJson(const CmdResult& r) {
  std::ostringstream ss;
  ss << "{\"exit_code\":" << r.exit_code << ",\"out\":\"" << JsonEscape(r.out) << "\"}";
  return ss.str();
}

// Extract a string value for `key` from the flat JSON object in req.
std::string ExtractParam(const std::string& req, const std::string& key) {
  auto kp = req.find("\"" + key + "\"");
  if (kp == std::string::npos) return {};
  auto cp = req.find(':', kp + key.size() + 2);
  if (cp == std::string::npos) return {};
  cp = req.find('"', cp);
  if (cp == std::string::npos) return {};
  auto ep = req.find('"', cp + 1);
  if (ep == std::string::npos) return {};
  return req.substr(cp + 1, ep - cp - 1);
}

// A systemd unit/service name: letters, digits and . _ - @ : \ (escaped units).
bool SafeUnit(const std::string& s) {
  if (s.empty() || s.size() > 128) return false;
  for (char c : s) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_' &&
        c != '-' && c != '@' && c != ':' && c != '\\')
      return false;
  }
  return true;
}

bool SafeAction(const std::string& s) {
  return s == "start" || s == "stop" || s == "restart" || s == "enable" ||
         s == "disable" || s == "status";
}

// A hwmon pwm control node: exactly /sys/class/hwmon/hwmonN/pwmM (digits only,
// nothing trailing). The _enable sibling is derived in the shell, never passed
// in — so an attacker cannot smuggle "..;rm -rf" or a path outside hwmon.
bool SafePwmPath(const std::string& s) {
  const std::string pre = "/sys/class/hwmon/hwmon";
  if (s.size() <= pre.size() || s.compare(0, pre.size(), pre) != 0) return false;
  size_t i = pre.size(), d0 = i;
  while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
  if (i == d0) return false;  // need a hwmon index
  const std::string mid = "/pwm";
  if (s.compare(i, mid.size(), mid) != 0) return false;
  i += mid.size();
  size_t d1 = i;
  while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
  if (i == d1) return false;  // need a pwm index
  return i == s.size();       // nothing trailing (rejects pwm1_enable, etc.)
}

// A raw PWM duty byte, 0..255.
bool SafeByte(const std::string& s) {
  if (s.empty() || s.size() > 3) return false;
  for (char c : s)
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  int v = std::stoi(s);
  return v >= 0 && v <= 255;
}

// A ThinkPad /proc/acpi/ibm/fan level token.
bool SafeFanLevel(const std::string& s) {
  if (s == "auto" || s == "full-speed" || s == "disengaged") return true;
  return s.size() == 1 && s[0] >= '0' && s[0] <= '7';
}

// A power-profiles-daemon profile name.
bool SafeProfile(const std::string& s) {
  return s == "power-saver" || s == "balanced" || s == "performance";
}

// A cpufreq governor (fixed whitelist; the only values we ever write to sysfs).
bool SafeGovernor(const std::string& s) {
  static const char* const ok[] = {"performance", "powersave", "ondemand",
                                    "conservative", "schedutil", "userspace"};
  for (const char* g : ok)
    if (s == g) return true;
  return false;
}
#endif

}  // namespace

int main() {
#if !LEVI_HAS_WEBVIEW
  std::cerr
      << "Leviathan Helm requires webview.h.\n"
      << "Place it at either:\n"
      << "  - third_party/webview/webview.h\n"
      << "  - or in your compiler include path as webview.h\n\n"
      << "Deps (Mint/Ubuntu):\n"
      << "  sudo apt-get update\n"
      << "  sudo apt-get install -y g++ pkg-config libgtk-3-dev libwebkit2gtk-4.1-dev\n";
  return 2;
#else
  webview::webview w(true, nullptr);
  w.set_title("Leviathan Helm — System Control Center");
  w.set_size(1180, 740, WEBVIEW_HINT_NONE);

  const char* html = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>Leviathan Helm</title>
  <style>
    :root {
      --bg: #0b0f16;
      --sidebar: #0d1219;
      --panel: rgba(18,26,39,.92);
      --panel2: rgba(255,255,255,.03);
      --text: #e9f0ff;
      --muted: #8aa0bd;
      --border: rgba(255,255,255,.08);
      --shadow: rgba(0,0,0,.4);
      --accent: #38bdf8;   /* Helm cyan-blue (distinct from DADA) */
      --accent2: #22d3aa;
      --good: #34d399;
      --warn: #fbbf24;
      --bad: #fb7185;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      display: flex;
      height: 100vh;
      overflow: hidden;
      font-family: ui-sans-serif, system-ui, -apple-system, Segoe UI, Roboto, Ubuntu, Cantarell, Noto Sans, Arial;
      background: var(--bg);
      color: var(--text);
      font-size: 13px;
    }

    /* Sidebar */
    #sidebar {
      width: 208px; min-width: 208px;
      background: var(--sidebar);
      border-right: 1px solid var(--border);
      display: flex; flex-direction: column;
    }
    .brand { padding: 18px 16px 14px; border-bottom: 1px solid var(--border); display:flex; gap:11px; align-items:center; }
    .logo {
      width: 34px; height: 34px; border-radius: 10px; flex: 0 0 auto;
      background: conic-gradient(from 210deg, var(--accent), var(--accent2), var(--accent));
      box-shadow: 0 8px 22px var(--shadow);
    }
    .brand-name { font-size: 15px; font-weight: 800; letter-spacing: .4px; }
    .brand-sub { font-size: 10px; color: var(--muted); margin-top: 2px; }
    nav { flex: 1; padding: 8px; overflow-y: auto; }
    .ni {
      display: flex; align-items: center; gap: 9px;
      padding: 9px 11px; border-radius: 9px; cursor: pointer;
      color: var(--muted); user-select: none; margin-bottom: 2px;
      border: 1px solid transparent; font-size: 12.5px; font-weight: 600;
      transition: all .12s;
    }
    .ni:hover { background: rgba(255,255,255,.04); color: var(--text); }
    .ni.active { background: rgba(56,189,248,.12); border-color: rgba(56,189,248,.28); color: var(--accent); }
    .ni-icon { font-size: 13px; width: 18px; text-align: center; }
    .sf { padding: 11px 16px; border-top: 1px solid var(--border); font-size: 10px; color: var(--muted); }
    .sf .dot { display:inline-block; width:7px; height:7px; border-radius:50%; background:var(--good); margin-right:6px; }

    /* Content */
    #content {
      flex: 1; overflow-y: auto;
      background: radial-gradient(1000px 640px at 8% 0%, rgba(56,189,248,.09), transparent 52%),
                  radial-gradient(760px 520px at 92% 24%, rgba(34,211,170,.06), transparent 55%),
                  var(--bg);
    }
    .tab { display: none; padding: 20px 24px; max-width: 1040px; }
    .tab.active { display: block; }
    .th { display: flex; align-items: center; justify-content: space-between; margin-bottom: 16px; gap: 10px; }
    .th h1 { font-size: 17px; font-weight: 800; }
    .th .desc { font-size: 11.5px; color: var(--muted); margin-top: 3px; font-weight: 500; }
    .th-acts { display: flex; gap: 8px; flex-wrap: wrap; }

    .card {
      background: var(--panel); border: 1px solid var(--border);
      border-radius: 14px; padding: 15px; margin-bottom: 14px;
      box-shadow: 0 8px 30px var(--shadow); backdrop-filter: blur(8px);
    }
    .card h2 { font-size: 12.5px; font-weight: 700; color: #dbeafe; margin-bottom: 11px; }
    .muted { color: var(--muted); font-size: 12px; }
    .hint { font-size: 10.5px; color: var(--muted); margin-top: 8px; line-height: 1.5; }

    .grid2 { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }
    @media (max-width: 900px) { .grid2 { grid-template-columns: 1fr; } }

    pre {
      margin: 0; padding: 11px;
      background: rgba(0,0,0,.28); border: 1px solid var(--border);
      border-radius: 10px; color: #c7d2fe;
      font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
      font-size: 11.5px; line-height: 1.5;
      overflow: auto; max-height: 460px; white-space: pre-wrap; word-break: break-word;
    }

    table { width: 100%; border-collapse: collapse; font-size: 12px; }
    th { text-align: left; padding: 7px 9px; color: var(--muted); font-weight: 650; border-bottom: 1px solid var(--border); }
    td { padding: 6px 9px; border-bottom: 1px solid rgba(255,255,255,.04); }
    tr:hover td { background: rgba(255,255,255,.02); }
    .mono { font-family: ui-monospace, monospace; font-size: 11px; }

    button {
      border: 1px solid var(--border); background: rgba(255,255,255,.04);
      color: var(--text); padding: 7px 12px; border-radius: 9px;
      cursor: pointer; font-size: 12px; font-weight: 650; transition: all .12s;
    }
    button:hover { border-color: rgba(56,189,248,.4); background: rgba(56,189,248,.07); }
    button.primary { background: rgba(56,189,248,.15); border-color: rgba(56,189,248,.3); color: var(--accent); }
    button.warn { background: rgba(251,191,36,.1); border-color: rgba(251,191,36,.28); color: var(--warn); }
    button.danger { background: rgba(251,113,133,.09); border-color: rgba(251,113,133,.24); color: var(--bad); }

    .fr { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; margin-bottom: 10px; }
    input[type=text], input[type=password] {
      background: rgba(0,0,0,.28); border: 1px solid var(--border);
      color: var(--text); padding: 7px 10px; border-radius: 9px; font-size: 12px; outline: none;
    }
    input:focus { border-color: rgba(56,189,248,.4); }

    .pills { display: flex; gap: 8px; flex-wrap: wrap; margin-bottom: 14px; }
    .pill {
      display: inline-flex; align-items: center; gap: 7px;
      padding: 6px 11px; border-radius: 999px; border: 1px solid var(--border);
      font-size: 11.5px; color: var(--muted); background: rgba(255,255,255,.025);
    }
    .dot { width: 7px; height: 7px; border-radius: 50%; background: var(--warn); flex-shrink: 0; }
    .dot.good { background: var(--good); } .dot.bad { background: var(--bad); }

    .sg { display: grid; grid-template-columns: repeat(4,1fr); gap: 11px; margin-bottom: 14px; }
    @media (max-width: 860px) { .sg { grid-template-columns: 1fr 1fr; } }
    .sc { background: var(--panel); border: 1px solid var(--border); border-radius: 13px; padding: 13px 15px; }
    .sl { font-size: 10px; color: var(--muted); text-transform: uppercase; letter-spacing: .6px; margin-bottom: 5px; }
    .sv { font-size: 14px; font-weight: 700; }
    .ss { font-size: 10px; color: var(--muted); margin-top: 3px; }
    .spin { display: inline-block; animation: spin .7s linear infinite; }
    @keyframes spin { to { transform: rotate(360deg); } }
  </style>
</head>
<body>
  <div id="sidebar">
    <div class="brand">
      <div class="logo"></div>
      <div>
        <div class="brand-name">HELM</div>
        <div class="brand-sub">Leviathan System Control</div>
      </div>
    </div>
    <nav id="nav"></nav>
    <div class="sf"><span class="dot"></span><span id="sysPill">System: loading…</span></div>
  </div>

  <div id="content">
    <div class="tab active" id="tab-view">
      <div class="th">
        <div>
          <h1 id="title">Loading…</h1>
          <div class="desc" id="desc"></div>
        </div>
        <div class="th-acts">
          <button class="primary" onclick="refresh(false)">↻ Refresh</button>
          <button id="autoBtn" onclick="toggleAuto()">Auto: off</button>
        </div>
      </div>
      <div id="view"></div>
    </div>
  </div>

  <script>
    const tabs = [
      { id: "thermal",   icon: "❄", label: "Thermal",          desc: "Live temps, fan RPM and Cool Mode fan control — let it rest" },
      { id: "hardware",  icon: "▦", label: "Hardware",         desc: "CPU, memory, GPU and machine inventory" },
      { id: "power",     icon: "⚡", label: "Power",            desc: "Power profiles and CPU frequency governor" },
      { id: "sensors",   icon: "◭", label: "Sensors & Temps",  desc: "Raw lm-sensors and thermal-zone dump" },
      { id: "resources", icon: "▤", label: "Resource Monitor", desc: "Live CPU / memory load and top processes (read-only)" },
      { id: "services",  icon: "◎", label: "Services",         desc: "systemd services — list, start, stop, enable, disable" },
      { id: "startup",   icon: "◇", label: "Startup Apps",     desc: "Autostart apps and enabled boot services" },
      { id: "updates",   icon: "⤓", label: "Updates",          desc: "APT package updates — list upgradable, refresh index" },
      { id: "storage",   icon: "▥", label: "Storage",          desc: "Disk usage, partitions and largest folders" },
      { id: "logs",      icon: "☰", label: "Logs",             desc: "System journal and kernel messages" },
      { id: "boot",      icon: "⏻", label: "Boot",             desc: "Bootloader, kernel and boot timing" },
      { id: "about",     icon: "ⓘ", label: "About",            desc: "License and legal notice" },
    ];

    let active = "thermal";
    let autoTimer = null;

    // ── Thermal / fan-control state ────────────────────────────────────────
    let thermalTimer = null;        // flagship live poll (independent of Auto)
    let tHist = {};                 // sensor id -> rolling history array
    const T_HIST_MAX = 60;          // ~2 min at 2s cadence
    let fanIfaces = [];             // detected controllable fan interfaces
    let fanMeta = { sensors: false, i8k: false };
    let lastControlTemp = 0;        // hottest CPU temp driving the curve
    const SAFETY_MAX = 85;          // hard floor: at/above this, fan -> MAX always
    // Cool Mode is a live userspace fan curve driven by this tab's 2s poll.
    let coolMode = { active: false, mode: "balanced", iface: null, path: null, lastPwm: null };

    // Fan curves: temperature(°C) -> PWM duty (0-255). Interpolated linearly.
    const CURVES = {
      quiet:    [[50, 0],  [60, 60],  [70, 120], [80, 180], [85, 255]],
      balanced: [[45, 40], [55, 90],  [65, 140], [75, 200], [82, 255]],
      cool:     [[40, 80], [50, 130], [60, 180], [70, 220], [75, 255]],
    };
    function curvePwm(mode, t) {
      if (t >= SAFETY_MAX) return 255;           // hard safety floor
      const c = CURVES[mode] || CURVES.balanced;
      if (t <= c[0][0]) return c[0][1];
      for (let i = 1; i < c.length; i++) {
        if (t <= c[i][0]) {
          const [t0, p0] = c[i - 1], [t1, p1] = c[i];
          return Math.round(p0 + (p1 - p0) * (t - t0) / (t1 - t0));
        }
      }
      return 255;
    }
    function tempColor(c) { return c >= 80 ? "#fb7185" : c >= 65 ? "#fbbf24" : "#34d399"; }

    function navRender() {
      const nav = document.getElementById("nav");
      nav.innerHTML = "";
      for (const t of tabs) {
        const b = document.createElement("div");
        b.className = "ni" + (t.id === active ? " active" : "");
        b.innerHTML = `<span class="ni-icon">${t.icon}</span>${t.label}`;
        b.onclick = () => { active = t.id; navRender(); refresh(false); };
        nav.appendChild(b);
      }
    }

    async function rpc(method, params) {
      return await window.levi({ method, ...(params || {}) });
    }

    function escapeHtml(s) {
      return (s || "").replaceAll("&","&amp;").replaceAll("<","&lt;").replaceAll(">","&gt;");
    }

    // Optional privileged-password bar shared by services / updates / logs.
    let sudoPassword = "";
    function pwBar(note) {
      return `<div class="card">
        <h2>Administrator access</h2>
        <div class="fr">
          <input type="password" id="pw" placeholder="password (optional — for privileged actions)" style="width:320px" value="${escapeHtml(sudoPassword)}"/>
          <button onclick="savePw()">Use for this session</button>
          <button onclick="clearPw()">Clear</button>
        </div>
        <div class="hint">${note || "Left blank, Helm tries passwordless sudo (sudo -n) and reports if it is refused."}</div>
      </div>`;
    }
    function savePw() { const el = document.getElementById("pw"); if (el) sudoPassword = el.value; }
    function clearPw() { sudoPassword = ""; const el = document.getElementById("pw"); if (el) el.value = ""; }

    function setView(html) { document.getElementById("view").innerHTML = html; }

    // ── Renderers ──────────────────────────────────────────────────────────
    function parsePS(out) {
      const lines = out.trim().split("\n").filter(Boolean);
      const rows = [];
      const exclude = /^(ps|awk|grep|sed|cut|sort|column|head|tail|dash|bash|sh)$/i;
      for (let i = 1; i < lines.length; i++) {
        const parts = lines[i].trim().split(/\s+/);
        if (parts.length < 5) continue;
        const [pid, comm, cpu, mem, rss] = parts;
        if (exclude.test(comm)) continue;
        rows.push({ pid, comm, cpu, mem, rss });
      }
      return rows;
    }

    async function renderHardware() {
      setView(`<div class="card"><div class="muted">Reading hardware…</div></div>`);
      const r = await rpc("hw_info");
      setView(`<div class="card"><h2>Hardware inventory</h2><pre>${escapeHtml(r.out)}</pre></div>`);
    }

    async function renderSensors() {
      const r = await rpc("sensors");
      setView(`<div class="card"><h2>Temperatures &amp; fans</h2><pre>${escapeHtml(r.out)}</pre>
        <div class="hint">Reads <b>sensors</b> (lm-sensors) if present, plus <b>/sys/class/thermal</b>. Use Auto for live updates.</div></div>`);
    }

    async function renderResources() {
      const [sys, ps] = await Promise.all([rpc("sys_summary"), rpc("task_top")]);
      const rows = parsePS(ps.out);
      setView(`
        <div class="grid2">
          <div class="card"><h2>System</h2><pre>${escapeHtml(sys.out)}</pre></div>
          <div class="card"><h2>Top processes (by CPU)</h2>
            <table>
              <thead><tr><th>PID</th><th>Name</th><th>%CPU</th><th>%MEM</th><th>RSS(KiB)</th></tr></thead>
              <tbody>${rows.map(p => `<tr><td class="mono">${p.pid}</td><td>${escapeHtml(p.comm)}</td><td>${p.cpu}</td><td>${p.mem}</td><td>${p.rss}</td></tr>`).join("")}</tbody>
            </table>
            <div class="hint">Read-only. Killing / managing processes lives in <b>DADA</b> (security).</div>
          </div>
        </div>`);
    }

    async function renderServices() {
      const r = await rpc("services_list");
      setView(`
        ${pwBar("start / stop / enable / disable need root. Fill the password above (or configure passwordless sudo).")}
        <div class="card">
          <h2>Control a service</h2>
          <div class="fr">
            <input type="text" id="svc-unit" placeholder="unit name  (e.g. ssh.service, cups)" style="width:280px"/>
          </div>
          <div class="fr">
            <button class="primary" onclick="svcAction('start')">Start</button>
            <button class="danger" onclick="svcAction('stop')">Stop</button>
            <button onclick="svcAction('restart')">Restart</button>
            <button class="primary" onclick="svcAction('enable')">Enable at boot</button>
            <button class="warn" onclick="svcAction('disable')">Disable at boot</button>
            <button onclick="svcAction('status')">Status</button>
          </div>
          <pre id="svc-out" style="max-height:200px;">No action run yet.</pre>
        </div>
        <div class="card"><h2>All services</h2><pre>${escapeHtml(r.out)}</pre></div>`);
    }
    async function svcAction(action) {
      savePw();
      const unit = document.getElementById("svc-unit").value.trim();
      const out = document.getElementById("svc-out");
      if (!unit) { out.textContent = "Enter a unit name first (e.g. ssh.service)."; return; }
      out.innerHTML = `<span class="spin">⟳</span> ${action} ${escapeHtml(unit)} …`;
      const r = await rpc("service_action", { action, unit, password: sudoPassword });
      out.textContent = (r.out || "").trim() || (r.exit_code === 0 ? "OK" : "Failed (exit " + r.exit_code + ")");
    }

    async function renderStartup() {
      const r = await rpc("startup_list");
      setView(`<div class="card"><h2>Startup applications &amp; enabled boot services</h2><pre>${escapeHtml(r.out)}</pre>
        <div class="hint">Autostart .desktop entries (~/.config/autostart, /etc/xdg/autostart) and systemd unit files marked <b>enabled</b>.</div></div>`);
    }

    async function renderUpdates() {
      setView(`
        ${pwBar("Refreshing the package index (apt update) needs root.")}
        <div class="card">
          <h2>Package updates</h2>
          <div class="fr">
            <button class="primary" onclick="updatesList()">List upgradable</button>
            <button class="warn" onclick="updatesRefresh()">Refresh index (apt update)</button>
          </div>
          <div id="upd-status" class="muted" style="min-height:16px;margin-bottom:6px;"></div>
          <pre id="upd-out" style="max-height:420px;">Click “List upgradable”.</pre>
          <div class="hint">Helm never auto-installs packages; it lists what is upgradable and refreshes the index. Run the actual upgrade yourself when ready.</div>
        </div>`);
    }
    async function updatesList() {
      const out = document.getElementById("upd-out");
      document.getElementById("upd-status").innerHTML = '<span class="spin">⟳</span> Reading apt cache…';
      const r = await rpc("updates_list");
      document.getElementById("upd-status").textContent = "";
      out.textContent = (r.out || "").trim() || "(nothing upgradable / apt not available)";
    }
    async function updatesRefresh() {
      savePw();
      const out = document.getElementById("upd-out");
      document.getElementById("upd-status").innerHTML = '<span class="spin">⟳</span> Running apt-get update…';
      const r = await rpc("updates_refresh", { password: sudoPassword });
      document.getElementById("upd-status").textContent = r.exit_code === 0 ? "✓ Index refreshed" : "✗ exit " + r.exit_code;
      out.textContent = (r.out || "").trim() || "(no output)";
    }

    async function renderStorage() {
      const r = await rpc("storage");
      setView(`<div class="card"><h2>Storage overview</h2><pre>${escapeHtml(r.out)}</pre>
        <div class="hint">df (mounted filesystems), lsblk (partitions) and du of your home folder’s largest directories.</div></div>`);
    }

    async function renderLogs() {
      setView(`
        ${pwBar("Kernel/journal reads may need root depending on your groups.")}
        <div class="card">
          <h2>System logs</h2>
          <div class="fr">
            <button class="primary" onclick="logsShow('journal')">System journal</button>
            <button onclick="logsShow('dmesg')">Kernel (dmesg)</button>
          </div>
          <pre id="log-out" style="max-height:480px;">Pick a source.</pre>
        </div>`);
      logsShow('journal');
    }
    async function logsShow(kind) {
      savePw();
      const out = document.getElementById("log-out");
      out.innerHTML = '<span class="spin">⟳</span> Reading…';
      const r = await rpc(kind === "dmesg" ? "logs_dmesg" : "logs_journal", { password: sudoPassword });
      out.textContent = (r.out || "").trim() || "(no output / not accessible)";
      out.scrollTop = out.scrollHeight;
    }

    async function renderBoot() {
      const r = await rpc("boot");
      setView(`<div class="card"><h2>Boot &amp; bootloader</h2><pre>${escapeHtml(r.out)}</pre></div>`);
    }

    async function renderAbout() {
      setView(`<div class="card" style="max-width:560px;">
        <h2>License</h2>
        <div style="font-size:12.5px;line-height:1.8;color:var(--text);">
          Helm — part of LeviathanOS<br/>
          Free software under the GNU General Public License, version 3.<br/>
          This program comes with ABSOLUTELY NO WARRANTY.
        </div>
        <div class="hint" style="line-height:1.8;">
          Full license: /usr/share/doc/leviathanos/LICENSE<br/>
          <a href="https://www.gnu.org/licenses/gpl-3.0.html" style="color:var(--accent);">https://www.gnu.org/licenses/gpl-3.0.html</a>
        </div>
      </div>`);
    }

    // ── Thermal (flagship, live) ───────────────────────────────────────────
    function parseThermal(out) {
      const temps = [], fans = [];
      for (const line of (out || "").split("\n")) {
        const p = line.split("|");
        if (p[0] === "T" && p.length >= 4) temps.push({ chip: p[1], label: p[2], milli: +p[3] });
        else if (p[0] === "F" && p.length >= 4) fans.push({ chip: p[1], label: p[2], rpm: +p[3] });
      }
      return { temps, fans };
    }
    function parseFanDetect(out) {
      const list = []; let sensors = false, i8k = false;
      for (const line of (out || "").split("\n")) {
        const p = line.split("|");
        if (p[0] === "SENSORS") sensors = (p[1] === "yes");
        else if (p[0] === "I8KCTL") i8k = (p[1] === "yes");
        else if (p[0] === "IFACE" && p[1] === "thinkpad")
          list.push({ iface: "thinkpad", path: p[2], chip: p[3], access: p[4] });
        else if (p[0] === "IFACE" && p[1] === "pwm")
          list.push({ iface: "pwm", path: p[2], chip: p[3], enable: p[4], val: +p[5] });
      }
      fanMeta = { sensors, i8k };
      return list;
    }
    function pushHist(id, v) {
      (tHist[id] || (tHist[id] = [])).push(v);
      if (tHist[id].length > T_HIST_MAX) tHist[id].shift();
    }
    // Minimal SVG sparkline — no chart library, scales to its own min/max.
    function spark(vals, color, w = 200, h = 34) {
      if (!vals || vals.length < 2) return `<svg width="${w}" height="${h}"></svg>`;
      const mn = Math.min(...vals), mx = Math.max(...vals), rng = (mx - mn) || 1;
      const pts = vals.map((v, i) => {
        const x = (i / (vals.length - 1)) * w;
        const y = h - 2 - ((v - mn) / rng) * (h - 4);
        return x.toFixed(1) + "," + y.toFixed(1);
      }).join(" ");
      return `<svg width="${w}" height="${h}" viewBox="0 0 ${w} ${h}" preserveAspectRatio="none" style="display:block">
        <polyline points="${pts}" fill="none" stroke="${color}" stroke-width="1.6" stroke-linejoin="round"/></svg>`;
    }
    const CPU_RE = /pkg|core|tccd|tctl|tcpu|package|cpu/i;

    function fanPlatformNote() {
      const chips = fanIfaces.map(f => f.chip).join(" ");
      if (/dell_smm/i.test(chips)) return "Detected <b>Dell</b> (dell_smm_hwmon).";
      if (/applesmc/i.test(chips)) return "Detected <b>Apple</b> (applesmc).";
      if (/thinkpad/i.test(chips)) return "Detected <b>ThinkPad</b> (thinkpad_acpi).";
      return "Generic hwmon PWM control.";
    }
    function fanControlsHtml() {
      if (!fanIfaces.length) {
        return `<div class="muted">No controllable fan interface found on this machine.</div>
          <div class="hint">Helm looked for Dell <b>dell_smm_hwmon</b> PWM nodes, ThinkPad
          <b>/proc/acpi/ibm/fan</b>, Apple <b>applesmc</b>, and generic hwmon <b>pwm*</b> controls
          and found none writable. Your firmware most likely locks fan control in the BIOS — that is
          common on modern laptops. Temperatures above stay live; the firmware is managing the fan.</div>`;
      }
      const opts = fanIfaces.map((f, i) =>
        `<option value="${i}">${escapeHtml(f.chip)} — ${escapeHtml(f.path)}` +
        `${f.iface === "pwm" && f.enable === "no" ? " (no enable node)" : ""}</option>`).join("");
      return `<div class="fr">
          <select id="fan-iface" style="background:rgba(0,0,0,.28);border:1px solid var(--border);color:var(--text);padding:7px 10px;border-radius:9px;font-size:12px;">${opts}</select>
          <button onclick="reDetectFans()">Re-detect</button>
        </div>
        <div class="fr">
          <button onclick="coolEngage('quiet')">🤫 Quiet</button>
          <button class="primary" onclick="coolEngage('balanced')">⚖ Balanced</button>
          <button class="warn" onclick="coolEngage('cool')">❄ Cool</button>
          <button class="danger" onclick="coolAuto()">↩ BIOS Auto</button>
        </div>
        <div class="hint">${fanPlatformNote()} Cool Mode runs a live fan curve <b>from this tab</b>:
          every 2s Helm re-reads CPU temp and sets the fan. <b>Hard safety floor:</b> at
          ${SAFETY_MAX}°C or above the fan is forced to maximum regardless of mode, and the fan is
          always allowed to spin up. Leaving this tab hands the fan back to your firmware.</div>`;
    }
    function thermalSkeleton() {
      return `
        ${pwBar("Cool Mode writes to /sys/class/hwmon PWM nodes and needs root. Blank = passwordless sudo.")}
        <div class="sg" id="th-stat"></div>
        <div class="card"><h2>Live sensors <span class="muted" style="font-weight:500">— polling every 2s</span></h2>
          <div id="th-graphs"><div class="muted">reading…</div></div>
        </div>
        <div class="card"><h2>Cool Mode — fan control</h2>
          ${fanControlsHtml()}
          <div id="th-fanstatus" class="hint" style="margin-top:10px;"></div>
        </div>`;
    }
    async function renderThermal() {
      if (document.getElementById("th-graphs")) { await thermalTick(); return; }
      const fd = await rpc("fan_detect");
      fanIfaces = parseFanDetect(fd.out);
      setView(thermalSkeleton());
      await thermalTick();
      if (!thermalTimer) thermalTimer = setInterval(thermalTick, 2000);
    }
    async function reDetectFans() {
      const fd = await rpc("fan_detect");
      fanIfaces = parseFanDetect(fd.out);
      const card = document.querySelector("#th-fanstatus");
      // rebuild just the controls card content
      if (card && card.parentElement) {
        card.parentElement.innerHTML = `<h2>Cool Mode — fan control</h2>${fanControlsHtml()}<div id="th-fanstatus" class="hint" style="margin-top:10px;"></div>`;
      }
    }
    function selIface() {
      const s = document.getElementById("fan-iface");
      if (!s || !fanIfaces.length) return null;
      return fanIfaces[+s.value] || fanIfaces[0];
    }
    async function coolEngage(mode) {
      savePw();
      const f = selIface();
      if (!f) return;
      coolMode = { active: true, mode, iface: f.iface, path: f.path, lastPwm: null };
      await applyFanNow();
    }
    async function coolAuto() {
      savePw();
      const f = selIface() || (coolMode.path ? { iface: coolMode.iface, path: coolMode.path } : null);
      coolMode.active = false; coolMode.lastPwm = null;
      const el = document.getElementById("th-fanstatus");
      if (f) {
        const r = await rpc("fan_auto", { iface: f.iface, path: f.path, password: sudoPassword });
        if (el) el.innerHTML = `<span class="muted">${escapeHtml((r.out || "").trim())}</span>`;
      }
    }
    async function applyFanNow() {
      if (!coolMode.active) return;
      const t = lastControlTemp;
      const v = curvePwm(coolMode.mode, t);
      coolMode.lastPwm = v;
      if (coolMode.iface === "pwm") {
        await rpc("fan_set", { iface: "pwm", path: coolMode.path, value: String(v), password: sudoPassword });
      } else if (coolMode.iface === "thinkpad") {
        const lvl = t >= SAFETY_MAX ? "full-speed" : String(Math.max(0, Math.min(7, Math.round(v / 255 * 7))));
        await rpc("fan_set", { iface: "thinkpad", level: lvl, password: sudoPassword });
      }
    }
    function renderThStats(temps, fans, ctl) {
      const el = document.getElementById("th-stat"); if (!el) return;
      const hot = temps.reduce((a, b) => (b.milli > a.milli ? b : a), { milli: 0, label: "-", chip: "" });
      const fan = fans.find(f => f.rpm > 0) || fans[0];
      const col = tempColor(ctl);
      el.innerHTML = `
        <div class="sc"><div class="sl">CPU control temp</div><div class="sv" style="color:${col}">${ctl.toFixed(1)}°C</div><div class="ss">drives the fan curve</div></div>
        <div class="sc"><div class="sl">Hottest sensor</div><div class="sv">${(hot.milli / 1000).toFixed(1)}°C</div><div class="ss">${escapeHtml(hot.label)}</div></div>
        <div class="sc"><div class="sl">Fan</div><div class="sv" style="color:#22d3aa">${fan ? fan.rpm + " RPM" : "—"}</div><div class="ss">${fan ? escapeHtml(fan.chip + " " + fan.label) : "no fan sensor"}</div></div>
        <div class="sc"><div class="sl">Cool Mode</div><div class="sv" style="color:${coolMode.active ? "var(--accent)" : "var(--muted)"}">${coolMode.active ? coolMode.mode : "off"}</div><div class="ss">${coolMode.active ? "live curve engaged" : "firmware controlled"}</div></div>`;
    }
    function renderThGraphs(temps, fans) {
      const el = document.getElementById("th-graphs"); if (!el) return;
      const sorted = [...temps].sort((a, b) =>
        (CPU_RE.test(b.chip + b.label) - CPU_RE.test(a.chip + a.label)) || (b.milli - a.milli)).slice(0, 20);
      const trow = t => {
        const id = "T:" + t.chip + "::" + t.label, c = t.milli / 1000, col = tempColor(c);
        return `<div style="display:flex;align-items:center;gap:10px;padding:4px 0;border-bottom:1px solid rgba(255,255,255,.04)">
          <div style="width:200px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap"><span class="mono" style="color:var(--muted)">${escapeHtml(t.chip)}</span> ${escapeHtml(t.label)}</div>
          <div style="width:60px;text-align:right;font-weight:700;color:${col}">${c.toFixed(1)}°</div>
          <div style="flex:1;min-width:120px">${spark(tHist[id], col)}</div></div>`;
      };
      const frow = f => {
        const id = "F:" + f.chip + "::" + f.label;
        return `<div style="display:flex;align-items:center;gap:10px;padding:4px 0;border-bottom:1px solid rgba(255,255,255,.04)">
          <div style="width:200px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap"><span class="mono" style="color:var(--muted)">${escapeHtml(f.chip)}</span> ${escapeHtml(f.label)}</div>
          <div style="width:60px;text-align:right;font-weight:700;color:#22d3aa">${f.rpm}</div>
          <div style="flex:1;min-width:120px">${spark(tHist[id], "#22d3aa")}</div></div>`;
      };
      el.innerHTML = sorted.map(trow).join("") +
        (fans.length ? `<div class="sl" style="margin:8px 0 2px">Fans (RPM)</div>` + fans.map(frow).join("") : "");
    }
    function renderFanStatus(ctl) {
      const el = document.getElementById("th-fanstatus"); if (!el) return;
      if (!coolMode.active) return;
      const safety = ctl >= SAFETY_MAX;
      const v = coolMode.lastPwm != null ? coolMode.lastPwm : curvePwm(coolMode.mode, ctl);
      let detail;
      if (coolMode.iface === "thinkpad") {
        const lvl = safety ? "full-speed" : Math.max(0, Math.min(7, Math.round(v / 255 * 7)));
        detail = `level ${lvl}`;
      } else {
        detail = `PWM ${v}/255 (${Math.round(v / 255 * 100)}%)`;
      }
      el.innerHTML = `<b style="color:var(--accent)">Cool Mode: ${coolMode.mode}</b> → ${detail} @ ${ctl.toFixed(1)}°C` +
        (safety ? ` — <span style="color:var(--bad)">SAFETY FLOOR: fan forced to MAX</span>` : "");
    }
    async function thermalTick() {
      const r = await rpc("thermal_read");
      const { temps, fans } = parseThermal(r.out);
      for (const t of temps) pushHist("T:" + t.chip + "::" + t.label, t.milli / 1000);
      for (const f of fans) pushHist("F:" + f.chip + "::" + f.label, f.rpm);
      const cpu = temps.filter(t => CPU_RE.test(t.chip + " " + t.label));
      lastControlTemp = (cpu.length ? cpu : temps).reduce((m, t) => Math.max(m, t.milli / 1000), 0);
      renderThStats(temps, fans, lastControlTemp);
      renderThGraphs(temps, fans);
      if (coolMode.active) await applyFanNow();
      renderFanStatus(lastControlTemp);
    }
    function stopThermal() {
      if (thermalTimer) { clearInterval(thermalTimer); thermalTimer = null; }
      if (coolMode.active) {  // safety: hand the fan back to firmware on leave
        const iface = coolMode.iface, path = coolMode.path;
        coolMode.active = false; coolMode.lastPwm = null;
        rpc("fan_auto", { iface, path, password: sudoPassword });
      }
    }

    // ── Power ──────────────────────────────────────────────────────────────
    async function renderPower() {
      const r = await rpc("power_read");
      setView(`
        <div class="card"><h2>Power profile</h2>
          <div class="fr">
            <button class="primary" onclick="setProfile('power-saver')">🍃 Power Saver</button>
            <button onclick="setProfile('balanced')">⚖ Balanced</button>
            <button class="warn" onclick="setProfile('performance')">⚡ Performance</button>
          </div>
          <div id="pp-out" class="muted" style="min-height:16px;margin-bottom:6px;"></div>
          <div class="hint">Uses power-profiles-daemon via polkit — usually no password needed.</div>
        </div>
        ${pwBar("Changing the CPU frequency governor writes to /sys and needs root.")}
        <div class="card"><h2>CPU frequency governor</h2>
          <div class="fr">
            <button onclick="setGov('powersave')">powersave</button>
            <button onclick="setGov('schedutil')">schedutil</button>
            <button onclick="setGov('ondemand')">ondemand</button>
            <button onclick="setGov('conservative')">conservative</button>
            <button class="warn" onclick="setGov('performance')">performance</button>
          </div>
          <div id="gov-out" class="muted" style="min-height:16px;margin-bottom:6px;"></div>
          <div class="hint">Applied to every CPU. Only governors your kernel exposes will take.</div>
        </div>
        <div class="card"><h2>Current status</h2><pre id="pow-status">${escapeHtml(r.out)}</pre></div>`);
    }
    async function refreshPowStatus() {
      const r = await rpc("power_read"); const el = document.getElementById("pow-status");
      if (el) el.textContent = r.out;
    }
    async function setProfile(p) {
      const o = document.getElementById("pp-out"); o.innerHTML = '<span class="spin">⟳</span> setting…';
      const r = await rpc("power_set", { profile: p });
      o.textContent = (r.out || "").trim(); refreshPowStatus();
    }
    async function setGov(g) {
      savePw();
      const o = document.getElementById("gov-out"); o.innerHTML = '<span class="spin">⟳</span> setting…';
      const r = await rpc("power_governor", { governor: g, password: sudoPassword });
      o.textContent = (r.out || "").trim(); refreshPowStatus();
    }

    const renderers = {
      thermal: renderThermal, power: renderPower,
      hardware: renderHardware, sensors: renderSensors, resources: renderResources,
      services: renderServices, startup: renderStartup, updates: renderUpdates,
      storage: renderStorage, logs: renderLogs, boot: renderBoot, about: renderAbout,
    };
    // Tabs safe to poll on Auto (read-only, no forms to clobber).
    const autoSafe = new Set(["sensors", "resources"]);

    function toggleAuto() {
      const btn = document.getElementById("autoBtn");
      if (autoTimer) { clearInterval(autoTimer); autoTimer = null; btn.textContent = "Auto: off"; return; }
      autoTimer = setInterval(() => { if (autoSafe.has(active)) refresh(false); }, 2000);
      btn.textContent = "Auto: on";
    }

    async function refresh() {
      if (active !== "thermal") stopThermal();  // stop live poll; hand fan back to firmware
      const tab = tabs.find(t => t.id === active) || {};
      document.getElementById("title").textContent = tab.label || active;
      document.getElementById("desc").textContent = tab.desc || "";

      const sys = await rpc("sys_summary");
      document.getElementById("sysPill").textContent = (sys.out || "").trim().split("\n")[0] || "OK";

      const fn = renderers[active];
      if (fn) await fn();
    }

    navRender();
    refresh();
  </script>
</body>
</html>
)HTML";

  w.bind("levi", [](std::string req) -> std::string {
    const std::string method = ExtractParam(req, "method");
    if (method.empty()) return R"({"exit_code":1,"out":"bad request"})";
    const std::string password = ExtractParam(req, "password");

    CmdResult r;

    // ── Header summary + resource monitor ────────────────────────────────
    if (method == "sys_summary") {
      r = RunCommandCapture({"bash", "-lc",
          "echo \"$(uname -srmo)\"; "
          "echo \"Uptime: $(uptime -p 2>/dev/null || true)\"; "
          "echo \"Load: $(awk '{print $1\" \"$2\" \"$3}' /proc/loadavg 2>/dev/null)\"; "
          "echo \"Mem: $(free -m 2>/dev/null | awk '/Mem:/{print $3\"MB used / \"$2\"MB total\"}')\""});
    } else if (method == "task_top") {
      r = RunCommandCapture({"bash", "-lc",
          "ps -eo pid,comm,%cpu,%mem,rss --sort=-%cpu | head -n 24"});

    // ── Hardware ─────────────────────────────────────────────────────────
    } else if (method == "hw_info") {
      r = RunCommandCapture({"bash", "-lc",
          "echo '== CPU =='; "
          "if command -v lscpu >/dev/null 2>&1; then "
          "  lscpu | grep -E 'Model name|Vendor ID|Architecture|^CPU\\(s\\)|Core\\(s\\) per socket|Thread\\(s\\) per core|Socket|CPU max MHz|CPU min MHz'; "
          "else grep -m1 'model name' /proc/cpuinfo; echo \"Cores: $(nproc)\"; fi; echo; "
          "echo '== Memory =='; free -h; echo; "
          "if command -v lsmem >/dev/null 2>&1; then lsmem --summary 2>/dev/null | head -n 6; fi; echo; "
          "echo '== GPU / display =='; "
          "if command -v lspci >/dev/null 2>&1; then lspci | grep -Ei 'vga|3d|display' || echo '(no GPU line found)'; "
          "else echo 'lspci not installed (pciutils)'; fi; echo; "
          "echo '== Machine =='; "
          "if command -v hostnamectl >/dev/null 2>&1; then hostnamectl 2>/dev/null | grep -Ei 'hostname|Operating|Kernel|Hardware|Chassis|Virtual'; "
          "else echo \"Host: $(hostname)\"; uname -a; fi; echo; "
          "echo '== Block devices =='; lsblk -d -o NAME,SIZE,ROTA,TYPE,MODEL 2>/dev/null | head -n 30"});

    // ── Sensors / temperatures ───────────────────────────────────────────
    } else if (method == "sensors") {
      r = RunCommandCapture({"bash", "-lc",
          "if command -v sensors >/dev/null 2>&1; then sensors 2>/dev/null; "
          "else echo '(lm-sensors not installed: sudo apt-get install lm-sensors)'; fi; "
          "echo; echo '== Thermal zones (/sys/class/thermal) =='; "
          "found=0; "
          "for z in /sys/class/thermal/thermal_zone*; do "
          "  [ -r \"$z/temp\" ] || continue; found=1; "
          "  t=$(cat \"$z/temp\" 2>/dev/null); "
          "  ty=$(cat \"$z/type\" 2>/dev/null); "
          "  printf '%-24s %s.%s C\\n' \"${ty:-$(basename $z)}\" \"$((t/1000))\" \"$(( (t/100)%10 ))\"; "
          "done; "
          "[ \"$found\" = 0 ] && echo '(no readable thermal zones)'; "
          "echo; echo '== Fans (/sys/class/hwmon) =='; "
          "ls /sys/class/hwmon/hwmon*/fan*_input 2>/dev/null | while read f; do "
          "  printf '%s: %s RPM\\n' \"$f\" \"$(cat \"$f\" 2>/dev/null)\"; done | head -n 20 || true"});

    // ── systemd services ─────────────────────────────────────────────────
    } else if (method == "services_list") {
      r = RunCommandCapture({"bash", "-lc",
          "command -v systemctl >/dev/null 2>&1 || { echo 'systemctl not available'; exit 1; }; "
          "systemctl list-units --type=service --all --no-pager --plain 2>/dev/null | head -n 220"});
    } else if (method == "service_action") {
      const std::string action = ExtractParam(req, "action");
      const std::string unit = ExtractParam(req, "unit");
      if (!SafeAction(action)) { r = {.exit_code = 1, .out = "invalid action"}; }
      else if (!SafeUnit(unit)) { r = {.exit_code = 1, .out = "invalid unit name"}; }
      else if (action == "status") {
        // status is read-only; no privileges needed.
        r = RunCommandCapture({"bash", "-lc",
            "command -v systemctl >/dev/null 2>&1 || { echo 'systemctl not available'; exit 1; }; "
            "systemctl status --no-pager " + ShellEscape(unit) + " 2>&1 | head -n 40"});
      } else {
        r = RunPrivileged(
            "command -v systemctl >/dev/null 2>&1 || { echo 'systemctl not available'; exit 1; }; "
            "SUDO systemctl " + action + " " + ShellEscape(unit) + " && "
            "echo 'OK: " + action + " " + unit + "' || "
            "echo 'Failed. If you saw a sudo error, enter your password in the Administrator access box above.'",
            password);
      }

    // ── Startup apps ─────────────────────────────────────────────────────
    } else if (method == "startup_list") {
      r = RunCommandCapture({"bash", "-lc",
          "echo '== User autostart (~/.config/autostart) =='; "
          "if ls \"$HOME/.config/autostart/\"*.desktop >/dev/null 2>&1; then "
          "  for f in \"$HOME/.config/autostart/\"*.desktop; do "
          "    n=$(grep -m1 '^Name=' \"$f\" | cut -d= -f2-); "
          "    e=$(grep -m1 '^Exec=' \"$f\" | cut -d= -f2-); "
          "    printf '  %-28s %s\\n' \"${n:-$(basename $f)}\" \"$e\"; done; "
          "else echo '  (none)'; fi; echo; "
          "echo '== System autostart (/etc/xdg/autostart) =='; "
          "ls /etc/xdg/autostart/*.desktop 2>/dev/null | while read f; do "
          "  n=$(grep -m1 '^Name=' \"$f\" | cut -d= -f2-); "
          "  printf '  %s\\n' \"${n:-$(basename $f)}\"; done | head -n 40 || echo '  (none)'; echo; "
          "echo '== Enabled boot services (systemd) =='; "
          "command -v systemctl >/dev/null 2>&1 && "
          "systemctl list-unit-files --type=service --state=enabled --no-pager --plain 2>/dev/null | head -n 80 || "
          "echo '  (systemctl not available)'"});

    // ── Package updates (apt) ────────────────────────────────────────────
    } else if (method == "updates_list") {
      r = RunCommandCapture({"bash", "-lc",
          "command -v apt >/dev/null 2>&1 || { echo 'apt not available (non-Debian system?)'; exit 1; }; "
          "echo '== Upgradable packages =='; "
          "apt list --upgradable 2>/dev/null | tail -n +2 || echo '(none)'; echo; "
          "n=$(apt list --upgradable 2>/dev/null | tail -n +2 | grep -c '.'); "
          "echo \"Total upgradable: ${n:-0}\""});
    } else if (method == "updates_refresh") {
      r = RunPrivileged(
          "command -v apt-get >/dev/null 2>&1 || { echo 'apt-get not available'; exit 1; }; "
          "SUDO apt-get update 2>&1 | tail -n 40 || "
          "echo 'apt-get update failed. Enter your password in Administrator access above.'",
          password);

    // ── Storage ──────────────────────────────────────────────────────────
    } else if (method == "storage") {
      r = RunCommandCapture({"bash", "-lc",
          "echo '== Filesystem usage (df) =='; "
          "df -hT -x tmpfs -x devtmpfs 2>/dev/null | head -n 30; echo; "
          "echo '== Partitions (lsblk) =='; "
          "lsblk -o NAME,SIZE,TYPE,FSTYPE,LABEL,MOUNTPOINTS 2>/dev/null | head -n 40; echo; "
          "echo '== Largest folders in your home =='; "
          "du -xhd1 \"$HOME\" 2>/dev/null | sort -rh | head -n 15 || echo '(du unavailable)'"});

    // ── Logs ─────────────────────────────────────────────────────────────
    } else if (method == "logs_journal") {
      r = RunPrivileged(
          "if command -v journalctl >/dev/null 2>&1; then "
          "  journalctl -b -n 400 --no-pager 2>/dev/null || SUDO journalctl -b -n 400 --no-pager 2>&1; "
          "else echo 'journalctl not available'; fi",
          password);
    } else if (method == "logs_dmesg") {
      r = RunPrivileged(
          "dmesg 2>/dev/null | tail -n 400 || "
          "SUDO dmesg 2>/dev/null | tail -n 400 || "
          "echo 'Kernel log not accessible. Enter your password in Administrator access above, "
          "or run: sudo sysctl kernel.dmesg_restrict=0'",
          password);

    // ── Boot ─────────────────────────────────────────────────────────────
    } else if (method == "boot") {
      r = RunCommandCapture({"bash", "-lc",
          "echo '== Running kernel =='; uname -r; echo; "
          "echo '== Boot partition =='; df -h /boot 2>/dev/null || echo '(not found)'; echo; "
          "echo '== Bootloader =='; "
          "(command -v efibootmgr >/dev/null 2>&1 && efibootmgr 2>/dev/null | head -n 12) || "
          "(grub-install --version 2>/dev/null) || echo 'Unknown bootloader'; echo; "
          "echo '== Boot timing =='; "
          "systemd-analyze 2>/dev/null | head -n 1 || echo '(systemd-analyze not available)'; echo; "
          "systemd-analyze blame 2>/dev/null | head -n 12 || true; echo; "
          "echo '== /boot files =='; ls -lh /boot 2>/dev/null | tail -n 15 || echo '(boot directory not found)'"});

    // ── Thermal (live) ───────────────────────────────────────────────────
    // Emits compact pipe-delimited lines the Thermal tab parses & graphs:
    //   T|<chip>|<label>|<millidegC>       (thermal_zone + hwmon temp*_input)
    //   F|<chip>|<label>|<rpm>             (hwmon fan*_input)
    } else if (method == "thermal_read") {
      r = RunCommandCapture({"bash", "-lc",
          "for z in /sys/class/thermal/thermal_zone*; do "
          "  [ -r \"$z/temp\" ] || continue; "
          "  ty=$(cat \"$z/type\" 2>/dev/null); t=$(cat \"$z/temp\" 2>/dev/null); "
          "  printf 'T|zone|%s|%s\\n' \"${ty:-zone}\" \"$t\"; done; "
          "for h in /sys/class/hwmon/hwmon*; do "
          "  nm=$(cat \"$h/name\" 2>/dev/null); "
          "  for ti in \"$h\"/temp*_input; do [ -r \"$ti\" ] || continue; "
          "    b=${ti%_input}; l=$(cat \"${b}_label\" 2>/dev/null); v=$(cat \"$ti\" 2>/dev/null); "
          "    printf 'T|%s|%s|%s\\n' \"${nm:-hwmon}\" \"${l:-$(basename \"$b\")}\" \"$v\"; done; "
          "  for fi in \"$h\"/fan*_input; do [ -r \"$fi\" ] || continue; "
          "    b=${fi%_input}; l=$(cat \"${b}_label\" 2>/dev/null); v=$(cat \"$fi\" 2>/dev/null); "
          "    printf 'F|%s|%s|%s\\n' \"${nm:-hwmon}\" \"${l:-$(basename \"$b\")}\" \"$v\"; done; "
          "done"});

    // ── Fan-control capability detection (read-only) ─────────────────────
    //   SENSORS|yes|no      I8KCTL|yes|no
    //   IFACE|thinkpad|/proc/acpi/ibm/fan|thinkpad_acpi|<rw|root>
    //   IFACE|pwm|<path>|<chip>|<yes|no enable node>|<current pwm>
    } else if (method == "fan_detect") {
      r = RunCommandCapture({"bash", "-lc",
          "printf 'SENSORS|%s\\n' \"$(command -v sensors >/dev/null 2>&1 && echo yes || echo no)\"; "
          "printf 'I8KCTL|%s\\n' \"$(command -v i8kctl >/dev/null 2>&1 && echo yes || echo no)\"; "
          "if [ -e /proc/acpi/ibm/fan ]; then "
          "  printf 'IFACE|thinkpad|/proc/acpi/ibm/fan|thinkpad_acpi|%s\\n' "
          "    \"$([ -w /proc/acpi/ibm/fan ] && echo rw || echo root)\"; fi; "
          "for h in /sys/class/hwmon/hwmon*; do nm=$(cat \"$h/name\" 2>/dev/null); "
          "  for p in \"$h\"/pwm*; do case \"$p\" in *_*) continue;; esac; "
          "    [ -e \"$p\" ] || continue; en=no; [ -e \"${p}_enable\" ] && en=yes; "
          "    printf 'IFACE|pwm|%s|%s|%s|%s\\n' \"$p\" \"${nm:-hwmon}\" \"$en\" "
          "      \"$(cat \"$p\" 2>/dev/null)\"; "
          "  done; done"});

    // ── Fan write (root) ─────────────────────────────────────────────────
    } else if (method == "fan_set") {
      const std::string iface = ExtractParam(req, "iface");
      if (iface == "thinkpad") {
        const std::string level = ExtractParam(req, "level");
        if (!SafeFanLevel(level)) { r = {.exit_code = 1, .out = "invalid fan level"}; }
        else r = RunPrivileged(
            "printf 'level " + level + "\\n' | SUDO tee /proc/acpi/ibm/fan >/dev/null "
            "&& echo 'OK level=" + level + "' "
            "|| echo 'write failed (needs thinkpad_acpi fan_control=1 and root)'",
            password);
      } else if (iface == "pwm") {
        const std::string path = ExtractParam(req, "path");
        const std::string value = ExtractParam(req, "value");
        if (!SafePwmPath(path)) { r = {.exit_code = 1, .out = "invalid pwm path"}; }
        else if (!SafeByte(value)) { r = {.exit_code = 1, .out = "invalid pwm value (0-255)"}; }
        else {
          r = RunPrivileged(
              "P=" + ShellEscape(path) + "; V=" + value + "; "
              "[ -e \"$P\" ] || { echo 'pwm interface gone'; exit 1; }; "
              "[ -e \"${P}_enable\" ] && printf '1\\n' | SUDO tee \"${P}_enable\" >/dev/null 2>&1; "
              "if printf '%s\\n' \"$V\" | SUDO tee \"$P\" >/dev/null; then "
              "  echo \"OK pwm=$(cat \"$P\" 2>/dev/null)\"; "
              "else echo 'write failed (firmware may lock fan control)'; fi",
              password);
        }
      } else { r = {.exit_code = 1, .out = "invalid iface"}; }

    // ── Hand the fan back to firmware (root) ─────────────────────────────
    } else if (method == "fan_auto") {
      const std::string iface = ExtractParam(req, "iface");
      if (iface == "thinkpad") {
        r = RunPrivileged(
            "printf 'level auto\\n' | SUDO tee /proc/acpi/ibm/fan >/dev/null "
            "&& echo 'BIOS auto restored' || echo 'failed'",
            password);
      } else if (iface == "pwm") {
        const std::string path = ExtractParam(req, "path");
        if (!SafePwmPath(path)) { r = {.exit_code = 1, .out = "invalid pwm path"}; }
        else r = RunPrivileged(
            "P=" + ShellEscape(path) + "; "
            "if [ -e \"${P}_enable\" ]; then "
            "  printf '2\\n' | SUDO tee \"${P}_enable\" >/dev/null 2>&1 "
            "    && echo 'BIOS auto control restored (pwm_enable=2)' "
            "  || { printf '0\\n' | SUDO tee \"${P}_enable\" >/dev/null "
            "         && echo 'fan set to full/no-control (pwm_enable=0)'; }; "
            "else echo 'No pwm_enable node: firmware reclaims fan control automatically within seconds'; fi",
            password);
      } else { r = {.exit_code = 1, .out = "invalid iface"}; }

    // ── Power profiles + governor ────────────────────────────────────────
    } else if (method == "power_read") {
      r = RunCommandCapture({"bash", "-lc",
          "echo '== Power profile (power-profiles-daemon) =='; "
          "if command -v powerprofilesctl >/dev/null 2>&1; then "
          "  echo \"Active: $(powerprofilesctl get 2>/dev/null)\"; "
          "  echo 'Available:'; powerprofilesctl list 2>/dev/null "
          "    | sed -n 's/^[* ] *\\([a-z-]*\\):/  \\1/p'; "
          "else echo '(power-profiles-daemon not installed)'; fi; echo; "
          "echo '== CPU frequency governor =='; "
          "g=/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor; "
          "if [ -r \"$g\" ]; then echo \"Current: $(cat \"$g\")\"; "
          "  echo \"Available: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors 2>/dev/null)\"; "
          "  echo \"Driver: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_driver 2>/dev/null)\"; "
          "else echo '(cpufreq not available)'; fi; echo; "
          "echo '== Power source =='; "
          "for ps in /sys/class/power_supply/*; do [ -r \"$ps/type\" ] || continue; "
          "  ty=$(cat \"$ps/type\"); "
          "  if [ \"$ty\" = Mains ]; then "
          "    echo \"AC ($(basename \"$ps\")): $([ \"$(cat \"$ps/online\" 2>/dev/null)\" = 1 ] && echo online || echo offline)\"; "
          "  elif [ \"$ty\" = Battery ]; then "
          "    echo \"Battery ($(basename \"$ps\")): $(cat \"$ps/capacity\" 2>/dev/null)% $(cat \"$ps/status\" 2>/dev/null)\"; fi; "
          "done"});
    } else if (method == "power_set") {
      const std::string profile = ExtractParam(req, "profile");
      if (!SafeProfile(profile)) { r = {.exit_code = 1, .out = "invalid profile"}; }
      else r = RunCommandCapture({"bash", "-lc",
          "command -v powerprofilesctl >/dev/null 2>&1 || { echo 'power-profiles-daemon not installed'; exit 1; }; "
          "powerprofilesctl set " + profile + " && echo 'OK: " + profile + "' "
          "|| echo 'failed (is power-profiles-daemon running?)'"});
    } else if (method == "power_governor") {
      const std::string gov = ExtractParam(req, "governor");
      if (!SafeGovernor(gov)) { r = {.exit_code = 1, .out = "invalid governor"}; }
      else r = RunPrivileged(
          "n=0; for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do "
          "  [ -e \"$g\" ] || continue; "
          "  printf '%s\\n' '" + gov + "' | SUDO tee \"$g\" >/dev/null 2>&1 && n=$((n+1)); done; "
          "echo \"Set governor '" + gov + "' on $n CPU(s)\"; "
          "echo \"Now: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)\"",
          password);

    } else {
      r = {.exit_code = 1, .out = "unknown method: " + method};
    }
    return ToJson(r);
  });

  w.set_html(html);
  w.run();
  return 0;
#endif
}
