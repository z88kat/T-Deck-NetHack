/* NetHack 5.0  esp32sys.c -- ESP32-S3 POSIX shims for the libnh cross build.
 *
 * NetHack's UNIX code paths reference a handful of POSIX functions
 * (fork/execv/wait/setuid/setgid/popen/pclose) that ESP-IDF's newlib does
 * not implement.  Most call sites are guarded by config-time options we
 * disable (-DNOMAIL, -DNO_SIGNAL, -DNOCRASHREPORT, -DNOPANICTRACE), but a
 * few survive inside #ifdef COMPRESS in src/files.c.  Rather than carve
 * the engine to remove them, we provide failure-returning stubs so the
 * linker is happy.  If any of these ever fires at run time, the right
 * fix is upstream (add another build-time guard) -- not making the stub
 * do anything useful.
 *
 * Only built when CROSS_TO_ESP32S3 is set (see top-level Makefile).
 */

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

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

/* popen/pclose: insurance.  The -DNOPANICTRACE and -DNOCRASHREPORT flags
 * should compile out every call site, but we provide the symbols anyway
 * so a future regression in a guard doesn't break the link. */
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
