/* DEFCON app */
/* Copyright ©2022 - 2023 Adrian Kennard, Andrews & Arnold Ltd.See LICENCE file for details .GPL 3.0 */

static __attribute__((unused))
const char TAG[] = "DEFCON";

/* Notes
 * 2. Timeout limited advertise, have timeout on phone setup.
 * 3. Disconnect after pairing, we don't stay connected.
 * 4. Work out can we passive scan and find the devices we paired with to determine presence?
 * 5. Work out why upgrade not working, watchdog?
 */

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
    int8_t defcon = 0;
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
   if (!strcmp(suffix, "upgrade"))
	   esp_bt_controller_disable();
   return NULL;
}

/* BLE */

/* Heart-rate configuration */
#define GATT_HRS_UUID                           0x180D
#define GATT_HRS_MEASUREMENT_UUID               0x2A37
#define GATT_HRS_BODY_SENSOR_LOC_UUID           0x2A38
#define GATT_DEVICE_INFO_UUID                   0x180A
#define GATT_MANUFACTURER_NAME_UUID             0x2A29
#define GATT_MODEL_NUMBER_UUID                  0x2A24

extern uint16_t hrs_hrm_handle;

struct ble_hs_cfg;
struct ble_gatt_register_ctxt;

void gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg);
int gatt_svr_init(void);

static bool notify_state;

static uint16_t conn_handle;

static const char *device_name = "DEFCON";

static int ble_gap_event(struct ble_gap_event *event, void *arg);

static uint8_t ble_addr_type;

static const char *manuf_name = "RevK";
static const char *model_num = "DEFCON Lights";
uint16_t hrs_hrm_handle;

static int
gatt_svr_chr_access_heart_rate(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg);

static int
gatt_svr_chr_access_device_info(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        /* Service: Heart-rate */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(GATT_HRS_UUID),
        .characteristics = (struct ble_gatt_chr_def[])
        { {
                /* Characteristic: Heart-rate measurement */
                .uuid = BLE_UUID16_DECLARE(GATT_HRS_MEASUREMENT_UUID),
                .access_cb = gatt_svr_chr_access_heart_rate,
                .val_handle = &hrs_hrm_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            }, {
                /* Characteristic: Body sensor location */
                .uuid = BLE_UUID16_DECLARE(GATT_HRS_BODY_SENSOR_LOC_UUID),
                .access_cb = gatt_svr_chr_access_heart_rate,
                .flags = BLE_GATT_CHR_F_READ,
            }, {
                0, /* No more characteristics in this service */
            },
        }
    },

    {
        /* Service: Device Information */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(GATT_DEVICE_INFO_UUID),
        .characteristics = (struct ble_gatt_chr_def[])
        { {
                /* Characteristic: * Manufacturer name */
                .uuid = BLE_UUID16_DECLARE(GATT_MANUFACTURER_NAME_UUID),
                .access_cb = gatt_svr_chr_access_device_info,
                .flags = BLE_GATT_CHR_F_READ,
            }, {
                /* Characteristic: Model number string */
                .uuid = BLE_UUID16_DECLARE(GATT_MODEL_NUMBER_UUID),
                .access_cb = gatt_svr_chr_access_device_info,
                .flags = BLE_GATT_CHR_F_READ,
            }, {
                0, /* No more characteristics in this service */
            },
        }
    },

    {
        0, /* No more services */
    },
};

