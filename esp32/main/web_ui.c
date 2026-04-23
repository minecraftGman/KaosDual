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
 * HTML page
 * ----------------------------------------------------------------------- */
static const char HTML_PAGE[] =
"<!DOCTYPE html><html lang='en'><head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>KAOS Portal</title>"
"<style>"
":root{"
  "--bg:#0a0a1a;--card:#12122a;--border:#2a2a5a;"
  "--accent:#7c3aed;--accent2:#a855f7;"
  "--fire:#ef4444;--water:#3b82f6;--earth:#a16207;"
  "--air:#06b6d4;--life:#22c55e;--undead:#8b5cf6;"
  "--magic:#ec4899;--tech:#f59e0b;--light:#fde68a;--dark:#6b7280;"
  "--text:#e2e8f0;--muted:#64748b;"
"}"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{background:var(--bg);color:var(--text);"
  "font-family:'Segoe UI',system-ui,sans-serif;min-height:100vh;padding:16px}"
"h1{text-align:center;font-size:1.8rem;margin-bottom:4px;"
  "background:linear-gradient(135deg,var(--accent),var(--accent2));"
  "-webkit-background-clip:text;-webkit-text-fill-color:transparent;background-clip:text}"
".sub{text-align:center;color:var(--muted);font-size:.85rem;margin-bottom:16px}"
/* Portal bar */
".pbar{max-width:700px;margin:0 auto 16px;background:var(--card);"
  "border:1px solid var(--border);border-radius:12px;padding:12px 16px;"
  "display:flex;align-items:center;gap:10px}"
".pbar label{font-size:.8rem;color:var(--muted);white-space:nowrap}"
".pbar select,.pbar select:focus{flex:1;padding:7px 10px;background:#1a1a35;"
  "border:1px solid var(--border);border-radius:8px;color:var(--text);"
  "font-size:.88rem;cursor:pointer;outline:none}"
".pbar select:focus{border-color:var(--accent)}"
".pbadge{font-size:.7rem;padding:3px 8px;border-radius:20px;"
  "background:var(--accent);color:#fff;white-space:nowrap}"
/* Slots */
".slots{display:grid;grid-template-columns:1fr 1fr;gap:14px;"
  "max-width:700px;margin:0 auto 14px}"
"@media(max-width:480px){.slots{grid-template-columns:1fr}}"
".card{background:var(--card);border:2px solid var(--border);"
  "border-radius:16px;padding:16px;transition:border-color .3s}"
".card.active{border-color:var(--accent)}"
".card-hdr{display:flex;justify-content:space-between;align-items:center;margin-bottom:12px}"
".lbl{font-size:.72rem;text-transform:uppercase;letter-spacing:.1em;color:var(--muted)}"
".pnum{font-size:1.4rem;font-weight:700;color:var(--accent2)}"
".portrait{width:100%;aspect-ratio:1;border-radius:10px;margin-bottom:8px;"
  "display:flex;align-items:center;justify-content:center;font-size:3rem;"
  "background:linear-gradient(135deg,#1e1e3a,#2d2d5a);"
  "border:1px solid var(--border);position:relative;overflow:hidden}"
".ebg{position:absolute;inset:0;opacity:.15;border-radius:10px}"
".emoji{position:relative;z-index:1}"
".sname{font-size:.95rem;font-weight:600;text-align:center;margin-bottom:3px}"
".selem{text-align:center;font-size:.68rem;font-weight:600;text-transform:uppercase;"
  "letter-spacing:.08em;margin-bottom:8px;padding:2px 8px;border-radius:20px;"
  "display:inline-block;width:100%}"
".sfile{font-size:.65rem;color:var(--muted);text-align:center;margin-bottom:10px;"
  "overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
".empty{text-align:center;color:var(--muted);padding:14px 0}"
".empty .eicon{font-size:2.2rem;margin-bottom:5px;opacity:.3}"
".empty p{font-size:.8rem}"
/* Select */
"select{width:100%;padding:8px 10px;background:#1a1a35;"
  "border:1px solid var(--border);border-radius:8px;color:var(--text);"
  "font-size:.83rem;margin-bottom:8px;cursor:pointer}"
"select:focus{outline:none;border-color:var(--accent)}"
".nofiles{font-size:.78rem;color:var(--muted);text-align:center;padding:7px;"
  "background:#0f0f20;border-radius:8px;margin-bottom:8px;"
  "border:1px dashed var(--border)}"
/* Buttons */
".btn{width:100%;padding:9px;border:none;border-radius:8px;font-size:.87rem;"
  "font-weight:600;cursor:pointer;transition:all .2s}"
".btn-load{background:linear-gradient(135deg,var(--accent),var(--accent2));color:#fff}"
".btn-load:hover{opacity:.9;transform:translateY(-1px)}"
".btn-load:disabled{opacity:.4;cursor:not-allowed;transform:none}"
".btn-dl{background:transparent;border:1px solid #22c55e;"
  "color:#22c55e;margin-top:7px}"
".btn-dl:hover{background:#22c55e;color:#000}"
".btn-ul{background:transparent;border:1px solid var(--border);"
  "color:var(--muted);margin-top:7px}"
".btn-ul:hover{border-color:#ef4444;color:#ef4444}"
/* Upload box */
".upbox{max-width:700px;margin:0 auto 14px;background:var(--card);"
  "border:1px dashed var(--border);border-radius:12px;padding:14px;text-align:center}"
".upicon{font-size:1.8rem;margin-bottom:4px;opacity:.45}"
".updesc{font-size:.8rem;color:var(--muted);margin-bottom:8px}"
"input[type=file]{display:none}"
".upbtn{display:inline-block;padding:7px 18px;background:var(--accent);"
  "color:#fff;border-radius:8px;font-size:.83rem;font-weight:600;cursor:pointer}"
".upstat{font-size:.78rem;color:var(--accent2);margin-top:6px;min-height:16px}"
/* Bottom row */
".btmrow{max-width:700px;margin:0 auto 14px;display:flex;gap:10px}"
".btn-sense{flex:1;padding:11px;background:transparent;border:1px solid var(--accent);"
  "border-radius:10px;color:var(--accent2);font-size:.88rem;font-weight:600;"
  "cursor:pointer;transition:all .2s}"
".btn-sense:hover{background:var(--accent);color:#fff}"
/* Status */
".sbar{max-width:700px;margin:0 auto;padding:9px 14px;"
  "background:var(--card);border:1px solid var(--border);"
  "border-radius:8px;font-size:.78rem;color:var(--muted);"
  "display:flex;align-items:center;gap:8px}"
".dot{width:8px;height:8px;border-radius:50%;background:var(--muted);flex-shrink:0}"
".dot.ok{background:#22c55e;box-shadow:0 0 6px #22c55e}"
".dot.err{background:#ef4444}"
/* Elements */
".el-Fire{color:var(--fire)}.el-bg-Fire{background:var(--fire)}"
".el-Water{color:var(--water)}.el-bg-Water{background:var(--water)}"
".el-Earth{color:var(--earth)}.el-bg-Earth{background:var(--earth)}"
".el-Air{color:var(--air)}.el-bg-Air{background:var(--air)}"
".el-Life{color:var(--life)}.el-bg-Life{background:var(--life)}"
".el-Undead{color:var(--undead)}.el-bg-Undead{background:var(--undead)}"
".el-Magic{color:var(--magic)}.el-bg-Magic{background:var(--magic)}"
".el-Tech{color:var(--tech)}.el-bg-Tech{background:var(--tech)}"
".el-Light{color:var(--light)}.el-bg-Light{background:var(--light)}"
".el-Dark{color:var(--dark)}.el-bg-Dark{background:var(--dark)}"
"</style></head><body>"
"<h1>&#9670; KAOS Portal</h1>"
"<p class='sub'>Skylander Portal Manager</p>"

/* Portal type bar */
"<div class='pbar'>"
  "<label>Portal type</label>"
  "<select id='ptype' onchange='setPortalType(this.value)'>"
    "<option value='3'>Imaginators / SuperChargers</option>"
    "<option value='1'>Swap Force</option>"
    "<option value='0'>Spyro's Adventure / Giants</option>"
    "<option value='2'>Trap Team (Traptanium)</option>"
  "</select>"
  "<span class='pbadge' id='pbadge'>Imaginators</span>"
"</div>"

/* Slot cards */
"<div class='slots'>"
  "<div class='card' id='c0'><div class='card-hdr'>"
    "<span class='lbl'>Player 1</span><span class='pnum'>P1</span></div>"
    "<div id='c0-info'></div>"
    "<div id='c0-files'></div>"
    "<div id='c0-actions'></div>"
  "</div>"
  "<div class='card' id='c1'><div class='card-hdr'>"
    "<span class='lbl'>Player 2</span><span class='pnum'>P2</span></div>"
    "<div id='c1-info'></div>"
    "<div id='c1-files'></div>"
    "<div id='c1-actions'></div>"
  "</div>"
"</div>"

/* Upload box */
"<div class='upbox'>"
  "<div class='upicon'>&#8679;</div>"
  "<p class='updesc'>Upload a Skylander dump to device flash</p>"
  "<label>"
    "<input type='file' id='upfile' accept='.bin,.dmp,.sky,.dump' onchange='uploadFile(this)'>"
    "<span class='upbtn'>Choose .bin / .dmp / .sky</span>"
  "</label>"
  "<div class='upstat' id='upstat'></div>"
"</div>"

/* Bottom row */
"<div class='btmrow'>"
  "<button class='btn-sense' onclick='sense()'>&#8635; Force Sense</button>"
"</div>"
"<div class='sbar'><div class='dot' id='dot'></div><span id='stxt'>Connecting...</span></div>"

"<script>"
"const EL={'Fire':'🔥','Water':'💧','Earth':'🪨','Air':'🌪️',"
  "'Life':'🌿','Undead':'💀','Magic':'✨','Tech':'⚙️','Light':'☀️','Dark':'🌑'};"
"const TN={0:\"Spyro's Adv\",1:'Giants/SwapForce',2:'Trap Team',3:'Imaginators'};"
"let S={files:[],slots:[{loaded:false},{loaded:false}],portal_type:3};"
"let _ptypeUserSet=false;"

/* The ONLY persistent state: what file each slot has selected */
"let _sel=['',''];"

"async function go(){"
  "try{"
    "S=await(await fetch('/api/state')).json();"
    "render();st('Connected',1);"
    "if(!_ptypeUserSet){"
      "document.getElementById('ptype').value=S.portal_type||3;"
      "document.getElementById('pbadge').textContent=TN[S.portal_type||3];"
    "}"
    "_ptypeUserSet=false;"
  "}catch(e){st('No connection',0)}"
"}"

"function render(){"
  "for(let i=0;i<2;i++){"
    "const s=S.slots[i];"
    "const card=document.getElementById('c'+i);"
    "const info=document.getElementById('c'+i+'-info');"
    "const fbox=document.getElementById('c'+i+'-files');"
    "const acts=document.getElementById('c'+i+'-actions');"
    "if(!card||!info||!fbox||!acts)continue;"
    "card.classList.toggle('active',s.loaded);"

    /* Info — always update */
    "if(s.loaded){"
      "const e=s.element||'Magic',em=EL[e]||'✨';"
      "info.innerHTML='<div class=\"portrait\"><div class=\"ebg el-bg-'+e+'\"></div>'"
        "+'<div class=\"emoji\">'+em+'</div></div>'"
        "+'<div class=\"sname\">'+(s.name||'Unknown')+'</div>'"
        "+'<div class=\"selem el-'+e+'\">'+e+'</div>'"
        "+'<div class=\"sfile\">'+(s.filename||'')+'</div>';"
    "}else{"
      "info.innerHTML='<div class=\"empty\"><div class=\"eicon\">👻</div><p>No Skylander</p></div>';"
    "}"

    /* File selector — read current value FIRST, then rebuild, then restore */
    "if(S.files&&S.files.length){"
      /* Read current DOM value before touching anything */
      "const domSel=document.getElementById('s'+i);"
      "if(domSel&&domSel.value)_sel[i]=domSel.value;"
      /* If _sel[i] not in file list, default to first file */
      "if(!S.files.includes(_sel[i]))_sel[i]=S.files[0];"
      /* Build select with correct option pre-selected */
      "let fh='<select id=\"s'+i+'\" onchange=\"_sel['+i+']=this.value\">';"
      "fh+=S.files.map(f=>'<option'+(f===_sel[i]?' selected':'')+'>'+f+'</option>').join('');"
      "fh+='</select>';"
      "fh+='<button class=\"btn btn-del\" onclick=\"delSel('+i+')\" "
        "style=\"background:transparent;border:1px solid #ef4444;color:#ef4444;"
        "margin-top:7px;width:100%;padding:9px;border-radius:8px;"
        "font-size:.87rem;font-weight:600;cursor:pointer\">&#128465; Delete</button>';"
      "fbox.innerHTML=fh;"
    "}else{"
      "fbox.innerHTML='<div class=\"nofiles\">No files \u2014 upload one above</div>';"
    "}"

    /* Actions — always update */
    "let ah='';"
    "if(S.files&&S.files.length)ah+='<button class=\"btn btn-load\" onclick=\"load('+i+')\">Load</button>';"
    "else ah+='<button class=\"btn btn-load\" disabled>Load</button>';"
    "if(s.loaded){"
      "ah+='<button class=\"btn btn-dl\" onclick=\"dl('+i+')\">&#8681; Download save</button>';"
      "ah+='<button class=\"btn btn-ul\" onclick=\"unload('+i+')\">Unload</button>';"
    "}"
    "acts.innerHTML=ah;"
  "}"
"}"

"function delSel(i){"
  "const sel=document.getElementById('s'+i);"
  "if(sel&&sel.value){_sel[i]=sel.value;delFile(sel.value);}"
"}"

"async function load(i){"
  "const sel=document.getElementById('s'+i);"
  "if(sel&&sel.value)_sel[i]=sel.value;"
  "if(!_sel[i]){st('Select a file first',0);return;}"
  "st('Loading...',1);"
  "try{"
    "const j=await(await fetch('/api/load',{method:'POST',"
      "headers:{'Content-Type':'application/json'},"
      "body:JSON.stringify({slot:i,file:_sel[i]})})).json();"
    "if(j.ok){await go();st('Loaded',1);}else st('Failed: '+(j.error||'?'),0);"
  "}catch(e){st('Error',0)}"
"}"

/* unload */
"async function unload(slot){"
  "st('Unloading...',1);"
  "await fetch('/api/unload',{method:'POST',"
    "headers:{'Content-Type':'application/json'},body:JSON.stringify({slot})});"
  "await go();st('Unloaded',1);"
"}"

/* download */
"function dl(slot){window.location='/api/download?slot='+slot;}"

/* upload */
"async function uploadFile(inp){"
  "const f=inp.files[0];if(!f)return;"
  "const stat=document.getElementById('upstat');"
  "stat.textContent='Uploading '+f.name+'...';"
  "const fd=new FormData();fd.append('file',f);"
  "try{"
    "const j=await(await fetch('/api/upload',{method:'POST',body:fd})).json();"
    "if(j.ok){stat.textContent='✓ '+f.name+' saved';await go();}"
    "else stat.textContent='✗ '+(j.error||'Upload failed');"
  "}catch(e){stat.textContent='✗ Error'}"
  "inp.value='';"
"}"

"async function delFile(f){"
  "if(!f||!confirm('Delete '+f+'?'))return;"
  "st('Deleting...',1);"
  "try{"
    "const j=await(await fetch('/api/delete',{method:'POST',"
      "headers:{'Content-Type':'application/json'},body:JSON.stringify({file:f})})).json();"
    "if(j.ok){st('Deleted',1);await go();}else st('Delete failed',0);"
  "}catch(e){st('Error',0)}"
"}"

/* sense */
"async function sense(){"
  "await fetch('/api/sense',{method:'POST'});st('Sense sent',1);"
"}"

/* portal type */
"async function setPortalType(v){"
  "_ptypeUserSet=true;"
  "document.getElementById('pbadge').textContent=TN[parseInt(v)];"
  "await fetch('/api/portaltype',{method:'POST',"
    "headers:{'Content-Type':'application/json'},body:JSON.stringify({type:parseInt(v)})});"
  "st('Portal type saved — unplug & replug Pico to apply',1);"
"}"

"function st(m,ok){"
  "document.getElementById('stxt').textContent=m;"
  "document.getElementById('dot').className='dot '+(ok?'ok':'err');"
"}"

"go();setInterval(go,4000);"
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
    char buf[2048];
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
