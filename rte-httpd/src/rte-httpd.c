#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define RTE_HTTPD_VERSION "0.1.0"
#define DEFAULT_HTTP_ADDRESS "0.0.0.0:8000"
#define DEFAULT_STATIC_DIR "/www/rte"
#define DEFAULT_MWIPCORE "/dev/mwipcore0"
#define DEFAULT_IIO_NAME "ad9361-phy"

#define REQUEST_MAX (64 * 1024)
#define BODY_MAX (32 * 1024)
#define RTE_REG_SIZE 0x10000u

#define RTE_REG_RESET 0x000u
#define RTE_REG_ENABLE 0x004u
#define RTE_REG_TIMESTAMP 0x008u
#define RTE_REG_DELAY_OFFSET 0x100u
#define RTE_REG_FCW 0x104u
#define RTE_REG_SCALING 0x108u
#define RTE_REG_PHASE_OFFSET 0x10cu
#define RTE_REG_LOAD_PARAM 0x110u

struct server_config {
    char bind_host[64];
    int bind_port;
    char static_dir[PATH_MAX];
    char mwipcore_path[PATH_MAX];
    char iio_name[64];
};

struct rte_core {
    int fd;
    volatile uint8_t *regs;
    size_t size;
    char path[PATH_MAX];
};

struct http_request {
    char method[12];
    char path[PATH_MAX];
    char *body;
    size_t body_len;
};

static struct server_config g_cfg;
static struct rte_core g_rte = {
    .fd = -1,
    .regs = NULL,
    .size = RTE_REG_SIZE,
    .path = "",
};

static void trim_newline(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [--http-address ADDR:PORT] [--static-dir DIR] "
            "[--mwipcore PATH] [--iio-name NAME]\n",
            argv0);
}

static int parse_http_address(const char *arg, char *host, size_t host_len, int *port)
{
    const char *colon = strrchr(arg, ':');
    if (colon == NULL) {
        char *end = NULL;
        long parsed = strtol(arg, &end, 10);
        if (end != NULL && *end == '\0' && parsed > 0 && parsed <= 65535) {
            snprintf(host, host_len, "0.0.0.0");
            *port = (int)parsed;
            return 0;
        }
        snprintf(host, host_len, "%s", arg);
        *port = 8000;
        return 0;
    }

    size_t hlen = (size_t)(colon - arg);
    if (hlen == 0 || hlen >= host_len) {
        return -1;
    }
    memcpy(host, arg, hlen);
    host[hlen] = '\0';

    char *end = NULL;
    long parsed = strtol(colon + 1, &end, 10);
    if (end == NULL || *end != '\0' || parsed <= 0 || parsed > 65535) {
        return -1;
    }
    *port = (int)parsed;
    return 0;
}

static int parse_args(int argc, char **argv)
{
    if (parse_http_address(DEFAULT_HTTP_ADDRESS, g_cfg.bind_host, sizeof(g_cfg.bind_host), &g_cfg.bind_port) != 0) {
        return -1;
    }
    snprintf(g_cfg.static_dir, sizeof(g_cfg.static_dir), "%s", DEFAULT_STATIC_DIR);
    snprintf(g_cfg.mwipcore_path, sizeof(g_cfg.mwipcore_path), "%s", DEFAULT_MWIPCORE);
    snprintf(g_cfg.iio_name, sizeof(g_cfg.iio_name), "%s", DEFAULT_IIO_NAME);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--http-address") == 0 && i + 1 < argc) {
            if (parse_http_address(argv[++i], g_cfg.bind_host, sizeof(g_cfg.bind_host), &g_cfg.bind_port) != 0) {
                fprintf(stderr, "invalid --http-address: %s\n", argv[i]);
                return -1;
            }
        } else if (strcmp(argv[i], "--static-dir") == 0 && i + 1 < argc) {
            snprintf(g_cfg.static_dir, sizeof(g_cfg.static_dir), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--mwipcore") == 0 && i + 1 < argc) {
            snprintf(g_cfg.mwipcore_path, sizeof(g_cfg.mwipcore_path), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--iio-name") == 0 && i + 1 < argc) {
            snprintf(g_cfg.iio_name, sizeof(g_cfg.iio_name), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            exit(0);
        } else {
            usage(argv[0]);
            return -1;
        }
    }
    return 0;
}

static ssize_t send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        sent += (size_t)n;
    }
    return (ssize_t)sent;
}

