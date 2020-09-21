/*
 * nvidia-installer: A tool for installing NVIDIA software packages on
 * Unix and Linux systems.
 *
 * Copyright (C) 2003 NVIDIA Corporation
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses>.
 *
 *
 * backup.c - this source file contains functions used for backing up
 * (and restoring) files that need to be moved out of the way during
 * installation.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <ctype.h>
#include <stdlib.h>

#include "nvidia-installer.h"
#include "user-interface.h"
#include "backup.h"
#include "files.h"
#include "crc.h"
#include "misc.h"
#include "kernel.h"
#include "conflicting-kernel-modules.h"

#define BACKUP_DIRECTORY "$PKG/var/lib/nvidia"
#define BACKUP_LOG       (BACKUP_DIRECTORY "/log")
#define BACKUP_MKDIR_LOG (BACKUP_DIRECTORY "/dirs")




/*
 * Syntax for the backup log file:
 *
 * 1. The first line is the version string, assumed to be in the form:
 *    MAJOR.MINOR-PATCH
 *
 * 2. The second line is the driver description.
 *
 * XXX do we need anything to distinguish between official builds,
 * nightlies, etc...?
 *
 * 3. The rest of the file is file entries; a file entry can be any one of:
 *
 * INSTALLED_FILE: <filename>
 *
 * INSTALLED_SYMLINK: <filename>
 *  <target>
 *
 * BACKED_UP_SYMLINK: <filename>
 *  <target>
 *  <permissions> <uid> <gid>
 *
 * BACKED_UP_FILE_NUM: <filename>
 *  <filesize> <permissions> <uid> <gid>
 *
 */

#define BACKUP_LOG_PERMS (S_IRUSR|S_IWUSR)

#define BACKUP_DIRECTORY_PERMS (S_IRUSR|S_IWUSR|S_IXUSR)


/*
 * uninstall struct
 *
 */

typedef struct {
    
    int    num;
    char  *filename;
    char  *target;
    uint32 crc;
    mode_t mode;
    uid_t  uid;
    gid_t  gid;
    int    ok;
    
} BackupLogEntry;


typedef struct {
    char *version;
    char *description;
    BackupLogEntry *e;
    int n;
} BackupInfo;


static BackupInfo *read_backup_log_file(Options *op);

static void free_backup_info(BackupInfo *b);

static int check_backup_log_entries(Options *op, BackupInfo *b);

static int do_uninstall(Options *op, const char *version,
                        const int skip_depmod);

static int sanity_check_backup_log_entries(Options *op, BackupInfo *b);

static char *create_backwards_compatible_version_string(const char *str);

static int reverse_strlen_compare(const void *a, const void *b);





/*
 * init_backup() - initialize the backup engine; this consists of
 * creating a new backup directory, and writing to the log file that
 * we're about to install a new driver version.
 */

int init_backup(Options *op, Package *p)
{
    mode_t orig_mode;
    char *version;
    FILE *log;
    
    /* remove the directory, if it already exists */

    if (directory_exists(BACKUP_DIRECTORY)) {
        if (!remove_directory(op, BACKUP_DIRECTORY)) {
            return FALSE;
        }
    }

    /* create the backup directory, with perms only for owner */

    if (!mkdir_recursive(op, BACKUP_DIRECTORY, BACKUP_DIRECTORY_PERMS, FALSE)) {
        return FALSE;
    }

    /*
     * fopen below creates the file with mode 0666 & ~umask.
     * In order to ensure the result of that calculation is BACKUP_LOG_PERMS,
     * we temporarily set umask to ~BACKUP_LOG_PERMS to leave just the bits
     * we want.
     *
     * This assumes that BACKUP_LOG does not already exist (if it does, the
     * file permissions will not be modified.) This is assured by the
     * directory removal and re-creation code above.
     */
    orig_mode = umask(~BACKUP_LOG_PERMS);
 
    /* create the log file */
    
    log = fopen(BACKUP_LOG, "a");
    umask(orig_mode);
    if (!log) {
        ui_error(op, "Unable to create backup log file '%s' (%s).",
                 BACKUP_LOG, strerror(errno));
        return FALSE;
    }

    /* write the version and description */

    version = create_backwards_compatible_version_string(p->version);
    
    fprintf(log, "%s\n", version);
    fprintf(log, "%s\n", p->description);

    nvfree(version);
        
    /* close the log file */

    if (fclose(log) != 0) {
        ui_error(op, "Error while closing backup log file '%s' (%s).",
                 BACKUP_LOG, strerror(errno));
        return FALSE;
    }

    return TRUE;
    
} /* init_backup() */



/*
 * do_backup() - backup the specified file.  If it is a regular file,
 * just move it into the backup directory, and add an entry to the log
 * file.
 */

