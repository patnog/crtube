// h264 - avcC parsing + AVCC-to-Annex-B conversion for cellVdec. See h264.h.
#include "h264.h"
#include "dbg.h"
#include <string.h>
#include <stdio.h>

static const uint8_t START_CODE[4] = { 0, 0, 0, 1 };

// minimal SPS bit reader: enough to reach max_num_ref_frames. cellVdec sizes its decoded-frame pool
// (the DPB) from the level, but a stream can legally ask for more reference frames than its declared
// level's DPB holds (common in BDrip encodes). Reading the real count lets us size the pool to fit,
// otherwise the decoder conceals every inter-frame as black.

typedef struct { const uint8_t *data; int size; int bitPos; } BitReader;

static int readBit(BitReader *reader)
{
   int byteIndex = reader->bitPos >> 3;
   if (byteIndex >= reader->size) return 0;
   int bit = (reader->data[byteIndex] >> (7 - (reader->bitPos & 7))) & 1;
   reader->bitPos++;
   return bit;
}

static uint32_t readBits(BitReader *reader, int count)
{
   uint32_t value = 0;
   while (count-- > 0) value = (value << 1) | readBit(reader);
   return value;
}

static uint32_t readUe(BitReader *reader)   // Exp-Golomb unsigned
{
   int zeros = 0;
   while (readBit(reader) == 0 && zeros < 32) zeros++;
   return (uint32_t)((1u << zeros) - 1 + (zeros ? readBits(reader, zeros) : 0));
}

static int readSe(BitReader *reader)        // Exp-Golomb signed
{
   uint32_t k = readUe(reader);
   return (k & 1) ? (int)((k + 1) / 2) : -(int)(k / 2);
}

// strips H.264 emulation-prevention bytes (00 00 03 -> 00 00) into `out`; returns bytes written.
// `sourceIndex` (optional) records, per output byte, its index in `in` so patched bytes can be
// written back to the emulation-encoded original.
static int stripEmulation(const uint8_t *in, int inSize, uint8_t *out, int *sourceIndex, int outCap)
{
   int outSize = 0;
   for (int i = 0; i < inSize && outSize < outCap; i++) {
      if (i >= 2 && in[i] == 3 && in[i - 1] == 0 && in[i - 2] == 0) continue;
      if (sourceIndex) sourceIndex[outSize] = i;
      out[outSize++] = in[i];
   }
   return outSize;
}

typedef struct {
   int refFrames;      // max_num_ref_frames
   int refFramesBit;   // rbsp bit position where its Exp-Golomb code starts
   int frameMbs;       // coded frame size in macroblocks
   int codedWidth;     // coded pixels - NOT the display size; encoders pad and crop back (see h264.h)
   int codedHeight;
   int level;          // level_idc
} SpsFields;

