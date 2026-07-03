#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <jsc/jsc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pptx.h"

#define MAX_PATH 1024

typedef struct {
    GtkWindow *window;
    WebKitWebView *web_view;
    char current_file[MAX_PATH];
    char startup_file[MAX_PATH];   /* deck passed on the command line */
    gboolean loaded;               /* page ready + startup deck injected */
} SwipeApp;

static void swipe_update_title(SwipeApp *app) {
    const char *base = app->current_file[0]
        ? (strrchr(app->current_file, '/') ? strrchr(app->current_file, '/') + 1
                                           : app->current_file)
        : "Untitled";
    char *t = g_strdup_printf("%s - Swipe", base);
    gtk_window_set_title(app->window, t);
    g_free(t);
}

/* ---------------- embedded editor (HTML + JS) ---------------- */
static const char *HTML_TEMPLATE =
"<!DOCTYPE html>"
"<html><head><meta charset='UTF-8'>"
"<style>"
"* { margin:0; padding:0; box-sizing:border-box; }"
"body { font-family:Arial,sans-serif; background:#1a1a1a; color:#fff; display:flex; flex-direction:column; height:100vh; overflow:hidden; }"
"#toolbar { background:#0a140f; padding:8px; border-bottom:2px solid #33ff33; display:flex; gap:4px; flex-wrap:wrap; align-items:center; }"
".btn { padding:7px 11px; background:#030705; color:#33ff33; border:1px solid #33ff33; cursor:pointer; font-size:11px; font-weight:bold; }"
".btn:hover { background:#33ff33; color:#030705; }"
".btn.on { background:#33ff33; color:#030705; }"
".sep { width:1px; align-self:stretch; background:#1c3a25; margin:0 4px; }"
"#main { flex:1; display:flex; min-height:0; }"
"#thumbs { width:170px; background:#08110c; border-right:2px solid #33ff33; overflow-y:auto; padding:6px; }"
".thumb { position:relative; width:100%; height:118px; margin-bottom:8px; border:2px solid #1c3a25; background:#000; cursor:pointer; overflow:hidden; }"
".thumb.cur { border-color:#33ff33; }"
".thumb .mini { position:absolute; top:0; left:0; width:960px; height:720px; transform:scale(0.158); transform-origin:top left; }"
".thumb .num { position:absolute; top:2px; left:4px; font-size:11px; color:#33ff33; z-index:5; text-shadow:0 0 2px #000; }"
".thumb .ops { position:absolute; bottom:2px; right:2px; z-index:5; display:flex; gap:2px; }"
".thumb .ops span { background:#030705; color:#33ff33; border:1px solid #33ff33; font-size:9px; padding:0 3px; cursor:pointer; }"
"#center { flex:1; display:flex; align-items:center; justify-content:center; background:#111; overflow:auto; padding:16px; }"
"#slide { width:960px; height:720px; background:#fff; position:relative; box-shadow:0 0 24px rgba(0,0,0,.6); flex:none; }"
".obj { position:absolute; overflow:hidden; }"
".obj.sel { outline:2px solid #0099FF; }"
".text-obj { color:#000; white-space:pre-wrap; word-wrap:break-word; padding:4px; cursor:move; }"
".text-obj[contenteditable=true] { cursor:text; outline:2px dashed #0099FF; background:rgba(0,153,255,.06); }"
".text-obj ul { margin-left:20px; }"
".handle { position:absolute; width:10px; height:10px; background:#0099FF; border:1px solid #fff; z-index:20; }"
"#props { width:190px; background:#0a140f; border-left:2px solid #33ff33; padding:10px; overflow-y:auto; color:#33ff33; font-size:11px; }"
".prop { margin:7px 0; }"
".prop label { display:block; margin-bottom:3px; font-weight:bold; }"
".prop input, .prop select { width:100%; padding:4px; background:#030705; color:#33ff33; border:1px solid #33ff33; font-size:10px; }"
".swatch { display:inline-block; width:20px; height:20px; border:1px solid #33ff33; cursor:pointer; margin:1px; }"
"#status { padding:6px 10px; background:#0a140f; border-top:1px solid #33ff33; color:#33ff33; font-size:10px; font-family:monospace; }"
"#present { position:fixed; inset:0; background:#000; z-index:1000; display:none; align-items:center; justify-content:center; }"
"#present .stage { position:relative; }"
"#present .pslide { width:960px; height:720px; position:relative; transform-origin:center; }"
"#phint { position:fixed; bottom:8px; left:8px; color:#888; font-size:12px; z-index:1001; }"
"#pcount { position:fixed; bottom:8px; right:12px; color:#888; font-size:13px; z-index:1001; }"
"#pnotes { position:fixed; bottom:30px; left:8px; right:8px; max-height:24%; overflow:auto; color:#cfcfcf; background:rgba(0,0,0,.55); font-size:14px; line-height:1.4; padding:8px 12px; z-index:1001; display:none; white-space:pre-wrap; border-top:1px solid #333; }"
"#pnav { position:fixed; top:50%; transform:translateY(-50%); width:100%; display:flex; justify-content:space-between; pointer-events:none; z-index:1002; }"
"#pnav span { pointer-events:auto; color:#444; font-size:40px; padding:0 20px; cursor:pointer; user-select:none; }"
"#pnav span:hover { color:#33ff33; }"
"#printarea { display:none; }"
"@page { size:landscape; margin:0; }"
"@media print {"
"  html,body { background:#fff; }"
"  body>*:not(#printarea) { display:none !important; }"
"  #printarea { display:block !important; }"
"  .psheet { width:960px; height:720px; position:relative; overflow:hidden; page-break-after:always; }"
"}"
"#aboutov{position:fixed;inset:0;background:rgba(0,0,0,.65);z-index:99999;display:none;align-items:center;justify-content:center;}"
"#aboutov.on{display:flex;}"
"#aboutbox{background:#0a140f;border:2px solid #33ff33;color:#33ff33;max-width:480px;margin:16px;padding:20px;font-family:monospace;font-size:12px;line-height:1.7;box-shadow:0 0 24px rgba(51,255,51,.35);}"
"#aboutbox .atitle{color:#8fffa0;font-weight:bold;font-size:13px;margin-bottom:10px;}"
"#aboutbox .aok{margin-top:16px;text-align:right;}"
"</style></head><body>"
"<div id='toolbar'>"
"  <button class='btn' id='t_text' onclick='setTool(\"text\")'>TEXT</button>"
"  <button class='btn' id='t_rect' onclick='setTool(\"rect\")'>RECT</button>"
"  <button class='btn' id='t_ellipse' onclick='setTool(\"ellipse\")'>ELLIPSE</button>"
"  <button class='btn' id='t_line' onclick='setTool(\"line\")'>LINE</button>"
"  <button class='btn' onclick='addImage()'>IMAGE</button>"
"  <div class='sep'></div>"
"  <button class='btn' onclick='undo()' title='Ctrl+Z'>UNDO</button>"
"  <button class='btn' onclick='redo()' title='Ctrl+Y'>REDO</button>"
"  <div class='sep'></div>"
"  <button class='btn' onclick='addSlide()'>+SLIDE</button>"
"  <button class='btn' onclick='dupSlide()'>DUP</button>"
"  <button class='btn' onclick='delSlide()'>-SLIDE</button>"
"  <button class='btn' onclick='prevSlide()'>&#9664;</button>"
"  <button class='btn' onclick='nextSlide()'>&#9654;</button>"
"  <div class='sep'></div>"
"  <button class='btn' onclick='dupObj()' title='Ctrl+D'>DUP OBJ</button>"
"  <button class='btn' onclick='toFront()'>FRONT</button>"
"  <button class='btn' onclick='toBack()'>BACK</button>"
"  <button class='btn' onclick='deleteObj()'>DELETE</button>"
"  <button class='btn' onclick='present()'>PRESENT</button>"
"  <div class='sep'></div>"
"  <button class='btn' onclick='doNew()'>NEW</button>"
"  <button class='btn' onclick='doLoad()'>LOAD</button>"
"  <button class='btn' onclick='doSave()'>SAVE</button>"
"  <button class='btn' onclick='doExport()'>EXPORT PPTX</button>"
"  <button class='btn' onclick='exportPdf()'>PDF</button>"
"  <div class='sep'></div>"
"  <button class='btn' onclick='showAbout()'>ABOUT</button>"
"  <input type='file' id='imgfile' accept='image/*' style='display:none'>"
"</div>"
"<div id='main'>"
"  <div id='thumbs'></div>"
"  <div id='center'><div id='slide'></div></div>"
"  <div id='props'>"
"    <div class='prop'><label>Slide Background</label>"
"      <input type='color' id='bgColor' onchange='setBg(this.value)'></div>"
"    <div class='prop'><label>Themes</label><div id='themes'></div></div>"
"    <hr style='border-color:#1c3a25'>"
"    <div id='textprops'>"
"      <div class='prop'><label>Font</label>"
"        <select id='font' onchange='updateSel()'>"
"          <option>Arial</option><option>Georgia</option><option>Times New Roman</option>"
"          <option>Courier New</option><option>Verdana</option><option>Impact</option>"
"          <option>Comic Sans MS</option></select></div>"
"      <div class='prop'><label>Font Size</label>"
"        <input type='number' id='fontSize' value='24' min='6' max='200' onchange='updateSel()'></div>"
"      <div class='prop'><label><input type='checkbox' id='bold' onchange='updateSel()'> Bold</label></div>"
"      <div class='prop'><label><input type='checkbox' id='italic' onchange='updateSel()'> Italic</label></div>"
"      <div class='prop'><label><input type='checkbox' id='underline' onchange='updateSel()'> Underline</label></div>"
"      <div class='prop'><label><input type='checkbox' id='bullet' onchange='updateSel()'> Bullet list</label></div>"
"      <div class='prop'><label>Align</label>"
"        <select id='align' onchange='updateSel()'>"
"          <option value='0'>Left</option><option value='1'>Center</option><option value='2'>Right</option>"
"        </select></div>"
"      <div class='prop'><label>Text Color</label>"
"        <input type='color' id='color' value='#000000' onchange='updateSel()'></div>"
"    </div>"
"    <div id='shapeprops'>"
"      <div class='prop'><label>Fill</label>"
"        <input type='color' id='fill' value='#ff0000' onchange='updateSel()'></div>"
"      <div class='prop'><label>Stroke</label>"
"        <input type='color' id='stroke' value='#aa0000' onchange='updateSel()'></div>"
"    </div>"
"    <hr style='border-color:#1c3a25'>"
"    <div class='prop'><label>Speaker Notes (this slide)</label>"
"      <textarea id='notes' rows='4' style='width:100%;padding:4px;background:#030705;color:#33ff33;border:1px solid #33ff33;font-size:10px;font-family:monospace' onchange='setNotes(this.value)'></textarea></div>"
"    <div class='prop' style='color:#888;font-size:10px'>Dbl-click text to edit.<br>Arrows nudge. Del removes.<br>Ctrl+Z undo, Ctrl+D duplicate.<br>Ctrl+S save. F5 present.</div>"
"  </div>"
"</div>"
"<div id='status'>&gt; Ready</div>"
"<div id='present'><div class='stage'><div class='pslide' id='pslide'></div></div>"
"<div id='pcount'></div><div id='pnotes'></div>"
"<div id='pnav'><span onclick='pPrev(event)'>&#9664;</span><span onclick='pNext(event)'>&#9654;</span></div></div>"
"<div id='phint' style='display:none'>click / &larr; &rarr; navigate &nbsp; Esc exit</div>"
"<div id='printarea'></div>"
"<script>"
"var THEMES=[['White','#ffffff','#000000'],['Dark','#1e1e2e','#e0e0e0'],['Blue','#0b3d91','#ffffff'],['Warm','#fff4e0','#3a2a10'],['Slate','#2f3b47','#eef2f5'],['Mint','#e6fff2','#0a3d2a']];"
"var deck={slides:[{bg:'#ffffff',objs:[]}]};"
"var cur=0, tool='text', sel=null, editing=null, action=null;"
"var presMode=false, presIdx=0;"
""
"function slide(){return deck.slides[cur];}"
"function esc(s){return (s||'').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}"
"function clamp(v,a,b){return Math.max(a,Math.min(b,v));}"
""
"/* ---- undo/redo + object clipboard + z-order ---- */"
"var _undo=[],_redo=[],clip=null;"
"function snapshot(){_undo.push(JSON.stringify(deck));if(_undo.length>80)_undo.shift();_redo=[];}"
"function undo(){if(!_undo.length)return;_redo.push(JSON.stringify(deck));deck=JSON.parse(_undo.pop());if(cur>=deck.slides.length)cur=deck.slides.length-1;sel=null;renderAll();}"
"function redo(){if(!_redo.length)return;_undo.push(JSON.stringify(deck));deck=JSON.parse(_redo.pop());if(cur>=deck.slides.length)cur=deck.slides.length-1;sel=null;renderAll();}"
"function copyObj(){if(sel)clip=JSON.parse(JSON.stringify(sel));}"
"function pasteObj(){if(!clip)return;snapshot();var o=JSON.parse(JSON.stringify(clip));o.id=newId();o.x=clamp((o.x||0)+20,0,960-o.w);o.y=clamp((o.y||0)+20,0,720-o.h);slide().objs.push(o);sel=o;renderAll();}"
"function dupObj(){copyObj();pasteObj();}"
"function toFront(){if(!sel)return;snapshot();var a=slide().objs,i=a.indexOf(sel);if(i>=0){a.splice(i,1);a.push(sel);}renderAll();}"
"function toBack(){if(!sel)return;snapshot();var a=slide().objs,i=a.indexOf(sel);if(i>=0){a.splice(i,1);a.unshift(sel);}renderAll();}"
""
"function setTool(t){tool=t;['text','rect','ellipse','line'].forEach(function(k){var e=document.getElementById('t_'+k);if(e)e.classList.toggle('on',k===t);});}"
""
"function newId(){return 'o'+Date.now()+'_'+Math.floor(Math.random()*10000);}"
""
"/* ---- build one object's inner DOM (no handlers): used by editor, thumbs, present ---- */"
"function paint(el,o){"
"  el.style.left=o.x+'px'; el.style.top=o.y+'px'; el.style.width=o.w+'px'; el.style.height=o.h+'px';"
"  if(o.type==='text'){"
"    el.className='obj text-obj';"
"    el.style.fontSize=(o.size||24)+'px';"
"    el.style.fontFamily=o.font||'Arial';"
"    el.style.fontWeight=o.bold?'bold':'normal';"
"    el.style.fontStyle=o.italic?'italic':'normal';"
"    el.style.textDecoration=o.underline?'underline':'none';"
"    el.style.color=o.color||'#000';"
"    el.style.textAlign=(o.align==1?'center':o.align==2?'right':'left');"
"    if(o.bullet){"
"      var lines=(o.text||'').split('\\n');"
"      el.innerHTML='<ul>'+lines.map(function(l){return '<li>'+esc(l)+'</li>';}).join('')+'</ul>';"
"    } else { el.innerHTML=esc(o.text||''); }"
"  } else if(o.type==='image'){"
"    el.className='obj';"
"    el.innerHTML='';"
"    el.style.backgroundImage='url('+o.src+')';"
"    el.style.backgroundSize='100% 100%';"
"    el.style.backgroundRepeat='no-repeat';"
"  } else if(o.type==='line'){"
"    el.className='obj';"
"    el.innerHTML='<svg width=\"100%\" height=\"100%\" preserveAspectRatio=\"none\" viewBox=\"0 0 100 100\"><line x1=\"0\" y1=\"100\" x2=\"100\" y2=\"0\" stroke=\"'+(o.stroke||'#000')+'\" stroke-width=\"3\" vector-effect=\"non-scaling-stroke\"/></svg>';"
"  } else {" /* rect / ellipse */
"    el.className='obj';"
"    el.style.background=o.fill||'#ff0000';"
"    el.style.border='2px solid '+(o.stroke||'#aa0000');"
"    el.style.borderRadius=(o.type==='ellipse')?'50%':'0';"
"    el.innerHTML='';"
"  }"
"}"
""
"/* ---- interactive editor render ---- */"
"function render(){"
"  var sd=document.getElementById('slide');"
"  sd.innerHTML='';"
"  sd.style.background=slide().bg;"
"  slide().objs.forEach(function(o){"
"    var el=document.createElement('div');"
"    paint(el,o);"
"    if(sel===o) el.classList.add('sel');"
"    el.onmousedown=function(e){"
"      if(editing) return;"
"      if(e.target.classList.contains('handle')) return;"
"      e.preventDefault();"
"      selectObj(o);"
"      action={mode:'move',o:o,sx:e.clientX,sy:e.clientY,ox:o.x,oy:o.y};"
"    };"
"    el.ondblclick=function(e){ e.stopPropagation(); if(o.type==='text') startEdit(o,el); };"
"    sd.appendChild(el);"
"    if(sel===o && !editing) addHandles(sd,o);"
"  });"
"  document.getElementById('status').innerHTML='&gt; Slide '+(cur+1)+'/'+deck.slides.length+' | '+slide().objs.length+' object(s) | tool: '+tool;"
"  syncProps();"
"}"
""
"function addHandles(sd,o){"
"  var cs=[['nw',0,0],['ne',1,0],['sw',0,1],['se',1,1]];"
"  cs.forEach(function(c){"
"    var h=document.createElement('div'); h.className='handle';"
"    h.style.left=(o.x+c[1]*o.w-5)+'px'; h.style.top=(o.y+c[2]*o.h-5)+'px';"
"    h.style.cursor=(c[0]==='nw'||c[0]==='se')?'nwse-resize':'nesw-resize';"
"    h.onmousedown=function(e){ e.preventDefault(); e.stopPropagation();"
"      action={mode:'resize',o:o,corner:c[0],sx:e.clientX,sy:e.clientY,ox:o.x,oy:o.y,ow:o.w,oh:o.h}; };"
"    sd.appendChild(h);"
"  });"
"}"
""
"function startEdit(o,el){"
"  snapshot();"
"  editing=o; sel=o;"
"  el.classList.remove('sel');"
"  el.innerHTML=esc(o.text||'');" /* edit raw text, bullets applied on blur */
"  el.contentEditable=true;"
"  el.focus();"
"  var r=document.createRange(); r.selectNodeContents(el); r.collapse(false);"
"  var s=window.getSelection(); s.removeAllRanges(); s.addRange(r);"
"  el.onblur=function(){"
"    o.text=el.innerText.replace(/\\n$/,'');"
"    el.contentEditable=false; editing=null; render();"
"  };"
"}"
""
"function selectObj(o){ sel=o; editing=null; render(); }"
""
"/* ---- global drag/resize ---- */"
"document.addEventListener('mousemove',function(e){"
"  if(!action) return;"
"  if(!action.snapped){ snapshot(); action.snapped=true; }"  /* one undo step per drag */
"  var o=action.o;"
"  var dx=e.clientX-action.sx, dy=e.clientY-action.sy;"
"  if(action.mode==='move'){"
"    o.x=clamp(action.ox+dx,0,960-o.w); o.y=clamp(action.oy+dy,0,720-o.h);"
"  } else {"
"    var nx=action.ox,ny=action.oy,nw=action.ow,nh=action.oh;"
"    if(action.corner.indexOf('e')>=0) nw=action.ow+dx;"
"    if(action.corner.indexOf('s')>=0) nh=action.oh+dy;"
"    if(action.corner.indexOf('w')>=0){ nw=action.ow-dx; nx=action.ox+dx; }"
"    if(action.corner.indexOf('n')>=0){ nh=action.oh-dy; ny=action.oy+dy; }"
"    if(nw<15){ nw=15; } if(nh<15){ nh=15; }"
"    o.x=clamp(nx,0,960-15); o.y=clamp(ny,0,720-15);"
"    o.w=clamp(nw,15,960-o.x); o.h=clamp(nh,15,720-o.y);"
"  }"
"  render();"
"});"
"document.addEventListener('mouseup',function(){ if(action){action=null; renderThumbs();} });"
""
"/* ---- click empty slide to create ---- */"
"document.getElementById('slide').addEventListener('mousedown',function(e){"
"  if(e.target.id!=='slide') return;"
"  if(editing){ return; }"
"  var r=e.currentTarget.getBoundingClientRect();"
"  var x=clamp(e.clientX-r.left,0,900), y=clamp(e.clientY-r.top,0,660);"
"  var o=null;"
"  if(tool==='text') o={id:newId(),type:'text',x:x,y:y,w:260,h:60,text:'Text',size:24,color:'#000000',bold:false,italic:false,bullet:false,align:0};"
"  else if(tool==='rect') o={id:newId(),type:'rect',x:x,y:y,w:200,h:120,fill:'#4472c4',stroke:'#2a4a90'};"
"  else if(tool==='ellipse') o={id:newId(),type:'ellipse',x:x,y:y,w:180,h:180,fill:'#ed7d31',stroke:'#b45a1e'};"
"  else if(tool==='line') o={id:newId(),type:'line',x:x,y:y,w:200,h:120,stroke:'#000000'};"
"  if(o){ snapshot(); slide().objs.push(o); sel=o; renderAll(); }"
"});"
""
"function deleteObj(){ if(sel){ snapshot(); var i=slide().objs.indexOf(sel); if(i>=0) slide().objs.splice(i,1); sel=null; } renderAll(); }"
""
"/* ---- props ---- */"
"function syncProps(){"
"  document.getElementById('bgColor').value=slide().bg;"
"  document.getElementById('notes').value=slide().notes||'';"
"  var tp=document.getElementById('textprops'), sp=document.getElementById('shapeprops');"
"  if(sel && sel.type==='text'){"
"    tp.style.display='block'; sp.style.display='none';"
"    document.getElementById('fontSize').value=sel.size||24;"
"    document.getElementById('bold').checked=!!sel.bold;"
"    document.getElementById('italic').checked=!!sel.italic;"
"    document.getElementById('bullet').checked=!!sel.bullet;"
"    document.getElementById('underline').checked=!!sel.underline;"
"    document.getElementById('font').value=sel.font||'Arial';"
"    document.getElementById('align').value=sel.align||0;"
"    document.getElementById('color').value=sel.color||'#000000';"
"  } else if(sel && (sel.type==='rect'||sel.type==='ellipse'||sel.type==='line')){"
"    tp.style.display='none'; sp.style.display='block';"
"    document.getElementById('fill').value=sel.fill||'#4472c4';"
"    document.getElementById('stroke').value=sel.stroke||'#000000';"
"  } else { tp.style.display='none'; sp.style.display='none'; }"
"}"
"function updateSel(){"
"  if(!sel) return;"
"  snapshot();"
"  if(sel.type==='text'){"
"    sel.size=parseInt(document.getElementById('fontSize').value)||24;"
"    sel.bold=document.getElementById('bold').checked;"
"    sel.italic=document.getElementById('italic').checked;"
"    sel.bullet=document.getElementById('bullet').checked;"
"    sel.underline=document.getElementById('underline').checked;"
"    sel.font=document.getElementById('font').value;"
"    sel.align=parseInt(document.getElementById('align').value)||0;"
"    sel.color=document.getElementById('color').value;"
"  } else {"
"    sel.fill=document.getElementById('fill').value;"
"    sel.stroke=document.getElementById('stroke').value;"
"  }"
"  renderAll();"
"}"
"function setBg(c){ snapshot(); slide().bg=c; renderAll(); }"
""
"function buildThemes(){"
"  var t=document.getElementById('themes'); t.innerHTML='';"
"  THEMES.forEach(function(th){"
"    var s=document.createElement('span'); s.className='swatch'; s.title=th[0]; s.style.background=th[1];"
"    s.onclick=function(){ snapshot(); slide().bg=th[1]; slide().objs.forEach(function(o){ if(o.type==='text') o.color=th[2]; }); renderAll(); };"
"    t.appendChild(s);"
"  });"
"}"
""
"/* ---- slides ---- */"
"function blankSlide(){ return {bg:'#ffffff',objs:[],notes:''}; }"
"function setNotes(v){ snapshot(); slide().notes=v; }"
"function addSlide(){ snapshot(); deck.slides.splice(cur+1,0,blankSlide()); cur++; sel=null; renderAll(); }"
"function dupSlide(){ snapshot(); var c=JSON.parse(JSON.stringify(slide())); deck.slides.splice(cur+1,0,c); cur++; sel=null; renderAll(); }"
"function delSlide(){ snapshot(); if(deck.slides.length>1){ deck.slides.splice(cur,1); cur=Math.max(0,cur-1); } else { deck.slides[0]=blankSlide(); } sel=null; renderAll(); }"
"function prevSlide(){ if(cur>0){cur--; sel=null; renderAll();} }"
"function nextSlide(){ if(cur<deck.slides.length-1){cur++; sel=null; renderAll();} }"
"function moveSlide(i,d){ var j=i+d; if(j<0||j>=deck.slides.length) return; snapshot(); var t=deck.slides[i]; deck.slides[i]=deck.slides[j]; deck.slides[j]=t; if(cur===i)cur=j; else if(cur===j)cur=i; renderAll(); }"
""
"function renderThumbs(){"
"  var c=document.getElementById('thumbs'); c.innerHTML='';"
"  deck.slides.forEach(function(sl,i){"
"    var t=document.createElement('div'); t.className='thumb'+(i===cur?' cur':'');"
"    var mini=document.createElement('div'); mini.className='mini'; mini.style.background=sl.bg;"
"    sl.objs.forEach(function(o){ var el=document.createElement('div'); paint(el,o); mini.appendChild(el); });"
"    t.appendChild(mini);"
"    var num=document.createElement('div'); num.className='num'; num.textContent=(i+1); t.appendChild(num);"
"    var ops=document.createElement('div'); ops.className='ops';"
"    var up=document.createElement('span'); up.textContent='\\u25B2'; up.onclick=function(e){e.stopPropagation();moveSlide(i,-1);};"
"    var dn=document.createElement('span'); dn.textContent='\\u25BC'; dn.onclick=function(e){e.stopPropagation();moveSlide(i,1);};"
"    ops.appendChild(up); ops.appendChild(dn); t.appendChild(ops);"
"    t.onclick=function(){ cur=i; sel=null; renderAll(); };"
"    c.appendChild(t);"
"  });"
"}"
"function renderAll(){ render(); renderThumbs(); }"
""
"/* ---- images ---- */"
"function addImage(){ document.getElementById('imgfile').click(); }"
"document.getElementById('imgfile').addEventListener('change',function(e){"
"  var f=e.target.files[0]; if(!f) return;"
"  var rd=new FileReader();"
"  rd.onload=function(){"
"    var img=new Image();"
"    img.onload=function(){"
"      var w=img.width,h=img.height,mx=480,my=360;"
"      var sc=Math.min(1,mx/w,my/h); w=Math.round(w*sc); h=Math.round(h*sc);"
"      snapshot(); var o={id:newId(),type:'image',x:60,y:60,w:w,h:h,src:rd.result};"
"      slide().objs.push(o); sel=o; renderAll();"
"    };"
"    img.src=rd.result;"
"  };"
"  rd.readAsDataURL(f); e.target.value='';"
"});"
""
"/* ---- present mode ---- */"
"function renderPresent(){"
"  var sl=deck.slides[presIdx];"
"  var ps=document.getElementById('pslide'); ps.innerHTML=''; ps.style.background=sl.bg;"
"  sl.objs.forEach(function(o){ var el=document.createElement('div'); paint(el,o); ps.appendChild(el); });"
"  var sc=Math.min(window.innerWidth/960, window.innerHeight/720);"
"  ps.style.transform='scale('+sc+')';"
"  document.querySelector('#present .stage').style.width=(960*sc)+'px';"
"  document.querySelector('#present .stage').style.height=(720*sc)+'px';"
"  ps.style.transformOrigin='top left';"
"  document.getElementById('pcount').textContent=(presIdx+1)+' / '+deck.slides.length;"
"  var nt=deck.slides[presIdx].notes||''; var pn=document.getElementById('pnotes');"
"  pn.textContent=nt; pn.style.display=nt?'block':'none';"
"}"
"function pNext(e){ if(e)e.stopPropagation(); if(presIdx<deck.slides.length-1){presIdx++;renderPresent();} }"
"function pPrev(e){ if(e)e.stopPropagation(); if(presIdx>0){presIdx--;renderPresent();} }"
"function present(){ presMode=true; presIdx=cur; document.getElementById('present').style.display='flex';"
"  document.getElementById('phint').style.display='block'; renderPresent(); }"
"document.getElementById('present').addEventListener('click',function(ev){"
"  if(ev.target.id==='pnotes'||ev.target.closest('#pnav')) return; pNext(); });"
"/* ---- PDF export: render every slide to a print sheet, then print-to-PDF ---- */"
"function exportPdf(){ var pa=document.getElementById('printarea'); pa.innerHTML='';"
"  deck.slides.forEach(function(sl){ var pg=document.createElement('div'); pg.className='psheet';"
"    pg.style.background=sl.bg;"
"    sl.objs.forEach(function(o){ var el=document.createElement('div'); paint(el,o); pg.appendChild(el); });"
"    pa.appendChild(pg); });"
"  window.webkit.messageHandlers.pdf.postMessage(''); }"
"function exitPresent(){ presMode=false; document.getElementById('present').style.display='none';"
"  document.getElementById('phint').style.display='none'; }"
"window.addEventListener('resize',function(){ if(presMode) renderPresent(); });"
""
"/* ---- keyboard ---- */"
"document.addEventListener('keydown',function(e){"
"  if(presMode){"
"    if(e.key==='Escape') exitPresent();"
"    else if(e.key==='ArrowRight'||e.key==='PageDown'||e.key===' '){ if(presIdx<deck.slides.length-1){presIdx++;renderPresent();} e.preventDefault(); }"
"    else if(e.key==='ArrowLeft'||e.key==='PageUp'){ if(presIdx>0){presIdx--;renderPresent();} e.preventDefault(); }"
"    return;"
"  }"
"  if(editing) return;"
"  var ae=document.activeElement;"
"  if(ae && (ae.tagName==='INPUT'||ae.tagName==='SELECT'||ae.isContentEditable)) return;"
"  if(e.key==='F5'){ present(); e.preventDefault(); return; }"
"  if((e.ctrlKey||e.metaKey) && e.key.toLowerCase()==='s'){ doSave(); e.preventDefault(); return; }"
"  if((e.ctrlKey||e.metaKey) && e.key.toLowerCase()==='z'){ (e.shiftKey?redo:undo)(); e.preventDefault(); return; }"
"  if((e.ctrlKey||e.metaKey) && e.key.toLowerCase()==='y'){ redo(); e.preventDefault(); return; }"
"  if((e.ctrlKey||e.metaKey) && e.key.toLowerCase()==='c'){ copyObj(); e.preventDefault(); return; }"
"  if((e.ctrlKey||e.metaKey) && e.key.toLowerCase()==='v'){ pasteObj(); e.preventDefault(); return; }"
"  if((e.ctrlKey||e.metaKey) && e.key.toLowerCase()==='d'){ dupObj(); e.preventDefault(); return; }"
"  if(e.key==='Delete'||e.key==='Backspace'){ if(sel){ deleteObj(); e.preventDefault(); } return; }"
"  if(e.key==='PageDown'||e.key==='n'||e.key==='N'){ nextSlide(); e.preventDefault(); return; }"
"  if(e.key==='PageUp'||e.key==='p'||e.key==='P'){ prevSlide(); e.preventDefault(); return; }"
"  if(sel && (e.key==='ArrowLeft'||e.key==='ArrowRight'||e.key==='ArrowUp'||e.key==='ArrowDown')){"
"    snapshot();"
"    var step=e.shiftKey?1:10;"
"    if(e.key==='ArrowLeft'){ sel.x=clamp(sel.x-step,0,960-sel.w); render(); e.preventDefault(); }"
"    else if(e.key==='ArrowRight'){ sel.x=clamp(sel.x+step,0,960-sel.w); render(); e.preventDefault(); }"
"    else if(e.key==='ArrowUp'){ sel.y=clamp(sel.y-step,0,720-sel.h); render(); e.preventDefault(); }"
"    else if(e.key==='ArrowDown'){ sel.y=clamp(sel.y+step,0,720-sel.h); render(); e.preventDefault(); }"
"  }"
"});"
""
"/* ---- backend bridge ---- */"
"function doSave(){ window.webkit.messageHandlers.save.postMessage(JSON.stringify(deck)); }"
"function doExport(){ window.webkit.messageHandlers.exportpptx.postMessage(deck); }"
"function doLoad(){ window.webkit.messageHandlers.load.postMessage(''); }"
"function doNew(){ if(confirm('Start a new deck? Unsaved changes lost.')){ deck={slides:[blankSlide()]}; cur=0; sel=null; renderAll(); } }"
"function loadDeck(obj){"
"  try{ var d=(typeof obj==='string')?JSON.parse(obj):obj;"
"    if(d && d.slides && d.slides.length){ deck=d; cur=0; sel=null; renderAll(); } }catch(err){ console.log('load fail',err); }"
"}"
""
"function showAbout(){document.getElementById('aboutov').classList.add('on');}"
"function closeAbout(){document.getElementById('aboutov').classList.remove('on');}"
"setTool('text'); buildThemes(); renderAll();"
"</script>"
"<div id='aboutov' onclick='if(event.target===this)closeAbout()'>"
"<div id='aboutbox'>"
"<div class='atitle'>Swipe &mdash; part of LeviathanOS</div>"
"Free software under the GNU General Public License, version 3.<br>"
"This program comes with ABSOLUTELY NO WARRANTY.<br>"
"Full license: /usr/share/doc/leviathanos/LICENSE<br>"
"https://www.gnu.org/licenses/gpl-3.0.html"
"<div class='aok'><button class='btn' onclick='closeAbout()'>OK</button></div>"
"</div></div>"
"</body></html>";

