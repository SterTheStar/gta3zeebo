// syscalls.cpp - Newlib bare-metal / BREW 4.0.2 syscall & runtime glue for Zeebo.
#include "zeebo_brew.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>

static char s_cwd[260] = "";

static void NormalizePath(const char* inPath, char* outPath, size_t outSize)
{
    if (!outPath || outSize == 0) return;
    if (!inPath) {
        outPath[0] = '\0';
        return;
    }
    const char* p = inPath;
    while (p[0] == '.' && (p[1] == '/' || p[1] == '\\')) {
        p += 2;
    }
    while (*p == '/' || *p == '\\') {
        ++p;
    }
    size_t i = 0;
    for (; p[i] && i + 1 < outSize; ++i) {
        char c = p[i];
        outPath[i] = (c == '\\') ? '/' : c;
    }
    outPath[i] = '\0';
}

static void ResolveCwdPath(const char* inPath, char* outPath, size_t outSize)
{
    char norm[260];
    NormalizePath(inPath, norm, sizeof(norm));
    if (s_cwd[0] != '\0' && norm[0] != '\0') {
        size_t cwdLen = strlen(s_cwd);
        if (strncmp(norm, s_cwd, cwdLen) != 0 || (norm[cwdLen] != '/' && norm[cwdLen] != '\0')) {
            snprintf(outPath, outSize, "%s/%s", s_cwd, norm);
            return;
        }
    }
    strncpy(outPath, norm, outSize - 1);
    outPath[outSize - 1] = '\0';
}

