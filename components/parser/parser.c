// parser.c
#include "parser.h"
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include "cJSON.h"
#include <strings.h>

#if defined(ESP_PLATFORM)
#include "esp_random.h"
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#endif

// ------------ intern: utils ------------
static void set_error(parser_result_t *r, parser_err_code_t code, const char *path, const char *detail) {
    r->ok = false;
    r->error.code = code;
    if (path)  snprintf(r->error.path,   sizeof(r->error.path),   "%s", path);
    else       r->error.path[0] = '\0';
    if (detail)snprintf(r->error.detail, sizeof(r->error.detail), "%s", detail);
    else       r->error.detail[0] = '\0';
}

static int parse_int_like(const cJSON *item, bool allow_percent, bool *is_percent, bool *ok) {
    *ok = false;
    if (is_percent) *is_percent = false;

    if (cJSON_IsNumber(item)) { *ok = true; return (int)(item->valuedouble); }

    if (cJSON_IsString(item) && item->valuestring) {
        const char *s = item->valuestring;
        size_t n = strlen(s);
        bool has_pct = (allow_percent && n>0 && s[n-1]=='%');
        char buf[32];
        if (has_pct) {
            if (n-1 >= sizeof(buf)) return 0;
            memcpy(buf, s, n-1); buf[n-1] = '\0';
            if (is_percent) *is_percent = true;
            // parse tot einde
            char *end = NULL;
            double d = strtod(buf, &end);
            // skip trailing spaces
            while (end && *end == ' ') end++;
            if (buf[0] == '\0' || (end && *end != '\0')) return 0; // niet volledig numeriek
            *ok = true; return (int)d;
        } else {
            if (n >= sizeof(buf)) return 0;
            memcpy(buf, s, n+1);
            char *end = NULL;
            double d = strtod(buf, &end);
            while (end && *end == ' ') end++;
            if (buf[0] == '\0' || (end && *end != '\0')) return 0; // bv. "ON" of "12ms" → ongeldig
            *ok = true; return (int)d;
        }
    }
    return 0;
}

static bool parse_bool_like(const cJSON *item, bool *out) {
    if (cJSON_IsBool(item)) { *out = cJSON_IsTrue(item); return true; }
    if (cJSON_IsNumber(item)) { *out = (item->valuedouble != 0.0); return true; }
    if (cJSON_IsString(item) && item->valuestring) {
        const char *s = item->valuestring;
        if (!strcasecmp(s,"on") || !strcasecmp(s,"true"))  { *out = true;  return true; }
        if (!strcasecmp(s,"off")|| !strcasecmp(s,"false")) { *out = false; return true; }
    }
    return false;
}

static const cJSON* get_any(const cJSON *obj, const char *const *keys) {
    for (size_t i=0; keys[i]; ++i) {
        const cJSON *it = cJSON_GetObjectItemCaseSensitive((cJSON*)obj, keys[i]);
        if (it) return it;
    }
    return NULL;
}

static const char *ACTION_KEYS[]   = {"action","command","state","relay_state","cmd",NULL};
static const char *TARGET_KEYS[]   = {"target_dev","target","device","dev","node",NULL};
static const char *IOKIND_KEYS[]   = {"io_kind","io","type","kind","device_type",NULL};
static const char *IOID_KEYS[]     = {"io_id","relay","pin","gpio","channel","index",NULL};
static const char *BRIGHT_KEYS[]   = {"brightness","level","duty","value","percent",NULL};
static const char *REPORT_KEYS[]   = {"value","val",NULL};
static const char *CORR_KEYS[]     = {"corr_id","correlation_id","id",NULL};
static const char *TOPIC_KEYS[]    = {"_topic","topic_hint",NULL};

// KEYS-tabel met metadata voor ms/s/min (synoniemen met unit-factor)
struct alias_ms { const char *name; int mult; const char *path; };

static const struct alias_ms DURATION_KEYS[] = {
    {"duration_ms", 1,      "params.duration_ms"},
    {"duration_s",  1000,   "params.duration_s"},
    {"duration",    1000,   "params.duration"},
    {"minutes",     60000,  "params.minutes"},
};


