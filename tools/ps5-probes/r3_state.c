/* Nonshipping R3 state probe: candidate Remote Play / Share Play status calls.
 * Candidate signatures are from the PS5 SDK stub symbol set and ShadPS4's
 * remoteplay/systemservice HLE declarations; this probe observes the actual
 * firmware returns. No media, no hooks, no mutation. */
#include <stdint.h>
#include <stdio.h>

int sceUserServiceInitialize(const void *params);
int sceUserServiceGetForegroundUser(int32_t *user_id);
int sceUserServiceTerminate(void);
int sceRemoteplayGetConnectionStatus(int32_t user_id, int32_t *status);
int sceRemoteplayIsRemotePlaying(void);
int sceShellCoreUtilGetSharePlayStatus(void);
int sceShellCoreUtilGetRemotePlayStatus(void);

static void line(const char *name, int rc, int value)
{
    printf("R3_STATE call=%s rc=%d value=%d\n", name, rc, value);
    fflush(stdout);
}

int main(void)
{
    int rc = sceUserServiceInitialize(NULL);
    int32_t user = -1;
    int user_rc = rc == 0 ? sceUserServiceGetForegroundUser(&user) : rc;
    printf("R3_STATE begin user_init_rc=%d user_rc=%d user=%d\n", rc, user_rc, (int)user);
    fflush(stdout);

    int status = -1;
    rc = sceRemoteplayGetConnectionStatus(user, &status);
    line("remoteplay_connection", rc, status);

    rc = sceRemoteplayIsRemotePlaying();
    line("remoteplay_is_playing", rc, rc);

    rc = sceShellCoreUtilGetSharePlayStatus();
    line("shareplay_status", rc, rc);

    rc = sceShellCoreUtilGetRemotePlayStatus();
    line("shell_remoteplay_status", rc, rc);

    rc = sceUserServiceTerminate();
    printf("R3_STATE end user_term_rc=%d\n", rc);
    fflush(stdout);
    return 0;
}
