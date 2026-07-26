/*
 * xmfutil — XMF/MXMF creation and extraction tool
 * © 2026 zefie
 *
 * 1) Create XMF v1 (XMF_1.00) from MIDI + DLS
 * 2) Create MXMF v2 (XMF_2.00) from MIDI + DLS
 * 3) Extract MIDI/DLS from XMF v1 or MXMF v2 files
 *
 * Implements XMF Meta File Format 1.00b + 2.00 per MMA specs:
 *   RP-030 XMF Meta File Format 1.0
 *   RP-040 XMF Compression Definition for "zlib" (UnpackerTypeID 0, UnpackerID 0x01)
 *   RP-043 XMF Meta File Format 2.0 (adds XmfFileTypeID / XmfFileTypeRevisionID BE32 fields)
 *   RP-042a Type 2 XMF (Mobile XMF): ResourceFormatID 5 for Mobile DLS, 0 for SMF
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <zlib.h>

#define FALSE 0
#define TRUE  1
typedef unsigned char bool;

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "xmfutil — XMF/MXMF file creation and extraction\n"
        "\n"
        "Usage:\n"
        "  Create XMF v1:   %s create v1  <midi.mid> <bank.dls> [output.xmf]\n"
        "  Create MXMF v2:  %s create v2  <midi.mid> <bank.dls> [output.mxmf]\n"
        "  Extract:         %s extract    <input.xmf|input.mxmf> [output_dir]\n"
        "\n"
        "Options:\n"
        "  -q               Quiet mode\n"
        "  -z LEVEL         Zlib compression level (0-9, 0=no compression, default: 6)\n"
        "\n"
        "Both v1 and v2 use VLQ-encoded tree nodes per XMF Meta File Format.\n"
        "v2 adds XmfFileTypeID=2 / XmfFileTypeRevisionID=1 after the magic.\n"
        "DLS (Mobile DLS for v2) before MIDI, v2 word-aligned payloads, zlib via\n"
        "Standard UnpackerID 0x01 per RP-040.\n",
        prog, prog, prog);
}

/* ---------- allocation / file I/O ---------- */

static void *xalloc(size_t sz)
{
    void *p = calloc(1, sz);
    if (!p) { fprintf(stderr, "Error: out of memory (alloc %zu)\n", sz); exit(1); }
    return p;
}

static uint8_t *load_file(const char *path, uint32_t *olen)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Error: cannot open '%s'\n", path); return NULL; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 0x7FFFFFFF) { fclose(f); return NULL; }
    uint8_t *b = (uint8_t *)xalloc((size_t)sz);
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return NULL; }
    fclose(f); *olen = (uint32_t)sz; return b;
}

static int save_file(const char *path, const uint8_t *d, uint32_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "Error: cannot write '%s'\n", path); return -1; }
    if (fwrite(d, 1, n, f) != n) { fclose(f); return -1; }
    fclose(f); return 0;
}

static char *make_outpath(const char *dir, const char *fn)
{
    size_t d = strlen(dir); while (d>0 && (dir[d-1]=='/'||dir[d-1]=='\\')) d--;
    size_t f = strlen(fn);
    char *o = (char *)xalloc(d + 1 + f + 1);
    if (d) { memcpy(o, dir, d); o[d] = '/'; memcpy(o+d+1, fn, f+1); }
    else memcpy(o, fn, f+1);
    return o;
}

static const char *strip_dir(const char *p)
{
    const char *a = strrchr(p, '/'), *b = strrchr(p, '\\');
    if (a && b) return (a>b?a:b)+1;
    if (a) return a+1;
    if (b) return b+1;
    return p;
}

static void replace_ext(char *buf, size_t bs, const char *path, const char *ne)
{
    const char *base = strip_dir(path), *dot = strrchr(base, '.');
    size_t bl = (dot && dot>base) ? (size_t)(dot-path) : strlen(path);
    if (bl >= bs) bl = bs-1;
    memcpy(buf, path, bl);
    snprintf(buf+bl, bs-bl, "%s", ne);
}