static const char *status_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "OK";
    }
}

static void send_response(int client, int status, const char *content_type, const char *body)
{
    if (body == NULL) {
        body = "";
    }
    char header[512];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 %d %s\r\n"
                              "Content-Type: %s\r\n"
                              "Content-Length: %zu\r\n"
                              "Connection: close\r\n"
                              "Access-Control-Allow-Origin: *\r\n"
                              "Access-Control-Allow-Methods: GET, PATCH, POST, OPTIONS\r\n"
                              "Access-Control-Allow-Headers: Content-Type\r\n"
                              "\r\n",
                              status, status_reason(status), content_type, strlen(body));
    if (header_len > 0) {
        send_all(client, header, (size_t)header_len);
        send_all(client, body, strlen(body));
    }
}

static void send_no_content(int client)
{
    const char *header =
        "HTTP/1.1 204 No Content\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, PATCH, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "\r\n";
    send_all(client, header, strlen(header));
}

static void json_escape(const char *in, char *out, size_t out_len)
{
    size_t pos = 0;
    if (out_len == 0) {
        return;
    }
    for (size_t i = 0; in[i] != '\0' && pos + 2 < out_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            if (pos + 2 >= out_len) {
                break;
            }
            out[pos++] = '\\';
            out[pos++] = (char)c;
        } else if (c >= 0x20 && c < 0x7f) {
            out[pos++] = (char)c;
        } else {
            out[pos++] = ' ';
        }
    }
    out[pos] = '\0';
}

static void send_json_error(int client, int status, const char *message)
{
    char escaped[512];
    char body[768];
    json_escape(message, escaped, sizeof(escaped));
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}\n", escaped);
    send_response(client, status, "application/json", body);
}

static char *find_headers_end(char *buf)
{
    char *p = strstr(buf, "\r\n\r\n");
    if (p != NULL) {
        return p + 4;
    }
    p = strstr(buf, "\n\n");
    if (p != NULL) {
        return p + 2;
    }
    return NULL;
}

static long parse_content_length(const char *buf, const char *headers_end)
{
    const char *line = buf;
    while (line < headers_end && *line != '\0') {
        const char *next = strstr(line, "\n");
        if (next == NULL || next > headers_end) {
            next = headers_end;
        }
        while (*line == '\r' || *line == '\n') {
            line++;
        }
        if ((size_t)(next - line) >= strlen("Content-Length:") &&
            strncasecmp(line, "Content-Length:", strlen("Content-Length:")) == 0) {
            const char *value = line + strlen("Content-Length:");
            while (*value == ' ' || *value == '\t') {
                value++;
            }
            return strtol(value, NULL, 10);
        }
        line = next + 1;
    }
    return 0;
}

static int read_http_request(int client, struct http_request *req, char *buf, size_t buf_len)
{
    size_t total = 0;
    char *headers_end = NULL;
    long content_length = 0;

    memset(req, 0, sizeof(*req));
    while (total + 1 < buf_len) {
        ssize_t n = recv(client, buf + total, buf_len - total - 1, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            break;
        }
        total += (size_t)n;
        buf[total] = '\0';

        if (headers_end == NULL) {
            headers_end = find_headers_end(buf);
            if (headers_end != NULL) {
                content_length = parse_content_length(buf, headers_end);
                if (content_length < 0 || content_length > BODY_MAX) {
                    return -2;
                }
            }
        }
        if (headers_end != NULL) {
            size_t header_len = (size_t)(headers_end - buf);
            if (total >= header_len + (size_t)content_length) {
                break;
            }
        }
    }

    if (headers_end == NULL) {
        return -1;
    }

    if (sscanf(buf, "%11s %1023s", req->method, req->path) != 2) {
        return -1;
    }
    char *query = strchr(req->path, '?');
    if (query != NULL) {
        *query = '\0';
    }
    req->body = headers_end;
    req->body_len = (size_t)content_length;
    req->body[req->body_len] = '\0';
    return 0;
}

