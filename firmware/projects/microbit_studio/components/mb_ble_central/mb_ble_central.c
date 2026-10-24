// SPDX-License-Identifier: Apache-2.0
//
// Gate G2 probe: connect to a micro:bit v1 over BLE, subscribe to the
// partial-flash characteristic, write 0xEE and log the STATUS answer.
//
// Everything protocol-shaped here comes from docs/partial-flash-protocol.md:
// service/characteristic UUIDs, the unsecured characteristic, and the 20-byte
// write payload that fits the default ATT MTU. The NimBLE call sequence follows
// the blecent example shipped with this IDF (/opt/esp/idf/examples/bluetooth/
// nimble/blecent/main/main.c), which is why the raw ble_gattc_* calls are used
// instead of a GATT cache.

#include "mb_ble_central.h"

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "mb_fixture.h"
#include "mb_flash.h"
#include "mb_mem_probe.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "mb_ble";

/* docs/partial-flash-protocol.md "GATT surface". Service first, then its
 * characteristics, then descriptors inside the service's handle range: NimBLE's
 * struct ble_gatt_chr carries no end handle (ble_gatt.h:471-479), so a
 * descriptor range can only come from the service. */
static const ble_uuid128_t k_partial_flash_svc = BLE_UUID128_INIT(
    0xa8, 0xa9, 0xdf, 0x22, 0x19, 0xfa, 0x62, 0xa0,
    0x0a, 0x47, 0x1d, 0x25, 0x1d, 0xd9, 0x7d, 0xe9);
static const ble_uuid128_t k_partial_flash_chr = BLE_UUID128_INIT(
    0xa8, 0xa9, 0xdf, 0x22, 0x19, 0xfa, 0x62, 0xa0,
    0x0a, 0x47, 0x1d, 0x25, 0x10, 0x3b, 0x7d, 0xe9);

/* The advertising name is "BBC micro:bit [abcde]"; the service UUID does not
 * fit in the 31-byte advertising payload, so the name is the only handle. */
static const char k_name_prefix[] = "BBC micro:bit";

#define MB_STATUS_OPCODE 0xEE
#define MB_SCAN_FOREVER BLE_HS_FOREVER /* in 30 ms units */
#define MB_CONNECT_TIMEOUT_MS 30000

static uint8_t s_own_addr_type;
static uint16_t s_round;             /* completed probe rounds, for the log */
static uint16_t s_chr_val_handle;    /* discovered value handle, 0 = none */
static uint16_t s_svc_end_handle;    /* end of the partial-flash service */
static bool s_cccd_pending;          /* subscribe write started for this round */

/* ---- gate G3: the partial-flash transfer -------------------------------
 *
 * Driven from the notify callback, one 64-byte block at a time: the device
 * notifies once per block, so the acknowledgement *is* the pacing. No task, no
 * semaphore, no timer — a block is sent only after the previous one landed.
 */
enum mb_flash_phase {
    MB_PHASE_IDLE = 0,
    MB_PHASE_REGION_DAL,  /* asked for region 1 hashes */
    MB_PHASE_REGION_PXT,  /* asked for region 2 hashes */
    MB_PHASE_STREAM,      /* sending blocks */
    MB_PHASE_END,         /* END_OF_TX sent, waiting for the device to reset */
};

static enum mb_flash_phase s_phase;
static uint16_t s_conn_handle;
static uint32_t s_block;    /* index of the next block to send */
static uint32_t s_packet;   /* transfer-wide packet counter, per the DAL */
static uint32_t s_resyncs;
static bool s_flashed;      /* a transfer completed on this boot */
static bool s_flash_done;   /* stop flashing, fall back to the STATUS probe */

void ble_store_config_init(void); /* provided by the NimBLE port */

static void mb_ble_scan(void);

/* ---- gate G3: transfer helpers ---------------------------------------- */

static void mb_ble_send_packet(const uint8_t *packet)
{
    const int rc = ble_gattc_write_no_rsp_flat(s_conn_handle, s_chr_val_handle, packet,
                                               MB_FLASH_PACKET_BYTES);
    if (rc != 0) {
        ESP_LOGE(TAG, "packet write failed rc=%d", rc);
    }
}

static void mb_ble_send_region_info(uint8_t region)
{
    uint8_t packet[MB_FLASH_PACKET_BYTES];
    mb_flash_build_region_info(packet, region);
    mb_ble_send_packet(packet);
}

static void mb_ble_send_end_of_tx(void)
{
    uint8_t packet[MB_FLASH_PACKET_BYTES];
    const uint32_t last = MB_FIXTURE_REGION2_START +
                          (MB_FIXTURE_BLOCK_COUNT - 1u) * MB_FLASH_BLOCK_BYTES;
    mb_flash_build_end_of_tx(packet, last);
    mb_ble_send_packet(packet);
}

