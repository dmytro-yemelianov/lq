/*
 * ls — list directory contents
 *
 * Based on xv6 (MIT License), extended for QRV-OS.
 * Supports -l (long listing) and -a (show dotfiles).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <pwd.h>
#include <grp.h>

static int opt_long;    /* -l */
static int opt_all;     /* -a */

/*
 * format_mode() - produce "drwxr-xr-x" style string from st_mode
 *
 * @mode: stat mode bits
 * @buf:  output buffer (at least 11 bytes)
 */
static void format_mode(mode_t mode, char *buf)
{
    switch (mode & S_IFMT) {
    case S_IFDIR:  buf[0] = 'd'; break;
    case S_IFLNK:  buf[0] = 'l'; break;
    case S_IFCHR:  buf[0] = 'c'; break;
    case S_IFBLK:  buf[0] = 'b'; break;
    case S_IFIFO:  buf[0] = 'p'; break;
    case S_IFSOCK: buf[0] = 's'; break;
    default:       buf[0] = '-'; break;
    }
    buf[1] = (mode & S_IRUSR) ? 'r' : '-';
    buf[2] = (mode & S_IWUSR) ? 'w' : '-';
    buf[3] = (mode & S_IXUSR) ? 'x' : '-';
    buf[4] = (mode & S_IRGRP) ? 'r' : '-';
    buf[5] = (mode & S_IWGRP) ? 'w' : '-';
    buf[6] = (mode & S_IXGRP) ? 'x' : '-';
    buf[7] = (mode & S_IROTH) ? 'r' : '-';
    buf[8] = (mode & S_IWOTH) ? 'w' : '-';
    buf[9] = (mode & S_IXOTH) ? 'x' : '-';
    buf[10] = '\0';
}

/*
 * print_entry() - print one directory entry
 *
 * @name: filename to display
 * @path: full path (used for readlink on symlinks; may be NULL)
 * @st:   lstat result
 */
static void print_entry(const char *name, const char *path, struct stat *st)
{
    char target[PATH_MAX];
    int  tlen = -1;

    /* For symlinks, read the target so -l can show "name -> target". */
    if (S_ISLNK(st->st_mode) && path != NULL) {
        tlen = (int)readlink(path, target, sizeof(target) - 1);
        if (tlen >= 0)
            target[tlen] = '\0';
    }

    if (!opt_long) {
        if (tlen >= 0)
            printf("%s -> %s\n", name, target);
        else
            printf("%s\n", name);
        return;
    }

    char mode[12];
    format_mode(st->st_mode, mode);

    if (tlen >= 0)
        printf("%s %2d %5d %5d %8ld %s -> %s\n",
               mode, (int)st->st_nlink,
               (int)st->st_uid, (int)st->st_gid,
               (long)st->st_size, name, target);
    else
        printf("%s %2d %5d %5d %8ld %s\n",
               mode, (int)st->st_nlink,
               (int)st->st_uid, (int)st->st_gid,
               (long)st->st_size, name);
}

/*
 * ls() - list one path
 *
 * @path: file or directory to list
 */
static void ls(const char *path)
{
    DIR *dp;
    struct dirent *de;
    struct stat st;
    char buf[512];

    /* lstat — don't follow the link if `path` is itself a symlink,
     * so we can display it as "x -> y". */
    if (lstat(path, &st) < 0) {
        fprintf(stderr, "ls: cannot stat %s\n", path);
        return;
    }

    if (!S_ISDIR(st.st_mode)) {
        /* Single file — just print it */
        const char *name = strrchr(path, '/');
        name = name ? name + 1 : path;
        print_entry(name, path, &st);
        return;
    }

    if ((dp = opendir(path)) == NULL) {
        fprintf(stderr, "ls: cannot open %s\n", path);
        return;
    }

    while ((de = readdir(dp)) != NULL) {
        if (!opt_all && de->d_name[0] == '.')
            continue;
        snprintf(buf, sizeof buf, "%s/%s", path, de->d_name);
        if (lstat(buf, &st) < 0) {
            fprintf(stderr, "ls: cannot stat %s\n", buf);
            continue;
        }
        print_entry(de->d_name, buf, &st);
    }
    closedir(dp);
}

int main(int argc, char *argv[])
{
    int opt;
    int i;

    while ((opt = getopt(argc, argv, "la")) != -1) {
        switch (opt) {
        case 'l':
            opt_long = 1;
            break;
        case 'a':
            opt_all = 1;
            break;
        default:
            fprintf(stderr, "Usage: ls [-la] [path ...]\n");
            return 1;
        }
    }

    if (optind >= argc) {
        ls(".");
    } else {
        for (i = optind; i < argc; i++)
            ls(argv[i]);
    }
    return 0;
}
