/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <rte/controller.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/magic.h>
#include <mongoose.h>

#define RTE_HTTP_BODY_MAX 16384U
#define RTE_JSON_RESPONSE_MAX 16384U
#define RTE_PERSIST_DEBOUNCE_MS 2000U
#define RTE_DEFAULT_STATE_PATH "/mnt/jffs2/rte-multitarget/last-config.json"
#define RTE_DEFAULT_WEB_ROOT "/usr/share/rte-control/www"

#define RTE_HTTP_SECURITY_HEADERS                                          \
    "X-Content-Type-Options: nosniff\r\n"                                \
    "X-Frame-Options: DENY\r\n"                                         \
    "Referrer-Policy: no-referrer\r\n"                                  \
    "Permissions-Policy: camera=(), microphone=(), geolocation=()\r\n"    \
    "Content-Security-Policy: default-src 'self'; connect-src 'self'; "    \
    "img-src 'self' data:; script-src 'self'; style-src 'self'; "          \
    "object-src 'none'; base-uri 'none'; frame-ancestors 'none'; "         \
    "form-action 'self'\r\n"

#define RTE_HTTP_JSON_HEADERS                                              \
    "Content-Type: application/json\r\n"                                 \
    "Cache-Control: no-store\r\n"                                        \
    RTE_HTTP_SECURITY_HEADERS

#define RTE_HTTP_STATIC_HEADERS                                            \
    "Cache-Control: no-cache\r\n"                                        \
    RTE_HTTP_SECURITY_HEADERS

struct json_buffer {
    char *data;
    size_t size;
    size_t used;
    bool overflow;
};

struct rte_http_app {
    struct rte_controller controller;
    const char *state_path;
    const char *web_root;
    struct rte_snapshot pending_snapshot;
    bool has_requested_rf;
    struct rte_rf_context requested_rf;
    bool has_saved[RTE_TARGET_COUNT];
    struct rte_config saved[RTE_TARGET_COUNT];
    uint64_t persistence_deadline_ms;
    bool persistence_dirty;
    bool require_jffs2;
    char warning[256];
};

enum endpoint_methods {
    ENDPOINT_UNKNOWN,
    ENDPOINT_GET,
    ENDPOINT_POST,
    ENDPOINT_GET_PATCH,
};

static volatile sig_atomic_t stop_signal;

static void signal_handler(int signal_number)
{
    stop_signal = signal_number;
}

static void set_error(struct rte_error *error, enum rte_error_code code,
                      const char *field, const char *format, ...)
{
    va_list args;

    if (error == NULL)
        return;
    error->code = code;
    snprintf(error->field, sizeof(error->field), "%s",
             field != NULL ? field : "");
    va_start(args, format);
    vsnprintf(error->message, sizeof(error->message), format, args);
    va_end(args);
}

static void json_append(struct json_buffer *buffer, const char *format, ...)
{
    va_list args;
    int length;

    if (buffer->overflow)
        return;
    va_start(args, format);
    length = vsnprintf(buffer->data + buffer->used,
                       buffer->size - buffer->used, format, args);
    va_end(args);
    if (length < 0 || (size_t)length >= buffer->size - buffer->used) {
        buffer->overflow = true;
        if (buffer->size != 0)
            buffer->data[buffer->size - 1] = '\0';
        return;
    }
    buffer->used += (size_t)length;
}

static void json_append_escaped(struct json_buffer *buffer, const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;

    json_append(buffer, "\"");
    while (!buffer->overflow && *cursor != '\0') {
        switch (*cursor) {
        case '"':
            json_append(buffer, "\\\"");
            break;
        case '\\':
            json_append(buffer, "\\\\");
            break;
        case '\b':
            json_append(buffer, "\\b");
            break;
        case '\f':
            json_append(buffer, "\\f");
            break;
        case '\n':
            json_append(buffer, "\\n");
            break;
        case '\r':
            json_append(buffer, "\\r");
            break;
        case '\t':
            json_append(buffer, "\\t");
            break;
        default:
            if (*cursor < 0x20)
                json_append(buffer, "\\u%04x", *cursor);
            else
                json_append(buffer, "%c", *cursor);
            break;
        }
        cursor++;
    }
    json_append(buffer, "\"");
}


static const char *json_bool(bool value) { return value ? "true" : "false"; }

static bool all_targets_known(const struct rte_snapshot *snapshot)
{
    unsigned int i;
    if (snapshot->degraded) return false;
    for (i = 0; i < RTE_TARGET_COUNT; ++i)
        if (!snapshot->targets[i].hardware_state_known) return false;
    return true;
}

static void json_append_config(struct json_buffer *buffer, const struct rte_config *config)
{
    json_append(buffer, "{\"enabled\":%s,\"range_m\":%.17g,"
                "\"radial_velocity_mps\":%.17g,\"gain_linear\":%.17g,"
                "\"phase_offset_deg\":%.17g}", json_bool(config->enabled),
                config->range_m, config->radial_velocity_mps,
                config->gain_linear, config->phase_offset_deg);
}

static void json_append_rf_config(struct json_buffer *buffer, const struct rte_rf_context *rf)
{
    json_append(buffer, "{\"carrier_hz\":%" PRIu64 ",\"bandwidth_hz\":%" PRIu64
                ",\"tx_gain_db\":%.3f,\"rx_gain_db\":%.3f}",
                rf->rx_lo_hz, rf->rx_bandwidth_hz,
                rf->tx_gain_mdb / 1000.0, rf->rx_gain_mdb / 1000.0);
}

static struct rte_rf_context rf_config_for_snapshot(const struct rte_http_app *app,
                                                    const struct rte_snapshot *snapshot)
{
    struct rte_rf_context config = snapshot->rf;
    /* Retain a nominal setpoint across partial patches and restarts, without
     * replacing an externally changed LO or other actual RF fields. */
    if (app->has_requested_rf &&
        rte_ad936x_lo_matches(app->requested_rf.rx_lo_hz, config.rx_lo_hz) &&
        rte_ad936x_lo_matches(app->requested_rf.tx_lo_hz, config.tx_lo_hz)) {
        config.rx_lo_hz = app->requested_rf.rx_lo_hz;
        config.tx_lo_hz = app->requested_rf.tx_lo_hz;
    }
    return config;
}

static void remember_rf_request(struct rte_http_app *app, const struct rte_rf_patch *patch,
                                 const struct rte_snapshot *snapshot)
{
    app->requested_rf = rf_config_for_snapshot(app, snapshot);
    if (patch->has_carrier_hz)
        app->requested_rf.rx_lo_hz = app->requested_rf.tx_lo_hz = patch->carrier_hz;
    app->has_requested_rf = true;
}