static const struct alias_ms RAMP_KEYS_MS[] = {
    {"ramp_ms", 1, "params.ramp_ms"}, {"ramp", 1, "params.ramp_ms"},
    {"fade",    1, "params.ramp_ms"}, {"transition", 1, "params.ramp_ms"},
};

static const struct alias_ms DEBOUNCE_KEYS_MS[] = {
    {"debounce_ms", 1, "params.debounce_ms"}, {"debounce", 1, "params.debounce_ms"},
};


static void strtolower_inplace(char *s) {
    for (; *s; ++s) *s = (char)tolower((unsigned char)*s);
}

static bool read_string(const cJSON *obj, const char *const *keys, char *out, size_t outsz) {
    const cJSON *it = get_any(obj, keys);
    if (!it || !cJSON_IsString(it) || !it->valuestring) return false;
    snprintf(out, outsz, "%s", it->valuestring);
    return true;
}

static bool read_int_any(const cJSON *obj, const char *const *keys, int *out, const char **path, bool allow_percent, bool *was_percent) {
    const cJSON *it = get_any(obj, keys);
    if (!it) return false;
    bool ok; int v = parse_int_like(it, allow_percent, was_percent, &ok);
    if (!ok) {
        if (path) *path = keys[0];
        return false;
    }
    *out = v;
    if (path) *path = keys[0];
    return true;
}

static bool read_param_ms(const cJSON *root,
                          const struct alias_ms *keys, size_t nkeys,
                          int min_ms, int max_ms,
                          const char *conflict_path,
                          bool *out_seen, int *out_ms,
                          parser_result_t *R)
{
    int seen = 0, value_ms = 0;

    for (size_t i = 0; i < nkeys; ++i) {
        const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, keys[i].name);
        if (!it) continue;

        bool ok=false, pct=false;
        int v = parse_int_like(it, false, &pct, &ok);
        if (!ok) { set_error(R, PARSER_ERR_TYPE_MISMATCH, keys[i].path, "int expected"); return false; }

        int ms = v * keys[i].mult;
        if (seen && ms != value_ms) {
            set_error(R, PARSER_ERR_CONFLICT, conflict_path, "conflicting values across aliases");
            return false;
        }
        value_ms = ms; seen = 1;
    }

    if (!seen) { *out_seen = false; return true; }  // niet opgegeven = OK

    if (value_ms < min_ms || value_ms > max_ms) {
        set_error(R, PARSER_ERR_OUT_OF_RANGE, conflict_path, "out of range");
        return false;
    }
    *out_seen = true;
    *out_ms   = value_ms;
    return true;
}

// "unknown key" lint helpers ---
static bool in_strv(const char *k, const char *const *arr) {
    for (size_t i = 0; arr[i]; ++i) if (!strcasecmp(k, arr[i])) return true;
    return false;
}
static bool in_alias_ms(const char *k, const struct alias_ms *arr, size_t n) {
    for (size_t i = 0; i < n; ++i) if (!strcasecmp(k, arr[i].name)) return true;
    return false;
}
static bool is_known_top_key(const char *k) {
    return
        in_strv(k, ACTION_KEYS) || in_strv(k, TARGET_KEYS) ||
        in_strv(k, IOKIND_KEYS) || in_strv(k, IOID_KEYS)   ||
        in_strv(k, BRIGHT_KEYS) || in_strv(k, REPORT_KEYS) ||
        in_strv(k, CORR_KEYS)   || in_strv(k, TOPIC_KEYS)  ||
        in_alias_ms(k, DURATION_KEYS, ARRAY_SIZE(DURATION_KEYS)) ||
        in_alias_ms(k, RAMP_KEYS_MS, ARRAY_SIZE(RAMP_KEYS_MS))   ||
        in_alias_ms(k, DEBOUNCE_KEYS_MS, ARRAY_SIZE(DEBOUNCE_KEYS_MS));
}
static int collect_unknown_top_keys(const cJSON *root, char *out, size_t outsz) {
    out[0] = '\0'; int count = 0;
    for (const cJSON *it = root ? root->child : NULL; it; it = it->next) {
        const char *k = it->string; if (!k) continue;
        if (is_known_top_key(k)) continue;
        if (k[0] == '_') continue;              // sta private velden toe (bijv. _topic)
        size_t used = strlen(out);
        if (used && used + 1 < outsz) { out[used++] = ','; out[used] = '\0'; }
        if (used < outsz) strncat(out, k, outsz - used - 1);
        count++;
    }
    return count;
}

