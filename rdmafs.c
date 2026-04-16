/*
 * CS6983: Modern Storage System Course Project
 * Tingyu Qu
 *
 * rdmafs.h - RDMA-FS Proof-of-Concept
 *
 * ============================================================
 *  Shared Memory (1GB):
 * ============================================================
 *
 *  offset 0: root directory
 *           -----------------------------------
 *           |  dir_entry log[DIR_LOG_SIZE]    |  <- log of directories
 *           |  [entry0][entry1][entry2][0]... |  <- 0 - vacancy
 *           -----------------------------------
 *
 *  certain offset:  file inode
 *           -----------------------------------
 *           |  uid, gid, mode, ctime          |  <- standard inode items
 *           |  update log[FILE_LOG_SIZE]      |  <- 64-bit update log
 *           |  [upd0][upd1][upd2][0][0]...    |  <- 0 - vacancy
 *           -----------------------------------
 *
 *  certain offset:  child dir
 *           -----------------------------------
 *           |  dir_entry log[DIR_LOG_SIZE]    |
 *           |  ...                            |
 *           -----------------------------------
 *
 *  empty space can be managed simply by bump allocator
 *  (keep a "next free offset", CAS)
 *
 * ============================================================
 *  data (local file system):
 * ============================================================
 *
 *  m1.00001  <- machine 1, first object file
 *    ------------
 *    | block 0  |  4096 bytes
 *    | block 1  |  4096 bytes
 *    | block 2  |  4096 bytes
 *    |  ...     |
 *    ------------
 *
 *  m2.00001  <- machine 2, first object file
 *    ------------
 *    | block 0  |  4096 bytes
 *    |  ...     |
 *    ------------
 */

#define FUSE_USE_VERSION 30
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <fuse.h>
#include <time.h>

/* ============================================================
 *  Constant
 * ============================================================ */
#define BLOCK_SIZE      4096
#define MAX_NAME_LEN    32
#define FILE_LOG_SIZE   8192    /* At most 8K update for each file*/
#define DIR_LOG_SIZE    1024    /* At most 1K entry for each dir */
#define SHM_SIZE        (1UL << 30)  /* 1GB shared memory */
//#define DATA_DIR        "./data"     /* dir for storing object files */
static char data_dir[512];

/* ============================================================
 *  64-bit Update (core data for files)
 * ============================================================
 *
 *  One update represented in 64 bits: 
 *  "The len(#) blocks starting at file_offset of the file, 
 *   now stores at the position(th) block at obj in host,
 *   with invalid(#) of bytes invalid in the last block."
 *
 *    host:        2 bits  -> At most 4 machines
 *    obj:        12 bits  -> At most 4K objects for each machine
 *    position:   12 bits  -> In object, at most 4K blocks (= 16MB/object)
 *    file_offset:14 bits  -> At most 16K blocks for each file (= 64MB)
 *    len:        12 bits  -> At most 4K blocks per writing
 *    invalid:    12 bits  -> The invalid bytes for the last block (0-4095)
 *    IN TOTAL:   64 bits
 */
struct _update {
    uint64_t host        :  2;
    uint64_t obj         : 12;
    uint64_t position    : 12;
    uint64_t file_offset : 14;
    uint64_t len         : 12;
    uint64_t invalid     : 12;
};

union update {
    struct _update s;
    uint64_t       val;   /* 0 - vacancy, invalid update if all 0s */
};

/* ============================================================
 *  Directory Entry
 * ============================================================
 *
 *  log-structured: append one entry when creating a file,
 *                  append one entry with type=TOMBSTONE when deleting a file.
 *  Scan the whole log when searching for files, the last entry with the same name wins.
 */
enum entry_type {
    ENTRY_EMPTY = 0,    /* vacancy in log */
    ENTRY_FILE,
    ENTRY_DIR,
    ENTRY_TOMBSTONE     /* Deleted */
};

/* dir entry: pointing to file_inode OR dir_inode in shared memory*/
struct dir_entry {
    uint32_t type;                  /* enum entry_type */
    char     name[MAX_NAME_LEN];   /* name of file OR dir */
    uint64_t inode_offset;          /* offset in shared memory */
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;
    uint64_t ctime;
};