static void json_append_snapshot(struct json_buffer *buffer,
                                 const struct rte_snapshot *snapshot,
                                 const struct rte_http_app *app)
{
    const struct rte_rf_context *rf = &snapshot->rf;
    unsigned int i, j;
    json_append(buffer, "{\"api_version\":\"v1\",\"hardware_profile\":\"multitarget-v2\","
                "\"revision\":%" PRIu64 ",\"hardware_build_id\":%" PRIu32
                ",\"degraded\":%s,\"hardware_state_known\":%s,"
                "\"last_hardware_check_ms\":%" PRIu64 ",\"warning\":",
                snapshot->revision, snapshot->hardware_build_id,
                json_bool(snapshot->degraded), json_bool(all_targets_known(snapshot)),
                snapshot->last_hardware_check_ms);
    json_append_escaped(buffer, app->warning);
    json_append(buffer, ",\"rf\":{\"sample_rate_hz\":%" PRIu64
                ",\"carrier_hz\":%" PRIu64 ",\"rx_lo_hz\":%" PRIu64
                ",\"tx_lo_hz\":%" PRIu64 ",\"bandwidth_hz\":%" PRIu64
                ",\"rx_bandwidth_hz\":%" PRIu64 ",\"tx_bandwidth_hz\":%" PRIu64
                ",\"tx_gain_db\":%.3f,\"rx_gain_db\":%.3f,\"gain_control_mode\":\"%s\","
                "\"compatible\":", rf->sample_rate_hz, rf->rx_lo_hz,
                rf->rx_lo_hz, rf->tx_lo_hz, rf->rx_bandwidth_hz,
                rf->rx_bandwidth_hz, rf->tx_bandwidth_hz,
                rf->tx_gain_mdb / 1000.0, rf->rx_gain_mdb / 1000.0,
                rf->manual_gain ? "manual" : "unsupported");
    json_append_escaped(buffer, rf->compatible);
    json_append(buffer, "},\"requested_rf\":");
    if (app->has_requested_rf) json_append_rf_config(buffer, &app->requested_rf);
    else json_append(buffer, "null");
    json_append(buffer, ",\"targets\":[");
    for (i = 0; i < RTE_TARGET_COUNT; ++i) {
        const struct rte_target_state *target = &snapshot->targets[i];
        const struct rte_applied *a = &target->applied;
        json_append(buffer, "%s{\"id\":%u,\"has_config\":%s,\"hardware_state_known\":%s,"
                    "\"needs_reapply\":%s,\"encoded_carrier_hz\":%" PRIu64 ",\"requested\":",
                    i ? "," : "", i + 1, json_bool(target->has_config),
                    json_bool(target->hardware_state_known), json_bool(target->needs_reapply),
                    target->encoded_carrier_hz);
        if (target->has_config) json_append_config(buffer, &target->requested);
        else json_append(buffer, "null");
        json_append(buffer, ",\"saved\":");
        if (app->has_saved[i]) json_append_config(buffer, &app->saved[i]);
        else json_append(buffer, "null");
        json_append(buffer, ",\"applied\":");
        if (!target->has_config) { json_append(buffer, "null}"); continue; }
        json_append(buffer, "{\"enabled\":%s,\"delay_samples\":%.17g,\"delay_range_m\":%.17g,"
                    "\"range_phase_rad\":%.17g,\"doppler_hz\":%.17g,"
                    "\"radial_velocity_mps\":%.17g,\"linear_gain\":%.17g,"
                    "\"effective_radial_velocity_mps\":",
                    json_bool(a->enabled), a->delay_samples, a->delay_range_m,
                    a->range_phase_rad, a->doppler_hz, a->radial_velocity_mps, a->linear_gain);
        if (!snapshot->degraded && target->hardware_state_known && rf->rx_lo_hz != 0)
            json_append(buffer, "%.17g", a->radial_velocity_mps *
                        (double)target->encoded_carrier_hz / (double)rf->rx_lo_hz);
        else json_append(buffer, "null");
        json_append(buffer, ",\"registers\":{");
        for (j = 0; j < RTE_PARAMETER_REGISTER_COUNT; ++j)
            json_append(buffer, "%s\"%s\":{\"offset\":\"0x%03" PRIx32
                        "\",\"value\":\"0x%08" PRIx32 "\"}", j ? "," : "",
                        rte_target_registers[i][j].name,
                        rte_target_registers[i][j].offset, target->image.words[j]);
        json_append(buffer, "}}}");
    }
    json_append(buffer, "]}");
}

static void reply_json(struct mg_connection *connection, int status,
                       const char *body, uint64_t revision)
{
    char headers[768];

    snprintf(headers, sizeof(headers),
             RTE_HTTP_JSON_HEADERS
             "ETag: \"%" PRIu64 "\"\r\n",
             revision);
    mg_http_reply(connection, status, headers, "%s\n", body);
}

static int http_status_for_error(const struct rte_error *error)
{
    switch (error->code) {
    case RTE_ERROR_ARGUMENT:
    case RTE_ERROR_RANGE:
    case RTE_ERROR_RF_CONTEXT:
    case RTE_ERROR_QUANTIZATION:
    case RTE_ERROR_ALIASING:
    case RTE_ERROR_STATE:
        return 422;
    case RTE_ERROR_CONFLICT:
        return 409;
    case RTE_ERROR_HARDWARE:
    case RTE_ERROR_IO:
        return 503;
    default:
        return 500;
    }
}

static void reply_error(struct mg_connection *connection,
                        const struct rte_error *error, uint64_t revision)
{
    char body[1024];
    struct json_buffer json = {body, sizeof(body), 0, false};

    json_append(&json, "{\"error\":{\"code\":");
    json_append_escaped(&json, rte_error_code_name(error->code));
    json_append(&json, ",\"field\":");
    json_append_escaped(&json, error->field);
    json_append(&json, ",\"message\":");
    json_append_escaped(&json, error->message);
    json_append(&json, "},\"revision\":%" PRIu64 "}", revision);
    reply_json(connection, http_status_for_error(error), body, revision);
}


static bool json_whitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void skip_space(struct mg_str json, size_t *pos)
{
    while (*pos < json.len && json_whitespace(json.ptr[*pos])) ++*pos;
}

