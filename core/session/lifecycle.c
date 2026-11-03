#include "session/lifecycle.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

enum { MIB = 1024 * 1024 };
struct sp5_lifecycle {
    sp5_lifecycle_ops ops;
    sp5_session_view view;
    sp5_lifecycle_policy policy;
    bool configured, started, prepared, capture, encoder, audio, released;
    atomic_bool input_open;
    sp5_reason cleanup_error;
    uint64_t last_contact;
    sp5_metrics *metrics;
    sp5_controller_input *controllers;
    sp5_session *session;
    uint8_t certificate[SP5_CERTIFICATE_CAPACITY];
    size_t certificate_size;
    uint8_t *pool;
    size_t pool_size;
};
static sp5_result unavailable_description(void *context, sp5_clock_description *description)
{
    (void)context;
    *description = (sp5_clock_description){.reason = SP5_REASON_UNVERIFIED};
    return sp5_failure(SP5_REASON_UNVERIFIED);
}
static sp5_result unavailable_now(void *context, sp5_timestamp *timestamp)
{
    (void)context;
    *timestamp = (sp5_timestamp){.reason = SP5_REASON_UNVERIFIED};
    return sp5_failure(SP5_REASON_UNVERIFIED);
}
static bool valid_ops(const sp5_lifecycle_ops *ops)
{
    return ops != NULL && ops->capture.open != NULL && ops->capture.close != NULL &&
           ops->encoder.open != NULL && ops->encoder.close != NULL && ops->audio.open != NULL &&
           ops->audio.close != NULL && ops->input.add != NULL && ops->input.deliver != NULL &&
           ops->input.remove != NULL && ops->input.release_all != NULL &&
           ops->system.probe != NULL && ops->system.state != NULL && ops->system.prepare != NULL &&
           ops->system.rollback != NULL;
}
sp5_lifecycle *sp5_lifecycle_create(uint64_t session_id, const sp5_lifecycle_ops *ops)
{
    if (session_id == 0 || !valid_ops(ops))
        return NULL;
    sp5_lifecycle *l = calloc(1, sizeof(*l));
    if (l == NULL)
        return NULL;
    l->ops = *ops;
    l->view = (sp5_session_view){.id = session_id, .state = SP5_SESSION_NEGOTIATING};
    atomic_init(&l->input_open, false);
    if (!atomic_is_lock_free(&l->input_open)) {
        free(l);
        return NULL;
    }
    return l;
}
sp5_result sp5_lifecycle_configure(sp5_lifecycle *l, const sp5_lifecycle_policy *p)
{
    if (l == NULL || p == NULL || l->started || l->view.has_end_reason)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    if (p->validity != SP5_VALIDITY_VALID || p->evidence_ref == NULL || p->evidence_ref[0] == 0)
        return sp5_failure(SP5_REASON_UNVERIFIED);
    if (p->media_bytes == 0 || p->media_bytes > (size_t)128 * MIB ||
        p->core_bytes > (size_t)48 * MIB || p->streaming_cpu_us_per_10s > 10000000 ||
        p->idle_cpu_us_per_10s > 500000)
        return sp5_failure(SP5_REASON_CAPACITY);
    if (p->mode_count == 0 || p->mode_count > SP5_MODE_CAPACITY || p->clock.now == NULL ||
        p->clock.describe == NULL || p->metrics_capacity.frame_records == 0 ||
        p->metrics_capacity.input_records == 0 || p->metrics_capacity.queue_records == 0 ||
        p->metrics_context.synthetic != p->synthetic)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    for (size_t i = 0; i < p->mode_count; ++i) {
        const sp5_host_mode *m = &p->modes[i];
        if (m->width == 0 || m->height == 0 || m->fps == 0 || m->codec[0] == 0 ||
            memchr(m->codec, 0, sizeof(m->codec)) == NULL || m->minimum_bitrate_kbps == 0 ||
            m->maximum_bitrate_kbps < m->minimum_bitrate_kbps ||
            (m->dynamic_range != SP5_SDR && m->dynamic_range != SP5_HDR_PQ_BT2020))
            return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    l->policy = *p;
    l->configured = true;
    return sp5_success();
}
sp5_result sp5_lifecycle_bind(sp5_lifecycle *l, sp5_session *session, sp5_bytes certificate)
{
    if (l == NULL || l->started || certificate.size == 0 ||
        certificate.size > sizeof(l->certificate) || certificate.data == NULL ||
        !sp5_session_admitted_id(session, certificate, l->view.id))
        return sp5_failure(SP5_REASON_OWNERSHIP);
    l->session = session;
    memcpy(l->certificate, certificate.data, certificate.size);
    l->certificate_size = certificate.size;
    return sp5_success();
}
static sp5_bytes certificate(const sp5_lifecycle *l)
{
    return (sp5_bytes){l->certificate, l->certificate_size};
}
static sp5_result system_check(sp5_lifecycle *l)
{
    sp5_system_state state = {0};
    sp5_result result = l->ops.system.state(l->ops.system.context, &state);
    if (result.status != SP5_OK)
        return result;
    if (state.validity != SP5_VALIDITY_VALID)
        return sp5_failure(SP5_REASON_UNVERIFIED);
    if (state.remote_play || state.share_play || state.stopping_or_rest)
        return sp5_failure(SP5_REASON_CONFLICT);
    return sp5_success();
}
static void remember(sp5_lifecycle *l, sp5_result result, sp5_result *first)
{
    if (result.status != SP5_OK) {
        if (first->status == SP5_OK)
            *first = result;
        if (l->cleanup_error == SP5_REASON_NONE)
            l->cleanup_error = result.reason;
    }
}
static sp5_reason session_diagnostic(const sp5_lifecycle *l)
{
    return l->view.end_reason == SP5_END_ERROR && l->view.diagnostic == SP5_REASON_NONE
               ? SP5_REASON_PLATFORM
               : l->view.diagnostic;
}
sp5_result sp5_lifecycle_stop_input(sp5_lifecycle *l, sp5_end_reason reason)
{
    if (l == NULL || reason > SP5_END_ERROR)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    atomic_store_explicit(&l->input_open, false, memory_order_release);
    if (!l->view.has_end_reason) {
        l->view.has_end_reason = true;
        l->view.end_reason = reason;
        l->view.state = SP5_SESSION_ENDING;
        if (l->session != NULL) {
            /* Either owner may stop input; only the atomic claim is safe from the
             * input owner. sp5_lifecycle_end records it on the management owner. */
            sp5_result result = sp5_session_claim_end(
                l->session, (sp5_session_end_record){l->view.id, reason, session_diagnostic(l)});
            if (result.status != SP5_OK)
                l->cleanup_error = result.reason;
        }
    }
    if (l->released)
        return sp5_success();
    sp5_result result = l->controllers != NULL
                            ? sp5_controller_close(l->controllers)
                            : l->ops.input.release_all(l->ops.input.context, l->view.id);
    if (result.status == SP5_OK)
        l->released = true;
    else if (l->cleanup_error == SP5_REASON_NONE)
        l->cleanup_error = result.reason;
    return result;
}
sp5_result sp5_lifecycle_end(sp5_lifecycle *l, sp5_end_reason reason)
{
    if (l == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    sp5_result first = sp5_lifecycle_stop_input(l, reason);
    /* Management owner: record the retained (first) reason before media cleanup,
     * so the end is in session history even when a close fails and is retried. */
    if (l->session != NULL && sp5_session_read(l->session).id == l->view.id)
        remember(l, sp5_session_begin_end(l->session, l->view.end_reason, session_diagnostic(l)),
                 &first);
    if (l->audio) {
        sp5_result r = l->ops.audio.close(l->ops.audio.context);
        remember(l, r, &first);
        if (r.status == SP5_OK)
            l->audio = false;
    }
    if (l->encoder) {
        sp5_result r = l->ops.encoder.close(l->ops.encoder.context);
        remember(l, r, &first);
        if (r.status == SP5_OK)
            l->encoder = false;
    }
    if (l->capture) {
        sp5_result r = l->ops.capture.close(l->ops.capture.context);
        remember(l, r, &first);
        if (r.status == SP5_OK)
            l->capture = false;
    }
    if (l->prepared) {
        sp5_result r = l->ops.system.rollback(l->ops.system.context);
        remember(l, r, &first);
        if (r.status == SP5_OK)
            l->prepared = false;
    }
    if (!l->audio && !l->encoder && !l->capture && !l->prepared && l->released) {
        free(l->pool);
        l->pool = NULL;
        l->pool_size = 0;
        l->view.state = SP5_SESSION_ENDED;
        if (l->session != NULL)
            remember(l, sp5_session_end(l->session, l->view.end_reason), &first);
    }
    return first;
}
static sp5_result fail_start(sp5_lifecycle *l, sp5_result result)
{
    l->view.diagnostic = result.reason;
    sp5_result cleanup = sp5_lifecycle_end(l, SP5_END_ERROR);
    (void)cleanup; /* Kept independently in cleanup_diagnostic; preserve root failure. */
    return result;
}
static bool mode_supported(const sp5_lifecycle *l, const sp5_media_config *c)
{
    if (c == NULL || c->codec == NULL || c->width == 0 || c->height == 0 || c->fps_numerator == 0 ||
        c->fps_denominator == 0 || c->chosen_bitrate_kbps == 0)
        return false;
    if (!l->configured)
        return true; /* Trusted synthetic fixture only; probe checked before open. */
    for (size_t i = 0; i < l->policy.mode_count; ++i) {
        const sp5_host_mode *m = &l->policy.modes[i];
        if (m->width == c->width && m->height == c->height &&
            (uint64_t)m->fps * c->fps_denominator == c->fps_numerator &&
            strcmp(m->codec, c->codec) == 0 && m->dynamic_range == c->dynamic_range &&
            c->chosen_bitrate_kbps >= m->minimum_bitrate_kbps &&
            c->chosen_bitrate_kbps <= m->maximum_bitrate_kbps)
            return true;
    }
    return false;
}
sp5_result sp5_lifecycle_start(sp5_lifecycle *l, const sp5_media_config *config, uint64_t now_us)
{
    if (l == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    if (l->started || l->view.has_end_reason)
        return sp5_failure(SP5_REASON_CONFLICT);
    l->started = true;
    if (!mode_supported(l, config))
        return fail_start(l, sp5_failure(SP5_REASON_UNSUPPORTED));
    sp5_platform_provenance provenance = {0};
    sp5_result result = l->ops.system.probe(l->ops.system.context, &provenance);
    if (result.status != SP5_OK)
        return fail_start(l, result);
    if (provenance.validity != SP5_VALIDITY_VALID || (!l->configured && !provenance.synthetic) ||
        (l->configured && l->policy.synthetic != provenance.synthetic))
        return fail_start(l, sp5_failure(SP5_REASON_UNVERIFIED));
    result = system_check(l);
    if (result.status != SP5_OK)
        return fail_start(l, result);
    if (l->session != NULL && !sp5_session_admitted_id(l->session, certificate(l), l->view.id))
        return fail_start(l, sp5_failure(SP5_REASON_OWNERSHIP));
    sp5_clock_ops unavailable = {NULL, unavailable_description, unavailable_now};
    sp5_metrics_capacity capacity =
        l->configured ? l->policy.metrics_capacity : (sp5_metrics_capacity){16, 16, 8};
    l->metrics =
        sp5_metrics_create(l->view.id, capacity, l->configured ? &l->policy.clock : &unavailable);
    if (l->metrics == NULL)
        return fail_start(l, sp5_failure(SP5_REASON_CAPACITY));
    sp5_metrics_context context =
        l->configured ? l->policy.metrics_context : (sp5_metrics_context){.synthetic = true};
    context.mode = *config;
    size_t codec_size = strlen(config->codec);
    if (codec_size >= sizeof(l->view.codec))
        return fail_start(l, sp5_failure(SP5_REASON_INVALID_ARGUMENT));
    memcpy(context.codec, config->codec, codec_size + 1);
    result = sp5_metrics_set_context(l->metrics, &context);
    if (result.status != SP5_OK)
        return fail_start(l, result);
    if (l->configured && l->policy.metrics_enabled) {
        sp5_metrics_view snapshot;
        result = sp5_metrics_snapshot(l->metrics, &snapshot);
        if (result.status != SP5_OK)
            return fail_start(l, result);
        uint64_t enabled_us = snapshot.published_us > now_us ? snapshot.published_us : now_us;
        result = sp5_metrics_enable(l->metrics, true, enabled_us);
        if (result.status != SP5_OK)
            return fail_start(l, result);
    }
    l->pool_size = l->configured ? l->policy.media_bytes : 4096;
    l->pool = calloc(1, l->pool_size);
    l->controllers = l->session != NULL
                         ? sp5_controller_create_reserved(l->session, certificate(l), &l->ops.input)
                         : sp5_controller_create(l->view.id, &l->ops.input);
    if (l->pool == NULL || l->controllers == NULL)
        return fail_start(l, sp5_failure(SP5_REASON_CAPACITY));
    result = l->ops.system.prepare(l->ops.system.context);
    if (result.status != SP5_OK)
        return fail_start(l, result);
    l->prepared = true;
    result = l->ops.capture.open(l->ops.capture.context, config);
    if (result.status != SP5_OK)
        return fail_start(l, result);
    l->capture = true;
    result = l->ops.encoder.open(l->ops.encoder.context, config);
    if (result.status != SP5_OK)
        return fail_start(l, result);
    l->encoder = true;
    result = l->ops.audio.open(l->ops.audio.context, 48000);
    if (result.status != SP5_OK)
        return fail_start(l, result);
    l->audio = true;
    l->last_contact = now_us;
    l->view.started_us = now_us;
    l->view.width = config->width;
    l->view.height = config->height;
    l->view.fps = config->fps_numerator / config->fps_denominator;
    l->view.dynamic_range = config->dynamic_range;
    l->view.chosen_bitrate_kbps = config->chosen_bitrate_kbps;
    l->view.mode_configured = true;
    memcpy(l->view.codec, config->codec, codec_size + 1);
    l->view.state = SP5_SESSION_ACTIVE;
    atomic_store_explicit(&l->input_open, true, memory_order_release);
    return sp5_success();
}
sp5_result sp5_lifecycle_contact(sp5_lifecycle *l, uint64_t now_us)
{
    if (l == NULL || l->view.state != SP5_SESSION_ACTIVE)
        return sp5_failure(SP5_REASON_OWNERSHIP);
    if (now_us < l->last_contact)
        return sp5_failure(SP5_REASON_CLOCK);
    l->last_contact = now_us;
    return sp5_success();
}
sp5_result sp5_lifecycle_tick(sp5_lifecycle *l, uint64_t now_us, bool detected_loss)
{
    if (l == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    if (l->view.state != SP5_SESSION_ACTIVE)
        return sp5_success();
    if (now_us < l->last_contact)
        return fail_start(l, sp5_failure(SP5_REASON_CLOCK));
    if (l->session != NULL && !sp5_session_authorized_id(l->session, certificate(l), l->view.id))
        return sp5_lifecycle_end(l, SP5_END_REVOKED);
    sp5_system_state state = {0};
    sp5_result result = l->ops.system.state(l->ops.system.context, &state);
    if (result.status != SP5_OK || state.validity != SP5_VALIDITY_VALID)
        return fail_start(l, result.status != SP5_OK ? result : sp5_failure(SP5_REASON_UNVERIFIED));
    if (state.remote_play || state.share_play)
        return sp5_lifecycle_end(l, SP5_END_CONSOLE_CONFLICT);
    if (state.stopping_or_rest)
        return sp5_lifecycle_end(l, SP5_END_HOST_STOPPING);
    if (detected_loss || now_us - l->last_contact >= 10000000)
        return sp5_lifecycle_end(l, SP5_END_CLIENT_LOST);
    return sp5_success();
}
sp5_result sp5_lifecycle_input(sp5_lifecycle *l, const sp5_control_event *packet,
                               sp5_timestamp received, uint64_t event_id,
                               sp5_input_evidence *evidence)
{
    if (l == NULL || packet == NULL || evidence == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    if (!sp5_lifecycle_input_open(l) ||
        (l->session != NULL && !sp5_session_authorized_id(l->session, certificate(l), l->view.id)))
        return sp5_failure(SP5_REASON_OWNERSHIP);
    *evidence = (sp5_input_evidence){
        .session_id = l->view.id, .event_id = event_id, .host_received = received};
    sp5_result result = sp5_controller_receive_control(l->controllers, packet,
                                                       sp5_metrics_enabled(l->metrics), evidence);
    if (evidence->delivery_attempted) {
        sp5_result recorded = sp5_metrics_record_input(l->metrics, evidence, true);
        (void)recorded; /* Measurement-only queue loss remains visible in metrics. */
    }
    return result;
}
bool sp5_lifecycle_input_open(const sp5_lifecycle *l)
{
    return l != NULL && atomic_load_explicit(&l->input_open, memory_order_acquire);
}
sp5_metrics *sp5_lifecycle_metrics(sp5_lifecycle *l)
{
    return l != NULL ? l->metrics : NULL;
}
sp5_bytes sp5_lifecycle_pool(const sp5_lifecycle *l)
{
    return l != NULL ? (sp5_bytes){l->pool, l->pool_size} : (sp5_bytes){0};
}
sp5_result sp5_lifecycle_media_buffer(sp5_lifecycle *l, size_t offset, sp5_buffer *buffer,
                                      size_t capacity)
{
    if (l == NULL || buffer == NULL || l->pool == NULL || offset > l->pool_size || capacity == 0 ||
        capacity > l->pool_size - offset)
        return sp5_failure(SP5_REASON_CAPACITY);
    return sp5_buffer_init(buffer, l->pool + offset, capacity);
}
sp5_session_view sp5_lifecycle_read(const sp5_lifecycle *l)
{
    return l != NULL ? l->view : (sp5_session_view){.state = SP5_SESSION_ENDED};
}
sp5_reason sp5_lifecycle_cleanup_diagnostic(const sp5_lifecycle *l)
{
    return l != NULL ? l->cleanup_error : SP5_REASON_INVALID_ARGUMENT;
}
const char *sp5_lifecycle_explanation(const sp5_lifecycle *l)
{
    if (l == NULL)
        return "The session is unavailable. Start a new session.";
    if (l->cleanup_error != SP5_REASON_NONE)
        return "Session cleanup failed. Stop the host and inspect the adapter diagnostics.";
    if (l->view.diagnostic == SP5_REASON_CLOCK)
        return "The host clock moved backwards. Check the clock adapter and reconnect.";
    if (l->view.diagnostic == SP5_REASON_UNSUPPORTED)
        return "This mode is unsupported. Choose a mode in the verified capability list.";
    if (l->view.diagnostic == SP5_REASON_CAPACITY)
        return "The session exceeds available pools or budgets. Reduce the mode and retry.";
    if (l->view.diagnostic == SP5_REASON_UNVERIFIED)
        return "Required platform evidence is unavailable. Verify the adapter before streaming.";
    if (l->view.has_end_reason) {
        switch (l->view.end_reason) {
        case SP5_END_CLIENT_QUIT:
            return "The client ended the stream. Reconnect to stream again.";
        case SP5_END_CLIENT_LOST:
            return "Contact with the client was lost. Check the network and reconnect.";
        case SP5_END_REVOKED:
            return "This device was revoked. Pair it again to reconnect.";
        case SP5_END_HOST_STOPPING:
            return "The host is stopping. Restart it before reconnecting.";
        case SP5_END_CONSOLE_CONFLICT:
            return "Remote Play or Share Play is active. Stop it before reconnecting.";
        case SP5_END_ERROR:
            return "The session failed. Inspect the adapter diagnostics and reconnect.";
        }
    }
    return "The session is ready. Connect with a paired client.";
}
void sp5_lifecycle_destroy(sp5_lifecycle *l)
{
    if (l == NULL)
        return;
    sp5_result result = sp5_lifecycle_end(l, SP5_END_HOST_STOPPING);
    if (result.status != SP5_OK)
        return; /* Caller must retry cleanup; never discard live ownership. */
    sp5_controller_destroy(l->controllers);
    sp5_metrics_destroy(l->metrics);
    free(l);
}