/* ---------------- C helpers: JSC walking ---------------- */

static uint32_t parse_hex_color(const char *s) {
    if (!s) return 0x000000;
    if (*s == '#') s++;
    unsigned int v = 0;
    sscanf(s, "%6x", &v);
    return (uint32_t)(v & 0xFFFFFF);
}

static char *jv_str(JSCValue *o, const char *key) {
    JSCValue *p = jsc_value_object_get_property(o, key);
    char *r = NULL;
    if (p && !jsc_value_is_null(p) && !jsc_value_is_undefined(p))
        r = jsc_value_to_string(p);
    if (p) g_object_unref(p);
    return r; /* g_free by caller */
}
static double jv_num(JSCValue *o, const char *key, double def) {
    JSCValue *p = jsc_value_object_get_property(o, key);
    double r = def;
    if (p && jsc_value_is_number(p)) r = jsc_value_to_double(p);
    if (p) g_object_unref(p);
    return r;
}
static int jv_bool(JSCValue *o, const char *key) {
    JSCValue *p = jsc_value_object_get_property(o, key);
    int r = 0;
    if (p && !jsc_value_is_null(p) && !jsc_value_is_undefined(p))
        r = jsc_value_to_boolean(p) ? 1 : 0;
    if (p) g_object_unref(p);
    return r;
}
static int jv_array_len(JSCValue *arr) {
    JSCValue *p = jsc_value_object_get_property(arr, "length");
    int r = 0;
    if (p && jsc_value_is_number(p)) r = jsc_value_to_int32(p);
    if (p) g_object_unref(p);
    return r;
}

