/*
 * web_ui.c — KAOS Portal HTTP server
 *
 * Endpoints:
 *   GET  /                  Full web app (single page)
 *   GET  /api/state         JSON: file list, slot states, portal type
 *   POST /api/load          {slot, file} — load a file into a slot
 *   POST /api/unload        {slot} — unload a slot
 *   POST /api/sense         Re-announce portal to game
 *   POST /api/portaltype    {type} — switch portal USB mode
 *   POST /api/upload        multipart/form-data file upload to SPIFFS
 *   GET  /api/download?slot=N  Download current slot data as .bin
 */

#include "web_ui.h"
#include "Skylander.h"
#include "SkylanderCrypt.h"
#include "skylander_ids.h"
#include "pico_bridge.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <dirent.h>

static const char *TAG = "WebUI";

#define SPIFFS_MOUNT "/spiffs"

extern SemaphoreHandle_t g_sky_mutex;
extern int  g_file_count;
extern char g_file_list[64][64];
extern void spiffs_full_path(const char *basename, char *out, size_t out_len);
extern void scan_files(void);

typedef enum {
    PORTAL_TYPE_SSA         = 0,
    PORTAL_TYPE_SWAP        = 1,
    PORTAL_TYPE_TRAP        = 2,
    PORTAL_TYPE_IMAGINATORS = 3
} portal_type_t;

static portal_type_t g_portal_type = PORTAL_TYPE_IMAGINATORS;