int do_backup(Options *op, const char *filename)
{
    int len, ret, ret_val;
    struct stat stat_buf;
    char *tmp = NULL;
    FILE *log;
    uint32 crc;

    static int backup_file_number = BACKED_UP_FILE_NUM;

    ret_val = FALSE;

    log = fopen(BACKUP_LOG, "a");
    if (!log) {
        ui_error(op, "Unable to open backup log file '%s' (%s).",
                 BACKUP_LOG, strerror(errno));
        return FALSE;
    }
    
    if (lstat(filename, &stat_buf) == -1) {
        switch (errno) {
        case ENOENT:
            ret_val = TRUE;
            break;
        default:
            ui_error(op, "Unable to determine properties for file '%s' (%s).",
                     filename, strerror(errno));
        }
        goto done;
    }

    if (S_ISREG(stat_buf.st_mode)) {
        crc = compute_crc(op, filename);
        len = strlen(BACKUP_DIRECTORY) + 64;
        tmp = nvalloc(len + 1);
        snprintf(tmp, len, "%s/%d", BACKUP_DIRECTORY, backup_file_number);
        if (!nvrename(op, filename, tmp)) {
            ui_error(op, "Unable to backup file '%s'.", filename);
            goto done;
        }
        
        fprintf(log, "%d: %s\n", backup_file_number, filename);
        
        /* write the filesize, permissions, uid, gid */
        fprintf(log, "%u %04o %d %d\n", crc, stat_buf.st_mode,
                stat_buf.st_uid, stat_buf.st_gid);
        
        backup_file_number++;
    } else if (S_ISLNK(stat_buf.st_mode)) {
        tmp = get_symlink_target(op, filename);
        
        ret = unlink(filename);
        if (ret == -1) {
            ui_error(op, "Unable to remove symbolic link '%s' (%s).",
                     filename, strerror(errno));
            goto done;
        }

        fprintf(log, "%d: %s\n", BACKED_UP_SYMLINK, filename);
        fprintf(log, "%s\n", tmp);
        fprintf(log, "%04o %d %d\n", stat_buf.st_mode,
                stat_buf.st_uid, stat_buf.st_gid);
    } else if (S_ISDIR(stat_buf.st_mode)) {

        /* XXX IMPLEMENT ME: recursive moving of a directory */

        ui_error(op, "Unable to backup directory '%s'.", filename);
        goto done;
    } else {
        ui_error(op, "Unable to backup file '%s' (don't know how to deal with "
                 "file type).", filename);
        goto done;
    }
    
    ret_val = TRUE;

 done:

    nvfree(tmp);

    /* close the log file */

    if (fclose(log) != 0) {
        ui_error(op, "Error while closing backup log file '%s' (%s).",
                 BACKUP_LOG, strerror(errno));
        ret_val = FALSE;
    }
    
    return ret_val;
    
} /* do_backup() */



int log_install_file(Options *op, const char *filename)
{
    FILE *log;
    uint32 crc;
    
    /* open the log file */

    log = fopen(BACKUP_LOG, "a");
    if (!log) {
        ui_error(op, "Unable to open backup log file '%s' (%s).",
                 BACKUP_LOG, strerror(errno));
        return FALSE;
    }
    
    fprintf(log, "%d: %s\n", INSTALLED_FILE, filename);
    
    crc = compute_crc(op, filename);

    fprintf(log, "%u\n", crc);
    
    /* close the log file */

    if (fclose(log) != 0) {
        ui_error(op, "Error while closing backup log file '%s' (%s).",
                 BACKUP_LOG, strerror(errno));
        return FALSE;
    }
    
    return TRUE;

} /* log_install_file() */



int log_create_symlink(Options *op, const char *filename, const char *target)
{
    FILE *log;
    
    /* open the log file */

    log = fopen(BACKUP_LOG, "a");
    if (!log) {
        ui_error(op, "Unable to open backup log file '%s' (%s).",
                 BACKUP_LOG, strerror(errno));
        return FALSE;
    }
    
    fprintf(log, "%d: %s\n", INSTALLED_SYMLINK, filename);
    fprintf(log, "%s\n", target);
    
    /* close the log file */

    if (fclose(log) != 0) {
        ui_error(op, "Error while closing backup log file '%s' (%s).",
                 BACKUP_LOG, strerror(errno));
        return FALSE;
    }
    
    return TRUE;

} /* log_create_symlink() */



static int parse_first_line(const char *buf, int *num, char **filename)
{
    char *c, *local_buf;
        
    if (!buf || !num || !filename) return FALSE;

    local_buf = nvstrdup(buf);
    
    c = local_buf;
    while ((*c != '\0') && (*c != ':')) {
        if (!isdigit(*c)) return FALSE;
        c++;
    }
    if (*c == '\0') return FALSE;

    *c = '\0';
    
    *num = strtol(local_buf, NULL, 10);

    c++;
    while(isspace(*c)) c++;

    *filename = nvstrdup(c);

    free(local_buf);

    return TRUE;
}


static int parse_mode_uid_gid(const char *buf, mode_t *mode,
                              uid_t *uid, gid_t *gid)
{
    char *c, *local_buf, *str;

    if (!buf || !mode || !uid || !gid) return FALSE;

    local_buf = nvstrdup(buf);

    c = str = local_buf;
    while ((*c != '\0') && (isdigit(*c))) c++;
    if (*c == '\0') return FALSE;
    *c = '\0';
    *mode = strtol(str, NULL, 8);

    str = ++c;
    while ((*c != '\0') && (isdigit(*c))) c++;
    if (*c == '\0') return FALSE;
    *c = '\0';
    *uid = strtol(str, NULL, 10);

    str = ++c;
    while ((*c != '\0') && (isdigit(*c))) c++;
    *c = '\0';
    *gid = strtol(str, NULL, 10);
      
    free(local_buf);

    return TRUE;
}


static int parse_crc_mode_uid_gid(const char *buf, uint32 *crc, mode_t *mode,
                                  uid_t *uid, gid_t *gid)
{
    char *c, *local_buf, *str;

    if (!buf || !crc || !mode || !uid || !gid) return FALSE;

    local_buf = nvstrdup(buf);

    c = str = local_buf;
    while ((*c != '\0') && (isdigit(*c))) c++;
    if (*c == '\0') return FALSE;
    *c = '\0';
    *crc = strtoul(str, NULL, 10);

    str = ++c;
    while ((*c != '\0') && (isdigit(*c))) c++;
    if (*c == '\0') return FALSE;
    *c = '\0';
    *mode = strtol(str, NULL, 8);

    str = ++c;
    while ((*c != '\0') && (isdigit(*c))) c++;
    if (*c == '\0') return FALSE;
    *c = '\0';
    *uid = strtol(str, NULL, 10);

    str = ++c;
    while ((*c != '\0') && (isdigit(*c))) c++;
    *c = '\0';
    *gid = strtol(str, NULL, 10);
      
    free(local_buf);

    return TRUE;
}