/* base64 decode of a data URL payload; returns malloc'd bytes, sets *outlen */
static unsigned char *b64_decode(const char *in, size_t *outlen) {
    static int tbl[256];
    static int init = 0;
    if (!init) {
        for (int i = 0; i < 256; i++) tbl[i] = -1;
        const char *a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; i++) tbl[(unsigned char)a[i]] = i;
        init = 1;
    }
    size_t len = strlen(in);
    unsigned char *out = malloc(len / 4 * 3 + 4);
    size_t o = 0;
    int val = 0, bits = 0;
    for (size_t i = 0; i < len; i++) {
        int c = tbl[(unsigned char)in[i]];
        if (c < 0) continue; /* skip whitespace/= */
        val = (val << 6) | c;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (unsigned char)((val >> bits) & 0xFF);
        }
    }
    *outlen = o;
    return out;
}

/* From data URL "data:image/png;base64,....": pptx_add_image with decoded bytes */
static void add_image_from_dataurl(PptxPresentation *pres, int slide_idx,
                                   const char *url, float x, float y, float w, float h) {
    if (!url || strncmp(url, "data:", 5) != 0) return;
    const char *comma = strchr(url, ',');
    if (!comma) return;
    char ext[8] = "png";
    if (strstr(url, "image/jpeg") || strstr(url, "image/jpg")) strcpy(ext, "jpeg");
    else if (strstr(url, "image/png")) strcpy(ext, "png");
    /* only base64 payloads supported */
    if (!strstr(url, "base64")) return;
    size_t outlen = 0;
    unsigned char *bytes = b64_decode(comma + 1, &outlen);
    if (bytes && outlen > 0)
        pptx_add_image(pres, slide_idx, bytes, outlen, ext, x, y, w, h);
    free(bytes);
}