/* Load saved portal type from NVS — called once at server start */
void web_ui_load_portal_type(void) {
    nvs_handle_t nvs;
    if (nvs_open("kaos", NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t v = 3;
        if (nvs_get_u8(nvs, "portal_type", &v) == ESP_OK && v <= 3)
            g_portal_type = (portal_type_t)v;
        nvs_close(nvs);
    }
    ESP_LOGI("WebUI", "Portal type loaded from NVS: %d", (int)g_portal_type);
}

/* -----------------------------------------------------------------------
 * HTML page — complete rewrite with clean JS architecture
 * State machine approach: fetch state → diff → update only what changed
 * No innerHTML rebuilding of interactive elements
 * ----------------------------------------------------------------------- */
static const char HTML_PAGE[] =
"<!DOCTYPE html><html lang='en'><head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>KAOS Portal</title>"
"<style>"
"@import url('https://fonts.googleapis.com/css2?family=Orbitron:wght@700&family=Exo+2:wght@400;600&display=swap');"
":root{"
  "--bg:#060612;--surface:#0e0e24;--card:#13132e;"
  "--border:#1e1e45;--accent:#6d28d9;--glow:#7c3aed;"
  "--accent2:#a855f7;--hot:#f43f5e;--ok:#10b981;"
  "--text:#e2e8f0;--muted:#475569;--bright:#f8fafc;"
"}"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{background:var(--bg);color:var(--text);"
  "font-family:'Exo 2',system-ui,sans-serif;"
  "min-height:100vh;padding:20px 16px;"
  "background-image:radial-gradient(ellipse at 20% 0%,#1a0a3e 0%,transparent 60%),"
    "radial-gradient(ellipse at 80% 100%,#0a1a3e 0%,transparent 60%)}"

/* Header */
"header{text-align:center;margin-bottom:24px}"
"h1{font-family:'Orbitron',monospace;font-size:2rem;letter-spacing:.1em;"
  "background:linear-gradient(135deg,#a78bfa,#7c3aed,#4f46e5);"
  "-webkit-background-clip:text;-webkit-text-fill-color:transparent;background-clip:text;"
  "margin-bottom:4px}"
".sub{color:var(--muted);font-size:.8rem;letter-spacing:.05em}"

/* Container */
".wrap{max-width:680px;margin:0 auto}"

/* Portal type bar */
".pbar{background:var(--card);border:1px solid var(--border);"
  "border-radius:14px;padding:14px 16px;margin-bottom:16px;"
  "display:flex;align-items:center;gap:12px}"
".pbar-label{font-size:.75rem;color:var(--muted);white-space:nowrap;font-weight:600;letter-spacing:.05em;text-transform:uppercase}"
"#ptype{flex:1;background:#0a0a20;border:1px solid var(--border);"
  "border-radius:8px;color:var(--text);padding:8px 12px;"
  "font-size:.87rem;font-family:'Exo 2',sans-serif;cursor:pointer;outline:none}"
"#ptype:focus{border-color:var(--accent)}"
".pbadge{background:var(--accent);color:#fff;font-size:.7rem;"
  "padding:4px 10px;border-radius:20px;font-weight:600;white-space:nowrap}"

/* Slot grid */
".slots{display:grid;grid-template-columns:1fr 1fr;gap:14px;margin-bottom:16px}"
"@media(max-width:500px){.slots{grid-template-columns:1fr}}"
".card{background:var(--card);border:1px solid var(--border);"
  "border-radius:14px;padding:16px;display:flex;flex-direction:column;gap:10px;"
  "transition:border-color .3s}"
".card.loaded{border-color:var(--accent);"
  "box-shadow:0 0 20px rgba(109,40,217,.15)}"
".card-hdr{display:flex;justify-content:space-between;align-items:center}"
".lbl{font-size:.7rem;color:var(--muted);font-weight:600;letter-spacing:.1em;text-transform:uppercase}"
".pnum{font-family:'Orbitron',monospace;font-size:.8rem;color:var(--accent2)}"

/* Character display */
".char-info{text-align:center;padding:8px 0}"
".char-name{font-weight:600;font-size:.95rem;color:var(--bright)}"
".char-elem{font-size:.72rem;color:var(--muted);margin-top:2px;text-transform:uppercase;letter-spacing:.05em}"
".char-file{font-size:.65rem;color:var(--muted);margin-top:4px;opacity:.6}"
".empty-slot{text-align:center;padding:12px 0;color:var(--muted);"
  "font-size:.8rem;display:flex;flex-direction:column;align-items:center;gap:6px}"
".empty-slot svg{opacity:.3}"

/* Element colors */
".el-Fire{color:#f97316}.el-Water{color:#38bdf8}.el-Earth{color:#a3e635}"
".el-Air{color:#67e8f9}.el-Life{color:#4ade80}.el-Undead{color:#c084fc}"
".el-Magic{color:#f472b6}.el-Tech{color:#fbbf24}.el-Light{color:#fef08a}"
".el-Dark{color:#94a3b8}.el-Kaos{color:#f43f5e}"

/* File selector */
"select.file-sel{width:100%;background:#0a0a20;border:1px solid var(--border);"
  "border-radius:8px;color:var(--text);padding:8px 10px;"
  "font-size:.83rem;font-family:'Exo 2',sans-serif;cursor:pointer;outline:none}"
"select.file-sel:focus{border-color:var(--accent)}"

/* Buttons */
".btn{width:100%;padding:9px;border-radius:8px;font-size:.83rem;"
  "font-family:'Exo 2',sans-serif;font-weight:600;cursor:pointer;"
  "border:none;transition:opacity .2s,transform .1s}"
".btn:active{transform:scale(.98)}"
".btn:disabled{opacity:.4;cursor:not-allowed}"
".btn-load{background:var(--accent);color:#fff}"
".btn-load:hover:not(:disabled){background:var(--glow)}"
".btn-unload{background:transparent;border:1px solid var(--border);color:var(--muted)}"
".btn-unload:hover{border-color:var(--hot);color:var(--hot)}"
".btn-dl{background:transparent;border:1px solid var(--border);color:var(--muted)}"
".btn-dl:hover{border-color:var(--accent2);color:var(--accent2)}"
".btn-del{background:transparent;border:1px solid #374151;color:#6b7280;"
  "font-size:.75rem;padding:6px}"
".btn-del:hover{border-color:var(--hot);color:var(--hot)}"

/* Upload box */
".upbox{background:var(--card);border:1px dashed var(--border);"
  "border-radius:14px;padding:20px;margin-bottom:14px;text-align:center}"
".upbox p{color:var(--muted);font-size:.82rem;margin-bottom:12px}"
".upbtn{display:inline-block;background:var(--accent);color:#fff;"
  "padding:9px 20px;border-radius:8px;font-size:.85rem;font-weight:600;"
  "cursor:pointer;transition:background .2s}"
".upbtn:hover{background:var(--glow)}"
"#upfile{display:none}"
"#upstat{margin-top:10px;font-size:.8rem;color:var(--ok);min-height:1em}"

/* Sense + Status */
".sense-btn{width:100%;max-width:680px;display:block;margin:0 auto 14px;"
  "background:transparent;border:1px solid var(--border);color:var(--muted);"
  "padding:11px;border-radius:10px;font-size:.83rem;font-family:'Exo 2',sans-serif;"
  "font-weight:600;cursor:pointer;transition:all .2s;letter-spacing:.05em}"
".sense-btn:hover{border-color:var(--accent2);color:var(--accent2)}"
".status{max-width:680px;margin:0 auto;display:flex;align-items:center;"
  "gap:8px;padding:10px 14px;background:var(--card);"
  "border:1px solid var(--border);border-radius:10px;font-size:.8rem}"
".dot{width:8px;height:8px;border-radius:50%;background:var(--muted);flex-shrink:0}"
".dot.ok{background:var(--ok);box-shadow:0 0 6px var(--ok)}"
".dot.err{background:var(--hot);box-shadow:0 0 6px var(--hot)}"
"#stxt{color:var(--muted)}"
"</style></head><body>"
"<div class='wrap'>"
"<header>"
  "<h1>◆ KAOS Portal</h1>"
  "<p class='sub'>Skylander Portal Manager</p>"
"</header>"

/* Portal type */
"<div class='pbar'>"
  "<span class='pbar-label'>Portal type</span>"
  "<select id='ptype' onchange='setPortalType(this.value)'>"
    "<option value='3'>Imaginators / SuperChargers</option>"
    "<option value='1'>Giants / Swap Force</option>"
    "<option value='0'>Spyro's Adventure</option>"
    "<option value='2'>Trap Team (Traptanium)</option>"
  "</select>"
  "<span class='pbadge' id='pbadge'>Imaginators</span>"
"</div>"

/* Slot cards — static structure, JS only updates inner content divs */
"<div class='slots'>"
  "<div class='card' id='card0'>"
    "<div class='card-hdr'>"
      "<span class='lbl'>Player 1</span><span class='pnum'>P1</span>"
    "</div>"
    "<div id='info0'></div>"
    "<select class='file-sel' id='sel0'></select>"
    "<button class='btn btn-load' id='btnLoad0' onclick='doLoad(0)'>Load</button>"
    "<div id='extra0'></div>"
  "</div>"
  "<div class='card' id='card1'>"
    "<div class='card-hdr'>"
      "<span class='lbl'>Player 2</span><span class='pnum'>P2</span>"
    "</div>"
    "<div id='info1'></div>"
    "<select class='file-sel' id='sel1'></select>"
    "<button class='btn btn-load' id='btnLoad1' onclick='doLoad(1)'>Load</button>"
    "<div id='extra1'></div>"
  "</div>"
"</div>"

/* Upload */
"<div class='upbox'>"
  "<p>Upload a Skylander dump (.bin / .dmp / .sky)</p>"
  "<label><input type='file' id='upfile' accept='.bin,.dmp,.sky,.dump' onchange='doUpload(this)'>"
    "<span class='upbtn'>&#8679; Choose file</span>"
  "</label>"
  "<div id='upstat'></div>"
"</div>"

/* Sense */
"<button class='sense-btn' onclick='doSense()'>&#8635; Force Sense</button>"

/* Status */
"<div class='status'><div class='dot' id='dot'></div><span id='stxt'>Connecting...</span></div>"
"</div>"

"<script>"
/* ── Constants ─────────────────────────────────────────── */
"const EL={"
  "Fire:'🔥',Water:'💧',Earth:'🌿',Air:'💨',"
  "Life:'🌱',Undead:'💀',Magic:'✨',Tech:'⚙️',"
  "Light:'☀️',Dark:'🌑',Kaos:'🔮'"
"};"
"const PT={0:\"Spyro's Adv\",1:'Giants/SwapForce',2:'Trap Team',3:'Imaginators'};"

/* ── State ─────────────────────────────────────────────── */
/* Single source of truth — never read from DOM to make decisions */
"let files=[];"          /* string[] — current file list from server */
"let slots=[{},{} ];"    /* slot state objects from server */
"let portalType=3;"
"let ptypeChanging=false;"

/* ── Render ────────────────────────────────────────────── */
/* Called after every state fetch. Updates DOM to match state.
 * Selects are ONLY populated once (when files change).
 * Never destroys interactive elements — only updates their properties. */
"let lastFileKey='';"

"function renderFiles(){"
  "const key=files.join('|');"
  "if(key===lastFileKey)return;"  /* files unchanged — leave selects alone */
  "lastFileKey=key;"
  "for(let i=0;i<2;i++){"
    "const sel=document.getElementById('sel'+i);"
    "if(!sel)continue;"
    "const cur=sel.value;"         /* preserve current selection */
    "sel.innerHTML=files.map(f=>'<option>'+f+'</option>').join('');"
    /* restore selection if file still exists */
    "if(files.includes(cur))sel.value=cur;"
  "}"
"}"

"function renderSlot(i){"
  "const s=slots[i]||{};"
  "const card=document.getElementById('card'+i);"
  "const info=document.getElementById('info'+i);"
  "const btnLoad=document.getElementById('btnLoad'+i);"
  "const extra=document.getElementById('extra'+i);"
  "if(!card||!info||!btnLoad||!extra)return;"

  "card.classList.toggle('loaded',!!s.loaded);"

  /* Info panel */
  "if(s.loaded){"
    "const e=s.element||'Magic';"
    "info.innerHTML="
      "'<div class=\"char-info\">'+"
      "'<div class=\"char-name\">'+(s.name||'Unknown')+'</div>'+"
      "'<div class=\"char-elem el-'+e+'\">'+(EL[e]||'')+'  '+e+'</div>'+"
      "'<div class=\"char-file\">'+(s.filename||'')+'</div>'+"
      "'</div>';"
  "}else{"
    "info.innerHTML="
      "'<div class=\"empty-slot\">'+"
      "'<svg width=32 height=32 viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=1.5>'+"
      "'<circle cx=12 cy=12 r=10/><path d=\"M8 13s1.5 2 4 2 4-2 4-2\"/><line x1=9 y1=9 x2=9.01 y2=9/><line x1=15 y1=9 x2=15.01 y2=9/>'+"
      "'</svg>No Skylander</div>';"
  "}"

  /* Load button */
  "btnLoad.disabled=(files.length===0);"

  /* Extra buttons (download + unload) — only shown when loaded */
  "if(s.loaded){"
    "extra.innerHTML="
      "'<button class=\"btn btn-dl\" onclick=\"doDl('+i+')\">&#8681; Download save</button>'+"
      "'<button class=\"btn btn-unload\" onclick=\"doUnload('+i+')\">Unload</button>';"
  "}else{"
    "extra.innerHTML="
      "'<button class=\"btn btn-del\" onclick=\"doDelSel('+i+')\">&#128465; Delete selected</button>';"
  "}"
"}"

"function renderPortalType(){"
  "if(ptypeChanging)return;"
  "const el=document.getElementById('ptype');"
  "if(el)el.value=portalType;"
  "const badge=document.getElementById('pbadge');"
  "if(badge)badge.textContent=PT[portalType]||'';"
"}"

/* ── Fetch ─────────────────────────────────────────────── */
"async function poll(){"
  "try{"
    "const r=await fetch('/api/state');"
    "if(!r.ok)throw new Error('bad');"
    "const d=await r.json();"
    "files=d.files||[];"
    "slots=d.slots||[{},{}];"
    "portalType=d.portal_type||3;"
    "renderFiles();"
    "renderSlot(0);"
    "renderSlot(1);"
    "renderPortalType();"
    "st('Connected',1);"
  "}catch(e){st('No connection',0)}"
"}"

/* ── Actions ───────────────────────────────────────────── */
"async function doLoad(i){"
  "const sel=document.getElementById('sel'+i);"
  "const file=sel?sel.value:'';"
  "if(!file){st('No file selected',0);return;}"
  "st('Loading...',1);"
  "try{"
    "const r=await fetch('/api/load',{method:'POST',"
      "headers:{'Content-Type':'application/json'},"
      "body:JSON.stringify({slot:i,file})});"
    "const d=await r.json();"
    "if(d.ok){await poll();st('Loaded: '+file,1);}else st('Failed: '+(d.error||'?'),0);"
  "}catch(e){st('Error',0)}"
"}"

"async function doUnload(i){"
  "st('Unloading...',1);"
  "try{"
    "await fetch('/api/unload',{method:'POST',"
      "headers:{'Content-Type':'application/json'},body:JSON.stringify({slot:i})});"
    "await poll();st('Unloaded',1);"
  "}catch(e){st('Error',0)}"
"}"

"async function doDl(i){"
  "window.location='/api/download?slot='+i;"
"}"

"async function doDelSel(i){"
  "const sel=document.getElementById('sel'+i);"
  "const file=sel?sel.value:'';"
  "if(!file||!confirm('Delete '+file+'?'))return;"
  "st('Deleting...',1);"
  "try{"
    "const r=await fetch('/api/delete',{method:'POST',"
      "headers:{'Content-Type':'application/json'},body:JSON.stringify({file})});"
    "const d=await r.json();"
    "if(d.ok){lastFileKey='';await poll();st('Deleted',1);}else st('Delete failed',0);"
  "}catch(e){st('Error',0)}"
"}"

"async function doUpload(inp){"
  "const f=inp.files[0];if(!f)return;"
  "const stat=document.getElementById('upstat');"
  "stat.style.color='#94a3b8';stat.textContent='Uploading...';"
  "const fd=new FormData();fd.append('file',f);"
  "try{"
    "const r=await fetch('/api/upload',{method:'POST',body:fd});"
    "const d=await r.json();"
    "if(d.ok){"
      "stat.style.color='var(--ok)';stat.textContent='✓ '+f.name+' saved';"
      "lastFileKey='';await poll();"
    "}else{"
      "stat.style.color='var(--hot)';stat.textContent='✗ '+(d.error||'Upload failed');"
    "}"
  "}catch(e){stat.style.color='var(--hot)';stat.textContent='✗ Error';}"
  "inp.value='';"
"}"

"async function doSense(){"
  "try{"
    "await fetch('/api/sense',{method:'POST'});"
    "st('Sense triggered',1);"
  "}catch(e){st('Error',0)}"
"}"

"async function setPortalType(v){"
  "ptypeChanging=true;"
  "const badge=document.getElementById('pbadge');"
  "if(badge)badge.textContent=PT[parseInt(v)]||'';"
  "try{"
    "await fetch('/api/portaltype',{method:'POST',"
      "headers:{'Content-Type':'application/json'},body:JSON.stringify({type:parseInt(v)})});"
    "st('Portal type saved — unplug & replug Pico',1);"
  "}catch(e){st('Error',0)}"
  "setTimeout(()=>ptypeChanging=false,5000);"
"}"

"function st(m,ok){"
  "document.getElementById('stxt').textContent=m;"
  "document.getElementById('dot').className='dot '+(ok?'ok':'err');"
"}"

/* ── Boot ──────────────────────────────────────────────── */
"poll();setInterval(poll,5000);"
"</script></body></html>";

/* -----------------------------------------------------------------------
 * JSON helpers
 * ----------------------------------------------------------------------- */
static int json_str(char *buf, int max, const char *s) {
    int n = 0;
    buf[n++] = '"';
    while (*s && n < max-2) {
        if (*s == '"' || *s == '\\') buf[n++] = '\\';
        buf[n++] = *s++;
    }
    buf[n++] = '"';
    buf[n]   = '\0';
    return n;
}

/* -----------------------------------------------------------------------
 * GET /
 * ----------------------------------------------------------------------- */
static esp_err_t handle_root(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, HTML_PAGE, strlen(HTML_PAGE));
    return ESP_OK;
}

/* -----------------------------------------------------------------------
 * GET /api/state
 * ----------------------------------------------------------------------- */
static esp_err_t handle_state(httpd_req_t *req) {
    char buf[4096];  /* Increased from 2048 to handle many files */
    int n = 0;

    n += snprintf(buf+n, sizeof(buf)-n, "{\"files\":[");
    xSemaphoreTake(g_sky_mutex, portMAX_DELAY);

    for (int i = 0; i < g_file_count && n < (int)sizeof(buf)-64; i++) {
        if (i) buf[n++] = ',';
        n += json_str(buf+n, sizeof(buf)-n-4, g_file_list[i]);
    }

    n += snprintf(buf+n, sizeof(buf)-n, "],\"slots\":[");

    for (int s = 0; s < 2; s++) {
        skylander_slot_t *sky = &g_skylanders[s];
        if (s) buf[n++] = ',';
        if (sky->loaded) {
            uint16_t char_id = skylander_read_char_id(sky->data);
            const char *name = skylander_name_from_id(char_id);
            const char *elem = skylander_element_from_id(char_id);
            /* filename stored as full path — extract basename */
            const char *base = strrchr(sky->filename, '/');
            base = base ? base+1 : sky->filename;
            char nb[64], fb[80];
            json_str(nb, sizeof(nb), name ? name : "Unknown");
            json_str(fb, sizeof(fb), base);
            n += snprintf(buf+n, sizeof(buf)-n,
                "{\"loaded\":true,\"name\":%s,\"element\":\"%s\","
                "\"filename\":%s,\"char_id\":%u}",
                nb, elem ? elem : "Magic", fb, char_id);
        } else {
            n += snprintf(buf+n, sizeof(buf)-n, "{\"loaded\":false}");
        }
    }

    n += snprintf(buf+n, sizeof(buf)-n, "],\"portal_type\":%d}", (int)g_portal_type);
    xSemaphoreGive(g_sky_mutex);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

/* -----------------------------------------------------------------------
 * POST /api/load
 * ----------------------------------------------------------------------- */
static esp_err_t handle_load(httpd_req_t *req) {
    char body[128] = {0};
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(body)) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Bad body\"}");
        return ESP_OK;
    }
    httpd_req_recv(req, body, len);

    int slot = -1;
    char file[64] = {0};
    char *ps = strstr(body, "\"slot\"");
    if (ps) slot = atoi(ps + 7);
    char *pf = strstr(body, "\"file\"");
    if (pf) {
        pf = strchr(pf+6, '"');
        if (pf) { pf++; int fi=0; while(*pf&&*pf!='"'&&fi<63) file[fi++]=*pf++; }
    }

    if (slot < 0 || slot > 1 || !file[0]) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Invalid params\"}");
        return ESP_OK;
    }

    char full[96];
    spiffs_full_path(file, full, sizeof(full));
    ESP_LOGI(TAG, "Load slot %d from %s", slot, full);

    /* Read raw bytes for Pico, then load decrypted for ESP32 metadata */
    static uint8_t raw[SKYLANDER_DUMP_SIZE];
    bool got_raw = false;
    FILE *rf = fopen(full, "rb");
    if (rf) {
        got_raw = (fread(raw, 1, SKYLANDER_DUMP_SIZE, rf) == SKYLANDER_DUMP_SIZE);
        fclose(rf);
    }

    xSemaphoreTake(g_sky_mutex, portMAX_DELAY);
    /* If slot is already loaded, unload it first so the game sees a clean
     * removal before the new figure arrives — prevents figure index confusion */
    if (g_skylanders[slot].loaded) {
        pico_bridge_unload((uint8_t)slot);
        vTaskDelay(pdMS_TO_TICKS(200)); /* give game time to process removal */
    }
    bool ok = skylander_load((uint8_t)slot, full);
    if (ok && got_raw) {
        ESP_LOGI(TAG, "Pushing slot %d to Pico", slot);
        pico_bridge_load((uint8_t)slot, raw);
    }
    xSemaphoreGive(g_sky_mutex);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"Load failed\"}");
    return ESP_OK;
}

