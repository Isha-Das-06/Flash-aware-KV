/*
 * fkv - flash-aware, power-loss-safe key-value store.
 *
 * On-flash layout of every block (all integers little-endian):
 *
 *   0  header A  magic u32 | version u16 | 0xFFFF | erase_count u32 | crc u32
 *                written right after the block is erased
 *   16 header B  seq u32 | src_seq u32 | 0xFFFFFFFF | crc u32
 *                written when the block is committed; src_seq names the block
 *                this one was compacted from (0xFFFFFFFF if none)
 *   32 header C  16 bytes, 0xFF while valid; programmed to 0x00 to retire the
 *                block before it is erased
 *   48 records   type u8 | klen u8 | vlen u16 | data_crc u32 | hdr_crc u32 |
 *                key | value | 0xFF padding to prog_size
 *
 * See docs/DESIGN.md for why each step is crash safe.
 */
#include "fkv.h"

#include <string.h>

#define MAGIC 0x564B4C46u /* "FLKV" */
#define FMT_VERSION 1u

#define OFF_A 0u
#define OFF_B 16u
#define OFF_C 32u
#define OFF_DATA FKV_BLOCK_HEADER_SIZE
#define HDR_FIELD 16u

#define NONE 0xFFFFFFFFu

#define REC_PUT 0x50u
#define REC_DEL 0x44u

enum { BS_FREE = 0, BS_DIRTY = 1, BS_ACTIVE = 2 };

#define SLOT_USED 1u
#define SLOT_TOMB 2u

/* Result of parsing a record header. */
enum { REC_OK, REC_BAD_DATA, REC_END_BLANK, REC_END_TORN };

struct rec {
    uint32_t len;
    uint32_t dcrc;
    uint16_t vlen;
    uint8_t type;
    uint8_t klen;
};

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* CRC-32 (IEEE 802.3), nibble-table variant: 64 bytes of table. */
static const uint32_t crc_tab[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u,
    0x4DB26158u, 0x5005713Cu, 0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

static uint32_t crc_update(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ crc_tab[crc & 15u];
        crc = (crc >> 4) ^ crc_tab[crc & 15u];
    }
    return crc;
}

static uint32_t crc32(const void *data, size_t len)
{
    return ~crc_update(0xFFFFFFFFu, data, len);
}

/* FNV-1a, used only to place keys in the RAM index. */
#define FNV_INIT 2166136261u

static uint32_t fnv_update(uint32_t h, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len--) {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}

static bool all_ff(const uint8_t *p, size_t len)
{
    while (len--)
        if (*p++ != 0xFFu)
            return false;
    return true;
}

static uint32_t rec_size(const fkv_t *db, uint32_t klen, uint32_t vlen)
{
    return FKV_RECORD_SIZE(klen, vlen, db->flash->prog_size);
}

/* ------------------------------------------------------------------------ */
/* Flash access. Any driver failure marks the handle broken: RAM state may   */
/* no longer match flash, so only a remount can continue.                    */
/* ------------------------------------------------------------------------ */

static int io_read(fkv_t *db, uint32_t b, uint32_t off, void *buf, uint32_t len)
{
    if (len == 0)
        return FKV_OK;
    if (db->flash->read(db->flash->ctx, b, off, buf, len) != 0) {
        db->broken = true;
        return FKV_ERR_IO;
    }
    return FKV_OK;
}

static int io_prog(fkv_t *db, uint32_t b, uint32_t off, const void *buf, uint32_t len)
{
    db->st.flash_bytes += len;
    if (db->flash->prog(db->flash->ctx, b, off, buf, len) != 0) {
        db->broken = true;
        return FKV_ERR_IO;
    }
    return FKV_OK;
}

static int io_erase(fkv_t *db, uint32_t b)
{
    db->st.erases++;
    if (db->flash->erase(db->flash->ctx, b) != 0) {
        db->broken = true;
        return FKV_ERR_IO;
    }
    return FKV_OK;
}

#define CHUNK 32u

/* The following stream a flash range through a small stack buffer so that
 * validating a record never needs RAM proportional to its size. */

static int io_crc(fkv_t *db, uint32_t b, uint32_t off, uint32_t len, uint32_t *crc)
{
    uint8_t buf[CHUNK];
    while (len) {
        uint32_t n = len < CHUNK ? len : CHUNK;
        int err = io_read(db, b, off, buf, n);
        if (err)
            return err;
        *crc = crc_update(*crc, buf, n);
        off += n;
        len -= n;
    }
    return FKV_OK;
}

static int io_hash(fkv_t *db, uint32_t b, uint32_t off, uint32_t len, uint32_t *h)
{
    uint8_t buf[CHUNK];
    while (len) {
        uint32_t n = len < CHUNK ? len : CHUNK;
        int err = io_read(db, b, off, buf, n);
        if (err)
            return err;
        *h = fnv_update(*h, buf, n);
        off += n;
        len -= n;
    }
    return FKV_OK;
}

static int io_cmp(fkv_t *db, uint32_t b, uint32_t off, const void *mem, uint32_t len, bool *eq)
{
    uint8_t buf[CHUNK];
    const uint8_t *m = mem;
    *eq = false;
    while (len) {
        uint32_t n = len < CHUNK ? len : CHUNK;
        int err = io_read(db, b, off, buf, n);
        if (err)
            return err;
        if (memcmp(buf, m, n) != 0)
            return FKV_OK;
        m += n;
        off += n;
        len -= n;
    }
    *eq = true;
    return FKV_OK;
}