/* ---------------- message handlers ---------------- */

static char *choose_file(SwipeApp *app, gboolean save, const char *pattern_name,
                         const char *pattern, const char *default_name) {
    GtkFileChooserAction action = save ? GTK_FILE_CHOOSER_ACTION_SAVE : GTK_FILE_CHOOSER_ACTION_OPEN;
    GtkWidget *d = gtk_file_chooser_dialog_new(
        save ? "Save" : "Open", app->window, action,
        "_Cancel", GTK_RESPONSE_CANCEL,
        save ? "_Save" : "_Open", GTK_RESPONSE_ACCEPT, NULL);
    GtkFileChooser *ch = GTK_FILE_CHOOSER(d);
    if (save) {
        gtk_file_chooser_set_do_overwrite_confirmation(ch, TRUE);
        if (default_name) gtk_file_chooser_set_current_name(ch, default_name);
    }
    GtkFileFilter *filt = gtk_file_filter_new();
    gtk_file_filter_set_name(filt, pattern_name);
    gtk_file_filter_add_pattern(filt, pattern);
    gtk_file_chooser_add_filter(ch, filt);
    char *path = NULL;
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT)
        path = gtk_file_chooser_get_filename(ch);
    gtk_widget_destroy(d);
    return path; /* g_free */
}

