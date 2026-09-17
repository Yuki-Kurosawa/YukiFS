// SPDX-License-Identifier: MIT
//
// fsck.c - YukiFS filesystem checker / recovery tool. Built on the former
// "scanyuki" scanner, it adds fsck behaviour:
//   * consistency check against infofs ground truth (blind scan never trusts
//     the superblock offset fields);
//   * -f/--fix: write reconstructed superblock offset fields back to the
//     device/image, so a damaged image mounts again;
//   * works on both regular image files (incl. loop images) and block devices.
//
// Two scan modes:
//   A) metadata partially readable: 55AA/YUKI found -> blind-scan the inode
//      table on FILE_OBJECT_ALIGN_SIZE boundaries with structural heuristics
//      (never trusts superblock->inode_table_offset/bitmap_offset/...).
//   B) everything destroyed: data carving. Guess the block size by zero-block
//      alignment, classify blocks as zero/sparse/data, and group contiguous
//      data blocks into recovered regions. Zero AND sparse blocks act as
//      separators, so "big file A + deleted small file B + big file C" is NOT
//      merged into one giant file - the sparse residue of B is reported as a
//      suspected hole instead.
//
// Usage: fsck.yukifs [-b blocksize] [-c] [-t sparse_threshold_pct]
//                    [-f|--fix] [-v|--version] <image|blockdev>
//
// Tested against 1024/2048/4096 byte block sizes.
// 8192 was dropped: the VFS buffer layer cannot mount blocks > PAGE_SIZE, so
// the filesystem no longer produces them. -b can still force any candidate.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <errno.h>

#include "../../include/file_table.h"

#define FSCK_VERSION "0.1.0"
#define HD_SUPERBLOCK_OFFSET_POS 136
#define CANDIDATE_BLOCK_SIZES { 1024, 2048, 4096 }
#define DEFAULT_SPARSE_THRESHOLD_PCT 5
/* An image whose hidden data (0x55AA) starts closer than this to offset 0 has
   no meaningful head padding - it is a plain file / partition slice, not a
   disk image. fsck --fix then pads the head to this alignment and saves the
   result as a NEW image file instead of patching in place. 1 MiB is the
   standard modern partition alignment (2048 x 512B sectors). */
#define PARTITION_ALIGN_SIZE (1024U * 1024U)

struct sb_view {
    int valid;
    uint32_t block_size;
    uint32_t block_count;
    uint64_t sb_offset;
    uint64_t hd_offset; /* absolute offset of the 0x55AA hidden-data header */
};

struct found {
    uint64_t offset;
    struct file_object fo;
};

static const char *mode_str(unsigned int d)
{
    switch (d & S_IFMT) {
        case S_IFREG: return "file";
        case S_IFDIR: return "dir ";
        case S_IFLNK: return "link";
        default:      return "????";
    }
}

static int is_printable_name(const unsigned char *s)
{
    if (s[0] == 0) return 0;
    for (int i = 0; i < FS_MAX_LEN; i++) {
        if (s[i] == 0) return 1;
        if (s[i] < 0x20 || s[i] > 0x7E) return 0;
    }
    return 0;
}

static int looks_like_inode(const struct file_object *fo,
                            uint32_t block_count, uint32_t block_size)
{
    if (fo->in_use != 1) return 0;
    if (!is_printable_name((const unsigned char *)fo->name)) return 0;
    if ((fo->descriptor & S_IFMT) != S_IFREG &&
        (fo->descriptor & S_IFMT) != S_IFDIR &&
        (fo->descriptor & S_IFMT) != S_IFLNK) return 0;
    if (fo->inner_file < 0) return 0;
    if (block_count == 0) return 1;
    if (fo->first_block >= block_count) return 0;
    uint64_t n = (fo->size + block_size - 1) / block_size;
    if (n == 0) n = 1;
    if ((uint64_t)fo->first_block + n > block_count) return 0;
    return 1;
}