/* ============================================================
 *  File Inode (In shared memory)
 * ============================================================ */
struct file_inode {
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;
    uint64_t ctime;
    /* update log: all records for the file */
    union update log[FILE_LOG_SIZE];
    /* log instantialized as all 0s, CAS write to the first place of 0 */
};

/* ============================================================
 *  Dir Inode (In shared memory)
 * ============================================================ */
struct dir_inode {
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;
    uint64_t ctime;
    /* Dir log */
    struct dir_entry entries[DIR_LOG_SIZE];
};

/* ============================================================
 *  Head of Shared Memory
 * ============================================================ */
struct shm_header {
    uint64_t next_free;    /* bump allocator: next free offset, update by CAS */
    struct dir_inode root; /* root dir, offset = offsetof(shm_header, root) */
};

/* ============================================================
 *  GLOBAL (Every FUSE processes keep one by themselves)
 * ============================================================ */
static int       machine_num;       /* Machine # (0-3)                   */
static void     *shm_base;          /* Starting addr for shared memory   */
static int       cur_obj_num = 1;   /* File # of current object          */
static int       cur_obj_fd = -1;   /* file descriptor of current object */
static uint32_t  cur_block_pos = 0; /* block pos of current object       */

/* ============================================================
 *  Helper Method: CAS (Compare-And-Swap)
 * ============================================================
 *
 *  mmap MAP_SHARED, multiple processes can access at the same time,
 *  Atomic ops. GCC/Clang has CAS:
 */
static inline int cas64(uint64_t *ptr, uint64_t expected, uint64_t desired) {
    return __sync_bool_compare_and_swap(ptr, expected, desired);
}

/* ============================================================
 *  Helper Method: allocate space from shared memory (bump allocator)
 * ============================================================ */
static uint64_t shm_alloc(size_t size) {
    struct shm_header *hdr = (struct shm_header *)shm_base;
    /* 8-byte aligned */
    size = (size + 7) & ~7UL;
    while (1) {
        uint64_t old = hdr->next_free;
        uint64_t new_val = old + size;
        if (new_val > SHM_SIZE) return (uint64_t)-1; /* OOM */
        if (cas64(&hdr->next_free, old, new_val))
            return old;
        /* If CAS fails, it means others take the space, retry */
    }
}

/* ============================================================
 *  Helper Method: wrtie blcoks to object
 * ============================================================
 *
 *  append nblocks(#) of blocks in buf to current object
 *  return (object number, start position).
 *  If current object is too big, have a new one
 */
static void ensure_object_file(void) {
    if (cur_obj_fd >= 0) return;
    char path[256];
    snprintf(path, sizeof(path), "%s/m%d.%05d",
             data_dir, machine_num, cur_obj_num);
    cur_obj_fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (cur_obj_fd < 0) {
        fprintf(stderr, "ERROR: cannot open %s: %s\n", path, strerror(errno));
    }
    cur_block_pos = 0;
}

/* Write nblocks(#) of blocks, return the start of block position */
static uint32_t write_blocks(const char *buf, int nblocks,
                             int *out_obj) {
    ensure_object_file();
    uint32_t start_pos = cur_block_pos;
    write(cur_obj_fd, buf, nblocks * BLOCK_SIZE);
    cur_block_pos += nblocks;
    *out_obj = cur_obj_num;

    /* If object is too big, (> 4096 blocks), have a new one */
    if (cur_block_pos >= 4096) {
        close(cur_obj_fd);
        cur_obj_fd = -1;
        cur_obj_num++;
        cur_block_pos = 0;
    }
    return start_pos;
}

/* ============================================================
 *  Helper Method: read blocks from object 
 * ============================================================ */
static int read_blocks(int host, int obj, int position, int nblocks,
                       char *buf) {
    char path[256];
    snprintf(path, sizeof(path), "%s/m%d.%05d", data_dir, host, obj);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -errno;
    pread(fd, buf, nblocks * BLOCK_SIZE, position * BLOCK_SIZE);
    close(fd);
    return 0;
}

/* ============================================================
 *  Helper Method: separate the parent path and file/dir name from path
 * ============================================================ */
