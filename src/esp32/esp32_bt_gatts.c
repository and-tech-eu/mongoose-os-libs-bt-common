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

#include "mgos_bt_gatts.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "mgos.h"

#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"

#include "esp32_bt_gap.h"
#include "esp32_bt_internal.h"

#ifndef MGOS_BT_GATTS_MAX_PREPARED_WRITE_LEN
#define MGOS_BT_GATTS_MAX_PREPARED_WRITE_LEN 4096
#endif

struct esp32_bt_service_attr_info;

struct esp32_bt_gatts_service_entry {
  struct mgos_bt_uuid uuid;
  uint16_t svc_handle;
  struct esp32_bt_service_attr_info *attrs;
  uint16_t num_attrs;
  enum mgos_bt_gatt_sec_level sec_level;
  uint8_t deleting : 1;
  mgos_bt_gatts_ev_handler_t handler;
  void *handler_arg;
  // NimBLE variants of UUID, characteristic and descriptor definitions.
  ble_uuid_any_t ble_uuid;
  struct ble_gatt_chr_def *ble_chars;
  struct ble_gatt_dsc_def *ble_descrs;
  struct ble_gatt_svc_def ble_svc_def[2];
  SLIST_ENTRY(esp32_bt_gatts_service_entry) next;
};

struct esp32_bt_service_attr_info {
  struct mgos_bt_gatts_char_def def;
  ble_uuid_any_t ble_uuid;
  uint16_t handle;
};

struct esp32_bt_gatts_pending_write {
  uint16_t handle;
  struct mbuf value;
  SLIST_ENTRY(esp32_bt_gatts_pending_write) next;
};

struct esp32_bt_gatts_pending_ind {
  uint16_t handle;
  bool is_ind;
  struct mg_str value;
  STAILQ_ENTRY(esp32_bt_gatts_pending_ind) next;
};

struct esp32_bt_gatts_connection_entry;

// A set of sessions is kept for each connection, one per service.
struct esp32_bt_gatts_session_entry {
  struct esp32_bt_gatts_connection_entry *ce;
  struct mgos_bt_gatts_conn gsc;
  struct esp32_bt_gatts_service_entry *se;
  struct mbuf resp_data;
  SLIST_HEAD(pending_writes, esp32_bt_gatts_pending_write) pending_writes;
  SLIST_ENTRY(esp32_bt_gatts_session_entry) next;
};

struct esp32_bt_gatts_connection_entry {
  struct mgos_bt_gatt_conn gc;
  enum mgos_bt_gatt_sec_level sec_level;
  bool need_auth;
  bool ind_in_flight;
  /* Notifications/indications are finicky, so we keep at most one in flight. */
  int ind_queue_len;
  STAILQ_HEAD(pending_inds, esp32_bt_gatts_pending_ind) pending_inds;
  SLIST_HEAD(sessions, esp32_bt_gatts_session_entry) sessions;  // 1 per service
  SLIST_ENTRY(esp32_bt_gatts_connection_entry) next;
};

struct esp32_bt_gatts_ev_info {
  // esp_gatts_cb_event_t ev;
  // esp_ble_gatts_cb_param_t ep;
};

struct mgos_rlock_type *s_lock;
static SLIST_HEAD(s_svcs, esp32_bt_gatts_service_entry) s_svcs =
    SLIST_HEAD_INITIALIZER(s_svcs);
static SLIST_HEAD(s_conns, esp32_bt_gatts_connection_entry) s_conns =
    SLIST_HEAD_INITIALIZER(s_conns);

static void esp32_bt_gatts_send_next_ind_locked(
    struct esp32_bt_gatts_connection_entry *ce);
static void esp32_bt_gatts_create_sessions(
    struct esp32_bt_gatts_connection_entry *ce);

static uint16_t get_read_perm(enum mgos_bt_gatt_sec_level sec_level) {
  if (mgos_sys_config_get_bt_gatts_min_sec_level() > sec_level) {
    sec_level = mgos_sys_config_get_bt_gatts_min_sec_level();
  }
  uint16_t res = BLE_GATT_CHR_F_READ;
  switch (sec_level) {
    case MGOS_BT_GATT_SEC_LEVEL_NONE:
      break;
    case MGOS_BT_GATT_SEC_LEVEL_AUTH:
      res |= BLE_GATT_CHR_F_READ_AUTHEN;
      break;
    case MGOS_BT_GATT_SEC_LEVEL_ENCR:
    case MGOS_BT_GATT_SEC_LEVEL_ENCR_MITM:
      res |= BLE_GATT_CHR_F_READ_ENC;
      break;
  }
  return res;
}

