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
#include <time.h>
#include <errno.h>

#include "../../include/file_table.h"

#define FSCK_VERSION "0.1.0"
#define HD_SUPERBLOCK_OFFSET_POS 136
#define CANDIDATE_BLOCK_SIZES { 1024, 2048, 4096 }
#define DEFAULT_SPARSE_THRESHOLD_PCT 5

struct sb_view {
    int valid;
    uint32_t block_size;
    uint32_t block_count;
    uint64_t sb_offset;
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

static void print_sb_full(const unsigned char *s, uint32_t bs, uint32_t bc,
                          uint64_t size, uint64_t ito_recon)
{
    /* Layout-derived offsets (same formulas as mkfs), so that even when the
       offset fields in the superblock are corrupt we print the values infofs
       would print on an intact image. Raw values are always shown when they
       differ. */
    uint32_t total_inodes = rd_u32(s + 20);
    uint64_t it_storage = ((uint64_t)total_inodes * FILE_OBJECT_ALIGN_SIZE +
                           bs - 1) / bs * bs;
    uint64_t bmo_recon = ito_recon + it_storage;
    uint32_t bmb_recon = ((bc + 7) / 8 + bs - 1) / bs;
    uint64_t dbo_recon = bmo_recon + (uint64_t)bmb_recon * bs;
    uint64_t dbo_total_recon = (uint64_t)bc * bs;
    uint64_t dbo_end_recon = dbo_recon + dbo_total_recon;
    uint64_t unalloc_recon = size > dbo_end_recon ? size - dbo_end_recon : 0;

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
    print_off("Inode Table Offset", rd_u32(s + 40), ito_recon, size);
    print_off("Data Blocks Offset", rd_u32(s + 44), dbo_recon, size);
    print_off("Data Blocks Total Size", rd_u32(s + 48), dbo_total_recon, size);
    print_off("Data Blocks End Offset", rd_u32(s + 52), dbo_end_recon, size);
    print_off("Block Bitmap Offset", rd_u32(s + 56), bmo_recon, size);
    print_off("Block Bitmap Blocks", rd_u32(s + 60), bmb_recon, size);
    print_off("Unallocated Space Size", rd_u32(s + 64), unalloc_recon, size);
}

/* Count superblock offset fields whose raw value is corrupt (out of range or
   != reconstruction). Used by fsck to report "problems remain" when run
   without -f/--fix. */
static int count_bad_sb_fields(const unsigned char *s, uint32_t bs, uint32_t bc,
                               uint64_t size, uint64_t ito_recon)
{
    uint32_t total_inodes = rd_u32(s + 20);
    uint64_t it_storage = ((uint64_t)total_inodes * FILE_OBJECT_ALIGN_SIZE +
                           bs - 1) / bs * bs;
    uint64_t bmo_recon = ito_recon + it_storage;
    uint32_t bmb_recon = ((bc + 7) / 8 + bs - 1) / bs;
    uint64_t dbo_recon = bmo_recon + (uint64_t)bmb_recon * bs;
    uint64_t dbo_total_recon = (uint64_t)bc * bs;
    uint64_t dbo_end_recon = dbo_recon + dbo_total_recon;
    uint64_t unalloc_recon = size > dbo_end_recon ? size - dbo_end_recon : 0;

    struct { unsigned off; uint64_t recon; } fields[] = {
        { 40, ito_recon }, { 44, dbo_recon }, { 48, dbo_total_recon },
        { 52, dbo_end_recon }, { 56, bmo_recon }, { 60, bmb_recon },
        { 64, unalloc_recon },
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
    uint32_t total_inodes = rd_u32(s + 20);
    uint64_t it_storage = ((uint64_t)total_inodes * FILE_OBJECT_ALIGN_SIZE +
                           bs - 1) / bs * bs;
    uint64_t bmo_recon = ito_recon + it_storage;
    uint32_t bmb_recon = ((bc + 7) / 8 + bs - 1) / bs;
    uint64_t dbo_recon = bmo_recon + (uint64_t)bmb_recon * bs;
    uint64_t dbo_total_recon = (uint64_t)bc * bs;
    uint64_t dbo_end_recon = dbo_recon + dbo_total_recon;
    uint64_t unalloc_recon = size > dbo_end_recon ? size - dbo_end_recon : 0;

    struct { const char *name; unsigned off; uint64_t recon; } fields[] = {
        { "Inode Table Offset",     40, ito_recon },
        { "Data Blocks Offset",     44, dbo_recon },
        { "Data Blocks Total Size", 48, dbo_total_recon },
        { "Data Blocks End Offset", 52, dbo_end_recon },
        { "Block Bitmap Offset",    56, bmo_recon },
        { "Block Bitmap Blocks",    60, bmb_recon },
        { "Unallocated Space Size", 64, unalloc_recon },
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
                       const struct sb_view *sb, int do_fix, int fd)
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
    int sb_problems = 0;
    if (sb->valid) {
        print_sb_full(img + sb->sb_offset, bs, bc, size, ito_recon);
        if (do_fix) {
            int fixed = fix_sb(fd, img + sb->sb_offset, bs, bc, size,
                               ito_recon, sb->sb_offset);
            if (fixed < 0)
                return -1;
            printf("  [fsck] superblock offset fields: %d inconsistent -> written back\n",
                   fixed);
        } else {
            sb_problems = count_bad_sb_fields(img + sb->sb_offset, bs, bc,
                                              size, ito_recon);
            if (sb_problems)
                printf("  [fsck] %d superblock offset field(s) inconsistent; "
                       "re-run with -f/--fix to repair\n", sb_problems);
        }
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

static int mode_b_carve(const unsigned char *img, size_t size,
                        uint32_t forced_bs, int threshold_pct)
{
    uint32_t bs = forced_bs;
    if (!bs) bs = guess_block_size(img, size, NULL);
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
                   start == 0 ? "  <- possible boot/padding area" : "");
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
    return 0;
}

/* ---------------------------------------------------------------------- */

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [-b blocksize] [-c] [-t pct] [-f|--fix] [-v|--version]\n"
            "            <image|blockdev>\n"
            "  -b <bs>       force block size (default: auto-detect 1024/2048/4096)\n"
            "  -c            force carving mode even if metadata is readable\n"
            "  -t <pct>      sparse threshold percent 1..50 (default 5)\n"
            "  -f, --fix     write reconstructed superblock offset fields back\n"
            "                to the device/image (repair mode)\n"
            "  -v, --version print version and exit\n"
            "exit status: 0 = clean or fixed, 1 = problems remain, 8 = I/O error,\n"
            "             16 = usage error\n",
            prog);
}

int main(int argc, char **argv)
{
    const char *path = NULL;
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
    if (sb.valid) {
        printf("hidden data: found (0x55AA..0xAA55 ok)\n");
        printf("superblock  : offset %llu, block_size=%u, block_count=%u (YUKI ok)\n",
               (unsigned long long)sb.sb_offset, sb.block_size, sb.block_count);
        if (!force_carve) {
            int rc = mode_a_scan(img, size, &sb, do_fix, fd);
            if (rc < 0) { /* fix write failed */
                if (mapped) munmap(img, size); else free(img);
                close(fd);
                return 8;
            }
            if (mapped) munmap(img, size); else free(img);
            if (do_fix) {
                fdatasync(fd);
                printf("\n=== done (metadata mode, repaired) ===\n");
            } else {
                printf("\n=== done (metadata mode) ===\n");
            }
            close(fd);
            /* fsck exit status: 1 if problems were found but not fixed */
            return rc ? 1 : 0;
        }
        printf("(-c forced) falling through to carving mode anyway\n");
    } else {
        printf("hidden data / superblock: NOT FOUND (assumed destroyed)\n");
        if (forced_bs)
            printf("using user-supplied block size: %u\n", forced_bs);
    }

    mode_b_carve(img, size, forced_bs, threshold_pct);

    if (mapped) munmap(img, size); else free(img);
    close(fd);
    printf("\n=== done (carving mode) ===\n");
    return 0;
}
