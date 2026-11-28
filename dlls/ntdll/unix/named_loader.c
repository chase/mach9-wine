/*
 * Per-process named macOS loader bundles.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#if defined(__APPLE__) && defined(__aarch64__)
#include <copyfile.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <removefile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "named_loader.h"

#define DIR_FLAGS (O_DIRECTORY | O_RDONLY | O_NOFOLLOW | O_CLOEXEC)

static int join_path( char *out, const char *dir, const char *name )
{
    int n = snprintf( out, PATH_MAX, "%s/%s", dir, name );
    return n >= 0 && n < PATH_MAX;
}

static int valid_dependency( int fd, const char *expected )
{
    char target[PATH_MAX];
    struct stat st;
    size_t len = strlen(expected);
    return readlinkat( fd, "ntdll.so", target, sizeof(target) ) == (ssize_t)len &&
           !memcmp( target, expected, len ) && !fstatat( fd, "ntdll.so", &st, 0 ) && S_ISREG(st.st_mode);
}

/* macOS can display ':' in a POSIX filename as '/'. Keep the unmodified
 * display name on the lease directory, never inside the signed bundle. */
#define TITLE_ATTR "org.winehq.named-loader.title"

static int bundle_name( const char *title, char name[NAME_MAX + 1] )
{
    size_t i, n = 0;
    int nonblank = 0;

    for (i = 0; title[i]; ++i)
    {
        unsigned char c = title[i];
        if (i >= NAME_MAX || c == '/' || c < 32 || c == 127) return 0;
        if (c == ':') continue;
        if (n >= NAME_MAX - 4) return 0;
        if (c != ' ') nonblank = 1;
        name[n++] = c;
    }
    if (!nonblank || name[0] == '.') return 0;
    memcpy( name + n, ".app", 5 );
    return 1;
}

static int read_title( int fd, char title[NAME_MAX + 1] )
{
    ssize_t len = fgetxattr( fd, TITLE_ATTR, title, NAME_MAX, 0, 0 );
    if (len <= 0 || memchr( title, 0, len )) return 0;
    title[len] = 0;
    return 1;
}

/* Under the root lock: nobody can publish a new entry between our lease
 * check and removal. A killed writer and a crashed game release their locks
 * just like a normal exit. Never open a prior entry to launch another game. */
static void prune_entries( DIR *root, const char *root_path )
{
    struct dirent *ent;
    char path[PATH_MAX];
    int fd;

    /* Only reclaim entries whose owning process has closed its lease. */

    while ((ent = readdir( root )))
    {
        if (strncmp( ent->d_name, "run.", 4 ) || !join_path( path, root_path, ent->d_name )) continue;
        if ((fd = openat( dirfd( root ), ent->d_name, DIR_FLAGS | O_EXLOCK | O_NONBLOCK )) == -1) continue;
        removefile( path, NULL, REMOVEFILE_RECURSIVE );
        close( fd );
    }
}

/* Serialize allocating and pruning lease directories. Copy outside the lock
 * so simultaneous launches do not hold each other up while cloning. */
static int create_entry( const char *root_path, char *entry )
{
    struct stat st;
    DIR *root;
    int fd = -1, root_fd;

    if (mkdir( root_path, 0700 ) && errno != EEXIST) return -1;
    do root_fd = open( root_path, DIR_FLAGS | O_EXLOCK ); while (root_fd == -1 && errno == EINTR);
    if (root_fd == -1) return -1;
    if (fstat( root_fd, &st ) || st.st_uid != geteuid() || (st.st_mode & 077) || !(root = fdopendir( root_fd )))
    {
        close( root_fd );
        return -1;
    }
    prune_entries( root, root_path );
    if (mkdtemp( entry ))
    {
        fd = open( entry, DIR_FLAGS | O_EXLOCK );
        /* The grandchild can replace the standard descriptors before exec. */
        if (fd >= 0 && fd <= STDERR_FILENO)
        {
            int dup = fcntl( fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1 );
            close( fd );
            fd = dup;
        }
        if (fd == -1) rmdir( entry );
    }
    closedir( root );
    return fd;
}