static int io_blank(fkv_t *db, uint32_t b, uint32_t off, uint32_t len, bool *blank)
{
    uint8_t buf[CHUNK];
    *blank = false;
    while (len) {
        uint32_t n = len < CHUNK ? len : CHUNK;
        int err = io_read(db, b, off, buf, n);
        if (err)
            return err;
        if (!all_ff(buf, n))
            return FKV_OK;
        off += n;
        len -= n;
    }
    *blank = true;
    return FKV_OK;
}

/* ------------------------------------------------------------------------ */
/* Block headers                                                             */
/* ------------------------------------------------------------------------ */

struct hdr {
    bool a_ok;
    uint32_t ec;
    int b; /* 0: erased, 1: valid, -1: torn */
    uint32_t seq;
    uint32_t src;
    bool retired;
};

static int hdr_read(fkv_t *db, uint32_t b, struct hdr *h)
{
    uint8_t raw[FKV_BLOCK_HEADER_SIZE];
    int err = io_read(db, b, 0, raw, sizeof raw);
    if (err)
        return err;

    memset(h, 0, sizeof *h);
    const uint8_t *a = raw + OFF_A;
    h->a_ok = get32(a) == MAGIC && get16(a + 4) == FMT_VERSION && get32(a + 12) == crc32(a, 12);
    h->ec = get32(a + 8);

    const uint8_t *bb = raw + OFF_B;
    if (all_ff(bb, HDR_FIELD)) {
        h->b = 0;
    } else if (get32(bb + 12) == crc32(bb, 12)) {
        h->b = 1;
        h->seq = get32(bb);
        h->src = get32(bb + 4);
    } else {
        h->b = -1;
    }

    /* Any programmed bit retires the block, so a torn retire still counts. */
    h->retired = !all_ff(raw + OFF_C, HDR_FIELD);
    return FKV_OK;
}

static int hdr_write_a(fkv_t *db, uint32_t b, uint32_t ec)
{
    uint8_t h[HDR_FIELD];
    put32(h, MAGIC);
    put16(h + 4, FMT_VERSION);
    put16(h + 6, 0xFFFFu);
    put32(h + 8, ec);
    put32(h + 12, crc32(h, 12));
    return io_prog(db, b, OFF_A, h, sizeof h);
}

static int hdr_write_b(fkv_t *db, uint32_t b, uint32_t seq, uint32_t src)
{
    uint8_t h[HDR_FIELD];
    put32(h, seq);
    put32(h + 4, src);
    put32(h + 8, 0xFFFFFFFFu);
    put32(h + 12, crc32(h, 12));
    return io_prog(db, b, OFF_B, h, sizeof h);
}

static int hdr_retire(fkv_t *db, uint32_t b)
{
    uint8_t h[HDR_FIELD];
    memset(h, 0, sizeof h);
    return io_prog(db, b, OFF_C, h, sizeof h);
}

/* ------------------------------------------------------------------------ */
/* Records                                                                   */
/* ------------------------------------------------------------------------ */

/* Parse the header at (b, off). With check_data, also verify the data CRC. */
static int rec_parse(fkv_t *db, uint32_t b, uint32_t off, bool check_data, struct rec *r,
                     int *status)
{
    uint8_t h[FKV_RECORD_HEADER_SIZE];
    uint32_t bs = db->flash->block_size;

    if (off + FKV_RECORD_HEADER_SIZE > bs) {
        *status = REC_END_BLANK;
        return FKV_OK;
    }
    int err = io_read(db, b, off, h, sizeof h);
    if (err)
        return err;
    if (all_ff(h, sizeof h)) {
        *status = REC_END_BLANK;
        return FKV_OK;
    }
    if (get32(h + 8) != crc32(h, 8)) {
        *status = REC_END_TORN;
        return FKV_OK;
    }
    r->type = h[0];
    r->klen = h[1];
    r->vlen = (uint16_t)get16(h + 2);
    r->dcrc = get32(h + 4);
    if ((r->type != REC_PUT && r->type != REC_DEL) || r->klen == 0 ||
        (r->type == REC_DEL && r->vlen != 0)) {
        *status = REC_END_TORN;
        return FKV_OK;
    }
    r->len = rec_size(db, r->klen, r->vlen);
    if (off + r->len > bs) {
        *status = REC_END_TORN;
        return FKV_OK;
    }
    *status = REC_OK;
    if (check_data) {
        uint32_t crc = 0xFFFFFFFFu;
        err = io_crc(db, b, off + FKV_RECORD_HEADER_SIZE, (uint32_t)r->klen + r->vlen, &crc);
        if (err)
            return err;
        if (~crc != r->dcrc)
            *status = REC_BAD_DATA;
    }
    return FKV_OK;
}

/* Build a record in db->cfg.scratch; returns its on-flash size. */
static uint32_t rec_build(fkv_t *db, uint8_t type, const void *key, uint32_t klen,
                          const void *val, uint32_t vlen)
{
    uint8_t *s = db->cfg.scratch;
    uint32_t len = rec_size(db, klen, vlen);

    memcpy(s + FKV_RECORD_HEADER_SIZE, key, klen);
    if (vlen)
        memcpy(s + FKV_RECORD_HEADER_SIZE + klen, val, vlen);
    memset(s + FKV_RECORD_HEADER_SIZE + klen + vlen, 0xFF,
           len - FKV_RECORD_HEADER_SIZE - klen - vlen);
    s[0] = type;
    s[1] = (uint8_t)klen;
    put16(s + 2, vlen);
    put32(s + 4, crc32(s + FKV_RECORD_HEADER_SIZE, klen + vlen));
    put32(s + 8, crc32(s, 8));
    return len;
}