/* -----------------------------------------------------------------------
 * POST /api/unload
 * ----------------------------------------------------------------------- */
static esp_err_t handle_unload(httpd_req_t *req) {
    char body[64] = {0};
    int len = req->content_len;
    if (len > 0 && len < (int)sizeof(body)) httpd_req_recv(req, body, len);
    int slot = -1;
    char *ps = strstr(body, "\"slot\"");
    if (ps) slot = atoi(ps + 7);
    if (slot >= 0 && slot <= 1) {
        xSemaphoreTake(g_sky_mutex, portMAX_DELAY);
        skylander_unload((uint8_t)slot);
        xSemaphoreGive(g_sky_mutex);
        pico_bridge_unload((uint8_t)slot);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* -----------------------------------------------------------------------
 * POST /api/sense
 * ----------------------------------------------------------------------- */
static esp_err_t handle_sense(httpd_req_t *req) {
    ESP_LOGI(TAG, "Sense requested");
    xSemaphoreTake(g_sky_mutex, portMAX_DELAY);
    int loaded = 0;
    for (int s = 0; s < 2; s++) {
        if (!g_skylanders[s].loaded) continue;
        loaded++;
        static uint8_t raw[SKYLANDER_DUMP_SIZE];
        FILE *f = fopen(g_skylanders[s].filename, "rb");
        if (f) {
            size_t n = fread(raw, 1, SKYLANDER_DUMP_SIZE, f);
            fclose(f);
            ESP_LOGI(TAG, "  Slot %d: re-sending %u bytes", s, (unsigned)n);
            pico_bridge_unload((uint8_t)s);
            vTaskDelay(pdMS_TO_TICKS(80));
            pico_bridge_load((uint8_t)s, raw);
        } else {
            ESP_LOGE(TAG, "  Slot %d: cannot open %s", s, g_skylanders[s].filename);
        }
    }
    if (!loaded) {
        ESP_LOGW(TAG, "  No slots loaded — sending empty unloads");
        pico_bridge_unload(0);
        pico_bridge_unload(1);
    }
    xSemaphoreGive(g_sky_mutex);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* -----------------------------------------------------------------------
 * POST /api/portaltype
 * ----------------------------------------------------------------------- */
static esp_err_t handle_portaltype(httpd_req_t *req) {
    char body[64] = {0};
    int len = req->content_len;
    if (len > 0 && len < (int)sizeof(body)) httpd_req_recv(req, body, len);
    char *pt = strstr(body, "\"type\"");
    if (pt) {
        int t = atoi(pt + 7);
        if (t >= 0 && t <= 3) {
            g_portal_type = (portal_type_t)t;
            pico_bridge_set_portal_type((uint8_t)t);
            ESP_LOGI(TAG, "Portal type → %d", t);
        }
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* -----------------------------------------------------------------------
 * POST /api/upload  — multipart form upload → SPIFFS
 * ----------------------------------------------------------------------- */
static esp_err_t handle_upload(httpd_req_t *req) {
    /* Parse multipart boundary from Content-Type header */
    char ct[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct)) != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"No Content-Type\"}");
        return ESP_OK;
    }

    char *bnd_ptr = strstr(ct, "boundary=");
    if (!bnd_ptr) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"No boundary\"}");
        return ESP_OK;
    }
    char boundary[80];
    snprintf(boundary, sizeof(boundary), "--%s", bnd_ptr + 9);

    /* Read entire multipart body into a heap buffer */
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 16384) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Bad size\"}");
        return ESP_OK;
    }

    char *body = malloc(total_len + 1);
    if (!body) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"OOM\"}");
        return ESP_OK;
    }

    int received = 0;
    while (received < total_len) {
        int r = httpd_req_recv(req, body + received, total_len - received);
        if (r <= 0) break;
        received += r;
    }
    body[received] = '\0';

    /* Extract filename from Content-Disposition header inside multipart */
    char filename[64] = "upload.bin";
    char *cd = strstr(body, "Content-Disposition:");
    if (cd) {
        char *fn = strstr(cd, "filename=\"");
        if (fn) {
            fn += 10;
            int fi = 0;
            while (*fn && *fn != '"' && fi < 63) filename[fi++] = *fn++;
            filename[fi] = '\0';
        }
    }

    /* Find start of binary data (after \r\n\r\n following headers) */
    char *data_start = strstr(body, "\r\n\r\n");
    if (!data_start) {
        free(body);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Malformed multipart\"}");
        return ESP_OK;
    }
    data_start += 4;

    /* Find end boundary */
    char end_boundary[84];
    snprintf(end_boundary, sizeof(end_boundary), "\r\n%s--", boundary);
    char *data_end = strstr(data_start, end_boundary);
    if (!data_end) data_end = body + received;

    int data_len = (int)(data_end - data_start);
    ESP_LOGI(TAG, "Upload: '%s' %d bytes", filename, data_len);

    /* Skylander dumps must be exactly 1024 bytes.
     * Some formats (e.g. Flipper Zero .nfc) have headers — find the 1024-byte
     * block by scanning for the last 1024-byte aligned chunk, or just take
     * the last 1024 bytes if the file is larger. */
    char *dump_start = data_start;
    int   dump_len   = data_len;

    if (data_len > SKYLANDER_DUMP_SIZE) {
        /* Take the first 1024 bytes — headers are prepended in some formats,
         * but the actual MIFARE dump starts right at the beginning.
         * 1070 bytes = 1024 dump + 46 byte footer/padding, so just truncate. */
        dump_len = SKYLANDER_DUMP_SIZE;
        ESP_LOGW(TAG, "File is %d bytes, saving first %d bytes",
                 data_len, SKYLANDER_DUMP_SIZE);
    } else if (data_len < SKYLANDER_DUMP_SIZE) {
        ESP_LOGE(TAG, "File too small: %d bytes (need %d)", data_len, SKYLANDER_DUMP_SIZE);
        free(body);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"File too small — need 1024 bytes\"}");
        return ESP_OK;
    }

    /* Write to SPIFFS */
    char full[96];
    spiffs_full_path(filename, full, sizeof(full));
    FILE *f = fopen(full, "wb");
    bool ok = false;
    if (f) {
        ok = (fwrite(dump_start, 1, dump_len, f) == (size_t)dump_len);
        fclose(f);
        if (ok) {
            ESP_LOGI(TAG, "Saved to %s", full);
            /* Refresh file list */
            xSemaphoreTake(g_sky_mutex, portMAX_DELAY);
            scan_files();
            xSemaphoreGive(g_sky_mutex);
        } else {
            ESP_LOGE(TAG, "Write failed for %s", full);
        }
    } else {
        ESP_LOGE(TAG, "Cannot create %s", full);
    }

    free(body);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"Write failed\"}");
    return ESP_OK;
}