/* ---------- VLQ ---------- */

static uint32_t vlq_size(uint32_t v) {
    if (v == 0) return 1;
    uint32_t n = 0; while (v) { v>>=7; n++; } return n;
}

static uint32_t vlq_write(uint32_t v, uint8_t *out)
{
    uint8_t t[5]; int i = 0;
    do { t[i++] = (uint8_t)(v & 0x7F); v >>= 7; } while (v);
    for (int j = i-1; j >= 0; j--)
        *out++ = (j==0) ? t[j] : (uint8_t)(t[j] | 0x80);
    return (uint32_t)i;
}

static uint32_t vlq_read(const uint8_t *b, uint32_t len, uint32_t *pos)
{
    uint32_t v = 0; int n = 0;
    while (*pos<len && n<5) {
        uint8_t c = b[(*pos)++];
        v = (v<<7) | (c & 0x7Fu); n++;
        if (!(c & 0x80u)) break;
    }
    return v;
}

/* ---------- dynamic buffer ---------- */

typedef struct { uint8_t *data; uint32_t len, cap; } buf_t;
static void b_init(buf_t *b) { b->data = NULL; b->len = b->cap = 0; }
static void b_free(buf_t *b) { if(b->data) free(b->data); b->data=NULL; b->len=b->cap=0; }
static void b_append(buf_t *b, const uint8_t *s, uint32_t n) {
    if (!n) return;
    uint32_t need = b->len + n;
    if (need > b->cap) {
        uint32_t nc = b->cap ? b->cap*2 : 256;
        while (nc < need) nc *= 2;
        b->data = (uint8_t *)realloc(b->data, nc); b->cap = nc;
    }
    memcpy(b->data+b->len, s, n); b->len = need;
}
static void b_vlq(buf_t *b, uint32_t v) { uint8_t t[5]; uint32_t n = vlq_write(v, t); b_append(b, t, n); }
static void b_be32(buf_t *b, uint32_t v) { uint8_t t[4]; t[0]=v>>24; t[1]=v>>16; t[2]=v>>8; t[3]=v; b_append(b, t, 4); }

/* ---------- zlib ---------- */

static uint8_t *z_compress(const uint8_t *s, uint32_t n, uint32_t *o, int lv) {
    if (!n) { *o=0; return NULL; }
    uLongf b = compressBound((uLong)n);
    uint8_t *d = (uint8_t *)xalloc((size_t)b);
    if (compress2(d, &b, s, (uLong)n, lv) != Z_OK) { free(d); return NULL; }
    *o = (uint32_t)b; return d;
}

static uint8_t *z_inflate(const uint8_t *s, uint32_t n, uint32_t *o, uint32_t h) {
    if (n < 2) return NULL;
    z_stream zs; memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, 15+32) != Z_OK) return NULL;
    uint32_t c = h ? h : (n*4); if (c<4096) c=4096;
    uint8_t *d = (uint8_t *)xalloc(c);
    zs.next_in = (Bytef*)s; zs.avail_in = (uInt)n;
    zs.next_out = (Bytef*)d; zs.avail_out = (uInt)c;
    for (;;) {
        int r = inflate(&zs, Z_NO_FLUSH);
        if (r==Z_STREAM_END) break;
        if (r!=Z_OK) { free(d); inflateEnd(&zs); return NULL; }
        if (!zs.avail_out) {
            uint32_t u = (uint32_t)((char*)zs.next_out-(char*)d), nc = c*2;
            if (nc < c) { free(d); inflateEnd(&zs); return NULL; }
            uint8_t *nd = (uint8_t *)realloc(d, nc);
            if (!nd) { free(d); inflateEnd(&zs); return NULL; }
            d=nd; c=nc; zs.next_out=(Bytef*)(d+u); zs.avail_out=(uInt)(c-u);
        }
    }
    *o = (uint32_t)((char*)zs.next_out-(char*)d);
    inflateEnd(&zs); return d;
}

/* ---------- signatures ---------- */

