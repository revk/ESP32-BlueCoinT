/* DEFCON app */
/* Copyright ©2022 - 2023 Adrian Kennard, Andrews & Arnold Ltd.See LICENCE file for details .GPL 3.0 */

static __attribute__((unused))
const char TAG[] = "DEFCON";

#include "revk.h"
#include "esp_sleep.h"
#include "esp_task_wdt.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_crt_bundle.h"
#include "esp_bt.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "console/console.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

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
#ifndef	CONFIG_SOC_BLE_SUPPORTED
#error	You need CONFIG_SOC_BLE_SUPPORTED
#endif

#define	MAXGPIO	36
#define BITFIELDS "-"
#define PORT_INV 0x40
#define port_mask(p) ((p)&0x3F)

httpd_handle_t webserver = NULL;

#define	settings		\
	u32(missingtime,30)	\

#define u32(n,d)        uint32_t n;
#define s8(n,d) int8_t n;
#define u8(n,d) uint8_t n;
#define b(n) uint8_t n;
#define s(n) char * n;
#define io(n,d)           uint8_t n;
settings
#undef io
#undef u32
#undef s8
#undef u8
#undef b
#undef s
    int8_t defcon_level = 9;
uint8_t pair = 0;               // pairing needed

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
   return web_foot(req);
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
   if (ble_gap_adv_active())
      ble_gap_adv_stop();
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
   if (prefix && !strcmp(prefix, TAG) && target && isdigit((int) *target) && !target[1])
      return setdefcon(*target - '0', value);
   if (client || !prefix || target || strcmp(prefix, prefixcommand) || !suffix)
      return NULL;              //Not for us or not a command from main MQTT
   if (isdigit((int) *suffix) && !suffix[1])
      return setdefcon(*suffix - '0', value);
   if (!strcmp(suffix, "connect"))
      lwmqtt_subscribe(revk_mqtt(0), "DEFCON/#");
   if (!strcmp(suffix, "shutdown"))
      httpd_stop(webserver);
   if (!strcmp(suffix, "upgrade"))
   {
      esp_bt_controller_disable();      // Kill bluetooth during download - TODO may be better done in RevK library, with a watchdog change
      esp_wifi_set_ps(WIFI_PS_NONE);    // Fulk wifi
      revk_restart("Download started", 10);     // Restart if download does not happen properly
   }
   if (!strcmp(suffix, "pair"))
      pair = 1;                 // TODO test pairing mode
   return NULL;
}

/* BLE */

typedef struct device_s device_t;
struct device_s {
   device_t *next;              // Linked list
   ble_addr_t addr;             // Address (includes type)
   int8_t rssi;                 // RSSI
   uint8_t len;                 // data len
   uint8_t data[31];            // data (adv)
   uint32_t last;               // uptime of last seen
   uint8_t new:1;               // Should try a connection
   uint8_t connected:1;         // We have seen a connection
   uint8_t missing:1;           // Missing
   uint8_t found:1;             // Found
   uint8_t apple:1;             // Found apple
   uint8_t nearby:1;            // Found apple nearby adv
   // TODO what DEFCON
};
device_t *device = NULL;

uint8_t connected = 0;
uint32_t scanstart = 0;

#define GATT_DEVICE_INFO_UUID                   0x180A
#define GATT_MANUFACTURER_NAME_UUID             0x2A29
#define GATT_MODEL_NUMBER_UUID                  0x2A24

struct ble_hs_cfg;
struct ble_gatt_register_ctxt;

static const char *ble_addr_format(ble_addr_t * a)
{
   static char buf[30];
   snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", a->val[5], a->val[4], a->val[3], a->val[2], a->val[1], a->val[0]);
   if (a->type == BLE_ADDR_PUBLIC)
      strcat(buf, "(pub)");
   else if (a->type == BLE_ADDR_RANDOM)
      strcat(buf, "(rand)");
   else if (a->type == BLE_ADDR_PUBLIC_ID)
      strcat(buf, "(pubid)");
   else if (a->type == BLE_ADDR_RANDOM_ID)
      strcat(buf, "(randid)");
   return buf;
}