static uint16_t get_write_perm(enum mgos_bt_gatt_sec_level sec_level) {
  if (mgos_sys_config_get_bt_gatts_min_sec_level() > sec_level) {
    sec_level = mgos_sys_config_get_bt_gatts_min_sec_level();
  }
  uint16_t res = BLE_GATT_CHR_F_WRITE;
  switch (sec_level) {
    case MGOS_BT_GATT_SEC_LEVEL_NONE:
      break;
    case MGOS_BT_GATT_SEC_LEVEL_AUTH:
      res |= BLE_GATT_CHR_F_WRITE_AUTHEN;
      break;
    case MGOS_BT_GATT_SEC_LEVEL_ENCR:
    case MGOS_BT_GATT_SEC_LEVEL_ENCR_MITM:
      res |= BLE_GATT_CHR_F_WRITE_ENC;
      break;
  }
  return res;
}

static void esp32_gatts_register_cb(struct ble_gatt_register_ctxt *ctxt,
                                    void *arg) {
  char buf[MGOS_BT_UUID_STR_LEN];

  switch (ctxt->op) {
    case BLE_GATT_REGISTER_OP_SVC:
      LOG(LL_DEBUG, ("REGISTER_SVC %s sh %d",
                     esp32_bt_uuid_to_str(ctxt->svc.svc_def->uuid, buf),
                     ctxt->svc.handle));
      break;

    case BLE_GATT_REGISTER_OP_CHR:
      LOG(LL_DEBUG, ("REGISTER_CHR %s dh %d vh %d",
                     esp32_bt_uuid_to_str(ctxt->chr.chr_def->uuid, buf),
                     ctxt->chr.def_handle, ctxt->chr.val_handle));
      break;

    case BLE_GATT_REGISTER_OP_DSC:
      LOG(LL_DEBUG, ("REGISTERP_DSC %s vh %d",
                     esp32_bt_uuid_to_str(ctxt->dsc.dsc_def->uuid, buf),
                     ctxt->dsc.handle));
      break;
  }
}

static struct esp32_bt_gatts_service_entry *find_service_by_attr_handle(
    uint16_t attr_handle, struct esp32_bt_service_attr_info **ai) {
  struct esp32_bt_gatts_service_entry *se;
  SLIST_FOREACH(se, &s_svcs, next) {
    for (size_t i = 0; i < se->num_attrs; i++) {
      if (se->attrs[i].handle == attr_handle) {
        if (ai != NULL) *ai = &se->attrs[i];
        return se;
      }
    }
  }
  return NULL;
}

static struct esp32_bt_gatts_connection_entry *find_connection(
    uint16_t conn_id) {
  struct esp32_bt_gatts_connection_entry *ce = NULL;
  SLIST_FOREACH(ce, &s_conns, next) {
    if (ce->gc.conn_id == conn_id) return ce;
  }
  return NULL;
}

static struct esp32_bt_gatts_session_entry *find_session(
    uint16_t conn_id, uint16_t handle, struct esp32_bt_service_attr_info **ai) {
  struct esp32_bt_gatts_connection_entry *ce = find_connection(conn_id);
  if (ce == NULL) return NULL;
  struct esp32_bt_gatts_service_entry *se =
      find_service_by_attr_handle(handle, ai);
  if (se == NULL) return NULL;
  struct esp32_bt_gatts_session_entry *sse;
  SLIST_FOREACH(sse, &ce->sessions, next) {
    if (sse->se == se) return sse;
  }
  return NULL;
}

static struct esp32_bt_gatts_session_entry *find_session_by_gsc(
    const struct mgos_bt_gatts_conn *gsc) {
  if (gsc == NULL) return NULL;
  struct esp32_bt_gatts_connection_entry *ce = find_connection(gsc->gc.conn_id);
  if (ce == NULL) return NULL;
  struct esp32_bt_gatts_session_entry *sse;
  SLIST_FOREACH(sse, &ce->sessions, next) {
    if (&sse->gsc == gsc) return sse;
  }
  return NULL;
}

static enum mgos_bt_gatt_status esp32_bt_gatts_call_handler(
    struct esp32_bt_gatts_session_entry *sse,
    struct esp32_bt_service_attr_info *ai, enum mgos_bt_gatts_ev ev,
    void *ev_arg) {
  struct esp32_bt_gatts_service_entry *se = sse->se;
  /* Invoke attr handler if defined, fall back to service-wide handler. */
  if (ai != NULL && ai->def.handler != NULL) {
    return ai->def.handler(&sse->gsc, ev, ev_arg, ai->def.handler_arg);
  }
  return se->handler(&sse->gsc, ev, ev_arg, se->handler_arg);
}