static uint64_t rd_u64(const unsigned char *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static uint32_t rd_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr_u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static int64_t find_bytes(const unsigned char *img, size_t size,
                          const unsigned char *pat, size_t plen,
                          size_t start, size_t window_limit)
{
    size_t end = size;
    if (window_limit && end > window_limit) end = window_limit;
    if (start + plen > end) return -1;
    for (size_t i = start; i + plen <= end; i++) {
        if (memcmp(img + i, pat, plen) == 0) return (int64_t)i;
    }
    return -1;
}

/* ---- mode A: metadata-aided blind scan ------------------------------- */

static struct sb_view detect_metadata(const unsigned char *img, size_t size,
                                      uint32_t forced_bs)
{
    struct sb_view sb = {0};
    static const unsigned char magic55aa[2] = {0x55, 0xAA};

    int64_t hd = find_bytes(img, size, magic55aa, 2, 0, size);
    if (hd >= 0 && (int64_t)(hd + sizeof(struct hidden_data_struct)) <= (int64_t)size) {
        const unsigned char *h = img + hd;
        if (h[0x90] == 0xAA && h[0x91] == 0x55) {
            uint64_t sbo = rd_u64(h + HD_SUPERBLOCK_OFFSET_POS);
            if (sbo + sizeof(struct superblock_info) <= size) {
                const unsigned char *s = img + sbo;
                if (memcmp(s, "YUKI", 4) == 0) {
                    sb.valid = 1;
                    sb.hd_offset = (uint64_t)hd;
                    sb.block_size = rd_u32(s + 8);
                    sb.block_count = rd_u32(s + 12);
                    sb.sb_offset = sbo;
                    return sb;
                }
            }
        }
    }
    if (forced_bs) {
        sb.valid = 0;
        sb.block_size = forced_bs;
        sb.block_count = 0;
    }
    return sb;
}

static void print_off(const char *name, uint32_t raw, uint64_t recon, uint64_t size)
{
    if (raw <= size && (uint64_t)raw == recon) {
        printf("  %s: %llu\n", name, (unsigned long long)recon);
    } else {
        printf("  %s: %llu (reconstructed; raw=%u corrupt)\n", name,
               (unsigned long long)recon, raw);
    }
}

struct sb_offsets {
    uint64_t ito, dbo, dbo_total, dbo_end, bmo, unalloc;
    uint32_t bmb;
};

/* Reconstruct the superblock offset layout from mkfs formulas (blind scan
   result ito_recon + layout rules), exactly as print_sb_full reports it. */
static void compute_sb_offsets(const unsigned char *s, uint32_t bs, uint32_t bc,
                               uint64_t size, uint64_t ito_recon,
                               struct sb_offsets *o)
{
    uint32_t total_inodes = rd_u32(s + 20);
    uint64_t it_storage = ((uint64_t)total_inodes * FILE_OBJECT_ALIGN_SIZE +
                           bs - 1) / bs * bs;
    o->ito = ito_recon;
    o->bmo = o->ito + it_storage;
    o->bmb = (uint32_t)(((bc + 7) / 8 + bs - 1) / bs);
    o->dbo = o->bmo + (uint64_t)o->bmb * bs;
    o->dbo_total = (uint64_t)bc * bs;
    o->dbo_end = o->dbo + o->dbo_total;
    o->unalloc = size > o->dbo_end ? size - o->dbo_end : 0;
}

static void print_sb_full(const unsigned char *s, uint32_t bs, uint32_t bc,
                          uint64_t size, uint64_t ito_recon)
{
    /* Layout-derived offsets (same formulas as mkfs), so that even when the
       offset fields in the superblock are corrupt we print the values infofs
       would print on an intact image. Raw values are always shown when they
       differ. */
    struct sb_offsets o;
    compute_sb_offsets(s, bs, bc, size, ito_recon, &o);
    uint32_t total_inodes = rd_u32(s + 20);

    printf("Superblock Info (offsets reconstructed, not trusted from disk):\n");
    printf("  Superblock Size: %zu\n", sizeof(struct superblock_info));
    printf("  Magic Number: %02X%02X%02X%02X%02X%02X%02X%02X\n",
           s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
    printf("  Magic String: %.8s\n", s);
    printf("  Block Size: %u\n", bs);
    printf("  Block Count: %u\n", bc);
    printf("  Free Blocks: %u\n", rd_u32(s + 16));
    printf("  Total Inodes: %u\n", total_inodes);
    printf("  Free Inodes: %u\n", rd_u32(s + 24));
    printf("  Inode Table Size: %u\n", rd_u32(s + 28));
    printf("  Inode Table Clusters: %u\n", rd_u32(s + 32));
    printf("  Inode Table Storage Size: %u\n", rd_u32(s + 36));
    print_off("Inode Table Offset", rd_u32(s + 40), o.ito, size);
    print_off("Data Blocks Offset", rd_u32(s + 44), o.dbo, size);
    print_off("Data Blocks Total Size", rd_u32(s + 48), o.dbo_total, size);
    print_off("Data Blocks End Offset", rd_u32(s + 52), o.dbo_end, size);
    print_off("Block Bitmap Offset", rd_u32(s + 56), o.bmo, size);
    print_off("Block Bitmap Blocks", rd_u32(s + 60), o.bmb, size);
    print_off("Unallocated Space Size", rd_u32(s + 64), o.unalloc, size);
}

/* Count superblock offset fields whose raw value is corrupt (out of range or
   != reconstruction). Used by fsck to report "problems remain" when run
   without -f/--fix. */
static int count_bad_sb_fields(const unsigned char *s, uint32_t bs, uint32_t bc,
                               uint64_t size, uint64_t ito_recon)
{
    struct sb_offsets o;
    compute_sb_offsets(s, bs, bc, size, ito_recon, &o);
    struct { unsigned off; uint64_t recon; } fields[] = {
        { 40, o.ito }, { 44, o.dbo }, { 48, o.dbo_total },
        { 52, o.dbo_end }, { 56, o.bmo }, { 60, o.bmb },
        { 64, o.unalloc },
    };
    int bad = 0;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        uint32_t raw = rd_u32(s + fields[i].off);
        if (!(raw <= size && (uint64_t)raw == fields[i].recon))
            bad++;
    }
    return bad;
}

/* Write the reconstructed offset fields back into the superblock on the
   device/image, so a damaged image mounts again. Only fields whose raw value
   is corrupt (out of range or != reconstruction) are touched. Returns the
   number of fields written, -1 on a hard error. */
static int fix_sb(int fd, const unsigned char *s, uint32_t bs, uint32_t bc,
                  uint64_t size, uint64_t ito_recon, uint64_t sb_off)
{
    struct sb_offsets o;
    compute_sb_offsets(s, bs, bc, size, ito_recon, &o);
    struct { const char *name; unsigned off; uint64_t recon; } fields[] = {
        { "Inode Table Offset",     40, o.ito },
        { "Data Blocks Offset",     44, o.dbo },
        { "Data Blocks Total Size", 48, o.dbo_total },
        { "Data Blocks End Offset", 52, o.dbo_end },
        { "Block Bitmap Offset",    56, o.bmo },
        { "Block Bitmap Blocks",    60, o.bmb },
        { "Unallocated Space Size", 64, o.unalloc },
    };
    int fixed = 0;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        uint32_t raw = rd_u32(s + fields[i].off);
        if (raw <= size && (uint64_t)raw == fields[i].recon)
            continue; /* already consistent */
        uint32_t val = (uint32_t)fields[i].recon;
        if (pwrite(fd, &val, 4, (off_t)(sb_off + fields[i].off)) != 4) {
            perror("pwrite");
            return -1;
        }
        printf("  fixed %-26s: %u -> %llu\n", fields[i].name, raw,
               (unsigned long long)fields[i].recon);
        fixed++;
    }
    return fixed;
}

/* Write a repaired copy of the image to <out_path>. Corrupt superblock offset
   fields are replaced with reconstructed values; the input is never touched.
   Returns 0 on success, -1 on error. */
static int emit_fixed_copy(const unsigned char *img, size_t size,
                           const struct sb_view *sb, uint64_t ito_recon,
                           const char *out_path)
{
    unsigned char *buf = malloc(size);
    if (!buf) { perror("malloc"); return -1; }
    memcpy(buf, img, size);

    const unsigned char *s = buf + sb->sb_offset;
    struct sb_offsets o;
    compute_sb_offsets(s, sb->block_size, sb->block_count, size, ito_recon, &o);
    struct { unsigned off; uint64_t recon; } fields[] = {
        { 40, o.ito }, { 44, o.dbo }, { 48, o.dbo_total },
        { 52, o.dbo_end }, { 56, o.bmo }, { 60, o.bmb },
        { 64, o.unalloc },
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        uint32_t raw = rd_u32(s + fields[i].off);
        if (raw <= size && (uint64_t)raw == fields[i].recon)
            continue;
        wr_u32((unsigned char *)s + fields[i].off, (uint32_t)fields[i].recon);
        printf("  fixed %-26s: %u -> %llu\n", "sb field",
               raw, (unsigned long long)fields[i].recon);
    }

    int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); free(buf); return -1; }
    size_t off = 0;
    while (off < size) {
        ssize_t w = write(fd, buf + off, size - off);
        if (w <= 0) { perror("write"); close(fd); free(buf); return -1; }
        off += (size_t)w;
    }
    fdatasync(fd);
    close(fd);
    free(buf);
    printf("  repaired image written to %s (%zu bytes, input untouched)\n",
           out_path, size);
    return 0;
}

/* Pad-and-save mode: the image's hidden data starts too close to offset 0
   (plain file / partition slice, no head padding). Build a NEW image whose
   head is padded to PARTITION_ALIGN_SIZE, shift every absolute offset field
   (hidden-data header + superblock) by the padding delta, repair corrupt
   fields with reconstructed values, and write the result to <out_path>. The
   input is never modified. Returns 0 on success, -1 on error. */
