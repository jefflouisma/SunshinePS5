#include "session.h"

#include <openssl/crypto.h>
#include <stdlib.h>
#include <string.h>

struct sp5_session {
    sp5_authority *authority;
    bool in_use;
    atomic_int state;
    /* Zero, or the first end claimed by either owner (encoding at claim()).
     * Only the management owner copies it into view and history (session.h). */
    atomic_uint_fast32_t end_claim;
    sp5_session_view view;
    sp5_device_lease lease;
    uint8_t certificate[SP5_CERTIFICATE_CAPACITY];
    size_t certificate_size;
    sp5_host_mode offered[SP5_MODE_CAPACITY];
    size_t offered_count;
    sp5_session_range_change range_events[SP5_SESSION_RANGE_EVENT_CAPACITY];
    size_t range_head, range_count;
};
struct sp5_authority {
    sp5_devices *registry;
    sp5_session sessions[SP5_SESSION_HANDLE_CAPACITY];
    sp5_session *claimed;
    uint64_t last_session_id;
    sp5_session_end_record ends[SP5_SESSION_END_HISTORY_CAPACITY];
    size_t end_count;
    size_t end_next;
};

sp5_authority *sp5_authority_create(sp5_devices *registry)
{
    if (registry == NULL) {
        return NULL;
    }
    sp5_authority *authority = calloc(1, sizeof(*authority));
    if (authority != NULL) {
        authority->registry = registry;
        for (size_t i = 0; i < SP5_SESSION_HANDLE_CAPACITY; ++i) {
            atomic_init(&authority->sessions[i].state, SP5_SESSION_ENDED);
            atomic_init(&authority->sessions[i].end_claim, 0);
        }
        if (!atomic_is_lock_free(&authority->sessions[0].state) ||
            !atomic_is_lock_free(&authority->sessions[0].end_claim)) {
            free(authority);
            return NULL;
        }
    }
    return authority;
}

void sp5_authority_destroy(sp5_authority *authority)
{
    if (authority != NULL) {
        for (size_t i = 0; i < SP5_SESSION_HANDLE_CAPACITY; ++i) {
            sp5_session_destroy(&authority->sessions[i]);
        }
        free(authority);
    }
}

size_t sp5_authority_end_count(const sp5_authority *authority)
{
    return authority != NULL ? authority->end_count : 0;
}

sp5_result sp5_authority_end_at(const sp5_authority *authority, size_t index,
                                sp5_session_end_record *record)
{
    if (authority == NULL || record == NULL || index >= authority->end_count) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    size_t oldest =
        authority->end_count == SP5_SESSION_END_HISTORY_CAPACITY ? authority->end_next : 0;
    *record = authority->ends[(oldest + index) % SP5_SESSION_END_HISTORY_CAPACITY];
    return sp5_success();
}

sp5_result sp5_authority_pair(sp5_authority *authority, sp5_bytes certificate)
{
    static const uint8_t name[] = "Paired device";
    return authority == NULL ? sp5_failure(SP5_REASON_INVALID_ARGUMENT)
                             : sp5_devices_pair(authority->registry, certificate,
                                                (sp5_bytes){name, sizeof(name) - 1}, 0);
}

sp5_result sp5_authority_revoke(sp5_authority *authority, sp5_bytes certificate)
{
    return authority == NULL ? sp5_failure(SP5_REASON_INVALID_ARGUMENT)
                             : sp5_devices_revoke(authority->registry, certificate);
}

sp5_session *sp5_session_create(sp5_authority *authority)
{
    if (authority == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < SP5_SESSION_HANDLE_CAPACITY; ++i) {
        sp5_session *session = &authority->sessions[i];
        if (!session->in_use) {
            session->in_use = true;
            session->authority = authority;
            session->view = (sp5_session_view){.state = SP5_SESSION_ENDED};
            /* No readers of an unused handle; drop the previous session's claim. */
            atomic_store_explicit(&session->end_claim, 0, memory_order_relaxed);
            return session;
        }
    }
    return NULL;
}