// --- enum parsers ---
static bool parse_action_any(const cJSON *obj, action_t *out, const char **path) {
    const cJSON *it = get_any(obj, ACTION_KEYS);
    if (!it) return false;

    bool bval;
    if (parse_bool_like(it, &bval)) {
        *out = bval ? ACT_ON : ACT_OFF;
        if (path) *path = ACTION_KEYS[0];
        return true;
    }
    if (!cJSON_IsString(it) || !it->valuestring) {
        if (path) *path = ACTION_KEYS[0];
        return false;
    }
    char tmp[16]; snprintf(tmp, sizeof(tmp), "%s", it->valuestring); strtolower_inplace(tmp);
    if (!strcmp(tmp,"on"))      *out = ACT_ON;
    else if (!strcmp(tmp,"off"))*out = ACT_OFF;
    else if (!strcmp(tmp,"toggle")) *out = ACT_TOGGLE;
    else if (!strcmp(tmp,"set"))    *out = ACT_SET;
    else if (!strcmp(tmp,"read"))   *out = ACT_READ;
    else if (!strcmp(tmp,"report")) *out = ACT_REPORT;
    else return false;

    if (path) *path = ACTION_KEYS[0];
    return true;
}

static bool parse_iokind_any(const cJSON *obj, io_kind_t *out) {
    const cJSON *it = get_any(obj, IOKIND_KEYS);
    if (!it) return false;
    if (!cJSON_IsString(it) || !it->valuestring) return false;
    char tmp[16]; snprintf(tmp, sizeof(tmp), "%s", it->valuestring); strtolower_inplace(tmp);
    if (!strcmp(tmp,"relay")) *out = IO_RELAY;
    else if (!strcmp(tmp,"pwm")) *out = IO_PWM;
    else if (!strcmp(tmp,"input")) *out = IO_INPUT;
    else return false;
    return true;
}

static bool derive_iokind_from_hints(const cJSON *obj, action_t act, io_kind_t *out) {
    // eerst READ/REPORT → INPUT
    if (act == ACT_READ || act == ACT_REPORT || get_any(obj, REPORT_KEYS)) { *out = IO_INPUT; return true; }
    // daarna brightness/duty → PWM
    if (get_any(obj, BRIGHT_KEYS)) { *out = IO_PWM; return true; }
    // anders veldnamen zoals relay/pin/gpio → RELAY
    if (get_any(obj, IOID_KEYS)) { *out = IO_RELAY; return true; }
    return false;
}

// corr-id
static void gen_corr_id(char out[PARSER_CORR_MAX]) {
#if defined(ESP_PLATFORM)
    uint32_t r[4] = { esp_random(), esp_random(), esp_random(), esp_random() };
#else
    uint32_t r[4] = { (uint32_t)rand(), (uint32_t)rand(), (uint32_t)rand(), (uint32_t)rand() };
#endif
    // gebruik PRI-macros zodat het op elke toolchain klopt
    snprintf(out, PARSER_CORR_MAX,
        "%08" PRIx32 "-%04" PRIx16 "-%04" PRIx16 "-%04" PRIx16 "-%08" PRIx32,
        r[0],
        (uint16_t)(r[1] & 0xFFFF),
        (uint16_t)((r[1] >> 16) & 0xFFFF),
        (uint16_t)(r[2] & 0xFFFF),
        r[3]);
}

