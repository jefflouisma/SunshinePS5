/* Nonshipping R3 state probe 2: Remote Play initialize/status only. */
#include <stdint.h>
#include <stdio.h>
int sceUserServiceInitialize(const void *params);
int sceUserServiceGetForegroundUser(int32_t *user_id);
int sceUserServiceTerminate(void);
int sceRemoteplayInitialize(void);
int sceRemoteplayGetConnectionStatus(int32_t user_id, int32_t *status);
int sceRemoteplayIsRemotePlaying(void);
int sceRemoteplayTerminate(void);
static void out(const char *name, int rc, int value)
{
    printf("R3_STATE2 call=%s rc=%d value=%d\n", name, rc, value);
    fflush(stdout);
}
int main(void)
{
    int rc = sceUserServiceInitialize(NULL);
    int32_t user = -1;
    int urc = rc == 0 ? sceUserServiceGetForegroundUser(&user) : rc;
    printf("R3_STATE2 begin user_init=%d user_rc=%d user=%d\n", rc, urc, (int)user);
    fflush(stdout);
    rc = sceRemoteplayInitialize();
    out("remoteplay_initialize", rc, rc);
    int status = -1;
    rc = sceRemoteplayGetConnectionStatus(user, &status);
    out("remoteplay_connection", rc, status);
    rc = sceRemoteplayIsRemotePlaying();
    out("remoteplay_is_playing", rc, rc);
    rc = sceRemoteplayTerminate();
    out("remoteplay_terminate", rc, rc);
    rc = sceUserServiceTerminate();
    out("user_terminate", rc, rc);
    return 0;
}