static int parse_crc(const char *buf, uint32 *crc)
{
    char *c, *local_buf, *str;

    if (!buf || !crc) return FALSE;

    local_buf = nvstrdup(buf);

    c = str = local_buf;
    while ((*c != '\0') && (isdigit(*c))) c++;
    *c = '\0';
    *crc = strtoul(str, NULL, 10);

    free(local_buf);

    return TRUE;

} /* parse_crc() */


/*
 * Syntax for the mkdir log file:
 *
 * Each line in the file contains the name of a directory that was created
 * during driver installation.
 *
 * XXX pathnames containing '\n' will break both this file, and the regular
 * backup log file.
 */


/*
 * log_mkdir() - takes a newline-delimited list of directories and appends
 * them to the log of directories created during installation.
 */
int log_mkdir(Options *op, const char *dirs)
{
    FILE *log;

    /*
     * create the backup directory if it doesn't exist; BACKUP_MKDIR_LOG
     * is within BACKUP_DIRECTORY, so the below fopen(3) call depends on
     * the existence of BACKUP_DIRECTORY
     */
    if (!directory_exists(BACKUP_DIRECTORY) &&
        !mkdir_recursive(op, BACKUP_DIRECTORY, BACKUP_DIRECTORY_PERMS, FALSE)) {
        return FALSE;
    }

    /* open the log file */

    log = fopen(BACKUP_MKDIR_LOG, "a");
    if (!log) {
        ui_error(op, "Unable to open mkdir log file '%s' (%s).",
                 BACKUP_MKDIR_LOG, strerror(errno));
        return FALSE;
    }

    fprintf(log, "%s", dirs);
 
    /* close the log file */

    if (fclose(log) != 0) {
        ui_error(op, "Error while closing mkdir log file '%s' (%s).",
                 BACKUP_MKDIR_LOG, strerror(errno));
        return FALSE;
    }
    
    return TRUE;
}



/*
 * reverse_strlen_compare() - Compare two strings by length, for sorting
 * in order of decreasing length.
 */
static int reverse_strlen_compare(const void *a, const void *b)
{
    return strlen(*(char * const *)b) - strlen(*(char * const *)a);
}



/*
 * rmdir_recursive() - Use BACKUP_MKDIR_LOG to find directories that were
 * created by a previous nvidia-installer, and delete any such directories.
 * Returns TRUE if the log is found and all directories are successfully
 * deleted; returns FALSE if any directories failed to be deleted or the
 * log isn't found. The log enries are processed in reverse order, so that
 * child directories get properly deleted before their parents.
 */
static int rmdir_recursive(Options *op)
{
    FILE *log;
    char **dirs;
    int eof = FALSE, ret = TRUE, lines, i;

    /* open the log file */

    log = fopen(BACKUP_MKDIR_LOG, "r");
    if (!log) {
        /* Fail silently: most likely, the current driver was simply installed
         * with an nvidia-installer that didn't log created directories. */
        return FALSE;
    }

    /* Count the number of lines */

    for (lines = 0; !eof; lines++) {
        char *dir;
        dir = fget_next_line(log, &eof);
        nvfree(dir);
    }

    rewind(log);

    dirs = nvalloc(lines * sizeof(char*));

    for (i = 0; i < lines; i++) {
        dirs[i] = fget_next_line(log, &eof); 
    }

    qsort(dirs, lines, sizeof(char*), reverse_strlen_compare);

    for (i = 0; i < lines; i++) {
        if (dirs[i]) {
            /* Ignore empty lines and the backup directory itself, since it is 
             * never empty as long as the dirs file is still around. */
            if (strlen(dirs[i]) && strcmp(dirs[i], BACKUP_DIRECTORY) != 0) {
                if (rmdir(dirs[i]) != 0) {
                    ui_log(op, "Failed to delete the directory '%s' (%s).",
                           dirs[i], strerror(errno));
                    ret = FALSE;
                }
            }
        }
        nvfree(dirs[i]);
    }

    nvfree(dirs);

    if (!ret) {
        ui_warn(op, "Failed to delete some directories. See %s for details.",
                op->log_file_name);
    }

    /* close the log file */

    if (fclose(log) != 0) {
        ui_error(op, "Error while closing mkdir log file '%s' (%s).",
                 BACKUP_MKDIR_LOG, strerror(errno));
        return FALSE;
    }

    return ret;
}



/*
 * do_uninstall() - this function uninstalls a previously installed
 * driver, by parsing the BACKUP_LOG file.
 */