/* Check a record already loaded into scratch. */
static bool rec_valid_in_scratch(const fkv_t *db, uint32_t len)
{
    const uint8_t *s = db->cfg.scratch;
    uint32_t klen = s[1], vlen = get16(s + 2);
    return get32(s + 8) == crc32(s, 8) && FKV_RECORD_HEADER_SIZE + klen + vlen <= len &&
           get32(s + 4) == crc32(s + FKV_RECORD_HEADER_SIZE, klen + vlen);
}

/* ------------------------------------------------------------------------ */
/* Key index: open addressing with linear probing                            */
/* ------------------------------------------------------------------------ */

/* Find key. On miss, *idx is the empty slot where it would be inserted. */
static int slot_find(fkv_t *db, const void *key, uint32_t klen, uint32_t hash, uint32_t *idx,
                     bool *found)
{
    struct fkv_slot *sl = db->cfg.slots;
    uint32_t i = hash & db->slot_mask;

    *found = false;
    while (sl[i].flags & SLOT_USED) {
        if (sl[i].hash == hash && sl[i].klen == klen) {
            bool eq;
            int err = io_cmp(db, sl[i].block, sl[i].off + FKV_RECORD_HEADER_SIZE, key, klen, &eq);
            if (err)
                return err;
            if (eq) {
                *found = true;
                *idx = i;
                return FKV_OK;
            }
        }
        i = (i + 1) & db->slot_mask;
    }
    *idx = i;
    return FKV_OK;
}

/* Find the slot that points exactly at (b, off), if any. */
static int slot_at(fkv_t *db, uint32_t b, uint32_t off, const struct rec *r, uint32_t *idx)
{
    struct fkv_slot *sl = db->cfg.slots;
    uint32_t h = FNV_INIT;
    int err = io_hash(db, b, off + FKV_RECORD_HEADER_SIZE, r->klen, &h);
    if (err)
        return err;

    uint32_t i = h & db->slot_mask;
    *idx = NONE;
    while (sl[i].flags & SLOT_USED) {
        if (sl[i].block == b && sl[i].off == off) {
            *idx = i;
            break;
        }
        i = (i + 1) & db->slot_mask;
    }
    return FKV_OK;
}

/* Backward-shift deletion keeps probe chains intact without tombstones. */
static void slot_remove(fkv_t *db, uint32_t i)
{
    struct fkv_slot *sl = db->cfg.slots;
    uint32_t mask = db->slot_mask, j = i;

    for (;;) {
        j = (j + 1) & mask;
        if (!(sl[j].flags & SLOT_USED))
            break;
        uint32_t k = sl[j].hash & mask;
        bool stays = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
        if (stays)
            continue;
        sl[i] = sl[j];
        i = j;
    }
    memset(&sl[i], 0, sizeof sl[i]);
    db->slot_used--;
}

/* Drop the index's reference to its current record for slot i. */
static void slot_release(fkv_t *db, uint32_t i)
{
    struct fkv_slot *s = &db->cfg.slots[i];
    db->cfg.blocks[s->block].live -= s->len;
    db->live_bytes -= s->len;
}

static void slot_point(fkv_t *db, uint32_t i, uint32_t b, uint32_t off, uint32_t len, bool tomb)
{
    struct fkv_slot *s = &db->cfg.slots[i];
    s->block = (uint16_t)b;
    s->off = off;
    s->len = len;
    s->flags = (uint8_t)(SLOT_USED | (tomb ? SLOT_TOMB : 0u));
    db->cfg.blocks[b].live += len;
    db->live_bytes += len;
}

/* Record (b, off) supersedes whatever the index held for its key. The key
 * must already be in scratch. */
static int index_apply(fkv_t *db, uint32_t b, uint32_t off, const struct rec *r)
{
    const uint8_t *key = db->cfg.scratch;
    uint32_t h = fnv_update(FNV_INIT, key, r->klen), idx;
    bool found;
    int err = slot_find(db, key, r->klen, h, &idx, &found);
    if (err)
        return err;
    if (found) {
        slot_release(db, idx);
    } else {
        if (db->slot_used >= db->slot_limit)
            return FKV_ERR_NOMEM;
        db->slot_used++;
        db->cfg.slots[idx].hash = h;
        db->cfg.slots[idx].klen = r->klen;
    }
    slot_point(db, idx, b, off, r->len, r->type == REC_DEL);
    return FKV_OK;
}

/* ------------------------------------------------------------------------ */
/* Block management                                                          */
/* ------------------------------------------------------------------------ */

/* Erase a block and give it a fresh header A. */
static int block_reset(fkv_t *db, uint32_t b)
{
    struct fkv_block *bl = &db->cfg.blocks[b];
    uint32_t ec = bl->ec + 1;
    int err = io_erase(db, b);
    if (err)
        return err;
    err = hdr_write_a(db, b, ec);
    if (err)
        return err;
    bl->ec = ec;
    bl->ec_known = 1;
    bl->state = BS_FREE;
    bl->verified = 1;
    bl->seq = 0;
    bl->end = OFF_DATA;
    bl->live = 0;
    bl->sealed = 0;
    return FKV_OK;
}

/* Pick a reusable block (least worn, or most worn for cold data) and make
 * sure it is fully erased. Does not change nfree: that happens on commit. */
