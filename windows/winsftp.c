/*
 * winsftp.c: the Windows-specific parts of PSFTP and PSCP.
 */

#include <winsock2.h> /* need to put this first, for winelib builds */
#include <assert.h>

#define NEED_DECLARATION_OF_SELECT

#include "putty.h"
#include "psftp.h"
#include "ssh.h"
#include "int64.h"
#include "winsecur.h"

char *get_ttymode(void *frontend, const char *mode) { return NULL; }

int get_userpass_input(prompts_t *p, const unsigned char *in, int inlen)
{
    int ret;
    ret = cmdline_get_passwd_input(p, in, inlen);
    if (ret == -1)
	ret = console_get_userpass_input(p, in, inlen);
    return ret;
}

void platform_get_x11_auth(struct X11Display *display, Conf *conf)
{
    /* Do nothing, therefore no auth. */
}
const int platform_uses_x11_unix_by_default = TRUE;

/* ----------------------------------------------------------------------
 * File access abstraction.
 */

/*
 * Set local current directory. Returns NULL on success, or else an
 * error message which must be freed after printing.
 */
char *psftp_lcd(char *dir)
{
    char *ret = NULL;

    if (!SetCurrentDirectory(dir)) {
	LPVOID message;
	int i;
	FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER |
		      FORMAT_MESSAGE_FROM_SYSTEM |
		      FORMAT_MESSAGE_IGNORE_INSERTS,
		      NULL, GetLastError(),
		      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		      (LPTSTR)&message, 0, NULL);
	i = strcspn((char *)message, "\n");
	ret = dupprintf("%.*s", i, (LPCTSTR)message);
	LocalFree(message);
    }

    return ret;
}

/*
 * Get local current directory. Returns a string which must be
 * freed.
 */
char *psftp_getcwd(void)
{
    char *ret = snewn(256, char);
    int len = GetCurrentDirectory(256, ret);
    if (len > 256)
	ret = sresize(ret, len, char);
    GetCurrentDirectory(len, ret);
    return ret;
}

#define TIME_POSIX_TO_WIN(t, ft) do { \
    ULARGE_INTEGER uli; \
    uli.QuadPart = ((ULONGLONG)(t) + 11644473600ull) * 10000000ull; \
    (ft).dwLowDateTime  = uli.LowPart; \
    (ft).dwHighDateTime = uli.HighPart; \
} while(0)
#define TIME_WIN_TO_POSIX(ft, t) do { \
    ULARGE_INTEGER uli; \
    uli.LowPart  = (ft).dwLowDateTime; \
    uli.HighPart = (ft).dwHighDateTime; \
    uli.QuadPart = uli.QuadPart / 10000000ull - 11644473600ull; \
    (t) = (unsigned long) uli.QuadPart; \
} while(0)

struct RFile {
    HANDLE h;
};

RFile *open_existing_file(const char *name, uint64 *size,
			  unsigned long *mtime, unsigned long *atime,
                          long *perms)
{
    HANDLE h;
    RFile *ret;

    /* utf-8 to utf-16 */
    char *nameUTF16 = NULL;
    int len = MultiByteToWideChar(CP_UTF8, 0, name, -1, NULL, 0);
    if(len > 0) {
        nameUTF16 = malloc(len * sizeof(wchar_t));
        MultiByteToWideChar(CP_UTF8, 0, name, -1, nameUTF16, len);
    }
    /* open file */
    h = CreateFileW(nameUTF16, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, 0);
    free(nameUTF16);
    if (h == INVALID_HANDLE_VALUE)
	return NULL;

    ret = snew(RFile);
    ret->h = h;

    if (size) {
        DWORD lo, hi;
        lo = GetFileSize(h, &hi);
        size->lo = lo;
        size->hi = hi;
    }

    if (mtime || atime) {
	FILETIME actime, wrtime;
	GetFileTime(h, NULL, &actime, &wrtime);
	if (atime)
	    TIME_WIN_TO_POSIX(actime, *atime);
	if (mtime)
	    TIME_WIN_TO_POSIX(wrtime, *mtime);
    }

    if (perms)
        *perms = -1;

    return ret;
}