static int split_path(const char *path, char *parent_path,
                      size_t parent_size, char *name, size_t name_size) {
    char tmp[256];
    strncpy(tmp, path, sizeof(tmp));
    tmp[sizeof(tmp)-1] = '\0';

    char *last_slash = strrchr(tmp, '/');
    if (!last_slash) return -EINVAL;

    strncpy(name, last_slash + 1, name_size);
    name[name_size - 1] = '\0';

    if (last_slash == tmp) {
        strcpy(parent_path, "/");
    } else {
        *last_slash = '\0';
        strncpy(parent_path, tmp, parent_size);
        parent_path[parent_size - 1] = '\0';
    }
    return 0;
}

/* ============================================================
 *  Core: append update to file log (by CAS)
 * ============================================================
 *
 *  Scan log, find the first vacancy (val == 0), then write by CAS.
 *  If CAS fails, (others take the space), find the next vacancy and write.
 */
static int append_update(struct file_inode *fi, union update upd) {
    for (int i = 0; i < FILE_LOG_SIZE; i++) {
        if (fi->log[i].val == 0) {
            if (cas64(&fi->log[i].val, 0, upd.val))
                return 0;  /* Succeed */
            /* CAS fails, find the next vacancy */
        }
    }
    return -ENOSPC; /* log is full, NO SPACE */
}

/* ============================================================
 *  Core: read file - scan log, construct block map
 * ============================================================
 *
 *  Scan the whole log, for every file_offset, the last update wins.
 *  then read the according data to block map.
 */
 struct block_location {
    int host;
    int obj;
    int position;
    int valid; // 0 - this block not written yet
};
 static int resolve_file_blocks(struct file_inode *fi, struct block_location *bmap, int bmap_size) {
    memset(bmap, 0, sizeof(struct block_location) * bmap_size);
    int max_block = -1;
    int invalid_last_block = 0;
    for (int i = 0; i < FILE_LOG_SIZE; i++) {
        if (fi->log[i].val != 0) {
            struct _update u = fi->log[i].s;
            for (int b = 0; b < u.len; b++) {
                int foff = u.file_offset + b; // how many blocks away from the start of the file (logically)
                if (foff >= bmap_size) continue;
                bmap[foff].host = u.host;
                bmap[foff].obj = u.obj;
                bmap[foff].position = u.position + b;
                bmap[foff].valid = 1;
                
                if(foff > max_block) {
                    max_block = foff;
                    invalid_last_block = u.invalid;
                }
            }
        }
    }
    if(max_block < 0) return 0;
    return (max_block+1) * BLOCK_SIZE - invalid_last_block;
 }

/* ============================================================
 *  Core: Dir ops
 * ============================================================ */

/* Search for the name in dir, return the last entry (or NULL) that matches */
static struct dir_entry* dir_lookup(struct dir_inode *dir,
                                    const char *name) {
    struct dir_entry *found = NULL;
    for (int i = 0; i < DIR_LOG_SIZE; i++) {
        if (dir->entries[i].type == ENTRY_EMPTY) break;
        if (strncmp(dir->entries[i].name, name, MAX_NAME_LEN) == 0) {
            found = &dir->entries[i];
        }
    }
    /* It's been delted if the last one is TOMBSTONE */
    if (found && found->type == ENTRY_TOMBSTONE)
        return NULL;
    return found;
}

/* Append one entry to the dir by CAS */
static int dir_append(struct dir_inode *dir, struct dir_entry *entry) {
    for (int i = 0; i < DIR_LOG_SIZE; i++) {
        if (dir->entries[i].type == ENTRY_EMPTY) {
            /* 
             * simple solution: the last one is type, type from ENTRY_EMPTY(0) to entry->type
             * when other processes see type != 0, other props in entry are changed.
             *
             * complex solution: store the entry elsewhere in shared memory
             * store a 64-bit ptr in log, CAS the pointer.
             */
            dir->entries[i].inode_offset = entry->inode_offset;
            strncpy(dir->entries[i].name, entry->name, MAX_NAME_LEN);
            dir->entries[i].uid = entry->uid;
            dir->entries[i].gid = entry->gid;
            dir->entries[i].mode = entry->mode;
            dir->entries[i].ctime = entry->ctime;
            /* full memory barrier, global visible */
            __sync_synchronize();
            /* atomically, publish the entry */
            if (__sync_bool_compare_and_swap(&dir->entries[i].type,
                      ENTRY_EMPTY, entry->type))
                return 0;
        }
    }
    return -ENOSPC;
}