static bool m_eq(const uint8_t *a, const char *b, uint32_t n) {
    for (uint32_t i=0; i<n; i++) if (a[i]!=(uint8_t)b[i]) return FALSE;
    return TRUE;
}
static int find4(const uint8_t *b, uint32_t n, const char s[4]) {
    for (uint32_t i=0; i+4<=n; i++)
        if (b[i]==(uint8_t)s[0]&&b[i+1]==(uint8_t)s[1]&&b[i+2]==(uint8_t)s[2]&&b[i+3]==(uint8_t)s[3]) return (int)i;
    return -1;
}
static bool rmid_smf(const uint8_t *b, uint32_t n, uint8_t **s, uint32_t *sl) {
    if (!b||n<12||!m_eq(b,"RIFF",4)) return FALSE;
    uint32_t rs = b[4]|((uint32_t)b[5]<<8)|((uint32_t)b[6]<<16)|((uint32_t)b[7]<<24);
    if (rs>(n-8)||!m_eq(b+8,"RMID",4)) return FALSE;
    uint32_t i=12;
    while (i+8<=n) {
        uint32_t cs = b[i+4]|((uint32_t)b[i+5]<<8)|((uint32_t)b[i+6]<<16)|((uint32_t)b[i+7]<<24);
        if (cs>(n-i-8)) break;
        if (m_eq(b+i,"data",4)) { *s=(uint8_t*)xalloc(cs); memcpy(*s,b+i+8,cs); *sl=cs; return TRUE; }
        i+=8+cs; if (i&1) i++;
    }
    return FALSE;
}

/* ========== METADATA BUILDERS (per-spec) ========== */

/*
 * Universal field contents: VLQ(0) | VLQ(LenIncludingFormat) | VLQ(StringFormatTypeID) | data
 */

static void meta_filename(buf_t *hdr, uint32_t fieldId, const char *filename)
{
    const char *nm = strip_dir(filename);
    uint32_t nlen = (uint32_t)strlen(nm);

    /* Standard Filename on Disk or Node Name field. */
    b_vlq(hdr, 0);
    b_vlq(hdr, fieldId);
    b_vlq(hdr, 0);              /* VLQ(0) = Universal */
    b_vlq(hdr, vlq_size(0) + nlen);
    b_vlq(hdr, 0);              /* StringFormatTypeID 0 = extended ASCII visible */
    b_append(hdr, (const uint8_t*)nm, nlen);
}

static void meta_resource_names(buf_t *hdr, const char *filename)
{
    meta_filename(hdr, 4, filename); /* Filename on Disk */
    meta_filename(hdr, 1, filename); /* Node Name */
}

static void meta_resourceFormat(buf_t *hdr, int fmtId)
{
    /* FieldSpecifier: Standard, FieldID=3 */
    b_append(hdr, (const uint8_t*)"\x00", 1);  /* VLQ(0) = Standard FieldID */
    b_vlq(hdr, 3);  /* FieldID 3 = Resource Format */

    /* FieldContents: Universal, LengthInBytes, StringFormatTypeID=6, FormatTypeID=0, ResourceFormatID */
    uint32_t ft_vlq = vlq_size(0);
    uint32_t id_vlq = vlq_size((uint32_t)fmtId);
    uint32_t dataLen = 1 + ft_vlq + id_vlq; /* 1=StringFormatTypeID byte */
    b_vlq(hdr, 0);              /* VLQ(0) = Universal */
    b_vlq(hdr, dataLen);        /* LengthInBytes = StringFormatTypeID + FormatTypeID + ResourceFormatID */
    b_vlq(hdr, 6);              /* StringFormatTypeID 6 = binary hidden */
    b_vlq(hdr, 0);              /* FormatTypeID 0 = Standard */
    b_vlq(hdr, (uint32_t)fmtId);
}

/* ========== UNPACKER (zlib per RP-040) ========== */
/* Standard UnpackerID: UnpackerTypeID=0, UnpackerID=0x01, DecodedSize VLQ */