// walks the stripped SPS rbsp up to the frame dimensions, filling `fields`. returns 0 / -1.
static int walkSps(const uint8_t *rbsp, int rbspSize, SpsFields *fields)
{
   BitReader reader = { rbsp, rbspSize, 0 };
   int profile = readBits(&reader, 8);
   readBits(&reader, 8);                 // constraint flags + reserved
   fields->level = (int)readBits(&reader, 8);
   readUe(&reader);                      // seq_parameter_set_id

   int isHighProfile = profile == 100 || profile == 110 || profile == 122 || profile == 244 ||
                        profile == 44  || profile == 83  || profile == 86  || profile == 118 ||
                        profile == 128 || profile == 138 || profile == 139 || profile == 134 || profile == 135;
   if (isHighProfile) {
      int chroma = readUe(&reader);
      if (chroma == 3) readBit(&reader);   // separate_colour_plane_flag
      readUe(&reader);                     // bit_depth_luma_minus8
      readUe(&reader);                     // bit_depth_chroma_minus8
      readBit(&reader);                    // qpprime_y_zero_transform_bypass_flag
      if (readBit(&reader)) {              // seq_scaling_matrix_present_flag
         int listCount = chroma == 3 ? 12 : 8;
         for (int list = 0; list < listCount; list++) {
            if (!readBit(&reader)) continue;   // scaling_list_present_flag
            int listSize = list < 6 ? 16 : 64, lastScale = 8, nextScale = 8;
            for (int j = 0; j < listSize; j++) {
               if (nextScale != 0) nextScale = (lastScale + readSe(&reader) + 256) % 256;
               lastScale = nextScale ? nextScale : lastScale;
            }
         }
      }
   }

   readUe(&reader);                      // log2_max_frame_num_minus4
   int pocType = readUe(&reader);
   if (pocType == 0) {
      readUe(&reader);                   // log2_max_pic_order_cnt_lsb_minus4
   } else if (pocType == 1) {
      readBit(&reader);                  // delta_pic_order_always_zero_flag
      readSe(&reader); readSe(&reader);  // offsets
      int cycle = (int)readUe(&reader);
      // the spec caps num_ref_frames_in_pic_order_cnt_cycle at 255; a hostile/corrupt SPS can encode a
      // value near INT_MAX, and the bit reader keeps returning 0 past end-of-buffer, so an unchecked loop
      // would spin for minutes on the decode thread. reject anything out of range instead.
      if (cycle < 0 || cycle > 255) return -1;
      for (int i = 0; i < cycle; i++) readSe(&reader);
   }

   fields->refFramesBit = reader.bitPos;
   fields->refFrames    = (int)readUe(&reader);
   readBit(&reader);                     // gaps_in_frame_num_value_allowed_flag
   int widthMbs   = (int)readUe(&reader) + 1;   // pic_width_in_mbs_minus1
   int heightUnits = (int)readUe(&reader) + 1;  // pic_height_in_map_units_minus1
   int frameMbsOnly = readBit(&reader);
   fields->frameMbs = widthMbs * heightUnits * (frameMbsOnly ? 1 : 2);
   fields->codedWidth  = widthMbs * 16;
   fields->codedHeight = heightUnits * 16 * (frameMbsOnly ? 1 : 2);
   return reader.bitPos <= rbspSize * 8 ? 0 : -1;
}

// writes `count` bits of `value` (MSB first) into `rbsp` at `bitPos`, mirroring each touched byte
// back into the emulation-encoded `raw` via `sourceIndex`.
static void writeBitsPatched(uint8_t *rbsp, uint8_t *raw, const int *sourceIndex, int bitPos, uint32_t value, int count)
{
   for (int i = 0; i < count; i++) {
      int bit = (value >> (count - 1 - i)) & 1;
      int byteIndex = (bitPos + i) >> 3, shift = 7 - ((bitPos + i) & 7);
      rbsp[byteIndex] = (uint8_t)((rbsp[byteIndex] & ~(1 << shift)) | (bit << shift));
      raw[sourceIndex[byteIndex]] = rbsp[byteIndex];
   }
}

// Exp-Golomb code length of ue(value): 2 * bits(value + 1) - 1
static int bitLengthUe(int value) { int topBits = 0; for (int v = value + 1; v; v >>= 1) topBits++; return 2 * topBits - 1; }