/* ============================================================
 *  resolve the path: "/foo/bar" -> find the corresponding inode
 * ============================================================ */
static void* resolve_path(const char *path, int *is_dir) {
    /* root */
    struct shm_header *hdr = (struct shm_header *)shm_base;
    struct dir_inode *cur_dir = &hdr->root;

    if (strcmp(path, "/") == 0) {
        if (is_dir) *is_dir = 1;
        return cur_dir;
    }

    /* skip the beginning '/' */
    char buf[256];
    strncpy(buf, path + 1, sizeof(buf));
    buf[sizeof(buf)-1] = '\0';

    char *saveptr;
    char *component = strtok_r(buf, "/", &saveptr);
    while (component) {
        struct dir_entry *e = dir_lookup(cur_dir, component);
        if (!e) return NULL;

        char *next = strtok_r(NULL, "/", &saveptr);
        if (next) {
            /* follows by more dirs, the current one must be a dir */
            if (e->type != ENTRY_DIR) return NULL;
            cur_dir = (struct dir_inode *)((char *)shm_base + e->inode_offset);
            component = next;
        } else {
            /* the last dir */
            if (is_dir) *is_dir = (e->type == ENTRY_DIR);
            return (char *)shm_base + e->inode_offset;
        }
    }
    return NULL;
}

/* ============================================================
 *  FUSE Callback
 * ============================================================ */

/* getattr: get the stat of file/dir */
static int rdmafs_getattr(const char *path, struct stat *st, struct fuse_file_info *fi) {
    (void)fi;
    memset(st, 0, sizeof(*st));

    int is_dir = 0;
    void *node = resolve_path(path, &is_dir);
    if (!node) return -ENOENT;

    if (is_dir) {
        struct dir_inode *d = (struct dir_inode *)node;
        st->st_mode = S_IFDIR | d->mode;
        st->st_nlink = 2;
        st->st_uid = d->uid;
        st->st_gid = d->gid;
    } else {
        struct file_inode *f = (struct file_inode *)node;
        st->st_mode = S_IFREG | f->mode;
        st->st_nlink = 1;
        st->st_uid = f->uid;
        st->st_gid = f->gid;
        /* scan f->log, calculate the size of file */
        struct block_location bmap[16384];
        st->st_size = resolve_file_blocks(f, bmap, 16384);
    }
    return 0;
}

/* readdir: list the content in a dir */
static int rdmafs_readdir(const char *path, void *buf,
                          fuse_fill_dir_t filler,
                          off_t offset, struct fuse_file_info *fi) {
    int is_dir = 0;
    void *node = resolve_path(path, &is_dir);
    if (!node || !is_dir) return -ENOENT;

    struct dir_inode *dir = (struct dir_inode *)node;
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    /* scan log, construct the valid entry set for current dir
     * (Need to deduplicate: the last entry with the same name wins, even if TOMBSTONE)
     */
    /* TODO: call filler(buf, name, NULL, 0) for every valid entry*/
    char seen_names[DIR_LOG_SIZE][MAX_NAME_LEN];
    int seen_count = 0;
    for(int i=0; i < DIR_LOG_SIZE; i++) {
        if (dir->entries[i].type == ENTRY_EMPTY) {break;} // || dir->entries[i].type == ENTRY_TOMBSTONE
        char * temp_name = dir->entries[i].name;
        int if_seen = 0;
        for(int j=0; j < seen_count; j++){
            if(strncmp(seen_names[j], temp_name, MAX_NAME_LEN) == 0){
                if_seen = 1;
                break;
            }
        }
        if(if_seen) continue;
        strncpy(seen_names[seen_count], temp_name, MAX_NAME_LEN);
        seen_count++;

        struct dir_entry *winner = dir_lookup(dir, temp_name);
        if(winner) {
            filler(buf, temp_name, NULL, 0, 0);
        }
    }

    return 0;
}