static void mb_ble_send_block(uint32_t index)
{
    if ((index + 1u) * MB_FLASH_BLOCK_BYTES > mb_flash_fixture_len()) {
        ESP_LOGE(TAG, "block %u is past the %u-byte fixture", (unsigned)index,
                 (unsigned)mb_flash_fixture_len());
        return;
    }

    uint8_t packets[MB_FLASH_PACKETS_PER_BLOCK][MB_FLASH_PACKET_BYTES];
    const uint32_t offset = index * MB_FLASH_BLOCK_BYTES;
    mb_flash_build_block(packets, MB_FIXTURE_REGION2_START + offset,
                         mb_flash_fixture_data() + offset);

    for (uint32_t i = 0; i < MB_FLASH_PACKETS_PER_BLOCK; i++) {
        /* The DAL compares data[3] with a counter that runs across the whole
         * transfer (packetCount++) instead of restarting per block, so stamping
         * the running value is what stops the device answering 0xAA on block 2. */
        packets[i][3] = (uint8_t)(s_packet + i);
        mb_ble_send_packet(packets[i]);
    }
    s_packet += MB_FLASH_PACKETS_PER_BLOCK;
}

static void mb_ble_region_answered(const mb_flash_notify_t *note)
{
    static const uint8_t k_template[8] = MB_FIXTURE_TEMPLATE_HASH_INIT;
    static const uint8_t k_program[8] = MB_FIXTURE_PROGRAM_HASH_INIT;

    if (note->region == MB_FLASH_REGION_DAL) {
        ESP_LOGI(TAG, "region 1 (DAL) hash %02X%02X%02X%02X%02X%02X%02X%02X", note->hash[0],
                 note->hash[1], note->hash[2], note->hash[3], note->hash[4], note->hash[5],
                 note->hash[6], note->hash[7]);
        if (memcmp(note->hash, k_template, sizeof(k_template)) != 0) {
            /* The device refuses a mismatched DAL anyway; refusing here keeps the
             * failure at "we did not write" instead of a half-transfer. */
            ESP_LOGE(TAG, "DAL hash differs from our image: refusing to write");
            s_flash_done = true;
            ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return;
        }
        ESP_LOGI(TAG, "DAL matches our template hash");
        s_phase = MB_PHASE_REGION_PXT;
        mb_ble_send_region_info(MB_FLASH_REGION_PXT);
        return;
    }

    if (note->region != MB_FLASH_REGION_PXT) {
        ESP_LOGW(TAG, "region %u is not one we asked for", note->region);
        return;
    }

    if (memcmp(note->hash, k_program, sizeof(k_program)) == 0) {
        if (s_flashed) {
            ESP_LOGI(TAG, "VERIFIED: region 2 now holds our program "
                          "(0x%05X-0x%05X)", MB_FIXTURE_REGION2_START,
                     MB_FIXTURE_REGION2_END - 1u);
        } else {
            ESP_LOGI(TAG, "region 2 already holds our program; nothing to flash");
        }
        s_flash_done = true;
        s_round++;
        ESP_LOGI(TAG, "round %u complete, disconnecting", s_round);
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }

    ESP_LOGI(TAG, "region 2 differs; streaming %u blocks from 0x%05X",
             (unsigned)MB_FIXTURE_BLOCK_COUNT, MB_FIXTURE_REGION2_START);
    s_phase = MB_PHASE_STREAM;
    s_block = 0;
    s_packet = 0;
    mb_ble_send_block(0);
}

