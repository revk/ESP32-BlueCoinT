/* DEFCON app */
/* Copyright ©2022 - 2023 Adrian Kennard, Andrews & Arnold Ltd.See LICENCE file for details .GPL 3.0 */

static const char TAG[] = "DEFCON";

#include "revk.h"
#include "esp_sleep.h"
#include "esp_task_wdt.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_crt_bundle.h"
#include <driver/gpio.h>

#ifdef	CONFIG_LWIP_DHCP_DOES_ARP_CHECK
#warning CONFIG_LWIP_DHCP_DOES_ARP_CHECK means DHCP is slow
#endif
#ifndef	CONFIG_LWIP_DHCP_RESTORE_LAST_IP
#warning CONFIG_LWIP_DHCP_RESTORE_LAST_IP may improve speed
#endif
#ifndef	CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP
#warning CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP may speed boot
#endif
#if	CONFIG_BOOTLOADER_LOG_LEVEL > 0
#warning CONFIG_BOOTLOADER_LOG_LEVEL recommended to be no output
#endif

#define	MAXGPIO	36
#define BITFIELDS "-^"
#define PORT_INV 0x4000
#define PORT_PU 0x2000
#define port_mask(p) ((p)&0xFF) // 16 bit

// TODO 8 bit?

// Dynamic
httpd_handle_t webserver = NULL;

#define	settings		\

#define u32(n,d)        uint32_t n;
#define s8(n,d) int8_t n;
#define u8(n,d) uint8_t n;
#define b(n) uint8_t n;
#define s(n) char * n;
#define io(n,d)           uint16_t n;
settings
#undef io
#undef u32
#undef s8
#undef u8
#undef b
#undef s

int8_t defcon=0;
int8_t defcon_level = 9;

static void web_head(httpd_req_t * req, const char *title)
{
   httpd_resp_set_type(req, "text/html; charset=utf-8");
   httpd_resp_sendstr_chunk(req, "<meta name='viewport' content='width=device-width, initial-scale=1'>");
   httpd_resp_sendstr_chunk(req, "<html><head><title>");
   if (title)
      httpd_resp_sendstr_chunk(req, title);
   httpd_resp_sendstr_chunk(req, "</title></head><style>"       //
                            "a.defcon{text-decoration:none;border:1px solid black;border-radius:50%;margin:2px;padding:3px;display:inline-block;width:1em;text-align:center;}"  //
                            "a.on{border:3px solid black;}"     //
                            "a.d1{background-color:white;}"     //
                            "a.d2{background-color:red;}"       //
                            "a.d3{background-color:yellow;}"    //
                            "a.d4{background-color:green;color:white;}" //
                            "a.d5{background-color:blue;color:white;}"  //
                            "body{font-family:sans-serif;background:#8cf;}"     //
                            "</style><body><h1>");
   if (title)
      httpd_resp_sendstr_chunk(req, title);
   httpd_resp_sendstr_chunk(req, "</h1>");
}

static esp_err_t web_foot(httpd_req_t * req)
{
   httpd_resp_sendstr_chunk(req, "<hr><address>");
   char temp[20];
   snprintf(temp, sizeof(temp), "%012llX", revk_binid);
   httpd_resp_sendstr_chunk(req, temp);
   httpd_resp_sendstr_chunk(req, " <a href='wifi'>WiFi Setup</a></address></body></html>");
   httpd_resp_sendstr_chunk(req, NULL);
   return ESP_OK;
}

static esp_err_t web_icon(httpd_req_t * req)
{                               // serve image -  maybe make more generic file serve
   extern const char start[] asm("_binary_apple_touch_icon_png_start");
   extern const char end[] asm("_binary_apple_touch_icon_png_end");
   httpd_resp_set_type(req, "image/png");
   httpd_resp_send(req, start, end - start);
   return ESP_OK;
}

static esp_err_t web_root(httpd_req_t * req)
{
   if (revk_link_down())
      return revk_web_config(req);      // Direct to web set up
   web_head(req, *hostname ? hostname : appname);
   if (defcon)
   {                            // Defcon controls
      size_t len = httpd_req_get_url_query_len(req);
      char q[2] = { };
      if (len == 1)
      {
         httpd_req_get_url_query_str(req, q, sizeof(q));
         if (isdigit((int) *q))
            defcon_level = *q - '0';
         else if (*q == '+' && defcon_level < 9)
            defcon_level++;
         else if (*q == '-' && defcon_level > 0)
            defcon_level--;
      }
      for (int i = 0; i <= 9; i++)
         if (i <= 6 || i == 9)
         {
            q[0] = '0' + i;
            httpd_resp_sendstr_chunk(req, "<a href='?");
            httpd_resp_sendstr_chunk(req, q);
            httpd_resp_sendstr_chunk(req, "' class='defcon d");
            httpd_resp_sendstr_chunk(req, q);
            if (i == defcon_level)
               httpd_resp_sendstr_chunk(req, " on");
            httpd_resp_sendstr_chunk(req, "'>");
            httpd_resp_sendstr_chunk(req, i == 9 ? "X" : q);
            httpd_resp_sendstr_chunk(req, "</a>");
         }
   }
   return web_foot(req);
}