/* create: create a new file */
static int rdmafs_create(const char *path, mode_t mode,
                         struct fuse_file_info *fi) {
    /* TODO */
    // separate the parent dir and the file
    char temp_path[256];
    strncpy(temp_path, path, sizeof(temp_path));
    temp_path[sizeof(temp_path)-1] = '\0';

    char *last_slash = strrchr(temp_path, '/');
    if(!last_slash) return -EINVAL;

    char filename[MAX_NAME_LEN];
    strncpy(filename, last_slash+1, MAX_NAME_LEN);
    filename[MAX_NAME_LEN-1] = '\0';

    // parent dir
    char parent_path[256];
    if(last_slash == temp_path){
        strcpy(parent_path, "/");
    }else {
        *last_slash = '\0';
        strcpy(parent_path, temp_path);
    }

    // find the parent dir
    int is_dir = 0;
    void* temp = resolve_path(parent_path, &is_dir);
    if(NULL == temp || !is_dir){
        return -ENOENT;
    }
    struct dir_inode *parent = (struct dir_inode *) temp;
    // allocate file node
    uint64_t offset = shm_alloc(sizeof(struct file_inode));
    if(offset == (uint64_t)-1) return -ENOSPC;

    // initialization, shared memory is 0 though, set up the props
    struct file_inode *fi_node = (struct file_inode *)((char *)shm_base + offset);
    memset(fi_node, 0, sizeof(struct file_inode));
    fi_node->mode = mode & 0777;
    fi_node->uid = getuid();
    fi_node->gid = getgid();
    fi_node->ctime = time(NULL);

    // append
    struct dir_entry temp_entry;
    memset(&temp_entry, 0, sizeof(temp_entry));
    temp_entry.type = ENTRY_FILE;
    strncpy(temp_entry.name, filename, MAX_NAME_LEN);
    temp_entry.inode_offset = offset;
    temp_entry.uid = fi_node->uid;
    temp_entry.gid = fi_node->gid;
    temp_entry.mode = fi_node->mode;
    temp_entry.ctime = fi_node->ctime;
    return dir_append(parent, &temp_entry);
}

/* write: write to a new file */
static int rdmafs_write(const char *path, const char *buf,
                        size_t len, off_t offset,
                        struct fuse_file_info *fi) {
    /*
     * 1. find file_inode (via resolve_path)
     *
     * 2. Dealing with alignment:
     *    if offset is not 4096 aligned, OR len is not times of 4096,
     *    need read-modify-write:
     *    a) Read the first and the last original data
     *    b) write buf to the corresponding pos
     *    c) write block(s)
     *
     * 3. write data to object by write_blocks()
     *    get (obj_num, start_position)
     *
     * 4. construct union update:
     *    u.s.host = machine_num
     *    u.s.obj  = obj_num
     *    u.s.position = start_position
     *    u.s.file_offset = offset / BLOCK_SIZE
     *    u.s.len = # of blocks wrttien
     *    u.s.invalid = # of invalid bytes at the end of the file
     *
     * 5. wrtie(CAS) to log by append_update(file_inode, update)
     */

    /* TODO */
    int is_dir = 0;
    void* temp = resolve_path(path, &is_dir);
    if(NULL == temp || is_dir){
        return -ENOENT;
    }
    struct file_inode *fnode = (struct file_inode *) temp;

    int first_block = offset / BLOCK_SIZE;
    int last_block = (offset + len - 1) / BLOCK_SIZE;
    int nblocks = last_block - first_block + 1;

    char *aligned_buf = calloc(nblocks, BLOCK_SIZE);

    if(offset % BLOCK_SIZE != 0 || len % BLOCK_SIZE != 0) {

    }

    int buf_offset = offset % BLOCK_SIZE;
    memcpy(aligned_buf + buf_offset, buf, len);
    int obj_num = 1;
    uint32_t start_pos = write_blocks(aligned_buf, nblocks, &obj_num);

    union update u;
    u.val = 0;
    u.s.host = machine_num;
    u.s.obj  = obj_num;
    u.s.position = start_pos;
    u.s.file_offset = first_block;
    u.s.len = nblocks;
    u.s.invalid = (nblocks * BLOCK_SIZE) - (offset % BLOCK_SIZE + len);

    if(0 != append_update(fnode, u)) {
        free(aligned_buf);
        return -ENOSPC;
    }
    free(aligned_buf);
    return len;
}