static int emit_padded_image(const unsigned char *img, size_t size,
                             const struct sb_view *sb, uint64_t ito_recon,
                             const char *out_path)
{
    uint64_t hd = sb->hd_offset;
    if (!hd || hd >= PARTITION_ALIGN_SIZE) {
        fprintf(stderr, "emit_padded_image: nothing to pad (hd=%llu)\n",
                (unsigned long long)hd);
        return -1;
    }
    uint64_t delta = PARTITION_ALIGN_SIZE - hd;
    size_t new_size = (size_t)(delta + size);
    unsigned char *buf = calloc(1, new_size);
    if (!buf) { perror("calloc"); return -1; }
    memcpy(buf + delta, img, size);

    /* hidden-data header: absolute offset fields shift by delta. Written
       through struct hidden_data_struct to honor compiler member alignment
       (see rebuild_image). */
    struct hidden_data_struct *h =
        (struct hidden_data_struct *)(buf + PARTITION_ALIGN_SIZE);
    h->hidden_data_offset += (uint32_t)delta;
    h->built_in_kernel_module_offset += (uint32_t)delta;
    h->superblock_offset += delta;

    /* superblock: absolute offset fields shift by delta; corrupt fields take
       the reconstructed value (already shifted); size/count fields keep their
       original value. */
    unsigned char *s = buf + delta + sb->sb_offset;
    struct sb_offsets o;
    compute_sb_offsets(s, sb->block_size, sb->block_count,
                       (uint64_t)new_size, ito_recon + delta, &o);
    struct { const char *name; unsigned off; int is_offset; uint64_t recon; } fields[] = {
        { "Inode Table Offset",     40, 1, o.ito },
        { "Data Blocks Offset",     44, 1, o.dbo },
        { "Data Blocks Total Size", 48, 0, o.dbo_total },
        { "Data Blocks End Offset", 52, 1, o.dbo_end },
        { "Block Bitmap Offset",    56, 1, o.bmo },
        { "Block Bitmap Blocks",    60, 0, o.bmb },
        { "Unallocated Space Size", 64, 0, o.unalloc },
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        uint32_t raw = rd_u32(s + fields[i].off);
        uint64_t target;
        if (raw <= size && (uint64_t)raw == fields[i].recon - (fields[i].is_offset ? delta : 0))
            target = fields[i].is_offset ? (uint64_t)raw + delta : (uint64_t)raw;
        else
            target = fields[i].recon;
        if (target != raw) {
            wr_u32(s + fields[i].off, (uint32_t)target);
            printf("  %-26s: %u -> %llu\n", fields[i].name, raw,
                   (unsigned long long)target);
        }
    }

    int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); free(buf); return -1; }
    size_t off = 0;
    while (off < new_size) {
        ssize_t w = write(fd, buf + off, new_size - off);
        if (w <= 0) { perror("write"); close(fd); free(buf); return -1; }
        off += (size_t)w;
    }
    fdatasync(fd);
    close(fd);
    free(buf);
    printf("  padded head by %llu bytes, hidden data now at %u\n",
           (unsigned long long)delta, PARTITION_ALIGN_SIZE);
    printf("  repaired image written to %s (%zu bytes, input untouched)\n",
           out_path, new_size);
    return 0;
}

static void print_ts(const char *label, uint64_t sec){
    if (sec == 0) { printf(" %s=-", label); return; }
    time_t t = (time_t)sec;
    char buf[32];
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    printf(" %s=%s", label, buf);
}

static int mode_a_scan(const unsigned char *img, size_t size,
                       const struct sb_view *sb, uint64_t *ito_out)
{
    uint32_t bs = sb->block_size;
    uint32_t bc = sb->block_count;
    uint64_t start = 0;
    if (sb->valid) {
        uint32_t sb_padded = bs > SUPER_BLOCK_ALIGN_SIZE ? bs : SUPER_BLOCK_ALIGN_SIZE;
        start = sb->sb_offset + sb_padded;
    }
    if (start + sizeof(struct file_object) > size) start = 0;

    struct found *found = NULL;
    size_t found_n = 0, found_cap = 0;

    printf("\n--- blind-scanned inode slots (aligned %u bytes) ---\n",
           FILE_OBJECT_ALIGN_SIZE);
    for (uint64_t off = start; off + sizeof(struct file_object) <= size;
         off += FILE_OBJECT_ALIGN_SIZE) {
        const struct file_object *fo = (const struct file_object *)(img + off);
        if (!looks_like_inode(fo, bc, bs)) continue;
        if (found_n == found_cap) {
            found_cap = found_cap ? found_cap * 2 : 64;
            found = realloc(found, found_cap * sizeof(*found));
            if (!found) { perror("realloc"); return 1; }
        }
        found[found_n].offset = off;
        memcpy(&found[found_n].fo, fo, sizeof(struct file_object));
        found_n++;
    }

    /* The lowest inode slot found is the root slot, hence the real
       inode_table_offset - even when the superblock field is corrupt. */
    uint64_t ito_recon = found_n ? found[0].offset : 0;
    if (ito_out) *ito_out = ito_recon;
    int sb_problems = 0;
    if (sb->valid) {
        print_sb_full(img + sb->sb_offset, bs, bc, size, ito_recon);
        sb_problems = count_bad_sb_fields(img + sb->sb_offset, bs, bc,
                                          size, ito_recon);
        if (sb_problems)
            printf("  [fsck] %d superblock offset field(s) inconsistent; "
                   "re-run with -f/--fix to repair\n", sb_problems);
    }

    if (found_n == 0) printf("  (no inode candidates found)\n");
    for (size_t i = 0; i < found_n; i++) {
        const struct file_object *fo = &found[i].fo;
        int has_idx = ito_recon && found[i].offset >= ito_recon &&
                      (found[i].offset - ito_recon) % FILE_OBJECT_ALIGN_SIZE == 0;
        printf("  off %8llu : %-*s %s size=%-7u first_block=%-5u mode=%06o uid=%-5u gid=%-5u",
               (unsigned long long)found[i].offset, FS_MAX_LEN, fo->name,
               mode_str(fo->descriptor), fo->size, fo->first_block,
               fo->descriptor, fo->uid, fo->gid);
        if (has_idx)
            printf(" inode_idx=%llu",
                   (unsigned long long)((found[i].offset - ito_recon) / FILE_OBJECT_ALIGN_SIZE));
        if (fo->inner_file) printf(" inner=%d", fo->inner_file);
        print_ts("atime", fo->atime_sec);
        print_ts("mtime", fo->mtime_sec);
        print_ts("ctime", fo->ctime_sec);
        printf("\n");
    }

    /* hole analysis: mark referenced data blocks, report unreferenced runs.
       This is what separates "big A + deleted small B + big C" - the residue
       of B leaves an unreferenced run between A and C that a naive scanner
       would have merged. No superblock offset fields are used here. */
    uint32_t bc2 = bc ? bc : (uint32_t)(size / bs);
    unsigned char *ref = calloc(bc2 ? bc2 : 1, 1);
    if (ref) {
        if (bc2 > 0) ref[0] = 1; /* root directory data block */
        for (size_t i = 0; i < found_n; i++) {
            const struct file_object *fo = &found[i].fo;
            if ((fo->descriptor & S_IFMT) == S_IFDIR) continue;
            uint64_t n = (fo->size + bs - 1) / bs;
            if (!n) n = 1;
            for (uint64_t k = 0; k < n; k++) {
                uint64_t b = (uint64_t)fo->first_block + k;
                if (b < bc2) ref[b] = 1;
            }
        }
        uint64_t max_ref = 0;
        for (uint64_t b = 0; b < bc2; b++) if (ref[b]) max_ref = b;
        printf("\n--- holes (unreferenced data blocks) ---\n");
        int any = 0;
        uint64_t b = 1;
        while (b <= max_ref) {
            if (!ref[b]) {
                uint64_t s = b;
                while (b + 1 <= max_ref && !ref[b + 1]) b++;
                printf("  hole: blocks %llu..%llu (bytes %llu..%llu) - deleted file suspect\n",
                       (unsigned long long)s, (unsigned long long)b,
                       (unsigned long long)(uint64_t)s * bs,
                       (unsigned long long)((uint64_t)(b + 1) * bs - 1));
                any = 1;
            }
            b++;
        }
        if (!any) printf("  (no holes)\n");
        free(ref);
    }
    free(found);
    return sb_problems;
}

