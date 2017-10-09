/***
  This file is part of systemd.

  Copyright 2013 Zbigniew Jędrzejewski-Szmek
  Copyright 2015 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include "acl-util.h"
#include "fs-util.h"
#include "hashmap.h"
#include "journal-internal.h"
#include "journal-util.h"
#include "log.h"
#include "strv.h"
#include "user-util.h"

static int access_check_var_log_journal(sd_journal *j) {
#if HAVE_ACL
        _cleanup_strv_free_ char **g = NULL;
        const char* dir;
#endif
        int r;

        assert(j);

        /* If we are root, we should have access, don't warn. */
        if (getuid() == 0)
                return 0;

        /* If we are in the 'systemd-journal' group, we should have
         * access too. */
        r = in_group("systemd-journal");
        if (r < 0)
                return log_error_errno(r, "Failed to check if we are in the 'systemd-journal' group: %m");
        if (r > 0)
                return 0;

#if HAVE_ACL
        if (laccess("/run/log/journal", F_OK) >= 0)
                dir = "/run/log/journal";
        else
                dir = "/var/log/journal";

        /* If we are in any of the groups listed in the journal ACLs,
         * then all is good, too. Let's enumerate all groups from the
         * default ACL of the directory, which generally should allow
         * access to most journal files too. */
        r = acl_search_groups(dir, &g);
        if (r < 0)
                return log_error_errno(r, "Failed to search journal ACL: %m");
        if (r > 0)
                return 0;

        /* Print a pretty list, if there were ACLs set. */
        if (!strv_isempty(g)) {
                _cleanup_free_ char *s = NULL;

                /* Thre are groups in the ACL, let's list them */
                r = strv_extend(&g, "systemd-journal");
                if (r < 0)
                        return log_oom();

                strv_sort(g);
                strv_uniq(g);

                s = strv_join(g, "', '");
                if (!s)
                        return log_oom();

                log_notice("Hint: You are currently not seeing messages from other users and the system.\n"
                           "      Users in groups '%s' can see all messages.\n"
                           "      Pass -q to turn off this notice.", s);
                return 1;
        }
#endif

        /* If no ACLs were found, print a short version of the message. */
        log_notice("Hint: You are currently not seeing messages from other users and the system.\n"
                   "      Users in the 'systemd-journal' group can see all messages. Pass -q to\n"
                   "      turn off this notice.");

        return 1;
}

int journal_access_check_and_warn(sd_journal *j, bool quiet) {
        Iterator it;
        void *code;
        char *path;
        int r = 0;

        assert(j);

        if (hashmap_isempty(j->errors)) {
                if (ordered_hashmap_isempty(j->files) && !quiet)
                        log_notice("No journal files were found.");

                return 0;
        }

        if (hashmap_contains(j->errors, INT_TO_PTR(-EACCES))) {
                if (!quiet)
                        (void) access_check_var_log_journal(j);

                if (ordered_hashmap_isempty(j->files))
                        r = log_error_errno(EACCES, "No journal files were opened due to insufficient permissions.");
        }

        HASHMAP_FOREACH_KEY(path, code, j->errors, it) {
                int err;

                err = abs(PTR_TO_INT(code));

                switch (err) {
                case EACCES:
                        continue;

                case ENODATA:
                        log_warning_errno(err, "Journal file %s is truncated, ignoring file.", path);
                        break;

                case EPROTONOSUPPORT:
                        log_warning_errno(err, "Journal file %1$s uses an unsupported feature, ignoring file.\n"
                                               "Use SYSTEMD_LOG_LEVEL=debug journalctl --file=%1$s to see the details.",
                                               path);
                        break;

                case EBADMSG:
                        log_warning_errno(err, "Journal file %s corrupted, ignoring file.", path);
                        break;

                default:
                        log_warning_errno(err, "An error was encountered while opening journal file or directory %s, ignoring file: %m", path);
                        break;
                }
        }

        return r;
}
