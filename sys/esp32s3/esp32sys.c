/* NetHack 5.0  esp32sys.c -- ESP32-S3 POSIX shims for the libnh cross build.
 *
 * NetHack's UNIX code paths reference POSIX functions that ESP-IDF's newlib
 * does not implement (fork, execv, getpwuid, popen, ...) and a handful of
 * helpers that we lost when we excluded sys/share/{ioctl,unixtty}.c from
 * the cross build (error, intron, introff, dosuspend).  Their job here is
 * to keep the linker happy; semantically NetHack should never actually
 * exercise these paths because we set -DNOMAIL -DNO_SIGNAL -DNOCRASHREPORT
 * -DNOPANICTRACE -DNOSHELL.  If one fires at run time, the right fix is
 * an upstream guard, not making the stub do anything useful.
 *
 * Only built when CROSS_TO_ESP32S3 is set (see top-level Makefile).
 */

#include <dirent.h>
#include <errno.h>
#include <pwd.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* WEAK qualifier so ESP-IDF VFS strong definitions win when both are
 * present.  The point of these stubs is not to do anything useful at run
 * time -- it is to prevent the static linker from pulling libnosys.a's
 * dir.o/getcwd.o/chmod.o (which would multi-def against esp_vfs.a). */
#define NH_WEAK __attribute__((weak))

/*--------------------------------------------------------------------
 * Process control (newlib has none of these on Xtensa)
 *------------------------------------------------------------------*/

pid_t
fork(void)
{
    errno = ENOSYS;
    return (pid_t) -1;
}

int
execv(const char *path, char *const argv[])
{
    (void) path;
    (void) argv;
    errno = ENOSYS;
    return -1;
}

int
execve(const char *path, char *const argv[], char *const envp[])
{
    (void) path;
    (void) argv;
    (void) envp;
    errno = ENOSYS;
    return -1;
}

pid_t
wait(int *status)
{
    (void) status;
    errno = ECHILD;
    return (pid_t) -1;
}

pid_t
waitpid(pid_t pid, int *status, int options)
{
    (void) pid;
    (void) status;
    (void) options;
    errno = ECHILD;
    return (pid_t) -1;
}

/*--------------------------------------------------------------------
 * User/group identity (newlib returns 0/error variants)
 *------------------------------------------------------------------*/

/* Newlib on the Xtensa toolchain does not provide getuid/getgid in
 * libc; FreeRTOS does not have a notion of users.  Returning 0 (root)
 * matches what NetHack's WASM build sees and lets the lockfile/save
 * path naming code keep working. */
uid_t
getuid(void)
{
    return (uid_t) 0;
}

gid_t
getgid(void)
{
    return (gid_t) 0;
}

uid_t
geteuid(void)
{
    return (uid_t) 0;
}

gid_t
getegid(void)
{
    return (gid_t) 0;
}

int
setuid(uid_t uid)
{
    (void) uid;
    errno = ENOSYS;
    return -1;
}

int
setgid(gid_t gid)
{
    (void) gid;
    errno = ENOSYS;
    return -1;
}

int
seteuid(uid_t uid)
{
    (void) uid;
    errno = ENOSYS;
    return -1;
}

int
setegid(gid_t gid)
{
    (void) gid;
    errno = ENOSYS;
    return -1;
}

char *
getlogin(void)
{
    /* Used by sys/libnh/libnhmain.c::whoami() before sysconf overrides
     * the player name; returning a fixed string is fine. */
    return (char *) "player";
}

static struct passwd nh_pwent = {
    .pw_name = (char *) "player",
    .pw_passwd = (char *) "",
    .pw_uid = 0,
    .pw_gid = 0,
    .pw_gecos = (char *) "T-Deck player",
    .pw_dir = (char *) "/sdcard",
    .pw_shell = (char *) "",
};

struct passwd *
getpwuid(uid_t uid)
{
    (void) uid;
    return &nh_pwent;
}

struct passwd *
getpwnam(const char *name)
{
    (void) name;
    return &nh_pwent;
}

/*--------------------------------------------------------------------
 * Filesystem (newlib provides chmod/unlink via VFS but not these)
 *------------------------------------------------------------------*/

mode_t
umask(mode_t mask)
{
    (void) mask;
    return (mode_t) 0022;
}

