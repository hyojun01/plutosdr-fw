/* SPDX-License-Identifier: GPL-2.0-or-later */
#define main rte_httpd_program_main
#include "../src/httpd.c"
#undef main
#include <assert.h>

static unsigned int rf_calls, target_calls;
static int last_target = -1;
static bool rf_failure, rf_failure_degraded;
static bool rf_quantization;
static const char *target_body = "{\"enabled\":true,\"range_m\":250,\"radial_velocity_mps\":-12,\"gain_linear\":0.5,\"phase_offset_deg\":45}";

/* HTTP tests stop at the controller boundary; controller tests trace MMIO/IIO. */
int rte_controller_open(struct rte_controller *c, const char *path, struct rte_error *error)
{
    (void)path; (void)error;
    memset(c, 0, sizeof(*c));
    c->state.revision = 1;
    c->state.hardware_build_id = RTE_EXPECTED_TIMESTAMP;
    c->state.rf = (struct rte_rf_context){.sample_rate_hz = RTE_FIXED_SAMPLE_RATE_HZ,
        .rx_lo_hz = 2400000000, .tx_lo_hz = 2400000000,
        .rx_bandwidth_hz = 30000000, .tx_bandwidth_hz = 30000000,
        .tx_gain_mdb = -10000, .rx_gain_mdb = 50000, .manual_gain = true,
        .compatible = "ad9364"};
    return 0;
}
void rte_controller_close(struct rte_controller *c) { (void)c; }
int rte_controller_snapshot(struct rte_controller *c, struct rte_snapshot *s, struct rte_error *e)
{ (void)e; *s = c->state; return 0; }
int rte_controller_rf_capabilities(struct rte_controller *c, struct rte_rf_capabilities *v, struct rte_error *e)
{
    (void)c; (void)e;
    *v = (struct rte_rf_capabilities){.min_carrier_hz=70000000,.max_carrier_hz=6000000000,
        .min_bandwidth_hz=200000,.max_bandwidth_hz=40000000,.min_tx_gain_mdb=-89750,
        .max_tx_gain_mdb=0,.tx_gain_step_mdb=250,.min_rx_gain_mdb=-3000,
        .max_rx_gain_mdb=71000,.rx_gain_step_mdb=1000};
    return 0;
}
static int check_revision(struct rte_controller *c, bool has, uint64_t rev, struct rte_error *e)
{
    if (has && rev != c->state.revision) {
        set_error(e, RTE_ERROR_CONFLICT, "revision", "revision conflict"); return -EAGAIN;
    }
    return 0;
}
int rte_controller_apply_rf(struct rte_controller *c, const struct rte_rf_patch *p,
                            bool has, uint64_t rev, struct rte_snapshot *s, struct rte_error *e)
{
    if (check_revision(c,has,rev,e)) return -EAGAIN;
    ++rf_calls;
    if (rf_failure) {
        c->state.degraded = rf_failure_degraded;
        set_error(e, RTE_ERROR_HARDWARE, "rf_rollback", "injected RF restore failure");
        return -EIO;
    }
    if (p->has_carrier_hz) c->state.rf.rx_lo_hz = c->state.rf.tx_lo_hz = p->carrier_hz - (rf_quantization ? 2 : 0);
    if (p->has_bandwidth_hz) c->state.rf.rx_bandwidth_hz = c->state.rf.tx_bandwidth_hz = p->bandwidth_hz;
    if (p->has_tx_gain_db) c->state.rf.tx_gain_mdb = (int32_t)llround(p->tx_gain_db*1000);
    if (p->has_rx_gain_db) c->state.rf.rx_gain_mdb = (int32_t)llround(p->rx_gain_db*1000);
    ++c->state.revision; *s = c->state; return 0;
}
int rte_controller_apply_target(struct rte_controller *c, unsigned int index, const struct rte_config *p,
                                bool has, uint64_t rev, struct rte_snapshot *s, struct rte_error *e)
{
    struct rte_target_state *t;
    assert(index < 4);
    if (check_revision(c,has,rev,e)) return -EAGAIN;
    t = &c->state.targets[index];
    if (rte_encode(p,&c->state.rf,&t->image,&t->applied,e)) return -EINVAL;
    ++target_calls; last_target = (int)index;
    t->has_config = t->hardware_state_known = true;
    t->encoded_carrier_hz = c->state.rf.rx_lo_hz; t->requested = *p;
    ++c->state.revision; *s = c->state; return 0;
}