/* -----------------------------------------------------------------------
 * GET /api/download?slot=N
 * Returns the current slot data re-encrypted as a .bin download
 * ----------------------------------------------------------------------- */
static esp_err_t handle_download(httpd_req_t *req) {
    /* Parse slot from query string */
    char query[32] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    int slot = 0;
    char val[8] = {0};
    if (httpd_query_key_value(query, "slot", val, sizeof(val)) == ESP_OK)
        slot = atoi(val);

    if (slot < 0 || slot > 1) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad slot");
        return ESP_OK;
    }

    xSemaphoreTake(g_sky_mutex, portMAX_DELAY);

    if (!g_skylanders[slot].loaded) {
        xSemaphoreGive(g_sky_mutex);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Slot empty");
        return ESP_OK;
    }

    /* Re-encrypt the current in-memory decrypted data */
    static uint8_t enc[SKYLANDER_DUMP_SIZE];
    memcpy(enc, g_skylanders[slot].data, SKYLANDER_DUMP_SIZE);
    encrypt_skylander(enc, g_skylanders[slot].uid);

    /* Build a download filename */
    const char *base = strrchr(g_skylanders[slot].filename, '/');
    base = base ? base+1 : g_skylanders[slot].filename;
    char cdispo[320];
    snprintf(cdispo, sizeof(cdispo), "attachment; filename=\"%s\"", base);

    xSemaphoreGive(g_sky_mutex);

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", cdispo);
    httpd_resp_send(req, (const char *)enc, SKYLANDER_DUMP_SIZE);
    return ESP_OK;
}

