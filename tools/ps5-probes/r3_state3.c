/* Nonshipping R3 state probe 3: system state + candidate SharePlay/RemotePlay getters. */
#include <stdint.h>
#include <stdio.h>
int sceUserServiceInitialize(const void *params);
int sceUserServiceGetForegroundUser(int32_t *user_id);
int sceUserServiceTerminate(void);
int sceRemoteplayIsRemotePlaying(void);
int sceSystemStateMgrGetCurrentState(void);
int sceSystemStateMgrGetTriggerCode(void);
int sceShellCoreUtilGetSharePlayStatus(int32_t *status);
int sceShellCoreUtilGetRemotePlayStatus(int32_t *status);
static void out(const char *name, int rc, int value)
{
    printf("R3_STATE3 call=%s rc=%d value=%d\n", name, rc, value);
    fflush(stdout);
}
int main(void)
{
    int rc = sceUserServiceInitialize(NULL);
    int32_t user = -1;
    int urc = rc == 0 ? sceUserServiceGetForegroundUser(&user) : rc;
    printf("R3_STATE3 begin user_init=%d user_rc=%d user=%d\n", rc, urc, (int)user);
    fflush(stdout);
    rc = sceRemoteplayIsRemotePlaying();
    out("remoteplay_is_playing", rc, rc);
    rc = sceSystemStateMgrGetCurrentState();
    out("system_state", rc, rc);
    rc = sceSystemStateMgrGetTriggerCode();
    out("system_trigger", rc, rc);
    int status = -1;
    rc = sceShellCoreUtilGetRemotePlayStatus(&status);
    out("shell_remoteplay_ptr", rc, status);
    status = -1;
    rc = sceShellCoreUtilGetSharePlayStatus(&status);
    out("shareplay_ptr", rc, status);
    rc = sceUserServiceTerminate();
    out("user_terminate", rc, rc);
    return 0;
}