void esp32_bt_gatts_close_session(struct esp32_bt_gatts_session_entry *sse) {
  struct esp32_bt_gatts_pending_write *pw, *pwt;
  SLIST_FOREACH_SAFE(pw, &sse->pending_writes, next, pwt) {
    mbuf_free(&pw->value);
    memset(pw, 0, sizeof(*pw));
    free(pw);
  }
  SLIST_REMOVE(&sse->ce->sessions, sse, esp32_bt_gatts_session_entry, next);
  esp32_bt_gatts_call_handler(sse, NULL, MGOS_BT_GATTS_EV_DISCONNECT, NULL);
  mbuf_free(&sse->resp_data);
  memset(sse, 0, sizeof(*sse));
  free(sse);
}

static void esp32_bt_gatts_create_sessions(
    struct esp32_bt_gatts_connection_entry *ce) {
  /* Create a session for each of the currently registered services. */
  struct esp32_bt_gatts_service_entry *se;
  SLIST_FOREACH(se, &s_svcs, next) {
    struct esp32_bt_gatts_session_entry *sse = calloc(1, sizeof(*sse));
    if (sse == NULL) break;
    sse->ce = ce;
    sse->se = se;
    sse->gsc.gc = ce->gc;
    sse->gsc.svc_uuid = se->uuid;
    mbuf_init(&sse->resp_data, 0);
    SLIST_INIT(&sse->pending_writes);
    enum mgos_bt_gatt_status st =
        esp32_bt_gatts_call_handler(sse, NULL, MGOS_BT_GATTS_EV_CONNECT, NULL);
    if (st != MGOS_BT_GATT_STATUS_OK) {
      /* Service rejected the connection, do not create session for it. */
      free(sse);
      continue;
    }
    SLIST_INSERT_HEAD(&ce->sessions, sse, next);
  }
}

int mgos_bt_gatts_get_num_connections(void) {
  int num = 0;
  mgos_rlock(s_lock);
  struct esp32_bt_gatts_connection_entry *ce;
  SLIST_FOREACH(ce, &s_conns, next) num++;
  mgos_runlock(s_lock);
  return num;
}

bool mgos_bt_gatts_is_send_queue_empty(void) {
  bool res = true;
  mgos_rlock(s_lock);
  struct esp32_bt_gatts_connection_entry *ce;
  SLIST_FOREACH(ce, &s_conns, next) {
    if (!STAILQ_EMPTY(&ce->pending_inds)) {
      res = false;
      break;
    }
  }
  mgos_runlock(s_lock);
  return res;
}

static void esp32_bt_gatts_send_next_ind_locked(
    struct esp32_bt_gatts_connection_entry *ce) {
  int rc;
  if (ce->ind_in_flight) return;
  if (STAILQ_EMPTY(&ce->pending_inds)) return;
  struct esp32_bt_gatts_pending_ind *pi = STAILQ_FIRST(&ce->pending_inds);
  struct os_mbuf *om = ble_hs_mbuf_from_flat(pi->value.p, pi->value.len);
  ce->ind_in_flight = true;
  if (pi->is_ind) {
    rc = ble_gattc_indicate_custom(ce->gc.conn_id, pi->handle, om);
  } else {
    rc = ble_gattc_notify_custom(ce->gc.conn_id, pi->handle, om);
  }
  if (rc != 0) {
    ce->ind_in_flight = false;
    os_mbuf_free(om);
  }
}