static int do_uninstall(Options *op, const char *version,
                        const int skip_depmod)
{
    BackupLogEntry *e;
    BackupInfo *b;
    int i, len, ok;
    char *tmpstr;
    float percent;
    int removal_failed = FALSE, restore_failed = FALSE;

    static const char existing_installation_is_borked[] = 
        "Your driver installation has been "
        "altered since it was initially installed; this may happen, "
        "for example, if you have since installed the NVIDIA driver through "
        "a mechanism other than nvidia-installer (such as your "
        "distribution's native package management system).  "
        "nvidia-installer will attempt to uninstall as best it can.";
    
    /* do we even have a backup directory? */

    if (access(BACKUP_DIRECTORY, F_OK) == -1) {
        ui_message(op, "No driver backed up.");
        return FALSE;
    }
    
    if ((b = read_backup_log_file(op)) == NULL) return FALSE;

    ok = check_backup_log_entries(op, b);

    if (!ok) {
        if (op->logging) {
            ui_warn(op, "%s  Please see the file '%s' for details.",
                    existing_installation_is_borked, op->log_file_name);

        } else {
            ui_warn(op, "%s", existing_installation_is_borked);
        }
    }
    
    tmpstr = nvstrcat("Uninstalling ", b->description, " (",
                      b->version, "):", NULL);

    run_distro_hook(op, "pre-uninstall");

    ui_status_begin(op, tmpstr, "Uninstalling");

    free(tmpstr);

    /* Remove any installed DKMS modules */

    if (dkms_module_installed(op, version)) {
        ui_log(op, "DKMS module detected; removing...");
        if (!dkms_remove_module(op, version)) {
            ui_warn(op, "Failed to remove installed DKMS module!");
        }
    }

    /*
     * given the list of Backup logfile entries, perform the necessary
     * operations:
     *
     * Step 1: remove everything that was previously installed
     *
     * Step 2: restore everything that was previously backed up
     */

    for (i = 0; i < b->n; i++) {

        percent = (float) i / (float) (b->n * 2);

        e = &b->e[i];

        if (!e->ok) continue;
        switch (e->num) {
            
            /*
             * This is a file that was installed -- now delete it.
             */

        case INSTALLED_FILE:
            if (unlink(e->filename) == -1) {
                ui_log(op, "Unable to remove installed file '%s' (%s).",
                       e->filename, strerror(errno));
                removal_failed = TRUE;
            }
            ui_status_update(op, percent, "%s", e->filename);
            break;

        case INSTALLED_SYMLINK:
            if (unlink(e->filename) == -1) {
                ui_log(op, "Unable to remove installed symlink '%s' (%s).",
                       e->filename, strerror(errno));
                removal_failed = TRUE;
            }
            ui_status_update(op, percent, "%s", e->filename);
            break;
        }
    }
    
    for (i = 0; i < b->n; i++) {
        
        percent = (float) (i + b->n) / (float) (b->n * 2);
        
        e = &b->e[i];
        
        if (!e->ok) continue;
        
        switch (e->num) {
            
          case INSTALLED_FILE:
          case INSTALLED_SYMLINK:
            /* nothing to do */
            break;

          case BACKED_UP_SYMLINK:
            if (symlink(e->target, e->filename) == -1) {
                
                /*
                 * XXX only print this warning if
                 * check_backup_log_entries() didn't see any problems.
                 */

                if (ok) {
                    restore_failed = TRUE;
                }

                ui_log(op, "Unable to restore symbolic link "
                       "%s -> %s (%s).", e->filename, e->target,
                       strerror(errno));
            } else {
                
                /* XXX do we need to chmod the symlink? */

                if (lchown(e->filename, e->uid, e->gid)) {
                    ui_log(op, "Unable to restore owner (%d) and group "
                           "(%d) for symbolic link '%s' (%s).",
                           e->uid, e->gid, e->filename, strerror(errno));
                    restore_failed = TRUE;
                }
            }
            ui_status_update(op, percent, "%s", e->filename);
            break;
            
          default:
            len = strlen(BACKUP_DIRECTORY) + 64;
            tmpstr = nvalloc(len + 1);
            snprintf(tmpstr, len, "%s/%d", BACKUP_DIRECTORY, e->num);
            if (!nvrename(op, tmpstr, e->filename)) {
                ui_log(op, "Unable to restore file '%s'.", e->filename);
                restore_failed = TRUE;
            } else {
                if (chown(e->filename, e->uid, e->gid)) {
                    ui_log(op, "Unable to restore owner (%d) and group "
                           "(%d) for file '%s' (%s).",
                           e->uid, e->gid, e->filename, strerror(errno));
                    restore_failed = TRUE;
                } else {
                    if (chmod(e->filename, e->mode) == -1) {
                        ui_log(op, "Unable to restore permissions %04o for "
                               "file '%s'.", e->mode, e->filename);
                        restore_failed = TRUE;
                    }
                }
            }
            ui_status_update(op, percent, "%s", e->filename);
            free(tmpstr);
            break;
        }
    }

    if (removal_failed) {
        ui_warn(op, "Failed to remove some installed files/symlinks. See %s "
                "for details", op->log_file_name);
    }

    if (restore_failed) {
        ui_warn(op, "Failed to restore some backed up files/symlinks, and/or "
                "their attributes. See %s for details", op->log_file_name);
    }

    if (!rmdir_recursive(op)) {
        ui_log(op, "Unable to delete directories created by previous "
               "installation.");
    }

    ui_status_end(op, "done.");

    /* remove the backup directory */

    if (!remove_directory(op, BACKUP_DIRECTORY)) {
        /* XXX what to do if this fails?... nothing */
    }

    if (!op->skip_module_unload) {
        /*
         * attempt to unload the kernel module(s), but don't abort if this
         * fails: the kernel may not have been configured with support for
         * module unloading or the user might have unloaded it themselves or the
         * module might not have existed at all.
         */

        for (i = 0; i < num_conflicting_kernel_modules; i++) {
            rmmod_kernel_module(op, conflicting_kernel_modules[i]);
        }
    }

    if (op->uninstall) {
        /* Update modules.dep and the ldconfig(8) cache to remove entries for
         * any DSOs and kernel modules that we just uninstalled. */

        int status = 0;

        ui_log(op, "Running %sldconfig:", op->skip_depmod ? "" : "depmod and ");

        if (!op->skip_depmod) {
            char *cmd = nvstrcat(op->utils[DEPMOD], " -a ", op->kernel_name, NULL);
            status |= run_command(op, cmd, NULL, FALSE, 0, FALSE);
            nvfree(cmd);
        }

        status |= run_command(op, op->utils[LDCONFIG], NULL, FALSE, 0, FALSE);

        if (status == 0) {
            ui_log(op, "done.");
        } else {
            ui_log(op, "error!");
            ui_warn(op, "An error occurred while running depmod or ldconfig "
                    "after uninstallation: your system may have stale state "
                    "involving recently uninstalled files.");
        }
    }

    run_distro_hook(op, "post-uninstall");

    free_backup_info(b);

    return TRUE;
    
} /* do_uninstall() */





/*
 * read_backup_log_file() - 
 */

