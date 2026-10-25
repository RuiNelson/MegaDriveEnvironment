#pragma once

#include "data_types.hpp"
#include <cstring>

/**
 * @file VDPState.hpp
 * @brief VDP internal state — registers, video memory, DMA, HV counter.
 */

class VDPState {
    public:
    // ── Display constants ─────────────────────────────────────────────────
    /// Default screen width in pixels (H40 mode).
    static constexpr int SCREEN_W = 320;
    /// Default screen height in pixels (NTSC visible area).
    static constexpr int SCREEN_H = 224;
    /// Mode 48 (host-only widescreen) output width in pixels: 48 tiles.
    static constexpr int WIDESCREEN_W = 384;
    /// Mode 48 output height in pixels: 27 tiles (the first of the 28 tile rows is not displayed).
    static constexpr int WIDESCREEN_H = 216;
    /// Mode 48 pixel offset of the original 320-pixel H40 picture, used to centre the window plane.
    static constexpr int WIDESCREEN_CENTER_X = (WIDESCREEN_W - SCREEN_W) / 2;
    /// Mode 48 scan lines (from the top of the 28-tile image) that are not displayed.
    static constexpr int WIDESCREEN_TOP_CROP = 8;
    /// Sprite X coordinate that maps to screen column 0 on hardware.
    static constexpr int SPRITE_ORIGIN_X = 128;
    /// Mode 48 sprite X coordinate that maps to screen column 0 (4 tiles further left than hardware).
    static constexpr int WIDESCREEN_SPRITE_ORIGIN_X = SPRITE_ORIGIN_X - WIDESCREEN_CENTER_X;
    /// Maximum framebuffer width in pixels (Mode 48).
    static constexpr int MAX_SCREEN_W = WIDESCREEN_W;
    /// Maximum progressive framebuffer height in pixels (PAL/240-line mode).
    static constexpr int MAX_PROGRESSIVE_H = 240;
    /// Maximum output height, including interlaced output.
    static constexpr int MAX_SCREEN_H = MAX_PROGRESSIVE_H * 2;
    /// Approximate 68000 master cycles per scanline.
    static constexpr int MASTER_CYCLES_PER_LINE = 3420;
    /// NTSC/PAL total scanlines.
    static constexpr int NTSC_LINES_PER_FRAME = 262;
    static constexpr int PAL_LINES_PER_FRAME  = 313;
    /// Video RAM size in bytes (64 KB).
    static constexpr int VRAM_SIZE = 0x10000;
    /// Number of color entries in CRAM (4 palettes × 16 colors).
    static constexpr int CRAM_ENTRIES = 64;
    /// Number of horizontal scroll offset values.
    static constexpr int VSRAM_ENTRIES = 40;
    /// Total VDP control registers (0–23).
    static constexpr int REG_COUNT = 24;
    /// Maximum sprite count per frame (H40 mode).
    static constexpr int SAT_MAX_SPRITES = 80;
    /// SAT region size in bytes (80 sprites × 8 bytes).
    static constexpr int SAT_SIZE = SAT_MAX_SPRITES * 8;

    // ── VDP registers ───────────────────────────────────────────────────────
    /// VDP control registers [0..23]. Each bit field controls display features.
    m_byte regs_[REG_COUNT]{};

    // ── Video memory ─────────────────────────────────────────────────────────
    /// Video RAM: tile data, nametables, hscroll table, sprite attribute table.
    m_byte vram_[VRAM_SIZE]{};
    /// CRAM: 64 color palette entries (4 palettes × 16 colors each), 16-bit BGR.
    m_word cram_[CRAM_ENTRIES]{};
    /// VSRAM: 40 vertical scroll offsets, one per plane row.
    m_word vsram_[VSRAM_ENTRIES]{};
    /// SAT shadow copy: mirrors SAT region of VRAM; updated on every VRAM write to the SAT area.
    /// Renderer reads Y/size from here so mid-frame SAT updates in VRAM don't affect the current frame.
    m_byte sat_[SAT_SIZE]{};

    // ── Control port state machine ──────────────────────────────────────────
    /// True if first word of address set has been written; awaiting second word.
    bool pendingSecondWord_ = false;
    /// First word of two-word address set command.
    m_word firstWord_ = 0;
    /// Code register (CD5–CD0): memory destination (VRAM/CRAM/VSRAM) and operation type.
    m_byte code_ = 0;
    /// Address register: 16-bit offset into VRAM/CRAM/VSRAM for next read/write.
    m_word address_ = 0;

    // ── DMA state ───────────────────────────────────────────────────────────
    /// True if DMA fill operation awaiting data word on data port.
    bool dmaFillPending_ = false;

    // ── Status register ─────────────────────────────────────────────────────
    /// Status register: Bit 9=FIFO empty (always 1), Bit 7–6=always 1, Bit 3=VBlank flag, Bit 1=DMA busy (always 0).
    m_word status_ = 0x34C0;
    /// Cycle at which the current DMA operation is considered complete. Zero means no DMA is active.
    uint64_t dmaEndCycle_ = 0;
    /// Estimated cycles at which queued FIFO slots drain. Used for status full/empty flags.
    uint64_t fifoDrainCycle_[4]{};
    /// FIFO write index for status estimation.
    int fifoIndex_ = 0;