extern "C" {

void* __dso_handle = 0;

static uint8_t s_heap[64 * 1024];
static size_t  s_heapOffset = 0;

void* malloc(size_t size)
{
    if (g_Zeebo.pHelpers && g_Zeebo.pHelpers->pfnMalloc) {
        return g_Zeebo.pHelpers->pfnMalloc((uint32_t)(size ? size : 1));
    }
    return NULL;
}

void free(void* ptr)
{
    if (ptr && g_Zeebo.pHelpers && g_Zeebo.pHelpers->pfnFree) {
        g_Zeebo.pHelpers->pfnFree(ptr);
    }
}

void* realloc(void* ptr, size_t size)
{
    if (g_Zeebo.pHelpers && g_Zeebo.pHelpers->pfnRealloc) {
        return g_Zeebo.pHelpers->pfnRealloc(ptr, (uint32_t)(size ? size : 1));
    }
    return NULL;
}

void* calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    void* p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void* _sbrk(ptrdiff_t incr)
{
    if (incr < 0) {
        size_t dec = (size_t)(-incr);
        if (dec > s_heapOffset) {
            errno = ENOMEM;
            return (void*)-1;
        }
        s_heapOffset -= dec;
        return s_heap + s_heapOffset;
    }

    size_t aligned = (s_heapOffset + 7u) & ~7u;
    if (aligned + (size_t)incr > sizeof(s_heap)) {
        errno = ENOMEM;
        return (void*)-1;
    }
    void* prev = s_heap + aligned;
    s_heapOffset = aligned + (size_t)incr;
    return prev;
}

#define MAX_FDS 64
static IFile*   s_fdTable[MAX_FDS];
static uint32_t s_fdPos[MAX_FDS];
static uint32_t s_fdSize[MAX_FDS];

int _open(const char* name, int flags, ...)
{
    if (!name || !g_Zeebo.pFileMgr || !g_Zeebo.pFileMgr->pvt) {
        errno = ENOENT;
        return -1;
    }

    char norm[260];
    ResolveCwdPath(name, norm, sizeof(norm));

    int acc = flags & O_ACCMODE;
    OpenFileMode mode = _OFM_READ;
    if (acc == O_WRONLY || acc == O_RDWR) {
        mode = _OFM_READWRITE;
        if (flags & O_TRUNC) {
            g_Zeebo.pFileMgr->pvt->Remove(g_Zeebo.pFileMgr, norm);
            if (s_cwd[0] != '\0') {
                char fallback[260];
                NormalizePath(name, fallback, sizeof(fallback));
                g_Zeebo.pFileMgr->pvt->Remove(g_Zeebo.pFileMgr, fallback);
            }
            mode = (OpenFileMode)(_OFM_READWRITE | _OFM_CREATE);
        } else if (flags & O_CREAT) {
            mode = (OpenFileMode)(_OFM_READWRITE | _OFM_CREATE);
        }
    }

    IFile* f = g_Zeebo.pFileMgr->pvt->OpenFile(g_Zeebo.pFileMgr, norm, mode);
    if (!f && s_cwd[0] != '\0') {
        char fallback[260];
        NormalizePath(name, fallback, sizeof(fallback));
        f = g_Zeebo.pFileMgr->pvt->OpenFile(g_Zeebo.pFileMgr, fallback, mode);
    }
    if (!f) {
        errno = ENOENT;
        return -1;
    }

    AEEFileInfo info;
    memset(&info, 0, sizeof(info));
    uint32_t fileSize = 0;
    if (f->pvt->GetInfo(f, &info) == AEE_SUCCESS) {
        fileSize = info.dwSize;
    }

    for (int fd = 3; fd < MAX_FDS; ++fd) {
        if (!s_fdTable[fd]) {
            s_fdTable[fd] = f;
            s_fdPos[fd]   = 0;
            s_fdSize[fd]  = fileSize;
            return fd;
        }
    }
    f->pvt->Release(f);
    errno = EMFILE;
    return -1;
}

int _close(int fd)
{
    if (fd >= 3 && fd < MAX_FDS && s_fdTable[fd]) {
        s_fdTable[fd]->pvt->Release(s_fdTable[fd]);
        s_fdTable[fd] = NULL;
        s_fdPos[fd]   = 0;
        s_fdSize[fd]  = 0;
        return 0;
    }
    errno = EBADF;
    return -1;
}

int _read(int fd, void* ptr, int len)
{
    if (fd >= 3 && fd < MAX_FDS && s_fdTable[fd] && ptr && len >= 0) {
        int32_t n = s_fdTable[fd]->pvt->Read(s_fdTable[fd], ptr, (uint32_t)len);
        if (n > 0) {
            s_fdPos[fd] += (uint32_t)n;
            return (int)n;
        }
        return 0;
    }
    errno = EBADF;
    return -1;
}

int _write(int fd, const void* ptr, int len)
{
    if (fd == 1 || fd == 2) {
        if (ptr && len > 0 && g_Zeebo.pHelpers && g_Zeebo.pHelpers->pfnDbgPrintf) {
            char buf[256];
            int n = (len < 255) ? len : 255;
            memcpy(buf, ptr, (size_t)n);
            buf[n] = '\0';
            g_Zeebo.pHelpers->pfnDbgPrintf("%s", buf);
        }
        return len;
    }
    if (fd >= 3 && fd < MAX_FDS && s_fdTable[fd] && ptr && len >= 0) {
        uint32_t n = s_fdTable[fd]->pvt->Write(s_fdTable[fd], ptr, (uint32_t)len);
        s_fdPos[fd] += n;
        if (s_fdPos[fd] > s_fdSize[fd]) s_fdSize[fd] = s_fdPos[fd];
        return (int)n;
    }
    errno = EBADF;
    return -1;
}

int _lseek(int fd, int ptr, int dir)
{
    if (fd >= 3 && fd < MAX_FDS && s_fdTable[fd]) {
        int32_t newPos = 0;
        FileSeekType st = _SEEK_START;
        if (dir == 0) {
            st = _SEEK_START;
            newPos = ptr;
        } else if (dir == 1) {
            st = _SEEK_CURRENT;
            newPos = (int32_t)s_fdPos[fd] + ptr;
        } else if (dir == 2) {
            st = _SEEK_END;
            newPos = (int32_t)s_fdSize[fd] + ptr;
        }
        if (newPos < 0) newPos = 0;
        s_fdTable[fd]->pvt->Seek(s_fdTable[fd], st, (int32_t)ptr);
        s_fdPos[fd] = (uint32_t)newPos;
        return (int)s_fdPos[fd];
    }
    errno = EBADF;
    return -1;
}

int _fstat(int fd, struct stat* st)
{
    if (!st) return -1;
    memset(st, 0, sizeof(*st));
    if (fd >= 3 && fd < MAX_FDS && s_fdTable[fd]) {
        AEEFileInfo info;
        if (s_fdTable[fd]->pvt->GetInfo(s_fdTable[fd], &info) == AEE_SUCCESS) {
            st->st_mode = S_IFREG;
            st->st_size = (off_t)info.dwSize;
            return 0;
        }
    }
    st->st_mode = S_IFCHR;
    return 0;
}

int _stat(const char* path, struct stat* st)
{
    if (!path || !st || !g_Zeebo.pFileMgr || !g_Zeebo.pFileMgr->pvt) {
        errno = ENOENT;
        return -1;
    }
    char norm[260];
    ResolveCwdPath(path, norm, sizeof(norm));
    AEEFileInfo info;
    memset(&info, 0, sizeof(info));
    if (g_Zeebo.pFileMgr->pvt->GetInfo(g_Zeebo.pFileMgr, norm, &info) != AEE_SUCCESS) {
        if (s_cwd[0] != '\0') {
            char fallback[260];
            NormalizePath(path, fallback, sizeof(fallback));
            if (g_Zeebo.pFileMgr->pvt->GetInfo(g_Zeebo.pFileMgr, fallback, &info) != AEE_SUCCESS) {
                errno = ENOENT;
                return -1;
            }
        } else {
            errno = ENOENT;
            return -1;
        }
    }
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG;
    st->st_size = (off_t)info.dwSize;
    return 0;
}

int _isatty(int fd) { return (fd >= 0 && fd <= 2) ? 1 : 0; }
int _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }
int _getpid(void) { return 1; }