static BackupInfo *read_backup_log_file(Options *op)
{
    struct stat stat_buf;
    char *buf, *c, *line, *filename;
    int fd, num, length, line_num = 0;
    float percent;
    
    BackupLogEntry *e;
    BackupInfo *b = NULL;

    /* check the permissions of the backup directory */

    if (stat(BACKUP_DIRECTORY, &stat_buf) == -1) {
        ui_error(op, "Unable to get properties of %s (%s).",
                 BACKUP_DIRECTORY, strerror(errno));
        return NULL;
    }
    
    if ((stat_buf.st_mode & PERM_MASK) != BACKUP_DIRECTORY_PERMS) {
        ui_error(op, "The directory permissions of %s have been changed since"
                 "the directory was created!", BACKUP_DIRECTORY);
        return NULL;
    }

    if ((fd = open(BACKUP_LOG, O_RDONLY)) == -1) {
        ui_error(op, "Failure opening %s (%s).", BACKUP_LOG, strerror(errno));
        return NULL;
    }

    if (fstat(fd, &stat_buf) == -1) {
        ui_error(op, "Failure getting file properties for %s (%s).",
                 BACKUP_LOG, strerror(errno));
        goto pre_map_fail;
    }

    if ((stat_buf.st_mode & PERM_MASK) != BACKUP_LOG_PERMS) {
        ui_error(op, "The file permissions of %s have been changed since "
                 "the file was written!", BACKUP_LOG);
        goto pre_map_fail;
    }

    /* map the file */

    length = stat_buf.st_size;

    buf = mmap(0, length, PROT_READ, MAP_FILE | MAP_SHARED, fd, 0);
    if (!buf) {
        ui_error(op, "Unable to mmap file '%s' (%s).", BACKUP_LOG,
                 strerror(errno));
        return NULL;
    }
    
    ui_status_begin(op, "Parsing log file:", "Parsing");

    b = nvalloc(sizeof(BackupInfo));

    b->version = get_next_line(buf, &c, buf, length);
    if (!b->version || !c) goto parse_error;
    
    percent = (float) (c - buf) / (float) stat_buf.st_size;
    ui_status_update(op, percent, NULL);

    b->description = get_next_line(c, &c, buf, length);
    if (!b->description || !c) goto parse_error;

    b->n = 0;
    b->e = NULL;
    line_num = 3;

    while(1) {

        percent = (float) (c - buf) / (float) stat_buf.st_size;
        ui_status_update(op, percent, NULL);
        
        /* read and parse the next line */

        line = get_next_line(c, &c, buf, length);
        if (!line) break;

        if (!parse_first_line(line, &num, &filename)) goto parse_error;
        line_num++;
        free(line);

        /* grow the BackupLogEntry array */

        b->n++;
        b->e = (BackupLogEntry *)
            nvrealloc(b->e, sizeof(BackupLogEntry) * b->n);

        memset(&b->e[b->n - 1], 0, sizeof(BackupLogEntry));

        e = &b->e[b->n - 1];
        e->num = num;
        e->filename = filename;
        e->ok = TRUE;
        
        switch(e->num) {

        case INSTALLED_FILE:
            line = get_next_line(c, &c, buf, length);
            if (line == NULL) goto parse_error;
            line_num++;

            if (!parse_crc(line, &e->crc)) goto parse_error;
            free(line);
        
            break;

        case INSTALLED_SYMLINK:
            line = get_next_line(c, &c, buf, length);
            if (line == NULL) goto parse_error;
            line_num++;
            
            e->target = line;
            
            break;
            
        case BACKED_UP_SYMLINK:
            line = get_next_line(c, &c, buf, length);
            if (line == NULL) goto parse_error;
            line_num++;
            
            e->target = line;

            line = get_next_line(c, &c, buf, length);
            if (line == NULL) goto parse_error;
            line_num++;

            if (!parse_mode_uid_gid(line, &e->mode, &e->uid, &e->gid))
                goto parse_error;
            free(line);
          
            break;
            
        default:
            if (num < BACKED_UP_FILE_NUM) goto parse_error;
            
            line = get_next_line(c, &c, buf, length);
            if (line == NULL) goto parse_error;
            line_num++;

            if (!parse_crc_mode_uid_gid(line, &e->crc, &e->mode,
                                        &e->uid, &e->gid)) goto parse_error;
            free(line);

            break;
        }
        
        if (!c) break;
    }

    ui_status_end(op, "done.");
    
    munmap(buf, stat_buf.st_size);
    close(fd);
    
    return b;

 parse_error:
    
    ui_status_end(op, "error.");

    munmap(buf, stat_buf.st_size);

    ui_error(op, "Error while parsing line %d of '%s'.", line_num, BACKUP_LOG);

    nvfree(b);

 pre_map_fail:

    close(fd);
    return NULL;

} /* read_backup_log_file() */



/*
 * free_backup_info() - free the data associated with BackupInfo
 */

static void free_backup_info(BackupInfo *b)
{
    int i;

    if (!b) return;
    
    nvfree(b->version);
    nvfree(b->description);
    
    for (i = 0; i < b->n; i++) {
        nvfree(b->e[i].filename);
        nvfree(b->e[i].target);
    }

    nvfree((char *) b->e);
    
    nvfree((char *) b);

} /* free_backup_info() */



/*
 * check_backup_log_entries() - for each backup log entry, perform
 * some basic sanity checks.  Set the 'ok' field to FALSE if a
 * particular entry should not be uninstalled/restored.
 */