static void request(struct rte_http_app *app, const char *method, const char *uri,
                     const char *body, const char *etag, int expected, const char *contains)
{
    struct mg_http_message message = {0};
    struct mg_connection connection = {0};
    struct mg_str response;
    char status[32];
    message.method = mg_str(method); message.uri = mg_str(uri);
    message.body = mg_str(body ? body : "");
    if (etag) {
        message.headers[0].name = mg_str("If-Match");
        message.headers[0].value = mg_str(etag);
    }
    http_handler(&connection, MG_EV_HTTP_MSG, &message, app);
    response = mg_str_n((const char *)connection.send.buf, connection.send.len);
    snprintf(status, sizeof(status), "HTTP/1.1 %d", expected);
    if (!mg_strstr(response, mg_str(status))) {
        fprintf(stderr, "%s %s expected %d: %.*s\n", method, uri, expected,
                (int)response.len, response.ptr); abort();
    }
    if (contains) assert(mg_strstr(response, mg_str(contains)) != NULL);
    assert(mg_strstr(response, mg_str("X-Content-Type-Options: nosniff")));
    if (connection.pfn && connection.pfn_data)
        connection.pfn(&connection, MG_EV_CLOSE, NULL, connection.pfn_data);
    mg_iobuf_free(&connection.send);
}

static void test_routes(void)
{
    struct rte_http_app app = {.web_root="www"};
    rte_controller_open(&app.controller,NULL,NULL);
    request(&app,"GET","/healthz",NULL,NULL,200,"\"hardware_state_known\":false");
    request(&app,"GET","/api/v1/system",NULL,NULL,200,"\"id\":4");
    request(&app,"GET","/api/v1/rf/capabilities",NULL,NULL,200,"\"step\":0.250");
    request(&app,"GET","/api/v1/rte/capabilities",NULL,NULL,200,"\"rf_calibrated\":false");
    request(&app,"PATCH","/api/v1/rf/config","{\"tx_gain_db\":-10.25}",NULL,428,NULL);
    request(&app,"PATCH","/api/v1/rf/config","{\"tx_gain_db\":-10.25}","0",409,NULL);
    request(&app,"PATCH","/api/v1/rf/config","{\"sample_rate_hz\":61440000}","1",422,NULL);
    request(&app,"PATCH","/api/v1/rf/config","{\"rx_gain_db\":-3,\"rx_gain_db\":50}","1",422,NULL);
    request(&app,"PATCH","/api/v1/rf/config","{\"tx_gain_db\":-10.25}","\"1\"",200,"\"tx_gain_db\":-10.250");
    assert(rf_calls == 1 && target_calls == 0);
    request(&app,"POST","/api/v1/rte/targets/3/load-param",target_body,"2",200,"\"offset\":\"0x158\"");
    assert(target_calls == 1 && last_target == 2 && rf_calls == 1);
    assert(app.has_saved[2] && !app.has_saved[0]);
    request(&app,"POST","/api/v1/rte/targets/3/load-param",target_body,"2",409,NULL);
    request(&app,"POST","/api/v1/rte/targets/0/load-param",target_body,"3",404,NULL);
    request(&app,"POST","/api/v1/rte/targets/1,2/load-param",target_body,"3",404,NULL);
    request(&app,"POST","/api/v1/rte/targets/01/load-param",target_body,"3",404,NULL);
    request(&app,"POST","/api/v1/rte/targets/5/load-param",target_body,"3",404,NULL);
    request(&app,"POST","/api/v1/rte/targets/1/load-param","{\"enabled\":true}","3",422,NULL);
    request(&app,"POST","/api/v1/rte/targets/1/load-param","[]","3",422,NULL);
    request(&app,"PATCH","/api/v1/rte/config",target_body,"3",405,"Allow: GET");
    request(&app,"PATCH","/api/v1/config",target_body,"3",404,NULL);
    request(&app,"GET","/api/v1/rte/targets/3",NULL,NULL,200,"\"encoded_carrier_hz\":2400000000");
    request(&app,"GET","/api/v1/rte/targets/3/load-param",NULL,NULL,405,"Allow: POST");
    assert(target_calls == 1 && rf_calls == 1);
    request(&app,"GET","/",NULL,NULL,200,"Content-Type: text/html");
    request(&app,"HEAD","/app.css",NULL,NULL,200,"Content-Type: text/css");
    request(&app,"GET","/%2e%2e/README.md",NULL,NULL,404,NULL);
    request(&app,"POST","/index.html",NULL,NULL,405,"Allow: GET, HEAD");
}