static void unpacker_zlib_write(buf_t *b, uint32_t decodedSize) {
    b_append(b, (const uint8_t*)"\x00", 1);  /* UnpackerTypeID 0 = Standard */
    b_append(b, (const uint8_t*)"\x01", 1);  /* UnpackerID 1 = zlib */
    b_vlq(b, decodedSize);
}

/* ========== BUILD XMF ========== */

/*
 * Build a FileNode or FolderNode, writing to buf.
 * Returns the number of bytes written (including the VLQ-size feedback for NodeLength).
 *
 * For a FileNode (itemCount=0):
 *   Node            NodeLength          VLQ (includes itself)
 *                   NodeContainedItems  VLQ(0)
 *                   NodeHeaderLength    VLQ
 *                   NodeMetaData        VLQ(len) + metadata_bytes
 *                   NodeUnpackers       VLQ(len) + unpackers_bytes
 *                   NodeContents        VLQ(ReferenceTypeID=1) + resource_data
 *
 * FolderNode contents also begin with a ContentReference. For an inline
 * folder this is ReferenceTypeID 1 followed by the child Node structures.
 */

typedef struct {
    uint32_t headerLen;
    uint32_t nodeLen;
    uint32_t padLen;
} node_layout_t;

static node_layout_t layout_node(uint32_t nodeOffset, bool alignInlineData,
                                  uint32_t itemCount,
                                  uint32_t metaLen, uint32_t unpackerLen,
                                  uint32_t contentLen)
{
    node_layout_t layout = { 0, 0, 0 };
    uint32_t headerLen = 0;
    uint32_t nodeLen = contentLen;

    for (;;) {
        uint32_t baseHeaderLen = vlq_size(nodeLen)
                               + vlq_size(itemCount)
                               + vlq_size(headerLen)
                               + vlq_size(metaLen) + metaLen
                               + vlq_size(unpackerLen) + unpackerLen;
        /* Mobile XMF requires the data after ReferenceTypeID to be word-aligned. */
        uint32_t padLen = alignInlineData
                        ? ((nodeOffset + baseHeaderLen + 1u) & 1u) : 0u;
        uint32_t newHeaderLen = baseHeaderLen + padLen;
        uint32_t newNodeLen = newHeaderLen + contentLen;
        if (newHeaderLen == headerLen && newNodeLen == nodeLen) {
            layout.headerLen = headerLen;
            layout.nodeLen = nodeLen;
            layout.padLen = padLen;
            return layout;
        }
        headerLen = newHeaderLen;
        nodeLen = newNodeLen;
    }
}

static void write_node(buf_t *nodes, node_layout_t layout, uint32_t itemCount,
                       const uint8_t *metaData, uint32_t metaLen,
                       const uint8_t *unpackerData, uint32_t unpackerLen,
                       const uint8_t *contentData, uint32_t contentLen)
{
    b_vlq(nodes, layout.nodeLen);
    b_vlq(nodes, itemCount);
    b_vlq(nodes, layout.headerLen);
    /* NodeMetaData */
    b_vlq(nodes, metaLen);
    if (metaLen) b_append(nodes, metaData, metaLen);
    /* NodeUnpackers */
    b_vlq(nodes, unpackerLen);
    if (unpackerLen) b_append(nodes, unpackerData, unpackerLen);
    if (layout.padLen) b_append(nodes, (const uint8_t*)"\x00", 1);
    /* NodeContents */
    if (contentLen) b_append(nodes, contentData, contentLen);
}

static uint32_t smf_resource_format(const uint8_t *midi, uint32_t midiLen)
{
    if (midiLen >= 10 && m_eq(midi, "MThd", 4)) {
        uint32_t format = ((uint32_t)midi[8] << 8) | midi[9];
        if (format <= 1) return format;
    }
    return 0;
}

