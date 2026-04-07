#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/_iovec.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <errno.h>
#include <limits.h>
#include <sqlite3.h>
#include <ps5/kernel.h>
#include <sys/mdioctl.h>
#include <time.h>
#include <stdbool.h>
#include <sys/ioctl.h>

#define IOVEC_ENTRY(x) { (void*)(x), (x) ? strlen(x) + 1 : 0 }
#define IOVEC_SIZE(x)  (sizeof(x) / sizeof(struct iovec))
#define MD_UNIT_MAX 256
#define IMAGE_FFPKG 1
#define IMAGE_UFS   2
#define IMAGE_EXFAT 3

typedef struct notify_request {
    char unused[45];
    char message[3075];
} notify_request_t;

extern "C" {
    int sceAppInstUtilInitialize(void);
    int sceAppInstUtilAppInstallTitleDir(const char* title_id, const char* install_path, void* reserved);
    int sceKernelSendNotificationRequest(int, notify_request_t*, size_t, int);
}

// NOTIFY
static void notify(const char* fmt, ...) {
    notify_request_t req = {};
    va_list args;
    va_start(args, fmt);
    vsnprintf(req.message, sizeof(req.message) - 1, fmt, args);
    va_end(args);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
    printf("%s\n", req.message);
}

// MOUNT HELPERS
static int remount_system_ex(void) {
    struct iovec iov[] = {
        IOVEC_ENTRY("from"),      IOVEC_ENTRY("/dev/ssd0.system_ex"),
        IOVEC_ENTRY("fspath"),    IOVEC_ENTRY("/system_ex"),
        IOVEC_ENTRY("fstype"),    IOVEC_ENTRY("exfatfs"),
        IOVEC_ENTRY("large"),     IOVEC_ENTRY("yes"),
        IOVEC_ENTRY("timezone"),  IOVEC_ENTRY("static"),
        IOVEC_ENTRY("async"),     IOVEC_ENTRY(NULL),
        IOVEC_ENTRY("ignoreacl"), IOVEC_ENTRY(NULL),
    };
    return nmount(iov, IOVEC_SIZE(iov), MNT_UPDATE);
}

static int mount_nullfs(const char* src, const char* dst) {
    struct iovec iov[] = {
        IOVEC_ENTRY("fstype"), IOVEC_ENTRY("nullfs"),
        IOVEC_ENTRY("from"),   IOVEC_ENTRY(src),
        IOVEC_ENTRY("fspath"), IOVEC_ENTRY(dst),
    };
    return nmount(iov, IOVEC_SIZE(iov), 0);
}

// COPY HELPERS
static int copy_file(const char* src, const char* dst) {
    FILE* fs = fopen(src, "rb"); if (!fs) return -1;
    FILE* fd = fopen(dst, "wb"); if (!fd) { fclose(fs); return -1; }
    char buf[8192]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fs)) > 0) fwrite(buf, 1, n, fd);
    fclose(fs); fclose(fd);
    return 0;
}

static int copy_dir(const char* src, const char* dst) {
    if (mkdir(dst, 0755) && errno != EEXIST) return -1;
    DIR* d = opendir(src); if (!d) return -1;
    struct dirent* e; char ss[PATH_MAX], dd[PATH_MAX]; struct stat st;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(ss, sizeof(ss), "%s/%s", src, e->d_name);
        snprintf(dd, sizeof(dd), "%s/%s", dst, e->d_name);
        if (stat(ss, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) copy_dir(ss, dd);
        else copy_file(ss, dd);
    }
    closedir(d);
    return 0;
}

static int is_appmeta_file(const char* name) {
    if (!strcasecmp(name, "param.json") || !strcasecmp(name, "param.sfo")) return 1;
    const char* ext = strrchr(name, '.');
    if (!ext) return 0;
    return !strcasecmp(ext, ".png") || !strcasecmp(ext, ".dds") || !strcasecmp(ext, ".at9");
}