/* ---- mode B: pure data carving --------------------------------------- */

static uint32_t guess_block_size(const unsigned char *img, size_t size,
                                 size_t *best_score_out)
{
    static const uint32_t cand[] = CANDIDATE_BLOCK_SIZES;
    uint32_t best = 1024;
    size_t best_score = 0;
    printf("  candidate block sizes (zero-block score):");
    for (size_t i = 0; i < sizeof(cand) / sizeof(cand[0]); i++) {
        uint32_t bs = cand[i];
        size_t nblk = size / bs;
        size_t score = 0;
        for (size_t b = 0; b < nblk; b++) {
            const unsigned char *p = img + b * bs;
            int zero = 1;
            for (size_t j = 0; j < bs; j++) {
                if (p[j]) { zero = 0; break; }
            }
            if (zero) score++;
        }
        printf(" %u:%zu", bs, score);
        if (score > best_score) { best_score = score; best = bs; }
    }
    printf("\n");
    if (best_score == 0) {
        printf("  (no zero blocks anywhere - block size ambiguous, "
               "defaulting to %u; retry with -b <blocksize>)\n", best);
    }
    if (best_score_out) *best_score_out = best_score;
    return best;
}

/* classify a block: 0=zero, 1=sparse, 2=data */
static int classify_block(const unsigned char *p, uint32_t bs, int threshold_pct)
{
    size_t nonzero = 0;
    for (uint32_t j = 0; j < bs; j++) if (p[j]) nonzero++;
    if (nonzero == 0) return 0;
    if (nonzero * 100 / bs < (size_t)threshold_pct) return 1;
    return 2;
}

static void guess_type(const unsigned char *p, size_t len, char *out, size_t outsz)
{
    if (len >= 4 && memcmp(p, "\x7f" "ELF", 4) == 0) {
        snprintf(out, outsz, "elf");
        return;
    }
    /* printable-text ratio */
    size_t printable = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = p[i];
        if (c == 0) break;
        if (c >= 0x20 && c <= 0x7E) printable++;
    }
    if (printable * 100 / (len ? len : 1) >= 80) {
        snprintf(out, outsz, "text");
        return;
    }
    /* plausible uint32 directory array? some slots nonzero, all values small */
    int dir_like = 0;
    size_t n = len / 4;
    for (size_t i = 0; i < n && i < 64; i++) {
        uint32_t v = rd_u32(p + i * 4);
        if (v > 0x01000000) { dir_like = -1; break; }
        if (v) dir_like = 1;
    }
    if (dir_like == 1 && n >= 2) {
        snprintf(out, outsz, "dir-array?");
        return;
    }
    snprintf(out, outsz, "unknown");
}

static void preview(const unsigned char *p, size_t len, char *out, size_t outsz)
{
    size_t n = len < 24 ? len : 24;
    size_t used = 0;
    for (size_t i = 0; i < n && used + 3 < outsz; i++) {
        used += (size_t)snprintf(out + used, outsz - used, "%02X ", p[i]);
    }
}

struct region {
    uint64_t start;  /* first data block number (relative to the data area) */
    uint64_t count;  /* number of contiguous data blocks */
};

static int mode_b_carve(const unsigned char *img, size_t size,
                        uint32_t forced_bs, int threshold_pct,
                        struct region **regions_out, size_t *nregions_out,
                        uint32_t *bs_out)
{
    uint32_t bs = forced_bs;
    if (!bs) bs = guess_block_size(img, size, NULL);
    if (bs_out) *bs_out = bs;
    size_t nblk = size / bs;

    printf("\n=== metadata GONE - data carving mode ===\n");
    printf("block size: %u (zero-block alignment heuristic%s)\n",
           bs, forced_bs ? ", user-forced" : "");

    /* classify every block */
    unsigned char *cls = malloc(nblk);
    if (!cls) { perror("malloc"); return 1; }
    size_t n_zero = 0, n_sparse = 0, n_data = 0;
    for (size_t b = 0; b < nblk; b++) {
        int c = classify_block(img + b * bs, bs, threshold_pct);
        cls[b] = (unsigned char)c;
        if (c == 0) n_zero++;
        else if (c == 1) n_sparse++;
        else n_data++;
    }
    printf("block classes: zero=%zu sparse=%zu data=%zu (sparse threshold %d%%)\n",
           n_zero, n_sparse, n_data, threshold_pct);

    /* group contiguous data blocks into recovered regions; zero/sparse split */
    printf("\n--- recovered regions / holes ---\n");
    size_t regions = 0, holes = 0;
    struct region *rlist = NULL;
    size_t rcap = 0;
    size_t b = 0;
    while (b < nblk) {
        if (cls[b] == 2) {
            size_t start = b;
            while (b + 1 < nblk && cls[b + 1] == 2) b++;
            uint64_t byte_off = (uint64_t)start * bs;
            uint64_t byte_len = (uint64_t)(b - start + 1) * bs;
            char typ[32], prv[96];
            guess_type(img + byte_off, byte_len > 256 ? 256 : byte_len, typ, sizeof(typ));
            preview(img + byte_off, byte_len > 64 ? 64 : byte_len, prv, sizeof(prv));
            printf("  region blk %zu..%zu : size=%llu type=%s preview=%s%s\n",
                   start, b, (unsigned long long)byte_len, typ, prv,
                   start == 0 ? "  <- possible root directory block" : "");
            if (regions == rcap) {
                rcap = rcap ? rcap * 2 : 64;
                rlist = realloc(rlist, rcap * sizeof(*rlist));
                if (!rlist) { perror("realloc"); free(cls); return 1; }
            }
            rlist[regions].start = (uint64_t)start;
            rlist[regions].count = (uint64_t)(b - start + 1);
            regions++;
            b++;
        } else {
            /* hole: zero or sparse run */
            size_t start = b;
            int kind = cls[b];
            while (b + 1 < nblk && cls[b + 1] != 2) b++;
            printf("  hole   blk %zu..%zu : %s (bytes %llu..%llu)%s\n",
                   start, b,
                   kind == 0 ? "zero block(s)" : "sparse residue (deleted small file suspect)",
                   (unsigned long long)(uint64_t)start * bs,
                   (unsigned long long)((uint64_t)(b + 1) * bs - 1),
                   start == 0 ? "  <- possible root directory block" : "");
            holes++;
            b++;
        }
    }
    printf("\n  total: %zu region(s), %zu hole region(s)\n", regions, holes);
    printf("  NOTE: carving cannot recover file names; regions are byte-order\n");
    printf("        reconstructions. A zero/sparse separator means the tool did\n");
    printf("        NOT merge surrounding data into one giant file.\n");

    free(cls);
    if (regions_out) *regions_out = rlist; else free(rlist);
    if (nregions_out) *nregions_out = regions;
    return 0;
}