static uint8_t *make_xmf(int version,
                          const char *midi_path, const uint8_t *midi, uint32_t midi_len,
                          const char *dls_path,  const uint8_t *dls,  uint32_t dls_len,
                          uint32_t *out_len, bool compress, int zlevel)
{
    const uint8_t *mpld = midi; uint32_t mplen = midi_len;
    const uint8_t *dpld = dls;  uint32_t dplen = dls_len;
    uint8_t *mc = NULL, *dc = NULL;
    bool alignInlineData = (version == 2) ? TRUE : FALSE;

    if (compress) {
        mc = z_compress(midi, midi_len, &mplen, zlevel);
        dc = z_compress(dls, dls_len, &dplen, zlevel);
        if (mc) mpld = mc;
        if (dc) dpld = dc;
    }

    /* ---- DLS FileNode ---- */
    buf_t dh; b_init(&dh);
    meta_resource_names(&dh, dls_path);
    meta_resourceFormat(&dh, (version==2) ? 5 : 2);

    /* DLS NodeUnpackers */
    buf_t d_unpk; b_init(&d_unpk);
    if (compress)
        unpacker_zlib_write(&d_unpk, dls_len);

    /* DLS NodeContents: ReferenceTypeID=1 + inline resource data */
    buf_t d_content; b_init(&d_content);
    b_vlq(&d_content, 1); /* In-Line Resource */
    b_append(&d_content, dpld, dplen);

    /* ---- MIDI FileNode ---- */
    buf_t mh; b_init(&mh);
    meta_resource_names(&mh, midi_path);
    meta_resourceFormat(&mh, (int)smf_resource_format(midi, midi_len));

    buf_t m_unpk; b_init(&m_unpk);
    if (compress)
        unpacker_zlib_write(&m_unpk, midi_len);

    buf_t m_content; b_init(&m_content);
    b_vlq(&m_content, 1);
    b_append(&m_content, mpld, mplen);

    /* ---- Root FolderNode ---- */
    /* Root has XMF File Type meta-data (Standard FieldID 0) */
    buf_t rh; b_init(&rh);
    /* Standard FieldSpecifier: 0x00, FieldID=0 */
    b_append(&rh, (const uint8_t*)"\x00", 1);
    b_vlq(&rh, 0);
    /* FieldContents: Universal, LengthInBytes, StringFormatTypeID=6, FileTypeID, Revision */
    b_vlq(&rh, 0);              /* VLQ(0) = Universal */
    b_vlq(&rh, 3);              /* LengthInBytes: 1(SFT) + 1(FT) + 1(Rev) = 3 */
    b_vlq(&rh, 6);              /* StringFormatTypeID 6 = binary hidden */
    b_vlq(&rh, (uint32_t)version);  /* FileTypeID */
    b_vlq(&rh, 0u);             /* RevisionID */

    /* Resolve all absolute offsets, alignment bytes, and self-sized VLQs. */
    uint32_t fixedHeaderLen = 8u + ((version == 2) ? 8u : 0u);
    uint32_t treeStart = fixedHeaderLen + 5u;
    uint32_t rootHeaderLen = 0;
    node_layout_t dl = { 0, 0, 0 }, mlayout = { 0, 0, 0 }, rl = { 0, 0, 0 };
    for (;;) {
        uint32_t dlsOffset = treeStart + rootHeaderLen + 1u;
        node_layout_t newDl = layout_node(dlsOffset, alignInlineData, 0,
                                          dh.len, d_unpk.len, d_content.len);
        uint32_t midiOffset = dlsOffset + newDl.nodeLen;
        node_layout_t newMl = layout_node(midiOffset, alignInlineData, 0,
                                          mh.len, m_unpk.len, m_content.len);
        uint32_t rootContentLen = 1u + newDl.nodeLen + newMl.nodeLen;
        node_layout_t newRl = layout_node(treeStart, alignInlineData, 2,
                                          rh.len, 0, rootContentLen);
        uint32_t fileLen = treeStart + newRl.nodeLen;
        uint32_t treeEnd = fileLen - 1u;
        uint32_t newTreeStart = fixedHeaderLen + vlq_size(fileLen) + 1u
                              + vlq_size(treeStart) + vlq_size(treeEnd);
        if (newTreeStart == treeStart && newRl.headerLen == rootHeaderLen
                && newDl.nodeLen == dl.nodeLen && newMl.nodeLen == mlayout.nodeLen) {
            dl = newDl;
            mlayout = newMl;
            rl = newRl;
            break;
        }
        treeStart = newTreeStart;
        rootHeaderLen = newRl.headerLen;
        dl = newDl;
        mlayout = newMl;
        rl = newRl;
    }

    buf_t dls_node; b_init(&dls_node);
    write_node(&dls_node, dl, 0, dh.data, dh.len,
               d_unpk.data, d_unpk.len, d_content.data, d_content.len);

    buf_t midi_node; b_init(&midi_node);
    write_node(&midi_node, mlayout, 0, mh.data, mh.len,
               m_unpk.data, m_unpk.len, m_content.data, m_content.len);

    buf_t r_content; b_init(&r_content);
    b_vlq(&r_content, 1); /* In-Line Resource (the contained Nodes) */
    b_append(&r_content, dls_node.data, dls_node.len);
    b_append(&r_content, midi_node.data, midi_node.len);

    buf_t root_node; b_init(&root_node);
    write_node(&root_node, rl, 2, rh.data, rh.len,
               NULL, 0, r_content.data, r_content.len);

    uint32_t fl = treeStart + root_node.len;
    uint32_t treeEnd = fl - 1u;

    buf_t result; b_init(&result);
    b_append(&result, (const uint8_t*)((version==2)?"XMF_2.00":"XMF_1.00"), 8);
    if (version == 2) { b_be32(&result, 2); b_be32(&result, 1); }
    b_vlq(&result, fl);
    b_vlq(&result, 0);          /* MetaDataTypesTable = 0 */
    b_vlq(&result, treeStart);  /* TreeStart */
    b_vlq(&result, treeEnd);    /* TreeEnd */
    b_append(&result, root_node.data, root_node.len);

    free(mc); free(dc);
    b_free(&dh); b_free(&mh); b_free(&rh);
    b_free(&d_unpk); b_free(&m_unpk);
    b_free(&d_content); b_free(&m_content);
    b_free(&dls_node); b_free(&midi_node); b_free(&r_content); b_free(&root_node);
    *out_len = result.len;
    return result.data;
}