// ------------ public helpers ------------
const char *parser_err_str(parser_err_code_t c){
    switch(c){
        case PARSER_OK: return "OK";
        case PARSER_ERR_INVALID_JSON: return "INVALID_JSON";
        case PARSER_ERR_MISSING_FIELD: return "MISSING_FIELD";
        case PARSER_ERR_TYPE_MISMATCH: return "TYPE_MISMATCH";
        case PARSER_ERR_OUT_OF_RANGE: return "OUT_OF_RANGE";
        case PARSER_ERR_INVALID_ENUM: return "INVALID_ENUM";
        case PARSER_ERR_UNKNOWN_ALIAS: return "UNKNOWN_ALIAS";
        case PARSER_ERR_CONFLICT: return "CONFLICT";
        default: return "INTERNAL";
    }
}
const char *parser_action_str(action_t a){
    switch(a){
        case ACT_ON: return "ON"; case ACT_OFF: return "OFF"; case ACT_TOGGLE: return "TOGGLE";
        case ACT_SET: return "SET"; case ACT_READ: return "READ"; case ACT_REPORT: return "REPORT";
        default: return "?";
    }
}
const char *parser_iokind_str(io_kind_t k){
    switch(k){ case IO_RELAY: return "RELAY"; case IO_PWM: return "PWM"; case IO_INPUT: return "INPUT"; default: return "?";}
}
const char *parser_msgtype_str(msg_type_t t){
    switch(t){ case MSG_COMMAND: return "COMMAND"; case MSG_QUERY: return "QUERY"; case MSG_EVENT: return "EVENT";
               case MSG_ACK: return "ACK"; case MSG_ERROR: return "ERROR"; default: return "?"; }
}

// ------------ API ------------
void parser_init(void) { /* voor toekomstige lookups/tables */ }