static int copy_sce_sys_to_appmeta(const char* src, const char* title_id) {
    char dst[PATH_MAX];
    snprintf(dst, sizeof(dst), "/user/appmeta/%s", title_id);
    mkdir("/user/appmeta", 0777);
    mkdir(dst, 0755);
    DIR* d = opendir(src); if (!d) return -1;
    struct dirent* e; char ss[PATH_MAX], dd[PATH_MAX]; struct stat st;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (!is_appmeta_file(e->d_name)) continue;
        snprintf(ss, sizeof(ss), "%s/%s", src, e->d_name);
        snprintf(dd, sizeof(dd), "%s/%s", dst, e->d_name);
        if (stat(ss, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        copy_file(ss, dd);
    }
    closedir(d);
    return 0;
}

// UPDATE SND0
static int update_snd0info(const char* title_id) {
    sqlite3* db = NULL;
    sqlite3_stmt* stmt = NULL;
    int ret = -1;
    char db_path[] = "/system_data/priv/mms/app.db";
    const char* sql = "UPDATE tbl_contentinfo SET snd0info = '/user/appmeta/' || ?1 || '/snd0.at9' WHERE titleId = ?1;";
    if (sqlite3_open(db_path, &db) != SQLITE_OK) goto out;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) goto out;
    sqlite3_bind_text(stmt, 1, title_id, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) != SQLITE_DONE) {}
    ret = sqlite3_changes(db);
out:
    if (stmt) sqlite3_finalize(stmt);
    if (db) sqlite3_close(db);
    return ret;
}

// JSON & SFO
static int extract_json_string(const char* json, const char* key, char* out, size_t out_size) {
    char search[64]; snprintf(search, sizeof(search), "\"%s\"", key);
    const char* p = strstr(json, search); if (!p) return -1;
    p = strchr(p + strlen(search), ':'); if (!p) return -1;
    while (*++p && isspace(*p));
    if (*p != '"') return -1; p++;
    size_t i=0;
    while (i < out_size-1 && p[i] && p[i] != '"') { out[i] = p[i]; i++; }
    out[i] = '\0';
    return 0;
}

typedef struct {
    uint16_t key_offset;
    uint16_t type;
    uint32_t size;
    uint32_t max_size;
    uint32_t data_offset;
} sfo_entry_t;

static int read_title_id_from_sfo(const char* path, char* title_id, size_t size) {
    FILE* f = fopen(path, "rb"); if (!f) return -1;
    uint32_t magic, version, key_off, data_off, count;
    if (fread(&magic,4,1,f)!=1 || fread(&version,4,1,f)!=1 ||
        fread(&key_off,4,1,f)!=1 || fread(&data_off,4,1,f)!=1 ||
        fread(&count,4,1,f)!=1) { fclose(f); return -1; }
    if (magic != 0x46535000) { fclose(f); return -1; }
    for (uint32_t i=0; i<count; i++) {
        sfo_entry_t entry;
        if (fseek(f, 0x14 + i*sizeof(sfo_entry_t), SEEK_SET)!=0) continue;
        if (fread(&entry, sizeof(sfo_entry_t),1,f)!=1) continue;
        char key[128]={0};
        if (fseek(f, key_off + entry.key_offset, SEEK_SET)!=0) continue;
        if (fread(key,1,sizeof(key)-1,f)<=0) continue;
        for (int k=0; k<128; k++) if (!isprint(key[k])) { key[k]='\0'; break; }
        if (strncmp(key,"TITLE_ID",8)==0) {
            if (fseek(f, data_off + entry.data_offset, SEEK_SET)!=0) continue;
            size_t rlen = (entry.size < size-1)?entry.size:size-1;
            if (fread(title_id,1,rlen,f)<=0) continue;
            title_id[rlen]='\0';
            fclose(f); return 0;
        }
    }
    fclose(f);
    return -1;
}