int esp32_bt_gatts_event(const struct ble_gap_event *ev, void *arg) {
  int ret = 0;
  char buf1[MGOS_BT_UUID_STR_LEN], buf2[MGOS_BT_UUID_STR_LEN];
  LOG(LL_DEBUG,
      ("GATTS EV %d hf %d", ev->type, (int) mgos_get_free_heap_size()));
  mgos_rlock(s_lock);
  switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT: {
      uint16_t conn_id = ev->connect.conn_handle;
      struct ble_gap_conn_desc cd;
      ble_gap_conn_find(conn_id, &cd);
      LOG(LL_INFO, ("CONNECT %s ch %d st %d",
                    esp32_bt_addr_to_str(&cd.peer_ota_addr, buf1), conn_id,
                    ev->connect.status));
      if (ev->connect.status != 0) break;
      struct esp32_bt_gatts_connection_entry *ce = calloc(1, sizeof(*ce));
      if (ce == NULL) {
        ble_gap_terminate(conn_id, BLE_ERR_REM_USER_CONN_TERM);
        break;
      }
      ce->gc.conn_id = conn_id;
      ce->gc.mtu = ble_att_mtu(conn_id);
      esp32_bt_addr_to_mgos(&cd.peer_ota_addr, &ce->gc.addr);
      STAILQ_INIT(&ce->pending_inds);
      SLIST_INSERT_HEAD(&s_conns, ce, next);
      ble_gattc_exchange_mtu(conn_id, NULL, NULL);
      if (mgos_sys_config_get_bt_gatts_min_sec_level() > 0) {
        // ble_gap_security_initiate(conn_id);
      }
      break;
    }
    case BLE_GAP_EVENT_DISCONNECT: {
      const struct ble_gap_conn_desc *cd = &ev->disconnect.conn;
      uint16_t conn_id = cd->conn_handle;
      LOG(LL_INFO, ("DISCONNECT %s ch %d reason %d",
                    esp32_bt_addr_to_str(&cd->peer_ota_addr, buf1), conn_id,
                    ev->disconnect.reason));
      struct esp32_bt_gatts_connection_entry *ce = find_connection(conn_id);
      if (ce == NULL) break;
      while (!SLIST_EMPTY(&ce->sessions)) {
        struct esp32_bt_gatts_session_entry *sse = SLIST_FIRST(&ce->sessions);
        esp32_bt_gatts_close_session(sse);
      }
      struct esp32_bt_gatts_pending_ind *pi, *pit;
      STAILQ_FOREACH_SAFE(pi, &ce->pending_inds, next, pit) {
        mg_strfree(&pi->value);
        memset(pi, 0, sizeof(*pi));
        free(pi);
      }
      SLIST_REMOVE(&s_conns, ce, esp32_bt_gatts_connection_entry, next);
      free(ce);
      break;
    }
    case BLE_GAP_EVENT_ENC_CHANGE: {
      uint16_t conn_id = ev->enc_change.conn_handle;
      struct ble_gap_conn_desc cd = {0};
      ble_gap_conn_find(conn_id, &cd);
      struct ble_gap_sec_state *ss = &cd.sec_state;
      LOG(LL_DEBUG, ("ENC_CHANGE %s ch %d st %d e %d a %d b %d ks %d",
                     esp32_bt_addr_to_str(&cd.peer_ota_addr, buf1), conn_id,
                     ev->enc_change.status, ss->encrypted, ss->authenticated,
                     ss->bonded, ss->key_size));
      break;
    }
    case BLE_GAP_EVENT_MTU: {
      struct esp32_bt_gatts_connection_entry *ce =
          find_connection(ev->mtu.conn_handle);
      if (ce == NULL) break;
      uint16_t mtu = ev->mtu.value;
      LOG(LL_DEBUG,
          ("%s: MTU %d",
           mgos_bt_addr_to_str(&ce->gc.addr, MGOS_BT_ADDR_STRINGIFY_TYPE, buf1),
           mtu));
      ce->gc.mtu = 1024;  // mtu;
      esp32_bt_gatts_create_sessions(ce);
      break;
    }
    case BLE_GAP_EVENT_SUBSCRIBE: {
      uint16_t ch = ev->subscribe.conn_handle;
      uint16_t ah = ev->subscribe.attr_handle;
      struct esp32_bt_service_attr_info *ai = NULL;
      struct esp32_bt_gatts_session_entry *sse = find_session(ch, ah, &ai);
      if (sse == NULL) break;
      struct mgos_bt_gatts_notify_mode_arg narg = {
          .svc_uuid = sse->se->uuid,
          .char_uuid = ai->def.uuid_bin,
          .handle = ev->subscribe.attr_handle,
          .mode = MGOS_BT_GATT_NOTIFY_MODE_OFF,
      };
      if (ev->subscribe.cur_notify) {
        narg.mode = MGOS_BT_GATT_NOTIFY_MODE_NOTIFY;
      } else if (ev->subscribe.cur_indicate) {
        narg.mode = MGOS_BT_GATT_NOTIFY_MODE_INDICATE;
      }
      LOG(LL_DEBUG, ("NOTIFY_MODE c %d h %d %s/%s %d", ch, ah,
                     mgos_bt_uuid_to_str(&narg.svc_uuid, buf1),
                     mgos_bt_uuid_to_str(&narg.char_uuid, buf2), narg.mode));
      esp32_bt_gatts_call_handler(sse, ai, MGOS_BT_GATTS_EV_NOTIFY_MODE, &narg);
      break;
    }
    case BLE_GAP_EVENT_NOTIFY_TX: {
      uint16_t ch = ev->notify_tx.conn_handle;
      uint16_t ah = ev->notify_tx.attr_handle;
      struct esp32_bt_service_attr_info *ai = NULL;
      struct esp32_bt_gatts_session_entry *sse = find_session(ch, ah, &ai);
      if (sse == NULL) break;
      struct esp32_bt_gatts_connection_entry *ce = sse->ce;
      ce->ind_in_flight = false;
      LOG(LL_DEBUG,
          ("NOTIFY_TX ch %d ah %d st %d", ch, ah, ev->notify_tx.status));
      if (STAILQ_EMPTY(&ce->pending_inds)) break;  // Shouldn't happen.
      bool remove = false;
      struct esp32_bt_gatts_pending_ind *pi = STAILQ_FIRST(&ce->pending_inds);
      if (pi->is_ind) {
        // Indication raises this event twice: first with status 0 when
        // indication is sent, then again when it is acknowledged or times out.
        if (ev->notify_tx.status == 0) break;
        remove = (ev->notify_tx.status == BLE_HS_EDONE);
        struct mgos_bt_gatts_ind_confirm_arg ic_arg = {
            .handle = pi->handle,
            .ok = remove,
        };
        esp32_bt_gatts_call_handler(sse, ai, MGOS_BT_GATTS_EV_IND_CONFIRM,
                                    &ic_arg);
      } else {
        remove = (ev->notify_tx.status == 0);
      }
      if (remove) {
        STAILQ_REMOVE_HEAD(&ce->pending_inds, next);
        mg_strfree(&pi->value);
        ce->ind_queue_len--;
        free(pi);
      }
      // Send next one or retry sending this one that failed.
      esp32_bt_gatts_send_next_ind_locked(ce);
      break;
    }
  }
  mgos_runlock(s_lock);
  return ret;
}

