/*
 * web_ui.c
 * ESP32 HTTP server for KAOS — serves a single-page portal management UI.
 *
 * Endpoints:
 *   GET  /           → full HTML page (self-contained, no CDN)
 *   GET  /api/state  → JSON: current slot state + file list
 *   POST /api/load   → JSON body {slot, file} → load skylander
 *   POST /api/unload → JSON body {slot}       → unload slot
 *   POST /api/sense  → trigger portal sense
 */

#include "web_ui.h"
#include "Skylander.h"
#include "skylander_ids.h"
#include "pico_bridge.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <dirent.h>

static const char *TAG = "WebUI";

extern SemaphoreHandle_t g_sky_mutex;
extern int  g_file_count;
extern char g_file_list[64][300];

/* Portal type — sent to Pico so it knows which USB descriptor to use */
typedef enum {
    PORTAL_TYPE_SSA      = 0,   /* Spyro's Adventure / Giants */
    PORTAL_TYPE_SWAP     = 1,   /* Swap Force */
    PORTAL_TYPE_TRAP     = 2,   /* Trap Team (Traptanium) */
    PORTAL_TYPE_IMAGINATORS = 3 /* SuperChargers / Imaginators */
} portal_type_t;

static portal_type_t g_portal_type = PORTAL_TYPE_IMAGINATORS;

/* -----------------------------------------------------------------------
 * The HTML page — single-file, zero external dependencies.
 * Stored as a string literal split across chunks to stay within
 * the compiler's string literal limits.
 * ----------------------------------------------------------------------- */

static const char HTML_PAGE[] =
"<!DOCTYPE html><html lang='en'><head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>KAOS Portal Manager</title>"
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
"body{background:var(--bg);color:var(--text);font-family:'Segoe UI',system-ui,sans-serif;"
  "min-height:100vh;padding:16px}"
"h1{text-align:center;font-size:1.8rem;margin-bottom:4px;"
  "background:linear-gradient(135deg,var(--accent),var(--accent2));"
  "-webkit-background-clip:text;-webkit-text-fill-color:transparent;background-clip:text}"
".subtitle{text-align:center;color:var(--muted);font-size:.85rem;margin-bottom:16px}"
/* Portal type bar */
".portal-bar{max-width:700px;margin:0 auto 20px;background:var(--card);"
  "border:1px solid var(--border);border-radius:12px;padding:14px 16px;"
  "display:flex;align-items:center;gap:12px}"
".portal-bar label{font-size:.8rem;color:var(--muted);white-space:nowrap}"
".portal-bar select{flex:1;padding:8px 10px;background:#1a1a35;"
  "border:1px solid var(--border);border-radius:8px;color:var(--text);"
  "font-size:.9rem;cursor:pointer}"
".portal-bar select:focus{outline:none;border-color:var(--accent)}"
".portal-badge{font-size:.7rem;padding:3px 8px;border-radius:20px;"
  "background:var(--accent);color:#fff;white-space:nowrap}"
/* Slots */
".slots{display:grid;grid-template-columns:1fr 1fr;gap:16px;max-width:700px;margin:0 auto 16px}"
"@media(max-width:500px){.slots{grid-template-columns:1fr}}"
".slot-card{background:var(--card);border:2px solid var(--border);"
  "border-radius:16px;padding:18px;transition:border-color .3s}"
".slot-card.active{border-color:var(--accent)}"
".slot-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:14px}"
".slot-label{font-size:.75rem;text-transform:uppercase;letter-spacing:.1em;color:var(--muted)}"
".slot-num{font-size:1.5rem;font-weight:700;color:var(--accent2)}"
".sky-portrait{width:100%;aspect-ratio:1;border-radius:12px;margin-bottom:10px;"
  "display:flex;align-items:center;justify-content:center;font-size:3.5rem;"
  "background:linear-gradient(135deg,#1e1e3a,#2d2d5a);"
  "border:1px solid var(--border);position:relative;overflow:hidden}"