static const char *skip_ws(const char *p)
{
    while (*p != '\0' && isspace((unsigned char)*p)) {
        p++;
    }
    return p;
}

static const char *json_value_for_key(const char *json, const char *key)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = json;
    while ((p = strstr(p, pattern)) != NULL) {
        const char *q = p + strlen(pattern);
        q = skip_ws(q);
        if (*q == ':') {
            return skip_ws(q + 1);
        }
        p += strlen(pattern);
    }
    return NULL;
}

static bool json_get_bool(const char *json, const char *key, bool *out)
{
    const char *p = json_value_for_key(json, key);
    if (p == NULL) {
        return false;
    }
    if (strncmp(p, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = false;
        return true;
    }
    long v = strtol(p, NULL, 10);
    *out = v != 0;
    return true;
}

static bool json_get_i64(const char *json, const char *key, int64_t *out)
{
    const char *p = json_value_for_key(json, key);
    if (p == NULL) {
        return false;
    }
    char *end = NULL;
    long long v = strtoll(p, &end, 10);
    if (end == p) {
        return false;
    }
    *out = (int64_t)v;
    return true;
}

static bool json_get_double(const char *json, const char *key, double *out)
{
    const char *p = json_value_for_key(json, key);
    if (p == NULL) {
        return false;
    }
    char *end = NULL;
    double v = strtod(p, &end);
    if (end == p) {
        return false;
    }
    *out = v;
    return true;
}

static bool json_get_string(const char *json, const char *key, char *out, size_t out_len)
{
    const char *p = json_value_for_key(json, key);
    if (p == NULL || *p != '"' || out_len == 0) {
        return false;
    }
    p++;
    size_t pos = 0;
    while (*p != '\0' && *p != '"' && pos + 1 < out_len) {
        if (*p == '\\' && p[1] != '\0') {
            p++;
        }
        out[pos++] = *p++;
    }
    out[pos] = '\0';
    return *p == '"';
}

static int find_mwipcore_device(char *out, size_t out_len)
{
    DIR *dir = opendir("/dev");
    if (dir == NULL) {
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "mwipcore", strlen("mwipcore")) == 0) {
            snprintf(out, out_len, "/dev/%s", entry->d_name);
            closedir(dir);
            return 0;
        }
    }
    closedir(dir);
    return -1;
}