bool mgos_bt_gatts_disconnect(struct mgos_bt_gatts_conn *gsc) {
  if (gsc == NULL) return false;
  return ble_gap_terminate(gsc->gc.conn_id, BLE_ERR_REM_USER_CONN_TERM) == 0;
}

static int esp32_gatts_get_att_err(enum mgos_bt_gatt_status st) {
  switch (st) {
    case MGOS_BT_GATT_STATUS_OK:
      return 0;
    case MGOS_BT_GATT_STATUS_INVALID_HANDLE:
      return BLE_ATT_ERR_INVALID_HANDLE;
    case MGOS_BT_GATT_STATUS_READ_NOT_PERMITTED:
      return BLE_ATT_ERR_READ_NOT_PERMITTED;
    case MGOS_BT_GATT_STATUS_WRITE_NOT_PERMITTED:
      return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    case MGOS_BT_GATT_STATUS_INSUF_AUTHENTICATION:
      return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    case MGOS_BT_GATT_STATUS_REQUEST_NOT_SUPPORTED:
      return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    case MGOS_BT_GATT_STATUS_INVALID_OFFSET:
      return BLE_ATT_ERR_INVALID_OFFSET;
    case MGOS_BT_GATT_STATUS_INSUF_AUTHORIZATION:
      return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    case MGOS_BT_GATT_STATUS_INVALID_ATT_VAL_LENGTH:
      return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    case MGOS_BT_GATT_STATUS_UNLIKELY_ERROR:
      return BLE_ATT_ERR_UNLIKELY;
    case MGOS_BT_GATT_STATUS_INSUF_RESOURCES:
      return BLE_ATT_ERR_INSUFFICIENT_RES;
  }
  return BLE_ATT_ERR_UNLIKELY;
}