".sky-portrait .element-bg{position:absolute;inset:0;opacity:.15;border-radius:12px}"
".sky-portrait .emoji{position:relative;z-index:1}"
".sky-name{font-size:1rem;font-weight:600;text-align:center;margin-bottom:4px}"
".sky-element{text-align:center;font-size:.7rem;font-weight:600;text-transform:uppercase;"
  "letter-spacing:.08em;margin-bottom:10px;padding:2px 10px;border-radius:20px;"
  "display:inline-block;width:100%}"
".sky-file{font-size:.68rem;color:var(--muted);text-align:center;margin-bottom:12px;"
  "overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
".empty-state{text-align:center;color:var(--muted);padding:16px 0}"
".empty-state .empty-icon{font-size:2.5rem;margin-bottom:6px;opacity:.3}"
".empty-state p{font-size:.82rem}"
/* Select dropdown */
"select{width:100%;padding:9px 12px;background:#1a1a35;"
  "border:1px solid var(--border);border-radius:8px;color:var(--text);"
  "font-size:.85rem;margin-bottom:10px;cursor:pointer;appearance:none;"
  "background-image:url(\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='12' height='8' viewBox='0 0 12 8'%3E%3Cpath d='M1 1l5 5 5-5' stroke='%2364748b' stroke-width='1.5' fill='none'/%3E%3C/svg%3E\");"
  "background-repeat:no-repeat;background-position:right 12px center;padding-right:32px}"
"select:focus{outline:none;border-color:var(--accent)}"
".no-files{font-size:.8rem;color:var(--muted);text-align:center;padding:8px;"
  "background:#0f0f20;border-radius:8px;margin-bottom:10px;"
  "border:1px dashed var(--border)}"
/* Buttons */
".btn{width:100%;padding:10px;border:none;border-radius:8px;font-size:.9rem;"
  "font-weight:600;cursor:pointer;transition:all .2s;letter-spacing:.03em}"
".btn-load{background:linear-gradient(135deg,var(--accent),var(--accent2));color:#fff}"
".btn-load:hover{opacity:.9;transform:translateY(-1px)}"
".btn-load:disabled{opacity:.4;cursor:not-allowed;transform:none}"
".btn-unload{background:transparent;border:1px solid var(--border);color:var(--muted);margin-top:8px}"
".btn-unload:hover{border-color:#ef4444;color:#ef4444}"
".btn-sense{display:block;width:100%;max-width:700px;margin:0 auto 14px;"
  "padding:12px;background:transparent;border:1px solid var(--accent);"
  "border-radius:10px;color:var(--accent2);font-size:.9rem;font-weight:600;"
  "cursor:pointer;transition:all .2s}"
".btn-sense:hover{background:var(--accent);color:#fff}"
".status-bar{max-width:700px;margin:0 auto;padding:10px 16px;"
  "background:var(--card);border:1px solid var(--border);"
  "border-radius:8px;font-size:.8rem;color:var(--muted);"
  "display:flex;align-items:center;gap:8px}"
".status-dot{width:8px;height:8px;border-radius:50%;background:var(--muted);flex-shrink:0}"
".status-dot.ok{background:#22c55e;box-shadow:0 0 6px #22c55e}"
".status-dot.err{background:#ef4444}"
/* Element colours */
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
"<p class='subtitle'>Skylander Portal Manager</p>"

/* Portal type selector */
"<div class='portal-bar'>"
  "<label>Portal type</label>"
  "<select id='portal-type' onchange='setPortalType(this.value)'>"
    "<option value='3'>Imaginators / SuperChargers</option>"
    "<option value='1'>Swap Force</option>"
    "<option value='0'>Spyro's Adventure / Giants</option>"
    "<option value='2'>Trap Team (Traptanium)</option>"
  "</select>"
  "<span class='portal-badge' id='type-badge'>Imaginators</span>"
"</div>"

