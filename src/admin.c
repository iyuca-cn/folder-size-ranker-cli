#include <windows.h>

#include "model.h"

bool mftscan_is_process_elevated(void) {
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID administrators_group = NULL;
    BOOL is_member = FALSE;

    if (!AllocateAndInitializeSid(
            &authority,
            2,
            SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS,
            0,
            0,
            0,
            0,
            0,
            0,
            &administrators_group)) {
        return false;
    }

    if (!CheckTokenMembership(NULL, administrators_group, &is_member)) {
        is_member = FALSE;
    }

    FreeSid(administrators_group);
    return is_member == TRUE;
}
