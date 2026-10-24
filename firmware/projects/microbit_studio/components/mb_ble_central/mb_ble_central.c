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
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
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
static bool s_connected;             /* a link is up; read by the tx task */
static volatile uint32_t s_last_progress_ms; /* last time the BLE path moved */
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
static bool s_flash_done;   /* stop flashing, fall back to the STATUS probe */

/* ---- gate G4: the cycle driver -----------------------------------------
 *
 * The image is fetched once from POST /v1/build?region2=true and flashed once
 * per cycle. Every cycle writes: a cycle that only re-read a matching hash
 * would prove nothing about what the gate is about — a flash that needs no A+B
 * tap afterwards and no power cycle in between.
 *
 *   connect → region 1 → region 2 → stream N blocks → END_OF_TX
 *   (device resets, link drops) → reconnect → region 1 → region 2 == program?
 *        yes → cycle verified → next cycle, or POC DONE
 *        no  → FAILED, screen shows E3
 */
static const mb_flash_image_t *s_image;
static mb_flash_status_fn s_status;
static uint32_t s_cycle;        /* cycles verified so far */
static bool s_await_verify;     /* a transfer landed; this connection verifies it */
static bool s_peer_seen;        /* a micro:bit answered at least once */

static void mb_ble_status(const char *text)
{
    if (s_status != NULL) {
        s_status(text);
    }
}

static void mb_ble_fail(const char *state, const char *why)
{
    ESP_LOGE(TAG, "FAILED: %s", why);
    mb_ble_status(state);
    s_flash_done = true;
    ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
}

void ble_store_config_init(void); /* provided by the NimBLE port */

static void mb_ble_scan(void);

/* The micro:bit resets itself after END_OF_TX, and its partial-flash service
 * rejects a write that arrives before the DAL has settled: cycle 2 of every run
 * failed 2.6 s in with status=7 (invalid attribute value length), while cycle 1
 * — against a board that had been up for 11 minutes — streamed all 96 blocks.
 * So the next scan waits CONFIG_MB_BLE_CYCLE_GAP_MS instead of racing the boot.
 *
 * A timer, not a delay: this is called from the GAP and GATT callbacks, which
 * run on the NimBLE host task, and sleeping there stops HCI processing for the
 * whole stack. */
static esp_timer_handle_t s_scan_timer;

static void mb_ble_scan_timer_cb(void *arg)
{
    (void)arg;
    mb_ble_scan();
}

static void mb_ble_scan_soon(uint32_t delay_ms)
{
    if (s_scan_timer == NULL) {
        mb_ble_scan();
        return;
    }
    (void)esp_timer_stop(s_scan_timer);
    if (esp_timer_start_once(s_scan_timer, (uint64_t)delay_ms * 1000u) != ESP_OK) {
        mb_ble_scan();
    }
}

/* A task-watchdog reset leaves no panic and no coredump, so the interesting
 * 5 s — where the BLE path stops moving — is invisible after the fact. This
 * detector names the stall while it is happening. */
static void mb_ble_mark_progress(void)
{
    s_last_progress_ms = (uint32_t)esp_log_timestamp();
}

static void mb_ble_watch_task(void *arg)
{
    (void)arg;

    mb_ble_mark_progress();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        const uint32_t gap = (uint32_t)esp_log_timestamp() - s_last_progress_ms;
        if (gap > 6000u) {
            ESP_LOGW(TAG, "no progress for %u ms: phase=%d block=%u packet=%u connected=%d",
                     (unsigned)gap, (int)s_phase, (unsigned)s_block, (unsigned)s_packet,
                     (int)s_connected);
            mb_ble_mark_progress();
        }
        /* "No state at all" is not a state the panel can show: if no micro:bit
         * ever answered, say so once rather than scanning forever in silence. */
        if (!s_peer_seen &&
            esp_log_timestamp() > (uint32_t)CONFIG_MB_BLE_PEER_TIMEOUT_S * 1000u) {
            ESP_LOGE(TAG, "no micro:bit within %d s; check the board is advertising",
                     CONFIG_MB_BLE_PEER_TIMEOUT_S);
            mb_ble_status("E4");
            s_peer_seen = true; /* report once */
        }
    }
}

/* ---- gate G3: transfer helpers ---------------------------------------- */

/* All ATT writes run on a dedicated task. NimBLE callbacks only enqueue:
 * starting a GATT procedure inside a GATT callback re-enters the client state
 * machine, and that corruption surfaces as an interrupt-watchdog stall. */
typedef struct {
    uint8_t count;
    uint8_t data[MB_FLASH_PACKETS_PER_BLOCK][MB_FLASH_PACKET_BYTES];
} mb_tx_batch_t;

static uint8_t s_tx[MB_FLASH_PACKETS_PER_BLOCK][MB_FLASH_PACKET_BYTES];
static QueueHandle_t s_txq;
static SemaphoreHandle_t s_tx_done;
static volatile int s_write_error; /* last ATT error from a packet write, 0 = none */