static int get_title_id(const char* base_path, char* title_id, size_t size) {
    char path[PATH_MAX];

    snprintf(path, sizeof(path), "%s/sce_sys/param.json", base_path);
    FILE* f = fopen(path, "rb");
    if (f) {
        fseek(f,0,SEEK_END); long len=ftell(f); fseek(f,0,SEEK_SET);
        if (len>0 && len<1024*1024) {
            char* buf = (char*)malloc(len+1);
            if (buf) {
                fread(buf,1,len,f); buf[len]='\0';
                if (extract_json_string(buf,"titleId",title_id,size)==0 ||
                    extract_json_string(buf,"title_id",title_id,size)==0) {
                    free(buf); fclose(f); return 0;
                }
                free(buf);
            }
        }
        fclose(f);
    }

    snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", base_path);
    return read_title_id_from_sfo(path, title_id, size);
}

// ---------------- PATCH DRM (PS5 only) ----------------
static int fix_application_drm_type(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 0 || len > 1024 * 1024) {
        fclose(f);
        return -1;
    }

    char* buf = (char*)malloc(len + 1);
    if (!buf) { fclose(f); return -1; }

    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);

    const char* key = "\"applicationDrmType\"";
    char* p = strstr(buf, key);
    if (!p) { free(buf); return 0; }

    char* colon = strchr(p + strlen(key), ':');
    char* q1 = colon ? strchr(colon, '"') : NULL;
    char* q2 = q1 ? strchr(q1 + 1, '"') : NULL;
    if (!q1 || !q2) { free(buf); return -1; }

    if ((q2 - q1 - 1) == strlen("standard") &&
        !strncmp(q1 + 1, "standard", strlen("standard"))) {
        free(buf);
        return 0;
    }

    size_t new_len = (q1 - buf) + 1 + strlen("standard") + 1 + strlen(q2 + 1);
    char* out = (char*)malloc(new_len + 1);
    if (!out) { free(buf); return -1; }

    memcpy(out, buf, q1 - buf + 1);
    memcpy(out + (q1 - buf + 1), "standard", strlen("standard"));
    strcpy(out + (q1 - buf + 1 + strlen("standard")), q2);

    f = fopen(path, "wb");
    if (!f) { free(buf); free(out); return -1; }

    fwrite(out, 1, strlen(out), f);
    fclose(f);

    free(buf);
    free(out);
    return 1;
}

// IMAGE TYPE DETECTION
#define UFS2_MAGIC          0x19540119
#define SBLOCK_UFS2_OFFSET  65536      // 64 KB
// Offsets within the superblock
#define OFFSET_UFS_FSIZE        52         // fs_fsize (int32_t)
#define OFFSET_UFS_FSBTODB      100        // fs_fsbtodb (int32_t)
#define OFFSET_UFS_MAGIC        1372       // fs_magic (int32_t)

static int32_t detect_is_ufs(const char* file_path) {
    FILE* fp = fopen(file_path, "rb");
    if (!fp) return 0;
    int32_t fsize = 0, fsbtodb = 0, magic = 0;

    // Verify Magic Number
    if (fseek(fp, SBLOCK_UFS2_OFFSET + OFFSET_UFS_MAGIC, SEEK_SET) == 0)
        fread(&magic, 4, 1, fp);
    if (magic != UFS2_MAGIC) {
        printf("Not a valid UFS image (Magic mismatch).\n");
        fclose(fp);
        return 0;
    }

    // Read fragment size and shift count
    if (fseek(fp, SBLOCK_UFS2_OFFSET + OFFSET_UFS_FSIZE, SEEK_SET) == 0)
        fread(&fsize, 4, 1, fp);

    if (fseek(fp, SBLOCK_UFS2_OFFSET + OFFSET_UFS_FSBTODB, SEEK_SET) == 0)
        fread(&fsbtodb, 4, 1, fp);

    // Calculate Sector Size
    // sector_size = fsize / (2^fsbtodb)
    int32_t sector_size = fsize < 512 ? 0 : (fsize >> fsbtodb);

    fclose(fp);
    return sector_size;
}