static void mb_ble_handle_notify(const struct os_mbuf *om)
{
    uint8_t buf[MB_FLASH_REGION_NOTIFY_BYTES];
    const uint16_t len = OS_MBUF_PKTLEN(om);
    const uint16_t copied = len < sizeof(buf) ? len : (uint16_t)sizeof(buf);

    if (os_mbuf_copydata(om, 0, copied, buf) != 0) {
        ESP_LOGE(TAG, "notify: could not copy %u bytes", len);
        return;
    }

    mb_flash_notify_t note;
    switch (mb_flash_parse_notify(buf, copied, &note)) {
    case MB_FLASH_NOTIFY_WRITE_ACK:
        if (s_phase != MB_PHASE_STREAM) {
            return;
        }
        s_block++;
        if (s_block < MB_FIXTURE_BLOCK_COUNT) {
            if ((s_block % 16u) == 0) {
                ESP_LOGI(TAG, "flashing %u/%u blocks", (unsigned)s_block,
                         (unsigned)MB_FIXTURE_BLOCK_COUNT);
            }
            mb_ble_send_block(s_block);
        } else {
            ESP_LOGI(TAG, "all %u blocks acknowledged (%u resyncs); END_OF_TX",
                     (unsigned)MB_FIXTURE_BLOCK_COUNT, (unsigned)s_resyncs);
            s_phase = MB_PHASE_END;
            mb_ble_send_end_of_tx();
        }
        return;

    case MB_FLASH_NOTIFY_WRITE_RESYNC:
        if (s_phase != MB_PHASE_STREAM) {
            ESP_LOGW(TAG, "resync outside a transfer; ignoring");
            return;
        }
        s_resyncs++;
        s_block++;
        ESP_LOGW(TAG, "device reported out-of-order; skipping to block %u",
                 (unsigned)s_block);
        if (s_resyncs > 4 || s_block >= MB_FIXTURE_BLOCK_COUNT) {
            ESP_LOGE(TAG, "transfer abandoned after %u resyncs", (unsigned)s_resyncs);
            s_flash_done = true;
            ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return;
        }
        /* The device set its counter to the next block boundary; match it. */
        s_packet = s_block * MB_FLASH_PACKETS_PER_BLOCK;
        mb_ble_send_block(s_block);
        return;

    case MB_FLASH_NOTIFY_REGION:
        mb_ble_region_answered(&note);
        return;

    case MB_FLASH_NOTIFY_STATUS:
        ESP_LOGI(TAG, "STATUS pfs_version=%u mode=%u (%s)", note.pfs_version, note.mode,
                 note.mode == 1 ? "application" : "pairing");
        s_round++;
        ESP_LOGI(TAG, "round %u complete, disconnecting", s_round);
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;

    default:
        ESP_LOGW(TAG, "unparsed notification: len=%u first=%02X", copied, buf[0]);
        return;
    }
}

static int mb_ble_on_write_status(uint16_t conn_handle, const struct ble_gatt_error *error,
                                  struct ble_gatt_attr *attr, void *arg)
{
    (void)attr;
    (void)arg;

    if (error->status != 0) {
        ESP_LOGE(TAG, "STATUS write failed: status=%d", error->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }
    ESP_LOGI(TAG, "STATUS write acknowledged (0xEE)");
    return 0;
}

static int mb_ble_on_cccd_write(uint16_t conn_handle, const struct ble_gatt_error *error,
                                struct ble_gatt_attr *attr, void *arg)
{
    (void)attr;
    (void)arg;

