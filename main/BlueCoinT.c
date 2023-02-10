/* BlueCoinT app */
/* Copyright ©2022 - 2023 Adrian Kennard, Andrews & Arnold Ltd.See LICENCE file for details .GPL 3.0 */

static __attribute__((unused))
const char TAG[] = "BlueCoinT";

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

#include <driver/gpio.h>

#ifndef	CONFIG_SOC_BLE_SUPPORTED
#error	You need CONFIG_SOC_BLE_SUPPORTED
#endif

#define	MAXGPIO	36
#define BITFIELDS "-"
#define PORT_INV 0x40
#define port_mask(p) ((p)&0x3F)

httpd_handle_t webserver = NULL;

typedef struct device_s device_t;
struct device_s {
   device_t *next;              // Linked list
   ble_addr_t addr;             // Address (includes type)
   uint8_t namelen;             // Device name length
   char name[32];               // Device name (null terminated)
   char better[13];             // ID (Mac) of better device
   int8_t betterrssi;           // Better RSSI
   int8_t rssi;                 // RSSI
   uint32_t lastbetter;         // uptime when last better entry
   uint32_t last;               // uptime of last seen
   uint32_t lastreport;         // uptime of last reported
   int16_t temp;                // Temp
   int16_t tempreport;          // Temp last reported
   uint8_t found:1;
   uint8_t missing:1;
};
device_t *device = NULL;
device_t *find_device(ble_addr_t * a, int make);

#define	settings		\
	u32(missingtime,30)	\
	u32(reporting,60)	\
	u8(temprise,50)		\

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
const char *app_callback(int client, const char *prefix, const char *target, const char *suffix, jo_t j)
{
   if (j && target && !strcmp(prefix, "info") && !strcmp(suffix, "report") && strlen(target) <= 12)
   {                            // Other reports
      ble_addr_t a = {.type = BLE_ADDR_PUBLIC };
      if (jo_find(j, "rssi"))
      {
         int rssi = jo_read_int(j);
         if (jo_find(j, "address"))
         {
            uint8_t add[18] = { 0 };
            jo_strncpy(j, add, sizeof(add));
            for (int i = 0; i < 6; i++)
               a.val[5 - i] = (((isalpha(add[i * 3]) ? 9 : 0) + (add[i * 3] & 0xF)) << 4) + (isalpha(add[i * 3 + 1]) ? 9 : 0) + (add[i * 3 + 1] & 0xF);
            device_t *d = find_device(&a, 0);
            if (d)
            {
               int c = strcmp(target, d->better);
               if (!c || !*d->better || rssi > d->rssi || (rssi == d->rssi && c > 0))
               {                // Record best
                  if (c)
                  {
                     strcpy(d->better, target);
                     ESP_LOGI(TAG, "Found possibly better \"%s\" %s %d", d->name, target, rssi);
                  }
               }
               d->betterrssi = rssi;
               d->lastbetter = uptime();
            }
         }
      }
   }
   if (client || !prefix || target || strcmp(prefix, prefixcommand) || !suffix)
      return NULL;              //Not for us or not a command from main MQTT
   if (!strcmp(suffix, "connect"))
   {
      lwmqtt_subscribe(revk_mqtt(0), "info/BlueCoinT/#");
   }
   if (!strcmp(suffix, "shutdown"))
      httpd_stop(webserver);
   if (!strcmp(suffix, "upgrade"))
   {
      esp_bt_controller_disable();      // Kill bluetooth during download - TODO may be better done in RevK library, with a watchdog change
      esp_wifi_set_ps(WIFI_PS_NONE);    // Full wifi
      revk_restart("Download started", 10);     // Restart if download does not happen properly
   }
   return NULL;
}

/* BLE */


struct ble_hs_cfg;
struct ble_gatt_register_ctxt;

static const char *ble_addr_format(ble_addr_t * a)
{
   static char buf[30];
   snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", a->val[5], a->val[4], a->val[3], a->val[2], a->val[1], a->val[0]);
   if (a->type == BLE_ADDR_RANDOM)
      strcat(buf, "(rand)");
   else if (a->type == BLE_ADDR_PUBLIC_ID)
      strcat(buf, "(pubid)");
   else if (a->type == BLE_ADDR_RANDOM_ID)
      strcat(buf, "(randid)");
   //else if (a->type == BLE_ADDR_PUBLIC) strcat(buf, "(pub)");
   return buf;
}