static int rte_open(char *err, size_t err_len)
{
    if (g_rte.regs != NULL) {
        return 0;
    }

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s", g_cfg.mwipcore_path);

    int fd = open(path, O_RDWR | O_SYNC);
    if (fd < 0 && strcmp(g_cfg.mwipcore_path, DEFAULT_MWIPCORE) == 0) {
        if (find_mwipcore_device(path, sizeof(path)) == 0) {
            fd = open(path, O_RDWR | O_SYNC);
        }
    }
    if (fd < 0) {
        snprintf(err, err_len, "failed to open mwipcore device %s: %s", g_cfg.mwipcore_path, strerror(errno));
        return -1;
    }

    void *regs = mmap(NULL, RTE_REG_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (regs == MAP_FAILED) {
        snprintf(err, err_len, "failed to mmap %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }

    g_rte.fd = fd;
    g_rte.regs = (volatile uint8_t *)regs;
    g_rte.size = RTE_REG_SIZE;
    snprintf(g_rte.path, sizeof(g_rte.path), "%s", path);
    fprintf(stderr, "rte-httpd: mapped RTE DUT registers from %s\n", g_rte.path);
    return 0;
}

static uint32_t rte_read_u32(uint32_t offset)
{
    volatile uint32_t *reg = (volatile uint32_t *)(g_rte.regs + offset);
    return *reg;
}

static void rte_write_u32(uint32_t offset, uint32_t value)
{
    volatile uint32_t *reg = (volatile uint32_t *)(g_rte.regs + offset);
    *reg = value;
    __sync_synchronize();
}

static int32_t sign_extend_12(uint32_t value)
{
    value &= 0xfffu;
    if ((value & 0x800u) != 0) {
        value |= 0xfffff000u;
    }
    return (int32_t)value;
}

static int rte_snapshot_json(char *out, size_t out_len, char *err, size_t err_len)
{
    if (rte_open(err, err_len) != 0) {
        return -1;
    }

    uint32_t enabled = rte_read_u32(RTE_REG_ENABLE) & 0x1u;
    uint32_t timestamp = rte_read_u32(RTE_REG_TIMESTAMP);
    uint32_t delay_offset = rte_read_u32(RTE_REG_DELAY_OFFSET) & 0x3ffu;
    int32_t fcw = (int32_t)rte_read_u32(RTE_REG_FCW);
    int32_t scaling_raw = sign_extend_12(rte_read_u32(RTE_REG_SCALING));
    double scaling = (double)scaling_raw / 2048.0;
    int32_t phase_offset = (int32_t)rte_read_u32(RTE_REG_PHASE_OFFSET);
    uint32_t load_param = rte_read_u32(RTE_REG_LOAD_PARAM) & 0x1u;

    snprintf(out, out_len,
             "{"
             "\"enabled\":%s,"
             "\"timestamp\":%u,"
             "\"delay_offset\":%u,"
             "\"fcw\":%d,"
             "\"scaling\":%.9g,"
             "\"scaling_raw\":%d,"
             "\"phase_offset\":%d,"
             "\"load_param\":%u,"
             "\"device\":\"%s\""
             "}\n",
             enabled ? "true" : "false",
             timestamp,
             delay_offset,
             fcw,
             scaling,
             scaling_raw,
             phase_offset,
             load_param,
             g_rte.path);
    return 0;
}

static int rte_apply_patch(const char *body, char *err, size_t err_len)
{
    if (rte_open(err, err_len) != 0) {
        return -1;
    }

    bool has_enabled = false;
    bool enabled = false;
    bool has_delay = false;
    int64_t delay = 0;
    bool has_fcw = false;
    int64_t fcw = 0;
    bool has_scaling = false;
    int64_t scaling_raw = 0;
    bool has_phase = false;
    int64_t phase = 0;
    bool has_load = false;
    bool load = false;

    has_enabled = json_get_bool(body, "enabled", &enabled);

    has_delay = json_get_i64(body, "delay_offset", &delay);
    if (has_delay && (delay < 0 || delay > 1023)) {
        snprintf(err, err_len, "delay_offset must be in 0..1023");
        return -1;
    }

    has_fcw = json_get_i64(body, "fcw", &fcw);
    if (has_fcw && (fcw < INT32_MIN || fcw > INT32_MAX)) {
        snprintf(err, err_len, "fcw must fit signed 32-bit");
        return -1;
    }

    double scaling = 0.0;
    if (json_get_double(body, "scaling", &scaling)) {
        double raw = scaling * 2048.0;
        scaling_raw = raw >= 0.0 ? (int64_t)(raw + 0.5) : (int64_t)(raw - 0.5);
        has_scaling = true;
    } else if (json_get_i64(body, "scaling_raw", &scaling_raw)) {
        has_scaling = true;
    }
    if (has_scaling && (scaling_raw < -2048 || scaling_raw > 2047)) {
        snprintf(err, err_len, "scaling must map to signed 12-bit raw range -2048..2047");
        return -1;
    }

    has_phase = json_get_i64(body, "phase_offset", &phase);
    if (has_phase && (phase < INT32_MIN || phase > INT32_MAX)) {
        snprintf(err, err_len, "phase_offset must fit signed 32-bit");
        return -1;
    }

    has_load = json_get_bool(body, "load_param", &load);

    if (has_enabled) {
        rte_write_u32(RTE_REG_ENABLE, enabled ? 1u : 0u);
    }
    if (has_delay) {
        rte_write_u32(RTE_REG_DELAY_OFFSET, (uint32_t)delay);
    }
    if (has_fcw) {
        rte_write_u32(RTE_REG_FCW, (uint32_t)(int32_t)fcw);
    }
    if (has_scaling) {
        rte_write_u32(RTE_REG_SCALING, (uint32_t)((int32_t)scaling_raw) & 0xfffu);
    }
    if (has_phase) {
        rte_write_u32(RTE_REG_PHASE_OFFSET, (uint32_t)(int32_t)phase);
    }
    if (has_load) {
        rte_write_u32(RTE_REG_LOAD_PARAM, load ? 1u : 0u);
    }

    return 0;
}

static int rte_pulse(uint32_t offset, char *err, size_t err_len)
{
    if (rte_open(err, err_len) != 0) {
        return -1;
    }
    rte_write_u32(offset, 1u);
    usleep(1000);
    rte_write_u32(offset, 0u);
    return 0;
}

static int iio_find_device(char *out, size_t out_len, char *err, size_t err_len)
{
    DIR *dir = opendir("/sys/bus/iio/devices");
    if (dir == NULL) {
        snprintf(err, err_len, "failed to open /sys/bus/iio/devices: %s", strerror(errno));
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "iio:device", strlen("iio:device")) != 0) {
            continue;
        }

        char name_path[PATH_MAX];
        snprintf(name_path, sizeof(name_path), "/sys/bus/iio/devices/%s/name", entry->d_name);
        FILE *f = fopen(name_path, "r");
        if (f == NULL) {
            continue;
        }
        char name[128];
        if (fgets(name, sizeof(name), f) != NULL) {
            trim_newline(name);
            if (strcmp(name, g_cfg.iio_name) == 0) {
                snprintf(out, out_len, "/sys/bus/iio/devices/%s", entry->d_name);
                fclose(f);
                closedir(dir);
                return 0;
            }
        }
        fclose(f);
    }

    closedir(dir);
    snprintf(err, err_len, "IIO device named %s not found", g_cfg.iio_name);
    return -1;
}

static int iio_read_attr(const char *dev, const char *attr, char *out, size_t out_len, char *err, size_t err_len)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dev, attr);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        snprintf(err, err_len, "failed to read %s: %s", path, strerror(errno));
        return -1;
    }
    if (fgets(out, (int)out_len, f) == NULL) {
        snprintf(err, err_len, "failed to read %s", path);
        fclose(f);
        return -1;
    }
    fclose(f);
    trim_newline(out);
    return 0;
}