/* read: read a file to a buf */
static int rdmafs_read(const char *path, char *buf, size_t len,
                       off_t offset, struct fuse_file_info *fi) {
    /*
     * 1. find file_inode (via resolve_path)
     * 2. scan log, constuct a block map (every file_offset -> the latest pos for storing)
     * 3. calculate the size of file, crop the range of reading
     * 4. for every valid block, read_blocks()
     * 5. put it to the buf
     */
    int is_dir = 0;
    void* temp = resolve_path(path, &is_dir);
    if(NULL == temp || is_dir){
        return -ENOENT;
    }
    struct file_inode *fnode = (struct file_inode *) temp;

    struct block_location bmap[16384];
    int file_size = resolve_file_blocks(fnode, bmap, 16384);

    // crop the range of reading
    if(offset >= file_size) return 0;
    if(offset + len > file_size) len = file_size - offset;

    // read block by block
    int bytes_read = 0;
    while(bytes_read < (int)len) {
        int fblock = (offset + bytes_read) / BLOCK_SIZE;
        int block_off = (offset + bytes_read) % BLOCK_SIZE;
        int to_read = BLOCK_SIZE - block_off;
        if (to_read > (int)len - bytes_read)
            {to_read = (int)len - bytes_read;}
        
        char temp[BLOCK_SIZE];
        if (bmap[fblock].valid) {
            read_blocks(bmap[fblock].host, bmap[fblock].obj,
                       bmap[fblock].position, 1, temp);
        } else {
            memset(temp, 0, BLOCK_SIZE);
        }
        memcpy(buf + bytes_read, temp + block_off, to_read);
        bytes_read += to_read;
    }

    return bytes_read;
}

/* mkdir */
static int rdmafs_mkdir(const char *path, mode_t mode) {
    char parent_path[256], dirname[MAX_NAME_LEN];
    if (split_path(path, parent_path, sizeof(parent_path),
                   dirname, sizeof(dirname)) != 0)
        return -EINVAL;

    int is_dir = 0;
    void *parent = resolve_path(parent_path, &is_dir);
    if (!parent || !is_dir) return -ENOENT;
    struct dir_inode *pdir = (struct dir_inode *)parent;

    if (dir_lookup(pdir, dirname) != NULL) return -EEXIST;

    uint64_t offset = shm_alloc(sizeof(struct dir_inode));
    if (offset == (uint64_t)-1) return -ENOSPC;

    struct dir_inode *new_dir = (struct dir_inode *)((char *)shm_base + offset);
    memset(new_dir, 0, sizeof(struct dir_inode));
    new_dir->mode = mode & 0777;
    new_dir->uid = getuid();
    new_dir->gid = getgid();
    new_dir->ctime = time(NULL);

    struct dir_entry entry;
    memset(&entry, 0, sizeof(entry));
    entry.type = ENTRY_DIR;
    strncpy(entry.name, dirname, MAX_NAME_LEN);
    entry.inode_offset = offset;
    entry.uid = new_dir->uid;
    entry.gid = new_dir->gid;
    entry.mode = new_dir->mode;
    entry.ctime = new_dir->ctime;

    return dir_append(pdir, &entry);
}

/* unlink : remove a single file */
static int rdmafs_unlink(const char *path) {
    char parent_path[256], filename[MAX_NAME_LEN];
    if (split_path(path, parent_path, sizeof(parent_path),
                   filename, sizeof(filename)) != 0)
        return -EINVAL;

    int is_dir = 0;
    void *parent = resolve_path(parent_path, &is_dir);
    if (!parent || !is_dir) return -ENOENT;
    struct dir_inode *pdir = (struct dir_inode *)parent;

    struct dir_entry *existing = dir_lookup(pdir, filename);
    if (!existing) return -ENOENT;
    if (existing->type != ENTRY_FILE) return -EISDIR;

    struct dir_entry tombstone;
    memset(&tombstone, 0, sizeof(tombstone));
    tombstone.type = ENTRY_TOMBSTONE;
    strncpy(tombstone.name, filename, MAX_NAME_LEN);

    return dir_append(pdir, &tombstone);
}