static int block_alloc(fkv_t *db, bool most_worn, uint32_t *out)
{
    struct fkv_block *bl = db->cfg.blocks;
    uint32_t best = NONE;

    for (uint32_t b = 0; b < db->flash->block_count; b++) {
        if (bl[b].state == BS_ACTIVE)
            continue;
        if (best == NONE || (most_worn ? bl[b].ec > bl[best].ec : bl[b].ec < bl[best].ec))
            best = b;
    }
    if (best == NONE)
        return FKV_ERR_NOSPC;

    if (bl[best].state == BS_FREE && !bl[best].verified) {
        bool blank;
        int err = io_blank(db, best, OFF_B, db->flash->block_size - OFF_B, &blank);
        if (err)
            return err;
        bl[best].verified = blank;
    }
    if (bl[best].state == BS_DIRTY || !bl[best].verified) {
        int err = block_reset(db, best);
        if (err)
            return err;
    }
    *out = best;
    return FKV_OK;
}

/* Writing header B is the commit point of a block. */
static int block_commit(fkv_t *db, uint32_t b, uint32_t src, uint32_t end, uint32_t live)
{
    struct fkv_block *bl = &db->cfg.blocks[b];
    int err = hdr_write_b(db, b, db->next_seq, src);
    if (err)
        return err;
    bl->state = BS_ACTIVE;
    bl->seq = db->next_seq++;
    bl->end = end;
    bl->live = live;
    bl->sealed = 0;
    db->nfree--;
    return FKV_OK;
}

static void set_head(fkv_t *db, uint32_t b)
{
    if (db->head != NONE && db->head != b)
        db->cfg.blocks[db->head].sealed = 1;
    db->head = b;
}

static int open_head(fkv_t *db)
{
    uint32_t b;
    int err = block_alloc(db, false, &b);
    if (err)
        return err;
    err = block_commit(db, b, NONE, OFF_DATA, 0);
    if (err)
        return err;
    set_head(db, b);
    return FKV_OK;
}

/* A tombstone in block v may be dropped only if no older block could still
 * hold a record for the same key; otherwise that record would come back to
 * life once v is erased. The key is in scratch. */
static int tomb_droppable(fkv_t *db, uint32_t v, uint32_t klen, bool *droppable)
{
    struct fkv_block *bl = db->cfg.blocks;
    const uint8_t *key = db->cfg.scratch + FKV_RECORD_HEADER_SIZE;

    *droppable = false;
    for (uint32_t b = 0; b < db->flash->block_count; b++) {
        if (b == v || bl[b].state != BS_ACTIVE || bl[b].seq > bl[v].seq)
            continue;
        uint32_t pos = OFF_DATA;
        while (pos < bl[b].end) {
            struct rec r;
            int st, err = rec_parse(db, b, pos, false, &r, &st);
            if (err)
                return err;
            if (st != REC_OK)
                break;
            if (r.klen == klen) {
                bool eq;
                err = io_cmp(db, b, pos + FKV_RECORD_HEADER_SIZE, key, klen, &eq);
                if (err)
                    return err;
                if (eq)
                    return FKV_OK;
            }
            pos += r.len;
        }
    }
    *droppable = true;
    return FKV_OK;
}

/*
 * Copy the live records of block v into a fresh block d, commit d (it names
 * v as its source), then retire and erase v. A crash before the commit
 * leaves d uncommitted (garbage) and v intact; a crash after it leaves a
 * committed d whose source v is retired at the next mount.
 */
static int compact(fkv_t *db, uint32_t v, bool wear_level)
{
    struct fkv_block *bl = db->cfg.blocks;
    uint32_t d, out = OFF_DATA, dlive = 0;
    int err = block_alloc(db, wear_level, &d);
    if (err)
        return err;

    for (uint32_t pos = OFF_DATA; pos < bl[v].end;) {
        struct rec r;
        uint32_t idx;
        int st;
        err = rec_parse(db, v, pos, false, &r, &st);
        if (err)
            return err;
        if (st != REC_OK)
            break;
        err = slot_at(db, v, pos, &r, &idx);
        if (err)
            return err;
        if (idx == NONE) { /* superseded */
            pos += r.len;
            continue;
        }

        err = io_read(db, v, pos, db->cfg.scratch, r.len);
        if (err)
            return err;
        bool keep = rec_valid_in_scratch(db, r.len);
        if (!keep) {
            db->st.corrupt_records++;
        } else if (r.type == REC_DEL) {
            bool drop;
            err = tomb_droppable(db, v, r.klen, &drop);
            if (err)
                return err;
            keep = !drop;
        }

        slot_release(db, idx);
        if (keep) {
            err = io_prog(db, d, out, db->cfg.scratch, r.len);
            if (err)
                return err;
            db->cfg.slots[idx].block = (uint16_t)d;
            db->cfg.slots[idx].off = out;
            db->live_bytes += r.len;
            dlive += r.len;
            out += r.len;
        } else {
            slot_remove(db, idx);
        }
        pos += r.len;
    }

    err = block_commit(db, d, bl[v].seq, out, dlive);
    if (err)
        return err;
    set_head(db, d);

    err = hdr_retire(db, v);
    if (err)
        return err;
    bl[v].state = BS_DIRTY;
    bl[v].live = 0;
    db->nfree++;
    err = block_reset(db, v);
    if (err)
        return err;

    if (wear_level)
        db->st.wl_moves++;
    else
        db->st.gc_runs++;
    return FKV_OK;
}