void sp5_session_destroy(sp5_session *session)
{
    if (session != NULL && session->in_use) {
        if (session->view.id != 0 && !session->view.has_end_reason) {
            sp5_result result = sp5_session_fail(session, SP5_REASON_OWNERSHIP);
            if (result.status != SP5_OK) {
                return;
            }
        }
        if (session->authority->claimed == session) {
            session->authority->claimed = NULL;
        }
        atomic_store_explicit(&session->state, SP5_SESSION_ENDED, memory_order_release);
        session->in_use = false;
    }
}

sp5_result sp5_session_admit(sp5_session *session, sp5_bytes certificate, uint64_t session_id)
{
    if (session == NULL || !session->in_use || session_id == 0) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    sp5_authority *authority = session->authority;
    if (authority->claimed != NULL || session_id <= authority->last_session_id) {
        return sp5_failure(SP5_REASON_CONFLICT);
    }
    sp5_device_lease lease = sp5_devices_lease(authority->registry, certificate);
    if (!sp5_device_lease_valid(lease)) {
        return sp5_failure(SP5_REASON_OWNERSHIP);
    }
    const sp5_device *device = NULL;
    for (size_t i = 0; i < sp5_devices_count(authority->registry); ++i) {
        const sp5_device *candidate = sp5_devices_at(authority->registry, i);
        if (candidate->certificate_size == certificate.size &&
            CRYPTO_memcmp(candidate->certificate, certificate.data, certificate.size) == 0) {
            device = candidate;
            break;
        }
    }
    if (device == NULL) {
        return sp5_failure(SP5_REASON_OWNERSHIP);
    }
    session->view = (sp5_session_view){.id = session_id, .state = SP5_SESSION_NEGOTIATING};
    memcpy(session->view.device_id, device->id, sizeof(device->id));
    memcpy(session->view.device_name, device->name, sizeof(device->name));
    memcpy(session->certificate, certificate.data, certificate.size);
    session->certificate_size = certificate.size;
    session->lease = lease;
    session->offered_count = 0;
    session->range_head = 0;
    session->range_count = 0;
    authority->last_session_id = session_id;
    authority->claimed = session;
    atomic_store_explicit(&session->end_claim, 0, memory_order_relaxed);
    atomic_store_explicit(&session->state, SP5_SESSION_NEGOTIATING, memory_order_release);
    return sp5_success();
}

static bool valid_mode(const sp5_host_mode *mode)
{
    return mode->width > 0 && mode->height > 0 && mode->fps > 0 && mode->codec[0] != 0 &&
           memchr(mode->codec, 0, sizeof(mode->codec)) != NULL &&
           (mode->dynamic_range == SP5_SDR || mode->dynamic_range == SP5_HDR_PQ_BT2020) &&
           mode->minimum_bitrate_kbps > 0 &&
           mode->maximum_bitrate_kbps >= mode->minimum_bitrate_kbps;
}