static void on_save(WebKitUserContentManager *manager,
                    WebKitJavascriptResult *js_result, gpointer user_data) {
    (void)manager;
    SwipeApp *app = user_data;
    JSCValue *value = webkit_javascript_result_get_js_value(js_result);
    if (!jsc_value_is_string(value)) return;
    char *json = jsc_value_to_string(value);
    char *path = choose_file(app, TRUE, "Swipe deck (*.json)", "*.json", "presentation.json");
    if (path) {
        FILE *f = fopen(path, "w");
        if (f) { fputs(json, f); fclose(f); g_print("Saved to %s\n", path); }
        else g_printerr("Could not write %s\n", path);
        g_strlcpy(app->current_file, path, MAX_PATH);
        swipe_update_title(app);
        g_free(path);
    }
    g_free(json);
}

static void on_load(WebKitUserContentManager *manager,
                    WebKitJavascriptResult *js_result, gpointer user_data) {
    (void)manager; (void)js_result;
    SwipeApp *app = user_data;
    char *path = choose_file(app, FALSE, "Swipe deck (*.json)", "*.json", NULL);
    if (!path) return;
    gchar *contents = NULL; gsize len = 0;
    if (g_file_get_contents(path, &contents, &len, NULL)) {
        /* file is a JSON object literal -> inject as loadDeck(<json>) */
        gchar *script = g_strdup_printf("loadDeck(%s);", contents);
        webkit_web_view_evaluate_javascript(app->web_view, script, -1,
                                             NULL, NULL, NULL, NULL, NULL);
        g_free(script);
        g_free(contents);
        g_strlcpy(app->current_file, path, MAX_PATH);
        swipe_update_title(app);
        g_print("Loaded %s\n", path);
    }
    g_free(path);
}