static int mb_ble_on_packet_write(uint16_t conn_handle, const struct ble_gatt_error *error,
                                  struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle;
    (void)attr;
    (void)arg;

    if (error->status != 0) {
        ESP_LOGE(TAG, "packet write rejected: status=%d", error->status);
        s_write_error = error->status;
    }
    mb_ble_mark_progress();
    xSemaphoreGive(s_tx_done);
    return 0;
}

static void mb_ble_tx_task(void *arg)
{
    (void)arg;
    mb_tx_batch_t batch;

    for (;;) {
        if (xQueueReceive(s_txq, &batch, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        for (uint8_t i = 0; i < batch.count; i++) {
            if (!s_connected) {
                break;
            }
            (void)xSemaphoreTake(s_tx_done, 0);
            s_write_error = 0;
            const int rc = ble_gattc_write_flat(s_conn_handle, s_chr_val_handle, batch.data[i],
                                                MB_FLASH_PACKET_BYTES, mb_ble_on_packet_write, NULL);
            if (rc != 0) {
                ESP_LOGE(TAG, "packet %u failed to start: rc=%d", i, rc);
                mb_ble_fail("E3", "the radio refused a packet");
                break;
            }
            if (xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
                /* A chain of missing acks is how a starved link looks, and it
                 * must end the cycle with a state instead of leaving the panel
                 * on "F" forever: measured on hardware, gate G4. */
                mb_ble_fail("E3", "packet ack timed out after 2 s");
                break;
            }
            if (s_write_error != 0) {
                mb_ble_fail("E3", "the peer rejected a packet");
                break;
            }
        }
    }
}

static void mb_ble_enqueue(uint8_t count)
{
    mb_tx_batch_t batch;
    batch.count = count;
    memcpy(batch.data, s_tx, (size_t)count * MB_FLASH_PACKET_BYTES);
    if (xQueueSend(s_txq, &batch, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGE(TAG, "tx queue full: dropped %u packets", count);
    }
}

static void mb_ble_send_region_info(uint8_t region)
{
    mb_flash_build_region_info(s_tx[0], region);
    mb_ble_enqueue(1);
}

static void mb_ble_send_end_of_tx(void)
{
    const uint32_t last = s_image->region2_start +
                          (s_image->block_count - 1u) * MB_FLASH_BLOCK_BYTES;
    mb_flash_build_end_of_tx(s_tx[0], last);
    mb_ble_enqueue(1);
}

static void mb_ble_send_block(uint32_t index)
{
    const uint32_t offset = index * MB_FLASH_BLOCK_BYTES;
    if (offset + MB_FLASH_BLOCK_BYTES > s_image->len) {
        mb_ble_fail("E3", "block index past the fetched image");
        return;
    }

    mb_flash_build_block(s_tx, s_image->region2_start + offset, s_image->data + offset);

    for (uint32_t i = 0; i < MB_FLASH_PACKETS_PER_BLOCK; i++) {
        /* The DAL compares data[3] with a counter that runs across the whole
         * transfer (packetCount++) instead of restarting per block, so stamping
         * the running value is what stops the device answering 0xAA on block 2. */
        s_tx[i][3] = (uint8_t)(s_packet + i);
    }

    if (index == 0) {
        /* One dump of the first burst: the address split and the counter are the
         * two things that silently corrupt a flash, so they are visible in the log. */
        for (uint32_t i = 0; i < MB_FLASH_PACKETS_PER_BLOCK; i++) {
            ESP_LOGI(TAG, "pkt%u: %02X %02X %02X %02X | %02X %02X %02X %02X …", (unsigned)i,
                     s_tx[i][0], s_tx[i][1], s_tx[i][2], s_tx[i][3], s_tx[i][4], s_tx[i][5],
                     s_tx[i][6], s_tx[i][7]);
        }
    }

    mb_ble_enqueue(MB_FLASH_PACKETS_PER_BLOCK);
    s_packet += MB_FLASH_PACKETS_PER_BLOCK;
}

static void mb_ble_region_answered(const mb_flash_notify_t *note)
{
    if (note->region == MB_FLASH_REGION_DAL) {
        ESP_LOGI(TAG, "region 1 (DAL) hash %02X%02X%02X%02X%02X%02X%02X%02X", note->hash[0],
                 note->hash[1], note->hash[2], note->hash[3], note->hash[4], note->hash[5],
                 note->hash[6], note->hash[7]);
        if (memcmp(note->hash, s_image->template_hash, sizeof(s_image->template_hash)) != 0) {
            /* The device refuses a mismatched DAL anyway; refusing here keeps the
             * failure at "we did not write" instead of a half-transfer. */
            mb_ble_fail("E2", "DAL hash differs from our image: refusing to write "
                              "(the board needs one USB bootstrap flash first)");
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

    const bool holds_ours = memcmp(note->hash, s_image->program_hash,
                                   sizeof(s_image->program_hash)) == 0;
    ESP_LOGI(TAG, "region 2 hash %02X%02X%02X%02X%02X%02X%02X%02X (%s our program)", note->hash[0],
             note->hash[1], note->hash[2], note->hash[3], note->hash[4], note->hash[5],
             note->hash[6], note->hash[7], holds_ours ? "holds" : "differs from");

    if (s_await_verify) {
        if (!holds_ours) {
            mb_ble_fail("E3", "region 2 does not hold the program after the transfer");
            return;
        }
        ++s_cycle;
        ESP_LOGI(TAG, "cycle %u/%u VERIFIED (0x%05X-0x%05X)", (unsigned)s_cycle,
                 (unsigned)CONFIG_MB_BLE_POC_CYCLES, s_image->region2_start,
                 s_image->region2_end - 1u);
        s_round++;
        if (s_cycle < CONFIG_MB_BLE_POC_CYCLES) {
            s_await_verify = false;
            ESP_LOGI(TAG, "starting cycle %u/%u; disconnecting to rescan",
                     (unsigned)(s_cycle + 1), (unsigned)CONFIG_MB_BLE_POC_CYCLES);
            ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return;
        }
        ESP_LOGI(TAG, "POC DONE %u/%u cycles verified with no manual reset",
                 (unsigned)s_cycle, (unsigned)CONFIG_MB_BLE_POC_CYCLES);
        mb_ble_status("OK");
        s_flash_done = true;
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }

    ESP_LOGI(TAG, "cycle %u/%u: streaming %u blocks from 0x%05X",
             (unsigned)(s_cycle + 1), (unsigned)CONFIG_MB_BLE_POC_CYCLES,
             (unsigned)s_image->block_count, s_image->region2_start);
    mb_ble_status("F");
    s_phase = MB_PHASE_STREAM;
    s_block = 0;
    s_packet = 0;
    mb_ble_send_block(0);
}

static void mb_ble_handle_notify(const struct os_mbuf *om)
{
    mb_ble_mark_progress();
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
        if (s_block < s_image->block_count) {
            if ((s_block % 16u) == 0) {
                ESP_LOGI(TAG, "flashing %u/%u blocks", (unsigned)s_block,
                         (unsigned)s_image->block_count);
            }
            mb_ble_send_block(s_block);
        } else {
            ESP_LOGI(TAG, "all %u blocks acknowledged (%u resyncs); END_OF_TX",
                     (unsigned)s_image->block_count, (unsigned)s_resyncs);
            s_phase = MB_PHASE_END;
            /* The device strips the source magic and resets; the link drops.
             * The next connection is what proves the write landed. */
            s_await_verify = true;
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
        if (s_resyncs > 4 || s_block >= s_image->block_count) {
            mb_ble_fail("E3", "transfer abandoned after too many resyncs");
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
        s_peer_seen = true;
        s_chr_val_handle = 0;
        s_svc_end_handle = 0;
        s_cccd_pending = false;
        s_conn_handle = event->connect.conn_handle;
        s_connected = true;
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
        s_connected = false;
        ESP_LOGW(TAG, "disconnected: reason=%d", event->disconnect.reason);
        /* No delay in this callback: it runs on the NimBLE host task, and
         * sleeping here stops HCI processing for the whole stack. The gap the
         * micro:bit needs after its own reset is a timer instead. */
        mb_ble_scan_soon(s_flash_done ? 0u : CONFIG_MB_BLE_CYCLE_GAP_MS);
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
    mb_ble_mark_progress();
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

esp_err_t mb_ble_central_start(const mb_flash_image_t *image, mb_flash_status_fn status)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (image == NULL || image->data == NULL || image->block_count == 0) {
        ESP_LOGE(TAG, "no image to flash: the build client must fetch one first");
        return ESP_ERR_INVALID_ARG;
    }
    s_image = image;
    s_status = status;
    s_cycle = 0;
    s_await_verify = false;

    /* NimBLE narrates every GAP/GATT procedure at INFO, which buries our own
     * lines: a single flash transfer produced ~3000 log lines of which ~40 were
     * ours. Errors and warnings stay visible; the play-by-play goes away. */
    esp_log_level_set("NimBLE", ESP_LOG_WARN);

    /* Stamp the build and the previous reset cause. Iris only starts logging
     * after app_main runs, so the ROM/bootloader reason for a restart is
     * invisible; without this a silent reboot loop looks like nothing happened.
     */
    ESP_LOGI(TAG, "probe build %s %s, reset_reason=%d",
             __DATE__, __TIME__, (int)esp_reset_reason());

    s_txq = xQueueCreate(8, sizeof(mb_tx_batch_t));
    s_tx_done = xSemaphoreCreateBinary();
    if (s_txq == NULL || s_tx_done == NULL) {
        ESP_LOGE(TAG, "could not allocate the tx queue");
        return ESP_ERR_NO_MEM;
    }
    const esp_timer_create_args_t timer_args = {
        .callback = mb_ble_scan_timer_cb,
        .name = "mb_ble_gap",
    };
    if (esp_timer_create(&timer_args, &s_scan_timer) != ESP_OK) {
        ESP_LOGW(TAG, "no timer for the inter-cycle gap; scans stay immediate");
        s_scan_timer = NULL;
    }
    if (xTaskCreate(mb_ble_tx_task, "mb_ble_tx", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start the tx task");
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(mb_ble_watch_task, "mb_ble_wd", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start the stall detector");
        return ESP_ERR_NO_MEM;
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