/* Slot cards */
"<div class='slots' id='slots'>"
  "<div class='slot-card' id='slot0'>"
    "<div class='slot-header'>"
      "<span class='slot-label'>Player 1</span>"
      "<span class='slot-num'>P1</span>"
    "</div>"
    "<div id='slot0-content'></div>"
  "</div>"
  "<div class='slot-card' id='slot1'>"
    "<div class='slot-header'>"
      "<span class='slot-label'>Player 2</span>"
      "<span class='slot-num'>P2</span>"
    "</div>"
    "<div id='slot1-content'></div>"
  "</div>"
"</div>"

"<button class='btn-sense' onclick='sendSense()'>&#8635; Force Portal Sense</button>"
"<div class='status-bar'>"
  "<div class='status-dot' id='status-dot'></div>"
  "<span id='status-text'>Connecting...</span>"
"</div>"

"<script>"
"const ELEMENTS={'Fire':'🔥','Water':'💧','Earth':'🪨','Air':'🌪️',"
  "'Life':'🌿','Undead':'💀','Magic':'✨','Tech':'⚙️','Light':'☀️','Dark':'🌑'};"
"const TYPE_NAMES={0:\"Spyro's Adv / Giants\",1:'Swap Force',2:'Trap Team',3:'Imaginators'};"
"let state={files:[],slots:[{loaded:false},{loaded:false}],portal_type:3};"

"async function fetchState(){"
  "try{"
    "const r=await fetch('/api/state');"
    "if(!r.ok)throw new Error();"
    "state=await r.json();"
    "renderSlots();"
    "document.getElementById('portal-type').value=state.portal_type||3;"
    "document.getElementById('type-badge').textContent=TYPE_NAMES[state.portal_type||3];"
    "setStatus('Connected',true);"
  "}catch(e){setStatus('No connection',false)}"
"}"

"function renderSlots(){"
  "for(let i=0;i<2;i++){"
    "const s=state.slots[i];"
    "const el=document.getElementById('slot'+i+'-content');"
    "const card=document.getElementById('slot'+i);"
    "card.classList.toggle('active',s.loaded);"
    "let html='';"
    "if(s.loaded){"
      "const elem=s.element||'Magic';"
      "const emoji=ELEMENTS[elem]||'✨';"
      "html+=`<div class='sky-portrait'>`"
        "+`<div class='element-bg el-bg-${elem}'></div>`"
        "+`<div class='emoji'>${emoji}</div></div>`"
        "+`<div class='sky-name'>${s.name||'Unknown'}</div>`"
        "+`<div class='sky-element el-${elem}'>${elem}</div>`"
        "+`<div class='sky-file'>${s.filename||''}</div>`;"
    "}else{"
      "html+=`<div class='empty-state'><div class='empty-icon'>👻</div><p>No Skylander loaded</p></div>`;"
    "}"
    /* File dropdown */
    "if(state.files&&state.files.length>0){"
      "html+=`<select id='sel${i}'>`;"
      "state.files.forEach(f=>{html+=`<option value='${f}'>${f}</option>`;});"
      "html+='</select>';"
      "html+=`<button class='btn btn-load' onclick='loadSlot(${i})'>Load</button>`;"
    "}else{"
      "html+=`<div class='no-files'>No files on SD card</div>`;"
      "html+=`<button class='btn btn-load' disabled>Load</button>`;"
    "}"
    "if(s.loaded)html+=`<button class='btn btn-unload' onclick='unloadSlot(${i})'>Unload</button>`;"
    "el.innerHTML=html;"
  "}"
"}"

"async function setPortalType(val){"
  "const v=parseInt(val);"
  "document.getElementById('type-badge').textContent=TYPE_NAMES[v];"
  "await fetch('/api/portaltype',{method:'POST',"
    "headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({type:v})});"
"}"

