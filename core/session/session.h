#ifndef SUNSHINE_SESSION_SESSION_H
#define SUNSHINE_SESSION_SESSION_H

#include "settings/devices.h"

/* One authority per host, bound to its sole registry for its entire lifetime.
 * Create/destroy allocate only at setup/teardown; session handles come from a
 * fixed pool. Management operations, including all snapshots, are serialized.
 * The independent input owner calls authorized() using only immutable session
 * certificate/lease and atomic state. Quiesce readers before reusing or
 * destroying a session/registry; stopping never waits on persistence or media.
 * Increasing nonzero session IDs prevent old-session credential resurrection. */
/* Same immutable lease/atomic authorization as authorized(), plus launch ID.
 * Safe for the independent input owner; no mutable management snapshot copy. */
bool sp5_session_authorized_id(const sp5_session *session, sp5_bytes certificate, uint64_t id);
/* Setup-only admission check permits reservation before activation. Immutable
 * lease and certificate checks; no input authorization is granted. */
bool sp5_session_admitted_id(const sp5_session *session, sp5_bytes certificate, uint64_t id);
enum { SP5_SESSION_HANDLE_CAPACITY = 4 };
enum { SP5_SESSION_END_HISTORY_CAPACITY = 16 };
typedef struct {
    uint64_t session_id;
    sp5_end_reason reason;
    sp5_reason diagnostic;
} sp5_session_end_record;
size_t sp5_authority_end_count(const sp5_authority *authority);
sp5_result sp5_authority_end_at(const sp5_authority *authority, size_t index,
                                sp5_session_end_record *record);
typedef struct {
    size_t mode_index;
    uint32_t requested_kbps;
    uint64_t started_us;
    sp5_optional_u64 wall_unix_us;
} sp5_session_parameters;
sp5_result sp5_session_configure(sp5_session *session, const sp5_host *host,
                                 const sp5_session_parameters *parameters);
/* Timestamped transitions queue bounded signals for the control owner (T059).
 * A full queue rejects a change without losing an already accepted event.
 * HDR must be a validated offered variant of the negotiated geometry/codec. */
typedef struct {
    sp5_dynamic_range range;
    uint64_t observed_us;
    uint64_t sequence;
} sp5_session_range_change;
enum { SP5_SESSION_RANGE_EVENT_CAPACITY = 4 };
sp5_result sp5_session_set_dynamic_range(sp5_session *session, sp5_session_range_change change);
sp5_result sp5_session_take_range_change(sp5_session *session, sp5_session_range_change *change);
sp5_result sp5_session_fail(sp5_session *session, sp5_reason diagnostic);
/* Ends input admission first; finish retains the first reason. Resource cleanup
 * and durable last_session_at update belong to the lifecycle management owner.
 * Management only: also records the retained (possibly input-claimed) reason
 * once in the snapshot and bounded end history. */
sp5_result sp5_session_begin_end(sp5_session *session, sp5_end_reason reason,
                                 sp5_reason diagnostic);
/* Independent input/control owner: closes input admission and claims an end
 * with lock-free atomics only; the first claim from either owner is retained.
 * Never writes management snapshots or history: sp5_session_read reports the
 * claim at once and the management owner records it at its next begin_end,
 * end or fail. Another launch ID (stale owner) is OWNERSHIP, nothing claimed. */
sp5_result sp5_session_claim_end(sp5_session *session, sp5_session_end_record end);
/* Input-owner read of the retained claim; false while no end is claimed. */
bool sp5_session_end_claimed(const sp5_session *session, sp5_end_reason *reason);

#endif