int read_from_file(RFile *f, void *buffer, int length)
{
    int ret;
    DWORD read;
    ret = ReadFile(f->h, buffer, length, &read, NULL);
    if (!ret)
	return -1;		       /* error */
    else
	return read;
}

void close_rfile(RFile *f)
{
    CloseHandle(f->h);
    sfree(f);
}

struct WFile {
    HANDLE h;
};

WFile *open_new_file(const char *name, long perms)
{
    HANDLE h;
    WFile *ret;

    /* utf-8 to utf-16 */
    char *nameUTF16 = NULL;
    int len = MultiByteToWideChar(CP_UTF8, 0, name, -1, NULL, 0);
    if(len > 0) {
        nameUTF16 = malloc(len * sizeof(wchar_t));
        MultiByteToWideChar(CP_UTF8, 0, name, -1, nameUTF16, len);
    }
    /* open file */
    h = CreateFileW(nameUTF16, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    free(nameUTF16);
    if (h == INVALID_HANDLE_VALUE)
	return NULL;

    ret = snew(WFile);
    ret->h = h;

    return ret;
}

WFile *open_existing_wfile(const char *name, uint64 *size)
{
    HANDLE h;
    WFile *ret;

    /* utf-8 to utf-16 */
    char *nameUTF16 = NULL;
    int len = MultiByteToWideChar(CP_UTF8, 0, name, -1, NULL, 0);
    if(len > 0) {
        nameUTF16 = malloc(len * sizeof(wchar_t));
        MultiByteToWideChar(CP_UTF8, 0, name, -1, nameUTF16, len);
    }
    /* open file */
    h = CreateFileW(nameUTF16, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, 0);
    free(nameUTF16);
    if (h == INVALID_HANDLE_VALUE)
	return NULL;

    ret = snew(WFile);
    ret->h = h;

    if (size) {
        DWORD lo, hi;
        lo = GetFileSize(h, &hi);
        size->lo = lo;
        size->hi = hi;
    }

    return ret;
}

int write_to_file(WFile *f, void *buffer, int length)
{
    int ret;
    DWORD written;
    ret = WriteFile(f->h, buffer, length, &written, NULL);
    if (!ret)
	return -1;		       /* error */
    else
	return written;
}

void set_file_times(WFile *f, unsigned long mtime, unsigned long atime)
{
    FILETIME actime, wrtime;
    TIME_POSIX_TO_WIN(atime, actime);
    TIME_POSIX_TO_WIN(mtime, wrtime);
    SetFileTime(f->h, NULL, &actime, &wrtime);
}

void close_wfile(WFile *f)
{
    CloseHandle(f->h);
    sfree(f);
}

/* Seek offset bytes through file, from whence, where whence is
   FROM_START, FROM_CURRENT, or FROM_END */
int seek_file(WFile *f, uint64 offset, int whence)
{
    DWORD movemethod;

    switch (whence) {
    case FROM_START:
	movemethod = FILE_BEGIN;
	break;
    case FROM_CURRENT:
	movemethod = FILE_CURRENT;
	break;
    case FROM_END:
	movemethod = FILE_END;
	break;
    default:
	return -1;
    }

    {
        LONG lo = offset.lo, hi = offset.hi;
        SetFilePointer(f->h, lo, &hi, movemethod);
    }
    
    if (GetLastError() != NO_ERROR)
	return -1;
    else 
	return 0;
}

uint64 get_file_posn(WFile *f)
{
    uint64 ret;
    LONG lo, hi = 0;

    lo = SetFilePointer(f->h, 0L, &hi, FILE_CURRENT);
    ret.lo = lo;
    ret.hi = hi;

    return ret;
}

int file_type(const char *name)
{
    DWORD attr;
    /* utf-8 to utf-16 */
    char *nameUTF16 = NULL;
    int len = MultiByteToWideChar(CP_UTF8, 0, name, -1, NULL, 0);
    if(len > 0) {
        nameUTF16 = malloc(len * sizeof(wchar_t));
        MultiByteToWideChar(CP_UTF8, 0, name, -1, nameUTF16, len);
    }
    /* unicode version instead of ANSI version */
    attr = GetFileAttributesW(nameUTF16);
    free(nameUTF16);

    /* We know of no `weird' files under Windows. */
    if (attr == (DWORD)-1)
	return FILE_TYPE_NONEXISTENT;
    else if (attr & FILE_ATTRIBUTE_DIRECTORY)
	return FILE_TYPE_DIRECTORY;
    else
	return FILE_TYPE_FILE;
}

struct DirHandle {
    HANDLE h;
    char *name;
};

DirHandle *open_directory(const char *name)
{
    HANDLE h;
    WIN32_FIND_DATAW fdat;
    char *findfile;
    DirHandle *ret;

    /* Enumerate files in dir `foo'. */
    findfile = dupcat(name, "/*", NULL);
    /* utf-8 to utf-16 */
    char *findfileUTF16 = NULL;
    int len = MultiByteToWideChar(CP_UTF8, 0, findfile, -1, NULL, 0);
    if(len > 0) {
        findfileUTF16 = malloc(len * sizeof(wchar_t));
        MultiByteToWideChar(CP_UTF8, 0, findfile, -1, findfileUTF16, len);
    }
    /* unicode version instead of ANSI version */
    h = FindFirstFileW(findfileUTF16, &fdat);
    free(findfileUTF16);
    if (h == INVALID_HANDLE_VALUE)
	return NULL;
    sfree(findfile);

    ret = snew(DirHandle);
    ret->h = h;
    ret->name = dupstr(fdat.cFileName);
    return ret;
}

char *read_filename(DirHandle *dir)
{
    do {

	if (!dir->name) {
	    WIN32_FIND_DATAW fdat;
	    int ok = FindNextFileW(dir->h, &fdat);
	    if (!ok) {
	        return NULL;
	    }
      else {
          /* utf-16 to uft-8 */
          char *nameUTF8 = NULL;
          int len = WideCharToMultiByte(CP_UTF8, 0, fdat.cFileName, -1, NULL, 0, NULL, NULL);
          if(len > 0) {
              nameUTF8 = malloc(len);
              WideCharToMultiByte(CP_UTF8, 0, fdat.cFileName, -1, nameUTF8, len, NULL, NULL);
          }
          dir->name = dupstr(nameUTF8);
          free(nameUTF8);
      }
	}

	assert(dir->name);
	if (dir->name[0] == '.' &&
	    (dir->name[1] == '\0' ||
	     (dir->name[1] == '.' && dir->name[2] == '\0'))) {
	    sfree(dir->name);
	    dir->name = NULL;
	}

    } while (!dir->name);

    if (dir->name) {
	char *ret = dir->name;
	dir->name = NULL;
	return ret;
    } else
	return NULL;
}

void close_directory(DirHandle *dir)
{
    FindClose(dir->h);
    if (dir->name)
	sfree(dir->name);
    sfree(dir);
}

int test_wildcard(const char *name, int cmdline)
{
    HANDLE fh;
    WIN32_FIND_DATA fdat;

    /* First see if the exact name exists. */
    if (GetFileAttributes(name) != (DWORD)-1)
	return WCTYPE_FILENAME;

    /* Otherwise see if a wildcard match finds anything. */
    fh = FindFirstFile(name, &fdat);
    if (fh == INVALID_HANDLE_VALUE)
	return WCTYPE_NONEXISTENT;

    FindClose(fh);
    return WCTYPE_WILDCARD;
}

struct WildcardMatcher {
    HANDLE h;
    char *name;
    char *srcpath;
};

char *stripslashes(const char *str, int local)
{
    char *p;

    /*
     * On Windows, \ / : are all path component separators.
     */

    if (local) {
        p = strchr(str, ':');
        if (p) str = p+1;
    }

    p = strrchr(str, '/');
    if (p) str = p+1;

    if (local) {
	p = strrchr(str, '\\');
	if (p) str = p+1;
    }

    return (char *)str;
}

WildcardMatcher *begin_wildcard_matching(const char *name)
{
    HANDLE h;
    WIN32_FIND_DATAW fdat;
    WildcardMatcher *ret;
    char *last;

    /* utf-8 to utf-16 */
    char *nameUTF16 = NULL;
    int len = MultiByteToWideChar(CP_UTF8, 0, name, -1, NULL, 0);
    if(len > 0) {
        nameUTF16 = malloc(len * sizeof(wchar_t));
        MultiByteToWideChar(CP_UTF8, 0, name, -1, nameUTF16, len);
    }
    /* unicode version instead of ANSI version */
    h = FindFirstFileW(nameUTF16, &fdat);
    free(nameUTF16);
    if (h == INVALID_HANDLE_VALUE)
	return NULL;

    ret = snew(WildcardMatcher);
    ret->h = h;
    ret->srcpath = dupstr(name);
    last = stripslashes(ret->srcpath, 1);
    *last = '\0';
    /* utf-16 to utf-8 */
    char *nameUTF8 = NULL;
    len = WideCharToMultiByte(CP_UTF8, 0, fdat.cFileName, -1, NULL, 0, NULL, NULL);
    if(len > 0) {
        nameUTF8 = malloc(len);
        WideCharToMultiByte(CP_UTF8, 0, fdat.cFileName, -1, nameUTF8, len, NULL, NULL);
    }
    if (nameUTF8[0] == '.' && (nameUTF8[1] == '\0' || (nameUTF8[1] == '.' && nameUTF8[2] == '\0'))) {
        ret->name = NULL;
    }
    else {
        ret->name = dupcat(ret->srcpath, nameUTF8, NULL);
    }
    free(nameUTF8);
    return ret;
}

char *wildcard_get_filename(WildcardMatcher *dir)
{
    while (!dir->name) {
    WIN32_FIND_DATAW fdat;
    int ok = FindNextFileW(dir->h, &fdat);
    if(!ok) {
        return NULL;
    }
    /* utf-16 to uft-8 */
    char *nameUTF8 = NULL;
    int len = WideCharToMultiByte(CP_UTF8, 0, fdat.cFileName, -1, NULL, 0, NULL, NULL);
    if(len > 0) {
        nameUTF8 = malloc(len);
        WideCharToMultiByte(CP_UTF8, 0, fdat.cFileName, -1, nameUTF8, len, NULL, NULL);
    }
    if(nameUTF8[0] == '.' && (nameUTF8[1] == '\0' || (nameUTF8[1] == '.' && nameUTF8[2] == '\0'))) {
        dir->name = NULL;
    }
    else {
        dir->name = dupcat(dir->srcpath, nameUTF8, NULL);
    }
    free(nameUTF8);
    }

    if (dir->name) {
	char *ret = dir->name;
	dir->name = NULL;
	return ret;
    } else
	return NULL;
}

void finish_wildcard_matching(WildcardMatcher *dir)
{
    FindClose(dir->h);
    if (dir->name)
	sfree(dir->name);
    sfree(dir->srcpath);
    sfree(dir);
}

int vet_filename(const char *name)
{
    if (strchr(name, '/') || strchr(name, '\\') || strchr(name, ':'))
	return FALSE;

    if (!name[strspn(name, ".")])      /* entirely composed of dots */
	return FALSE;

    return TRUE;
}

int create_directory(const char *name)
{
    /* utf-8 to utf-16 */
    char *nameUTF16 = NULL;
    int len = MultiByteToWideChar(CP_UTF8, 0, name, -1, NULL, 0);
    if(len > 0) {
        nameUTF16 = malloc(len * sizeof(wchar_t));
        MultiByteToWideChar(CP_UTF8, 0, name, -1, nameUTF16, len);
    }
    BOOL result = CreateDirectoryW(nameUTF16, NULL);
    free(nameUTF16);
    return result != 0;
}

char *dir_file_cat(const char *dir, const char *file)
{
    return dupcat(dir, "\\", file, NULL);
}

/* ----------------------------------------------------------------------
 * Platform-specific network handling.
 */

/*
 * Be told what socket we're supposed to be using.
 */
static SOCKET sftp_ssh_socket = INVALID_SOCKET;
static HANDLE netevent = INVALID_HANDLE_VALUE;
char *do_select(SOCKET skt, int startup)
{
    int events;
    if (startup)
	sftp_ssh_socket = skt;
    else
	sftp_ssh_socket = INVALID_SOCKET;

    if (p_WSAEventSelect) {
	if (startup) {
	    events = (FD_CONNECT | FD_READ | FD_WRITE |
		      FD_OOB | FD_CLOSE | FD_ACCEPT);
	    netevent = CreateEvent(NULL, FALSE, FALSE, NULL);
	} else {
	    events = 0;
	}
	if (p_WSAEventSelect(skt, netevent, events) == SOCKET_ERROR) {
	    switch (p_WSAGetLastError()) {
	      case WSAENETDOWN:
		return "Network is down";
	      default:
		return "WSAEventSelect(): unknown error";
	    }
	}
    }
    return NULL;
}

int do_eventsel_loop(HANDLE other_event)
{
    int n, nhandles, nallhandles, netindex, otherindex;
    unsigned long next, then;
    long ticks;
    HANDLE *handles;
    SOCKET *sklist;
    int skcount;
    unsigned long now = GETTICKCOUNT();

    if (toplevel_callback_pending()) {
        ticks = 0;
        next = now;
    } else if (run_timers(now, &next)) {
	then = now;
	now = GETTICKCOUNT();
	if (now - then > next - then)
	    ticks = 0;
	else
	    ticks = next - now;
    } else {
	ticks = INFINITE;
        /* no need to initialise next here because we can never get
         * WAIT_TIMEOUT */
    }

    handles = handle_get_events(&nhandles);
    handles = sresize(handles, nhandles+2, HANDLE);
    nallhandles = nhandles;

    if (netevent != INVALID_HANDLE_VALUE)
	handles[netindex = nallhandles++] = netevent;
    else
	netindex = -1;
    if (other_event != INVALID_HANDLE_VALUE)
	handles[otherindex = nallhandles++] = other_event;
    else
	otherindex = -1;

    n = WaitForMultipleObjects(nallhandles, handles, FALSE, ticks);

    if ((unsigned)(n - WAIT_OBJECT_0) < (unsigned)nhandles) {
	handle_got_event(handles[n - WAIT_OBJECT_0]);
    } else if (netindex >= 0 && n == WAIT_OBJECT_0 + netindex) {
	WSANETWORKEVENTS things;
	SOCKET socket;
	extern SOCKET first_socket(int *), next_socket(int *);
	int i, socketstate;

	/*
	 * We must not call select_result() for any socket
	 * until we have finished enumerating within the
	 * tree. This is because select_result() may close
	 * the socket and modify the tree.
	 */
	/* Count the active sockets. */
	i = 0;
	for (socket = first_socket(&socketstate);
	     socket != INVALID_SOCKET;
	     socket = next_socket(&socketstate)) i++;

	/* Expand the buffer if necessary. */
	sklist = snewn(i, SOCKET);

	/* Retrieve the sockets into sklist. */
	skcount = 0;
	for (socket = first_socket(&socketstate);
	     socket != INVALID_SOCKET;
	     socket = next_socket(&socketstate)) {
	    sklist[skcount++] = socket;
	}

	/* Now we're done enumerating; go through the list. */
	for (i = 0; i < skcount; i++) {
	    WPARAM wp;
	    socket = sklist[i];
	    wp = (WPARAM) socket;
	    if (!p_WSAEnumNetworkEvents(socket, NULL, &things)) {
		static const struct { int bit, mask; } eventtypes[] = {
		    {FD_CONNECT_BIT, FD_CONNECT},
		    {FD_READ_BIT, FD_READ},
		    {FD_CLOSE_BIT, FD_CLOSE},
		    {FD_OOB_BIT, FD_OOB},
		    {FD_WRITE_BIT, FD_WRITE},
		    {FD_ACCEPT_BIT, FD_ACCEPT},
		};
		int e;

		noise_ultralight(socket);
		noise_ultralight(things.lNetworkEvents);

		for (e = 0; e < lenof(eventtypes); e++)
		    if (things.lNetworkEvents & eventtypes[e].mask) {
			LPARAM lp;
			int err = things.iErrorCode[eventtypes[e].bit];
			lp = WSAMAKESELECTREPLY(eventtypes[e].mask, err);
			select_result(wp, lp);
		    }
	    }
	}

	sfree(sklist);
    }

    sfree(handles);

    run_toplevel_callbacks();

    if (n == WAIT_TIMEOUT) {
	now = next;
    } else {
	now = GETTICKCOUNT();
    }

    if (otherindex >= 0 && n == WAIT_OBJECT_0 + otherindex)
	return 1;

    return 0;
}

/*
 * Wait for some network data and process it.
 *
 * We have two variants of this function. One uses select() so that
 * it's compatible with WinSock 1. The other uses WSAEventSelect
 * and MsgWaitForMultipleObjects, so that we can consistently use
 * WSAEventSelect throughout; this enables us to also implement
 * ssh_sftp_get_cmdline() using a parallel mechanism.
 */
int ssh_sftp_loop_iteration(void)
{
    if (p_WSAEventSelect == NULL) {
	fd_set readfds;
	int ret;
	unsigned long now = GETTICKCOUNT(), then;

	if (sftp_ssh_socket == INVALID_SOCKET)
	    return -1;		       /* doom */

	if (socket_writable(sftp_ssh_socket))
	    select_result((WPARAM) sftp_ssh_socket, (LPARAM) FD_WRITE);

	do {
	    unsigned long next;
	    long ticks;
	    struct timeval tv, *ptv;

	    if (run_timers(now, &next)) {
		then = now;
		now = GETTICKCOUNT();
		if (now - then > next - then)
		    ticks = 0;
		else
		    ticks = next - now;
		tv.tv_sec = ticks / 1000;
		tv.tv_usec = ticks % 1000 * 1000;
		ptv = &tv;
	    } else {
		ptv = NULL;
	    }

	    FD_ZERO(&readfds);
	    FD_SET(sftp_ssh_socket, &readfds);
	    ret = p_select(1, &readfds, NULL, NULL, ptv);

	    if (ret < 0)
		return -1;		       /* doom */
	    else if (ret == 0)
		now = next;
	    else
		now = GETTICKCOUNT();

	} while (ret == 0);

	select_result((WPARAM) sftp_ssh_socket, (LPARAM) FD_READ);

	return 0;
    } else {
	return do_eventsel_loop(INVALID_HANDLE_VALUE);
    }
}

/*
 * Read a command line from standard input.
 * 
 * In the presence of WinSock 2, we can use WSAEventSelect to
 * mediate between the socket and stdin, meaning we can send
 * keepalives and respond to server events even while waiting at
 * the PSFTP command prompt. Without WS2, we fall back to a simple
 * fgets.
 */
struct command_read_ctx {
    HANDLE event;
    char *line;
};

static DWORD WINAPI command_read_thread(void *param)
{
    struct command_read_ctx *ctx = (struct command_read_ctx *) param;

    ctx->line = fgetline(stdin);

    SetEvent(ctx->event);

    return 0;
}

char *ssh_sftp_get_cmdline(const char *prompt, int no_fds_ok)
{
    int ret;
    struct command_read_ctx actx, *ctx = &actx;
    DWORD threadid;
    HANDLE hThread;

    fputs(prompt, stdout);
    fflush(stdout);

    if ((sftp_ssh_socket == INVALID_SOCKET && no_fds_ok) ||
	p_WSAEventSelect == NULL) {
	return fgetline(stdin);	       /* very simple */
    }

    /*
     * Create a second thread to read from stdin. Process network
     * and timing events until it terminates.
     */
    ctx->event = CreateEvent(NULL, FALSE, FALSE, NULL);
    ctx->line = NULL;

    hThread = CreateThread(NULL, 0, command_read_thread, ctx, 0, &threadid);
    if (!hThread) {
	CloseHandle(ctx->event);
	fprintf(stderr, "Unable to create command input thread\n");
	cleanup_exit(1);
    }

    do {
	ret = do_eventsel_loop(ctx->event);

	/* Error return can only occur if netevent==NULL, and it ain't. */
	assert(ret >= 0);
    } while (ret == 0);

    CloseHandle(hThread);
    CloseHandle(ctx->event);

    return ctx->line;
}

void platform_psftp_pre_conn_setup(void)
{
    if (restricted_acl) {
	logevent(NULL, "Running with restricted process ACL");
    }
}

/* ----------------------------------------------------------------------
 * Main program. Parse arguments etc.
 */
int main(int argc, char *argv[])
{
    int ret;

    dll_hijacking_protection();

    ret = psftp_main(argc, argv);

    return ret;
}