// PS3 cellVdec tops out at AVC level 4.2, whose DPB budget is 34816 macroblocks (H.264 Table A-1).
// A stream may declare more reference frames than fit (common in BDrip x264 --ref 5 encodes); the
// hardware then rejects the whole sequence and every picture conceals to black (ERROR_PIC). This
// rewrites max_num_ref_frames in the SPS down to what the DPB actually holds — an in-place bit patch
// when the old and new Exp-Golomb codes are the same length (covers the 5..6 -> 4 case at 1080p).
// `sps` starts at the NAL header byte. returns the capped count, 0 if no patch was needed, -1 if a
// patch was needed but impossible.
int patchSpsToDpbCap(uint8_t *sps, int size)
{
   // parse the fields that size the DPB
   if (size < 4) return 0;
   uint8_t rbsp[96]; int sourceIndex[96];
   int rbspSize = stripEmulation(sps + 1, size - 1, rbsp, sourceIndex, sizeof rbsp);
   SpsFields fields;
   if (walkSps(rbsp, rbspSize, &fields) != 0 || fields.frameMbs <= 0) return 0;

   int dpbFrames = 34816 / fields.frameMbs;
   if (dpbFrames > 16) dpbFrames = 16;
   if (dpbFrames < 1)  dpbFrames = 1;
   if (fields.refFrames <= dpbFrames) return 0;
   if (bitLengthUe(fields.refFrames) != bitLengthUe(dpbFrames)) return -1;   // would shift the rest of the SPS

   // patch, then verify by re-reading the patched bytes (also catches a disturbed emulation pattern)
   writeBitsPatched(rbsp, sps + 1, sourceIndex, fields.refFramesBit, (uint32_t)(dpbFrames + 1), bitLengthUe(dpbFrames));
   uint8_t check[96];
   int checkSize = stripEmulation(sps + 1, size - 1, check, 0, sizeof check);
   SpsFields patched;
   if (checkSize != rbspSize || walkSps(check, checkSize, &patched) != 0 || patched.refFrames != dpbFrames) return -1;
   return dpbFrames;
}

// returns the SPS's max_num_ref_frames, or 0 if it can't be parsed. `sps` starts at the NAL header byte.
static int parseSpsMaxRefFrames(const uint8_t *sps, int size)
{
   if (size < 4) return 0;
   uint8_t rbsp[96];
   int rbspSize = stripEmulation(sps + 1, size - 1, rbsp, 0, sizeof rbsp);   // skip the NAL header byte
   SpsFields fields;
   return walkSps(rbsp, rbspSize, &fields) == 0 ? fields.refFrames : 0;
}

int readH264StreamInfo(const uint8_t *annexB, int size, H264StreamInfo *info)
{
   // find the SPS (NAL type 7) and read the stream's own description out of it
   for (int i = 0; i + 4 < size; i++) {
      if (annexB[i] || annexB[i + 1] || annexB[i + 2] != 1) continue;
      const uint8_t *nal = annexB + i + 3;
      if ((nal[0] & 0x1F) != 7) continue;

      int nalSize = size - (i + 3);
      uint8_t rbsp[96];
      int rbspSize = stripEmulation(nal + 1, nalSize - 1, rbsp, 0, sizeof rbsp);   // skip the NAL header byte
      SpsFields fields;
      if (walkSps(rbsp, rbspSize, &fields) != 0) return -1;

      info->codedWidth   = fields.codedWidth;
      info->codedHeight  = fields.codedHeight;
      info->level        = fields.level;
      info->maxRefFrames = fields.refFrames;
      return 0;
   }
   return -1;
}

// appends a start code + `len` bytes of `nal` into config->header, bounds-checked. 0 / -1.
static int appendHeaderNal(H264Config *config, const uint8_t *nal, int len)
{
   if (config->headerSize + 4 + len > (int)sizeof config->header) return -1;
   memcpy(config->header + config->headerSize, START_CODE, 4);
   config->headerSize += 4;
   memcpy(config->header + config->headerSize, nal, len);
   config->headerSize += len;
   return 0;
}