"async function loadSlot(slot){"
  "const sel=document.getElementById('sel'+slot);"
  "if(!sel)return;"
  "setStatus('Loading...',true);"
  "try{"
    "const r=await fetch('/api/load',{method:'POST',"
      "headers:{'Content-Type':'application/json'},"
      "body:JSON.stringify({slot,file:sel.value})});"
    "const j=await r.json();"
    "if(j.ok){await fetchState();setStatus('Loaded '+sel.value,true);}"
    "else setStatus('Load failed: '+(j.error||'unknown'),false);"
  "}catch(e){setStatus('Error',false)}"
"}"

"async function unloadSlot(slot){"
  "setStatus('Unloading...',true);"
  "try{"
    "await fetch('/api/unload',{method:'POST',"
      "headers:{'Content-Type':'application/json'},"
      "body:JSON.stringify({slot})});"
    "await fetchState();setStatus('Unloaded',true);"
  "}catch(e){setStatus('Error',false)}"
"}"

"async function sendSense(){"
  "await fetch('/api/sense',{method:'POST'});"
  "setStatus('Sense sent',true);"
"}"

"function setStatus(msg,ok){"
  "document.getElementById('status-text').textContent=msg;"
  "document.getElementById('status-dot').className='status-dot '+(ok?'ok':'err');"
"}"

"fetchState();"
"setInterval(fetchState,4000);"
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
        /* Strip mount point prefix for display */
        const char *base = strrchr(g_file_list[i], '/');
        base = base ? base+1 : g_file_list[i];
        if (i) buf[n++] = ',';
        n += json_str(buf+n, sizeof(buf)-n-4, base);
    }

    n += snprintf(buf+n, sizeof(buf)-n, "],\"slots\":[");

    for (int s = 0; s < 2; s++) {
        skylander_slot_t *sky = &g_skylanders[s];
        if (s) buf[n++] = ',';
        if (sky->loaded) {
            uint16_t char_id = skylander_read_char_id(sky->data);
            const char *name = skylander_name_from_id(char_id);
            const char *elem = skylander_element_from_id(char_id);
            const char *base = strrchr(sky->filename, '/');
            base = base ? base+1 : sky->filename;

            char namebuf[64], filbuf[128];
            json_str(namebuf, sizeof(namebuf), name ? name : "Unknown");
            json_str(filbuf,  sizeof(filbuf),  base);

            n += snprintf(buf+n, sizeof(buf)-n,
                "{\"loaded\":true,\"name\":%s,\"element\":\"%s\","
                "\"filename\":%s,\"char_id\":%u}",
                namebuf, elem ? elem : "Magic", filbuf, char_id);
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
 * POST /api/load  — body: {"slot":0,"file":"Spyro.bin"}
 * ----------------------------------------------------------------------- */
static esp_err_t handle_load(httpd_req_t *req) {
    char body[300] = {0};
    int  len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body");
        return ESP_OK;
    }
    httpd_req_recv(req, body, len);

    /* Minimal JSON parse — find "slot" and "file" fields */
    int slot = -1;
    char file[256] = {0};

    char *ps = strstr(body, "\"slot\"");
    if (ps) slot = atoi(ps + 7);

    char *pf = strstr(body, "\"file\"");
    if (pf) {
        pf = strchr(pf+6, '"');
        if (pf) {
            pf++;
            int fi = 0;
            while (*pf && *pf != '"' && fi < 255) file[fi++] = *pf++;
            file[fi] = '\0';
        }
    }

    if (slot < 0 || slot > 1 || file[0] == '\0') {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Invalid params\"}");
        return ESP_OK;
    }

    /* Find full path from file_list matching the basename */
    char full_path[300] = {0};
    xSemaphoreTake(g_sky_mutex, portMAX_DELAY);
    for (int i = 0; i < g_file_count; i++) {
        const char *base = strrchr(g_file_list[i], '/');
        base = base ? base+1 : g_file_list[i];
        if (strcmp(base, file) == 0) {
            strncpy(full_path, g_file_list[i], sizeof(full_path)-1);
            break;
        }
    }
    xSemaphoreGive(g_sky_mutex);

    if (full_path[0] == '\0') {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"File not found\"}");
        return ESP_OK;
    }

    xSemaphoreTake(g_sky_mutex, portMAX_DELAY);

    /* Read the raw encrypted bytes once — ESP32 needs to decrypt for metadata,
     * Pico also needs the raw form to decrypt on its side */
    static uint8_t raw[SKYLANDER_DUMP_SIZE];
    bool got_raw = false;
    FILE *rf = fopen(full_path, "rb");
    if (rf) {
        got_raw = (fread(raw, 1, SKYLANDER_DUMP_SIZE, rf) == SKYLANDER_DUMP_SIZE);
        fclose(rf);
    }

    bool ok = skylander_load((uint8_t)slot, full_path);
    if (ok && got_raw) {
        pico_bridge_load((uint8_t)slot, raw);
    }
    xSemaphoreGive(g_sky_mutex);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"Load failed\"}");
    return ESP_OK;
}