/* -----------------------------------------------------------------------
 * POST /api/delete  — body: {"file":"Spyro.bin"}
 * ----------------------------------------------------------------------- */
static esp_err_t handle_delete(httpd_req_t *req) {
    char body[128] = {0};
    int len = req->content_len;
    if (len > 0 && len < (int)sizeof(body)) httpd_req_recv(req, body, len);

    char file[64] = {0};
    char *pf = strstr(body, "\"file\"");
    if (pf) {
        pf = strchr(pf+6, '"');
        if (pf) { pf++; int fi=0; while(*pf&&*pf!='"'&&fi<63) file[fi++]=*pf++; }
    }

    bool ok = false;
    if (file[0]) {
        char full[96];
        spiffs_full_path(file, full, sizeof(full));
        ok = (remove(full) == 0);
        if (ok) {
            ESP_LOGI(TAG, "Deleted %s", full);
            xSemaphoreTake(g_sky_mutex, portMAX_DELAY);
            scan_files();
            xSemaphoreGive(g_sky_mutex);
        } else {
            ESP_LOGE(TAG, "Failed to delete %s", full);
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"Delete failed\"}");
    return ESP_OK;
}
httpd_handle_t web_ui_start(void) {
    httpd_config_t config  = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;
    config.stack_size       = 8192;
    /* Increase recv buffer for file uploads */
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return NULL;
    }

    static const httpd_uri_t uris[] = {
        { .uri="/",               .method=HTTP_GET,  .handler=handle_root       },
        { .uri="/api/state",      .method=HTTP_GET,  .handler=handle_state      },
        { .uri="/api/download",   .method=HTTP_GET,  .handler=handle_download   },
        { .uri="/api/load",       .method=HTTP_POST, .handler=handle_load       },
        { .uri="/api/unload",     .method=HTTP_POST, .handler=handle_unload     },
        { .uri="/api/sense",      .method=HTTP_POST, .handler=handle_sense      },
        { .uri="/api/portaltype", .method=HTTP_POST, .handler=handle_portaltype },
        { .uri="/api/upload",     .method=HTTP_POST, .handler=handle_upload     },
        { .uri="/api/delete",     .method=HTTP_POST, .handler=handle_delete     },
    };
    for (int i = 0; i < 9; i++) httpd_register_uri_handler(server, &uris[i]);

    ESP_LOGI(TAG, "HTTP server ready");
    return server;
}

void web_ui_stop(httpd_handle_t server) {
    if (server) httpd_stop(server);
}