static bool scan_string(struct mg_str json, size_t *pos)
{
    if (*pos >= json.len || json.ptr[(*pos)++] != '"') return false;
    while (*pos < json.len) {
        unsigned char c = (unsigned char)json.ptr[(*pos)++];
        if (c == '"') return true;
        if (c < 0x20) return false;
        if (c == '\\') {
            unsigned int k;
            if (*pos == json.len) return false;
            c = (unsigned char)json.ptr[(*pos)++];
            if (c == 'u') {
                for (k = 0; k < 4; ++k) {
                    if (*pos == json.len) return false;
                    c = (unsigned char)json.ptr[(*pos)++];
                    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                          (c >= 'A' && c <= 'F'))) return false;
                }
            } else if (strchr("\"\\/bfnrt", c) == NULL || c == 0) return false;
        }
    }
    return false;
}

static bool scan_value(struct mg_str json, size_t *pos, unsigned int depth)
{
    char c;
    if (depth > 16) return false;
    skip_space(json, pos);
    if (*pos == json.len) return false;
    c = json.ptr[*pos];
    if (c == '"') return scan_string(json, pos);
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        ++*pos;
        skip_space(json, pos);
        if (*pos < json.len && json.ptr[*pos] == close) { ++*pos; return true; }
        for (;;) {
            if (c == '{') {
                if (!scan_string(json, pos)) return false;
                skip_space(json, pos);
                if (*pos == json.len || json.ptr[(*pos)++] != ':') return false;
            }
            if (!scan_value(json, pos, depth + 1)) return false;
            skip_space(json, pos);
            if (*pos == json.len) return false;
            if (json.ptr[*pos] == close) { ++*pos; return true; }
            if (json.ptr[(*pos)++] != ',') return false;
            skip_space(json, pos);
        }
    }
    if (c == 't' || c == 'f' || c == 'n') {
        const char *literal = c == 't' ? "true" : c == 'f' ? "false" : "null";
        size_t length = strlen(literal);
        if (json.len - *pos < length || memcmp(json.ptr + *pos, literal, length)) return false;
        *pos += length;
        return true;
    }
    if (c == '-') ++*pos;
    if (*pos == json.len) return false;
    if (json.ptr[*pos] == '0') ++*pos;
    else {
        if (json.ptr[*pos] < '1' || json.ptr[*pos] > '9') return false;
        while (*pos < json.len && json.ptr[*pos] >= '0' && json.ptr[*pos] <= '9') ++*pos;
    }
    if (*pos < json.len && json.ptr[*pos] == '.') {
        ++*pos;
        if (*pos == json.len || json.ptr[*pos] < '0' || json.ptr[*pos] > '9') return false;
        while (*pos < json.len && json.ptr[*pos] >= '0' && json.ptr[*pos] <= '9') ++*pos;
    }
    if (*pos < json.len && (json.ptr[*pos] == 'e' || json.ptr[*pos] == 'E')) {
        ++*pos;
        if (*pos < json.len && (json.ptr[*pos] == '+' || json.ptr[*pos] == '-')) ++*pos;
        if (*pos == json.len || json.ptr[*pos] < '0' || json.ptr[*pos] > '9') return false;
        while (*pos < json.len && json.ptr[*pos] >= '0' && json.ptr[*pos] <= '9') ++*pos;
    }
    return true;
}

static int json_root_object(struct mg_str json, struct rte_error *error)
{
    size_t pos = 0;
    skip_space(json, &pos);
    if (pos == json.len || json.ptr[pos] != '{' || !scan_value(json, &pos, 0)) goto invalid;
    skip_space(json, &pos);
    if (pos == json.len) return 0;
invalid:
    set_error(error, RTE_ERROR_ARGUMENT, "body", "body must be one valid JSON object");
    return -EINVAL;
}

static int optional_number(struct mg_str json, const char *path,
                           bool *present, double *value,
                           struct rte_error *error)
{
    int status = mg_json_get(json, path, NULL);

    *present = false;
    if (status == MG_JSON_NOT_FOUND)
        return 0;
    if (status < 0 || !mg_json_get_num(json, path, value) ||
        !isfinite(*value)) {
        set_error(error, RTE_ERROR_ARGUMENT, path,
                  "field must be a finite JSON number");
        return -EINVAL;
    }
    *present = true;
    return 0;
}

static int optional_u64(struct mg_str json, const char *path,
                        bool *present, uint64_t *value,
                        struct rte_error *error)
{
    double number;
    int status = optional_number(json, path, present, &number, error);

    if (status != 0 || !*present)
        return status;
    if (number < 1.0 || number > 9007199254740991.0 ||
        floor(number) != number) {
        set_error(error, RTE_ERROR_RANGE, path,
                  "field must be a positive integer no larger than 2^53-1");
        return -ERANGE;
    }
    *value = (uint64_t)number;
    return 0;
}

static int make_path(char *output, size_t output_size,
                     const char *prefix, const char *field)
{
    int length = snprintf(output, output_size, "%s.%s", prefix, field);

    return length < 0 || (size_t)length >= output_size ? -ENAMETOOLONG : 0;
}

static int json_key_index(const char *key, size_t key_length,
                          const char *const *allowed, size_t allowed_count)
{
    size_t index;

    for (index = 0; index < allowed_count; ++index) {
        if (strlen(allowed[index]) == key_length &&
            memcmp(key, allowed[index], key_length) == 0)
            return (int)index;
    }
    return -1;
}

static int validate_object_keys(struct mg_str json, const char *path,
                                const char *const *allowed,
                                size_t allowed_count,
                                struct rte_error *error)
{
    uint32_t seen = 0;
    int token_length = 0;
    int offset = mg_json_get(json, path, &token_length);
    size_t cursor;
    size_t end;
    size_t member_count = 0;

    if (offset == MG_JSON_NOT_FOUND)
        return 0;
    if (offset < 0 || token_length < 2 || json.ptr[offset] != '{') {
        set_error(error, RTE_ERROR_ARGUMENT, path,
                  "field must be a JSON object");
        return -EINVAL;
    }
    cursor = (size_t)offset + 1U;
    end = (size_t)offset + (size_t)token_length - 1U;

