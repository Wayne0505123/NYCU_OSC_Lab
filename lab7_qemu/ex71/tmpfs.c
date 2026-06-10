#include "vfs.h"

#define TMPFS_MAX_FILE_NAME 15
#define TMPFS_MAX_DIR_ENTRY 16
#define TMPFS_MAX_FILE_SIZE 4096

enum fsnode_type { FS_DIR, FS_FILE };

struct tmpfs_vnode {
    enum fsnode_type type;
    char name[TMPFS_MAX_FILE_NAME];
    struct vnode* entry[TMPFS_MAX_DIR_ENTRY];
    char* data;
    size_t size;
};

struct file_operations tmpfs_file_ops = {.open = tmpfs_open,
                                         .close = tmpfs_close,
                                         .read = tmpfs_read,
                                         .write = tmpfs_write};

struct vnode_operations tmpfs_vnode_ops = {.lookup = tmpfs_lookup,
                                           .create = tmpfs_create};

struct vnode* tmpfs_create_vnode(enum fsnode_type type) {
    struct vnode* vnode = calloc(1, sizeof(struct vnode));
    struct tmpfs_vnode* inode = calloc(1, sizeof(struct tmpfs_vnode));

    if (!vnode || !inode) {
        free(vnode);
        free(inode);
        return NULL;
    }

    inode->type = type;
    vnode->v_ops = &tmpfs_vnode_ops;
    vnode->f_ops = &tmpfs_file_ops;
    vnode->internal = inode;
    return vnode;
}

int tmpfs_setup_mount(struct filesystem* fs, struct mount* mnt) {
    mnt->root = tmpfs_create_vnode(FS_DIR);
    mnt->fs = fs;
    return 0;
}

int tmpfs_open(struct vnode* file_node, struct file** target) {
    (*target)->vnode = file_node;
    (*target)->f_ops = &tmpfs_file_ops;
    (*target)->f_pos = 0;
    return 0;
}

int tmpfs_close(struct file* file) {
    free(file);
    return 0;
}

int tmpfs_read(struct file* file, void* buf, size_t len) {
    struct tmpfs_vnode* inode = file->vnode->internal;

    if (inode->type != FS_FILE)
        return -EINVAL;
    if (!inode->data || file->f_pos >= inode->size)
        return 0;

    size_t readable = inode->size - file->f_pos;
    if (len > readable)
        len = readable;

    memcpy(buf, inode->data + file->f_pos, len);
    file->f_pos += len;
    return len;
}

int tmpfs_write(struct file* file, const void* buf, size_t len) {
    struct tmpfs_vnode* inode = file->vnode->internal;

    if (inode->type != FS_FILE)
        return -EINVAL;
    if (file->f_pos >= TMPFS_MAX_FILE_SIZE)
        return 0;
    if (!inode->data) {
        inode->data = calloc(1, TMPFS_MAX_FILE_SIZE);
        if (!inode->data)
            return -ENOMEM;
    }

    size_t writable = TMPFS_MAX_FILE_SIZE - file->f_pos;
    if (len > writable)
        len = writable;

    memcpy(inode->data + file->f_pos, buf, len);
    file->f_pos += len;
    if (file->f_pos > inode->size)
        inode->size = file->f_pos;
    return len;
}

int tmpfs_lookup(struct vnode* dir_node,
                 struct vnode** target,
                 const char* component_name) {
    struct tmpfs_vnode* dentry = dir_node->internal;
    for (int i = 0; i < TMPFS_MAX_DIR_ENTRY; i++) {
        if (!dentry->entry[i])
            return -1;
        struct tmpfs_vnode* inode = dentry->entry[i]->internal;
        if (!strcmp(inode->name, component_name)) {
            *target = dentry->entry[i];
            return 0;
        }
    }
    return -1;
}

int tmpfs_create(struct vnode* dir_node,
                 struct vnode** target,
                 const char* component_name) {
    struct tmpfs_vnode* dentry = dir_node->internal;

    if (dentry->type != FS_DIR)
        return -ENOTDIR;

    for (int i = 0; i < TMPFS_MAX_DIR_ENTRY; i++) {
        if (dentry->entry[i]) {
            struct tmpfs_vnode* inode = dentry->entry[i]->internal;
            if (!strcmp(inode->name, component_name))
                return -EEXIST;
            continue;
        }

        struct vnode* vnode = tmpfs_create_vnode(FS_FILE);
        if (!vnode)
            return -ENOMEM;

        struct tmpfs_vnode* inode = vnode->internal;
        strncpy(inode->name, component_name, TMPFS_MAX_FILE_NAME - 1);
        dentry->entry[i] = vnode;
        *target = vnode;
        return 0;
    }

    return -ENOSPC;
}