device_t *find_device(ble_addr_t * a)
{                               // Find (create) a device record
   device_t *d;
   for (d = device; d; d = d->next)
      if (d->addr.type == a->type && !memcmp(d->addr.val, a->val, 6))
         break;
   if (!d)
   {
      d = malloc(sizeof(*d));
      memset(d, 0, sizeof(*d));
      d->addr = *a;
      d->new = 1;
      d->next = device;
      device = d;
      d->found = 1;
   }
   if (d->missing)
   {
      d->missing = 0;
      d->found = 1;
   }
   d->last = uptime();
   return d;
}

static int gatt_svr_chr_access_device_info(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg);

int gatt_svr_init(void);

static int ble_gap_event(struct ble_gap_event *event, void *arg);

static uint8_t ble_addr_type;

static const char *manuf_name = "RevK";
static const char *model_num = "DEFCON Lights";

static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
   {
    /* Service: Device Information */
    .type = BLE_GATT_SVC_TYPE_PRIMARY,
    .uuid = BLE_UUID16_DECLARE(GATT_DEVICE_INFO_UUID),
    .characteristics = (struct ble_gatt_chr_def[])
    {
     {
      /* Characteristic: * Manufacturer name */
      .uuid = BLE_UUID16_DECLARE(GATT_MANUFACTURER_NAME_UUID),
      .access_cb = gatt_svr_chr_access_device_info,
      .flags = BLE_GATT_CHR_F_READ,
      },
     {
      /* Characteristic: Model number string */
      .uuid = BLE_UUID16_DECLARE(GATT_MODEL_NUMBER_UUID),
      .access_cb = gatt_svr_chr_access_device_info,
      .flags = BLE_GATT_CHR_F_READ,
      },
     {
      0,                        /* No more characteristics in this service */
      },
     }
     },

   {
    0,                          /* No more services */
     },
};

static int gatt_svr_chr_access_device_info(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
   uint16_t uuid;
   int rc;

   uuid = ble_uuid_u16(ctxt->chr->uuid);

   if (uuid == GATT_MODEL_NUMBER_UUID)
   {
      rc = os_mbuf_append(ctxt->om, model_num, strlen(model_num));
      return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
   }

   if (uuid == GATT_MANUFACTURER_NAME_UUID)
   {
      rc = os_mbuf_append(ctxt->om, manuf_name, strlen(manuf_name));
      return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
   }

   assert(0);
   return BLE_ATT_ERR_UNLIKELY;
}

int gatt_svr_init(void)
{
   int rc;

   ble_svc_gap_init();
   ble_svc_gatt_init();

   rc = ble_gatts_count_cfg(gatt_svr_svcs);
   if (rc != 0)
   {
      return rc;
   }

   rc = ble_gatts_add_svcs(gatt_svr_svcs);
   if (rc != 0)
   {
      return rc;
   }

   return 0;
}

/*
 * Enables advertising with parameters:
 *     o General discoverable mode
 *     o Undirected connectable mode
 */