    if (error->status != 0) {
        ESP_LOGE(TAG, "CCCD write failed: status=%d", error->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    if (!s_flash_done) {
        /* Gate G3: ask what the device already holds before writing anything. */
        ESP_LOGI(TAG, "subscribed; reading region 1 hash before any write");
        s_phase = MB_PHASE_REGION_DAL;
        mb_ble_send_region_info(MB_FLASH_REGION_DAL);
        return 0;
    }

    /* Steady state (gate G2): one byte, the STATUS opcode carries no payload. */
    const uint8_t opcode = MB_FLASH_CMD_STATUS;
    const int rc = ble_gattc_write_flat(conn_handle, s_chr_val_handle, &opcode, sizeof(opcode),
                                        mb_ble_on_write_status, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "write 0xEE failed to start: rc=%d", rc);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static int mb_ble_on_dsc_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                              uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)arg;
    (void)chr_val_handle; /* ble_gatt_dsc_fn signature; we already know the characteristic */

    switch (error->status) {
    case 0:
        if (dsc != NULL && ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) {
            /* Subscribe: notifications on (0x0001, little-endian). */
            const uint8_t cccd[2] = {1, 0};
            s_cccd_pending = true;
            const int rc = ble_gattc_write_flat(conn_handle, dsc->handle, cccd, sizeof(cccd),
                                                mb_ble_on_cccd_write, NULL);
            if (rc != 0) {
                s_cccd_pending = false;
                ESP_LOGE(TAG, "CCCD subscribe failed to start: rc=%d", rc);
                ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
        }
        return 0;
    case BLE_HS_EDONE:
        /* EDONE also follows a successful walk, so only complain when no CCCD
         * was seen at all — a started subscribe completes on its own callback. */
        if (!s_cccd_pending) {
            ESP_LOGW(TAG, "no CCCD under the partial-flash characteristic; cannot subscribe");
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;
    default:
        ESP_LOGE(TAG, "descriptor discovery failed: status=%d", error->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }
}

static int mb_ble_on_chr_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                              const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;

    switch (error->status) {
    case 0:
        if (chr != NULL && ble_uuid_cmp(&chr->uuid.u, &k_partial_flash_chr.u) == 0) {
            s_chr_val_handle = chr->val_handle;
        }
        return 0;
    case BLE_HS_EDONE:
        if (s_chr_val_handle == 0) {
            ESP_LOGE(TAG, "partial-flash characteristic not found in the service");
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        ESP_LOGI(TAG, "characteristic found: val_handle=%u", s_chr_val_handle);
        /* Descriptors live between the value handle and the end of the service. */
        if (ble_gattc_disc_all_dscs(conn_handle, s_chr_val_handle, s_svc_end_handle,
                                    mb_ble_on_dsc_disc, NULL) != 0) {
            ESP_LOGE(TAG, "descriptor discovery failed to start");
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;
    default:
        ESP_LOGE(TAG, "characteristic discovery failed: status=%d", error->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }
}

static int mb_ble_on_svc_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                              const struct ble_gatt_svc *service, void *arg)
{
    (void)arg;

    switch (error->status) {
    case 0:
        if (service != NULL) {
            s_svc_end_handle = service->end_handle;
            ESP_LOGI(TAG, "service found: handles %u..%u", service->start_handle,
                     service->end_handle);
        }
        return 0;
    case BLE_HS_EDONE:
        if (s_svc_end_handle == 0) {
            ESP_LOGE(TAG, "partial-flash service not found on the peer");
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        if (ble_gattc_disc_all_chrs(conn_handle, 1, s_svc_end_handle, mb_ble_on_chr_disc,
                                    NULL) != 0) {
            ESP_LOGE(TAG, "characteristic discovery failed to start");
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;
    default:
        ESP_LOGE(TAG, "service discovery failed: status=%d", error->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }
}

static int mb_ble_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) != 0) {
            return 0;
        }
        if (fields.name == NULL || fields.name_len < sizeof(k_name_prefix) - 1 ||
            memcmp(fields.name, k_name_prefix, sizeof(k_name_prefix) - 1) != 0) {
            return 0;
        }
        ESP_LOGI(TAG, "found peer '%.*s'", fields.name_len, (const char *)fields.name);

        /* Scanning must stop before connecting. */
        if (ble_gap_disc_cancel() != 0) {
            return 0;
        }
        if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
            ESP_LOGE(TAG, "could not infer our address type");
            return 0;
        }
        const int rc = ble_gap_connect(s_own_addr_type, &event->disc.addr, MB_CONNECT_TIMEOUT_MS,
                                       NULL, mb_ble_gap_event, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "connect failed to start: rc=%d", rc);
            mb_ble_scan();
        }
        return 0;
    }

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGE(TAG, "connection failed: status=%d", event->connect.status);
            mb_ble_scan();
            return 0;
        }
        ESP_LOGI(TAG, "connected: conn_handle=%u mtu=%u", event->connect.conn_handle,
                 ble_att_mtu(event->connect.conn_handle));
        s_chr_val_handle = 0;
        s_svc_end_handle = 0;
        s_cccd_pending = false;
        s_conn_handle = event->connect.conn_handle;
        s_phase = MB_PHASE_IDLE;
        s_block = 0;
        s_packet = 0;
        s_resyncs = 0;
        if (ble_gattc_disc_svc_by_uuid(event->connect.conn_handle, &k_partial_flash_svc.u,
                                       mb_ble_on_svc_disc, NULL) != 0) {
            ESP_LOGE(TAG, "service discovery failed to start");
            ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX:
        mb_ble_handle_notify(event->notify_rx.om);
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "disconnected: reason=%d", event->disconnect.reason);
        vTaskDelay(pdMS_TO_TICKS(1000));
        mb_ble_scan();
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu update: conn_handle=%u mtu=%u", event->mtu.conn_handle,
                 event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGW(TAG, "scan complete without a match: reason=%d", event->disc_complete.reason);
        return 0;

    default:
        return 0;
    }
}

static void mb_ble_scan(void)
{
    struct ble_gap_disc_params params = {0};
    params.passive = 0;
    params.filter_duplicates = 1;

    const int rc = ble_gap_disc(s_own_addr_type, MB_SCAN_FOREVER, &params, mb_ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "scan failed to start: rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "scanning for '%s…'", k_name_prefix);
    }
}

static void mb_ble_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "no usable BLE address: rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "address inference failed: rc=%d", rc);
        return;
    }
    /* The AC asks for internal RAM with the stack up and running. */
    mb_mem_probe_stage("ble-up");
    mb_ble_scan();
}

static void mb_ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset: reason=%d", reason);
}

static void mb_ble_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static bool s_started;

static void mb_ble_task(void *param)
{
    (void)param;

    const esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    ble_hs_cfg.reset_cb = mb_ble_on_reset;
    ble_hs_cfg.sync_cb = mb_ble_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* No GAP service: this device is a central only and never advertises, so
     * ble_svc_gap_device_name_set() is neither needed nor linked. */
    ble_store_config_init();
    nimble_port_freertos_init(mb_ble_host_task);

    vTaskDelete(NULL);
}

esp_err_t mb_ble_central_start(void)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    /* The host needs its own stack; the caller's task is the GSP/UI owner and
     * must stay free to service the panel. */
    const BaseType_t created = xTaskCreate(mb_ble_task, "mb_ble", 6144, NULL, 5, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "could not create the BLE task");
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}