static int check_backup_log_entries(Options *op, BackupInfo *b)
{
    BackupLogEntry *e;
    uint32 crc;
    char *tmpstr;
    int i, j, len, ret = TRUE;
    float percent;

    ui_status_begin(op, "Validating previous installation:", "Validating");
    
    for (i = 0; i < b->n; i++) {

        percent = (float) i / (float) (b->n);

        e = &b->e[i];

        switch (e->num) {

        case INSTALLED_FILE:

            /* check if the file still matches its backup log entry */

            e->ok = check_installed_file(op, e->filename, e->mode, e->crc,
                                         ui_log);
            ret = ret && e->ok;
 
            ui_status_update(op, percent, "%s", e->filename);

            break;

        case INSTALLED_SYMLINK:
            
            /*
             * check if the symlink is still there, and has the same
             * target
             */
            
            if (access(e->filename, F_OK) == -1) {
                ui_log(op, "Unable to access previously installed "
                       "symlink '%s' (%s).", e->filename, strerror(errno));
                ret = e->ok = FALSE;
            } else {
                tmpstr = get_symlink_target(op, e->filename);
                if (!tmpstr) {
                    ret = e->ok = FALSE;
                } else {
                    if (strcmp(tmpstr, e->target) != 0) {
                        ui_log(op, "The previously installed symlink '%s' "
                               "has target '%s', but it was installed "
                               "with target '%s'.  %s will not be "
                               "uninstalled.",
                               e->filename, tmpstr, e->target, e->filename);
                        ret = e->ok = FALSE;
                        
                        /*
                         * if an installed symbolic link has a
                         * different target, then we don't remove it.
                         * This also means that we shouldn't attempt
                         * to restore a backed up symbolic link of the
                         * same name, or whose target matches this
                         * target.
                         */

                        for (j = 0; j < b->n; j++) {
                            if ((b->e[j].num == BACKED_UP_SYMLINK) &&
                                (strcmp(b->e[j].filename, e->filename) == 0)) {
                                b->e[j].ok = FALSE;
                            }
                        }
                    }
                    free(tmpstr);
                }
            }
            ui_status_update(op, percent, "%s", e->filename);

            break;

        case BACKED_UP_SYMLINK:
            
            /* nothing to do */
            
            break;

        default:
            
            /*
             * this is a backed up file; check that the file is still
             * present and has the same crc
             */

            len = strlen(BACKUP_DIRECTORY) + 64;
            tmpstr = nvalloc(len + 1);
            snprintf(tmpstr, len, "%s/%d", BACKUP_DIRECTORY, e->num);
            if (access(tmpstr, F_OK) == -1) {
                ui_log(op, "Unable to access backed up file '%s' "
                       "(saved as '%s') (%s).",
                       e->filename, tmpstr, strerror(errno));
                ret = e->ok = FALSE;
            } else {
                crc = compute_crc(op, tmpstr);
                
                if (crc != e->crc) {
                    ui_log(op, "Backed up file '%s' (saved as '%s) has "
                           "different checksum (%" PRIu32 ") than when it was "
                           "backed up (%" PRIu32").  %s will not be restored.",
                           e->filename, tmpstr, crc, e->crc, e->filename);
                    ret = e->ok = FALSE;
                }
            }
            ui_status_update(op, percent, "%s", tmpstr);
            free(tmpstr);
            break;
        }
    }

    ui_status_end(op, "done.");

    return (ret);

} /* check_backup_log_entries() */



/*
 * get_installed_driver_version_and_descr() - determine the currently
 * installed driver version and description.  Returns the description
 * string if a previous driver is installed.  Returns NULL if no
 * driver is currently installed.
 *
 * XXX for now, we'll just get the installed driver version by reading
 * BACKUP_LOG.  This is probably insufficient, though; how do we
 * detect a driver that was installed prior to the new installer?
 * Should we look for the installed files on the system and pull nvid
 * from them?
 *
 * XXX we should probably check the file permissions of BACKUP_LOG.
 */

int get_installed_driver_version_and_descr(Options *op,
                                           char **pVersion, char **pDescr)
{
    struct stat stat_buf;
    char *c, *version = NULL, *descr = NULL, *buf = NULL;
    char *version_line = NULL;
    int length, fd = -1;
    int ret = FALSE;

    if ((fd = open(BACKUP_LOG, O_RDONLY)) == -1) goto done;
    
    if (fstat(fd, &stat_buf) == -1) goto done;
    
    /* map the file */

    length = stat_buf.st_size;
    
    buf = mmap(0, length, PROT_READ, MAP_FILE | MAP_SHARED, fd, 0);
    if (!buf) goto done;
    
    version_line = get_next_line(buf, &c, buf, length);
    if (!version_line) goto done;

    version = extract_version_string(version_line);

    if (!version) goto done;
    
    descr = get_next_line(c, NULL, buf, length);

    if (!descr) goto done;

    *pVersion = strdup(version);
    *pDescr = strdup(descr);
    
    ret = TRUE;
    
 done:
    nvfree(version_line);
    nvfree(version);
    nvfree(descr);

    if (buf) munmap(buf, stat_buf.st_size);
    if (fd != -1) close(fd);
    
    return ret;

} /* get_installed_driver_version_and_descr() */



/*
 * check_for_existing_driver() - Get the existing driver description
 * and version from BACKUP_LOG.  If an existing driver is present, ask
 * the user if they really want it to be uninstalled.
 *
 * Returns TRUE if it is OK to continue with the installation process.
 * Returns FALSE if the user decided they didn't want to continue with
 * installation.
 *
 * If we are only installing a kernel module, then there must be an
 * existing driver installation, and the version of that installation
 * must match the module we're trying to install.
 */