int parseAvcc(const uint8_t *avcc, int size, H264Config *config)
{
   memset(config, 0, sizeof *config);
   if (size < 7 || avcc[0] != 1) return -1;   // configurationVersion must be 1

   config->nalLengthSize = (avcc[4] & 0x03) + 1;

   int pos = 5;
   int spsCount = avcc[pos++] & 0x1F;
   for (int i = 0; i < spsCount; i++) {
      if (pos + 2 > size) return -1;
      int len = (avcc[pos] << 8) | avcc[pos + 1];
      pos += 2;
      if (pos + len > size || appendHeaderNal(config, avcc + pos, len) != 0) return -1;
      uint8_t *headerSps = config->header + config->headerSize - len;
      int capped = patchSpsToDpbCap(headerSps, len);
      if (capped > 0) {
         config->spsPatched = 1;
         logWarn("[h264] SPS wants %d ref frames, over the PS3 DPB cap - patched down to %d\n", parseSpsMaxRefFrames(avcc + pos, len), capped);
      }
      else if (capped < 0) {   // the probe rejects such files with a user-facing reason
         config->refFramesExceedDpb = 1;
         logWarn("[h264] SPS ref frames exceed the PS3 DPB cap and can't be patched in place\n");
      }
      if (i == 0) config->maxRefFrames = parseSpsMaxRefFrames(headerSps, len);
      pos += len;
   }

   if (pos >= size) return -1;
   int ppsCount = avcc[pos++];
   for (int i = 0; i < ppsCount; i++) {
      if (pos + 2 > size) return -1;
      int len = (avcc[pos] << 8) | avcc[pos + 1];
      pos += 2;
      if (pos + len > size || appendHeaderNal(config, avcc + pos, len) != 0) return -1;
      pos += len;
   }

   config->valid = (config->headerSize > 0);
   return config->valid ? 0 : -1;
}

int avccToAnnexB(const uint8_t *in, int inSize, int nalLengthSize, uint8_t *out, int outCap,
                 int *isIdr, int *spsPatched)
{
   int inPos = 0, outPos = 0;
   *isIdr = 0;

   while (inPos + nalLengthSize <= inSize) {
      int nalLen = 0;
      for (int i = 0; i < nalLengthSize; i++) nalLen = (nalLen << 8) | in[inPos++];
      if (nalLen <= 0 || inPos + nalLen > inSize) break;   // trailing padding / malformed

      if (outPos + 4 + nalLen > outCap) return -1;
      memcpy(out + outPos, START_CODE, 4);
      outPos += 4;
      if ((in[inPos] & 0x1F) == 5) *isIdr = 1;   // NAL type 5 = IDR slice
      memcpy(out + outPos, in + inPos, nalLen);
      if ((in[inPos] & 0x1F) == 7) {
         int capped = patchSpsToDpbCap(out + outPos, nalLen);   // cap in-band SPS ref frames too
         if (capped > 0 && spsPatched) *spsPatched = 1;
      }
      outPos += nalLen;
      inPos  += nalLen;
   }
   return outPos;
}


// TEE Decoder compatibility checks. Keep this parser separate from the Sony SPS rewriter above:
// the latter intentionally supports streams that the TEE decoder cannot decode.
typedef struct { uint8_t data[256]; int size; int bit; int bad; } TeeRbsp;

static void teeLoadRbsp(TeeRbsp *r, const uint8_t *nal, int size)
{
   r->size = 0; r->bit = 0; r->bad = 0;
   int zeros = 0;
   for (int i = 0; i < size; i++) {
      if (zeros >= 2 && nal[i] == 3) { zeros = 0; continue; }
      zeros = nal[i] == 0 ? zeros + 1 : 0;
      if (r->size >= (int)sizeof r->data) { r->bad = 1; return; }
      r->data[r->size++] = nal[i];
   }
}

static uint32_t teeReadBit(TeeRbsp *r)
{
   if (r->bit >= r->size * 8) { r->bad = 1; return 0; }
   uint32_t value = (r->data[r->bit >> 3] >> (7 - (r->bit & 7))) & 1;
   r->bit++;
   return value;
}

static uint32_t teeReadBits(TeeRbsp *r, int count)
{
   uint32_t value = 0;
   while (count-- > 0) value = (value << 1) | teeReadBit(r);
   return value;
}

static uint32_t teeReadUe(TeeRbsp *r)
{
   int zeros = 0;
   while (!r->bad && teeReadBit(r) == 0) {
      if (++zeros > 31) { r->bad = 1; return 0; }
   }
   if (r->bad) return 0;
   return ((1u << zeros) - 1u) + teeReadBits(r, zeros);
}