#define OFFSET_EXFAT_MAGIC 3
#define OFFSET_EXFAT_SHIFT 108
static int32_t detect_is_exfat(const char *file_path) {
    FILE *fp = fopen(file_path, "rb");
    if (!fp) return 0;

    char magic[9] = {0};
    uint8_t shift = 0;

    // Verify Magic Number ("EXFAT   " at offset 3)
    if (fseek(fp, OFFSET_EXFAT_MAGIC, SEEK_SET) == 0)
        fread(magic, 1, 8, fp);
    if (strncmp(magic, "EXFAT   ", 8) != 0) {
        printf("Not a valid EXFAT image (Magic mismatch).\n");
        fclose(fp);
        return 0;
    }

    // Read BytesPerSectorShift at offset 108
    if (fseek(fp, OFFSET_EXFAT_SHIFT, SEEK_SET) == 0)
        fread(&shift, 1, 1, fp);

    // Calculation: 2^shift
    // Valid values for exFAT are 9 (512 bytes) through 12 (4096 bytes)
    int32_t sector_size = shift < 9 ? 0 : (1 << shift);

    fclose(fp);
    return sector_size;
}

static int is_image_file(const char* name) {
    const char* dot = strrchr(name, '.');
    if (!dot) return 0;
    if (!strcasecmp(dot, ".ffpkg")) return IMAGE_FFPKG;
    else if (!strcasecmp(dot, ".ufs")) return IMAGE_UFS;
    else if (!strcasecmp(dot, ".exfat")) return IMAGE_EXFAT;
    else return 0;
}

static int find_image_in_dir(const char* dir, char* out, size_t out_sz) {
    DIR* d = opendir(dir); if (!d) return 0;
    struct dirent* e;
    int image = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (!(image = is_image_file(e->d_name))) continue;
        snprintf(out, out_sz, "%s/%s", dir, e->d_name);
        closedir(d); return image;
    }
    closedir(d); return 0;
}

static bool find_mountdev(const char* file_path, char* dev_path, size_t dev_path_len) {
    int mdctl = open("/dev/mdctl", O_RDWR);
    if (mdctl < 0) {
        notify("/dev/mdctl open failed: %s", strerror(errno));
        return false;
    }

    struct md_ioctl mdio;
    char current_file[PATH_MAX];
    bool exist = false;

    for (int unit = 0; unit < MD_UNIT_MAX; unit++) {
        memset(&mdio, 0, sizeof(mdio));
        mdio.md_version = MDIOVERSION;
        mdio.md_unit = unit;
        mdio.md_file = current_file;

        if (ioctl(mdctl, (unsigned long)MDIOCQUERY, &mdio) == 0) {
            if (mdio.md_type == MD_VNODE && strcmp(current_file, file_path) == 0) {
                exist = true;
                break;
            }
        }
    }

    if (exist)
        snprintf(dev_path, dev_path_len, "/dev/md%u", mdio.md_unit);
    close(mdctl);
    return exist;
}

static const char* find_mountpoint(const char* src) {
    struct statfs *mntbuf;
    int mntsize = getmntinfo(&mntbuf, MNT_WAIT);

    for (int i = 0; i < mntsize; i++) {
        /*printf("Device: %s  Mounted on: %s  Type: %s\n",
               mntbuf[i].f_mntfromname,
               mntbuf[i].f_mntonname,
               mntbuf[i].f_fstypename);*/
        if (strcmp(mntbuf[i].f_mntfromname, src) == 0) {
            return mntbuf[i].f_mntonname;
        }
    }
    return NULL;
}