static void ble_advertise(uint8_t pair)
{
   struct ble_gap_adv_params adv_params;
   struct ble_hs_adv_fields fields;
   int rc;

   memset(&fields, 0, sizeof(fields));
   fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

   static const uint8_t svc[] = { 0x12, 0x18 }; // HID - gets iPhone attention
   fields.svc_data_uuid16 = svc;
   fields.svc_data_uuid16_len = 2;

   fields.appearance = 0x0140;  // Generic display
   fields.appearance_is_present = 1;

   fields.tx_pwr_lvl_is_present = 1;
   fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

   static uint8_t name[sizeof(TAG)];
   memcpy(name, TAG, sizeof(TAG) - 1);
   name[sizeof(TAG) - 1] = '0' + defcon_level;  // TODO should be the pair level we are setting really
   fields.name = name;
   fields.name_len = sizeof(TAG) - ((defcon_level < 0 || defcon_level > 9) ? 1 : 0);
   fields.name_is_complete = 1;

   rc = ble_gap_adv_set_fields(&fields);
   if (rc != 0)
      return;

   if (ble_gap_adv_active())
      ble_gap_adv_stop();

   /* Begin advertising */
   memset(&adv_params, 0, sizeof(adv_params));
   adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
   adv_params.disc_mode = (pair ? BLE_GAP_DISC_MODE_LTD : BLE_GAP_DISC_MODE_GEN);
   if (ble_gap_adv_start(ble_addr_type, NULL, pair ? 30000 : BLE_HS_FOREVER, &adv_params, ble_gap_event, NULL))
      ESP_LOGI(TAG, "Advertised start failed");
}

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
   switch (event->type)
   {
   case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status)
      {                         // Failed
         ESP_LOGI(TAG, "Connect failed");
         connected = 0;
         break;
      }
      connected = 1;
      struct ble_gap_conn_desc c;
      if (!ble_gap_conn_find(event->connect.conn_handle, &c))
      {
         device_t *d = find_device(&c.peer_ota_addr);
         d->connected = 1;
         ESP_LOGI(TAG, "Connected %s %s", ble_addr_format(&c.peer_ota_addr), c.role == BLE_GAP_ROLE_SLAVE ? "slave" : "master");
         if (ble_gap_security_initiate(event->connect.conn_handle))
            ESP_LOGE(TAG, "Security failed");
      } else
         ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
      break;

   case BLE_GAP_EVENT_L2CAP_UPDATE_REQ:
      ESP_LOGI(TAG, "L2CAP update req");
      return 1;

   case BLE_GAP_EVENT_CONN_UPDATE:
      {
         ESP_LOGI(TAG, "Update");
      }
      break;

   case BLE_GAP_EVENT_ENC_CHANGE:
      {
         struct ble_gap_conn_desc c;
         if (!ble_gap_conn_find(event->enc_change.conn_handle, &c))
         {
            ESP_LOGI(TAG, "Enc %s %s%s%s%s", ble_addr_format(&c.peer_ota_addr), c.role == BLE_GAP_ROLE_SLAVE ? "slave" : "master", c.sec_state.encrypted ? " encrypted" : "", c.sec_state.authenticated ? " authenticated" : "", c.sec_state.bonded ? " bonded" : "");
            if (memcmp(c.peer_ota_addr.val, c.peer_id_addr.val, 6))
            {
               ESP_LOGE(TAG, "Different ID address %s", ble_addr_format(&c.peer_id_addr));
            }
         }
         ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
         ble_gap_adv_stop();
         // TODO update based on pair logic for requested defcon
         // TODO how do we confirm the bonding data used?
      }
      break;
   case BLE_GAP_EVENT_REPEAT_PAIRING:

      return BLE_GAP_REPEAT_PAIRING_RETRY;

   case BLE_GAP_EVENT_IDENTITY_RESOLVED:
      ESP_LOGI(TAG, "Resolved");
      break;

   case BLE_GAP_EVENT_DISCONNECT:
      ESP_LOGI(TAG, "Disconnected");
      connected = 0;
      break;

   case BLE_GAP_EVENT_ADV_COMPLETE:
      break;

   case BLE_GAP_EVENT_SUBSCRIBE:
      break;

   case BLE_GAP_EVENT_MTU:
      break;

   case BLE_GAP_EVENT_DISC:
      {
         if (event->disc.event_type)
            break;              // Not simple adv
         const uint8_t *p = event->disc.data,
             *e = p + event->disc.length_data;
         if (e > p + 31)
            break;
         device_t *d = find_device(&event->disc.addr);
         d->rssi = event->disc.rssi;
         if (d->len == event->disc.length_data && !memcmp(d->data, event->disc.data, d->len))
            break;
         d->len = event->disc.length_data;
         if (d->len)
            memcpy(d->data, event->disc.data, d->len);
         char msg[200],         // Message size very limited so always OK
         *o = msg;
         while (p < e)
         {
            const uint8_t *n = p + *p + 1;
            if (n > e)
               break;
            if (*p > 3 && p[1] == 0xFF && p[2] == 0x4C && p[3] == 0x00)
            {                   // Apple
               d->apple = 1;
               const uint8_t *P = p + 4;
               while (P + 1 < n)
               {
                  const uint8_t *N = P + P[1] + 2;
                  if (N > n)
                     break;
                  if (*P == 0x10)
                  {
                     d->nearby = 1;
                     o += sprintf(o, "Nearby ");
                     const uint8_t *Q = P + 2;
                     while (Q < N)
                        o += sprintf(o, "%02X ", *Q++);
                  }
                  P = N;
               }
            }
            p = n;
         }
         if (o == msg)
            break;
         if (o[-1] == ' ')
            o--;
         *o++ = 0;
         ESP_LOGI(TAG, "Disc event %d, rssi %d, %s", event->disc.event_type, event->disc.rssi, ble_addr_format(&d->addr));
         break;
      }

   default:
      ESP_LOGE(TAG, "BLE event %d", event->type);
      break;
   }

   return 0;
}