device_t *find_device(ble_addr_t * a, int make)
{                               // Find (create) a device record
   device_t *d;
   for (d = device; d; d = d->next)
      if (d->addr.type == a->type && !memcmp(d->addr.val, a->val, 6))
         break;
   if (!d && !make)
      return d;
   if (!d)
   {
      d = malloc(sizeof(*d));
      memset(d, 0, sizeof(*d));
      d->addr = *a;
      d->next = device;
      device = d;
      d->found = 1;
   }
   d->last = uptime();
   return d;
}

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
   switch (event->type)
   {
   case BLE_GAP_EVENT_DISC:
      {
         const uint8_t *p = event->disc.data,
             *e = p + event->disc.length_data;
         if (e > p + 31)
            break;              // Silly
         // Check if a temp device
         const uint8_t *name = NULL;
         const uint8_t *temp = NULL;
         //const uint8_t *bat = NULL;
         while (p < e)
         {
            const uint8_t *n = p + *p + 1;
            if (n > e)
               break;
            if (p[0] > 1 && (p[1] == 8 || p[1] == 9))
               name = p;
            else if (*p == 5 && p[1] == 0x16 && p[2] == 0x6E && p[3] == 0x2A)
               temp = p;
            // TODO bat
            p = n;
         }
         if (!temp || !name)
            break;              // Not temp device
         device_t *d = find_device(&event->disc.addr, 1);
         if (d->namelen != *name - 1 || memcmp(d->name, name + 2, d->namelen))
         {
            memcpy(d->name, name + 2, d->namelen = *name - 1);
            d->name[d->namelen] = 0;
         }
         if (temp)
            d->temp = ((temp[5] << 8) | temp[4]);
         // TODO bat
         d->rssi = event->disc.rssi;
         if (d->missing)
         {
            d->lastreport = 0;
            d->missing = 0;
            d->found = 1;
         }
         ESP_LOGI(TAG, "Temp \"%s\" %d %d", d->name, d->temp, d->rssi);
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
      ESP_LOGE(TAG, "Discover failed to start");
}

static uint8_t ble_addr_type;
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

   revk_wait_mqtt(60);

   REVK_ERR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));  /* default mode, but library may have overridden, needed for BLE at same time as wifi */
   nimble_port_init();

   /* Initialize the NimBLE host configuration */
   ble_hs_cfg.sync_cb = ble_on_sync;
   ble_hs_cfg.reset_cb = ble_on_reset;
   ble_hs_cfg.sm_sc = 1;
   ble_hs_cfg.sm_mitm = 0;
   ble_hs_cfg.sm_bonding = 1;
   ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;

   /* Start the task */
   nimble_port_freertos_init(ble_task);

   jo_t jo_device(device_t * d) {
      jo_t j = jo_object_alloc();
      jo_string(j, "address", ble_addr_format(&d->addr));
      jo_string(j, "name", d->name);
      return j;
   }

   /* main loop */
   while (1)
   {
      usleep(100000);
      uint32_t now = uptime();
      // Devices missing
      for (device_t * d = device; d; d = d->next)
         if (!d->missing && d->last + missingtime < now && !*d->better)
         {                      // Missing
            d->missing = 1;
            jo_t j = jo_device(d);
            revk_event("missing", &j);
            ESP_LOGI(TAG, "Missing %s", ble_addr_format(&d->addr));
         }
      // Devices found
      for (device_t * d = device; d; d = d->next)
         if (d->found)
         {
            d->found = 0;
            jo_t j = jo_device(d);
            revk_event("found", &j);
            ESP_LOGI(TAG, "Found %s", ble_addr_format(&d->addr));
         }
      for (device_t * d = device; d; d = d->next)
         if (*d->better && d->lastbetter + reporting * 3 / 2 < now)
            *d->better = 0;     // Not seeing better
      // Reporting
      for (device_t * d = device; d; d = d->next)
         if (!d->missing && (d->lastreport + reporting <= now || d->tempreport + temprise < d->temp))
         {
            d->lastreport = now;
            d->tempreport = d->temp;
            if (*d->better && (d->betterrssi > d->rssi || (d->betterrssi == d->rssi && strcmp(d->better, revk_id) > 0)))
            {
               ESP_LOGI(TAG, "Not reporting \"%s\" %d as better %s %d", d->name, d->rssi, d->better, d->betterrssi);
               continue;
            }
            jo_t j = jo_device(d);
            if (d->temp < 0)
               jo_litf(j, "temp", "-%d.%02d", (-d->temp) / 100, (-d->temp) % 100);
            else
               jo_litf(j, "temp", "%d.%02d", d->temp / 100, d->temp % 100);
            jo_int(j, "rssi", d->rssi);
            // TODO bat
            revk_info("report", &j);
            ESP_LOGI(TAG, "Report %s \"%s\" %d (%s %d)", ble_addr_format(&d->addr), d->name, d->rssi, d->better, d->betterrssi);
            // TODO listening and only reporting if we are better rssi than other reports
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
   }
   return;
}
