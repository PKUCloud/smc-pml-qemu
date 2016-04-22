#ifndef MIGRATION_SMC_H
#define MIGRATION_SMC_H

#include "qemu/typedefs.h"
#include "qemu-common.h"
#include "smc-debug.h"
#include "smc-cache.h"

#define SMC_DIRTY_FLAGS_IN_CHECKPOINT   0x1U

/* Info about a dirty page within a chunk */
typedef struct SMCDirtyPage {
    /* Offset of the RAMBlock which contains the page */
    uint64_t block_offset;
    /* Offset inside the RAMBlock which contains the page */
    uint64_t offset;
    uint32_t size;
    uint32_t flags;
} SMCDirtyPage;

/* Maintain an array of a struct */
typedef struct SMCSet {
    uint8_t *eles;     /* An array of a struct */
    int cap;        /* Total number of the struct @eles can hold */
    int nb_eles;    /* Current number of the struct @eles holds */
    int ele_size;   /* sizeof(struct) */
} SMCSet;

#define SMC_JHASH_INIT_VAL  1824115964UL
typedef uint32_t    SMC_HASH;

typedef struct SMCFetchPage {
    uint64_t block_offset;
    uint64_t offset;
    uint32_t size;
    uint32_t idx;   /* Index in SMCSet */
    SMC_HASH hash;
} SMCFetchPage;

typedef struct SMCBackupPage {
    uint64_t block_offset;
    uint64_t offset;
    uint64_t size;
    uint8_t *data;
    uint8_t *host_addr;
} SMCBackupPage;

#define SMC_STATE_RECV_CHECKPOINT       0
#define SMC_STATE_PREFETCH_START        1
#define SMC_STATE_PREFETCH_DONE         2
#define SMC_STATE_PREFETCH_ABANDON      3
#define SMC_STATE_TRANSACTION_START     4

#define SMC_FETCH_CACHE_CAP             20000

typedef struct SMCInfo {
    bool init;
    SMCSet dirty_pages;
    SMCSet prefetch_pages;
    /* Pay attention to maintain the dynamically allocated memory */
    SMCSet backup_pages;
    /* Used to find whether a given page is a the prefetch list when the dst
     * is committing the checkpoint.
     * [page_physical_addr] -> the pointer of the corresponding SMCFetchPage
     */
    GHashTable *prefetch_map;
    int state;
    bool need_rollback;
    void *opaque;   /* QEMUFileRDMA */
    SMCCache cache;
    uint64_t nr_checkpoints;
} SMCInfo;

extern SMCInfo glo_smc_info;

void smc_init(SMCInfo *smc_info, void *opaque);
void smc_exit(SMCInfo *smc_info);
void smc_dirty_pages_insert(SMCInfo *smc_info, uint64_t block_offset,
                            uint64_t offset, uint32_t size, uint32_t flags);
void smc_dirty_pages_reset(SMCInfo *smc_info);
void smc_dirty_pages_insert_from_buf(SMCInfo *smc_info, const void *buf,
                                     int nb_pages);
void smc_prefetch_pages_reset(SMCInfo *smc_info);
SMCFetchPage *smc_prefetch_pages_insert(SMCInfo *smc_info,
                                        uint64_t block_offset,
                                        uint64_t offset, uint32_t size,
                                        SMC_HASH hash);
void smc_prefetch_pages_insert_from_buf(SMCInfo *smc_info, const void *buf,
                                        int nb_pages);

int smc_send_dirty_info(void *opaque, SMCInfo *smc_info);
int smc_recv_dirty_info(void *opaque, SMCInfo *smc_info);
int smc_recv_prefetch_info(void *opaque, SMCInfo *smc_info,
                           bool request_info);
int smc_sync_notice_dest_to_recv(void *opaque, SMCInfo *smc_info);
int smc_sync_src_ready_to_recv(void *opaque, SMCInfo *smc_info);
int smc_prefetch_dirty_pages(void *opaque, SMCInfo *smc_info);
void smc_backup_pages_insert(SMCInfo *smc_info, uint64_t block_offset,
                             uint64_t offset, uint64_t size,
                             uint8_t *data);
void *smc_backup_pages_insert_empty(SMCInfo *smc_info, uint64_t block_offset,
                                    uint64_t offset, uint64_t size,
                                    uint8_t *host_addr);
void smc_backup_pages_reset(SMCInfo *smc_info);
void smc_recover_backup_pages(SMCInfo *smc_info);
void smc_prefetch_page_cal_hash(SMCInfo *smc_info, int index);
void smc_rollback_with_prefetch(SMCInfo *smc_info);
bool smc_loadvm_need_check_prefetch(SMCInfo *smc_info);
bool smc_check_dirty_page(SMCInfo *smc_info, uint64_t block_offset,
                          uint64_t offset, uint64_t size, SMC_HASH hash_val);

uint8_t *smc_host_addr_from_offset(void *opaque, uint64_t block_offset,
                                   uint64_t offset);
void smc_prefetch_map_gen_from_pages(SMCInfo *smc_info);
void smc_update_prefetch_cache(SMCInfo *smc_info);

static inline int smc_dirty_pages_count(SMCInfo *smc_info)
{
    return smc_info->dirty_pages.nb_eles;
}

static inline SMCDirtyPage *smc_dirty_pages_info(SMCInfo *smc_info)
{
    return (SMCDirtyPage *)(smc_info->dirty_pages.eles);
}

static inline bool smc_is_init(SMCInfo *smc_info)
{
    return smc_info->init;
}

static inline int smc_prefetch_pages_count(SMCInfo *smc_info)
{
    return smc_info->prefetch_pages.nb_eles;
}

static inline SMCFetchPage *smc_prefetch_pages_info(SMCInfo *smc_info)
{
    return (SMCFetchPage *)(smc_info->prefetch_pages.eles);
}

static inline void smc_prefetch_map_reset(SMCInfo *smc_info)
{
    g_hash_table_remove_all(smc_info->prefetch_map);
}

static inline void smc_prefetch_map_insert(SMCInfo *smc_info, uint64_t phy_addr,
                                           SMCFetchPage *page)
{
    gpointer key = (void *)(uintptr_t)phy_addr;

    SMC_ASSERT(page);
    SMC_ASSERT(g_hash_table_lookup(smc_info->prefetch_map, key) == NULL);

    g_hash_table_insert(smc_info->prefetch_map, key, page);
}

static inline SMCFetchPage *smc_prefetch_map_lookup(SMCInfo *smc_info,
                                                    uint64_t phy_addr)
{
    return g_hash_table_lookup(smc_info->prefetch_map,
                               (void *)(uintptr_t)phy_addr);
}

static inline void smc_set_state(SMCInfo *smc_info, int state)
{
    smc_info->state = state;
}

static inline int smc_get_state(SMCInfo *smc_info)
{
    return smc_info->state;
}
#endif