parser_result_t parser_parse(const char *json, const parser_meta_t *meta) {
    parser_result_t R = {0};
    R.ok = false;

    if (!json) { set_error(&R, PARSER_ERR_INVALID_JSON, "root", "NULL input"); return R; }

    cJSON *root = cJSON_Parse(json);
    if (!root) { set_error(&R, PARSER_ERR_INVALID_JSON, "root", "JSON parse failed"); return R; }

    // --- unknown keys ---
    (void)collect_unknown_top_keys(root, R.unknown_keys, sizeof(R.unknown_keys));

    // --- corr_id ---
    if (!read_string(root, CORR_KEYS, R.msg.corr_id, sizeof(R.msg.corr_id))) {
        gen_corr_id(R.msg.corr_id);
        R.msg.meta.corr_generated = true;
    }

    // --- topic_hint (meta > payload) ---
    if (meta && meta->topic_hint) {
        snprintf(R.msg.topic_hint, sizeof(R.msg.topic_hint), "%s", meta->topic_hint);
    } else {
        (void)read_string(root, TOPIC_KEYS, R.msg.topic_hint, sizeof(R.msg.topic_hint));
    }

    // --- meta ---
    R.msg.meta.source = meta ? meta->source : PARSER_SRC_LOCAL;
    R.msg.meta.received_ts_ms = meta ? meta->received_ts_ms : 0;

    // --- target_dev ---
    if (!read_string(root, TARGET_KEYS, R.msg.target_dev, sizeof(R.msg.target_dev))) {
        set_error(&R, PARSER_ERR_MISSING_FIELD, "target_dev", "expected non-empty string");
        cJSON_Delete(root); return R;
    }
    if (R.msg.target_dev[0] == '\0') {
        set_error(&R, PARSER_ERR_OUT_OF_RANGE, "target_dev", "empty");
        cJSON_Delete(root); return R;
    }

    // --- action ---
    action_t act;
    if (!parse_action_any(root, &act, NULL)) {
        set_error(&R, PARSER_ERR_INVALID_ENUM, "action", "allowed: ON/OFF/TOGGLE/SET/READ/REPORT");
        cJSON_Delete(root); return R;
    }
    R.msg.action = act;
    // msg_type afleiden
    R.msg.type = (act==ACT_READ) ? MSG_QUERY : (act==ACT_REPORT ? MSG_EVENT : MSG_COMMAND);

    // --- io_kind ---
    io_kind_t kind;
    if (!parse_iokind_any(root, &kind)) {
        if (!derive_iokind_from_hints(root, act, &kind)) {
            set_error(&R, PARSER_ERR_INVALID_ENUM, "io_kind", "derive failed; provide io_kind");
            cJSON_Delete(root); return R;
        }
    }
    R.msg.io_kind = kind;

    // --- io_id ---
    bool dummy_pct=false; int io_id = -1;
    if (!read_int_any(root, IOID_KEYS, &io_id, NULL, false, &dummy_pct)) {
        set_error(&R, PARSER_ERR_MISSING_FIELD, "io_id", "expected int 0..63");
        cJSON_Delete(root); return R;
    }
    if (io_id < 0 || io_id > 63) {
        set_error(&R, PARSER_ERR_OUT_OF_RANGE, "io_id", "expected 0..63");
        cJSON_Delete(root); return R;
    }
    R.msg.io_id = io_id;

    // --- params normalisatie ---
    R.msg.params = (parser_params_t){0};

    // RELAY: duration*
    if (kind == IO_RELAY) {bool dur_seen=false; int dur_ms=0;
        if (!read_param_ms(root, DURATION_KEYS, ARRAY_SIZE(DURATION_KEYS),
                        0, 86400000, "params.duration", &dur_seen, &dur_ms, &R)) {
            cJSON_Delete(root); return R;  // error gezet
        }
        if (dur_seen) {
            R.msg.params.duration_ms = dur_ms;       // mag 0 zijn
            R.msg.params.has_duration_ms = true;
        }
    }

    // PWM: brightness/duty + ramp
    if (kind == IO_PWM) {
        bool is_pct=false, ok=false;
        int b_val=0;
        const cJSON *b_item = get_any(root, BRIGHT_KEYS);
        if (b_item) {
            b_val = parse_int_like(b_item, true, &is_pct, &ok);
            if (!ok) { set_error(&R, PARSER_ERR_TYPE_MISMATCH, "params.brightness", "int or \"NN%\" expected"); cJSON_Delete(root); return R; }
            if (!is_pct) {
                // duty 0..255? Detecteer op key-naam "duty"
                const cJSON *duty_it = cJSON_GetObjectItemCaseSensitive(root,"duty");
                if (duty_it == b_item) {
                    // duty → pct
                    if (b_val < 0 || b_val > 255) { set_error(&R, PARSER_ERR_OUT_OF_RANGE, "params.duty", "0..255"); cJSON_Delete(root); return R; }
                    b_val = (int)((b_val * 100 + 127) / 255); // ronding
                }
            }
            if (b_val < 0 || b_val > 100) { set_error(&R, PARSER_ERR_OUT_OF_RANGE, "params.brightness_pct", "0..100"); cJSON_Delete(root); return R; }
            R.msg.params.brightness_pct = b_val;
            R.msg.params.has_brightness_pct = true;
        }

        bool ramp_seen=false; int ramp_ms=0;
        if (!read_param_ms(root, RAMP_KEYS_MS, ARRAY_SIZE(RAMP_KEYS_MS),
                        0, 60000, "params.ramp_ms", &ramp_seen, &ramp_ms, &R)) {
            cJSON_Delete(root); return R;
        }
        if (ramp_seen) {
            R.msg.params.ramp_ms = ramp_ms;
            R.msg.params.has_ramp_ms = true;
        }
    }

    // INPUT: REPORT value (note: READ heeft geen value)
    if (kind == IO_INPUT) {
        if (act == ACT_REPORT) {
            const cJSON *val = get_any(root, REPORT_KEYS);
            if (!val) { set_error(&R, PARSER_ERR_MISSING_FIELD, "params.value", "required for REPORT"); cJSON_Delete(root); return R; }
        } else if (act == ACT_READ) {
            bool seen=false; int ms=0;
            if (!read_param_ms(root, DEBOUNCE_KEYS_MS, ARRAY_SIZE(DEBOUNCE_KEYS_MS),
                            0, 5000, "params.debounce_ms", &seen, &ms, &R)) {
                cJSON_Delete(root); return R;
            }
            if (seen) {
                R.msg.params.debounce_ms = ms;
                R.msg.params.has_debounce_ms = true;
            }
        }
    }

    // --- done ---
    R.ok = true;
    cJSON_Delete(root);
    return R;
}