/* ========== EXTRACT ========== */

static bool extract_scan(const uint8_t *bytes, uint32_t len, uint32_t scanStart,
                          uint8_t **midi, uint32_t *midiLen,
                          uint8_t **dls, uint32_t *dlsLen)
{
    /* Scan raw: MThd and RIFF DLS after scanStart */
    bool seenDls = FALSE;
    for (uint32_t i = scanStart; i + 8 < len; i++) {
        if (!seenDls && m_eq(bytes+i, "RIFF", 4)) {
            uint32_t sz = (uint32_t)bytes[i+4]|((uint32_t)bytes[i+5]<<8)|((uint32_t)bytes[i+6]<<16)|((uint32_t)bytes[i+7]<<24);
            if (sz <= (len-i-8) && m_eq(bytes+i+8, "DLS ", 4) && (8+sz) >= 1024) {
                *dls = (uint8_t *)xalloc(8+sz);
                memcpy(*dls, bytes+i, 8+sz); *dlsLen = 8+sz; seenDls = TRUE;
            } else if (sz <= (len-i-8)) i += 7+sz;
        }
        if (!*midi && m_eq(bytes+i, "MThd", 4)) {
            *midi = (uint8_t *)xalloc(len-i);
            memcpy(*midi, bytes+i, len-i); *midiLen = len-i;
        }
        if (seenDls && *midi) break;
    }

    /* zlib decompress scan for any remaining content */
    if (!seenDls || !*midi) {
        for (uint32_t i = scanStart; i + 2 < len; i++) {
            if (bytes[i] == 0x78 || (bytes[i]==0x1f && i+1<len && bytes[i+1]==0x8b)) {
                uint32_t infLen = 0;
                uint8_t *inf = z_inflate(bytes+i, len-i, &infLen, (len-i)*4);
                if (inf && infLen > 0) {
                    if (!seenDls) {
                        for (uint32_t j = 0; j+8 <= infLen; j++) {
                            if (m_eq(inf+j, "RIFF", 4)) {
                                uint32_t sz = (uint32_t)inf[j+4]|((uint32_t)inf[j+5]<<8)|((uint32_t)inf[j+6]<<16)|((uint32_t)inf[j+7]<<24);
                                if (sz<=(infLen-j-8) && m_eq(inf+j+8,"DLS ",4) && (8+sz)>=1024) {
                                    *dls = (uint8_t*)xalloc(8+sz); memcpy(*dls, inf+j, 8+sz); *dlsLen = 8+sz; seenDls = TRUE; break;
                                } else if (sz <= (infLen-j-8)) j += 7+sz;
                            }
                        }
                    }
                    if (!*midi) {
                        int mo = find4(inf, infLen, "MThd");
                        if (mo >= 0) {
                            *midi = (uint8_t*)xalloc(infLen-mo);
                            memcpy(*midi, inf+mo, infLen-mo); *midiLen = infLen-mo;
                        } else {
                            uint8_t *smf = NULL; uint32_t sl = 0;
                            if (rmid_smf(inf, infLen, &smf, &sl)) { *midi = smf; *midiLen = sl; }
                        }
                    }
                    free(inf);
                    if (seenDls && *midi) break;
                }
            }
        }
    }
    return (*midi != NULL) || (*dls != NULL);
}

