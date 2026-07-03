/* Leviathan Reading Time — bundled demo extension for Infinitas.
 * Injected into every http(s) page via the extension system's content_scripts.
 * Shows word count + estimated reading time in a dismissable corner pill. */
(function () {
  if (!document.body) return;
  if (location.protocol !== 'http:' && location.protocol !== 'https:') return;
  if (document.getElementById('_lev_readtime_')) return;

  var text = (document.body.innerText || '').trim();
  var words = text ? text.split(/\s+/).length : 0;
  if (words < 30) return;                       /* skip trivial/empty pages */
  var mins = Math.max(1, Math.round(words / 200));

  var pill = document.createElement('div');
  pill.id = '_lev_readtime_';
  pill.textContent = '🦎 ' + words.toLocaleString() + ' words · ' + mins + ' min read';
  pill.title = 'Leviathan demo extension — click to dismiss';
  pill.style.cssText = [
    'position:fixed', 'bottom:18px', 'left:18px', 'z-index:2147483646',
    'background:#0f0f13', 'color:#4ade80', 'border:1px solid #2a2a3a',
    'border-radius:20px', 'padding:7px 14px',
    'font:600 12px system-ui,-apple-system,sans-serif',
    'box-shadow:0 4px 16px rgba(0,0,0,.4)', 'cursor:pointer',
    'opacity:.9', 'user-select:none'
  ].join(';');
  pill.addEventListener('click', function () { pill.remove(); });
  document.body.appendChild(pill);
})();