// Image MOUNT - mount ffpkg image directly to the provided mount_point
static bool mount_ufs_image(const char* file_path, const char* mount_point, const char* fs, int sector_size, char* dev_path, size_t dev_path_len) {
    struct stat st;
    if (stat(file_path, &st) != 0) {
        notify("stat failed: %s", strerror(errno));
        return false;
    }

    if (mkdir(mount_point, 0777) != 0 && errno != EEXIST) {
        notify("mkdir failed: %s", strerror(errno));
        return false;
    }

    int mdctl = open("/dev/mdctl", O_RDWR);
    if (mdctl < 0) {
        notify("/dev/mdctl open failed: %s", strerror(errno));
        return false;
    }

    struct md_ioctl mdio;
    char current_file[PATH_MAX];
    int exist = 0;
    int ret;

    for (int unit = 0; unit < MD_UNIT_MAX; unit++) {
        memset(&mdio, 0, sizeof(mdio));
        mdio.md_version = MDIOVERSION;
        mdio.md_unit = unit;
        mdio.md_file = current_file;

        if (ioctl(mdctl, (unsigned long)MDIOCQUERY, &mdio) == 0) {
            if (mdio.md_type == MD_VNODE && strcmp(current_file, file_path) == 0) {
                exist = 1;
                break;
            }
        }
    }

    if (!exist) {
        memset(&mdio, 0, sizeof(mdio));
        mdio.md_version    = MDIOVERSION;
        mdio.md_type       = MD_VNODE;
        mdio.md_file       = (char*)file_path;
        mdio.md_mediasize  = st.st_size;
        mdio.md_sectorsize = sector_size;
        mdio.md_options    = MD_AUTOUNIT;

        ret = ioctl(mdctl, (unsigned long)MDIOCATTACH, &mdio);
        if (ret != 0) {
            notify("MDIOCATTACH failed: %s (errno %d)", strerror(errno), errno);
            close(mdctl);
            return false;
        }
    }

    snprintf(dev_path, dev_path_len, "/dev/md%u", mdio.md_unit);
    close(mdctl);

    printf("%s image attached as %s\n", fs, dev_path);

    struct iovec iov_ufs[] = {
        IOVEC_ENTRY("fstype"),    IOVEC_ENTRY("ufs"),
        IOVEC_ENTRY("fspath"),    IOVEC_ENTRY(mount_point),
        IOVEC_ENTRY("from"),      IOVEC_ENTRY(dev_path),
        IOVEC_ENTRY("async"),     IOVEC_ENTRY(NULL),
        IOVEC_ENTRY("noatime"),   IOVEC_ENTRY(NULL),
    };
    int iov_ufs_count = sizeof(iov_ufs) / sizeof(iov_ufs[0]);

    struct iovec iov_exfat[] = {
        IOVEC_ENTRY("fstype"),    IOVEC_ENTRY("exfatfs"),
        IOVEC_ENTRY("fspath"),    IOVEC_ENTRY(mount_point),
        IOVEC_ENTRY("from"),      IOVEC_ENTRY(dev_path),
        IOVEC_ENTRY("large"),     IOVEC_ENTRY("yes"),
        IOVEC_ENTRY("timezone"),  IOVEC_ENTRY("static"),
        IOVEC_ENTRY("async"),     IOVEC_ENTRY(NULL),
        IOVEC_ENTRY("ignoreacl"), IOVEC_ENTRY(NULL),
        IOVEC_ENTRY("noatime"),   IOVEC_ENTRY(NULL),
    };
    int iov_exfat_count = sizeof(iov_exfat) / sizeof(iov_exfat[0]);

    struct iovec* iov = (fs[0] == 'u') ? iov_ufs : iov_exfat;
    int iov_count = (fs[0] == 'u') ? iov_ufs_count : iov_exfat_count;

    // Prefer RW first for install compatibility
    ret = nmount(iov, iov_count, 0);
    int ro = 0;
    if (ret != 0) {
        printf("nmount rw failed: %s - falling back to rdonly\n", strerror(errno));
        ret = nmount(iov, iov_count, MNT_RDONLY);
        if (ret != 0) {
            notify("nmount ufs failed: %s", strerror(errno));
            return false;
        }
        ro = 1;
    }

    printf("Image mounted OK -> %s (%s)\n", mount_point, ro ? "ro" : "rw");
    return true;
}

static void unmount_ufs_image(const char* mount_point, const char* dev_path) {
    unmount(mount_point, MNT_FORCE);

    struct md_ioctl mdio = {0};
    mdio.md_version = MDIOVERSION;
    if (sscanf(dev_path, "/dev/md%u", &mdio.md_unit) > 0) {
        int mdctl = open("/dev/mdctl", O_RDWR);
        if (mdctl < 0) {
            notify("/dev/mdctl open failed: %s", strerror(errno));
            return;
        }

        ioctl(mdctl, (unsigned long)MDIOCDETACH, &mdio);
        close(mdctl);
    }
}