static bool extract_tree(const uint8_t *bytes, uint32_t len, uint32_t hdrStart,
                          uint8_t **midi, uint32_t *midiLen,
                          uint8_t **dls, uint32_t *dlsLen)
{
    uint32_t pos = hdrStart;
    vlq_read(bytes, len, &pos); /* fileLen */
    uint32_t mttLen = vlq_read(bytes, len, &pos); /* metaTableLen */
    pos += mttLen;
    if (pos > len) return FALSE;
    uint32_t rootOff = vlq_read(bytes, len, &pos); /* TreeStart */
    vlq_read(bytes, len, &pos); /* TreeEnd */
    return extract_scan(bytes, len, rootOff, midi, midiLen, dls, dlsLen);
}

/* ========== CLI ========== */

typedef struct { uint8_t *midi; uint32_t mlen; uint8_t *dls; uint32_t dlen; } eres;
static void eres_free(eres *r) { if(r->midi){free(r->midi);r->midi=NULL;} if(r->dls){free(r->dls);r->dls=NULL;} }

static int cmd_extract(const char *in, const char *odir, int quiet)
{
    uint32_t n; uint8_t *d = load_file(in, &n); if (!d) return 1;
    eres r; memset(&r, 0, sizeof(r));
    bool ok = FALSE;
    if (n>=8 && m_eq(d,"XMF_1.00",8)) { if(!quiet) printf("Detected XMF v1\n"); ok=extract_tree(d,n,8,&r.midi,&r.mlen,&r.dls,&r.dlen); }
    else if (n>=8 && m_eq(d,"XMF_2.00",8)) { if(!quiet) printf("Detected MXMF v2\n"); ok=extract_tree(d,n,16,&r.midi,&r.mlen,&r.dls,&r.dlen); }
    else { fprintf(stderr,"Error: not an XMF file\n"); free(d); return 1; }
    free(d);
    if (!ok) { fprintf(stderr,"Error: no content found\n"); eres_free(&r); return 1; }

    const char *dir = odir ? odir : ".";
    { char cmd[1024]; snprintf(cmd,sizeof(cmd),"mkdir -p \"%s\" 2>/dev/null",dir); (void)system(cmd); }
    char base[256]; {
        const char *s = strip_dir(in), *dot = strrchr(s,'.');
        size_t bl = (dot&&dot>s)?(size_t)(dot-s):strlen(s);
        if (bl>sizeof(base)-1) bl=sizeof(base)-1;
        memcpy(base,s,bl); base[bl]=0;
    }
    int ret = 0;
    if (r.dls) {
        char nm[320]; snprintf(nm,sizeof(nm),"%s.dls",base);
        char *p = make_outpath(dir, nm);
        if (save_file(p, r.dls, r.dlen)==0) { if(!quiet) printf("Extracted DLS:  %s (%u bytes)\n",p,r.dlen); }
        else { fprintf(stderr,"Error: write DLS failed\n"); ret=1; }
        free(p); free(r.dls);
    } else if (!quiet) printf("No DLS bank data found\n");    
    if (r.midi) {
        char nm[320]; snprintf(nm,sizeof(nm),"%s.mid",base);
        char *p = make_outpath(dir, nm);
        if (save_file(p, r.midi, r.mlen)==0) { if(!quiet) printf("Extracted MIDI: %s (%u bytes)\n",p,r.mlen); }
        else { fprintf(stderr,"Error: write MIDI failed\n"); ret=1; }
        free(p); free(r.midi);
    } else if (!quiet) printf("No MIDI data found\n");
    return ret;
}