/* ESP-IDF VFS provides access(), chmod(), the dir functions, and getcwd().
 * We provide WEAK fallback stubs ONLY to prevent the linker from going
 * looking in libnosys.a/dir.o (which would multi-def against vfs.c).  Once
 * VFS is linked, its strong symbols replace these. */

NH_WEAK int
chmod(const char *path, mode_t mode)
{
    (void) path;
    (void) mode;
    return 0;
}

NH_WEAK char *
getcwd(char *buf, size_t size)
{
    if (buf && size > 0)
        buf[0] = '\0';
    return buf;
}

NH_WEAK DIR *
opendir(const char *name)
{
    (void) name;
    errno = ENOSYS;
    return NULL;
}

NH_WEAK int
closedir(DIR *d)
{
    (void) d;
    return 0;
}

NH_WEAK struct dirent *
readdir(DIR *d)
{
    (void) d;
    return NULL;
}

NH_WEAK void
rewinddir(DIR *d)
{
    (void) d;
}

NH_WEAK long
telldir(DIR *d)
{
    (void) d;
    return 0;
}

NH_WEAK void
seekdir(DIR *d, long loc)
{
    (void) d;
    (void) loc;
}

NH_WEAK int
mkdir(const char *path, mode_t mode)
{
    (void) path;
    (void) mode;
    errno = ENOSYS;
    return -1;
}

/*--------------------------------------------------------------------
 * Sleep -- POSIX sleep(3).  ESP-IDF has its own vTaskDelay; we route
 * sleep() to it if available, otherwise just busy-yield.  Defined as
 * weak so the ESP-IDF application can override with a FreeRTOS-aware
 * version if it wants finer granularity.
 *------------------------------------------------------------------*/

unsigned int __attribute__((weak))
sleep(unsigned int seconds)
{
    /* ESP-IDF supplies usleep() and vTaskDelay(); both live outside
     * libnh.a, so we keep this stub trivial -- the lockfile retry path
     * in src/files.c::lock_file() is the only consumer and it's fine
     * with this returning immediately. */
    (void) seconds;
    return 0;
}

/*--------------------------------------------------------------------
 * popen/pclose -- only referenced inside #ifdef CRASHREPORT, which we
 * disable, but the symbols are referenced unconditionally by report.c
 * declarations.  Stubs are insurance.
 *------------------------------------------------------------------*/

FILE *
popen(const char *command, const char *mode)
{
    (void) command;
    (void) mode;
    errno = ENOSYS;
    return (FILE *) NULL;
}

int
pclose(FILE *stream)
{
    (void) stream;
    errno = ENOSYS;
    return -1;
}

/*--------------------------------------------------------------------
 * NetHack engine helpers that lived in sys/share/unixtty.c (which we
 * cannot compile because it pulls in <termios.h>).
 *------------------------------------------------------------------*/

/* From sys/share/unixtty.c: error() prints a message and exits.  It is
 * declared in include/extern.h with ATTRNORETURN; the engine calls it
 * from a handful of fatal-error paths (sfstruct.c::mread, do.c, etc.).
 * Routing to abort() preserves NORETURN semantics and produces a
 * coredump entry the ESP-IDF panic handler can decode.  No newline is
 * appended; mirror the original. */
void
error(const char *s, ...)
{
    va_list ap;
    va_start(ap, s);
    (void) vfprintf(stderr, s, ap);
    (void) fputc('\n', stderr);
    va_end(ap);
    abort();
}

/* intron/introff toggle Ctrl-C handling on the controlling tty.  With a
 * keyboard scanner (TCA8418) and no Unix tty we have nothing to do; the
 * shim windowport handles input itself. */
void
intron(void)
{
}

void
introff(void)
{
}

/* dosuspend is the SUSPEND handler -- gated by #ifdef SUSPEND
 * (undefined for us) in sys/unix/unixunix.c, but the prototype is
 * referenced unconditionally from src/cmd.c::dosuspend_core. */
int
dosuspend(void)
{
    return 0;
}

/* If NetHack ever calls exit() at run time, ESP-IDF's newlib routes it to
 * `syscall_not_implemented_aborts` which kills the firmware -- annoying
 * but clear.  We previously stubbed a spin-loop here but newlib's _exit
 * has a strong definition that multi-def's against ours, and -DSYSCF was
 * the main exit() trigger we've already eliminated.  Leave newlib's
 * behaviour in place; if it fires the abort backtrace tells us where to
 * add an upstream guard. */