int _gettimeofday(struct timeval* tv, void* tz)
{
    (void)tz;
    if (tv) {
        uint32_t ms = Zeebo_TimeMs();
        tv->tv_sec  = (long)(ms / 1000u);
        tv->tv_usec = (long)((ms % 1000u) * 1000u);
    }
    return 0;
}

int mkdir(const char* path, mode_t mode)
{
    (void)mode;
    if (!path || !g_Zeebo.pFileMgr || !g_Zeebo.pFileMgr->pvt) {
        errno = ENOENT;
        return -1;
    }
    char norm[260];
    ResolveCwdPath(path, norm, sizeof(norm));
    if (g_Zeebo.pFileMgr->pvt->MkDir(g_Zeebo.pFileMgr, norm) == AEE_SUCCESS) {
        return 0;
    }
    errno = EEXIST;
    return -1;
}

int rename(const char* oldpath, const char* newpath)
{
    if (!oldpath || !newpath || !g_Zeebo.pFileMgr || !g_Zeebo.pFileMgr->pvt) {
        errno = ENOENT;
        return -1;
    }
    char oldNorm[260], newNorm[260];
    ResolveCwdPath(oldpath, oldNorm, sizeof(oldNorm));
    ResolveCwdPath(newpath, newNorm, sizeof(newNorm));
    if (g_Zeebo.pFileMgr->pvt->Rename(g_Zeebo.pFileMgr, oldNorm, newNorm) == AEE_SUCCESS) {
        return 0;
    }
    errno = EACCES;
    return -1;
}

int _link(const char* oldpath, const char* newpath)
{
    (void)oldpath; (void)newpath;
    errno = EMLINK;
    return -1;
}

int _unlink(const char* path)
{
    if (!path || !g_Zeebo.pFileMgr || !g_Zeebo.pFileMgr->pvt) {
        errno = ENOENT;
        return -1;
    }
    char norm[260];
    ResolveCwdPath(path, norm, sizeof(norm));
    if (g_Zeebo.pFileMgr->pvt->Remove(g_Zeebo.pFileMgr, norm) == AEE_SUCCESS) {
        return 0;
    }
    if (s_cwd[0] != '\0') {
        char fallback[260];
        NormalizePath(path, fallback, sizeof(fallback));
        if (g_Zeebo.pFileMgr->pvt->Remove(g_Zeebo.pFileMgr, fallback) == AEE_SUCCESS) {
            return 0;
        }
    }
    errno = ENOENT;
    return -1;
}

int chdir(const char* path)
{
    if (!path) return -1;
    char norm[260];
    NormalizePath(path, norm, sizeof(norm));
    size_t len = strlen(norm);
    while (len > 0 && norm[len - 1] == '/') {
        norm[--len] = '\0';
    }
    if (strcmp(norm, ".") == 0 || len == 0) {
        s_cwd[0] = '\0';
        return 0;
    }
    strncpy(s_cwd, norm, sizeof(s_cwd) - 1);
    s_cwd[sizeof(s_cwd) - 1] = '\0';
    return 0;
}

char* getcwd(char* buf, size_t size)
{
    if (!buf || size == 0) return NULL;
    if (s_cwd[0] == '\0') {
        strncpy(buf, ".", size - 1);
    } else {
        strncpy(buf, s_cwd, size - 1);
    }
    buf[size - 1] = '\0';
    return buf;
}

int usleep(unsigned int usec)
{
    (void)usec;
    return 0;
}

void _fini(void) {}

void _exit(int status)
{
    Zeebo_Log("[SYS] _exit(%d)\n", status);
    if (g_Zeebo.pShell) {
        ISHELL_CloseApplet(g_Zeebo.pShell, TRUE);
    }
    while (1) {}
}

void abort(void)
{
    Zeebo_Log("[SYS] abort()\n");
    _exit(1);
}

} // extern "C"