    while (cursor < end) {
        size_t key_start;
        size_t key_length;
        unsigned int nesting = 0;
        bool in_string = false;
        bool escaped_key = false;
        bool escape = false;
        int key_index;

        while (cursor < end && json_whitespace(json.ptr[cursor]))
            cursor++;
        if (cursor == end)
            break;
        if (json.ptr[cursor++] != '"') {
            set_error(error, RTE_ERROR_ARGUMENT, path,
                      "object contains an invalid member name");
            return -EINVAL;
        }
        key_start = cursor;
        while (cursor < end && json.ptr[cursor] != '"') {
            if (json.ptr[cursor] == '\\') {
                escaped_key = true;
                cursor++;
                if (cursor == end)
                    break;
            }
            cursor++;
        }
        if (cursor == end) {
            set_error(error, RTE_ERROR_ARGUMENT, path,
                      "object contains an unterminated member name");
            return -EINVAL;
        }
        key_length = cursor - key_start;
        cursor++;
        key_index = escaped_key ? -1 : json_key_index(
            json.ptr + key_start, key_length, allowed, allowed_count);
        if (key_index < 0) {
            set_error(error, RTE_ERROR_ARGUMENT, path,
                      "object contains an unsupported field");
            return -EINVAL;
        }
        if ((seen & (UINT32_C(1) << (unsigned int)key_index)) != 0) {
            set_error(error, RTE_ERROR_ARGUMENT, path,
                      "object contains a duplicate field");
            return -EINVAL;
        }
        seen |= UINT32_C(1) << (unsigned int)key_index;
        member_count++;

        while (cursor < end && json_whitespace(json.ptr[cursor]))
            cursor++;
        if (cursor == end || json.ptr[cursor++] != ':') {
            set_error(error, RTE_ERROR_ARGUMENT, path,
                      "object member is missing a value");
            return -EINVAL;
        }
        while (cursor < end && json_whitespace(json.ptr[cursor]))
            cursor++;

        for (; cursor < end; ++cursor) {
            char character = json.ptr[cursor];

            if (in_string) {
                if (escape) {
                    escape = false;
                } else if (character == '\\') {
                    escape = true;
                } else if (character == '"') {
                    in_string = false;
                }
            } else if (character == '"') {
                in_string = true;
            } else if (character == '{' || character == '[') {
                nesting++;
            } else if (character == '}' || character == ']') {
                if (nesting > 0)
                    nesting--;
            } else if (character == ',' && nesting == 0) {
                cursor++;
                break;
            }
        }
    }

    if (member_count == 0) {
        set_error(error, RTE_ERROR_ARGUMENT, path,
                  "object must contain at least one supported field");
        return -EINVAL;
    }
    return 0;
}


static int parse_rf_patch(struct mg_str json, const char *prefix,
                           struct rte_rf_patch *patch, struct rte_error *error)
{
    static const char *const fields[] = {"carrier_hz", "bandwidth_hz", "tx_gain_db", "rx_gain_db"};
    char path[128];
    int status;
    memset(patch, 0, sizeof(*patch));
    status = validate_object_keys(json, prefix, fields, 4, error);
    if (status != 0) return status;
#define RF_FIELD(name, type) do { \
    if (make_path(path, sizeof(path), prefix, #name) != 0) return -EINVAL; \
    status = optional_##type(json, path, &patch->has_##name, &patch->name, error); \
    if (status != 0) return status; \
} while (0)
    RF_FIELD(carrier_hz, u64);
    RF_FIELD(bandwidth_hz, u64);
    RF_FIELD(tx_gain_db, number);
    RF_FIELD(rx_gain_db, number);
#undef RF_FIELD
    if (!patch->has_carrier_hz && !patch->has_bandwidth_hz &&
        !patch->has_tx_gain_db && !patch->has_rx_gain_db) {
        set_error(error, RTE_ERROR_ARGUMENT, prefix, "RF fields are required");
        return -EINVAL;
    }
    return 0;
}

static int parse_target_config(struct mg_str json, const char *prefix,
                                struct rte_config *config, struct rte_error *error)
{
    static const char *const fields[] = {"enabled", "range_m", "radial_velocity_mps", "gain_linear", "phase_offset_deg"};
    char path[128];
    bool present;
    int status;
    memset(config, 0, sizeof(*config));
    status = validate_object_keys(json, prefix, fields, 5, error);
    if (status != 0) return status;
    if (make_path(path, sizeof(path), prefix, "enabled") != 0) return -EINVAL;
    if (!mg_json_get_bool(json, path, &config->enabled)) goto missing;
#define TARGET_FIELD(name) do { \
    if (make_path(path, sizeof(path), prefix, #name) != 0) return -EINVAL; \
    status = optional_number(json, path, &present, &config->name, error); \
    if (status != 0) return status; \
    if (!present) goto missing; \
} while (0)
    TARGET_FIELD(range_m);
    TARGET_FIELD(radial_velocity_mps);
    TARGET_FIELD(gain_linear);
    TARGET_FIELD(phase_offset_deg);
#undef TARGET_FIELD
    return 0;
missing:
    set_error(error, RTE_ERROR_ARGUMENT, path, "all five target fields are required with their declared types");
    return -EINVAL;
}

static int parse_if_match(struct mg_http_message *message,
                          bool *present, uint64_t *revision,
                          struct rte_error *error)
{
    struct mg_str *header = mg_http_get_header(message, "If-Match");
    char value[64];
    char *start;
    char *end;
    unsigned long long parsed;

    *present = false;
    if (header == NULL)
        return 0;
    if (header->len == 0 || header->len >= sizeof(value)) {
        set_error(error, RTE_ERROR_ARGUMENT, "If-Match",
                  "invalid revision header");
        return -EINVAL;
    }
    memcpy(value, header->ptr, header->len);
    value[header->len] = '\0';
    start = value;
    if (*start == '"') {
        start++;
        end = strrchr(start, '"');
        if (end == NULL || end[1] != '\0') {
            set_error(error, RTE_ERROR_ARGUMENT, "If-Match",
                      "invalid quoted revision");
            return -EINVAL;
        }
        *end = '\0';
    }
    if (*start == '\0') {
        set_error(error, RTE_ERROR_ARGUMENT, "If-Match",
                  "revision must be an unsigned integer ETag");
        return -EINVAL;
    }
    for (end = start; *end != '\0'; ++end) {
        if (*end < '0' || *end > '9') {
            set_error(error, RTE_ERROR_ARGUMENT, "If-Match",
                      "revision must be an unsigned integer ETag");
            return -EINVAL;
        }
    }
    errno = 0;
    parsed = strtoull(start, &end, 10);
    if (errno != 0 || end == start || *end != '\0') {
        set_error(error, RTE_ERROR_ARGUMENT, "If-Match",
                  "revision must be an unsigned integer ETag");
        return -EINVAL;
    }
    *revision = (uint64_t)parsed;
    *present = true;
    return 0;
}

static int snapshot_json(struct rte_http_app *app, char *body,
                         size_t body_size, struct rte_snapshot *snapshot,
                         struct rte_error *error)
{
    struct json_buffer json = {body, body_size, 0, false};
    int status = rte_controller_snapshot(
        &app->controller, snapshot, error);