char *prepare_named_loader( const char *ntdll_dir, const char *title, int *lease )
{
    char cache[PATH_MAX], root[PATH_MAX], entry[PATH_MAX], src[PATH_MAX], ntdll[PATH_MAX];
    char name[NAME_MAX + 1], bundle[PATH_MAX], link[PATH_MAX], exe[PATH_MAX];
    char *ret;
    size_t len;
    int fd;

    *lease = -1;
    if (!bundle_name( title, name ) ||
        !(len = confstr( _CS_DARWIN_USER_CACHE_DIR, cache, sizeof(cache) )) || len > sizeof(cache) ||
        !join_path( root, cache, "wine-named-loaders" ) || !join_path( entry, root, "run.XXXXXX" ) ||
        !join_path( src, ntdll_dir, "wine.app" ) || !join_path( ntdll, ntdll_dir, "ntdll.so" ) ||
        (fd = create_entry( root, entry )) == -1) return NULL;

    if (join_path( bundle, entry, name ) && join_path( link, entry, "ntdll.so" ) &&
        join_path( exe, bundle, "Contents/MacOS/wine" ) &&
        !fsetxattr( fd, TITLE_ATTR, title, strlen(title), 0, XATTR_CREATE ) && !symlink( ntdll, link ) &&
        !copyfile( src, bundle, NULL, COPYFILE_ALL | COPYFILE_RECURSIVE | COPYFILE_NOFOLLOW | COPYFILE_CLONE ) &&
        (ret = strdup( exe )))
    {
        *lease = fd;
        return ret;
    }
    removefile( entry, NULL, REMOVEFILE_RECURSIVE );
    close( fd );
    return NULL;
}

/* The loader must find the original ntdll.so next to its .app. Check that
 * dependency before exec: after exec, the default-loader fallback is too late.
 * Validate relative to the actual leased directory, not another user's path. */
int inherit_named_loader( int lease, const char *exe, const char *ntdll_dir )
{
    char expected[PATH_MAX], value[32];
    struct stat st;

    if (lease == -1 || !join_path( expected, ntdll_dir, "ntdll.so" ) ||
        !valid_dependency( lease, expected ) ||
        lstat( exe, &st ) || !S_ISREG( st.st_mode ) || access( exe, X_OK )) return 0;
    snprintf( value, sizeof(value), "%d", lease );
    if (fcntl( lease, F_SETFD, 0 ) == -1) return 0;
    if (!setenv( NAMED_LOADER_LEASE_ENV, value, 1 )) return 1;
    fcntl( lease, F_SETFD, FD_CLOEXEC );
    return 0;
}

/* Keep the inherited lease for this process, but not its children. Only
 * alter an fd if it names the directory around our actual running bundle. */
const char *adopt_named_loader(void)
{
    static const char suffix[] = "/Contents/MacOS/wine";
    static char title[NAME_MAX + 1];
    char exe[PATH_MAX], *end;
    const char *ret = NULL, *value = getenv( NAMED_LOADER_LEASE_ENV );
    uint32_t size = sizeof(exe);
    struct stat fd_st, path_st;
    long fd;
    size_t len;

    if (!value) return NULL;
    fd = strtol( value, &end, 10 );
    if (end != value && !*end && fd > STDERR_FILENO && fd <= INT_MAX && !_NSGetExecutablePath( exe, &size ) &&
        (len = strlen( exe )) >= sizeof(suffix) && !strcmp( exe + len - (sizeof(suffix) - 1), suffix ))
    {
        exe[len - (sizeof(suffix) - 1)] = 0;
        if ((end = strrchr( exe, '/' )))
        {
            *end = 0;
            if (!stat( exe, &path_st ) && !fstat( fd, &fd_st ) && S_ISDIR( fd_st.st_mode ) &&
                fd_st.st_ino == path_st.st_ino && fd_st.st_dev == path_st.st_dev &&
                !fcntl( fd, F_SETFD, FD_CLOEXEC ) && read_title( fd, title )) ret = title;
        }
    }
    unsetenv( NAMED_LOADER_LEASE_ENV );
    return ret;
}
#endif