/* Choose the block to compact so that at least `need` bytes become free. */
static uint32_t pick_victim(const fkv_t *db, uint32_t need)
{
    const struct fkv_block *bl = db->cfg.blocks;
    uint64_t best_score = 0;
    uint32_t best = NONE;

    for (uint32_t b = 0; b < db->flash->block_count; b++) {
        if (bl[b].state != BS_ACTIVE)
            continue;
        uint32_t reclaim = db->payload - bl[b].live;
        if (reclaim < need)
            continue;
        uint64_t score;
        if (db->cfg.gc_policy == FKV_GC_COST_BENEFIT) {
            uint64_t age = (uint64_t)(db->next_seq - bl[b].seq);
            score = ((uint64_t)reclaim * age << 8) / (db->payload + bl[b].live);
        } else {
            score = reclaim;
        }
        if (best == NONE || score > best_score || (score == best_score && bl[b].ec < bl[best].ec)) {
            best = b;
            best_score = score;
        }
    }
    return best;
}

/* Static wear leveling: the least-worn block that holds data, if the spread
 * between it and the most-worn block exceeds the threshold. */
static uint32_t wl_candidate(const fkv_t *db)
{
    const struct fkv_block *bl = db->cfg.blocks;
    uint32_t max_ec = 0, cold = NONE;

    for (uint32_t b = 0; b < db->flash->block_count; b++) {
        if (bl[b].ec > max_ec)
            max_ec = bl[b].ec;
        if (bl[b].state == BS_ACTIVE && b != db->head && (cold == NONE || bl[b].ec < bl[cold].ec))
            cold = b;
    }
    if (cold == NONE || max_ec - bl[cold].ec <= db->cfg.wl_threshold)
        return NONE;
    return cold;
}

/*
 * Make sure the head block can take `need` more bytes.
 *
 * Invariant: at least one block is free (or reclaimable by erase) whenever
 * the store is at rest, so compaction always has a destination. A new head
 * is therefore only opened while two or more blocks are free; otherwise a
 * block is compacted. The capacity limit enforced in write_record() makes a
 * suitable victim always exist (see docs/DESIGN.md).
 */
static int make_room(fkv_t *db, uint32_t need)
{
    bool wl_done = db->cfg.wl_threshold == 0;

    for (int iter = 0; iter < 8; iter++) {
        if (db->head != NONE) {
            const struct fkv_block *h = &db->cfg.blocks[db->head];
            if (!h->sealed && db->flash->block_size - h->end >= need)
                return FKV_OK;
        }
        int err;
        if (!wl_done) {
            wl_done = true;
            uint32_t cold = wl_candidate(db);
            if (cold != NONE && db->nfree >= 1) {
                err = compact(db, cold, true);
                if (err)
                    return err;
                continue;
            }
        }
        if (db->nfree >= 2) {
            err = open_head(db);
        } else {
            uint32_t v = db->nfree ? pick_victim(db, need) : NONE;
            if (v == NONE)
                return FKV_ERR_NOSPC;
            err = compact(db, v, false);
        }
        if (err)
            return err;
    }
    return FKV_ERR_NOSPC;
}

/* ------------------------------------------------------------------------ */
/* Configuration                                                             */
/* ------------------------------------------------------------------------ */

static int check_flash(const struct fkv_flash *f)
{
    if (!f || !f->read || !f->prog || !f->erase)
        return FKV_ERR_INVAL;
    uint32_t p = f->prog_size;
    if (p != 1 && p != 2 && p != 4 && p != 8 && p != 16)
        return FKV_ERR_INVAL;
    if (f->block_count < 2 || f->block_count > 0xFFFFu)
        return FKV_ERR_INVAL;
    if (f->block_size % p != 0 || f->block_size < OFF_DATA + 64u)
        return FKV_ERR_INVAL;
    return FKV_OK;
}

static int check_config(const struct fkv_config *c)
{
    if (!c)
        return FKV_ERR_INVAL;
    int err = check_flash(c->flash);
    if (err)
        return err;
    if (c->max_key_len < 1 || c->max_key_len > FKV_MAX_KEY_LEN || c->max_value_len > 0xFFFFu)
        return FKV_ERR_INVAL;
    if (c->gc_policy != FKV_GC_GREEDY && c->gc_policy != FKV_GC_COST_BENEFIT)
        return FKV_ERR_INVAL;
    if (!c->blocks || !c->slots || !c->scratch)
        return FKV_ERR_INVAL;
    if (c->slot_count < 4 || (c->slot_count & (c->slot_count - 1)) != 0)
        return FKV_ERR_INVAL;
    uint32_t max_rec = FKV_RECORD_SIZE(c->max_key_len, c->max_value_len, c->flash->prog_size);
    if (c->scratch_size < max_rec)
        return FKV_ERR_INVAL;
    /* Each block must hold at least two maximum-size records, or capacity
     * would be mostly lost to fragmentation. */
    if (2u * max_rec > c->flash->block_size - OFF_DATA)
        return FKV_ERR_INVAL;
    return FKV_OK;
}

/* ------------------------------------------------------------------------ */
/* Public API                                                                */
/* ------------------------------------------------------------------------ */

int fkv_format(const struct fkv_config *cfg)
{
    if (!cfg)
        return FKV_ERR_INVAL;
    int err = check_flash(cfg->flash);
    if (err)
        return err;

    fkv_t tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.flash = cfg->flash;

    /* Keep wear history: blocks whose header survived keep their count;
     * unreadable ones are assumed as worn as the most-worn known block. */
    uint32_t n = cfg->flash->block_count, max_ec = 0;
    for (uint32_t b = 0; b < n; b++) {
        struct hdr h;
        err = hdr_read(&tmp, b, &h);
        if (err)
            return err;
        if (h.a_ok && h.ec > max_ec)
            max_ec = h.ec;
    }
    for (uint32_t b = 0; b < n; b++) {
        struct hdr h;
        err = hdr_read(&tmp, b, &h);
        if (err)
            return err;
        err = io_erase(&tmp, b);
        if (err)
            return err;
        err = hdr_write_a(&tmp, b, (h.a_ok ? h.ec : max_ec) + 1);
        if (err)
            return err;
    }
    return FKV_OK;
}