/* rmdir : remove empty directories */
static int rdmafs_rmdir(const char *path) {
    char parent_path[256], dirname[MAX_NAME_LEN];
    if (split_path(path, parent_path, sizeof(parent_path),
                   dirname, sizeof(dirname)) != 0)
        return -EINVAL;

    int is_dir = 0;
    void *parent = resolve_path(parent_path, &is_dir);
    if (!parent || !is_dir) return -ENOENT;
    struct dir_inode *pdir = (struct dir_inode *)parent;

    struct dir_entry *existing = dir_lookup(pdir, dirname);
    if (!existing) return -ENOENT;
    if (existing->type != ENTRY_DIR) return -ENOTDIR;

    struct dir_inode *target = (struct dir_inode *)((char *)shm_base + existing->inode_offset);
    for (int i = 0; i < DIR_LOG_SIZE; i++) {
        if (target->entries[i].type == ENTRY_EMPTY) break;
        if (target->entries[i].type == ENTRY_FILE ||
            target->entries[i].type == ENTRY_DIR) {
            if (dir_lookup(target, target->entries[i].name) != NULL)
                return -ENOTEMPTY;
        }
    }

    struct dir_entry tombstone;
    memset(&tombstone, 0, sizeof(tombstone));
    tombstone.type = ENTRY_TOMBSTONE;
    strncpy(tombstone.name, dirname, MAX_NAME_LEN);

    return dir_append(pdir, &tombstone);
}

/* utimes : set the timestamp */
static int rdmafs_utimens(const char *path, const struct timespec tv[2],
                          struct fuse_file_info *fi) {
    (void)path; (void)tv; (void)fi;
    return 0;
}

/* truncate : truncate the file */
static int rdmafs_truncate(const char *path, off_t size,
                           struct fuse_file_info *fi) {
    (void)path; (void)size; (void)fi;
    return 0;
}

/* ============================================================
 *  FUSE ops & main
 * ============================================================ */
static struct fuse_operations fs_ops = {
    .getattr = rdmafs_getattr,
    .readdir = rdmafs_readdir,
    .create  = rdmafs_create,
    .write   = rdmafs_write,
    .read    = rdmafs_read,
    .mkdir   = rdmafs_mkdir,
    .unlink  = rdmafs_unlink,
    .rmdir   = rdmafs_rmdir,
    .utimens  = rdmafs_utimens,
    .truncate = rdmafs_truncate
};

static struct fuse_opt opts[] = {
    {"-machine %d", 0, 0},
    FUSE_OPT_END
};

int main(int argc, char **argv) {
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    if (fuse_opt_parse(&args, &machine_num, opts, NULL) == -1)
        exit(1);

    /* create the dir of data */
    mkdir("./data", 0755);
    realpath("./data", data_dir);
    printf("Data dir: %s\n", data_dir);

    /* open shared memory */
    int fd = shm_open("/rdmafs", O_RDWR | O_CREAT, 0600);
    ftruncate(fd, SHM_SIZE);
    
    shm_base = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE,
                    MAP_SHARED, fd, 0);
    close(fd);

    /* if first process, instantialize shared memory */
    struct shm_header *hdr = (struct shm_header *)shm_base;
    uint64_t expected_free = 0;
    if (cas64(&hdr->next_free, 0, sizeof(struct shm_header))) {
        /* first, instantialize root */
        hdr->root.mode = 0755;
        hdr->root.uid = getuid();
        hdr->root.gid = getgid();
        hdr->root.ctime = time(NULL);
        printf("Machine %d: initialized shared memory\n", machine_num);
    } else {
        printf("Machine %d: attached to existing shared memory\n", machine_num);
    }

    printf("Machine %d: starting FUSE\n", machine_num);

    /*
     * Use:
     *   mkdir -p /tmp/mnt0 /tmp/mnt1
     *   ./rdmafs -machine 0 /tmp/mnt0
     *   ./rdmafs -machine 1 /tmp/mnt1   (another terminal window)
     *
     *   # Write in mnt0, can read from mnt1:
     *   echo "hello" > /tmp/mnt0/test.txt
     *   cat /tmp/mnt1/test.txt
     */
    return fuse_main(args.argc, args.argv, &fs_ops, NULL);
}