static int
gatt_svr_chr_access_heart_rate(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    /* Sensor location, set to "Chest" */
    static uint8_t body_sens_loc = 0x01;
    uint16_t uuid;
    int rc;

    uuid = ble_uuid_u16(ctxt->chr->uuid);

    if (uuid == GATT_HRS_BODY_SENSOR_LOC_UUID) {
        rc = os_mbuf_append(ctxt->om, &body_sens_loc, sizeof(body_sens_loc));

        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    assert(0);
    return BLE_ATT_ERR_UNLIKELY;
}

static int
gatt_svr_chr_access_device_info(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint16_t uuid;
    int rc;

    uuid = ble_uuid_u16(ctxt->chr->uuid);

    if (uuid == GATT_MODEL_NUMBER_UUID) {
        rc = os_mbuf_append(ctxt->om, model_num, strlen(model_num));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (uuid == GATT_MANUFACTURER_NAME_UUID) {
        rc = os_mbuf_append(ctxt->om, manuf_name, strlen(manuf_name));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    assert(0);
    return BLE_ATT_ERR_UNLIKELY;
}

void
gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    switch (ctxt->op) {
    case BLE_GATT_REGISTER_OP_SVC:
        break;

    case BLE_GATT_REGISTER_OP_CHR:
        break;

    case BLE_GATT_REGISTER_OP_DSC:
        break;

    default:
        assert(0);
        break;
    }
}

int
gatt_svr_init(void)
{
    int rc;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    rc = ble_gatts_count_cfg(gatt_svr_svcs);
    if (rc != 0) {
        return rc;
    }

    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

/*
 * Enables advertising with parameters:
 *     o General discoverable mode
 *     o Undirected connectable mode
 */
static void
ble_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    int rc;

    /*
     *  Set the advertisement data included in our advertisements:
     *     o Flags (indicates advertisement type and other general info)
     *     o Advertising tx power
     *     o Device name
     */
    memset(&fields, 0, sizeof(fields));

    /*
     * Advertise two flags:
     *      o Discoverability in forthcoming advertisement (general)
     *      o BLE-only (BR/EDR unsupported)
     */
    fields.flags = BLE_HS_ADV_F_DISC_GEN |
                   BLE_HS_ADV_F_BREDR_UNSUP;

    /*
     * Indicate that the TX power level field should be included; have the
     * stack fill this value automatically.  This is done by assigning the
     * special value BLE_HS_ADV_TX_PWR_LVL_AUTO.
     */

    static const uint8_t svc[]={0x12,0x18};
    fields.svc_data_uuid16=svc;
    fields.svc_data_uuid16_len=2;

    fields.appearance=0x0140; // Generic display
    fields.appearance_is_present=1;

    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    fields.name = (uint8_t *)device_name;
    fields.name_len = strlen(device_name);
    fields.name_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0)
        return;

    /* Begin advertising */
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN; /* TODO BLE_GAP_DISC_MODE_LTD */
    rc = ble_gap_adv_start(ble_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_gap_event, NULL); /* TODO 30 seconds */
    if (rc != 0)
        return;
}

static int
ble_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        /* A new connection was established or a connection attempt failed */
        if (event->connect.status != 0) {
            /* Connection failed; resume advertising */
            ble_advertise();
        }
        conn_handle = event->connect.conn_handle;
	ble_gap_security_initiate(conn_handle);
        break;

    case BLE_GAP_EVENT_IDENTITY_RESOLVED:
	ESP_LOGI(TAG,"Resolved");
	break;

    case BLE_GAP_EVENT_DISCONNECT:
	ESP_LOGI(TAG,"Disconnect");

        /* Connection terminated; resume advertising */
        ble_advertise();
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ble_advertise();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == hrs_hrm_handle) {
            notify_state = event->subscribe.cur_notify;
        } else if (event->subscribe.attr_handle != hrs_hrm_handle) {
            notify_state = event->subscribe.cur_notify;
        }
        break;

    case BLE_GAP_EVENT_MTU:
        break;

    case BLE_GAP_EVENT_DISC:
	{
	ESP_LOGI(TAG,"Disc event %d, rssi %d, addr type %d %02X:%02X:%02X:%02X:%02X:%02X",event->disc.event_type,event->disc.rssi,event->disc.addr.type,event->disc.addr.val[0],event->disc.addr.val[1],event->disc.addr.val[2],event->disc.addr.val[3],event->disc.addr.val[4],event->disc.addr.val[5]);
	const uint8_t *p=event->disc.data,*e=p+event->disc.length_data;
	while(p<e)
	{
		if(*p)
		{
			if(*p==2&&p[1]==1)ESP_LOGI(TAG,"Flags %02X",p[2]);
			else if(p[1]==9)ESP_LOGI(TAG,"Name %.*s",*p-1,p+2);
			else ESP_LOG_BUFFER_HEX(TAG,p,*p+1);
		}
		p+=*p+1;
	}
	break;
	}

    default:
	ESP_LOGI(TAG,"BLE event %d",event->type);
	break;
    }

    return 0;
}

static void
ble_on_sync(void)
{
    int rc;

    rc = ble_hs_id_infer_auto(0, &ble_addr_type);
    assert(rc == 0);

    uint8_t addr_val[6] = {0};
    rc = ble_hs_id_copy_addr(ble_addr_type, addr_val, NULL);

    /* Begin advertising */
    //ble_advertise(); // TODO
     struct ble_gap_disc_params disc_params={
//.passive=1,
.filter_duplicates=1,	// TODO we should handle maybe
     };
     ble_gap_disc(0 /* public */, BLE_HS_FOREVER , &disc_params, ble_gap_event,NULL);
}

static void
ble_on_reset(int reason)
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

    /* Set the default device name */
    ble_svc_gap_device_name_set(device_name);

    /* Start the task */
    nimble_port_freertos_init(ble_task);

    /* main loop */
   while (1)
   {
      sleep(1);
   }
   return;
}