int check_for_existing_driver(Options *op, Package *p)
{
    char *descr = NULL;
    char *version = NULL;
    int ret = FALSE;
    int localRet;

    if (!check_for_existing_rpms(op)) goto done;

    localRet = get_installed_driver_version_and_descr(op, &version, &descr);

    if (op->kernel_module_only) {
        if (!localRet) {
            ui_error(op, "No NVIDIA driver is currently installed; the "
                     "'--kernel-module-only' option can only be used "
                     "to install the NVIDIA kernel module on top of an "
                     "existing driver installation.");
            goto done;
        } else {
            if (strcmp(p->version, version) != 0) {
                ui_error(op, "The '--kernel-module-only' option can only be "
                         "used to install a kernel module on top of an "
                         "existing driver installation of the same driver "
                         "version.  The existing driver installation is "
                         "%s, but the kernel module is %s.\n",
                         version, p->version);
                goto done;
            } else {
                ret = TRUE;
                goto done;
            }
        }
    }
    
    /* no existing driver -- it is fine to continue with installation */

    if (!localRet) {
        ret = TRUE;
        goto done;
    }

    /*
     * XXX we could do a comparison, here, to check that the Package
     * version is greater than or equal to the installed version, and
     * issue a warning if the user is downgrading.  That doesn't seem
     * necessary, though; I can't think of a good reason why
     * downgrading is any different than upgrading.
     */
    
    if (ui_multiple_choice(op, CONTINUE_ABORT_CHOICES,
                           NUM_CONTINUE_ABORT_CHOICES,
                           CONTINUE_CHOICE, /* Default choice */
                           "There appears to already be a driver installed on "
                           "your system (version: %s).  As part of installing "
                           "this driver (version: %s), the existing driver will "
                           "be uninstalled.  Are you sure you want to continue?",
                           version, p->version) == ABORT_CHOICE) {

        ui_log(op, "Installation aborted.");
        goto done;
    }
    
    ret = TRUE;

 done:

    nvfree(descr);
    nvfree(version);
    
    return ret;

} /* check_for_existing_driver() */



/*
 * uninstall_existing_driver() - check if there is a driver already
 * installed, and if there is, uninstall it.
 *
 * Currently, nothing about this function should cause installation to
 * stop (so it always returns TRUE).
 */

int uninstall_existing_driver(Options *op, const int interactive,
                              const int skip_depmod)
{
    int ret;
    char *descr = NULL;
    char *version = NULL;
    
    ret = get_installed_driver_version_and_descr(op, &version, &descr);
    if (!ret) {
        if (interactive) {
            ui_message(op, "There is no NVIDIA driver currently installed.");
        }
        return TRUE;
    }

    if (interactive && op->uninstall) {
        const char *msg = "If you plan to no longer use the NVIDIA driver, you "
                        "should make sure that no X screens are configured to "
                        "use the NVIDIA X driver in your X configuration file. "
                        "If you used nvidia-xconfig to configure X, it may have "
                        "created a backup of your original configuration. Would "
                        "you like to run `nvidia-xconfig --restore-original-"
                        "backup` to attempt restoration of the original X "
                        "configuration file?";
        run_nvidia_xconfig(op, TRUE, msg, FALSE);
    }

    ret = do_uninstall(op, version, skip_depmod);

    if (ret) {
        if (interactive) {
            ui_message(op, "Uninstallation of existing driver: %s (%s) "
                       "is complete.", descr, version);
        } else {
            ui_log(op, "Uninstallation of existing driver: %s (%s) "
                   "is complete.", descr, version);
        }
    } else {
        ui_error(op, "Uninstallation failed.");
    }
    
    nvfree(descr);
    nvfree(version);

    return TRUE;

} /* uninstall_existing_driver() */

/*
 * Determine if the nvidia-uninstall executable at the path in 'uninstaller'
 * supports the '--skip-depmod' option.  To do this, we simply examine the help
 * text for the presence of the option.
 */
static int check_skip_depmod_support(Options *op, const char *uninstaller)
{
    char *cmd = nvstrcat(uninstaller, " -A | ", op->utils[GREP],
                         " -q '^ \\+--skip-depmod$'", NULL);

    int ret = run_command(op, cmd, NULL, FALSE, 0, FALSE);

    nvfree(cmd);

    /* exit status is 0 in case of success, so invert here */
    return !ret;
}


/*
 * run_existing_uninstaller() - attempt to run `nvidia-uninstall` if it
 * exists; if it does not exist or fails, fall back to normal uninstallation.
 */
int run_existing_uninstaller(Options *op)
{
    char *uninstaller = find_system_util("nvidia-uninstall");

    /*
     * This function is run as part of installation.  If we're about to install
     * kernel modules and run depmod afterwards, we don't need to run depmod
     * as part of uninstallation.
     */
    int skip_depmod = !op->no_kernel_module;

    if (uninstaller) {
        char *uninstall_cmd = NULL;
        char *data = NULL;
        int ret;

        skip_depmod = skip_depmod && check_skip_depmod_support(op, uninstaller);

        /* Run the uninstaller non-interactively, and explicitly log to the
         * uninstall log location: older installers may not do so implicitly. */
        uninstall_cmd = nvstrcat(uninstaller, " -s --log-file-name="
                                 DEFAULT_UNINSTALL_LOG_FILE_NAME,
                                 skip_depmod ? " --skip-depmod" : NULL,
                                 NULL);

        ui_log(op, "Uninstalling the previous installation with %s.",
               uninstaller);

        ret = run_command(op, uninstall_cmd, &data, FALSE, 0, TRUE);

        nvfree(uninstall_cmd);

        /* if nvidia-uninstall succeeded, return early; otherwise, fall back to
         * uninstalling via the backup log file. */
        if (ret == 0) {
            nvfree(uninstaller);
            nvfree(data);
            return TRUE;
        } else {
            ui_log(op, "%s failed; see %s for more details.", uninstaller,
                   DEFAULT_UNINSTALL_LOG_FILE_NAME);
            if (data && strlen(data)) {
                ui_log(op, "The output from %s was:\n%s", uninstaller, data);
            }
            nvfree(data);
        }

        nvfree(uninstaller);
    }

    return uninstall_existing_driver(op, FALSE /* interactive */,
                                     skip_depmod);
}



/*
 * report_driver_information() - report basic information about the
 * currently installed driver.
 */

int report_driver_information(Options *op)
{
    char *descr, *version;
    int ret;
    
    ret = get_installed_driver_version_and_descr(op, &version, &descr);
    if (!ret) {
        ui_message(op, "There is no NVIDIA driver currently installed.");
        return FALSE;
    }

    ui_message(op, "The currently installed driver is: '%s' "
               "(version: %s).", descr, version);
    
    nvfree(descr);
    nvfree(version);

    return TRUE;
    
} /* report_driver_information() */



/*
 * test_installed_files() - 
 */