static int iio_write_attr(const char *dev, const char *attr, const char *value, char *err, size_t err_len)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dev, attr);
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        snprintf(err, err_len, "failed to open %s for write: %s", path, strerror(errno));
        return -1;
    }
    if (fprintf(f, "%s", value) < 0) {
        snprintf(err, err_len, "failed to write %s", path);
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0) {
        snprintf(err, err_len, "failed to close %s after write: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

static double parse_db_value(const char *s)
{
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s", s);
    char *db = strstr(tmp, " dB");
    if (db != NULL) {
        *db = '\0';
    }
    return strtod(tmp, NULL);
}

static int ad9361_snapshot_json(char *out, size_t out_len, char *err, size_t err_len)
{
    char dev[PATH_MAX];
    if (iio_find_device(dev, sizeof(dev), err, err_len) != 0) {
        return -1;
    }

    char sampling[128], rx_bw[128], tx_bw[128], rx_lo[128], tx_lo[128];
    char rx_gain[128], tx_gain[128], rx_gain_mode[128];
    if (iio_read_attr(dev, "in_voltage_sampling_frequency", sampling, sizeof(sampling), err, err_len) != 0 ||
        iio_read_attr(dev, "in_voltage_rf_bandwidth", rx_bw, sizeof(rx_bw), err, err_len) != 0 ||
        iio_read_attr(dev, "out_voltage_rf_bandwidth", tx_bw, sizeof(tx_bw), err, err_len) != 0 ||
        iio_read_attr(dev, "out_altvoltage0_RX_LO_frequency", rx_lo, sizeof(rx_lo), err, err_len) != 0 ||
        iio_read_attr(dev, "out_altvoltage1_TX_LO_frequency", tx_lo, sizeof(tx_lo), err, err_len) != 0 ||
        iio_read_attr(dev, "in_voltage0_hardwaregain", rx_gain, sizeof(rx_gain), err, err_len) != 0 ||
        iio_read_attr(dev, "out_voltage0_hardwaregain", tx_gain, sizeof(tx_gain), err, err_len) != 0 ||
        iio_read_attr(dev, "in_voltage0_gain_control_mode", rx_gain_mode, sizeof(rx_gain_mode), err, err_len) != 0) {
        return -1;
    }

    char mode_escaped[128];
    json_escape(rx_gain_mode, mode_escaped, sizeof(mode_escaped));
    snprintf(out, out_len,
             "{"
             "\"sampling_frequency\":%llu,"
             "\"rx_rf_bandwidth\":%llu,"
             "\"tx_rf_bandwidth\":%llu,"
             "\"rx_lo_frequency\":%llu,"
             "\"tx_lo_frequency\":%llu,"
             "\"rx_gain\":%.9g,"
             "\"tx_gain\":%.9g,"
             "\"rx_gain_mode\":\"%s\","
             "\"device\":\"%s\""
             "}\n",
             strtoull(sampling, NULL, 10),
             strtoull(rx_bw, NULL, 10),
             strtoull(tx_bw, NULL, 10),
             strtoull(rx_lo, NULL, 10),
             strtoull(tx_lo, NULL, 10),
             parse_db_value(rx_gain),
             parse_db_value(tx_gain),
             mode_escaped,
             dev);
    return 0;
}

static bool valid_gain_mode(const char *mode)
{
    return strcmp(mode, "manual") == 0 ||
           strcmp(mode, "fast_attack") == 0 ||
           strcmp(mode, "slow_attack") == 0 ||
           strcmp(mode, "hybrid") == 0;
}

static int write_iio_u64_if_present(const char *body, const char *json_key, const char *dev,
                                    const char *attr, char *err, size_t err_len)
{
    int64_t value = 0;
    if (!json_get_i64(body, json_key, &value)) {
        return 0;
    }
    if (value < 0) {
        snprintf(err, err_len, "%s must be non-negative", json_key);
        return -1;
    }
    char value_str[64];
    snprintf(value_str, sizeof(value_str), "%llu", (unsigned long long)value);
    return iio_write_attr(dev, attr, value_str, err, err_len);
}

static int write_iio_double_if_present(const char *body, const char *json_key, const char *dev,
                                       const char *attr, char *err, size_t err_len)
{
    double value = 0.0;
    if (!json_get_double(body, json_key, &value)) {
        return 0;
    }
    char value_str[64];
    snprintf(value_str, sizeof(value_str), "%.6f", value);
    return iio_write_attr(dev, attr, value_str, err, err_len);
}

static int ad9361_apply_patch(const char *body, char *err, size_t err_len)
{
    char dev[PATH_MAX];
    if (iio_find_device(dev, sizeof(dev), err, err_len) != 0) {
        return -1;
    }

    if (write_iio_u64_if_present(body, "sampling_frequency", dev, "in_voltage_sampling_frequency", err, err_len) != 0 ||
        write_iio_u64_if_present(body, "rx_rf_bandwidth", dev, "in_voltage_rf_bandwidth", err, err_len) != 0 ||
        write_iio_u64_if_present(body, "tx_rf_bandwidth", dev, "out_voltage_rf_bandwidth", err, err_len) != 0 ||
        write_iio_u64_if_present(body, "rx_lo_frequency", dev, "out_altvoltage0_RX_LO_frequency", err, err_len) != 0 ||
        write_iio_u64_if_present(body, "tx_lo_frequency", dev, "out_altvoltage1_TX_LO_frequency", err, err_len) != 0) {
        return -1;
    }

    char mode[64];
    if (json_get_string(body, "rx_gain_mode", mode, sizeof(mode))) {
        if (!valid_gain_mode(mode)) {
            snprintf(err, err_len, "rx_gain_mode must be manual, fast_attack, slow_attack, or hybrid");
            return -1;
        }
        if (iio_write_attr(dev, "in_voltage0_gain_control_mode", mode, err, err_len) != 0) {
            return -1;
        }
    }

    if (write_iio_double_if_present(body, "rx_gain", dev, "in_voltage0_hardwaregain", err, err_len) != 0 ||
        write_iio_double_if_present(body, "tx_gain", dev, "out_voltage0_hardwaregain", err, err_len) != 0) {
        return -1;
    }

    return 0;
}

static void handle_api_request(int client, const struct http_request *req)
{
    char body[4096];
    char err[512];

    if (strcmp(req->method, "OPTIONS") == 0) {
        send_no_content(client);
        return;
    }

    if (strcmp(req->path, "/api") == 0 && strcmp(req->method, "GET") == 0) {
        snprintf(body, sizeof(body),
                 "{"
                 "\"name\":\"rte-httpd\","
                 "\"version\":\"%s\","
                 "\"routes\":["
                 "\"GET /api/health\","
                 "\"GET /api/versions\","
                 "\"GET /api/rte\","
                 "\"PATCH /api/rte\","
                 "\"POST /api/rte/reset\","
                 "\"POST /api/rte/load-param\","
                 "\"GET /api/ad9361\","
                 "\"PATCH /api/ad9361\""
                 "]"
                 "}\n",
                 RTE_HTTPD_VERSION);
        send_response(client, 200, "application/json", body);
        return;
    }

    if (strcmp(req->path, "/api/health") == 0 && strcmp(req->method, "GET") == 0) {
        send_response(client, 200, "application/json", "{\"ok\":true}\n");
        return;
    }

    if (strcmp(req->path, "/api/versions") == 0 && strcmp(req->method, "GET") == 0) {
        snprintf(body, sizeof(body),
                 "{\"rte_httpd\":\"%s\",\"mwipcore\":\"%s\",\"iio_name\":\"%s\"}\n",
                 RTE_HTTPD_VERSION, g_cfg.mwipcore_path, g_cfg.iio_name);
        send_response(client, 200, "application/json", body);
        return;
    }

    if (strcmp(req->path, "/api/rte") == 0) {
        if (strcmp(req->method, "GET") == 0) {
            if (rte_snapshot_json(body, sizeof(body), err, sizeof(err)) != 0) {
                send_json_error(client, 503, err);
                return;
            }
            send_response(client, 200, "application/json", body);
            return;
        }
        if (strcmp(req->method, "PATCH") == 0 || strcmp(req->method, "PUT") == 0) {
            if (rte_apply_patch(req->body, err, sizeof(err)) != 0) {
                send_json_error(client, 400, err);
                return;
            }
            if (rte_snapshot_json(body, sizeof(body), err, sizeof(err)) != 0) {
                send_json_error(client, 503, err);
                return;
            }
            send_response(client, 200, "application/json", body);
            return;
        }
        send_json_error(client, 405, "method not allowed for /api/rte");
        return;
    }

    if (strcmp(req->path, "/api/rte/reset") == 0 && strcmp(req->method, "POST") == 0) {
        if (rte_pulse(RTE_REG_RESET, err, sizeof(err)) != 0) {
            send_json_error(client, 503, err);
            return;
        }
        send_response(client, 200, "application/json", "{\"reset\":true}\n");
        return;
    }

    if (strcmp(req->path, "/api/rte/load-param") == 0 && strcmp(req->method, "POST") == 0) {
        if (rte_pulse(RTE_REG_LOAD_PARAM, err, sizeof(err)) != 0) {
            send_json_error(client, 503, err);
            return;
        }
        send_response(client, 200, "application/json", "{\"load_param_pulsed\":true}\n");
        return;
    }

    if (strcmp(req->path, "/api/ad9361") == 0) {
        if (strcmp(req->method, "GET") == 0) {
            if (ad9361_snapshot_json(body, sizeof(body), err, sizeof(err)) != 0) {
                send_json_error(client, 503, err);
                return;
            }
            send_response(client, 200, "application/json", body);
            return;
        }
        if (strcmp(req->method, "PATCH") == 0 || strcmp(req->method, "PUT") == 0) {
            if (ad9361_apply_patch(req->body, err, sizeof(err)) != 0) {
                send_json_error(client, 400, err);
                return;
            }
            if (ad9361_snapshot_json(body, sizeof(body), err, sizeof(err)) != 0) {
                send_json_error(client, 503, err);
                return;
            }
            send_response(client, 200, "application/json", body);
            return;
        }
        send_json_error(client, 405, "method not allowed for /api/ad9361");
        return;
    }

    send_json_error(client, 404, "API route not found");
}

static const char *content_type_for_path(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (ext == NULL) {
        return "application/octet-stream";
    }
    if (strcmp(ext, ".html") == 0) return "text/html";
    if (strcmp(ext, ".css") == 0) return "text/css";
    if (strcmp(ext, ".js") == 0) return "application/javascript";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".svg") == 0) return "image/svg+xml";
    if (strcmp(ext, ".png") == 0) return "image/png";
    return "application/octet-stream";
}