sp5_result sp5_session_configure(sp5_session *session, const sp5_host *host,
                                 const sp5_session_parameters *parameters)
{
    if (session == NULL || !session->in_use || host == NULL || parameters == NULL ||
        host->supported_mode_count > SP5_MODE_CAPACITY ||
        parameters->mode_index >= host->supported_mode_count || parameters->requested_kbps == 0 ||
        session->view.state != SP5_SESSION_NEGOTIATING ||
        atomic_load_explicit(&session->state, memory_order_acquire) != SP5_SESSION_NEGOTIATING ||
        !sp5_device_lease_valid(session->lease)) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    const sp5_host_mode *mode = &host->supported_modes[parameters->mode_index];
    if (!valid_mode(mode)) {
        return sp5_failure(SP5_REASON_UNSUPPORTED);
    }
    sp5_session_view *view = &session->view;
    view->width = mode->width;
    view->height = mode->height;
    view->fps = mode->fps;
    memcpy(view->codec, mode->codec, sizeof(mode->codec));
    view->dynamic_range = mode->dynamic_range;
    view->dynamic_range_changed_us = parameters->started_us;
    view->started_us = parameters->started_us;
    view->started_wall_unix_us =
        parameters->wall_unix_us.present ? parameters->wall_unix_us : (sp5_optional_u64){0};
    view->requested_bitrate_kbps = parameters->requested_kbps;
    view->chosen_bitrate_kbps = parameters->requested_kbps;
    view->bitrate_adjustment = SP5_BITRATE_NONE;
    if (parameters->requested_kbps < mode->minimum_bitrate_kbps) {
        view->chosen_bitrate_kbps = mode->minimum_bitrate_kbps;
        view->bitrate_adjustment = SP5_BITRATE_RAISED;
    } else if (parameters->requested_kbps > mode->maximum_bitrate_kbps) {
        view->chosen_bitrate_kbps = mode->maximum_bitrate_kbps;
        view->bitrate_adjustment = SP5_BITRATE_LOWERED;
    }
    view->mode_configured = true;
    session->offered_count = host->supported_mode_count;
    memcpy(session->offered, host->supported_modes,
           session->offered_count * sizeof(*session->offered));
    return sp5_success();
}

sp5_result sp5_session_activate(sp5_session *session)
{
    if (session == NULL || !session->in_use || session->view.state != SP5_SESSION_NEGOTIATING) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    if (!sp5_device_lease_valid(session->lease)) {
        return sp5_failure(SP5_REASON_OWNERSHIP);
    }
    if (!session->view.mode_configured) {
        return sp5_failure(SP5_REASON_UNSUPPORTED);
    }
    /* An end claimed before activation must never reopen input admission. */
    int expected = SP5_SESSION_NEGOTIATING;
    if (!atomic_compare_exchange_strong_explicit(&session->state, &expected, SP5_SESSION_ACTIVE,
                                                 memory_order_acq_rel, memory_order_acquire)) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    session->view.state = SP5_SESSION_ACTIVE;
    return sp5_success();
}

bool sp5_session_authorized(const sp5_session *session, sp5_bytes certificate)
{
    /* Session storage/lease are immutable while active. Release publication at
     * activation and acquire here make them visible to the input owner; registry
     * generation checks independently linearize admission against revocation. */
    return session != NULL &&
           atomic_load_explicit(&session->state, memory_order_acquire) == SP5_SESSION_ACTIVE &&
           certificate.data != NULL && certificate.size == session->certificate_size &&
           CRYPTO_memcmp(certificate.data, session->certificate, certificate.size) == 0 &&
           sp5_device_lease_valid(session->lease);
}

bool sp5_session_authorized_id(const sp5_session *session, sp5_bytes certificate, uint64_t id)
{
    /* id, certificate and lease are immutable until readers are quiesced before
     * handle reuse. Acquire authorization first; avoid copying mutable status. */
    return sp5_session_authorized(session, certificate) && session->view.id == id;
}