/* Scan the records of an ACTIVE block into the index. */
static int scan_block(fkv_t *db, uint32_t b)
{
    uint32_t pos = OFF_DATA;
    for (;;) {
        struct rec r;
        int st, err = rec_parse(db, b, pos, true, &r, &st);
        if (err)
            return err;
        if (st == REC_END_BLANK || st == REC_END_TORN)
            break;
        if (r.len > db->cfg.scratch_size)
            return FKV_ERR_INVAL; /* written with a larger configuration */
        if (st == REC_BAD_DATA) {
            /* Torn or rotted payload with an intact header: skip it. The
             * previous version of the key (if any) stays current. */
            db->st.corrupt_records++;
        } else {
            err = io_read(db, b, pos + FKV_RECORD_HEADER_SIZE, db->cfg.scratch, r.klen);
            if (err)
                return err;
            err = index_apply(db, b, pos, &r);
            if (err)
                return err;
        }
        pos += r.len;
    }
    db->cfg.blocks[b].end = pos;
    return FKV_OK;
}

int fkv_mount(fkv_t *db, const struct fkv_config *cfg)
{
    if (!db)
        return FKV_ERR_INVAL;
    memset(db, 0, sizeof *db);
    int err = check_config(cfg);
    if (err)
        return err;

    db->cfg = *cfg;
    db->flash = cfg->flash;
    db->payload = db->flash->block_size - OFF_DATA;
    db->max_rec = FKV_RECORD_SIZE(cfg->max_key_len, cfg->max_value_len, db->flash->prog_size);
    db->capacity = (db->flash->block_count - 1) * (db->payload - db->max_rec);
    db->slot_mask = cfg->slot_count - 1;
    db->slot_limit = cfg->slot_count / 4 * 3;
    db->head = NONE;
    memset(cfg->slots, 0, sizeof *cfg->slots * cfg->slot_count);

    uint32_t n = db->flash->block_count;
    struct fkv_block *bl = cfg->blocks;
    uint32_t max_seq = 0, max_ec = 0;
    bool any = false;

    /* 1. Classify every block from its header. */
    for (uint32_t b = 0; b < n; b++) {
        struct hdr h;
        err = hdr_read(db, b, &h);
        if (err)
            return err;
        memset(&bl[b], 0, sizeof bl[b]);
        bl[b].end = OFF_DATA;
        bl[b].state = BS_DIRTY;
        if (h.b == 1 && h.seq + 1 > max_seq)
            max_seq = h.seq + 1;
        if (!h.a_ok)
            continue;
        any = true;
        bl[b].ec = h.ec;
        bl[b].ec_known = 1;
        if (h.ec > max_ec)
            max_ec = h.ec;
        if (h.retired || h.b == -1)
            continue;
        if (h.b == 0) {
            bl[b].state = BS_FREE; /* erased-ness verified lazily on allocation */
        } else {
            bl[b].state = BS_ACTIVE;
            bl[b].seq = h.seq;
            bl[b].live = h.src; /* stash src_seq until step 2 */
        }
    }
    if (!any)
        return FKV_ERR_NOFS;
    for (uint32_t b = 0; b < n; b++)
        if (!bl[b].ec_known)
            bl[b].ec = max_ec;
    db->next_seq = max_seq;

    /* 2. A committed compaction whose source was not yet retired: finish the
     *    job. The source is redundant, and reading it could resurrect keys
     *    whose tombstones were dropped during the compaction. */
    for (uint32_t d = 0; d < n; d++) {
        if (bl[d].state != BS_ACTIVE || bl[d].live == NONE)
            continue;
        for (uint32_t v = 0; v < n; v++) {
            if (v != d && bl[v].state == BS_ACTIVE && bl[v].seq == bl[d].live &&
                bl[v].seq < bl[d].seq) {
                err = hdr_retire(db, v);
                if (err)
                    return err;
                bl[v].state = BS_DIRTY;
                db->st.recovered_blocks++;
            }
        }
    }
    for (uint32_t b = 0; b < n; b++)
        bl[b].live = 0;

    /* 3. Replay active blocks oldest-first; later records win. O(n^2) in the
     *    block count but needs no extra memory. */
    uint32_t last = 0;
    bool first = true;
    for (;;) {
        uint32_t next = NONE;
        for (uint32_t b = 0; b < n; b++) {
            if (bl[b].state != BS_ACTIVE || (!first && bl[b].seq <= last))
                continue;
            if (next == NONE || bl[b].seq < bl[next].seq)
                next = b;
        }
        if (next == NONE)
            break;
        err = scan_block(db, next);
        if (err)
            return err;
        bl[next].sealed = 1;
        db->head = next;
        last = bl[next].seq;
        first = false;
    }

    /* 4. The newest block is the head. It can be appended to only if every
     *    byte after its last record is still erased (no torn write). */
    if (db->head != NONE) {
        bool blank;
        struct fkv_block *h = &bl[db->head];
        err = io_blank(db, db->head, h->end, db->flash->block_size - h->end, &blank);
        if (err)
            return err;
        h->sealed = blank ? 0 : 1;
    }
    for (uint32_t b = 0; b < n; b++)
        if (bl[b].state != BS_ACTIVE)
            db->nfree++;

    db->mounted = true;
    return FKV_OK;
}

int fkv_unmount(fkv_t *db)
{
    if (!db)
        return FKV_ERR_INVAL;
    db->mounted = false;
    return FKV_OK;
}