static void on_export(WebKitUserContentManager *manager,
                      WebKitJavascriptResult *js_result, gpointer user_data) {
    (void)manager;
    SwipeApp *app = user_data;
    JSCValue *deck = webkit_javascript_result_get_js_value(js_result);
    if (!deck || !jsc_value_is_object(deck)) { g_printerr("Export: bad deck\n"); return; }

    JSCValue *slides = jsc_value_object_get_property(deck, "slides");
    if (!slides || !jsc_value_is_array(slides)) {
        if (slides) g_object_unref(slides);
        g_printerr("Export: no slides\n");
        return;
    }

    char *path = choose_file(app, TRUE, "PowerPoint (*.pptx)", "*.pptx", "presentation.pptx");
    if (!path) { g_object_unref(slides); return; }

    PptxPresentation *pres = pptx_create("Swipe Presentation");
    int nslides = jv_array_len(slides);
    for (int i = 0; i < nslides; i++) {
        JSCValue *sl = jsc_value_object_get_property_at_index(slides, i);
        if (!sl || !jsc_value_is_object(sl)) { if (sl) g_object_unref(sl); continue; }
        char *bg = jv_str(sl, "bg");
        pptx_add_slide(pres, parse_hex_color(bg ? bg : "#ffffff"));
        g_free(bg);

        JSCValue *objs = jsc_value_object_get_property(sl, "objs");
        if (objs && jsc_value_is_array(objs)) {
            int no = jv_array_len(objs);
            for (int j = 0; j < no; j++) {
                JSCValue *o = jsc_value_object_get_property_at_index(objs, j);
                if (!o || !jsc_value_is_object(o)) { if (o) g_object_unref(o); continue; }
                char *type = jv_str(o, "type");
                float x = (float)jv_num(o, "x", 0), y = (float)jv_num(o, "y", 0);
                float w = (float)jv_num(o, "w", 100), h = (float)jv_num(o, "h", 60);
                if (type && strcmp(type, "text") == 0) {
                    char *txt = jv_str(o, "text");
                    char *col = jv_str(o, "color");
                    pptx_add_text(pres, i, txt ? txt : "", x, y, w, h,
                                  (int)jv_num(o, "size", 24), parse_hex_color(col ? col : "#000000"),
                                  jv_bool(o, "bold"), jv_bool(o, "italic"),
                                  jv_bool(o, "bullet"), (int)jv_num(o, "align", 0));
                    g_free(txt); g_free(col);
                } else if (type && strcmp(type, "image") == 0) {
                    char *src = jv_str(o, "src");
                    if (src) add_image_from_dataurl(pres, i, src, x, y, w, h);
                    g_free(src);
                } else if (type && (strcmp(type, "rect") == 0 || strcmp(type, "ellipse") == 0 || strcmp(type, "line") == 0)) {
                    char *fill = jv_str(o, "fill");
                    char *stroke = jv_str(o, "stroke");
                    PptxShapeType k = (strcmp(type, "ellipse") == 0) ? SHAPE_ELLIPSE
                                    : (strcmp(type, "line") == 0)    ? SHAPE_LINE : SHAPE_RECT;
                    int filled = (k != SHAPE_LINE);
                    pptx_add_shape(pres, i, k, x, y, w, h,
                                   parse_hex_color(fill ? fill : "#4472c4"),
                                   parse_hex_color(stroke ? stroke : "#000000"), filled);
                    g_free(fill); g_free(stroke);
                }
                g_free(type);
                g_object_unref(o);
            }
        }
        if (objs) g_object_unref(objs);
        g_object_unref(sl);
    }
    g_object_unref(slides);

    int ok = pptx_save(pres, path);
    pptx_free(pres);
    if (ok) g_print("Exported PPTX to %s\n", path);
    else g_printerr("Export failed: %s\n", path);
    g_free(path);
}