/* -----------------------------------------------------------------------
 * POST /api/unload  — body: {"slot":0}
 * ----------------------------------------------------------------------- */
static esp_err_t handle_unload(httpd_req_t *req) {
    char body[64] = {0};
    int  len = req->content_len;
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
 * POST /api/sense  — re-announce all loaded slots to the game
 * We do this by briefly unloading then reloading each active slot on the Pico,
 * which causes the game to re-detect them.
 * ----------------------------------------------------------------------- */
static esp_err_t handle_sense(httpd_req_t *req) {
    xSemaphoreTake(g_sky_mutex, portMAX_DELAY);
    for (int s = 0; s < 2; s++) {
        if (g_skylanders[s].loaded) {
            /* Re-send the raw dump to force the Pico to re-announce the tag */
            FILE *f = fopen(g_skylanders[s].filename, "rb");
            if (f) {
                static uint8_t raw[SKYLANDER_DUMP_SIZE];
                fread(raw, 1, SKYLANDER_DUMP_SIZE, f);
                fclose(f);
                pico_bridge_unload((uint8_t)s);
                vTaskDelay(pdMS_TO_TICKS(50));
                pico_bridge_load((uint8_t)s, raw);
            }
        }
    }
    xSemaphoreGive(g_sky_mutex);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* -----------------------------------------------------------------------
 * POST /api/portaltype  — body: {"type":0}
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
            /* Tell the Pico to switch portal type */
            uint8_t type_byte = (uint8_t)t;
            /* Reuse MSG_STATUS channel — send a special 2-byte payload:
             * [0xFF] = portal type change sentinel, [type_byte] = new type */
            /* We signal via pico_bridge directly */
            extern void pico_bridge_set_portal_type(uint8_t t);
            pico_bridge_set_portal_type(type_byte);
            ESP_LOGI(TAG, "Portal type → %d", t);
        }
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* -----------------------------------------------------------------------
 * Start server
 * ----------------------------------------------------------------------- */
httpd_handle_t web_ui_start(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size = 8192;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return NULL;
    }

    static const httpd_uri_t uris[] = {
        { .uri="/",                .method=HTTP_GET,  .handler=handle_root       },
        { .uri="/api/state",       .method=HTTP_GET,  .handler=handle_state      },
        { .uri="/api/load",        .method=HTTP_POST, .handler=handle_load       },
        { .uri="/api/unload",      .method=HTTP_POST, .handler=handle_unload     },
        { .uri="/api/sense",       .method=HTTP_POST, .handler=handle_sense      },
        { .uri="/api/portaltype",  .method=HTTP_POST, .handler=handle_portaltype },
    };
    for (int i = 0; i < 6; i++) httpd_register_uri_handler(server, &uris[i]);

    ESP_LOGI(TAG, "HTTP server started");
    return server;
}

void web_ui_stop(httpd_handle_t server) {
    if (server) httpd_stop(server);
}