static void send_static_file(int client, const char *path)
{
    if (strstr(path, "..") != NULL) {
        send_json_error(client, 403, "path traversal is not allowed");
        return;
    }

    char rel[PATH_MAX];
    if (strcmp(path, "/") == 0) {
        snprintf(rel, sizeof(rel), "/index.html");
    } else {
        snprintf(rel, sizeof(rel), "%s", path);
    }

    char full[PATH_MAX];
    snprintf(full, sizeof(full), "%s%s", g_cfg.static_dir, rel);

    int fd = open(full, O_RDONLY);
    if (fd < 0) {
        send_json_error(client, 404, "file not found");
        return;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        send_json_error(client, 404, "file not found");
        return;
    }

    char header[512];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 200 OK\r\n"
                              "Content-Type: %s\r\n"
                              "Content-Length: %lld\r\n"
                              "Connection: close\r\n"
                              "\r\n",
                              content_type_for_path(full),
                              (long long)st.st_size);
    if (header_len > 0) {
        send_all(client, header, (size_t)header_len);
    }

    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (send_all(client, buf, (size_t)n) < 0) {
            break;
        }
    }
    close(fd);
}

static void handle_client(int client)
{
    char request_buf[REQUEST_MAX];
    struct http_request req;
    int ret = read_http_request(client, &req, request_buf, sizeof(request_buf));
    if (ret == -2) {
        send_json_error(client, 413, "request body too large");
        return;
    }
    if (ret != 0) {
        send_json_error(client, 400, "bad HTTP request");
        return;
    }

    if (strncmp(req.path, "/api", 4) == 0) {
        handle_api_request(client, &req);
        return;
    }

    if (strcmp(req.method, "GET") != 0) {
        send_json_error(client, 405, "only GET is supported for static files");
        return;
    }
    send_static_file(client, req.path);
}

static int create_server_socket(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)g_cfg.bind_port);
    if (inet_aton(g_cfg.bind_host, &addr.sin_addr) == 0) {
        fprintf(stderr, "invalid bind address: %s\n", g_cfg.bind_host);
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 16) != 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv)
{
    if (parse_args(argc, argv) != 0) {
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);

    int server = create_server_socket();
    if (server < 0) {
        return 1;
    }

    fprintf(stderr,
            "rte-httpd %s listening on %s:%d, static=%s, mwipcore=%s, iio=%s\n",
            RTE_HTTPD_VERSION,
            g_cfg.bind_host,
            g_cfg.bind_port,
            g_cfg.static_dir,
            g_cfg.mwipcore_path,
            g_cfg.iio_name);

    for (;;) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int client = accept(server, (struct sockaddr *)&peer, &peer_len);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }
        handle_client(client);
        close(client);
    }

    if (g_rte.regs != NULL) {
        munmap((void *)g_rte.regs, g_rte.size);
    }
    if (g_rte.fd >= 0) {
        close(g_rte.fd);
    }
    close(server);
    return 0;
}