static int esp32_gatts_attr_access_cb(uint16_t conn_handle,
                                      uint16_t attr_handle,
                                      struct ble_gatt_access_ctxt *ctxt,
                                      void *arg) {
  int res = 0;
  char buf[MGOS_BT_UUID_STR_LEN], buf2[MGOS_BT_UUID_STR_LEN];
  struct esp32_bt_service_attr_info *ai = arg;
  struct esp32_bt_gatts_session_entry *sse =
      find_session(conn_handle, attr_handle, &ai);
  const ble_uuid_t *uuid = NULL;
  LOG(LL_DEBUG, ("GATTS ATTR OP %d sse %p", ctxt->op, sse));
  if (sse == NULL) return BLE_ATT_ERR_UNLIKELY;
  switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
      uuid = ctxt->chr->uuid;
      // fallthrough
    case BLE_GATT_ACCESS_OP_READ_DSC: {
      if (uuid == NULL) {
        uuid = ctxt->dsc->uuid;
      }
      struct mgos_bt_gatts_read_arg rarg = {
          .svc_uuid = sse->se->uuid,
          .handle = attr_handle,
          .trans_id = 0,
          .offset = 0,
      };
      esp32_bt_uuid_to_mgos(uuid, &rarg.char_uuid);
      enum mgos_bt_gatt_status st =
          esp32_bt_gatts_call_handler(sse, ai, MGOS_BT_GATTS_EV_READ, &rarg);
      LOG(LL_DEBUG,
          ("READ %s ch %d ah %u (%s) -> %d %d",
           mgos_bt_addr_to_str(&sse->gsc.gc.addr, 0, buf), sse->gsc.gc.conn_id,
           rarg.handle, mgos_bt_uuid_to_str(&rarg.char_uuid, buf2), st,
           (int) sse->resp_data.len));
      res = esp32_gatts_get_att_err(st);
      if (res == 0 && sse->resp_data.len > 0) {
        os_mbuf_append(ctxt->om, sse->resp_data.buf, sse->resp_data.len);
        mbuf_clear(&sse->resp_data);
      }
      break;
    }
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
      uuid = ctxt->chr->uuid;
      // fallthrough
    case BLE_GATT_ACCESS_OP_WRITE_DSC: {
      if (uuid == NULL) {
        uuid = ctxt->dsc->uuid;
      }
      char *data = NULL;
      uint16_t data_len = OS_MBUF_PKTLEN(ctxt->om);
      if (data_len > 0) {
        data = malloc(data_len);
        if (data == NULL) {
          res = BLE_ATT_ERR_UNLIKELY;
          break;
        }
        ble_hs_mbuf_to_flat(ctxt->om, data, data_len, &data_len);
      }
      struct mgos_bt_gatts_write_arg warg = {
          .svc_uuid = sse->se->uuid,
          .char_uuid = ai->def.uuid_bin,
          .handle = attr_handle,
          .data = mg_mk_str_n(data, data_len),
          .trans_id = 0,
          .offset = 0,
          .need_rsp = true,
      };
      enum mgos_bt_gatt_status st =
          esp32_bt_gatts_call_handler(sse, ai, MGOS_BT_GATTS_EV_WRITE, &warg);
      LOG(LL_DEBUG,
          ("WRITE %s ch %d ah %u (%s) len %d -> %d",
           mgos_bt_addr_to_str(&sse->gsc.gc.addr, 0, buf), sse->gsc.gc.conn_id,
           warg.handle, mgos_bt_uuid_to_str(&warg.char_uuid, buf2),
           (int) warg.data.len, st));
      res = esp32_gatts_get_att_err(st);
      free(data);
      break;
    }
  }
  return res;
}

static int esp32_bt_register_service(struct esp32_bt_gatts_service_entry *se) {
  int rc = 0;
  rc = ble_gatts_count_cfg(&se->ble_svc_def[0]);
  if (rc != 0) {
    LOG(LL_INFO, ("Count failed"));
    return rc;
  }
  rc = ble_gatts_add_svcs(&se->ble_svc_def[0]);
  if (rc != 0) {
    LOG(LL_INFO, ("Add failed"));
    return rc;
  }
  return rc;
}

static void esp32_bt_register_services(void) {
  char buf[MGOS_BT_UUID_STR_LEN];
  struct esp32_bt_gatts_service_entry *se;
  SLIST_FOREACH(se, &s_svcs, next) {
    int rc = esp32_bt_register_service(se);
    if (rc != 0) {
      LOG(LL_ERROR, ("Failed to register BT service %s: %d",
                     mgos_bt_uuid_to_str(&se->uuid, buf), rc));
      continue;
    }
  }
}