static void ble_start_disc(void)
{
   struct ble_gap_disc_params disc_params = {
      .passive = 1,
   };
   if (ble_gap_disc(0 /* public */ , BLE_HS_FOREVER, &disc_params, ble_gap_event, NULL))
      ESP_LOGI(TAG, "Discover failed to start");
   scanstart = uptime();
}

static void ble_on_sync(void)
{
   int rc;

   rc = ble_hs_id_infer_auto(0, &ble_addr_type);
   assert(rc == 0);

   uint8_t addr_val[6] = { 0 };
   rc = ble_hs_id_copy_addr(ble_addr_type, addr_val, NULL);

   ble_start_disc();
}

static void ble_on_reset(int reason)
{
}

void ble_task(void *param)
{
   ESP_LOGI(TAG, "BLE Host Task Started");
   /* This function will return only when nimble_port_stop() is executed */
   nimble_port_run();

   nimble_port_freertos_deinit();
}


/* MAIN */
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
   REVK_ERR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));  /* default mode, but library may have overridden, needed for BLE at same time as wifi */
   nimble_port_init();

   /* Initialize the NimBLE host configuration */
   ble_hs_cfg.sync_cb = ble_on_sync;
   ble_hs_cfg.reset_cb = ble_on_reset;
   ble_hs_cfg.sm_sc = 1;
   ble_hs_cfg.sm_mitm = 1;
   ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;

   gatt_svr_init();
   ble_svc_gap_device_name_set(TAG);

   /* Start the task */
   nimble_port_freertos_init(ble_task);

   /* main loop */
   while (1)
   {
      sleep(1);
      uint32_t now = uptime();
      // Devices missing
      if (ble_gap_disc_active() && scanstart + missingtime < now)
         for (device_t * d = device; d; d = d->next)
            if (!d->missing && d->last + (d->apple ? missingtime : 300) < now)
            {                   // Missing
               d->missing = 1;
               ESP_LOGI(TAG, "Missing %s", ble_addr_format(&d->addr));
            }
      // Devices found
      for (device_t * d = device; d; d = d->next)
         if (d->found)
         {
            d->found = 0;
            ESP_LOGI(TAG, "Found %s", ble_addr_format(&d->addr));
         }

      for (device_t * d = device; d; d = d->next)
         if (d->new)
         {                      // New devices
            d->new = 0;
            ESP_LOGI(TAG, "New %s", ble_addr_format(&d->addr));
            // TODO We are going to need to do a short directed connection request to each new device
         }

      if (connected || ble_gap_conn_active())
         continue;

      if (pair)
      {                         // Start pairing logic
         pair = 0;
         ble_advertise(1);
      }

      if (!ble_gap_disc_active())
      {                         // Restart discovery, but first should be safe to check for deletions
         uint32_t now = uptime();
         device_t **dd = &device;
         while (*dd)
         {
            device_t *d = *dd;
            if (d->last + 300 < now)
            {
               ESP_LOGI(TAG, "Forget %s", ble_addr_format(&d->addr));
               *dd = d->next;
               free(d);
               continue;
            }
            dd = &d->next;
         }
         ble_start_disc();
      }

      if (!ble_gap_adv_active())
         ble_advertise(0);
   }
   return;
}