static int32_t teeReadSe(TeeRbsp *r)
{
   uint32_t code = teeReadUe(r);
   return (code & 1) ? (int32_t)((code + 1) / 2) : -(int32_t)(code / 2);
}

static int teeMoreRbspData(const TeeRbsp *r)
{
   int last = r->size - 1;
   while (last >= 0 && r->data[last] == 0) last--;
   if (last < 0) return 0;
   int low = 0;
   while (low < 8 && !((r->data[last] >> low) & 1)) low++;
   if (low == 8) return 0;
   return r->bit < last * 8 + (7 - low);
}

static int teeCheckSps(TeeRbsp *r, int codedWidth, int codedHeight, char *reason, int reasonSize)
{
   int profile = (int)teeReadBits(r, 8);
   teeReadBits(r, 16);                    // constraint flags and level_idc
   teeReadUe(r);                          // seq_parameter_set_id
   if (profile != 77 && profile != 100) {
      snprintf(reason, (size_t)reasonSize, "profile %d (TEE supports Main and High)", profile);
      return 0;
   }
   if (profile == 100) {
      uint32_t chroma = teeReadUe(r);
      if (chroma != 1) {
         snprintf(reason, (size_t)reasonSize, "chroma format %u (TEE supports 4:2:0)", (unsigned)chroma);
         return 0;
      }
      uint32_t lumaDepth = teeReadUe(r), chromaDepth = teeReadUe(r);
      if (lumaDepth || chromaDepth) {
         snprintf(reason, (size_t)reasonSize, "%u-bit video (TEE supports 8-bit)", (unsigned)(8 + lumaDepth));
         return 0;
      }
      teeReadBit(r);                     // qpprime_y_zero_transform_bypass_flag
      if (teeReadBit(r)) {
         snprintf(reason, (size_t)reasonSize, "scaling matrices in SPS");
         return 0;
      }
   }
   teeReadUe(r);                         // log2_max_frame_num_minus4
   uint32_t pocType = teeReadUe(r);
   if (pocType == 0) teeReadUe(r);
   else if (pocType != 2) {
      snprintf(reason, (size_t)reasonSize, "picture order count type %u", (unsigned)pocType);
      return 0;
   }
   teeReadUe(r);                         // max_num_ref_frames
   if (teeReadBit(r)) {
      snprintf(reason, (size_t)reasonSize, "frame_num gaps are enabled");
      return 0;
   }
   uint32_t widthMbs = teeReadUe(r) + 1;
   uint32_t heightUnits = teeReadUe(r) + 1;
   if (!teeReadBit(r)) {
      snprintf(reason, (size_t)reasonSize, "interlaced stream");
      return 0;
   }
   if (widthMbs > 120 || heightUnits > 68 ||
       widthMbs * 16u != (uint32_t)codedWidth || heightUnits * 16u != (uint32_t)codedHeight) {
      snprintf(reason, (size_t)reasonSize, "SPS coded dimensions do not match %dx%d", codedWidth, codedHeight);
      return 0;
   }
   if (r->bad) {
      snprintf(reason, (size_t)reasonSize, "malformed or unsupported SPS");
      return 0;
   }
   return 1;
}