bool mgos_bt_gatts_register_service(const char *svc_uuid,
                                    enum mgos_bt_gatt_sec_level sec_level,
                                    const struct mgos_bt_gatts_char_def *chars,
                                    mgos_bt_gatts_ev_handler_t handler,
                                    void *handler_arg) {
  bool res = false;
  struct esp32_bt_gatts_service_entry *se = calloc(1, sizeof(*se));
  if (se == NULL) goto out;
  if (!mgos_bt_uuid_from_str(mg_mk_str(svc_uuid), &se->uuid)) {
    LOG(LL_ERROR, ("%s: Invalid svc UUID", svc_uuid));
    goto out;
  }
  se->handler = handler;
  se->handler_arg = handler_arg;
  se->sec_level = sec_level;
  const struct mgos_bt_gatts_char_def *cd = NULL;
  // Count the number of attrs required.
  int num_chars = 0, num_descrs = 0;
  for (cd = chars; cd->uuid != NULL; cd++) {
    se->num_attrs++;
    if (!cd->is_desc) {
      num_chars++;
    } else if (cd != chars) {
      num_descrs++;
      // Add 1 desc entry per characteristic - for end marker.
      if (!(cd - 1)->is_desc) num_descrs++;
    } else {
      // Descriptors must always follow a characteristic definition
      // so a descriptor cannot be the first item in the list.
      goto out;
    }
  }
  if (num_chars == 0) goto out;
  se->attrs = calloc(se->num_attrs, sizeof(*se->attrs));
  if (se->attrs == NULL) goto out;
  se->ble_chars = calloc(num_chars + 1, sizeof(*se->ble_chars));
  if (se->ble_chars == NULL) goto out;
  if (num_descrs > 0) {
    se->ble_descrs = calloc(num_descrs + 1, sizeof(*se->ble_descrs));
    if (se->ble_descrs == NULL) goto out;
  }

  struct esp32_bt_service_attr_info *ai = se->attrs;
  struct ble_gatt_chr_def *bchr = se->ble_chars;
  struct ble_gatt_dsc_def *bdsc = se->ble_descrs;
  for (cd = chars; cd->uuid != NULL; cd++, ai++) {
    ai->def = *cd;
    if (!cd->is_uuid_bin &&
        !mgos_bt_uuid_from_str(mg_mk_str(cd->uuid), &ai->def.uuid_bin)) {
      LOG(LL_ERROR, ("%s: %s: invalid char UUID", svc_uuid, cd->uuid));
      goto out;
    }
    ai->def.is_uuid_bin = true;
    mgos_bt_uuid_to_esp32(&ai->def.uuid_bin, &ai->ble_uuid);
    if (!cd->is_desc) {
      bchr->uuid = &ai->ble_uuid.u;
      bchr->access_cb = esp32_gatts_attr_access_cb;
      bchr->arg = ai;
      bchr->val_handle = &ai->handle;
      uint16_t pp = cd->prop, ff = 0;
      if (pp & MGOS_BT_GATT_PROP_READ) ff |= get_read_perm(se->sec_level);
      if (pp & MGOS_BT_GATT_PROP_WRITE) ff |= get_write_perm(se->sec_level);
      if (pp & MGOS_BT_GATT_PROP_NOTIFY) ff |= BLE_GATT_CHR_F_NOTIFY;
      if (pp & MGOS_BT_GATT_PROP_INDICATE) ff |= BLE_GATT_CHR_F_INDICATE;
      if (pp & MGOS_BT_GATT_PROP_WRITE_NR) ff |= BLE_GATT_CHR_PROP_WRITE_NO_RSP;
      bchr->flags = ff;
      if ((ff & (BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE_ENC)) != 0) {
        bchr->min_key_size = 16;
      }
      bchr++;
      if (cd != chars && (cd - 1)->is_desc) bdsc++;
    } else {
      bdsc->uuid = &ai->ble_uuid.u;
      bdsc->min_key_size = 0;  // TODO
      bdsc->access_cb = esp32_gatts_attr_access_cb;
      bdsc->arg = ai;
      uint8_t pp = cd->prop, af = 0;
      if (pp & MGOS_BT_GATT_PROP_READ) af |= BLE_ATT_F_READ;
      if (pp & MGOS_BT_GATT_PROP_WRITE) af |= BLE_ATT_F_WRITE;
      bdsc->att_flags = af;
      if (!(cd - 1)->is_desc) (bchr - 1)->descriptors = bdsc;
      bdsc++;
    }
  }

  mgos_bt_uuid_to_esp32(&se->uuid, &se->ble_uuid);
  struct ble_gatt_svc_def *bsvc = &se->ble_svc_def[0];
  bsvc->type = BLE_GATT_SVC_TYPE_PRIMARY;
  bsvc->uuid = &se->ble_uuid.u;
  bsvc->characteristics = se->ble_chars;
  se->ble_svc_def[1].type = BLE_GATT_SVC_TYPE_END;

  mgos_rlock(s_lock);
  SLIST_INSERT_HEAD(&s_svcs, se, next);
  mgos_runlock(s_lock);
  // TODO: restart if needed.
  res = true;
out:
  if (!res && se != NULL) {
    free(se->attrs);
    free(se->ble_chars);
    free(se->ble_descrs);
    free(se);
  }
  LOG(LL_INFO, ("REG %d", res));
  return res;
}

