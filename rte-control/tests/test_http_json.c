/* SPDX-License-Identifier: GPL-2.0-or-later */
#define main rte_httpd_program_main
#include "../src/httpd.c"
#undef main

static unsigned int test_failures;

#define TEST_CHECK(condition) do {                                        \
    if (!(condition)) {                                                   \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n",                    \
                __FILE__, __LINE__, #condition);                          \
        test_failures++;                                                  \
    }                                                                     \
} while (0)

static void test_rte_patch(void)
{
    const char payload[] =
        "{"
        "\"range_m\":2.0,"
        "\"radial_velocity_mps\":-0.15,"
        "\"loss_db\":12.0,"
        "\"respiration\":{"
        "\"amplitude_m\":0.005,\"frequency_hz\":0.25,"
        "\"phase_rad\":0.5235987755982988},"
        "\"heartbeat\":{"
        "\"amplitude_m\":0.0005,\"frequency_hz\":1.2,"
        "\"phase_rad\":1.5707963267948966}"
        "}";
    struct rte_config_patch patch;
    struct rte_error error;
    struct mg_str json = mg_str(payload);
    bool found = false;

    TEST_CHECK(json_root_object(json, &error) == 0);
    TEST_CHECK(parse_config_patch(
                   json, "$", &patch, &found, &error) == 0);
    TEST_CHECK(found);
    TEST_CHECK(patch.has_range_m);
    TEST_CHECK(patch.has_radial_velocity_mps);
    TEST_CHECK(patch.has_loss_db);
    TEST_CHECK(patch.respiration.has_amplitude_m);
    TEST_CHECK(patch.respiration.has_frequency_hz);
    TEST_CHECK(patch.respiration.has_phase_rad);
    TEST_CHECK(patch.heartbeat.has_amplitude_m);
    TEST_CHECK(patch.heartbeat.has_frequency_hz);
    TEST_CHECK(patch.heartbeat.has_phase_rad);
    TEST_CHECK(fabs(patch.radial_velocity_mps + 0.15) < 1e-15);
    TEST_CHECK(fabs(patch.heartbeat.frequency_hz - 1.2) < 1e-15);
}

static void test_combined_patch(void)
{
    const char payload[] =
        "{\"rf\":{\"sample_rate_hz\":61440000,"
        "\"carrier_hz\":2450000000},"
        "\"rte\":{\"loss_db\":18.0}}";
    struct rte_rf_patch rf;
    struct rte_config_patch config;
    struct rte_error error;
    struct mg_str json = mg_str(payload);
    bool rf_found = false;
    bool config_found = false;

    TEST_CHECK(parse_rf_patch(
                   json, "$.rf", &rf, &rf_found, &error) == 0);
    TEST_CHECK(parse_config_patch(
                   json, "$.rte", &config, &config_found, &error) == 0);
    TEST_CHECK(rf_found);
    TEST_CHECK(config_found);
    TEST_CHECK(rf.sample_rate_hz == UINT64_C(61440000));
    TEST_CHECK(rf.carrier_hz == UINT64_C(2450000000));
    TEST_CHECK(config.has_loss_db);
    TEST_CHECK(config.loss_db == 18.0);
    TEST_CHECK(!config.has_range_m);
}

static void test_invalid_json_types(void)
{
    struct rte_config_patch patch;
    struct rte_error error;
    bool found;

    TEST_CHECK(json_root_object(
                   mg_str("{\"range_m\":"), &error) != 0);
    TEST_CHECK(json_root_object(
                   mg_str("{\"range_m\":1} trailing"), &error) != 0);
    TEST_CHECK(parse_config_patch(
                   mg_str("{\"range_m\":\"two\"}"), "$",
                   &patch, &found, &error) != 0);
    TEST_CHECK(error.code == RTE_ERROR_ARGUMENT);
}

static void test_strict_patch_shape(void)
{
    struct rte_error error;

    TEST_CHECK(validate_patch_shape(
                   mg_str("{\"loss_db\":12,\"heartbeat\":{"
                          "\"frequency_hz\":1.2}}"),
                   PATCH_RTE, &error) == 0);
    TEST_CHECK(validate_patch_shape(
                   mg_str("{\"loss_db\":12,\"loss_dB\":13}"),
                   PATCH_RTE, &error) != 0);
    TEST_CHECK(validate_patch_shape(
                   mg_str("{\"loss_db\":12,\"loss_db\":13}"),
                   PATCH_RTE, &error) != 0);
    TEST_CHECK(validate_patch_shape(
                   mg_str("{\"range_m\":1,\"respiration\":1}"),
                   PATCH_RTE, &error) != 0);
    TEST_CHECK(validate_patch_shape(
                   mg_str("{\"rf\":{},\"rte\":{\"loss_db\":1}}"),
                   PATCH_COMBINED, &error) != 0);
    TEST_CHECK(validate_patch_shape(
                   mg_str("{\"loss\\u005fdb\":12}"),
                   PATCH_RTE, &error) != 0);
}

static void test_snapshot_serialization(void)
{
    char output[RTE_JSON_RESPONSE_MAX];
    struct json_buffer json = {output, sizeof(output), 0, false};
    struct rte_snapshot snapshot = {
        .revision = 7,
        .hardware_build_id = RTE_EXPECTED_TIMESTAMP,
        .has_config = true,
        .rf = {
            .sample_rate_hz = UINT64_C(61440000),
            .rx_lo_hz = UINT64_C(2450000000),
            .tx_lo_hz = UINT64_C(2450000000),
        },
        .requested = {
            .range_m = 2.0,
            .radial_velocity_mps = -0.15,
            .loss_db = 12.0,
            .respiration = {0.005, 0.25, 0.5},
            .heartbeat = {0.0005, 1.2, 1.5},
        },
    };
    struct rte_error error;
    double revision = 0;

    TEST_CHECK(rte_encode(
                   &snapshot.requested, &snapshot.rf,
                   &snapshot.image, &snapshot.applied, &error) == 0);
    json_append_snapshot(&json, &snapshot, "quoted \"warning\"");
    TEST_CHECK(!json.overflow);
    TEST_CHECK(mg_json_get_num(
                   mg_str(output), "$.revision", &revision));
    TEST_CHECK(revision == 7.0);
    TEST_CHECK(mg_json_get(
                   mg_str(output), "$.applied.registers.microGainRes",
                   NULL) >= 0);
    TEST_CHECK(strstr(output, "\"hardware_state_known\":true") != NULL);
    TEST_CHECK(strstr(output, "quoted \\\"warning\\\"") != NULL);

    snapshot.degraded = true;
    json.used = 0;
    json.overflow = false;
    json_append_snapshot(&json, &snapshot, NULL);
    TEST_CHECK(strstr(output, "\"hardware_state_known\":false") != NULL);

    snapshot.degraded = false;
    snapshot.has_config = false;
    json.used = 0;
    json.overflow = false;
    json_append_snapshot(&json, &snapshot, NULL);
    TEST_CHECK(strstr(output, "\"hardware_state_known\":false") != NULL);
}

static void test_persistence_debounce(void)
{
    struct rte_http_app app = {0};
    struct rte_snapshot snapshot = {.has_config = true};

    schedule_persistence(&app, &snapshot);
    TEST_CHECK(!app.persistence_dirty);

    app.state_path = "/tmp/rte-httpd-test-state-unused";
    schedule_persistence(&app, &snapshot);
    TEST_CHECK(app.persistence_dirty);
    TEST_CHECK(app.persistence_deadline_ms >= mg_millis());
    TEST_CHECK(strstr(app.warning, "pending") != NULL);

    app.state_path = NULL;
    flush_persistence(&app, true);
    TEST_CHECK(!app.persistence_dirty);
    TEST_CHECK(app.warning[0] == '\0');
}

static bool response_contains(const struct mg_connection *connection,
                              const char *text)
{
    const struct mg_str response = mg_str_n(
        (const char *)connection->send.buf, connection->send.len);

    return mg_strstr(response, mg_str(text)) != NULL;
}

static void test_static_route_policy(void)
{
    struct mg_http_message message = {0};

    message.uri = mg_str("/api/v1");
    TEST_CHECK(api_uri(&message));
    TEST_CHECK(methods_for_known_uri(&message) == ENDPOINT_GET);

    message.uri = mg_str("/api/v1/rte/config");
    TEST_CHECK(methods_for_known_uri(&message) == ENDPOINT_GET_PATCH);

    message.uri = mg_str("/api/v1/config");
    TEST_CHECK(methods_for_known_uri(&message) == ENDPOINT_PATCH);

    message.uri = mg_str("/healthz");
    TEST_CHECK(methods_for_known_uri(&message) == ENDPOINT_GET);

    message.uri = mg_str("/api/v2/status");
    TEST_CHECK(api_uri(&message));
    TEST_CHECK(methods_for_known_uri(&message) == ENDPOINT_UNKNOWN);

    message.uri = mg_str("/api/v1/unknown");
    TEST_CHECK(api_uri(&message));
    TEST_CHECK(methods_for_known_uri(&message) == ENDPOINT_UNKNOWN);

    message.uri = mg_str("/apiary");
    TEST_CHECK(!api_uri(&message));

    message.uri = mg_str("/healthz/detail");
    TEST_CHECK(health_uri(&message));

    message.method = mg_str("GET");
    TEST_CHECK(static_method_is_allowed(&message));
    message.method = mg_str("HEAD");
    TEST_CHECK(static_method_is_allowed(&message));
    message.method = mg_str("POST");
    TEST_CHECK(!static_method_is_allowed(&message));
}

static void test_static_route_responses(void)
{
    struct rte_http_app app = {
        .web_root = "/definitely-not-an-rte-web-root",
    };
    struct mg_http_message message = {0};
    struct mg_connection connection = {0};

    message.method = mg_str("GET");
    message.uri = mg_str("/api/v1");
    http_handler(&connection, MG_EV_HTTP_MSG, &message, &app);
    TEST_CHECK(response_contains(&connection, "HTTP/1.1 200"));
    TEST_CHECK(response_contains(&connection, "\"api_version\":\"v1\""));
    TEST_CHECK(response_contains(&connection, "Content-Security-Policy:"));
    mg_iobuf_free(&connection.send);

    memset(&connection, 0, sizeof(connection));
    message.uri = mg_str("/api/v1/unknown");
    http_handler(&connection, MG_EV_HTTP_MSG, &message, &app);
    TEST_CHECK(response_contains(&connection, "HTTP/1.1 404"));
    TEST_CHECK(response_contains(&connection, "\"code\":\"not_found\""));
    TEST_CHECK(response_contains(&connection,
                                 "Content-Type: application/json"));
    mg_iobuf_free(&connection.send);

    memset(&connection, 0, sizeof(connection));
    message.method = mg_str("HEAD");
    message.uri = mg_str("/healthz");
    http_handler(&connection, MG_EV_HTTP_MSG, &message, &app);
    TEST_CHECK(response_contains(&connection, "HTTP/1.1 405"));
    TEST_CHECK(response_contains(&connection, "Allow: GET\r\n"));
    TEST_CHECK(!response_contains(&connection, "Allow: GET, PATCH"));
    mg_iobuf_free(&connection.send);

    memset(&connection, 0, sizeof(connection));
    message.uri = mg_str("/api/v1/rte/config");
    http_handler(&connection, MG_EV_HTTP_MSG, &message, &app);
    TEST_CHECK(response_contains(&connection, "HTTP/1.1 405"));
    TEST_CHECK(response_contains(&connection, "Allow: GET, PATCH"));
    mg_iobuf_free(&connection.send);

    memset(&connection, 0, sizeof(connection));
    message.method = mg_str("GET");
    message.uri = mg_str("/missing-static-file");
    http_handler(&connection, MG_EV_HTTP_MSG, &message, &app);
    TEST_CHECK(response_contains(&connection, "HTTP/1.1 404"));
    TEST_CHECK(response_contains(&connection, "Not found"));
    TEST_CHECK(response_contains(&connection, "X-Frame-Options: DENY"));
    mg_iobuf_free(&connection.send);

    memset(&connection, 0, sizeof(connection));
    message.method = mg_str("POST");
    message.uri = mg_str("/index.html");
    http_handler(&connection, MG_EV_HTTP_MSG, &message, &app);
    TEST_CHECK(response_contains(&connection, "HTTP/1.1 405"));
    TEST_CHECK(response_contains(&connection, "Allow: GET, HEAD"));
    mg_iobuf_free(&connection.send);

    memset(&connection, 0, sizeof(connection));
    app.web_root = "www";
    message.method = mg_str("GET");
    message.uri = mg_str("/");
    http_handler(&connection, MG_EV_HTTP_MSG, &message, &app);
    TEST_CHECK(response_contains(&connection, "HTTP/1.1 200"));
    TEST_CHECK(response_contains(&connection,
                                 "Content-Type: text/html; charset=utf-8"));
    TEST_CHECK(response_contains(&connection, "Content-Security-Policy:"));
    if (connection.pfn != NULL && connection.pfn_data != NULL)
        connection.pfn(&connection, MG_EV_CLOSE, NULL, connection.pfn_data);
    mg_iobuf_free(&connection.send);

    memset(&connection, 0, sizeof(connection));
    message.method = mg_str("HEAD");
    message.uri = mg_str("/app.css");
    http_handler(&connection, MG_EV_HTTP_MSG, &message, &app);
    TEST_CHECK(response_contains(&connection, "HTTP/1.1 200"));
    TEST_CHECK(response_contains(&connection,
                                 "Content-Type: text/css; charset=utf-8"));
    TEST_CHECK(!response_contains(&connection, "Method not allowed"));
    mg_iobuf_free(&connection.send);

    memset(&connection, 0, sizeof(connection));
    message.method = mg_str("GET");
    message.uri = mg_str("/%2e%2e/README.md");
    http_handler(&connection, MG_EV_HTTP_MSG, &message, &app);
    TEST_CHECK(response_contains(&connection, "HTTP/1.1 404"));
    TEST_CHECK(response_contains(&connection, "Not found"));
    mg_iobuf_free(&connection.send);
}

int main(void)
{
    test_rte_patch();
    test_combined_patch();
    test_invalid_json_types();
    test_strict_patch_shape();
    test_snapshot_serialization();
    test_persistence_debounce();
    test_static_route_policy();
    test_static_route_responses();

    if (test_failures != 0) {
        fprintf(stderr, "%u test failure(s)\n", test_failures);
        return EXIT_FAILURE;
    }
    puts("test_http_json: PASS");
    return EXIT_SUCCESS;
}
