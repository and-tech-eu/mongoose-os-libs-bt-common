/*
 * Copyright (c) 2014-2018 Cesanta Software Limited
 * All rights reserved
 *
 * Licensed under the Apache License, Version 2.0 (the ""License"");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an ""AS IS"" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "mgos_bt.h"
#include "mgos_bt_gatt.h"
#include "mgos_event.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MGOS_BT_GATTC_EV_BASE MGOS_EVENT_BASE('G', 'A', 'C')

/* Note: Keep in sync with api_bt_gattc.js */
enum mgos_bt_gattc_event {
  MGOS_BT_GATTC_EV_CONNECT =
      MGOS_BT_GATTC_EV_BASE,         /* mgos_bt_gattc_connect_arg */
  MGOS_BT_GATTC_EV_DISCONNECT,       /* mgos_bt_gattc_disconnect_arg */
  MGOS_BT_GATTC_EV_DISCOVERY_RESULT, /* mgos_bt_gattc_discovery_result_arg */
  MGOS_BT_GATTC_EV_DISCOVERY_DONE,   /* mgos_bt_gattc_discovery_done_arg */
  MGOS_BT_GATTC_EV_READ_RESULT,      /* mgos_bt_gattc_read_result_arg */
  MGOS_BT_GATTC_EV_WRITE_RESULT,     /* mgos_bt_gattc_write_result_arg */
  MGOS_BT_GATTC_EV_NOTIFY,           /* mgos_bt_gattc_notify_arg */
};

#define MGOS_BT_GATTC_INVALID_CONN_ID (-1)

struct mgos_bt_gattc_connect_arg {
  struct mgos_bt_gatt_conn conn; /* Device address */
  bool ok;                       /* Success indicator. */
};

struct mgos_bt_gattc_disconnect_arg {
  struct mgos_bt_gatt_conn conn; /* Device address */
};

struct mgos_bt_gattc_discovery_result_arg {
  struct mgos_bt_gatt_conn conn; /* Device address */
  struct mgos_bt_uuid svc;       /* Service UUID */
  struct mgos_bt_uuid chr;       /* Characteristic UUID */
  uint16_t handle;               /* Characteristic handle  */
  uint8_t prop;                  /* Characteristic properties */
};

struct mgos_bt_gattc_discovery_done_arg {
  struct mgos_bt_gatt_conn conn; /* Device address */
  bool ok;                       /* Disconvery completed successfuly or not. */
};

struct mgos_bt_gattc_read_result_arg {
  struct mgos_bt_gatt_conn conn; /* Device address */
  uint16_t handle;               /* Characteristic handle  */
  bool ok;                       /* Success indicator. */
  struct mg_str data;            /* Data that has been read */
};

struct mgos_bt_gattc_write_result_arg {
  struct mgos_bt_gatt_conn conn; /* Device address */
  uint16_t handle;               /* Characteristic handle  */
  bool ok;                       /* Success indicator. */
};

struct mgos_bt_gattc_notify_arg {
  struct mgos_bt_gatt_conn conn; /* Device address */
  uint16_t handle;               /* Characteristic handle  */
  bool is_indication;            /* True if this is an indication. */
  struct mg_str data;            /* Notification data sent by the server */
};

bool mgos_bt_gattc_connect(const struct mgos_bt_addr *addr);

bool mgos_bt_gattc_discover(uint16_t conn_id);

bool mgos_bt_gattc_disconnect(uint16_t conn_id);

bool mgos_bt_gattc_read(uint16_t conn_id, uint16_t handle);

bool mgos_bt_gattc_write(uint16_t conn_id, uint16_t handle, struct mg_str data,
                         bool resp_required);

// Note that the handle here is the handle of the CCCD for the attribute,
// not the attribute itself. Usually CCCD immediately follows the attribute
// so att_handle + 1 is a reasonable guess here.
bool mgos_bt_gattc_set_notify_mode_cccd(uint16_t conn_id, uint16_t cccd_handle,
                                        enum mgos_bt_gatt_notify_mode mode);

#ifdef __cplusplus
}
#endif