static void swipe_on_print_finished(WebKitPrintOperation *op, gpointer d) {
    (void)d; g_object_unref(op);
}

/* Print-to-PDF the hidden #printarea (one .psheet per slide) built by JS. */
static void on_pdf(WebKitUserContentManager *manager,
                   WebKitJavascriptResult *js_result, gpointer user_data) {
    (void)manager; (void)js_result;
    SwipeApp *app = user_data;
    char *path = choose_file(app, TRUE, "PDF (*.pdf)", "*.pdf", "presentation.pdf");
    if (!path) return;
    WebKitPrintOperation *op = webkit_print_operation_new(app->web_view);
    GtkPrintSettings *st = gtk_print_settings_new();
    char *uri = g_strdup_printf("file://%s", path);
    gtk_print_settings_set(st, GTK_PRINT_SETTINGS_OUTPUT_URI, uri);
    gtk_print_settings_set(st, "output-file-format", "pdf");
    gtk_print_settings_set_orientation(st, GTK_PAGE_ORIENTATION_LANDSCAPE);
    webkit_print_operation_set_print_settings(op, st);
    g_signal_connect(op, "finished", G_CALLBACK(swipe_on_print_finished), NULL);
    webkit_print_operation_print(op);
    g_free(uri); g_object_unref(st); g_free(path);
    g_print("Exported PDF\n");
}