    if (status != 0)
        return status;
    json_append_snapshot(&json, snapshot, app);
    if (json.overflow) {
        set_error(error, RTE_ERROR_STATE, "response",
                  "JSON response buffer is too small");
        return -EOVERFLOW;
    }
    return 0;
}

static int persist_snapshot(struct rte_http_app *app,
                            const struct rte_snapshot *snapshot)
{
    char body[4096];
    char temporary[PATH_MAX];
    char directory[PATH_MAX];
    char *separator;
    struct json_buffer json = {body, sizeof(body), 0, false};
    size_t total;
    ssize_t written;
    int descriptor = -1;
    int directory_fd = -1;
    int status = -EIO;
    struct statfs state_filesystem;
    struct rte_rf_context config = rf_config_for_snapshot(app, snapshot);

    if (app->state_path == NULL || app->state_path[0] == '\0')
        return 0;
    if (app->require_jffs2 &&
        (statfs("/mnt/jffs2", &state_filesystem) != 0 ||
         (unsigned long)state_filesystem.f_type !=
             (unsigned long)JFFS2_SUPER_MAGIC))
        return -ENODEV;

    json_append(&json, "{\"schema_version\":2,\"profile\":\"multitarget-v2\",\"hardware_build_id\":%" PRIu32 ",\"rf\":", snapshot->hardware_build_id);
    json_append_rf_config(&json, &config);
    json_append(&json, ",\"targets\":{");
    for (unsigned int i = 0; i < RTE_TARGET_COUNT; ++i) {
        json_append(&json, "%s\"%u\":", i ? "," : "", i + 1);
        if (app->has_saved[i]) json_append_config(&json, &app->saved[i]);
        else json_append(&json, "null");
    }
    json_append(&json, "}}");
    json_append(&json, "\n");
    if (json.overflow)
        return -EOVERFLOW;
    if (snprintf(temporary, sizeof(temporary), "%s.tmp.%ld",
                 app->state_path, (long)getpid()) >=
        (int)sizeof(temporary))
        return -ENAMETOOLONG;
    if (snprintf(directory, sizeof(directory), "%s", app->state_path) >=
        (int)sizeof(directory))
        return -ENAMETOOLONG;
    separator = strrchr(directory, '/');
    if (separator != NULL && separator != directory) {
        *separator = '\0';
        if (mkdir(directory, 0700) != 0 && errno != EEXIST)
            return -errno;
    } else {
        snprintf(directory, sizeof(directory), ".");
    }

    descriptor = open(temporary,
                      O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                      0600);
    if (descriptor < 0)
        return -errno;
    total = 0;
    while (total < json.used) {
        written = write(descriptor, body + total, json.used - total);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            status = -errno;
            goto out;
        }
        if (written == 0) {
            status = -EIO;
            goto out;
        }
        total += (size_t)written;
    }
    if (fsync(descriptor) != 0) {
        status = -errno;
        goto out;
    }
    if (close(descriptor) != 0) {
        descriptor = -1;
        status = -errno;
        goto out;
    }
    descriptor = -1;
    if (rename(temporary, app->state_path) != 0) {
        status = -errno;
        goto out;
    }
    directory_fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_fd < 0) {
        status = -errno;
        goto out;
    }
    if (fsync(directory_fd) != 0) {
        status = -errno;
        goto out;
    }
    status = 0;

out:
    if (descriptor >= 0)
        (void)close(descriptor);
    if (directory_fd >= 0)
        (void)close(directory_fd);
    if (status != 0)
        (void)unlink(temporary);
    return status;
}

static void update_persistence_warning(struct rte_http_app *app, int status)
{
    if (status == 0) {
        app->warning[0] = '\0';
    } else {
        snprintf(app->warning, sizeof(app->warning),
                 "hardware applied, but persistence failed: %s",
                 strerror(-status));
    }
}

static void schedule_persistence(struct rte_http_app *app,
                                 const struct rte_snapshot *snapshot)
{
    if (app->state_path == NULL || app->state_path[0] == '\0')
        return;

    app->pending_snapshot = *snapshot;
    app->persistence_dirty = true;
    app->persistence_deadline_ms =
        mg_millis() + RTE_PERSIST_DEBOUNCE_MS;
    snprintf(app->warning, sizeof(app->warning),
             "hardware applied; configuration persistence is pending");
}

static void flush_persistence(struct rte_http_app *app, bool force)
{
    int status;

    if (!app->persistence_dirty)
        return;
    if (!force && mg_millis() < app->persistence_deadline_ms)
        return;

    status = persist_snapshot(app, &app->pending_snapshot);
    app->persistence_dirty = false;
    update_persistence_warning(app, status);
}

static void handle_get_snapshot(struct mg_connection *connection,
                                struct rte_http_app *app)
{
    char body[RTE_JSON_RESPONSE_MAX];
    struct rte_snapshot snapshot;
    struct rte_error error;
    int status = snapshot_json(app, body, sizeof(body), &snapshot, &error);

    if (status != 0)
        reply_error(connection, &error, 0);
    else
        reply_json(connection, 200, body, snapshot.revision);
}

static void handle_health(struct mg_connection *connection,
                          struct rte_http_app *app)
{
    struct rte_snapshot snapshot;
    struct rte_error error;
    int status = rte_controller_snapshot(
        &app->controller, &snapshot, &error);

    if (status != 0) {
        reply_error(connection, &error, 0);
    } else if (snapshot.degraded) {
        reply_json(connection, 503,
                   "{\"status\":\"degraded\","
                   "\"hardware_state_known\":false}",
                   snapshot.revision);
    } else {
        reply_json(
            connection, 200,
            all_targets_known(&snapshot)
                ? "{\"status\":\"ok\","
                  "\"hardware_state_known\":true}"
                : "{\"status\":\"ok\","
                  "\"hardware_state_known\":false}",
            snapshot.revision);
    }
}


