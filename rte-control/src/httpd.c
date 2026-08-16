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
#define RTE_JSON_RESPONSE_MAX 12288U
#define RTE_PERSIST_DEBOUNCE_MS 2000U
#define RTE_DEFAULT_STATE_PATH "/mnt/jffs2/rte/last-config.json"
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
    uint64_t persistence_deadline_ms;
    bool persistence_dirty;
    bool require_jffs2;
    char warning[256];
};

enum patch_kind {
    PATCH_RF,
    PATCH_RTE,
    PATCH_COMBINED,
};

enum endpoint_methods {
    ENDPOINT_UNKNOWN,
    ENDPOINT_GET,
    ENDPOINT_PATCH,
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

static void json_append_motion_config(
    struct json_buffer *buffer, const struct rte_motion_config *motion)
{
    json_append(buffer,
                "{\"amplitude_m\":%.17g,\"frequency_hz\":%.17g,"
                "\"phase_rad\":%.17g}",
                motion->amplitude_m, motion->frequency_hz,
                motion->phase_rad);
}

static void json_append_config(struct json_buffer *buffer,
                               const struct rte_config *config)
{
    json_append(buffer,
                "{\"range_m\":%.17g,\"radial_velocity_mps\":%.17g,"
                "\"loss_db\":%.17g,\"respiration\":",
                config->range_m, config->radial_velocity_mps,
                config->loss_db);
    json_append_motion_config(buffer, &config->respiration);
    json_append(buffer, ",\"heartbeat\":");
    json_append_motion_config(buffer, &config->heartbeat);
    json_append(buffer, "}");
}

static void json_append_rf(struct json_buffer *buffer,
                           const struct rte_rf_context *rf)
{
    json_append(buffer,
                "{\"sample_rate_hz\":%" PRIu64 ","
                "\"carrier_hz\":%" PRIu64 ","
                "\"rx_lo_hz\":%" PRIu64 ","
                "\"tx_lo_hz\":%" PRIu64 "}",
                rf->sample_rate_hz, rf->rx_lo_hz,
                rf->rx_lo_hz, rf->tx_lo_hz);
}

static void json_append_motion_applied(
    struct json_buffer *buffer, const struct rte_motion_applied *motion)
{
    json_append(buffer,
                "{\"amplitude_m\":%.17g,\"frequency_hz\":%.17g,"
                "\"phase_rad\":%.17g,\"phase_gain_cycles\":%.17g,"
                "\"max_doppler_hz\":%.17g}",
                motion->amplitude_m, motion->frequency_hz,
                motion->phase_rad, motion->phase_gain_cycles,
                motion->max_doppler_hz);
}

static void json_append_registers(
    struct json_buffer *buffer, const struct rte_register_image *image)
{
    size_t index;