/* Inject a saved .json deck (a JSON object literal) via loadDeck(<json>). */
static void inject_deck_file(SwipeApp *app, const char *path) {
    gchar *contents = NULL; gsize len = 0;
    if (g_file_get_contents(path, &contents, &len, NULL)) {
        gchar *script = g_strdup_printf("loadDeck(%s);", contents);
        webkit_web_view_evaluate_javascript(app->web_view, script, -1,
                                             NULL, NULL, NULL, NULL, NULL);
        g_free(script); g_free(contents);
        g_strlcpy(app->current_file, path, MAX_PATH);
        swipe_update_title(app);
        g_print("Loaded %s\n", path);
    }
}

static void on_load_changed(WebKitWebView *wv, WebKitLoadEvent ev,
                            gpointer user_data) {
    (void)wv;
    SwipeApp *app = user_data;
    if (ev == WEBKIT_LOAD_FINISHED && !app->loaded) {
        app->loaded = TRUE;
        if (app->startup_file[0]) inject_deck_file(app, app->startup_file);
    }
}

/* ---------------- UI ---------------- */

static void setup_ui(SwipeApp *app) {
    app->window = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
    gtk_window_set_title(app->window, "Swipe - Presentation Editor");
    gtk_window_set_default_size(app->window, 1400, 900);
    g_signal_connect(app->window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkBox *vbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
    gtk_container_add(GTK_CONTAINER(app->window), GTK_WIDGET(vbox));

    app->web_view = WEBKIT_WEB_VIEW(webkit_web_view_new());
    gtk_box_pack_start(vbox, GTK_WIDGET(app->web_view), TRUE, TRUE, 0);

    WebKitSettings *settings = webkit_settings_new();
    webkit_settings_set_enable_write_console_messages_to_stdout(settings, TRUE);
    webkit_web_view_set_settings(app->web_view, settings);

    WebKitUserContentManager *manager = webkit_web_view_get_user_content_manager(app->web_view);
    webkit_user_content_manager_register_script_message_handler(manager, "save");
    webkit_user_content_manager_register_script_message_handler(manager, "load");
    webkit_user_content_manager_register_script_message_handler(manager, "exportpptx");
    webkit_user_content_manager_register_script_message_handler(manager, "pdf");

    g_signal_connect(manager, "script-message-received::save", G_CALLBACK(on_save), app);
    g_signal_connect(manager, "script-message-received::load", G_CALLBACK(on_load), app);
    g_signal_connect(manager, "script-message-received::exportpptx", G_CALLBACK(on_export), app);
    g_signal_connect(manager, "script-message-received::pdf", G_CALLBACK(on_pdf), app);

    g_signal_connect(app->web_view, "load-changed", G_CALLBACK(on_load_changed), app);

    webkit_web_view_load_html(app->web_view, HTML_TEMPLATE, NULL);
    gtk_widget_show_all(GTK_WIDGET(app->window));
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);
    SwipeApp app = {0};
    if (argc > 1) g_strlcpy(app.startup_file, argv[1], MAX_PATH);
    setup_ui(&app);
    gtk_main();
    return 0;
}