static int check_handle(const fkv_t *db)
{
    if (!db || !db->mounted)
        return FKV_ERR_INVAL;
    if (db->broken)
        return FKV_ERR_IO;
    return FKV_OK;
}

static bool any_tombstones(const fkv_t *db)
{
    for (uint32_t i = 0; i < db->cfg.slot_count; i++)
        if (db->cfg.slots[i].flags & SLOT_TOMB)
            return true;
    return false;
}

static uint32_t oldest_active(const fkv_t *db)
{
    const struct fkv_block *bl = db->cfg.blocks;
    uint32_t best = NONE;
    for (uint32_t b = 0; b < db->flash->block_count; b++)
        if (bl[b].state == BS_ACTIVE && (best == NONE || bl[b].seq < bl[best].seq))
            best = b;
    return best;
}

/*
 * Tombstones occupy space and index slots until compaction proves they are
 * no longer needed. When a write is refused for lack of either, compact the
 * log oldest-first: a tombstone in the oldest block can always be dropped,
 * so one pass over the log reclaims every tombstone whose key is gone.
 */
static int write_record(fkv_t *db, uint8_t type, const void *key, uint32_t klen, const void *val,
                        uint32_t vlen)
{
    uint32_t h = fnv_update(FNV_INIT, key, klen), idx, len = rec_size(db, klen, vlen);
    bool found;
    int err;

    for (uint32_t pass = 0;; pass++) {
        err = slot_find(db, key, klen, h, &idx, &found);
        if (err)
            return err;
        if (type == REC_DEL && (!found || (db->cfg.slots[idx].flags & SLOT_TOMB)))
            return FKV_ERR_NOTFOUND;
        uint32_t old = found ? db->cfg.slots[idx].len : 0;
        bool no_space = db->live_bytes - old + len > db->capacity;
        bool no_slot = !found && db->slot_used >= db->slot_limit;
        if (!no_space && !no_slot)
            break;
        if (pass >= db->flash->block_count || db->nfree < 1 || !any_tombstones(db))
            return no_space ? FKV_ERR_NOSPC : FKV_ERR_NOMEM;
        err = compact(db, oldest_active(db), false);
        if (err)
            return err;
    }

    bool tomb = found && (db->cfg.slots[idx].flags & SLOT_TOMB);

    /* Rewriting an identical value would only wear the flash. */
    if (type == REC_PUT && found && !tomb && db->cfg.slots[idx].len == len) {
        const struct fkv_slot *s = &db->cfg.slots[idx];
        uint8_t hdr[FKV_RECORD_HEADER_SIZE];
        err = io_read(db, s->block, s->off, hdr, sizeof hdr);
        if (err)
            return err;
        if (get16(hdr + 2) == vlen) {
            bool eq;
            err = io_cmp(db, s->block, s->off + FKV_RECORD_HEADER_SIZE + klen, val, vlen, &eq);
            if (err)
                return err;
            if (eq) {
                /* Only skip the write if the stored copy is intact. */
                uint32_t crc = 0xFFFFFFFFu;
                err = io_crc(db, s->block, s->off + FKV_RECORD_HEADER_SIZE, klen + vlen, &crc);
                if (err)
                    return err;
                if (~crc == get32(hdr + 4))
                    return FKV_OK;
            }
        }
    }

    err = make_room(db, len);
    if (err)
        return err;

    /* Compaction may have moved or dropped the old record: look again. */
    err = slot_find(db, key, klen, h, &idx, &found);
    if (err)
        return err;
    if (!found && db->slot_used >= db->slot_limit)
        return FKV_ERR_NOMEM;

    uint32_t b = db->head, off = db->cfg.blocks[b].end;
    rec_build(db, type, key, klen, val, vlen);
    err = io_prog(db, b, off, db->cfg.scratch, len);
    if (err)
        return err;
    db->cfg.blocks[b].end = off + len;
    db->st.user_bytes += len;

    if (found) {
        slot_release(db, idx);
    } else {
        db->slot_used++;
        db->cfg.slots[idx].hash = h;
        db->cfg.slots[idx].klen = (uint8_t)klen;
    }
    slot_point(db, idx, b, off, len, type == REC_DEL);
    return FKV_OK;
}

int fkv_put(fkv_t *db, const void *key, size_t klen, const void *val, size_t vlen)
{
    int err = check_handle(db);
    if (err)
        return err;
    if (!key || klen == 0 || klen > db->cfg.max_key_len || vlen > db->cfg.max_value_len ||
        (vlen && !val))
        return FKV_ERR_INVAL;
    return write_record(db, REC_PUT, key, (uint32_t)klen, val, (uint32_t)vlen);
}

int fkv_del(fkv_t *db, const void *key, size_t klen)
{
    int err = check_handle(db);
    if (err)
        return err;
    if (!key || klen == 0 || klen > db->cfg.max_key_len)
        return FKV_ERR_INVAL;
    return write_record(db, REC_DEL, key, (uint32_t)klen, NULL, 0);
}

