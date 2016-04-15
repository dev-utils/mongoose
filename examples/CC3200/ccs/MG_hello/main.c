/*
 * Copyright (c) 2014-2016 Cesanta Software Limited
 * All rights reserved
 */

/* Set up an AP or connect to existing WiFi network. */
#define WIFI_AP_SSID "Mongoose"
#define WIFI_AP_PASS ""
#define WIFI_AP_CHAN 6
// #define WIFI_STA_SSID "YourWiFi"
// #define WIFI_STA_PASS "YourPass"

#define MG_TASK_PRIORITY 3
#define MG_TASK_STACK_SIZE 8192

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <inc/hw_types.h>
#include <inc/hw_ints.h>
#include <inc/hw_memmap.h>

#include <driverlib/gpio.h>
#include <driverlib/interrupt.h>
#include <driverlib/pin.h>
#include <driverlib/prcm.h>
#include <driverlib/rom.h>
#include <driverlib/rom_map.h>

#include <example/common/gpio_if.h>

/* Mongoose.h brings in SimpleLink support. Do not include simplelink.h. */
#include <mongoose.h>

#include <simplelink/include/device.h>

#include "wifi.h"

static const char *upload_form =
    "\
<h1>Upload file</h1> \
<form action='/upload' method='POST' enctype='multipart/form-data'> \
  <input type='file' name='file'> \
  <input type='submit' value='Upload'> \
</form>";

static struct mg_str upload_fname(struct mg_connection *nc,
                                  struct mg_str fname) {
  struct mg_str lfn;
  lfn.len = fname.len + 3;
  lfn.p = malloc(lfn.len);
  memcpy((char *) lfn.p, "SL:", 3);
  memcpy((char *) lfn.p + 3, fname.p, fname.len);
  return lfn;
}

void mg_ev_handler(struct mg_connection *nc, int ev, void *ev_data) {
  LOG(LL_DEBUG, ("%p ev %d", nc, ev));

  switch (ev) {
    case MG_EV_ACCEPT: {
      char addr[32];
      mg_sock_addr_to_str(&nc->sa, addr, sizeof(addr),
                          MG_SOCK_STRINGIFY_IP | MG_SOCK_STRINGIFY_PORT);
      LOG(LL_INFO, ("Connection %p from %s", nc, addr));
      break;
    }
    case MG_EV_HTTP_REQUEST: {
      char addr[32];
      struct http_message *hm = (struct http_message *) ev_data;
      cs_stat_t st;
      mg_sock_addr_to_str(&nc->sa, addr, sizeof(addr),
                          MG_SOCK_STRINGIFY_IP | MG_SOCK_STRINGIFY_PORT);
      LOG(LL_INFO,
          ("HTTP request from %s: %.*s %.*s", addr, (int) hm->method.len,
           hm->method.p, (int) hm->uri.len, hm->uri.p));
      if (mg_vcmp(&hm->uri, "/upload") == 0 ||
          (mg_vcmp(&hm->uri, "/") == 0 && mg_stat("SL:index.html", &st) != 0)) {
        mg_send(nc, upload_form, strlen(upload_form));
        nc->flags |= MG_F_SEND_AND_CLOSE;
        break;
      }
      struct mg_serve_http_opts opts;
      memset(&opts, 0, sizeof(opts));
      opts.document_root = "SL:";
      mg_serve_http(nc, hm, opts);
      break;
    }
    case MG_EV_CLOSE: {
      LOG(LL_INFO, ("Connection %p closed", nc));
      break;
    }
    case MG_EV_HTTP_PART_BEGIN:
    case MG_EV_HTTP_PART_DATA:
    case MG_EV_HTTP_PART_END: {
      struct mg_http_multipart_part *mp =
          (struct mg_http_multipart_part *) ev_data;
      if (ev == MG_EV_HTTP_PART_BEGIN) {
        LOG(LL_INFO, ("Begin file upload: %s", mp->file_name));
      } else if (ev == MG_EV_HTTP_PART_END) {
        LOG(LL_INFO, ("End file upload: %s", mp->file_name));
      }
      mg_file_upload_handler(nc, ev, ev_data, upload_fname);
    }
  }
}