    json_append(buffer, "{");
    for (index = 0; index < rte_parameter_register_count; ++index) {
        const struct rte_register_descriptor *reg =
            &rte_parameter_registers[index];
        json_append(buffer, "%s\"%s\":\"0x%08" PRIx32 "\"",
                    index == 0 ? "" : ",", reg->name,
                    rte_register_image_word(image, reg->offset));
    }
    json_append(buffer, "}");
}

static void json_append_snapshot(struct json_buffer *buffer,
                                 const struct rte_snapshot *snapshot,
                                 const char *warning)
{
    json_append(buffer,
                "{\"revision\":%" PRIu64 ","
                "\"hardware_build_id\":%" PRIu32 ","
                "\"hardware_state_known\":%s,\"rf\":",
                snapshot->revision, snapshot->hardware_build_id,
                snapshot->has_config && !snapshot->degraded
                    ? "true" : "false");
    json_append_rf(buffer, &snapshot->rf);
    json_append(buffer, ",\"requested\":");
    if (snapshot->has_config)
        json_append_config(buffer, &snapshot->requested);
    else
        json_append(buffer, "null");

    json_append(buffer, ",\"applied\":");
    if (!snapshot->has_config) {
        json_append(buffer, "null");
    } else {
        json_append(buffer,
                    "{\"range\":{\"delay_samples\":%u,"
                    "\"delay_range_m\":%.17g,"
                    "\"phase_rad\":%.17g},"
                    "\"doppler_hz\":%.17g,"
                    "\"radial_velocity_mps\":%.17g,"
                    "\"scaling_raw\":%d,"
                    "\"linear_gain\":%.17g,\"muted\":%s,"
                    "\"loss_db\":",
                    snapshot->applied.delay_samples,
                    snapshot->applied.delay_range_m,
                    snapshot->applied.range_phase_rad,
                    snapshot->applied.doppler_hz,
                    snapshot->applied.radial_velocity_mps,
                    snapshot->applied.scaling_raw,
                    snapshot->applied.linear_gain,
                    snapshot->applied.muted ? "true" : "false");
        if (snapshot->applied.muted)
            json_append(buffer, "null");
        else
            json_append(buffer, "%.17g", snapshot->applied.loss_db);
        json_append(buffer, ",\"respiration\":");
        json_append_motion_applied(buffer, &snapshot->applied.respiration);
        json_append(buffer, ",\"heartbeat\":");
        json_append_motion_applied(buffer, &snapshot->applied.heartbeat);
        json_append(buffer, ",\"registers\":");
        json_append_registers(buffer, &snapshot->image);
        json_append(buffer, "}");
    }

    json_append(buffer, ",\"warning\":");
    if (warning != NULL && warning[0] != '\0')
        json_append_escaped(buffer, warning);
    else
        json_append(buffer, "null");
    json_append(buffer, "}");
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

static int json_root_object(struct mg_str json, struct rte_error *error)
{
    int token_length = 0;
    int offset = mg_json_get(json, "$", &token_length);
    size_t end;

    if (offset < 0 || token_length <= 1 || json.ptr[offset] != '{') {
        set_error(error, RTE_ERROR_ARGUMENT, "body",
                  "request body must be a valid JSON object");
        return -EINVAL;
    }
    end = (size_t)offset + (size_t)token_length;
    while (end < json.len &&
           (json.ptr[end] == ' ' || json.ptr[end] == '\t' ||
            json.ptr[end] == '\r' || json.ptr[end] == '\n'))
        end++;
    if (end != json.len) {
        set_error(error, RTE_ERROR_ARGUMENT, "body",
                  "unexpected data after the JSON object");
        return -EINVAL;
    }
    return 0;
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

static bool json_whitespace(char character)
{
    return character == ' ' || character == '\t' ||
        character == '\r' || character == '\n';
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

static int validate_rte_shape(struct mg_str json, const char *prefix,
                              struct rte_error *error)
{
    static const char *const rte_fields[] = {
        "range_m", "radial_velocity_mps", "loss_db",
        "respiration", "heartbeat",
    };
    static const char *const motion_fields[] = {
        "amplitude_m", "frequency_hz", "phase_rad",
    };
    char path[128];
    int status;

    status = validate_object_keys(
        json, prefix, rte_fields,
        sizeof(rte_fields) / sizeof(rte_fields[0]), error);
    if (status != 0)
        return status;
    if (make_path(path, sizeof(path), prefix, "respiration") != 0)
        return -ENAMETOOLONG;
    status = validate_object_keys(
        json, path, motion_fields,
        sizeof(motion_fields) / sizeof(motion_fields[0]), error);
    if (status != 0)
        return status;
    if (make_path(path, sizeof(path), prefix, "heartbeat") != 0)
        return -ENAMETOOLONG;
    return validate_object_keys(
        json, path, motion_fields,
        sizeof(motion_fields) / sizeof(motion_fields[0]), error);
}

static int validate_patch_shape(struct mg_str json, enum patch_kind kind,
                                struct rte_error *error)
{
    static const char *const rf_fields[] = {
        "sample_rate_hz", "carrier_hz",
    };
    static const char *const combined_fields[] = {"rf", "rte"};
    int status;

    if (kind == PATCH_RF)
        return validate_object_keys(
            json, "$", rf_fields,
            sizeof(rf_fields) / sizeof(rf_fields[0]), error);
    if (kind == PATCH_RTE)
        return validate_rte_shape(json, "$", error);

    status = validate_object_keys(
        json, "$", combined_fields,
        sizeof(combined_fields) / sizeof(combined_fields[0]), error);
    if (status != 0)
        return status;
    status = validate_object_keys(
        json, "$.rf", rf_fields,
        sizeof(rf_fields) / sizeof(rf_fields[0]), error);
    if (status != 0)
        return status;
    return validate_rte_shape(json, "$.rte", error);
}

static int parse_motion_patch(struct mg_str json, const char *prefix,
                              struct rte_motion_patch *patch,
                              bool *found, struct rte_error *error)
{
    char path[128];
    int status;

    memset(patch, 0, sizeof(*patch));
    if (make_path(path, sizeof(path), prefix, "amplitude_m") != 0)
        return -ENAMETOOLONG;
    status = optional_number(json, path, &patch->has_amplitude_m,
                             &patch->amplitude_m, error);
    if (status != 0)
        return status;
    if (make_path(path, sizeof(path), prefix, "frequency_hz") != 0)
        return -ENAMETOOLONG;
    status = optional_number(json, path, &patch->has_frequency_hz,
                             &patch->frequency_hz, error);
    if (status != 0)
        return status;
    if (make_path(path, sizeof(path), prefix, "phase_rad") != 0)
        return -ENAMETOOLONG;
    status = optional_number(json, path, &patch->has_phase_rad,
                             &patch->phase_rad, error);
    if (status != 0)
        return status;
    *found = patch->has_amplitude_m || patch->has_frequency_hz ||
        patch->has_phase_rad;
    return 0;
}

static int parse_config_patch(struct mg_str json, const char *prefix,
                              struct rte_config_patch *patch,
                              bool *found, struct rte_error *error)
{
    char path[128];
    char motion_path[128];
    bool motion_found;
    int status;

    memset(patch, 0, sizeof(*patch));
    if (make_path(path, sizeof(path), prefix, "range_m") != 0)
        return -ENAMETOOLONG;
    status = optional_number(json, path, &patch->has_range_m,
                             &patch->range_m, error);
    if (status != 0)
        return status;
    if (make_path(path, sizeof(path), prefix,
                  "radial_velocity_mps") != 0)
        return -ENAMETOOLONG;
    status = optional_number(json, path,
                             &patch->has_radial_velocity_mps,
                             &patch->radial_velocity_mps, error);
    if (status != 0)
        return status;
    if (make_path(path, sizeof(path), prefix, "loss_db") != 0)
        return -ENAMETOOLONG;
    status = optional_number(json, path, &patch->has_loss_db,
                             &patch->loss_db, error);
    if (status != 0)
        return status;

    if (make_path(motion_path, sizeof(motion_path), prefix,
                  "respiration") != 0)
        return -ENAMETOOLONG;
    status = parse_motion_patch(json, motion_path, &patch->respiration,
                                &motion_found, error);
    if (status != 0)
        return status;
    *found = patch->has_range_m ||
        patch->has_radial_velocity_mps || patch->has_loss_db ||
        motion_found;

    if (make_path(motion_path, sizeof(motion_path), prefix,
                  "heartbeat") != 0)
        return -ENAMETOOLONG;
    status = parse_motion_patch(json, motion_path, &patch->heartbeat,
                                &motion_found, error);
    if (status != 0)
        return status;
    *found = *found || motion_found;
    return 0;
}

static int parse_rf_patch(struct mg_str json, const char *prefix,
                          struct rte_rf_patch *patch, bool *found,
                          struct rte_error *error)
{
    char path[128];
    int status;

    memset(patch, 0, sizeof(*patch));
    if (make_path(path, sizeof(path), prefix, "sample_rate_hz") != 0)
        return -ENAMETOOLONG;
    status = optional_u64(json, path, &patch->has_sample_rate_hz,
                          &patch->sample_rate_hz, error);
    if (status != 0)
        return status;
    if (make_path(path, sizeof(path), prefix, "carrier_hz") != 0)
        return -ENAMETOOLONG;
    status = optional_u64(json, path, &patch->has_carrier_hz,
                          &patch->carrier_hz, error);
    if (status != 0)
        return status;
    *found = patch->has_sample_rate_hz || patch->has_carrier_hz;
    return 0;
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
    json_append_snapshot(&json, snapshot, app->warning);
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
    char body[2048];
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

    if (app->state_path == NULL || app->state_path[0] == '\0' ||
        !snapshot->has_config)
        return 0;
    if (app->require_jffs2 &&
        (statfs("/mnt/jffs2", &state_filesystem) != 0 ||
         (unsigned long)state_filesystem.f_type !=
             (unsigned long)JFFS2_SUPER_MAGIC))
        return -ENODEV;

    json_append_config(&json, &snapshot->requested);
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
    if (app->state_path == NULL || app->state_path[0] == '\0' ||
        !snapshot->has_config)
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
            snapshot.has_config
                ? "{\"status\":\"ok\","
                  "\"hardware_state_known\":true}"
                : "{\"status\":\"ok\","
                  "\"hardware_state_known\":false}",
            snapshot.revision);
    }
}

static void handle_capabilities(struct mg_connection *connection,
                                struct rte_http_app *app)
{
    char body[1024];
    struct rte_snapshot snapshot = {0};
    struct rte_capabilities capabilities;
    struct rte_error error;
    int status = rte_controller_snapshot(
        &app->controller, &snapshot, &error);

    if (status == 0)
        status = rte_capabilities_for_rf(
            &snapshot.rf, &capabilities, &error);
    if (status != 0) {
        reply_error(connection, &error, snapshot.revision);
        return;
    }
    snprintf(body, sizeof(body),
             "{\"revision\":%" PRIu64 ","
             "\"max_range_m\":%.17g,"
             "\"max_abs_velocity_mps\":%.17g,"
             "\"phase_resolution_rad\":%.17g,"
             "\"frequency_resolution_hz\":%.17g,"
             "\"max_motion_amplitude_m\":%.17g}",
             snapshot.revision, capabilities.max_range_m,
             capabilities.max_abs_velocity_mps,
             capabilities.phase_resolution_rad,
             capabilities.frequency_resolution_hz,
             capabilities.max_motion_amplitude_m);
    reply_json(connection, 200, body, snapshot.revision);
}

static void handle_patch(struct mg_connection *connection,
                         struct mg_http_message *message,
                         struct rte_http_app *app,
                         enum patch_kind kind)
{
    char body[RTE_JSON_RESPONSE_MAX];
    struct rte_rf_patch rf_patch;
    struct rte_config_patch config_patch;
    struct rte_snapshot snapshot = {0};
    struct rte_error error;
    struct rte_error original_error;
    struct rte_error snapshot_error;
    const struct rte_rf_patch *rf_argument = NULL;
    const struct rte_config_patch *config_argument = NULL;
    bool rf_found = false;
    bool config_found = false;
    bool has_expected_revision;
    uint64_t expected_revision = 0;
    int status;

    rte_error_clear(&error);
    if (message->body.len > RTE_HTTP_BODY_MAX) {
        set_error(&error, RTE_ERROR_ARGUMENT, "body",
                  "request body exceeds %u bytes", RTE_HTTP_BODY_MAX);
        reply_error(connection, &error, 0);
        return;
    }
    status = json_root_object(message->body, &error);
    if (status != 0)
        goto error_without_revision;
    status = validate_patch_shape(message->body, kind, &error);
    if (status != 0)
        goto error_without_revision;
    status = parse_if_match(message, &has_expected_revision,
                            &expected_revision, &error);
    if (status != 0)
        goto error_without_revision;

    if (kind == PATCH_RF || kind == PATCH_COMBINED) {
        status = parse_rf_patch(
            message->body, kind == PATCH_RF ? "$" : "$.rf",
            &rf_patch, &rf_found, &error);
        if (status != 0)
            goto error_without_revision;
        if (rf_found)
            rf_argument = &rf_patch;
    }
    if (kind == PATCH_RTE || kind == PATCH_COMBINED) {
        status = parse_config_patch(
            message->body, kind == PATCH_RTE ? "$" : "$.rte",
            &config_patch, &config_found, &error);
        if (status != 0)
            goto error_without_revision;
        if (config_found)
            config_argument = &config_patch;
    }
    if (!rf_found && !config_found) {
        set_error(&error, RTE_ERROR_ARGUMENT, "body",
                  "patch contains no supported fields");
        goto error_without_revision;
    }

    status = rte_controller_apply(
        &app->controller, rf_argument, config_argument,
        has_expected_revision, expected_revision, &snapshot, &error);
    if (status != 0)
        goto error_with_revision;
    schedule_persistence(app, &snapshot);

    {
        struct json_buffer json = {body, sizeof(body), 0, false};
        json_append_snapshot(&json, &snapshot, app->warning);
        if (json.overflow) {
            set_error(&error, RTE_ERROR_STATE, "response",
                      "JSON response buffer is too small");
            goto error_with_revision;
        }
    }
    reply_json(connection, 200, body, snapshot.revision);
    return;

error_without_revision:
    reply_error(connection, &error, 0);
    return;

error_with_revision:
    original_error = error;
    rte_error_clear(&snapshot_error);
    if (rte_controller_snapshot(
            &app->controller, &snapshot, &snapshot_error) != 0)
        snapshot.revision = 0;
    reply_error(connection, &original_error, snapshot.revision);
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

static enum endpoint_methods methods_for_known_uri(
    const struct mg_http_message *message)
{
    if (mg_http_match_uri(message, "/api/v1/rf/config") ||
        mg_http_match_uri(message, "/api/v1/rte/config"))
        return ENDPOINT_GET_PATCH;
    if (mg_http_match_uri(message, "/api/v1/config"))
        return ENDPOINT_PATCH;
    if (mg_http_match_uri(message, "/healthz") ||
        mg_http_match_uri(message, "/api/v1") ||
        mg_http_match_uri(message, "/api/v1/system") ||
        mg_http_match_uri(message, "/api/v1/rte/status") ||
        mg_http_match_uri(message, "/api/v1/rte/capabilities"))
        return ENDPOINT_GET;
    return ENDPOINT_UNKNOWN;
}

static void reply_method_not_allowed(struct mg_connection *connection,
                                     enum endpoint_methods methods)
{
    const char *headers;

    if (methods == ENDPOINT_GET_PATCH)
        headers = RTE_HTTP_JSON_HEADERS "Allow: GET, PATCH\r\n";
    else if (methods == ENDPOINT_PATCH)
        headers = RTE_HTTP_JSON_HEADERS "Allow: PATCH\r\n";
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
    enum endpoint_methods endpoint_methods;

    if (event != MG_EV_HTTP_MSG)
        return;

    if (method_is(message, "GET") &&
        mg_http_match_uri(message, "/healthz")) {
        handle_health(connection, app);
    } else if (method_is(message, "GET") &&
               mg_http_match_uri(message, "/api/v1")) {
        mg_http_reply(
            connection, 200,
            RTE_HTTP_JSON_HEADERS,
            "{\"api_version\":\"v1\",\"endpoints\":["
            "\"/api/v1/system\",\"/api/v1/rf/config\","
            "\"/api/v1/rte/config\","
            "\"/api/v1/rte/capabilities\","
            "\"/api/v1/rte/status\",\"/api/v1/config\"]}\n");
    } else if (method_is(message, "GET") &&
               (mg_http_match_uri(message, "/api/v1/system") ||
                mg_http_match_uri(message, "/api/v1/rf/config") ||
                mg_http_match_uri(message, "/api/v1/rte/config") ||
                mg_http_match_uri(message, "/api/v1/rte/status"))) {
        handle_get_snapshot(connection, app);
    } else if (method_is(message, "GET") &&
               mg_http_match_uri(
                   message, "/api/v1/rte/capabilities")) {
        handle_capabilities(connection, app);
    } else if (method_is(message, "PATCH") &&
               mg_http_match_uri(message, "/api/v1/rf/config")) {
        handle_patch(connection, message, app, PATCH_RF);
    } else if (method_is(message, "PATCH") &&
               mg_http_match_uri(message, "/api/v1/rte/config")) {
        handle_patch(connection, message, app, PATCH_RTE);
    } else if (method_is(message, "PATCH") &&
               mg_http_match_uri(message, "/api/v1/config")) {
        handle_patch(connection, message, app, PATCH_COMBINED);
    } else if ((endpoint_methods = methods_for_known_uri(message)) !=
               ENDPOINT_UNKNOWN) {
        reply_method_not_allowed(connection, endpoint_methods);
    } else if (api_uri(message) || health_uri(message)) {
        mg_http_reply(connection, 404,
                      RTE_HTTP_JSON_HEADERS,
                      "{\"error\":{\"code\":\"not_found\","
                      "\"message\":\"endpoint not found\"}}\n");
    } else if (!static_method_is_allowed(message)) {
        mg_http_reply(connection, 405,
                      "Content-Type: text/plain; charset=utf-8\r\n"
                      "Cache-Control: no-store\r\n"
                      RTE_HTTP_SECURITY_HEADERS
                      "Allow: GET, HEAD\r\n",
                      "Method not allowed\n");
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

    descriptor = open(path, O_RDONLY | O_CLOEXEC);
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

static void restore_persisted_config(struct rte_http_app *app)
{
    char body[4096];
    struct mg_str json;
    struct rte_config_patch patch;
    struct rte_snapshot snapshot;
    struct rte_error error;
    size_t length = 0;
    bool found;
    int status;

    if (app->state_path == NULL || app->state_path[0] == '\0')
        return;
    status = read_state_file(
        app->state_path, body, sizeof(body), &length);
    if (status == -ENOENT)
        return;
    if (status != 0) {
        snprintf(app->warning, sizeof(app->warning),
                 "cannot read persisted config: %s", strerror(-status));
        return;
    }
    json = mg_str_n(body, length);
    rte_error_clear(&error);
    if (json_root_object(json, &error) != 0 ||
        validate_rte_shape(json, "$", &error) != 0 ||
        parse_config_patch(json, "$", &patch, &found, &error) != 0 ||
        !found) {
        snprintf(app->warning, sizeof(app->warning),
                 "persisted config is invalid: %.180s", error.message);
        return;
    }
    status = rte_controller_apply(
        &app->controller, NULL, &patch, false, 0, &snapshot, &error);
    if (status != 0) {
        snprintf(app->warning, sizeof(app->warning),
                 "persisted config was not applied: %.180s",
                 error.message);
        return;
    }
    app->warning[0] = '\0';
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