static void test_strict_json(void)
{
    static const char *invalid[] = {"", "[]", "{}{}", "{\"x\":01}", "{\"x\":NaN}",
        "{\"x\":1,}", "{\"x\":1.}", "{\"x\":+1}", "{\"x\":1e}", "{\"x\":true false}",
        "{\"x\":\"\\q\"}", "{\"x\":1]}", "{\"x\":[1,]}", "{\"x\":null}junk"};
    struct rte_error error;
    struct rte_rf_patch patch;
    struct rte_config target;
    for (size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i)
        assert(json_root_object(mg_str(invalid[i]),&error) != 0);
    assert(json_root_object(mg_str(" {\"x\":[null, true, {\"y\":-1.2e+3}]} \n"),&error)==0);
    assert(parse_rf_patch(mg_str("{\"tx_gain_db\":\"-1\"}"),"$",&patch,&error)!=0);
    assert(parse_rf_patch(mg_str("{\"carrier_hz\":1.5}"),"$",&patch,&error)!=0);
    assert(parse_rf_patch(mg_str("{\"carrier_hz\":1e300}"),"$",&patch,&error)!=0);
    assert(parse_rf_patch(mg_str("{\"tx_gain_db\":1e999}"),"$",&patch,&error)!=0);
    assert(parse_rf_patch(mg_str("{\"tx_gain\\u005fdb\":-1}"),"$",&patch,&error)!=0);
    assert(parse_target_config(mg_str(target_body),"$",&target,&error)==0);
}

static void test_persistence(void)
{
    char directory[]="/tmp/rte-http-test-XXXXXX", path[PATH_MAX], body[4096], original[4096];
    struct rte_http_app app={0}, restored={0};
    size_t length=0;
    unsigned int old_targets=target_calls, old_rf;
    struct rte_error error;
    struct rte_config config;
    int fd;
    assert(mkdtemp(directory)); snprintf(path,sizeof(path),"%s/state.json",directory);
    app.state_path=path; restored.state_path=path;
    rte_controller_open(&app.controller,NULL,NULL); rte_controller_open(&restored.controller,NULL,NULL);
    assert(parse_target_config(mg_str(target_body),"$",&config,&error)==0);
    app.has_saved[3]=true; app.saved[3]=config;
    schedule_persistence(&app,&app.controller.state);
    assert(app.persistence_dirty); flush_persistence(&app,true); assert(!app.persistence_dirty);
    old_rf=rf_calls;
    restore_persisted_config(&restored);
    assert(rf_calls==old_rf+1 && target_calls==old_targets);
    assert(restored.has_saved[3] && !restored.has_saved[0]);
    for(unsigned int i=0;i<4;++i) assert(!restored.controller.state.targets[i].has_config);
    assert(read_state_file(path,body,sizeof(body),&length)==0);
    memcpy(original,body,length+1);
    /* Wrong hardware profile cannot write RF or latch a target. */
    char *profile=strstr(body,"multitarget-v2"); assert(profile); profile[0]='X';
    fd=open(path,O_WRONLY|O_TRUNC); assert(fd>=0); assert(write(fd,body,length)==(ssize_t)length); close(fd);
    old_rf=rf_calls; restore_persisted_config(&restored);
    assert(rf_calls==old_rf && target_calls==old_targets);
    assert(strstr(restored.warning,"rejected"));
    /* Entire schema/value validation precedes even RF restoration. */
    static const struct { const char *needle; char replacement; } corruptions[] = {
        {"\"schema_version\":2", '3'}, {"\"hardware_build_id\":2609182026", '3'},
        {"\"range_m\":250", '-'}, {"\"4\":{", '5'}
    };
    for (size_t i=0;i<sizeof(corruptions)/sizeof(corruptions[0]);++i) {
        memcpy(body,original,length+1);
        char *found=strstr(body,corruptions[i].needle); assert(found);
        if (i==3) found[1]=corruptions[i].replacement;
        else { char *value=strchr(found,':')+1; *value=corruptions[i].replacement; }
        fd=open(path,O_WRONLY|O_TRUNC); assert(fd>=0);
        assert(write(fd,body,length)==(ssize_t)length); close(fd);
        restore_persisted_config(&restored);
        assert(rf_calls==old_rf && target_calls==old_targets);
        assert(strstr(restored.warning,"rejected"));
    }
    /* A failed RF restore never promotes drafts/applied state or latches targets. */
    fd=open(path,O_WRONLY|O_TRUNC); assert(fd>=0);
    assert(write(fd,original,length)==(ssize_t)length); close(fd);
    for (unsigned int degraded=0;degraded<2;++degraded) {
        memset(&restored,0,sizeof(restored)); restored.state_path=path;
        rte_controller_open(&restored.controller,NULL,NULL);
        rf_failure=true; rf_failure_degraded=degraded!=0;
        restore_persisted_config(&restored);
        assert(strstr(restored.warning,"RF was not restored"));
        assert(!restored.has_saved[3] && !restored.controller.state.targets[3].has_config);
        assert(target_calls==old_targets);
        request(&restored,"GET","/api/v1/system",NULL,NULL,200,"RF was not restored");
        request(&restored,"GET","/healthz",NULL,NULL,degraded?503:200,NULL);
    }
    rf_failure=false; rf_failure_degraded=false;
    /* An RF retune may leave a valid saved draft outside the new Doppler range.
     * It must still restore as a draft, never prevent independent RF restore. */
    app.saved[3].radial_velocity_mps=10000000;
    assert(persist_snapshot(&app,&app.controller.state)==0);
    memset(&restored,0,sizeof(restored)); restored.state_path=path;
    rte_controller_open(&restored.controller,NULL,NULL);
    restore_persisted_config(&restored);
    assert(restored.has_saved[3] && restored.saved[3].radial_velocity_mps==10000000);
    assert(target_calls==old_targets);
    /* Storage failures report warning after successful hardware action. */
    app.state_path=directory;
    schedule_persistence(&app,&app.controller.state);
    flush_persistence(&app,true);
    assert(!app.persistence_dirty && strstr(app.warning,"persistence failed"));
    unlink(path); rmdir(directory);
}