static int cmd_create(int v, const char *mp, const char *dp, const char *op, int zl, int q)
{
    uint32_t ml, dl;
    uint8_t *m = load_file(mp, &ml), *ds = load_file(dp, &dl);
    if (!m || !ds) { free(m); free(ds); return 1; }
    if (ml<4 || !m_eq(m,"MThd",4)) fprintf(stderr,"Warning: MIDI has no MThd header\n");
    bool cmp = (zl > 0) ? TRUE : FALSE;
    if (!q) printf("Creating %s from %s + %s ...\n", (v==2)?"MXMF v2":"XMF v1", strip_dir(mp), strip_dir(dp));
    uint32_t ol; uint8_t *o = make_xmf(v, mp, m, ml, dp, ds, dl, &ol, cmp, zl);
    free(m); free(ds);
    if (!o) { fprintf(stderr,"Error: creation failed\n"); return 1; }
    char def[512];
    if (!op) {
        replace_ext(def, sizeof(def), strip_dir(mp), v==1 ? ".xmf" : ".mxmf");
        op=def;
    }
    if (save_file(op, o, ol)==0) { if(!q) printf("Created: %s (%u bytes)\n",op,ol); free(o); return 0; }
    fprintf(stderr,"Error: write failed '%s'\n",op); free(o); return 1;
}

int main(int argc, char *argv[])
{
    int zl=6, quiet=0;
    for (int i=1; i<argc; i++) {
        if (!strcmp(argv[i],"-q")) quiet=1;
        else if (!strcmp(argv[i],"-z") && i+1<argc) { zl=atoi(argv[++i]); if(zl<0)zl=0; if(zl>9)zl=9; }
        else if (!strcmp(argv[i],"-h")||!strcmp(argv[i],"--help")) { print_usage(argv[0]); return 0; }
    }
    const char *pa[10]; int pc=0;
    for (int i=1; i<argc && pc<10; i++) {
        if (!strcmp(argv[i],"-q")) continue;
        if (!strcmp(argv[i],"-z")){i++;continue;}
        pa[pc++]=argv[i];
    }
    if (pc<2) { print_usage(argv[0]); return 1; }
    if (!strcmp(pa[0],"create")) {
        if (pc<4) { fprintf(stderr,"'create' needs version (v1|v2), MIDI, DLS\n"); return 1; }
        int v; if (!strcmp(pa[1],"v1")||!strcmp(pa[1],"1")) v=1; else if (!strcmp(pa[1],"v2")||!strcmp(pa[1],"2")) v=2;
        else { fprintf(stderr,"version must be 'v1' or 'v2'\n"); return 1; }
        return cmd_create(v, pa[2], pa[3], pc>=5?pa[4]:NULL, zl, quiet);
    }
    if (!strcmp(pa[0],"extract")) {
        if (pc<2) { fprintf(stderr,"'extract' needs input file\n"); return 1; }
        return cmd_extract(pa[1], pc>=3?pa[2]:NULL, quiet);
    }
    fprintf(stderr,"unknown command '%s'\n",pa[0]); print_usage(argv[0]); return 1;
}
