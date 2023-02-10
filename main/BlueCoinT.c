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
   if (client || !prefix || target || strcmp(prefix, prefixcommand) || !suffix)
      return NULL;              //Not for us or not a command from main MQTT
   if (!strcmp(suffix, "connect"))
      lwmqtt_subscribe(revk_mqtt(0), "BlueCoinT/#");
   if (!strcmp(suffix, "shutdown"))
      httpd_stop(webserver);
   if (!strcmp(suffix, "upgrade"))
   {
      esp_bt_controller_disable();      // Kill bluetooth during download - TODO may be better done in RevK library, with a watchdog change
      esp_wifi_set_ps(WIFI_PS_NONE);    // Fulk wifi
      revk_restart("Download started", 10);     // Restart if download does not happen properly
   }
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
   int16_t temp;		// Temp
   uint8_t new:1;               // Should try a connection
   uint8_t connected:1;         // We have seen a connection
   uint8_t missing:1;           // Missing
   uint8_t found:1;             // Found
   uint8_t apple:1;             // Found apple
   uint8_t nearby:1;            // Found apple nearby adv
   // TODO what BlueCoinT
};
device_t *device = NULL;

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

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
   switch (event->type)
   {
   case BLE_GAP_EVENT_DISC:
      {
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
            const uint8_t *P = p + 1;
	    o+=sprintf(o,"%02X:",*P++);
	    if(p[1]==8||p[1]==9)
	    { // Name short or full
	       o+=sprintf(o,"\"");
	       while(P<n)o+=sprintf(o,"%c",*P++);
	       o+=sprintf(o,"\"");
	    }else if(p[1]==0x16)
	    { // 16 bit UUID and value
	       if(P[0]==0x6E&&P[1]==0x2A&&n==P+4)
	       { // Temp
		  int16_t v=((P[3]<<8)|P[2]);
		  d->temp=v;
		  if(v<0)
		  {
	             o+=sprintf(o,"-");
	             v=-v;
		  }
		  o+=sprintf(o,"%d.%02d℃",v/100,v%100);
	       }else
	       {
                  o+=sprintf(o,"%02X%02X=",P[0],P[1]);
	          P+=2;
	          while(P<n)
	             o+=sprintf(o,"%02X",*P++);
	       }
	    }else
	    { // Other data just hex dumped
	       while(P<n)o+=sprintf(o,"%02X",*P++);
	    }
	    o+=sprintf(o," ");
	    p=n;
	 }
         if (o == msg)
            break;
         if (o[-1] == ' ')
            o--;
         *o++ = 0;
         ESP_LOGI(TAG, "Disc event %d, rssi %d, %s %s", event->disc.event_type, event->disc.rssi, ble_addr_format(&d->addr), msg);
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

   /* main loop */
   while (1)
   {
      usleep(100000);
      uint32_t now = uptime();
      // Devices missing
         for (device_t * d = device; d; d = d->next)
            if (!d->missing && d->last + (d->apple ? missingtime : 300) < now)
            {                   // Missing
               d->missing = 1;
               d->connected = 0;
               ESP_LOGI(TAG, "Missing %s", ble_addr_format(&d->addr));
            }
      // Devices found
      for (device_t * d = device; d; d = d->next)
         if (d->found)
         {
            d->found = 0;
            ESP_LOGI(TAG, "Found %s", ble_addr_format(&d->addr));
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