/* ---- foreign filesystem detection (priority 4) ------------------------- */

struct foreign_fs {
    const char *name;   /* human readable */
    const char *fsck;   /* fsck program to dispatch to */
    size_t off;         /* signature offset */
    const unsigned char *sig;
    size_t len;
};

static const unsigned char SIG_XFS[]    = { 'X','F','S','B' };
static const unsigned char SIG_BTRFS[]  = { '_','B','H','R','f','S','_','M' };
static const unsigned char SIG_NTFS[]   = { 'N','T','F','S' };
static const unsigned char SIG_EXFAT[]  = { 'E','X','F','A','T' };
static const unsigned char SIG_F2FS[]   = { 'F','2','F','S' };
static const unsigned char SIG_JFS[]    = { 'J','F','S','1' };
static const unsigned char SIG_UDF[]    = { 'B','E','A','0','1' };
static const unsigned char SIG_REISER[] = { 'R','e','i','s','e','r','F','s' };
static const unsigned char SIG_EXT[]    = { 0x53, 0xEF }; /* ext2/3/4 @1080 */

/* FAT boot sectors end with 0x55AA at offset 510 and carry a BPB; distinguish
   from a bare MBR by the BPB jump + OEM name area. */
static int looks_like_fat(const unsigned char *p, size_t size)
{
    if (size < 512) return 0;
    if (!(p[510] == 0x55 && p[511] == 0xAA)) return 0;
    if (!(p[0] == 0xEB || p[0] == 0xE9)) return 0; /* x86 jump */
    return 1;
}

static const char *detect_foreign_fs(const unsigned char *img, size_t size,
                                     const char **fsck_out)
{
    static const struct foreign_fs table[] = {
        { "ext2/ext3/ext4", "fsck.ext4", 1080, SIG_EXT, sizeof(SIG_EXT) },
        { "XFS",            "xfs_repair", 0,    SIG_XFS, sizeof(SIG_XFS) },
        { "Btrfs",          "btrfs",     0x10040, SIG_BTRFS, sizeof(SIG_BTRFS) },
        { "NTFS",           "ntfsfix",   3,    SIG_NTFS, sizeof(SIG_NTFS) },
        { "exFAT",          "fsck.exfat",3,    SIG_EXFAT, sizeof(SIG_EXFAT) },
        { "F2FS",           "fsck.f2fs", 1024, SIG_F2FS, sizeof(SIG_F2FS) },
        { "JFS",            "fsck.jfs",  32768, SIG_JFS, sizeof(SIG_JFS) },
        { "UDF",            "fsck.udf",  32769, SIG_UDF, sizeof(SIG_UDF) },
        { "ReiserFS",       "fsck.reiserfs", 0x10000, SIG_REISER, sizeof(SIG_REISER) },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        const struct foreign_fs *f = &table[i];
        if (f->off + f->len > size) continue;
        if (memcmp(img + f->off, f->sig, f->len) == 0) {
            *fsck_out = f->fsck;
            return f->name;
        }
    }
    if (looks_like_fat(img, size)) {
        *fsck_out = "fsck.fat";
        return "FAT12/16/32";
    }
    return NULL;
}

/* Check/fix semantics of fsck.yukifs are NOT those of the target fsck: we
   pass only the image path plus the flag that mirrors our mode, so the
   hand-off works with no tty attached (e2fsck otherwise aborts with
   "need terminal for interactive repairs"). */
struct foreign_fsck_map {
    const char *prog_substr;  /* substring of the fsck program name */
    const char *check_opt;    /* added when we run WITHOUT --fix (read-only) */
    const char *fix_opt;      /* added when we run WITH --fix (auto-repair) */
};

/* Dispatch the check to the matching fsck program (priority 4). The original
   command-line options of fsck.yukifs are NOT forwarded (the target would not
   understand --fix/-b/...); only the image path and the translated mode flag
   are. If the program is not installed, exit gracefully instead of pretending
   to check it. */
static int dispatch_foreign_fs(const char *fsck_prog, const char *fname,
                               int do_fix, const char *path)
{
    printf("not a YukiFS image: detected %s filesystem\n", fname);
    printf("handing off to %s...\n", fsck_prog);

    static const struct foreign_fsck_map maps[] = {
        { "xfs_repair",    "-n",            "" },          /* repairs by default */
        { "btrfs",         "",              "--repair" },  /* read-only by default */
        { "ntfsfix",       NULL,            "" },          /* repair-only: no read-only check mode */
        { "e2fsck",        "-n",            "-y" },
        { "fsck.ext",      "-n",            "-y" },
        { "fsck.fat",      "-n",            "-y" },
        { "fsck.vfat",     "-n",            "-y" },
        { "fsck.msdos",    "-n",            "-y" },
        { "fsck.exfat",    "-n",            "-y" },
        { "fsck.f2fs",     "-n",            "-y" },
        { "fsck.jfs",      "-n",            "-y" },
        { "fsck.udf",      "-n",            "-y" },
        { "fsck.reiserfs", "-n",            "-y" },
        { "fsck.minix",    "-n",            "-y" },
        { NULL,            NULL,            NULL },
    };
    const char *map_opt = NULL;
    int map_found = 0;
    for (const struct foreign_fsck_map *m = maps; m->prog_substr; m++) {
        if (strstr(fsck_prog, m->prog_substr)) {
            map_found = 1;
            map_opt = do_fix ? m->fix_opt : m->check_opt;
            break;
        }
    }
    if (map_found && map_opt == NULL) {
        fprintf(stderr, "fsck.yukifs: %s only repairs, it has no read-only "
                        "check mode; re-run with --fix to invoke it\n",
                fsck_prog);
        return 1;
    }
    if (map_opt && map_opt[0])
        printf("  translated semantics: %s\n", map_opt);

    char *new_argv[4];
    int n = 0;
    new_argv[n++] = (char *)fsck_prog;
    new_argv[n++] = (char *)path;
    if (map_opt && map_opt[0]) new_argv[n++] = (char *)map_opt;
    new_argv[n] = NULL;

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 8; }
    if (pid == 0) {
        execvp(fsck_prog, new_argv);
        fprintf(stderr, "fsck.yukifs: %s not found - cannot check %s image; "
                        "exiting\n", fsck_prog, fname);
        _exit(127);
    }
    int st;
    if (waitpid(pid, &st, 0) < 0) { perror("waitpid"); return 8; }
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    return 8;
}