static int teeCheckPps(TeeRbsp *r, char *reason, int reasonSize)
{
   teeReadUe(r); teeReadUe(r);            // pic_parameter_set_id, seq_parameter_set_id
   if (!teeReadBit(r)) {
      snprintf(reason, (size_t)reasonSize, "CAVLC entropy coding (TEE requires CABAC)");
      return 0;
   }
   teeReadBit(r);                         // bottom_field_pic_order_in_frame_present_flag
   if (teeReadUe(r) != 0) {
      snprintf(reason, (size_t)reasonSize, "PPS slice groups");
      return 0;
   }
   teeReadUe(r); teeReadUe(r);            // num_ref_idx_l0/l1_default_active_minus1
   teeReadBit(r); teeReadBits(r, 2);      // weighted prediction
   teeReadSe(r); teeReadSe(r); teeReadSe(r);
   teeReadBit(r);                         // deblocking_filter_control_present_flag
   if (teeReadBit(r)) {
      snprintf(reason, (size_t)reasonSize, "constrained intra prediction");
      return 0;
   }
   if (teeReadBit(r)) {
      snprintf(reason, (size_t)reasonSize, "redundant pictures");
      return 0;
   }
   if (teeMoreRbspData(r)) {
      teeReadBit(r);                      // transform_8x8_mode_flag
      if (teeReadBit(r)) {
         snprintf(reason, (size_t)reasonSize, "scaling matrices in PPS");
         return 0;
      }
   }
   if (r->bad) {
      snprintf(reason, (size_t)reasonSize, "malformed or unsupported PPS");
      return 0;
   }
   return 1;
}

static int teeFindStartCode(const uint8_t *data, int size, int from, int *prefixSize)
{
   for (int i = from; i + 3 <= size; i++) {
      if (data[i] || data[i + 1]) continue;
      if (data[i + 2] == 1) { *prefixSize = 3; return i; }
      if (i + 4 <= size && data[i + 2] == 0 && data[i + 3] == 1) {
         *prefixSize = 4; return i;
      }
   }
   return -1;
}

int isH264TeeCompatible(const H264Config *config, int codedWidth, int codedHeight,
                        char *reason, int reasonSize)
{
   int spsCount = 0, ppsCount = 0, prefix = 0;
   if (reason && reasonSize > 0) reason[0] = 0;
   if (!config || !config->valid || config->headerSize <= 0) {
      if (reason && reasonSize > 0) snprintf(reason, (size_t)reasonSize, "missing SPS/PPS header");
      return 0;
   }
   if (config->spsPatched) {
      if (reason && reasonSize > 0) snprintf(reason, (size_t)reasonSize, "SPS reference count was rewritten for Sony VDEC");
      return 0;
   }
   if (codedWidth < 128 || codedWidth > 1920 || codedWidth % 128 || codedHeight < 32 || codedHeight > 1088) {
      if (reason && reasonSize > 0) snprintf(reason, (size_t)reasonSize, "coded size %dx%d is outside TEE limits", codedWidth, codedHeight);
      return 0;
   }

   int startCode = teeFindStartCode(config->header, config->headerSize, 0, &prefix);
   while (startCode >= 0) {
      int nalStart = startCode + prefix;
      int nextPrefix = 0;
      int next = teeFindStartCode(config->header, config->headerSize, nalStart, &nextPrefix);
      int nalEnd = next >= 0 ? next : config->headerSize;
      if (nalEnd > nalStart) {
         int type = config->header[nalStart] & 0x1f;
         if (type == 7 || type == 8) {
            TeeRbsp rbsp;
            teeLoadRbsp(&rbsp, config->header + nalStart + 1, nalEnd - nalStart - 1);
            if (rbsp.bad) {
               if (reason && reasonSize > 0) snprintf(reason, (size_t)reasonSize, "parameter set exceeds %d bytes", (int)sizeof rbsp.data);
               return 0;
            }
            if (type == 7) {
               spsCount++;
               if (!teeCheckSps(&rbsp, codedWidth, codedHeight, reason, reasonSize)) return 0;
            } else {
               ppsCount++;
               if (!teeCheckPps(&rbsp, reason, reasonSize)) return 0;
            }
         }
      }
      if (next < 0) break;
      startCode = next;
      prefix = nextPrefix;
   }
   if (!spsCount || !ppsCount) {
      if (reason && reasonSize > 0) snprintf(reason, (size_t)reasonSize, "missing SPS or PPS");
      return 0;
   }
   return 1;
}