int test_installed_files(Options *op)
{
    BackupInfo *b;
    int ret;
    
    b = read_backup_log_file(op);
    if (!b) return FALSE;
    
    ret = sanity_check_backup_log_entries(op, b);
    
    /* free resources associated with b */
    
    free_backup_info(b);

    return ret;
    
} /* test_installed_files() */



/*
 * find_installed_file() - scan the backup log file for the specified
 * filename; return TRUE if the filename is listed as an installed file.
 */

int find_installed_file(Options *op, char *filename)
{
    BackupInfo *b;
    BackupLogEntry *e;
    int i, ret = FALSE;
    
    if ((b = read_backup_log_file(op)) == NULL) return FALSE;

    for (i = 0; i < b->n; i++) {
        e = &b->e[i];
        if ((e->num == INSTALLED_FILE) && strcmp(filename, e->filename) == 0) {
            
            /* XXX should maybe compare inodes rather than
               filenames? */

            ret =  TRUE;
            break;
        }
    }

    free_backup_info(b);

    return ret;

} /* find_installed_file() */



/*
 * sanity_check_backup_log_entries() - this function is very similar
 * to check_backup_log_entries(); however, it varies in it's error
 * messages because it is used as part of the sanity check path.
 */

static int sanity_check_backup_log_entries(Options *op, BackupInfo *b)
{
    BackupLogEntry *e;
    uint32 crc;
    char *tmpstr;
    int i, len, ret = TRUE;
    float percent;
    
    ui_status_begin(op, "Validating installation:", "Validating");

    for (i = 0; i < b->n; i++) {
        
        e = &b->e[i];
        
        switch (e->num) {

        case INSTALLED_FILE:
            
            /* check if the file is still there, and has the same crc */
            
            if (access(e->filename, F_OK) == -1) {
                ui_error(op, "The installed file '%s' no longer exists.",
                         e->filename);
                ret = FALSE;
            } else {
                crc = compute_crc(op, e->filename);
                
                if (crc != e->crc) {
                    ui_error(op, "The installed file '%s' has a different "
                             "checksum (%" PRIu32 ") than when it was "
                             "installed (%" PRIu32 ").", e->filename, crc,
                             e->crc);
                    ret = FALSE;
                }
            }
            break;

        case INSTALLED_SYMLINK:
            
            /*
             * check if the symlink is still there, and has the same
             * target
             */
            
            if (access(e->filename, F_OK) == -1) {
                ui_error(op, "The installed symbolic link '%s' no "
                         "longer exists.", e->filename);
                ret = FALSE;
            } else {
                tmpstr = get_symlink_target(op, e->filename);
                if (!tmpstr) {
                    ret = FALSE;
                } else {
                    if (strcmp(tmpstr, e->target) != 0) {
                        ui_error(op, "The installed symbolic link '%s' "
                                 "has target '%s', but it was installed "
                                 "with target '%s'.",
                                 e->filename, tmpstr, e->target);
                        ret = FALSE;
                    }
                    free(tmpstr);
                }
            }
            break;

        case BACKED_UP_SYMLINK:
            
            /* nothing to do */
            
            break;
       
        default:
            
            /*
             * this is a backed up file; check that the file is still
             * present and has the same crc
             */
            
            len = strlen(BACKUP_DIRECTORY) + 64;
            tmpstr = nvalloc(len + 1);
            snprintf(tmpstr, len, "%s/%d", BACKUP_DIRECTORY, e->num);
            if (access(tmpstr, F_OK) == -1) {
                ui_error(op, "The backed up file '%s' (saved as '%s') "
                         "no longer exists.", e->filename, tmpstr);
                ret = FALSE;
            } else {
                crc = compute_crc(op, tmpstr);
                
                if (crc != e->crc) {
                    ui_error(op, "Backed up file '%s' (saved as '%s) has a "
                             "different checksum (%" PRIu32 ") than when it "
                             "was backed up (%" PRIu32 ").", e->filename,
                             tmpstr, crc, e->crc);
                    ret = FALSE;
                }
            }
            free(tmpstr);
            break;
        }
        
        percent = (float) i / (float) (b->n);
        ui_status_update(op, percent, "%s", e->filename);
    }

    ui_status_end(op, "done.");
    
    return ret;
    
} /* sanity_check_backup_log_entries */



/*
 * create_backwards_compatible_version_string() - given the version
 * string 'str', generate a version string to write to the backup log
 * file that can be read by older nvidia-installers that assumed the
 * X.Y-ZZZZ version format.
 *
 * Fortunately, old nvidia-installers' version parsing didn't care if
 * there were extra digits beyond the four Z's (e.g., it could be
 * X.Y-ZZZZZ, or X.Y-ZZZZZZZZ); they would just look for the first 4
 * Z's if they needed to parse the version.
 *
 * So, the strategy is to take the new version string, remove any
 * periods, and print that out as the old style version, followed by
 * the real version string in parenthesis; e.g.,
 *
 *  "1.0-105917 (105.9.17)"
 *
 * In this way, an old nvidia-installer will atleast be able to parse
 * the string, even though it may not understand it, but a new
 * nvidia-installer can be smart and pull out the new version string.
 */

static char *create_backwards_compatible_version_string(const char *str)
{
    char *version, *scratch, *s, *t;
    int len;

    /*
     * create a scratch string by duping the passed in str, but
     * collapse the scratch string by only retaining the digits; e.g.,
     * "105.9.17" --> "105917"
     */
    
    scratch = nvstrdup(str);

    for (s = t = scratch; *t; t++) {
        if (isdigit(*t)) {
            *s++ = *t;
        }
    }
    
    *s = '\0';

    /* allocate a new string to contain the result */

    len = strlen(str);
    version = nvalloc((len * 2) + 16);
    
    sprintf(version, "1.0-%s (%s)", scratch, str);
    
    nvfree(scratch);

    return version;
    
} /* create_backwards_compatible_version_string() */