/* Length of the leading run of zero bytes - the "head padding". YukiFS disk
   images normally have >= 1 MiB here (partition alignment); a plain file or a
   raw partition slice has only fs_padding (or none at all). */
static size_t head_zero_len(const unsigned char *img, size_t size)
{
    size_t z = 0;
    while (z < size && img[z] == 0) z++;
    return z;
}

/* ---------------------------------------------------------------------- */

/* Rebuild a full YukiFS metadata set (hidden header + superblock + inode
   table + block bitmap) from carving results, then lay out the data area.
   priority 3/5 (save mode): write a NEW image file - metadata at the head,
   carved data moved after it. priority 2 (in_place): rewrite the metadata at
   the head of the existing image, data stays where it is (only valid when
   the reconstructed metadata fits in the zero head). The superblock layout
   follows mkfs formulas (no built-in kernel module: hidden data == one
   block). Returns 0 on success, 8 on hard error. */
static int rebuild_image(const unsigned char *img, size_t size, uint32_t bs,
                         size_t Z, const struct region *regions, size_t nregions,
                         int in_place, int orig_fd, const char *out_path)
{
    uint64_t data_len = (uint64_t)size - Z;      /* bytes of carved data area */
    uint64_t nblk = data_len / bs;               /* total data blocks */
    uint64_t tail = data_len - nblk * bs;        /* trailing partial block */
    uint64_t inode_count = 1 + nregions;
    uint64_t it_storage = ((nblk * FILE_OBJECT_ALIGN_SIZE + bs - 1) / bs) * bs;
    uint64_t bmb = (((nblk + 7) / 8) + bs - 1) / bs;
    uint64_t sb_off = (uint64_t)bs * 2;          /* no built-in ko */
    uint64_t ito = sb_off + bs;                  /* superblock padded to bs */
    uint64_t bmo = ito + it_storage;
    uint64_t dbo_formula = bmo + bmb * bs;
    uint64_t dbo = in_place ? (uint64_t)Z : dbo_formula;
    uint64_t dbo_end = dbo + nblk * bs;
    uint64_t new_size = dbo_end + tail;

    if (!in_place && dbo_end > UINT32_MAX) {
        fprintf(stderr, "fsck: rebuilt layout exceeds the 4GiB limit\n");
        return 8;
    }
    if (in_place && dbo_formula > Z) {
        fprintf(stderr, "fsck: in-place rebuild needs %llu bytes of head space "
                        "but only %zu zero bytes are available; "
                        "use -o/--out to save a new image instead\n",
                (unsigned long long)dbo_formula, Z);
        return 8;
    }

    unsigned char *buf = calloc(1, new_size ? new_size : 1);
    if (!buf) { perror("calloc"); return 8; }
    if (in_place) {
        memcpy(buf, img, size); /* preserve data area + any non-zero tail */
    } else {
        memcpy(buf + dbo, img + Z, data_len); /* move carved data after metadata */
    }

    time_t now = time(NULL);

    /* hidden data header (no built-in kernel module), 55AA at fs_padding=bs.
       Written through struct hidden_data_struct so the compiler-applied
       member alignment (u32 fields shifted by 2 after the u8 arrays) matches
       what mkfs and infofs produce/consume. */
    struct hidden_data_struct *hd = (struct hidden_data_struct *)(buf + bs);
    memset(hd, 0, sizeof(struct hidden_data_struct));
    hd->hidden_magic_number[0] = 0x55;
    hd->hidden_magic_number[1] = 0xAA;
    hd->hidden_end_magic_number[0] = 0xAA;
    hd->hidden_end_magic_number[1] = 0x55;
    hd->fs_version[0] = 0; hd->fs_version[1] = 1; hd->fs_version[2] = 0;
    memcpy(hd->fs_build_tool_name, "fsck", 4);
    hd->fs_build_tool_version[0] = 0; hd->fs_build_tool_version[1] = 1;
    hd->fs_build_tool_version[2] = 0;
    hd->built_in_ELF_offset = 0;
    hd->built_in_ELF_size = 0;
    hd->built_in_ELF_storage_size = bs;
    hd->hidden_data_offset = bs;
    hd->hidden_data_header_size = sizeof(struct hidden_data_struct);
    hd->hidden_data_header_storage_size = bs;
    hd->hidden_data_size = bs;
    hd->hidden_data_storage_size = bs;
    /* built_in_kernel_module_version[64]: all zero (no built-in ko) */
    hd->built_in_kernel_module_offset = bs * 2;
    hd->built_in_kernel_module_size = 0;
    hd->built_in_kernel_module_storage_size = 0;
    hd->built_in_kernel_architechture = 0;
    hd->superblock_offset = sb_off;

    /* superblock */
    unsigned char *s = buf + sb_off;
    memset(s, 0, sizeof(struct superblock_info));
    memcpy(s + 0, "YUKI", 4);                    /* magic_number[8] */
    wr_u32(s + 8, bs);                           /* block_size */
    wr_u32(s + 12, (uint32_t)nblk);              /* block_count */
    /* count used data blocks (root + regions, clipped to nblk) */
    uint64_t used = 1;
    for (size_t i = 0; i < nregions; i++)
        used += regions[i].count < nblk - regions[i].start
                    ? regions[i].count : (nblk > regions[i].start ? nblk - regions[i].start : 0);
    wr_u32(s + 16, (uint32_t)(nblk - used));     /* block_free */
    wr_u32(s + 20, (uint32_t)nblk);              /* total_inodes */
    wr_u32(s + 24, (uint32_t)(nblk - inode_count)); /* free_inodes */
    wr_u32(s + 28, (uint32_t)(nblk * FILE_OBJECT_ALIGN_SIZE)); /* inode_table_size */
    wr_u32(s + 32, (uint32_t)((nblk * FILE_OBJECT_ALIGN_SIZE + bs - 1) / bs)); /* clusters */
    wr_u32(s + 36, (uint32_t)it_storage);        /* inode_table_storage_size */
    wr_u32(s + 40, (uint32_t)ito);               /* inode_table_offset */
    wr_u32(s + 56, (uint32_t)bmo);               /* bitmap_offset */
    wr_u32(s + 60, (uint32_t)bmb);               /* bitmap_blocks */
    wr_u32(s + 44, (uint32_t)dbo);               /* data_blocks_offset */
    wr_u32(s + 48, (uint32_t)(nblk * bs));       /* data_blocks_total_size */
    wr_u32(s + 52, (uint32_t)dbo_end);           /* data_blocks_end_offset */
    wr_u32(s + 64, (uint32_t)tail);              /* unallocated_space_size */

    /* inode table: total_inodes slots, first = root, then one per region */
    for (uint64_t i = 0; i < nblk; i++)
        memset(buf + ito + i * FILE_OBJECT_ALIGN_SIZE, 0, FILE_OBJECT_ALIGN_SIZE);
    struct file_object *root = (struct file_object *)(buf + ito);
    root->in_use = 1;
    root->size = bs;
    root->inner_file = 0;
    root->name[0] = '$';
    root->descriptor = S_IFDIR | 0777;
    root->first_block = 0;
    root->uid = 0;
    root->gid = 0;
    root->atime_sec = root->mtime_sec = root->ctime_sec = (uint64_t)now;
    for (size_t i = 0; i < nregions; i++) {
        struct file_object *fo =
            (struct file_object *)(buf + ito + (i + 1) * FILE_OBJECT_ALIGN_SIZE);
        fo->in_use = 1;
        fo->size = (uint32_t)(regions[i].count * bs);
        fo->inner_file = 0;
        snprintf((char *)fo->name, sizeof(fo->name), "region-%zu", i);
        fo->descriptor = S_IFREG | 0644;
        fo->first_block = (uint32_t)regions[i].start;
        fo->uid = 0;
        fo->gid = 0;
        fo->atime_sec = fo->mtime_sec = fo->ctime_sec = (uint64_t)now;
    }

    /* block bitmap: bit per data block, block 0 (root) always used */
    size_t bitmap_bytes = (size_t)(bmb * bs);
    memset(buf + bmo, 0, bitmap_bytes);
    buf[bmo] = 0x01;
    for (size_t i = 0; i < nregions; i++) {
        uint64_t lim = regions[i].start + regions[i].count;
        if (lim > nblk) lim = nblk;
        for (uint64_t blk = regions[i].start; blk < lim; blk++)
            buf[bmo + blk / 8] |= (unsigned char)(1u << (blk % 8));
    }

    if (in_place) {
        if (pwrite(orig_fd, buf, size, 0) != (ssize_t)size) {
            perror("pwrite"); free(buf); return 8;
        }
        fdatasync(orig_fd);
        printf("  rebuilt metadata in place (%llu-byte head, data untouched)\n",
               (unsigned long long)dbo_formula);
    } else {
        int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { perror("open"); free(buf); return 8; }
        size_t off = 0;
        while (off < new_size) {
            ssize_t w = write(fd, buf + off, new_size - off);
            if (w <= 0) { perror("write"); close(fd); free(buf); return 8; }
            off += (size_t)w;
        }
        fdatasync(fd);
        close(fd);
        printf("  rebuilt image written to %s (%llu bytes, input untouched)\n",
               out_path, (unsigned long long)new_size);
    }
    free(buf);
    return 0;
}

