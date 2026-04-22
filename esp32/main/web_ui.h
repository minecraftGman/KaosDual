#pragma once
#ifndef WEB_UI_H
#define WEB_UI_H

#include "esp_http_server.h"

/* Load saved portal type from NVS into g_portal_type — call before web_ui_start */
void web_ui_load_portal_type(void);

/* Start the HTTP server and register all URI handlers */
httpd_handle_t web_ui_start(void);

/* Stop the HTTP server */
void web_ui_stop(httpd_handle_t server);

#endif
