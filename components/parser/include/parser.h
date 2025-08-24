// parser.h
#pragma once
// ESP32 Runtime Parser (JSON -> canonieke Msg)
// Afhankelijkheden: cJSON

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Limits ---
#define PARSER_TOPIC_MAX 128
#define PARSER_DEVNAME_MAX 32
#define PARSER_CORR_MAX  40   // "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" + '\0'

// --- Meta bron ---
typedef enum {
    PARSER_SRC_MQTT = 0,
    PARSER_SRC_MESH,
    PARSER_SRC_LOCAL
} parser_source_t;

// --- Canonieke enums ---
typedef enum {
    MSG_COMMAND = 0,
    MSG_QUERY,
    MSG_EVENT,
    MSG_ACK,
    MSG_ERROR
} msg_type_t;

typedef enum {
    IO_RELAY = 0,
    IO_PWM,
    IO_INPUT
} io_kind_t;

typedef enum {
    ACT_ON = 0,
    ACT_OFF,
    ACT_TOGGLE,
    ACT_SET,
    ACT_READ,
    ACT_REPORT
} action_t;

// --- Parser foutcodes ---
typedef enum {
    PARSER_OK = 0,
    PARSER_ERR_INVALID_JSON,
    PARSER_ERR_MISSING_FIELD,
    PARSER_ERR_TYPE_MISMATCH,
    PARSER_ERR_OUT_OF_RANGE,
    PARSER_ERR_INVALID_ENUM,
    PARSER_ERR_UNKNOWN_ALIAS,
    PARSER_ERR_CONFLICT,
    PARSER_ERR_INTERNAL
} parser_err_code_t;

// --- Meta meegegeven door caller (RX-context) ---
typedef struct {
    parser_source_t source;
    const char *topic_hint;     // optioneel, mag NULL
    uint64_t received_ts_ms;    // optioneel (0 indien onbekend)
} parser_meta_t;

// --- Canonieke Msg + params ---
typedef struct {
    int duration_ms;       bool has_duration_ms;
    int brightness_pct;    bool has_brightness_pct;
    int ramp_ms;           bool has_ramp_ms;
    int debounce_ms;       bool has_debounce_ms;
} parser_params_t;

typedef struct {
    msg_type_t     type;
    char           topic_hint[PARSER_TOPIC_MAX];
    char           target_dev[PARSER_DEVNAME_MAX + 1];
    io_kind_t      io_kind;
    int            io_id;              // 0..63 (logisch kanaal of GPIO, downstream mapping)
    action_t       action;
    parser_params_t params;
    char           corr_id[PARSER_CORR_MAX];
    struct {
        parser_source_t source;
        uint64_t received_ts_ms;
        bool corr_generated;
    } meta;
} parser_msg_t;

// --- Error detail ---
typedef struct {
    parser_err_code_t code;
    char path[64];    // bv. "params.duration_ms"
    char detail[96];  // korte uitleg/waarden
} parser_error_t;

// --- Resultaat ---
typedef struct {
    bool ok;
    parser_msg_t msg;        // geldig als ok==true
    parser_error_t error;    // geldig als ok==false
    char unknown_keys[128];  // csv-lijst met onbekende top-level velden ("" = geen)
} parser_result_t;

// --- API ---
void parser_init(void);  // momenteel no-op; laat staan voor toekomst
parser_result_t parser_parse(const char *json, const parser_meta_t *meta);

// (optioneel) helpers voor logging/debug
const char *parser_err_str(parser_err_code_t c);
const char *parser_action_str(action_t a);
const char *parser_iokind_str(io_kind_t k);
const char *parser_msgtype_str(msg_type_t t);

#ifdef __cplusplus
}
#endif
