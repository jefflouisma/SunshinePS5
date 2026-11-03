#ifndef SUNSHINE_SESSION_REVOKE_H
#define SUNSHINE_SESSION_REVOKE_H
#include "lifecycle.h"
/* Serialized management operation. Stop admission/input before durable I/O;
 * finish resource cleanup even on persistence failure. Session/lifecycle may
 * be NULL when idle. Failed persistence never restores current-process access.
 * A pending removal or matching ENDING session retains retry ownership
 * after registry removal. Retries finish cleanup as well as durable storage;
 * an unknown or fully completed removal returns OWNERSHIP. */
sp5_result sp5_revoke_device(sp5_devices *devices, sp5_session *session, sp5_lifecycle *lifecycle,
                             const char *device_id, bool *durable);
#endif