static void handle_capabilities(struct mg_connection *connection, struct rte_http_app *app, bool rf)
{
    char body[1536];
    struct json_buffer json = {body, sizeof(body), 0, false};
    struct rte_snapshot snapshot = {0};
    struct rte_error error;
    int status = rte_controller_snapshot(&app->controller, &snapshot, &error);
    if (status != 0) { reply_error(connection, &error, snapshot.revision); return; }
    json_append(&json, "{\"revision\":%" PRIu64, snapshot.revision);
    if (rf) {
        struct rte_rf_capabilities c;
        status = rte_controller_rf_capabilities(&app->controller, &c, &error);
        if (status == 0)
            status = rte_controller_snapshot(&app->controller, &snapshot, &error);
        if (status != 0) { reply_error(connection, &error, snapshot.revision); return; }
        /* Capabilities reads may observe an external LO change. */
        json.used = 0;
        json_append(&json, "{\"revision\":%" PRIu64, snapshot.revision);
        json_append(&json, ",\"carrier_hz\":{\"min\":%" PRIu64 ",\"max\":%" PRIu64
                    ",\"step\":1,\"readback_tolerance_hz\":%" PRIu64 "},"
                    "\"bandwidth_hz\":{\"min\":%" PRIu64 ",\"max\":%" PRIu64 ",\"step\":1},"
                    "\"tx_gain_db\":{\"min\":%.3f,\"max\":%.3f,\"step\":%.3f},"
                    "\"rx_gain_db\":{\"min\":%.3f,\"max\":%.3f,\"step\":%.3f},"
                    "\"sample_rate_hz\":%" PRIu64 ",\"sample_rate_writable\":false,\"gain_control_mode\":\"manual\"}",
                    c.min_carrier_hz, c.max_carrier_hz, RTE_AD936X_LO_READBACK_TOLERANCE_HZ,
                    c.min_bandwidth_hz, c.max_bandwidth_hz,
                    c.min_tx_gain_mdb / 1000.0, c.max_tx_gain_mdb / 1000.0, c.tx_gain_step_mdb / 1000.0,
                    c.min_rx_gain_mdb / 1000.0, c.max_rx_gain_mdb / 1000.0, c.rx_gain_step_mdb / 1000.0,
                    RTE_FIXED_SAMPLE_RATE_HZ);
    } else {
        struct rte_capabilities c;
        status = rte_capabilities_for_rf(&snapshot.rf, &c, &error);
        if (status != 0) { reply_error(connection, &error, snapshot.revision); return; }
        json_append(&json, ",\"target_count\":4,\"min_range_m\":%.17g,\"max_range_m\":%.17g,"
                    "\"max_abs_velocity_mps\":%.17g,\"range_resolution_m\":%.17g,"
                    "\"phase_resolution_rad\":%.17g,\"frequency_resolution_hz\":%.17g,"
                    "\"max_gain_linear\":%.17g,\"fixed_latency_samples\":%.17g,"
                    "\"rf_calibrated\":%s,\"range_reference\":\"DUT digital input/output\"}",
                    c.min_range_m, c.max_range_m, c.max_abs_velocity_mps, c.range_resolution_m,
                    c.phase_resolution_rad, c.frequency_resolution_hz, c.max_gain_linear,
                    c.fixed_latency_samples, json_bool(c.rf_calibrated));
    }
    reply_json(connection, 200, body, snapshot.revision);
}

static void reply_request_error(struct mg_connection *connection, struct rte_http_app *app,
                                int status, const char *code, const char *field, const char *message)
{
    char body[1024];
    struct json_buffer json = {body, sizeof(body), 0, false};
    struct rte_snapshot snapshot = {0};
    struct rte_error error;
    (void)rte_controller_snapshot(&app->controller, &snapshot, &error);
    json_append(&json, "{\"error\":{\"code\":");
    json_append_escaped(&json, code);
    json_append(&json, ",\"field\":");
    json_append_escaped(&json, field);
    json_append(&json, ",\"message\":");
    json_append_escaped(&json, message);
    json_append(&json, "},\"revision\":%" PRIu64 "}", snapshot.revision);
    reply_json(connection, status, body, snapshot.revision);
}

static void handle_mutation(struct mg_connection *connection, struct mg_http_message *message,
                             struct rte_http_app *app, int target_index)
{
    struct rte_error error;
    struct rte_snapshot snapshot = {0};
    struct rte_config config;
    struct rte_rf_patch patch;
    char body[RTE_JSON_RESPONSE_MAX];
    struct json_buffer json = {body, sizeof(body), 0, false};
    bool has_revision;
    uint64_t revision = 0;
    int status;
    rte_error_clear(&error);
    if (message->body.len > RTE_HTTP_BODY_MAX) {
        reply_request_error(connection, app, 413, "body_too_large", "body", "request body exceeds 16384 bytes");
        return;
    }
    status = parse_if_match(message, &has_revision, &revision, &error);
    if (status != 0) goto failure;
    if (!has_revision) {
        reply_request_error(connection, app, 428, "precondition_required", "If-Match",
                            "read the current revision and supply If-Match");
        return;
    }
    if (json_root_object(message->body, &error) != 0) goto failure;
    if (target_index < 0) {
        if (parse_rf_patch(message->body, "$", &patch, &error) != 0) goto failure;
        status = rte_controller_apply_rf(&app->controller, &patch, true, revision, &snapshot, &error);
    } else {
        if (parse_target_config(message->body, "$", &config, &error) != 0) goto failure;
        status = rte_controller_apply_target(&app->controller, (unsigned int)target_index,
                                              &config, true, revision, &snapshot, &error);
    }
    if (status != 0) goto failure;
    if (target_index >= 0) {
        app->has_saved[target_index] = true;
        app->saved[target_index] = config;
    } else {
        remember_rf_request(app, &patch, &snapshot);
    }
    schedule_persistence(app, &snapshot);
    json_append_snapshot(&json, &snapshot, app);
    if (json.overflow) {
        set_error(&error, RTE_ERROR_STATE, "response", "applied; response overflow, read status before another apply");
        goto failure;
    }
    reply_json(connection, 200, body, snapshot.revision);
    return;
failure:
    {
        struct rte_error snapshot_error;
        (void)rte_controller_snapshot(&app->controller, &snapshot, &snapshot_error);
    }
    reply_error(connection, &error, snapshot.revision);
}

static bool method_is(const struct mg_http_message *message,
                      const char *method)
{
    return mg_vcmp(&message->method, method) == 0;
}

static bool api_uri(const struct mg_http_message *message)
{
    return mg_http_match_uri(message, "/api") ||
           mg_http_match_uri(message, "/api/#");
}

static bool health_uri(const struct mg_http_message *message)
{
    return mg_http_match_uri(message, "/healthz") ||
           mg_http_match_uri(message, "/healthz/#");
}

static bool static_method_is_allowed(const struct mg_http_message *message)
{
    return method_is(message, "GET") || method_is(message, "HEAD");
}


/* Match exactly one public ID; reject lists, zero, encoded IDs and suffixes. */
static int target_uri(const struct mg_http_message *message, bool load_param)
{
    char path[80];
    unsigned int i;
    for (i = 0; i < RTE_TARGET_COUNT; ++i) {
        snprintf(path, sizeof(path), "/api/v1/rte/targets/%u%s", i + 1,
                 load_param ? "/load-param" : "");
        if (mg_vcmp(&message->uri, path) == 0) return (int)i;
    }
    return -1;
}