sp5_result sp5_session_set_dynamic_range(sp5_session *session, sp5_session_range_change change)
{
    /* The atomic state also reflects an end claimed by the input owner. */
    if (session == NULL || session->view.state != SP5_SESSION_ACTIVE ||
        atomic_load_explicit(&session->state, memory_order_acquire) != SP5_SESSION_ACTIVE ||
        !sp5_device_lease_valid(session->lease) ||
        (change.range != SP5_SDR && change.range != SP5_HDR_PQ_BT2020)) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    sp5_session_view *view = &session->view;
    if (change.observed_us < view->dynamic_range_changed_us) {
        return sp5_failure(SP5_REASON_CLOCK);
    }
    if (change.range == view->dynamic_range) {
        return sp5_success();
    }
    for (size_t i = 0; i < session->offered_count; ++i) {
        const sp5_host_mode *mode = &session->offered[i];
        if (valid_mode(mode) && mode->width == view->width && mode->height == view->height &&
            mode->fps == view->fps && strcmp(mode->codec, view->codec) == 0 &&
            mode->dynamic_range == change.range &&
            view->chosen_bitrate_kbps >= mode->minimum_bitrate_kbps &&
            view->chosen_bitrate_kbps <= mode->maximum_bitrate_kbps) {
            if (view->dynamic_range_sequence == UINT64_MAX) {
                return sp5_failure(SP5_REASON_OVERFLOW);
            }
            if (session->range_count == SP5_SESSION_RANGE_EVENT_CAPACITY) {
                return sp5_failure(SP5_REASON_WOULD_BLOCK);
            }
            change.sequence = view->dynamic_range_sequence + 1;
            size_t tail =
                (session->range_head + session->range_count) % SP5_SESSION_RANGE_EVENT_CAPACITY;
            session->range_events[tail] = change;
            ++session->range_count;
            view->dynamic_range = change.range;
            view->dynamic_range_changed_us = change.observed_us;
            ++view->dynamic_range_sequence;
            return sp5_success();
        }
    }
    return sp5_failure(SP5_REASON_UNSUPPORTED);
}

