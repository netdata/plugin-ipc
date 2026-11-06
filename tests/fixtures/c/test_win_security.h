/*
 * test_win_security.h - Assertions on the NetIPC Windows access policy.
 */

#ifndef TEST_WIN_SECURITY_H
#define TEST_WIN_SECURITY_H

#include <aclapi.h>
#include <sddl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <windows.h>

static inline bool test_sid_is_process_user(PSID sid)
{
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    DWORD len = 0;
    GetTokenInformation(token, TokenUser, NULL, 0, &len);
    TOKEN_USER *user = malloc(len);
    bool same = user && GetTokenInformation(token, TokenUser, user, len, &len) &&
                EqualSid(sid, user->User.Sid);
    free(user);
    CloseHandle(token);
    return same;
}

/* True when the object's DACL is protected and holds exactly one allow ACE,
 * with access mask full_access, for each of SYSTEM, Administrators and the
 * process user. */
static inline bool test_object_dacl_owner_only(HANDLE object, DWORD full_access)
{
    PACL dacl = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    if (GetSecurityInfo(object, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
                        NULL, NULL, &dacl, NULL, &sd) != ERROR_SUCCESS)
        return false;

    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    bool ok = GetSecurityDescriptorControl(sd, &control, &revision) &&
              (control & SE_DACL_PROTECTED) && dacl && dacl->AceCount == 3;
    int system = 0, admins = 0, self = 0;
    for (DWORD i = 0; ok && i < dacl->AceCount; i++) {
        ACCESS_ALLOWED_ACE *ace = NULL;
        if (!GetAce(dacl, i, (void **)&ace) ||
            ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE ||
            ace->Mask != full_access) {
            ok = false;
            break;
        }
        PSID sid = (PSID)&ace->SidStart;
        if (IsWellKnownSid(sid, WinLocalSystemSid))
            system++;
        else if (IsWellKnownSid(sid, WinBuiltinAdministratorsSid))
            admins++;
        else if (test_sid_is_process_user(sid))
            self++;
        else
            ok = false;
    }
    LocalFree(sd);
    return ok && system == 1 && admins == 1 && self == 1;
}

/* The object's owner as a SID string, LocalFree'd by the caller. */
static inline char *test_object_owner_sid(HANDLE object)
{
    PSID owner = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    char *str = NULL;
    if (GetSecurityInfo(object, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION,
                        &owner, NULL, NULL, NULL, &sd) != ERROR_SUCCESS)
        return NULL;
    if (!ConvertSidToStringSidA(owner, &str))
        str = NULL;
    LocalFree(sd);
    return str;
}

#endif /* TEST_WIN_SECURITY_H */