static void test_quantized_rf_persistence(void)
{
    char directory[]="/tmp/rte-http-pll-XXXXXX", path[PATH_MAX], body[4096];
    size_t length;
    struct rte_http_app app={0};
    unsigned int old_targets=target_calls;
    assert(mkdtemp(directory)); snprintf(path,sizeof(path),"%s/state.json",directory);
    app.state_path=path; rte_controller_open(&app.controller,NULL,NULL); rf_quantization=true;
    request(&app,"GET","/api/v1/rf/capabilities",NULL,NULL,200,"\"readback_tolerance_hz\":5");
    request(&app,"PATCH","/api/v1/rf/config",
            "{\"carrier_hz\":2450000000,\"bandwidth_hz\":40000000,\"tx_gain_db\":-10,\"rx_gain_db\":50}",
            "1",200,"\"requested_rf\":{\"carrier_hz\":2450000000");
    request(&app,"GET","/api/v1/system",NULL,NULL,200,"\"rx_lo_hz\":2449999998");
    request(&app,"PATCH","/api/v1/rf/config","{\"tx_gain_db\":-10.25}","2",200,
            "\"requested_rf\":{\"carrier_hz\":2450000000");
    flush_persistence(&app,true);
    for (unsigned int i=0;i<3;i++) {
        assert(read_state_file(path,body,sizeof(body),&length)==0);
        assert(strstr(body,"\"carrier_hz\":2450000000") && !strstr(body,"2449999998"));
        memset(&app,0,sizeof(app)); app.state_path=path;
        rte_controller_open(&app.controller,NULL,NULL);
        restore_persisted_config(&app);
        assert(app.has_requested_rf && app.requested_rf.rx_lo_hz == 2450000000);
        assert(app.controller.state.rf.rx_lo_hz == 2449999998);
        assert(app.controller.state.rf.tx_gain_mdb == -10250);
        assert(persist_snapshot(&app,&app.controller.state)==0);
    }
    assert(target_calls == old_targets);
    /* An external LO observation must supersede the obsolete nominal setpoint. */
    app.controller.state.rf.rx_lo_hz=app.controller.state.rf.tx_lo_hz=2500000000;
    assert(persist_snapshot(&app,&app.controller.state)==0);
    assert(read_state_file(path,body,sizeof(body),&length)==0);
    assert(strstr(body,"\"carrier_hz\":2500000000"));
    rf_quantization=false; unlink(path); rmdir(directory);
}

int main(void)
{
    test_strict_json(); test_routes(); test_persistence(); test_quantized_rf_persistence();
    puts("test_http_json: PASS (strict requests, target-only routing, revisions, draft persistence, static UI)");
    return 0;
}