#if 0
bool mgos_bt_gatts_unregister_service(const char *uuid_str) {
  struct esp32_bt_gatts_service_entry *se;
  struct mgos_bt_uuid uuid;
  if (!mgos_bt_uuid_from_str(mg_mk_str(uuid_str), &uuid)) return false;
  SLIST_FOREACH(se, &s_svcs, next) {
    if (mgos_bt_uuid_eq(&se->uuid, &uuid)) break;
  }
  if (se == NULL) return false;
  // Remove from the list immediately in case it's re-registered.
  SLIST_REMOVE(&s_svcs, se, esp32_bt_gatts_service_entry, next);
  se->deleting = true;
  // Close the associated sessions;
  struct esp32_bt_gatts_connection_entry *ce;
  SLIST_FOREACH(ce, &s_conns, next) {
    struct esp32_bt_gatts_session_entry *sse;
    SLIST_FOREACH(sse, &ce->sessions, next) {
      if (sse->se == se) break;
    }
    if (sse == NULL) continue;
    esp32_bt_gatts_close_session(sse);
  }
  free(se->attrs);
  free(se);
  // Stop and then delete the service.
  return (esp_ble_gatts_stop_service(se->svc_handle) == ESP_OK);
}

static void esp32_bt_gatts_send_resp(struct mgos_bt_gatts_conn *gsc,
                                     uint16_t handle, uint32_t trans_id,
                                     enum mgos_bt_gatt_status st) {
  struct esp32_bt_gatts_session_entry *sse = find_session_by_gsc(gsc);
  if (sse == NULL) return;
  esp_gatt_status_t est = esp32_bt_gatt_get_status(st);
  esp_gatt_rsp_t rsp = {.handle = handle};
  LOG(LL_DEBUG, ("h %u tid %u st %d est %d", handle, trans_id, st, est));
  esp_ble_gatts_send_response(s_gatts_if, gsc->gc.conn_id, trans_id, est, &rsp);
}
#endif

void mgos_bt_gatts_send_resp_data(struct mgos_bt_gatts_conn *gsc,
                                  struct mgos_bt_gatts_read_arg *ra,
                                  struct mg_str data) {
  struct esp32_bt_gatts_session_entry *sse = find_session_by_gsc(gsc);
  if (sse == NULL) return;
  mbuf_append(&sse->resp_data, data.p, data.len);
}

void mgos_bt_gatts_notify(struct mgos_bt_gatts_conn *gsc,
                          enum mgos_bt_gatt_notify_mode mode, uint16_t handle,
                          struct mg_str data) {
  if (mode == MGOS_BT_GATT_NOTIFY_MODE_OFF) return;
  struct esp32_bt_gatts_session_entry *sse = find_session_by_gsc(gsc);
  if (sse == NULL) return;
  struct esp32_bt_gatts_pending_ind *pi = calloc(1, sizeof(*pi));
  if (pi == NULL) return;
  pi->handle = handle;
  pi->is_ind = (mode == MGOS_BT_GATT_NOTIFY_MODE_INDICATE);
  pi->value = mg_strdup(data);
  mgos_rlock(s_lock);
  STAILQ_INSERT_TAIL(&sse->ce->pending_inds, pi, next);
  sse->ce->ind_queue_len++;
  esp32_bt_gatts_send_next_ind_locked(sse->ce);
  mgos_runlock(s_lock);
}

void mgos_bt_gatts_notify_uuid(struct mgos_bt_gatts_conn *gsc,
                               const struct mgos_bt_uuid *char_uuid,
                               enum mgos_bt_gatt_notify_mode mode,
                               struct mg_str data) {
  struct esp32_bt_gatts_session_entry *sse = find_session_by_gsc(gsc);
  if (sse == NULL) return;
  struct esp32_bt_gatts_service_entry *se = sse->se;
  for (uint16_t i = 0; i < se->num_attrs; i++) {
    struct esp32_bt_service_attr_info *ai = &se->attrs[i];
    if (mgos_bt_uuid_eq(&ai->def.uuid_bin, char_uuid)) {
      mgos_bt_gatts_notify(gsc, mode, ai->handle, data);
      return;
    }
  }
}

bool esp32_bt_gatts_init(void) {
  LOG(LL_INFO, ("GATTS init, synced? %d", ble_hs_synced()));
  ble_hs_cfg.gatts_register_cb = esp32_gatts_register_cb;
  if (s_lock == NULL) {
    s_lock = mgos_rlock_create();
  }
  esp32_bt_register_services();
  return true;
}