/* ---------------------------------------------------------------------- */

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [-b blocksize] [-c] [-t pct] [-o file] [-f|--fix] [-v|--version]\n"
            "            <image|blockdev>\n"
            "  -b <bs>       force block size (default: auto-detect 1024/2048/4096)\n"
            "  -c            force carving mode even if metadata is readable\n"
            "  -t <pct>      sparse threshold percent 1..50 (default 5)\n"
            "  -o, --out <file>  save a repaired/rebuilt copy to <file> instead of\n"
            "                writing the input in place\n"
            "  -f, --fix     repair: write reconstructed offsets back to the\n"
            "                device/image (metadata mode), rebuild metadata in\n"
            "                place (enough head padding), or save a new image\n"
            "  -v, --version print version and exit\n"
            "processing priority:\n"
            "  1 metadata found            : carve + compare, fix offsets on request\n"
            "  2 metadata gone, padding ok : carve (rebuild in place with --fix)\n"
            "  3 metadata gone, no padding : carve, rebuild metadata, save new image\n"
            "  4 foreign filesystem        : hand off to its fsck, else exit\n"
            "  5 no padding at all         : carve, rebuild metadata, save new image\n"
            "exit status: 0 = clean or fixed, 1 = problems remain, 8 = I/O error,\n"
            "             16 = usage error\n",
            prog);
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    const char *output_path = NULL;
    uint32_t forced_bs = 0;
    int force_carve = 0, do_fix = 0;
    int threshold_pct = DEFAULT_SPARSE_THRESHOLD_PCT;

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-b") == 0) && i + 1 < argc) {
            forced_bs = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "-c") == 0) {
            force_carve = 1;
        } else if ((strcmp(argv[i], "-t") == 0) && i + 1 < argc) {
            threshold_pct = atoi(argv[++i]);
            if (threshold_pct < 1 || threshold_pct > 50) {
                fprintf(stderr, "sparse threshold must be 1..50\n");
                return 16;
            }
        } else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--out") == 0) {
            if (i + 1 >= argc) { print_usage(argv[0]); return 16; }
            output_path = argv[++i];
        } else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--fix") == 0) {
            do_fix = 1;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("fsck.yukifs %s (YukiFS filesystem checker)\n", FSCK_VERSION);
            return 0;
        } else if (argv[i][0] != '-') {
            path = argv[i];
        } else {
            print_usage(argv[0]);
            return 16;
        }
    }
    if (!path) {
        print_usage(argv[0]);
        return 16;
    }

    /* Open read-write in fix mode (repair writes back to the device/image),
       read-only otherwise. Both regular files (incl. loop images) and block
       devices are supported: size comes from fstat for files and from
       lseek(SEEK_END) for block devices. */
    int fd = open(path, do_fix ? O_RDWR : O_RDONLY);
    if (fd < 0) { perror("open"); return 8; }
    struct stat st;
    if (fstat(fd, &st) != 0) { perror("fstat"); close(fd); return 8; }
    uint64_t size64;
    if (S_ISBLK(st.st_mode)) {
        off_t end = lseek(fd, 0, SEEK_END);
        if (end < 0) { perror("lseek"); close(fd); return 8; }
        size64 = (uint64_t)end;
    } else {
        size64 = (uint64_t)st.st_size;
    }
    if (size64 == 0) { fprintf(stderr, "empty device/image\n"); close(fd); return 8; }
    size_t size = (size_t)size64;

    unsigned char *img;
    int mapped = 0;
    if (do_fix) {
        img = malloc(size);
        if (!img) { perror("malloc"); close(fd); return 8; }
        if (pread(fd, img, size, 0) != (ssize_t)size) {
            perror("pread"); free(img); close(fd); return 8;
        }
    } else {
        img = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (img == MAP_FAILED) {
            img = malloc(size);
            if (!img) { perror("malloc"); close(fd); return 8; }
            if (pread(fd, img, size, 0) != (ssize_t)size) {
                perror("pread"); free(img); close(fd); return 8;
            }
        } else {
            mapped = 1;
        }
    }

    printf("=== fsck.yukifs %s: %s (%zu bytes%s) ===\n", FSCK_VERSION, path, size,
           S_ISBLK(st.st_mode) ? ", block device" : "");

    struct sb_view sb = detect_metadata(img, size, forced_bs);

    /* ---- priority 1: metadata found - carve first, then compare/fix ------ */
    if (sb.valid) {
        printf("hidden data: found (0x55AA..0xAA55 ok)\n");
        printf("superblock  : offset %llu, block_size=%u, block_count=%u (YUKI ok)\n",
               (unsigned long long)sb.sb_offset, sb.block_size, sb.block_count);
        if (!force_carve) {
            uint64_t ito_recon = 0;
            int rc = mode_a_scan(img, size, &sb, &ito_recon);
            if (do_fix) {
                /* plain file whose hidden data sits too close to offset 0:
                   pad the head to partition alignment and save a NEW image */
                int need_pad = !S_ISBLK(st.st_mode) && sb.hd_offset &&
                               sb.hd_offset < PARTITION_ALIGN_SIZE;
                if (need_pad || output_path) {
                    char auto_name[4096];
                    const char *out = output_path;
                    if (!out) {
                        snprintf(auto_name, sizeof(auto_name), "%s.fsck.img", path);
                        out = auto_name;
                    }
                    if (need_pad) {
                        printf("\n=== head padding too small - pad-and-save mode ===\n");
                        if (emit_padded_image(img, size, &sb, ito_recon, out) != 0) {
                            if (mapped) munmap(img, size); else free(img);
                            close(fd);
                            return 8;
                        }
                    } else {
                        printf("\n=== save repaired copy ===\n");
                        if (emit_fixed_copy(img, size, &sb, ito_recon, out) != 0) {
                            if (mapped) munmap(img, size); else free(img);
                            close(fd);
                            return 8;
                        }
                    }
                    if (mapped) munmap(img, size); else free(img);
                    close(fd);
                    return 0;
                }
                int fixed = fix_sb(fd, img + sb.sb_offset, sb.block_size,
                                   sb.block_count, size, ito_recon, sb.sb_offset);
                if (fixed < 0) {
                    if (mapped) munmap(img, size); else free(img);
                    close(fd);
                    return 8;
                }
                printf("  [fsck] superblock offset fields: %d inconsistent -> written back\n",
                       fixed);
                fdatasync(fd);
                if (mapped) munmap(img, size); else free(img);
                close(fd);
                printf("\n=== done (metadata mode, repaired) ===\n");
                return 0;
            }
            if (mapped) munmap(img, size); else free(img);
            close(fd);
            printf("\n=== done (metadata mode) ===\n");
            /* fsck exit status: 1 if problems were found but not fixed */
            return rc ? 1 : 0;
        }
        printf("(-c forced) falling through to carving mode anyway\n");
    } else {
        printf("hidden data / superblock: NOT FOUND (assumed destroyed)\n");
        if (forced_bs)
            printf("using user-supplied block size: %u\n", forced_bs);

        /* ---- priority 4: foreign filesystem? hand off, else exit ------- */
        const char *fsck_prog = NULL;
        const char *fname = detect_foreign_fs(img, size, &fsck_prog);
        if (fname) {
            if (mapped) munmap(img, size); else free(img);
            close(fd);
            return dispatch_foreign_fs(fsck_prog, fname, do_fix, path);
        }

        /* ---- priority 2/3/5: carve, then rebuild as needed ------------- */
        size_t Z = head_zero_len(img, size);
        struct region *regions = NULL;
        size_t nregions = 0;
        uint32_t carve_bs = 0;
        mode_b_carve(img, size, forced_bs, threshold_pct,
                     &regions, &nregions, &carve_bs);

        if (regions && nregions && carve_bs) {
            uint64_t data_len = (uint64_t)size - Z;
            uint64_t nblk = data_len / carve_bs;
            uint64_t it_storage = ((nblk * FILE_OBJECT_ALIGN_SIZE + carve_bs - 1) /
                                   carve_bs) * carve_bs;
            uint64_t bmb = (((nblk + 7) / 8) + carve_bs - 1) / carve_bs;
            uint64_t dbo_formula = (uint64_t)carve_bs * 3 +
                                   it_storage + bmb * carve_bs;

            if (Z >= dbo_formula) {
                if (do_fix && !output_path) {
                    /* priority 2: enough head padding - rebuild in place */
                    printf("\n=== enough head padding - rebuilding metadata in place ===\n");
                    int r = rebuild_image(img, size, carve_bs, Z, regions, nregions,
                                          1, fd, NULL);
                    if (r != 0) {
                        free(regions);
                        if (mapped) munmap(img, size); else free(img);
                        close(fd);
                        return 8;
                    }
                } else if (output_path) {
                    printf("\n=== enough head padding - saving rebuilt copy ===\n");
                    int r = rebuild_image(img, size, carve_bs, Z, regions, nregions,
                                          0, -1, output_path);
                    if (r != 0) {
                        free(regions);
                        if (mapped) munmap(img, size); else free(img);
                        close(fd);
                        return 8;
                    }
                } else {
                    printf("\n[fsck] head padding (%zu bytes) is sufficient to rebuild "
                           "metadata in place; run with -f/--fix to write it back\n", Z);
                }
            } else {
                /* priority 3/5: no/insufficient head padding - new image */
                char auto_name[4096];
                const char *out = output_path;
                if (!out) {
                    snprintf(auto_name, sizeof(auto_name), "%s.fsck.img", path);
                    out = auto_name;
                }
                if (Z == 0)
                    printf("\n=== no head padding - rebuilding metadata, saving new image ===\n");
                else
                    printf("\n=== head padding too small - rebuilding metadata, saving new image ===\n");
                int r = rebuild_image(img, size, carve_bs, Z, regions, nregions,
                                      0, -1, out);
                if (r != 0) {
                    free(regions);
                    if (mapped) munmap(img, size); else free(img);
                    close(fd);
                    return 8;
                }
            }
        }
        free(regions);
    }

    if (mapped) munmap(img, size); else free(img);
    close(fd);
    printf("\n=== done (carving mode) ===\n");
    return 0;
}