int fkv_get(fkv_t *db, const void *key, size_t klen, void *buf, size_t bufsize, size_t *vlen)
{
    int err = check_handle(db);
    if (err)
        return err;
    if (!key || klen == 0 || klen > db->cfg.max_key_len)
        return FKV_ERR_INVAL;

    uint32_t h = fnv_update(FNV_INIT, key, klen), idx;
    bool found;
    err = slot_find(db, key, (uint32_t)klen, h, &idx, &found);
    if (err)
        return err;
    if (!found || (db->cfg.slots[idx].flags & SLOT_TOMB))
        return FKV_ERR_NOTFOUND;

    const struct fkv_slot *s = &db->cfg.slots[idx];
    err = io_read(db, s->block, s->off, db->cfg.scratch, s->len);
    if (err)
        return err;
    if (!rec_valid_in_scratch(db, s->len))
        return FKV_ERR_CORRUPT;

    size_t n = get16(db->cfg.scratch + 2);
    if (vlen)
        *vlen = n;
    if (!buf)
        return FKV_OK;
    if (bufsize < n)
        return FKV_ERR_TOOBIG;
    memcpy(buf, db->cfg.scratch + FKV_RECORD_HEADER_SIZE + klen, n);
    return FKV_OK;
}

int fkv_foreach(fkv_t *db, fkv_foreach_cb cb, void *ctx)
{
    int err = check_handle(db);
    if (err)
        return err;
    if (!cb)
        return FKV_ERR_INVAL;

    for (uint32_t i = 0; i < db->cfg.slot_count; i++) {
        const struct fkv_slot *s = &db->cfg.slots[i];
        if (!(s->flags & SLOT_USED) || (s->flags & SLOT_TOMB))
            continue;
        err = io_read(db, s->block, s->off, db->cfg.scratch, s->len);
        if (err)
            return err;
        if (!rec_valid_in_scratch(db, s->len))
            return FKV_ERR_CORRUPT;
        const uint8_t *r = db->cfg.scratch;
        int rc = cb(r + FKV_RECORD_HEADER_SIZE, r[1], r + FKV_RECORD_HEADER_SIZE + r[1],
                    get16(r + 2), ctx);
        if (rc)
            return rc;
    }
    return FKV_OK;
}

int fkv_stat(const fkv_t *db, struct fkv_stats *st)
{
    if (!db || !db->mounted || !st)
        return FKV_ERR_INVAL;
    *st = db->st;
    st->keys = 0;
    st->tombstones = 0;
    for (uint32_t i = 0; i < db->cfg.slot_count; i++) {
        uint8_t f = db->cfg.slots[i].flags;
        if (f & SLOT_TOMB)
            st->tombstones++;
        else if (f & SLOT_USED)
            st->keys++;
    }
    st->live_bytes = db->live_bytes;
    st->capacity_bytes = db->capacity;
    st->block_count = db->flash->block_count;
    st->free_blocks = db->nfree;
    st->erase_min = 0xFFFFFFFFu;
    st->erase_max = 0;
    st->erase_total = 0;
    for (uint32_t b = 0; b < db->flash->block_count; b++) {
        uint32_t ec = db->cfg.blocks[b].ec;
        if (ec < st->erase_min)
            st->erase_min = ec;
        if (ec > st->erase_max)
            st->erase_max = ec;
        st->erase_total += ec;
    }
    return FKV_OK;
}

int fkv_check(fkv_t *db)
{
    int err = check_handle(db);
    if (err)
        return err;

    const struct fkv_block *bl = db->cfg.blocks;
    uint32_t n = db->flash->block_count, nfree = 0, used = 0;
    uint64_t total = 0;

    for (uint32_t i = 0; i < db->cfg.slot_count; i++) {
        const struct fkv_slot *s = &db->cfg.slots[i];
        if (!(s->flags & SLOT_USED))
            continue;
        used++;
        if (s->block >= n || bl[s->block].state != BS_ACTIVE || s->off < OFF_DATA ||
            s->off + s->len > bl[s->block].end)
            return FKV_ERR_CORRUPT;
        err = io_read(db, s->block, s->off, db->cfg.scratch, s->len);
        if (err)
            return err;
        const uint8_t *r = db->cfg.scratch;
        if (!rec_valid_in_scratch(db, s->len) || r[1] != s->klen ||
            rec_size(db, r[1], get16(r + 2)) != s->len ||
            (r[0] == REC_DEL) != ((s->flags & SLOT_TOMB) != 0) ||
            fnv_update(FNV_INIT, r + FKV_RECORD_HEADER_SIZE, r[1]) != s->hash)
            return FKV_ERR_CORRUPT;
        total += s->len;
    }
    if (used != db->slot_used || total != db->live_bytes)
        return FKV_ERR_CORRUPT;

    uint64_t live_sum = 0;
    for (uint32_t b = 0; b < n; b++) {
        if (bl[b].state != BS_ACTIVE) {
            nfree++;
            if (bl[b].live != 0)
                return FKV_ERR_CORRUPT;
            continue;
        }
        if (bl[b].live > bl[b].end - OFF_DATA || bl[b].end > db->flash->block_size)
            return FKV_ERR_CORRUPT;
        live_sum += bl[b].live;
    }
    if (nfree != db->nfree || live_sum != db->live_bytes)
        return FKV_ERR_CORRUPT;
    if (db->head != NONE && bl[db->head].state != BS_ACTIVE)
        return FKV_ERR_CORRUPT;
    return FKV_OK;
}

const char *fkv_strerror(int err)
{
    switch (err) {
    case FKV_OK: return "ok";
    case FKV_ERR_IO: return "flash I/O error (remount required)";
    case FKV_ERR_CORRUPT: return "data corrupted";
    case FKV_ERR_NOSPC: return "no space left";
    case FKV_ERR_NOTFOUND: return "key not found";
    case FKV_ERR_INVAL: return "invalid argument";
    case FKV_ERR_NOMEM: return "key index full";
    case FKV_ERR_NOFS: return "no fkv store on flash";
    case FKV_ERR_TOOBIG: return "buffer too small";
    default: return "unknown error";
    }
}