static enum endpoint_methods methods_for_known_uri(const struct mg_http_message *message)
{
    if (mg_http_match_uri(message, "/api/v1/rf/config")) return ENDPOINT_GET_PATCH;
    if (target_uri(message, true) >= 0) return ENDPOINT_POST;
    if (mg_http_match_uri(message, "/healthz") || mg_http_match_uri(message, "/api/v1") ||
        mg_http_match_uri(message, "/api/v1/system") || mg_http_match_uri(message, "/api/v1/rte/config") ||
        mg_http_match_uri(message, "/api/v1/rte/status") || mg_http_match_uri(message, "/api/v1/rte/capabilities") ||
        mg_http_match_uri(message, "/api/v1/rf/capabilities") || target_uri(message, false) >= 0)
        return ENDPOINT_GET;
    return ENDPOINT_UNKNOWN;
}

static void reply_method_not_allowed(struct mg_connection *connection,
                                     enum endpoint_methods methods)
{
    const char *headers;

    if (methods == ENDPOINT_GET_PATCH)
        headers = RTE_HTTP_JSON_HEADERS "Allow: GET, PATCH\r\n";
    else if (methods == ENDPOINT_POST)
        headers = RTE_HTTP_JSON_HEADERS "Allow: POST\r\n";
    else
        headers = RTE_HTTP_JSON_HEADERS "Allow: GET\r\n";
    mg_http_reply(connection, 405, headers,
                  "{\"error\":{\"code\":\"method_not_allowed\","
                  "\"message\":\"method not allowed\"}}\n");
}

static void serve_static(struct mg_connection *connection,
                         struct mg_http_message *message,
                         const struct rte_http_app *app)
{
    const struct mg_http_serve_opts options = {
        .root_dir = app->web_root,
        .extra_headers = RTE_HTTP_STATIC_HEADERS,
    };

    mg_http_serve_dir(connection, message, &options);
}


static void http_handler(struct mg_connection *connection, int event,
                         void *event_data, void *function_data)
{
    struct rte_http_app *app = function_data;
    struct mg_http_message *message = event_data;
    enum endpoint_methods methods;
    int target_index;
    if (event != MG_EV_HTTP_MSG) return;
    methods = methods_for_known_uri(message);
    if (method_is(message, "GET") && mg_http_match_uri(message, "/healthz")) {
        handle_health(connection, app);
    } else if (method_is(message, "GET") && mg_http_match_uri(message, "/api/v1")) {
        reply_json(connection, 200, "{\"api_version\":\"v1\",\"hardware_profile\":\"multitarget-v2\","
                   "\"endpoints\":[\"/api/v1/system\",\"/api/v1/rf/config\",\"/api/v1/rf/capabilities\","
                   "\"/api/v1/rte/config\",\"/api/v1/rte/status\",\"/api/v1/rte/capabilities\","
                   "\"/api/v1/rte/targets/{id}/load-param\"]}", 0);
    } else if (method_is(message, "GET") && mg_http_match_uri(message, "/api/v1/rf/capabilities")) {
        handle_capabilities(connection, app, true);
    } else if (method_is(message, "GET") && mg_http_match_uri(message, "/api/v1/rte/capabilities")) {
        handle_capabilities(connection, app, false);
    } else if (method_is(message, "GET") && (methods == ENDPOINT_GET || methods == ENDPOINT_GET_PATCH)) {
        handle_get_snapshot(connection, app);
    } else if (method_is(message, "PATCH") && mg_http_match_uri(message, "/api/v1/rf/config")) {
        handle_mutation(connection, message, app, -1);
    } else if (method_is(message, "POST") && (target_index = target_uri(message, true)) >= 0) {
        handle_mutation(connection, message, app, target_index);
    } else if (methods != ENDPOINT_UNKNOWN) {
        reply_method_not_allowed(connection, methods);
    } else if (api_uri(message) || health_uri(message)) {
        mg_http_reply(connection, 404, RTE_HTTP_JSON_HEADERS,
                      "{\"error\":{\"code\":\"not_found\",\"message\":\"endpoint not found\"}}\n");
    } else if (!static_method_is_allowed(message)) {
        mg_http_reply(connection, 405, RTE_HTTP_SECURITY_HEADERS "Allow: GET, HEAD\r\n", "Method not allowed\n");
    } else {
        serve_static(connection, message, app);
    }
}

static int read_state_file(const char *path, char *buffer,
                           size_t buffer_size, size_t *length)
{
    ssize_t result;
    size_t total = 0;
    int descriptor;

    descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return -errno;
    while (total + 1 < buffer_size) {
        result = read(descriptor, buffer + total,
                      buffer_size - total - 1);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            (void)close(descriptor);
            return -errno;
        }
        if (result == 0)
            break;
        total += (size_t)result;
    }
    (void)close(descriptor);
    if (total + 1 == buffer_size)
        return -EFBIG;
    buffer[total] = '\0';
    *length = total;
    return 0;
}


static bool json_literal_at(struct mg_str json, const char *path, const char *literal)
{
    int length = 0, offset = mg_json_get(json, path, &length);
    return offset >= 0 && (size_t)length == strlen(literal) &&
        memcmp(json.ptr + offset, literal, (size_t)length) == 0;
}