sp5_result sp5_session_take_range_change(sp5_session *session, sp5_session_range_change *change)
{
    if (session == NULL || !session->in_use || change == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    if (session->range_count == 0) {
        return sp5_failure(SP5_REASON_WOULD_BLOCK);
    }
    *change = session->range_events[session->range_head];
    session->range_head = (session->range_head + 1) % SP5_SESSION_RANGE_EVENT_CAPACITY;
    --session->range_count;
    return sp5_success();
}

/* Claim codes are (reason + 1) << 8 | diagnostic, so zero means unclaimed. */
_Static_assert(SP5_END_ERROR < 255 && SP5_REASON_OWNERSHIP < 256, "end claim encoding");
static bool end_valid(sp5_end_reason reason, sp5_reason diagnostic)
{
    return reason >= SP5_END_CLIENT_QUIT && reason <= SP5_END_ERROR &&
           diagnostic >= SP5_REASON_NONE && diagnostic <= SP5_REASON_OWNERSHIP &&
           (reason != SP5_END_ERROR || diagnostic != SP5_REASON_NONE);
}

static sp5_end_reason claimed_reason(uint_fast32_t code)
{
    return (sp5_end_reason)((code >> 8) - 1);
}

static sp5_reason claimed_diagnostic(uint_fast32_t code)
{
    return (sp5_reason)(code & 0xff);
}

/* Either owner: input admission closes first (never reopening an ended
 * session), then the first claimed reason wins. Atomics only; returns the
 * retained claim code. */
static uint_fast32_t claim(sp5_session *session, sp5_end_reason reason, sp5_reason diagnostic)
{
    int state = atomic_load_explicit(&session->state, memory_order_acquire);
    while ((state == SP5_SESSION_NEGOTIATING || state == SP5_SESSION_ACTIVE) &&
           !atomic_compare_exchange_weak_explicit(&session->state, &state, SP5_SESSION_ENDING,
                                                  memory_order_acq_rel, memory_order_acquire)) {
    }
    uint_fast32_t retained = 0;
    uint_fast32_t code = ((uint_fast32_t)reason + 1) << 8 | (uint_fast32_t)diagnostic;
    if (atomic_compare_exchange_strong_explicit(&session->end_claim, &retained, code,
                                                memory_order_acq_rel, memory_order_acquire)) {
        return code;
    }
    return retained;
}

sp5_result sp5_session_begin_end(sp5_session *session, sp5_end_reason reason, sp5_reason diagnostic)
{
    if (session == NULL || !session->in_use || session->view.id == 0 ||
        !end_valid(reason, diagnostic)) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    uint_fast32_t retained = claim(session, reason, diagnostic);
    if (!session->view.has_end_reason) {
        /* Management owner: record the first reason, possibly input-claimed. */
        sp5_end_reason first = claimed_reason(retained);
        sp5_reason detail = claimed_diagnostic(retained);
        session->view.state = SP5_SESSION_ENDING;
        session->view.has_end_reason = true;
        session->view.end_reason = first;
        session->view.diagnostic = detail;
        sp5_authority *authority = session->authority;
        authority->ends[authority->end_next] =
            (sp5_session_end_record){session->view.id, first, detail};
        authority->end_next = (authority->end_next + 1) % SP5_SESSION_END_HISTORY_CAPACITY;
        if (authority->end_count < SP5_SESSION_END_HISTORY_CAPACITY) {
            ++authority->end_count;
        }
    }
    return sp5_success();
}

sp5_result sp5_session_claim_end(sp5_session *session, sp5_session_end_record end)
{
    if (session == NULL || !end_valid(end.reason, end.diagnostic)) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    /* in_use and id stay immutable while this owner runs: readers are quiesced
     * before handle reuse, exactly as for sp5_session_authorized_id. */
    if (!session->in_use || end.session_id == 0 || session->view.id != end.session_id) {
        return sp5_failure(SP5_REASON_OWNERSHIP);
    }
    (void)claim(session, end.reason, end.diagnostic);
    return sp5_success();
}

bool sp5_session_end_claimed(const sp5_session *session, sp5_end_reason *reason)
{
    uint_fast32_t code =
        session != NULL ? atomic_load_explicit(&session->end_claim, memory_order_acquire) : 0;
    if (code != 0 && reason != NULL) {
        *reason = claimed_reason(code);
    }
    return code != 0;
}

sp5_result sp5_session_end(sp5_session *session, sp5_end_reason reason)
{
    sp5_result result = sp5_session_begin_end(
        session, reason, reason == SP5_END_ERROR ? SP5_REASON_PLATFORM : SP5_REASON_NONE);
    if (result.status == SP5_OK) {
        session->view.state = SP5_SESSION_ENDED;
        atomic_store_explicit(&session->state, SP5_SESSION_ENDED, memory_order_release);
        if (session->authority->claimed == session) {
            session->authority->claimed = NULL;
        }
    }
    return result;
}

sp5_result sp5_session_fail(sp5_session *session, sp5_reason diagnostic)
{
    sp5_result result = sp5_session_begin_end(session, SP5_END_ERROR, diagnostic);
    return result.status == SP5_OK ? sp5_session_end(session, SP5_END_ERROR) : result;
}

sp5_session_view sp5_session_read(const sp5_session *session)
{
    if (session == NULL) {
        return (sp5_session_view){.state = SP5_SESSION_ENDED};
    }
    sp5_session_view view = session->view;
    uint_fast32_t code = atomic_load_explicit(&session->end_claim, memory_order_acquire);
    if (!view.has_end_reason && code != 0) {
        /* Claimed by the input owner; recorded at the next management end. */
        view.state = SP5_SESSION_ENDING;
        view.has_end_reason = true;
        view.end_reason = claimed_reason(code);
        view.diagnostic = claimed_diagnostic(code);
    }
    return view;
}

/* Resource setup before activation, never input admission. */
bool sp5_session_admitted_id(const sp5_session *session, sp5_bytes certificate, uint64_t id)
{
    if (session == NULL)
        return false;
    int state = atomic_load_explicit(&session->state, memory_order_acquire);
    return (state == SP5_SESSION_NEGOTIATING || state == SP5_SESSION_ACTIVE) &&
           certificate.data != NULL && certificate.size == session->certificate_size &&
           CRYPTO_memcmp(certificate.data, session->certificate, certificate.size) == 0 &&
           sp5_device_lease_valid(session->lease) && session->view.id == id;
}