static void mg_init(struct mg_mgr *mgr) {
  LOG(LL_INFO, ("MG task running"));

  stop_nwp(); /* See function description in wifi.c */
  int role = sl_Start(0, 0, 0);
  if (role < 0) {
    LOG(LL_ERROR, ("Failed to start NWP"));
    return;
  }

  LOG(LL_INFO, ("NWP started"));

  sl_fs_init();

#if defined(WIFI_STA_SSID)
  if (!wifi_setup_sta(WIFI_STA_SSID, WIFI_STA_PASS)) {
    LOG(LL_ERROR, ("Error setting up WiFi station"));
  }
#elif defined(WIFI_AP_SSID)
  if (!wifi_setup_ap(WIFI_AP_SSID, WIFI_AP_PASS, WIFI_AP_CHAN)) {
    LOG(LL_ERROR, ("Error setting up WiFi AP"));
  }
#else
#error WiFi not configured
#endif

  /* We don't need SimpleLink's web server. */
  sl_NetAppStop(SL_NET_APP_HTTP_SERVER_ID);

  const char *err = "";
  struct mg_bind_opts opts;
  memset(&opts, 0, sizeof(opts));
  opts.error_string = &err;

  struct mg_connection *nc = mg_bind(mgr, "80", mg_ev_handler);
  if (nc != NULL) {
    mg_set_protocol_http_websocket(nc);
  } else {
    LOG(LL_ERROR, ("Failed to create listener: %s", err));
  }
}

#ifndef USE_TIRTOS
/* Int vector table, defined in startup_gcc.c */
extern void (*const g_pfnVectors[])(void);
#endif

int main() {
#ifndef USE_TIRTOS
  MAP_IntVTableBaseSet((unsigned long) &g_pfnVectors[0]);
#endif
  MAP_IntEnable(FAULT_SYSTICK);
  MAP_IntMasterEnable();
  PRCMCC3200MCUInit();

  setvbuf(stdout, NULL, _IOLBF, 0);
  setvbuf(stderr, NULL, _IOLBF, 0);
  cs_log_set_level(LL_INFO);
  cs_log_set_file(stdout);

  LOG(LL_INFO, ("Hello, world!"));

  /* Set up the red LED. Note that amber and green cannot be used as they share
   * pins with I2C. */
  MAP_PRCMPeripheralClkEnable(PRCM_GPIOA1, PRCM_RUN_MODE_CLK);
  MAP_PinTypeGPIO(PIN_64, PIN_MODE_0, false);
  MAP_GPIODirModeSet(GPIOA1_BASE, 0x2, GPIO_DIR_MODE_OUT);
  GPIO_IF_LedConfigure(LED1);
  GPIO_IF_LedOn(MCU_RED_LED_GPIO);

  if (VStartSimpleLinkSpawnTask(8) != 0) {
    LOG(LL_ERROR, ("Failed to create SL task"));
  }

  if (!mg_start_task(MG_TASK_PRIORITY, MG_TASK_STACK_SIZE, mg_init)) {
    LOG(LL_ERROR, ("Failed to create MG task"));
  }

  osi_start();

  return 0;
}

/* These are FreeRTOS hooks for various life situations. */
void vApplicationMallocFailedHook() {
}

void vApplicationIdleHook() {
}

void vApplicationStackOverflowHook(OsiTaskHandle *th, signed char *tn) {
}

void SimpleLinkHttpServerCallback(SlHttpServerEvent_t *e,
                                  SlHttpServerResponse_t *resp) {
}

void SimpleLinkSockEventHandler(SlSockEvent_t *e) {
}

void SimpleLinkGeneralEventHandler(SlDeviceEvent_t *e) {
  LOG(LL_ERROR, ("status %d sender %d", e->EventData.deviceEvent.status,
                 e->EventData.deviceEvent.sender));
}