    // ── HV counter ──────────────────────────────────────────────────────────
    /// Vertical counter: current scanline (0–261 in NTSC). Updated each frame.
    m_word vCounter_ = 0;
    /// Horizontal counter: position within scanline. Not fully emulated; simplified.
    m_word hCounter_ = 0;

    // ── Read-ahead buffer ───────────────────────────────────────────────────
    /// Hardware read-ahead: holds value from previous data port read.
    m_word readBuffer_ = 0;
    /// Current interlace field. Toggled once per rendered frame in interlace modes.
    bool oddFrame_ = false;

    // ── Host-only widescreen ────────────────────────────────────────────────
    /// Host setting for Mode 48. Not a register and not cleared by reset(): it takes effect whenever the
    /// game selects H40.
    bool widescreen_ = false;

    // ── Register accessors ──────────────────────────────────────────────────

    /// Plane A base address in VRAM (bits 13–10 of register $02).
    int planeABase() const;

    /// Plane B base address in VRAM (bits 2–0 of register $04).
    int planeBBase() const;

    /// Window plane base address in VRAM (bits 5–1 of register $03).
    int windowBase() const;

    /// Sprite attribute table base address in VRAM (bits 6–0 of register $05). In H40 mode, bit 0 is
    /// ignored (hardware quirk), so the base is forced to a multiple of 0x400 instead of 0x200.
    int satBase() const;

    /// Horizontal scroll table base address in VRAM (bits 5–0 of register $0D).
    int hscrollBase() const;

    /// Plane width in cells: 32 (default), 64, or 128. From register $10 bits 1–0.
    int planeWidthCells() const;

    /// Plane height in cells: 32 (default), 64, or 128. From register $10 bits 5–4.
    int planeHeightCells() const;

    /// Auto-increment value for address register after each data port access. From register $0F.
    int autoIncrement() const;

    /// Background color palette index (0–3). From register $07 bits 5–4.
    int bgColorPalette() const;

    /// Background color index within palette (0–15). From register $07 bits 3–0.
    int bgColorIndex() const;

    /// Horizontal scroll mode (0=full screen, 2=per-8-scanline, 3=per-scanline). From register $0B bits 1–0.
    int hscrollMode() const;

    /// Vertical scroll mode (0=full screen, 1=per-2-cell). From register $0B bit 2.
    int vscrollMode() const;

    /// True if display is enabled (show image). From register $01 bit 6.
    bool displayEnabled() const;

    /// True if DMA is enabled. From register $01 bit 4.
    bool dmaEnabled() const;

    /// True if VBlank interrupt is enabled. From register $01 bit 5.
    bool vblankIRQEnabled() const;

    /// True if HBlank interrupt is enabled. From register $00 bit 4.
    bool hintEnabled() const;

    /// True when all three CRAM bits per color channel are used. From register $00 bit 2.
    bool fullColorPaletteEnabled() const;

    /// Reload value for the horizontal interrupt down-counter. From register $0A.
    int hintReloadValue() const;

    /// Window plane horizontal position in cells (0–31). From register $11 bits 4–0.
    int windowHPos() const;

    /// Window plane vertical position in cells (0–31). From register $12 bits 4–0.
    int windowVPos() const;

    /// True if window is positioned on right side; false=left. From register $11 bit 7.
    bool windowRight() const;

    /// True if window is positioned on bottom; false=top. From register $12 bit 7.
    bool windowDown() const;

    /// True in H40 mode (320 pixels); false in H32 mode (256 pixels). From register $0C bit 0.
    bool h40Mode() const;

    /// True when Mode 48 is in effect: the widescreen host setting is on and the game selected H40.
    bool widescreenActive() const;

    /// Width in pixels of the hardware display (256 or 320), also in Mode 48. Sprite limits and the window
    /// plane follow this width.
    int hardwareWidth() const;

    /// Output width in pixels (256, 320, or 384 in Mode 48).
    int activeWidth() const;

    /// Output pixel column of the original picture's left edge (0, or 32 in Mode 48).
    int centerOffsetX() const;

    /// Sprite X coordinate that maps to output column 0 (128, or 96 in Mode 48).
    int spriteOriginX() const;

    /// Scan lines at the top of the rendered image that are not output (0, or 8 in Mode 48).
    int topCropLines() const;

    /// Active progressive display height in pixels (224 or 240 for Mega Drive mode 5).
    int activeHeight() const;

    /// Active output height after cropping, doubled for interlaced output.
    int activeOutputHeight() const;

    /// Interlace mode from register $0C bits 2–1 (0=off, 1=interlace 1, 2=interlace 2).
    int interlaceMode() const;

    /// True when either interlace mode is enabled.
    bool interlaced() const;

    /// True when shadow/highlight mode is enabled. From register $0C bit 3.
    bool shadowHighlightEnabled() const;

    /// Total scanlines per frame for the selected video standard.
    int linesPerFrame(bool pal) const;

    /// Updates vCounter_/hCounter_ from an approximate 68K master cycle timestamp.
    void updateCountersFromCycles(uint64_t masterCycles, bool pal);

    // ── Memory reset ────────────────────────────────────────────────────────

    /// Resets all memory, registers, and state to initial values.
    void reset();
};