static void restore_persisted_config(struct rte_http_app *app)
{
    static const char *const root_fields[] = {"schema_version", "profile", "hardware_build_id", "rf", "targets"};
    static const char *const target_fields[] = {"1", "2", "3", "4"};
    char body[4096], path[32];
    struct mg_str json;
    struct rte_rf_patch rf;
    struct rte_snapshot snapshot;
    struct rte_config saved[RTE_TARGET_COUNT] = {0};
    bool has_saved[RTE_TARGET_COUNT] = {false}, present;
    struct rte_error error = {0};
    uint64_t timestamp = 0;
    size_t length = 0;
    unsigned int i;
    int status;
    if (app->state_path == NULL || app->state_path[0] == '\0') return;
    status = read_state_file(app->state_path, body, sizeof(body), &length);
    if (status == -ENOENT) return;
    if (status != 0) {
        snprintf(app->warning, sizeof(app->warning), "cannot read persisted config: %s", strerror(-status));
        return;
    }
    json = mg_str_n(body, length);
    if (json_root_object(json, &error) != 0 ||
        validate_object_keys(json, "$", root_fields, 5, &error) != 0 ||
        !json_literal_at(json, "$.schema_version", "2") ||
        !json_literal_at(json, "$.profile", "\"multitarget-v2\"") ||
        optional_u64(json, "$.hardware_build_id", &present, &timestamp, &error) != 0 ||
        !present || timestamp != RTE_EXPECTED_TIMESTAMP ||
        parse_rf_patch(json, "$.rf", &rf, &error) != 0 ||
        !rf.has_carrier_hz || !rf.has_bandwidth_hz || !rf.has_tx_gain_db || !rf.has_rx_gain_db ||
        validate_object_keys(json, "$.targets", target_fields, 4, &error) != 0)
        goto invalid;
    for (i = 0; i < RTE_TARGET_COUNT; ++i) {
        snprintf(path, sizeof(path), "$.targets.%u", i + 1);
        if (json_literal_at(json, path, "null")) continue;
        if (parse_target_config(json, path, &saved[i], &error) != 0) goto invalid;
        has_saved[i] = true;
    }
    /* Validate carrier-independent draft limits before hardware action. A saved
     * draft may require editing after an RF-only retune: do not reject the whole
     * file for carrier-dependent Doppler limits. Full encode runs on loadParam. */
    if (rte_controller_snapshot(&app->controller, &snapshot, &error) != 0) goto invalid;
    for (i = 0; i < RTE_TARGET_COUNT; ++i) {
        struct rte_capabilities caps;
        if (!has_saved[i]) continue;
        if (rte_capabilities_for_rf(&snapshot.rf, &caps, &error) != 0) goto invalid;
        if (saved[i].range_m < caps.min_range_m || saved[i].range_m > caps.max_range_m ||
            saved[i].gain_linear < 0 || saved[i].gain_linear > caps.max_gain_linear) {
            set_error(&error, RTE_ERROR_RANGE, "saved", "saved distance or digital gain exceeds this hardware profile");
            goto invalid;
        }
    }
    if (rte_controller_apply_rf(&app->controller, &rf, false, 0, &snapshot, &error) != 0) {
        snprintf(app->warning, sizeof(app->warning), "persisted RF was not restored: %.180s", error.message);
        return;
    }
    memcpy(app->has_saved, has_saved, sizeof(has_saved));
    memcpy(app->saved, saved, sizeof(saved));
    remember_rf_request(app, &rf, &snapshot);
    snprintf(app->warning, sizeof(app->warning), "saved targets restored as drafts; apply each target loadParam explicitly");
    return;
invalid:
    snprintf(app->warning, sizeof(app->warning), "persisted config rejected (profile/build/schema/value): %.160s", error.message);
}

static void usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "Usage: %s [--listen URL] [--device PATH] [--web-root PATH] "
            "[--state PATH|--no-persist] [--log-level 0..4]\n",
            program);
}

int main(int argc, char **argv)
{
    const char *listen_url = "http://127.0.0.1:8080";
    const char *device_path = "/dev/mwipcore0";
    const char *state_path = RTE_DEFAULT_STATE_PATH;
    const char *web_root = RTE_DEFAULT_WEB_ROOT;
    struct rte_http_app app;
    struct rte_error error;
    struct mg_mgr manager;
    struct mg_connection *listener;
    struct stat web_root_status;
    struct statfs state_filesystem;
    bool state_path_explicit = false;
    int log_level = MG_LL_INFO;
    int status;
    int index;

    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--listen") == 0 && index + 1 < argc) {
            listen_url = argv[++index];
        } else if (strcmp(argv[index], "--device") == 0 &&
                   index + 1 < argc) {
            device_path = argv[++index];
        } else if (strcmp(argv[index], "--web-root") == 0 &&
                   index + 1 < argc) {
            web_root = argv[++index];
        } else if (strcmp(argv[index], "--state") == 0 &&
                   index + 1 < argc) {
            state_path = argv[++index];
            state_path_explicit = true;
        } else if (strcmp(argv[index], "--no-persist") == 0) {
            state_path = NULL;
            state_path_explicit = true;
        } else if (strcmp(argv[index], "--log-level") == 0 &&
                   index + 1 < argc) {
            log_level = atoi(argv[++index]);
            if (log_level < 0 || log_level > 4) {
                usage(stderr, argv[0]);
                return EXIT_FAILURE;
            }
        } else {
            usage(stderr, argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (web_root[0] == '\0' || stat(web_root, &web_root_status) != 0 ||
        !S_ISDIR(web_root_status.st_mode)) {
        fprintf(stderr, "rte-httpd: web root is not a directory: %s\n",
                web_root[0] != '\0' ? web_root : "(empty)");
        return EXIT_FAILURE;
    }

    memset(&app, 0, sizeof(app));
    if (state_path != NULL && !state_path_explicit &&
        (statfs("/mnt/jffs2", &state_filesystem) != 0 ||
         (unsigned long)state_filesystem.f_type !=
             (unsigned long)JFFS2_SUPER_MAGIC)) {
        state_path = NULL;
        snprintf(app.warning, sizeof(app.warning),
                 "persistence disabled: /mnt/jffs2 is not a JFFS2 mount");
    }
    if (state_path == NULL && app.warning[0] == '\0')
        snprintf(app.warning, sizeof(app.warning),
                 "persistence disabled by configuration");
    app.state_path = state_path;
    app.web_root = web_root;
    app.require_jffs2 = state_path != NULL &&
        strcmp(state_path, RTE_DEFAULT_STATE_PATH) == 0;
    status = rte_controller_open(&app.controller, device_path, &error);
    if (status != 0) {
        fprintf(stderr, "rte-httpd: %s (%s): %s\n",
                rte_error_code_name(error.code), error.field,
                error.message);
        return EXIT_FAILURE;
    }
    restore_persisted_config(&app);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    mg_log_set(log_level);
    mg_mgr_init(&manager);
    listener = mg_http_listen(
        &manager, listen_url, http_handler, &app);
    if (listener == NULL) {
        fprintf(stderr, "rte-httpd: cannot listen on %s\n", listen_url);
        mg_mgr_free(&manager);
        rte_controller_close(&app.controller);
        return EXIT_FAILURE;
    }

    fprintf(stderr, "rte-httpd: listening on %s\n", listen_url);
    fprintf(stderr, "rte-httpd: serving web UI from %s\n", web_root);
    if (app.warning[0] != '\0')
        fprintf(stderr, "rte-httpd: warning: %s\n", app.warning);
    while (stop_signal == 0) {
        mg_mgr_poll(&manager, 250);
        flush_persistence(&app, false);
    }

    flush_persistence(&app, true);
    if (app.warning[0] != '\0')
        fprintf(stderr, "rte-httpd: warning: %s\n", app.warning);
    mg_mgr_free(&manager);
    rte_controller_close(&app.controller);
    return EXIT_SUCCESS;
}