void defcon_task(void *arg)
{
   while (1)
   {
      usleep(10000);
   }
}


char *setdefcon(int level, char *value)
{                               // DEFCON state
   // With value it is used to turn on/off a defcon state, the lowest set dictates the defcon level
   // With no value, this sets the DEFCON state directly instead of using lowest of state set
   static uint8_t state = 0;    // DEFCON state
   if (*value)
   {
      if (*value == '1' || *value == 't' || *value == 'y')
         state |= (1 << level);
      else
         state &= ~(1 << level);
      int l;
      for (l = 0; l < 8 && !(state & (1 << l)); l++);
      defcon_level = l;
   } else
      defcon_level = level;
   return "";
}

const char *app_callback(int client, const char *prefix, const char *target, const char *suffix, jo_t j)
{
   char value[1000];
   int len = 0;
   *value = 0;
   if (j)
   {
      len = jo_strncpy(j, value, sizeof(value));
      if (len < 0)
         return "Expecting JSON string";
      if (len > sizeof(value))
         return "Too long";
   }
   if (defcon && prefix && !strcmp(prefix, "DEFCON") && target && isdigit((int) *target) && !target[1])
      return setdefcon(*target - '0', value);
   if (client || !prefix || target || strcmp(prefix, prefixcommand) || !suffix)
      return NULL;              //Not for us or not a command from main MQTT
   if (defcon && isdigit((int) *suffix) && !suffix[1])
      return setdefcon(*suffix - '0', value);
   if (!strcmp(suffix, "connect"))
   {
      if (defcon)
         lwmqtt_subscribe(revk_mqtt(0), "DEFCON/#");
   }
   if (!strcmp(suffix, "shutdown"))
      httpd_stop(webserver);
   return NULL;
}

// --------------------------------------------------------------------------------
// Web
#ifdef	CONFIG_REVK_APCONFIG
#error 	Clash with CONFIG_REVK_APCONFIG set
#endif

void app_main()
{
   revk_boot(&app_callback);
#define io(n,d)           revk_register(#n,0,sizeof(n),&n,"- "#d,SETTING_SET|SETTING_BITFIELD);
#define b(n) revk_register(#n,0,sizeof(n),&n,NULL,SETTING_BOOLEAN);
#define u32(n,d) revk_register(#n,0,sizeof(n),&n,#d,0);
#define s8(n,d) revk_register(#n,0,sizeof(n),&n,#d,SETTING_SIGNED);
#define u8(n,d) revk_register(#n,0,sizeof(n),&n,#d,0);
#define s(n) revk_register(#n,0,0,&n,NULL,0);
   settings
#undef io
#undef u32
#undef s8
#undef u8
#undef b
#undef s
       revk_start();

   // Web interface
   httpd_config_t config = HTTPD_DEFAULT_CONFIG();
   if (!httpd_start(&webserver, &config))
   {
      {
         httpd_uri_t uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = web_root,
            .user_ctx = NULL
         };
         REVK_ERR_CHECK(httpd_register_uri_handler(webserver, &uri));
      }
      {
         httpd_uri_t uri = {
            .uri = "/apple-touch-icon.png",
            .method = HTTP_GET,
            .handler = web_icon,
            .user_ctx = NULL
         };
         REVK_ERR_CHECK(httpd_register_uri_handler(webserver, &uri));
      }
      {
         httpd_uri_t uri = {
            .uri = "/wifi",
            .method = HTTP_GET,
            .handler = revk_web_config,
            .user_ctx = NULL
         };
         REVK_ERR_CHECK(httpd_register_uri_handler(webserver, &uri));
      }
      revk_web_config_start(webserver);
   }

      revk_task("defcon", defcon_task, 0);

      //We run forever, not sleeping
      ESP_LOGE(TAG, "Idle");
      while (1)
         sleep(1);
      return;
}