// MAIN
int main(int argc, const char* argv[]) {
    char cwd[PATH_MAX];
    char title_id[32] = {};
    char system_ex_app[PATH_MAX];
    char user_app_dir[PATH_MAX];
    char user_sce_sys[PATH_MAX];
    char mount_lnk_path[PATH_MAX];
    char param_json_path[PATH_MAX];
    const char* image_file_path = NULL;;

    notify("Dump Installer 1.08 Beta - UFS/EXFAT image Support");

    if (!getcwd(cwd, sizeof(cwd))) {
        printf("Error: Unable to determine working directory\n");
        return -1;
    }

    if (argc > 1) {
        image_file_path = argv[1];
    }

    kernel_set_ucred_authid(getpid(), 0x4800000000000010ULL);

    char image_file[PATH_MAX] = {};
    char tmp_mount[PATH_MAX] = {0};
    char dev_path[32] = {0};
    int fs_sector_size = 0;
    int has_image = 0;
    const char* fs = "nullfs";

    if (image_file_path) {
        has_image = is_image_file(image_file_path);
        if (!has_image) {
            notify("Not supported image file");
            return -1;
        }
    } else {
        has_image = find_image_in_dir(cwd, image_file, sizeof(image_file));
        image_file_path = image_file;
    }

    if (has_image == IMAGE_FFPKG || has_image == IMAGE_UFS) {
        fs_sector_size = detect_is_ufs(image_file_path);
        if (fs_sector_size <= 0) {
            notify("Error: Invalid UFS FFPKG image");
            return -1;
        }
        fs = "ufs";
    } else if (has_image == IMAGE_EXFAT) {
        fs_sector_size = detect_is_exfat(image_file_path);
        if (fs_sector_size <= 0) {
            notify("Error: Invalid EXFAT image");
            return -1;
        }
        fs = "exfatfs";
    }

    if (has_image) {
        notify("Image: %s SectorSize: %d", strrchr(image_file_path, '/') ? strrchr(image_file_path, '/') + 1 : image_file_path, fs_sector_size);

        // tmp mount to /data/di_tmp
        snprintf(tmp_mount, sizeof(tmp_mount), "%s/%s", "/data", "di_tmp");
        mkdir(tmp_mount, 0755);

        if (find_mountdev(image_file_path, dev_path, sizeof(dev_path))) {
            const char* mount_point = find_mountpoint(dev_path);
            const char sysex[] = "/system_ex/app/";
            if (mount_point && strncmp(mount_point, sysex, sizeof(sysex) - 1) == 0) {
                // unlink old mount.lnk
                snprintf(mount_lnk_path, sizeof(mount_lnk_path), "/user/app/%s/mount.lnk", mount_point + sizeof(sysex) - 1);
                unlink(mount_lnk_path);
                printf("unlink %s\n", mount_lnk_path);
            }
            unmount_ufs_image(mount_point ? mount_point : tmp_mount, dev_path);
            memset(dev_path, 0, sizeof(dev_path));
        }

        if (!mount_ufs_image(image_file_path, tmp_mount, fs, fs_sector_size, dev_path, sizeof(dev_path))) {
            notify("Image mount failed -> %s (errno %d)", tmp_mount, errno);
            unmount_ufs_image(tmp_mount, dev_path);
            return -1;
        }

        if (get_title_id(tmp_mount, title_id, sizeof(title_id)) != 0) {
            notify("Failed to read Title ID from %s/sce_sys", tmp_mount);
            unmount_ufs_image(tmp_mount, dev_path);
            return -1;
        }

        snprintf(param_json_path, sizeof(param_json_path),
                 "%s/sce_sys/param.json", tmp_mount);

        if (fix_application_drm_type(param_json_path) > 0)
            printf("applicationDrmType patched to standard\n");

        unmount(tmp_mount, MNT_FORCE);;
    } else {
        // Folder mode - derive title_id from sce_sys on disk
        notify("Folder: %s", cwd);
        if (get_title_id(cwd, title_id, sizeof(title_id)) != 0) {
            notify("Failed to read Title ID from %s/sce_sys", cwd);
            return -1;
        }

        snprintf(param_json_path, sizeof(param_json_path),
                 "%s/sce_sys/param.json", cwd);

        if (fix_application_drm_type(param_json_path) > 0)
            printf("applicationDrmType patched to standard\n");
    }

    notify("Installing %s, please wait...", title_id);

    snprintf(system_ex_app, sizeof(system_ex_app), "/system_ex/app/%s", title_id);
    mkdir(system_ex_app, 0755);
    unmount(system_ex_app, MNT_FORCE);

    if (has_image) {
        // Image - mount ffpkg image directly to /system_ex/app/<title_id>
        if (!mount_ufs_image(image_file_path, system_ex_app, fs, fs_sector_size, dev_path, sizeof(dev_path))) {
            notify("Image mount failed -> %s (errno %d)", system_ex_app, errno);
            unmount_ufs_image(system_ex_app, dev_path);
            return -1;
        }
    } else {
        // Folder mode - mount cwd via nullfs to /system_ex/app/<title_id>
        if (mount_nullfs(cwd, system_ex_app) != 0) {
            notify("Folder mount failed -> %s (errno %d)", system_ex_app, errno);
            return -1;
        }
    }

    notify("%s mounted OK -> %s", fs, system_ex_app);
    remount_system_ex();

    snprintf(user_app_dir, sizeof(user_app_dir), "/user/app/%s", title_id);
    mkdir(user_app_dir, 0755);

    snprintf(user_sce_sys, sizeof(user_sce_sys), "%s/sce_sys", user_app_dir);
    mkdir(user_sce_sys, 0755);

    if (has_image) {
        // Mount mode - copy sce_sys from mount point
        char src_sce_sys[PATH_MAX];
        snprintf(src_sce_sys, sizeof(src_sce_sys), "%s/sce_sys", system_ex_app);
        copy_dir(src_sce_sys, user_sce_sys);
        copy_sce_sys_to_appmeta(src_sce_sys, title_id);
    } else {
        // Folder mode - copy sce_sys from cwd
        char src_sce_sys[PATH_MAX];
        snprintf(src_sce_sys, sizeof(src_sce_sys), "%s/sce_sys", cwd);
        copy_dir(src_sce_sys, user_sce_sys);
        copy_sce_sys_to_appmeta(src_sce_sys, title_id);
    }

    // Use stronger authid for install
    kernel_set_ucred_authid(getpid(), 0x4801000000000013ULL);

    sceAppInstUtilInitialize();
    int install_ret = sceAppInstUtilAppInstallTitleDir(title_id, "/user/app/", 0);
    if (install_ret != 0) {
        notify("sceAppInstUtilAppInstallTitleDir failed: ret=0x%08x errno=%d (%s)",
               install_ret, errno, strerror(errno));
        return -1;
    }

    // Restore original authid
    kernel_set_ucred_authid(getpid(), 0x4800000000000010ULL);

    snprintf(mount_lnk_path, sizeof(mount_lnk_path), "/user/app/%s/mount.lnk", title_id);
    FILE* f = fopen(mount_lnk_path, "w");
    if (f) {
        if (has_image) {
            fprintf(f, "%s:%s", fs, image_file_path);
        } else {
            fprintf(f, "%s", cwd);
        }
        fclose(f);
        notify("mount.lnk to: %s", has_image ? image_file_path : cwd);
    } else {
        notify("Failed to create mount.lnk");
    }

    notify("Fixing Config (snd0), please wait...");

    sleep(3);
    update_snd0info(title_id);

    notify("%s installed and ready to use!", title_id);
    printf("The icon should now appear on the home screen.\n");

    return 0;
}
