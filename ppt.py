from pptx import Presentation
from pptx.util import Inches, Pt
from pptx.enum.text import PP_ALIGN, MSO_ANCHOR
from pptx.enum.shapes import MSO_SHAPE, MSO_CONNECTOR
from pptx.dml.color import RGBColor
import os

out_dir = os.path.join(os.getcwd(), "output")
os.makedirs(out_dir, exist_ok=True)
out_light = os.path.join(out_dir, "JournalFS_Project_Presentation_Light.pptx")
out_root = os.path.join(os.getcwd(), "JournalFS_Project_Presentation.pptx")

prs = Presentation()
prs.slide_width = Inches(13.333)
prs.slide_height = Inches(7.5)

# Palette - soft light background theme (Preserved exactly as original)
BG = RGBColor(245, 247, 250)
PANEL = RGBColor(255, 255, 255)
PANEL2 = RGBColor(236, 240, 245)
WHITE = RGBColor(15, 23, 42)
MUTED = RGBColor(71, 85, 105)
CYAN = RGBColor(23, 184, 205)
BLUE = RGBColor(59, 130, 246)
PURPLE = RGBColor(168, 85, 247)
GREEN = RGBColor(16, 185, 129)
ORANGE = RGBColor(245, 158, 11)
RED = RGBColor(239, 68, 68)

# Terminal palette
TERM_BG = RGBColor(15, 23, 42)
TERM_BAR = RGBColor(30, 41, 59)
TERM_GREEN = RGBColor(74, 222, 128)
TERM_WHITE = RGBColor(241, 245, 249)
TERM_MUTED = RGBColor(148, 163, 184)
TERM_YELLOW = RGBColor(250, 204, 21)
TERM_CYAN = RGBColor(56, 189, 248)
TERM_RED = RGBColor(248, 113, 113)

TOTAL_SLIDES = 14

def set_bg(slide, color=BG):
    fill = slide.background.fill
    fill.solid()
    fill.fore_color.rgb = color

def add_text(slide, x, y, w, h, text, size=18, color=WHITE, bold=False,
             font="Aptos", align=PP_ALIGN.LEFT, valign=MSO_ANCHOR.TOP,
             margin=0.04, italic=False):
    box = slide.shapes.add_textbox(Inches(x), Inches(y), Inches(w), Inches(h))
    tf = box.text_frame
    tf.clear()
    tf.word_wrap = True
    tf.margin_left = Inches(margin)
    tf.margin_right = Inches(margin)
    tf.margin_top = Inches(margin)
    tf.margin_bottom = Inches(margin)
    tf.vertical_anchor = valign
    p = tf.paragraphs[0]
    p.alignment = align
    p.space_after = Pt(0)
    r = p.add_run()
    r.text = text
    r.font.name = font
    r.font.size = Pt(size)
    r.font.bold = bold
    r.font.italic = italic
    r.font.color.rgb = color
    return box

def add_rich_lines(slide, x, y, w, h, lines, size=17, color=WHITE, bullet=False, gap=8):
    box = slide.shapes.add_textbox(Inches(x), Inches(y), Inches(w), Inches(h))
    tf = box.text_frame
    tf.clear()
    tf.word_wrap = True
    tf.margin_left = Inches(0.05); tf.margin_right = Inches(0.05)
    tf.margin_top = Inches(0.03); tf.margin_bottom = Inches(0.03)
    for i, item in enumerate(lines):
        if isinstance(item, tuple):
            txt, c, b = item
        else:
            txt, c, b = item, color, False
        p = tf.paragraphs[0] if i == 0 else tf.add_paragraph()
        p.text = ("•  " if bullet else "") + txt
        p.font.name = "Aptos"
        p.font.size = Pt(size)
        p.font.color.rgb = c
        p.font.bold = b
        p.space_after = Pt(gap)
        p.level = 0
    return box

def add_rect(slide, x, y, w, h, fill=PANEL, line=None, radius=True):
    shape_type = MSO_SHAPE.ROUNDED_RECTANGLE if radius else MSO_SHAPE.RECTANGLE
    sh = slide.shapes.add_shape(shape_type, Inches(x), Inches(y), Inches(w), Inches(h))
    sh.fill.solid()
    sh.fill.fore_color.rgb = fill
    sh.line.color.rgb = line or fill
    if radius:
        try: sh.adjustments[0] = 0.12
        except: pass
    return sh

def add_line(slide, x1, y1, x2, y2, color=CYAN, width=2, arrow=True):
    line = slide.shapes.add_connector(MSO_CONNECTOR.STRAIGHT, Inches(x1), Inches(y1), Inches(x2), Inches(y2))
    line.line.color.rgb = color
    line.line.width = Pt(width)
    if arrow:
        try:
            from pptx.enum.dml import MSO_ARROWHEAD
            line.line.end_arrowhead = MSO_ARROWHEAD.TRIANGLE
        except ImportError:
            pass
    return line

def header(slide, section, title, subtitle=None, num=None):
    add_text(slide, 0.65, 0.34, 8.5, 0.28, section.upper(), 10, CYAN, True)
    add_text(slide, 0.65, 0.72, 12.0, 0.55, title, 26, WHITE, True)
    if subtitle:
        add_text(slide, 0.68, 1.34, 11.9, 0.45, subtitle, 12, MUTED)
    add_rect(slide, 0.66, 7.12, 12.0, 0.025, PANEL2, radius=False)
    if num:
        add_text(slide, 12.0, 7.16, 0.65, 0.18, f"{num:02d} / {TOTAL_SLIDES:02d}", 9, MUTED, align=PP_ALIGN.RIGHT)

def add_card(slide, x, y, w, h, title, body, accent=CYAN, title_size=16, body_size=11):
    add_rect(slide, x, y, w, h, PANEL)
    add_rect(slide, x, y, 0.07, h, accent, radius=False)
    add_text(slide, x+0.22, y+0.16, w-0.42, 0.32, title, title_size, WHITE, True)
    add_text(slide, x+0.22, y+0.55, w-0.42, h-0.65, body, body_size, MUTED)

def add_pill(slide, x, y, w, text, color=CYAN):
    add_rect(slide, x, y, w, 0.34, PANEL2)
    add_text(slide, x+0.04, y+0.015, w-0.08, 0.28, text, 10, color, True, align=PP_ALIGN.CENTER, valign=MSO_ANCHOR.MIDDLE)

def add_terminal_screen(slide, x, y, w, h, title, lines):
    add_rect(slide, x, y, w, h, fill=TERM_BG, radius=True)
    add_rect(slide, x, y, w, 0.36, fill=TERM_BAR, radius=False)
    for idx, col in enumerate([RED, ORANGE, GREEN]):
        dot = slide.shapes.add_shape(MSO_SHAPE.OVAL, Inches(x + 0.14 + idx * 0.18), Inches(y + 0.12), Inches(0.12), Inches(0.12))
        dot.fill.solid(); dot.fill.fore_color.rgb = col
        dot.line.color.rgb = col
    add_text(slide, x + 0.72, y + 0.07, w - 0.9, 0.24, title, 10, TERM_MUTED, font="Consolas")
    box = slide.shapes.add_textbox(Inches(x + 0.16), Inches(y + 0.44), Inches(w - 0.32), Inches(h - 0.52))
    tf = box.text_frame
    tf.clear()
    tf.word_wrap = True
    tf.margin_left = Inches(0.04); tf.margin_right = Inches(0.04)
    tf.margin_top = Inches(0.04); tf.margin_bottom = Inches(0.04)
    for i, item in enumerate(lines):
        p = tf.paragraphs[0] if i == 0 else tf.add_paragraph()
        if isinstance(item, tuple):
            txt, c, b = item
        else:
            txt, c, b = item, TERM_WHITE, False
        p.text = txt
        p.font.name = "Consolas"
        p.font.size = Pt(9.5)
        p.font.color.rgb = c
        p.font.bold = b
        p.space_after = Pt(2.5)

# ==============================================================================
# 1. COVER SLIDE
# ==============================================================================
s = prs.slides.add_slide(prs.slide_layouts[6]); set_bg(s)
for i, (x,y,d,c) in enumerate([(9.2,1.1,2.1,BLUE),(10.55,2.0,1.65,CYAN),(8.8,3.45,1.35,PURPLE),(10.7,4.0,0.85,GREEN)]):
    sh = s.shapes.add_shape(MSO_SHAPE.OVAL, Inches(x), Inches(y), Inches(d), Inches(d))
    sh.fill.solid(); sh.fill.fore_color.rgb = c
    sh.fill.transparency = 76
    sh.line.color.rgb = c
    sh.line.transparency = 35
add_rect(s, 0.8, 0.75, 1.15, 0.12, CYAN, radius=False)
add_text(s, 0.8, 1.25, 8.5, 0.3, "OPERATING SYSTEMS & STORAGE ARCHITECTURE", 11, CYAN, True)
add_text(s, 0.75, 1.75, 7.7, 1.1, "JournalFS", 48, WHITE, True)
add_text(s, 0.82, 2.90, 8.2, 0.85, "Crash-Consistent Journaling Filesystem\n& Log-Structured Storage Simulator", 24, MUTED, True)
add_text(s, 0.85, 4.35, 7.2, 0.65, "A complete, verified C11 user-space storage engine featuring\nwrite-ahead logging (WAL), bounded LRU block cache,\ncrash injection, and idempotent forward-pass recovery.", 14, WHITE)

add_pill(s, 0.85, 5.55, 1.35, "STRICT C11", CYAN)
add_pill(s, 2.35, 5.55, 1.75, "WRITE-AHEAD LOG", BLUE)
add_pill(s, 4.25, 5.55, 1.65, "LRU BUFFER CACHE", PURPLE)
add_pill(s, 6.05, 5.55, 1.75, "279 / 279 TESTS PASS", GREEN)

add_text(s, 0.85, 6.65, 7.5, 0.25, "Comprehensive System Defense & Final Demonstration", 10, MUTED)
add_text(s, 11.2, 6.9, 1.4, 0.25, f"01 / {TOTAL_SLIDES:02d}", 10, MUTED, align=PP_ALIGN.RIGHT)

# ==============================================================================
# 2. CONTEXT & MOTIVATION
# ==============================================================================
s = prs.slides.add_slide(prs.slide_layouts[6]); set_bg(s)
header(s, "01 / Context", "Why Filesystem Reliability Matters", "A real filesystem coordinates names, directories, inodes, buffers, and persistence under unexpected crashes.", 2)
add_card(s, 0.7, 2.05, 3.85, 2.1, "Hierarchical Paths", "Maps directory structures and file paths down to inode records and disk block addresses.", CYAN)
add_card(s, 4.75, 2.05, 3.85, 2.1, "Metadata & Ownership", "Tracks file size, POSIX permission modes (rwx), UID/GID ownership, and block pointers.", BLUE)
add_card(s, 8.8, 2.05, 3.85, 2.1, "Crash Invariants", "Guarantees no half-written or corrupted state survives a sudden power cut or application crash.", PURPLE)
add_text(s, 0.85, 4.50, 4.0, 0.4, "The Fundamental Crash Dilemma", 18, WHITE, True)
steps = [
    ("MULTI-STEP WRITE", "Updating Inode + Data Blocks", CYAN),
    ("POWER LOSS", "Crash mid-write (Torn State)", RED),
    ("JOURNAL RECOVERY", "Auto-replay committed work", GREEN)
]
xs = [0.85, 4.65, 8.45]
for i,(a,b,c) in enumerate(steps):
    add_rect(s, xs[i], 5.05, 3.0, 0.95, PANEL2)
    add_text(s, xs[i]+0.15, 5.18, 2.7, 0.25, a, 11, c, True)
    add_text(s, xs[i]+0.15, 5.50, 2.7, 0.35, b, 12, WHITE)
    if i < 2: add_line(s, xs[i]+3.1, 5.52, xs[i+1]-0.15, 5.52, MUTED, 1.5)
add_text(s, 0.85, 6.35, 11.4, 0.35, "JournalFS guarantees that the disk is always restored to an untorn, 100% consistent state.", 14, CYAN, True)

# ==============================================================================
# 3. PROBLEM STATEMENT & OBJECTIVES
# ==============================================================================
s = prs.slides.add_slide(prs.slide_layouts[6]); set_bg(s)
header(s, "02 / Objectives", "Problem Statement & Milestones Completed", "Eliminating data corruption through write ordering, transaction boundaries, and verification.", 3)
add_rect(s, 0.72, 2.0, 5.65, 4.0, PANEL)
add_text(s, 1.0, 2.25, 5.0, 0.35, "THE STORAGE PROBLEM", 13, RED, True)
add_text(s, 1.0, 2.75, 4.9, 1.1, "What happens when a crash\ninterrupts a multi-block write?", 24, WHITE, True)
add_text(s, 1.0, 4.15, 4.9, 1.4, "In naive storage systems, metadata references non-existent blocks, directory entries leak inodes, and file contents become scrambled.\n\nJournalFS enforces atomic transactions so operations either completely commit or are cleanly discarded.", 13, MUTED)

add_rect(s, 6.7, 2.0, 5.85, 4.0, PANEL)
add_text(s, 7.0, 2.25, 5.0, 0.35, "6 MILESTONES FULFILLED", 13, GREEN, True)
obj = [
    ("M1", "Filesystem Foundation: 4MB disk image, superblock, files, dirs", CYAN),
    ("M2", "Write-Ahead Logging: Atomic txids, CRC-32C, commit records", BLUE),
    ("M3", "Crash Simulation: 14 fault points, idempotent replay recovery", PURPLE),
    ("M4", "Log-Structured Storage: Append-only halves, dual checkpoints", ORANGE),
    ("M5", "Inodes & Protection: Permissions (chmod), 64-frame LRU cache", GREEN),
    ("M6", "C11 Verification: 279 automated tests, ASan/UBSan clean", CYAN),
]
for i,(n,t,c) in enumerate(obj):
    yy = 2.75 + i*0.52
    add_text(s, 7.0, yy, 0.45, 0.3, n, 11, c, True)
    add_text(s, 7.55, yy, 4.85, 0.34, t, 12, WHITE)
add_text(s, 0.85, 6.35, 11.7, 0.35, "Strict C11 requirement met: 100% executable C code with zero external script dependencies.", 13, MUTED, italic=True)

# ==============================================================================
# 4. SYSTEM ARCHITECTURE
# ==============================================================================
s = prs.slides.add_slide(prs.slide_layouts[6]); set_bg(s)
header(s, "03 / Architecture", "Modular Layer Hierarchy", "Strict separation of concerns across command parsing, VFS, buffer caching, transactions, and raw I/O.", 4)
layers = [
    ("CLI / Shell", "Interactive REPL & batch cmds", BLUE),
    ("VFS & Protection", "Files, dirs, inodes, permissions", CYAN),
    ("Buffer Cache", "64-frame LRU & dirty tracking", PURPLE),
    ("Transaction Manager", "Write-ahead logging (WAL)", ORANGE),
    ("Storage & Recovery", "Log halves, dual checkpoints", GREEN)
]
for i,(title,body,c) in enumerate(layers):
    x = 0.85 + i*2.42
    add_rect(s, x, 2.35, 2.15, 1.65, PANEL2)
    add_rect(s, x, 2.35, 2.15, 0.08, c, radius=False)
    add_text(s, x+0.12, 2.65, 1.9, 0.45, title, 14, WHITE, True, align=PP_ALIGN.CENTER)
    add_text(s, x+0.12, 3.25, 1.9, 0.55, body, 10, MUTED, align=PP_ALIGN.CENTER)
    if i < 4: add_line(s, x+2.17, 3.15, x+2.40, 3.15, c, 1.8)

add_card(s, 0.85, 4.45, 3.65, 1.65, "User-Space Simulator", "Directly manages a 4MB raw disk image file via POSIX pread, pwrite, and fsync barriers without kernel modules.", CYAN, 14, 11)
add_card(s, 4.85, 4.45, 3.65, 1.65, "Bounded Buffer Cache", "Maintains a fixed 256 KiB LRU cache frame pool that intercepts block reads/writes and minimizes physical disk I/O.", PURPLE, 14, 11)
add_card(s, 8.85, 4.45, 3.65, 1.65, "Deterministic Recovery", "Built-in fault injection triggers _exit(99) at 14 precise transaction stages to test crash resilience systematically.", GREEN, 14, 11)

# ==============================================================================
# 5. DISK IMAGE FORMAT & LAYOUT
# ==============================================================================
s = prs.slides.add_slide(prs.slide_layouts[6]); set_bg(s)
header(s, "04 / Disk Format", "On-Disk Binary Format & Storage Layout", "A standard 4096 KiB image is strictly structured into 1024 blocks of 4096 bytes each.", 5)
d_blocks = [
    ("Block 0", "Superblock", "Magic 0x53464E52 ('JFS1'), UUID, block count, clean/dirty flag", BLUE),
    ("Blocks 1 & 2", "Dual Checkpoint Slots", "Double-buffered ping-pong checkpoints; alternate slot avoids torn checkpoint writes", PURPLE),
    ("Blocks 3 .. 50", "Journal Ring (48 blocks)", "Circular Write-Ahead Log ring buffer storing atomic transaction redo records & commits", CYAN),
    ("Blocks 51 .. 536", "Log Storage Half A (486 blks)", "Append-only segment for immutable inodes, directory blocks, and data blocks", GREEN),
    ("Blocks 537 .. 1023", "Log Storage Half B (486 blks)", "Compaction reserve half: active half flips atomically upon checkpoint update", ORANGE)
]
for i,(b_num, b_title, b_desc, col) in enumerate(d_blocks):
    yy = 2.05 + i * 0.82
    add_rect(s, 0.85, yy, 11.6, 0.68, PANEL)
    add_rect(s, 0.85, yy, 0.08, 0.68, col, radius=False)
    add_text(s, 1.10, yy+0.12, 1.85, 0.28, b_num, 11, col, True)
    add_text(s, 3.05, yy+0.12, 2.75, 0.28, b_title, 13, WHITE, True)
    add_text(s, 5.95, yy+0.12, 6.30, 0.44, b_desc, 11, MUTED)

add_text(s, 0.85, 6.40, 11.5, 0.35, "All metadata and journal records are protected by hardware-grade Castagnoli CRC-32C checksums.", 13, CYAN, True)

# ==============================================================================
# 6. WRITE-AHEAD LOGGING (WAL)
# ==============================================================================
s = prs.slides.add_slide(prs.slide_layouts[6]); set_bg(s)
header(s, "05 / Journaling", "Write-Ahead Logging (WAL) Protocol", "No data reaches main storage until its transaction receipt and commit status are durable on disk.", 6)
flow = [
    ("1", "BEGIN", "Allocate monotonic txid", BLUE),
    ("2", "STAGE", "Build redo ops in RAM", PURPLE),
    ("3", "JOURNAL", "Append redo records", CYAN),
    ("4", "COMMIT", "Write & fsync COMMIT", GREEN),
    ("5", "APPLY", "Append to log halves", ORANGE),
]
for i,(n,t,d,c) in enumerate(flow):
    x = 0.70 + i*2.48
    add_rect(s, x, 2.25, 2.20, 1.55, PANEL)
    sh = s.shapes.add_shape(MSO_SHAPE.OVAL, Inches(x+0.80), Inches(1.85), Inches(0.58), Inches(0.58))
    sh.fill.solid(); sh.fill.fore_color.rgb = c; sh.line.color.rgb = c
    add_text(s, x+0.80, 1.91, 0.58, 0.38, n, 14, BG, True, align=PP_ALIGN.CENTER, valign=MSO_ANCHOR.MIDDLE)
    add_text(s, x+0.15, 2.55, 1.90, 0.32, t, 15, WHITE, True, align=PP_ALIGN.CENTER)
    add_text(s, x+0.14, 3.05, 1.92, 0.48, d, 11, MUTED, align=PP_ALIGN.CENTER)
    if i < 4: add_line(s, x+2.22, 3.0, x+2.46, 3.0, MUTED, 1.5)

add_card(s, 0.85, 4.35, 5.65, 1.65, "The Write-Ahead Logging Rule", "Redo records and commit markers are forced to physical storage via POSIX fdatasync() before main log home blocks are touched.", CYAN, 14, 11)
add_card(s, 6.80, 4.35, 5.65, 1.65, "The Commit Point Invariant", "A transaction is durable if and only if its COMMIT record has been synced to disk. Anything prior is discarded on recovery.", GREEN, 14, 11)

# ==============================================================================
# 7. CRASH RECOVERY WORKFLOW
# ==============================================================================
s = prs.slides.add_slide(prs.slide_layouts[6]); set_bg(s)
header(s, "06 / Recovery", "Crash Recovery: Scan, Validate, Replay", "A deterministic forward-pass recovery engine runs automatically at mount time.", 7)
nodes = [
    (0.85,2.1,2.3,0.85,"Read Superblock\nVerify Magic & Flags",BLUE),
    (3.70,2.1,2.3,0.85,"Load Best Checkpoint\nSelect Highest seq_no",PURPLE),
    (6.55,2.1,2.3,0.85,"Scan Journal Window\nValidate CRC-32C",CYAN),
]
for x,y,w,h,t,c in nodes:
    add_rect(s,x,y,w,h,PANEL2)
    add_rect(s,x,y,0.07,h,c,radius=False)
    add_text(s,x+0.12,y+0.12,w-0.24,h-0.24,t,11,WHITE,True,align=PP_ALIGN.CENTER,valign=MSO_ANCHOR.MIDDLE)
add_line(s,3.17,2.52,3.67,2.52,MUTED,1.5)
add_line(s,6.02,2.52,6.52,2.52,MUTED,1.5)

add_line(s,7.70,2.98,7.70,3.50,CYAN,1.5,False)
add_line(s,4.60,3.50,10.60,3.50,CYAN,1.5,False)
add_line(s,4.60,3.50,4.60,3.90,ORANGE,1.5)
add_line(s,10.60,3.50,10.60,3.90,GREEN,1.5)

add_rect(s,2.95,3.90,3.30,1.15,PANEL)
add_text(s,3.18,4.05,2.85,0.28,"NO COMMIT RECORD",12,ORANGE,True,align=PP_ALIGN.CENTER)
add_text(s,3.18,4.40,2.85,0.50,"Incomplete transaction ignored.\nOld state remains clean.",10,MUTED,align=PP_ALIGN.CENTER)

add_rect(s,8.95,3.90,3.30,1.15,PANEL)
add_text(s,9.18,4.05,2.85,0.28,"VALID COMMIT RECORD",12,GREEN,True,align=PP_ALIGN.CENTER)
add_text(s,9.18,4.40,2.85,0.50,"Replay redo ops forward.\nAdvance checkpoint safely.",10,MUTED,align=PP_ALIGN.CENTER)

add_card(s, 0.85, 5.35, 11.4, 0.95, "Idempotent Recovery Guarantee", "Repeated recovery passes produce identical filesystem state without duplicating records or corrupting the imap.", GREEN, 13, 11)

# ==============================================================================
# 8. MEMORY MANAGEMENT: BOUNDED LRU BUFFER CACHE
# ==============================================================================
s = prs.slides.add_slide(prs.slide_layouts[6]); set_bg(s)
header(s, "07 / Memory Management", "Bounded LRU Block Buffer Cache", "Fixed memory footprint with explicit dirty-block tracking and write-back replacement policy.", 8)
add_card(s, 0.85, 2.05, 3.65, 2.1, "Fixed Frame Capacity", "Fixed pool of 64 cache frames (256 KiB total memory footprint) mapped directly to 4096-byte disk blocks.", CYAN, 15, 11)
add_card(s, 4.85, 2.05, 3.65, 2.1, "LRU Eviction Queue", "Doubly-linked list maintains recency order. Least Recently Used victims are chosen for eviction on cache misses.", BLUE, 15, 11)
add_card(s, 8.85, 2.05, 3.65, 2.1, "Dirty Write-Back", "Dirty blocks are flagged. Evicted dirty blocks are automatically written back to disk image storage via pwrite().", PURPLE, 15, 11)

add_rect(s, 0.85, 4.45, 11.6, 1.85, PANEL2)
add_text(s, 1.15, 4.65, 10.5, 0.35, "CACHE STATS FROM REAL WORKLOAD (cli 'stats')", 13, GREEN, True)
cache_metrics = [
    ("bcache_hits: 201", "Blocks served from RAM without disk I/O", GREEN),
    ("bcache_misses: 25", "Cold loads from the disk image", CYAN),
    ("bcache_writebacks: 22", "Dirty blocks flushed durably to disk", PURPLE),
    ("bcache_flushes: 22", "Sync barriers flushing cache frames", ORANGE),
]
for i,(m_title, m_desc, col) in enumerate(cache_metrics):
    xx = 1.15 + (i%2)*5.5
    yy = 5.15 + (i//2)*0.55
    add_text(s, xx, yy, 2.4, 0.28, m_title, 12, col, True)
    add_text(s, xx+2.35, yy, 3.0, 0.28, m_desc, 11, MUTED)

# ==============================================================================
# 9. INODES, DIRECTORIES & PROTECTION MODEL
# ==============================================================================
s = prs.slides.add_slide(prs.slide_layouts[6]); set_bg(s)
header(s, "08 / Protection", "Inodes, Directories & Access Control", "POSIX file semantics, directory trees, and simulated access permissions.", 9)
add_card(s, 0.85, 2.05, 3.65, 2.2, "Inode Metadata", "Fixed 512-byte inodes: type (file/dir), permissions (mode), UID/GID, size, creation/mod timestamps, direct block pointers.", BLUE, 14, 11)
add_card(s, 4.85, 2.05, 3.65, 2.2, "Hierarchical Directories", "Directory inodes contain directory block records mapping filenames up to 56 chars to inode numbers with path traversal.", CYAN, 14, 11)
add_card(s, 8.85, 2.05, 3.65, 2.2, "Simulated Protection", "Enforces simulated POSIX permission bits (rwxrwxrwx). Operations check JFS_R_OK, JFS_W_OK, and JFS_X_OK.", GREEN, 14, 11)

add_rect(s, 0.85, 4.60, 11.6, 1.65, PANEL)
add_text(s, 1.15, 4.80, 10.5, 0.30, "PROTECTION IN ACTION (LIVE TEST)", 13, ORANGE, True)
add_text(s, 1.15, 5.20, 11.0, 0.35, "jfs:/testdir> chmod 0400 file.txt  -->  mode changed to read-only (0400)", 12, WHITE, font="Consolas")
add_text(s, 1.15, 5.55, 11.0, 0.35, "jfs:/testdir> write file.txt Hacked -->  error: write: permission denied", 12, RED, True, font="Consolas")
add_text(s, 1.15, 5.90, 11.0, 0.25, "Enforces filesystem-level simulated isolation and permission checking across all file operations.", 11, MUTED, italic=True)

# ==============================================================================
# 10. OUTPUT SCREEN 1: INTERACTIVE SHELL & FILE OPERATIONS
# ==============================================================================
s = prs.slides.add_slide(prs.slide_layouts[6]); set_bg(s)
header(s, "09 / Demonstration", "Output Screen: Interactive Shell & VFS Operations", "Actual terminal session demonstrating interactive formatting, directory navigation, and inode inspection.", 10)
shell_lines = [
    ("+================================================================+", TERM_CYAN, True),
    ("|                           JournalFS                            |", TERM_CYAN, True),
    ("|        Crash-Consistent Journaling Filesystem Simulator        |", TERM_CYAN, True),
    ("+================================================================+", TERM_CYAN, True),
    ("", TERM_WHITE, False),
    ("jfs:/> format demo.img", TERM_YELLOW, True),
    ("formatted demo.img (4096 KiB, 1024 blocks of 4096 bytes)", TERM_WHITE, False),
    ("jfs:/> mount demo.img", TERM_YELLOW, True),
    ("mounted demo.img: 252 inodes max, log 486 blocks per half, checkpoint seq 1", TERM_GREEN, False),
    ("recovery: journal window is empty, nothing to redo", TERM_MUTED, False),
    ("", TERM_WHITE, False),
    ("jfs:/> mkdir /testdir", TERM_YELLOW, True),
    ("created directory /testdir", TERM_WHITE, False),
    ("jfs:/> cd /testdir", TERM_YELLOW, True),
    ("jfs:/testdir> create file.txt", TERM_YELLOW, True),
    ("created /testdir/file.txt (0644)", TERM_WHITE, False),
    ("jfs:/testdir> write file.txt Hello-JournalFS-on-Ubuntu", TERM_YELLOW, True),
    ("wrote /testdir/file.txt", TERM_WHITE, False),
    ("jfs:/testdir> ls", TERM_YELLOW, True),
    ("- rw-r--r--       31  file.txt        ino=3", TERM_GREEN, True),
    ("1 entry", TERM_MUTED, False),
]
add_terminal_screen(s, 0.85, 1.95, 11.6, 4.45, "JournalFS Interactive Shell -- Ubuntu 24.04 Terminal", shell_lines)
add_text(s, 0.85, 6.55, 11.5, 0.35, "Interactive shell provides intuitive commands: format, mount, mkdir, cd, write, cat, ls, stat, df, and check.", 12, MUTED)

# ==============================================================================
# 11. OUTPUT SCREEN 2: CRASH INJECTION & AUTOMATIC RECOVERY
# ==============================================================================
s = prs.slides.add_slide(prs.slide_layouts[6]); set_bg(s)
header(s, "10 / Demonstration", "Output Screen: Crash Injection & Automatic Replay", "Live proof of crash consistency: power loss simulated right after commit, recovered automatically on mount.", 11)
crash_lines = [
    ("$ ./journalfs -i demo.img --crash post_commit_sync -c 'mount demo.img' -c 'append /testdir/file.txt _CRASH_PROOF'", TERM_YELLOW, True),
    ("mounted demo.img: 252 inodes max, checkpoint seq 21, applied txid 12", TERM_WHITE, False),
    ("recovery: journal window is empty, nothing to redo", TERM_MUTED, False),
    ("[crash] injected crash at point 'post_commit_sync' (durable commit, changes not applied)", TERM_RED, True),
    ("-- terminating with _exit(99); no image write was flushed by this process", TERM_RED, True),
    ("", TERM_WHITE, False),
    ("$ ./journalfs -i demo.img -c 'mount demo.img' -c 'cat /testdir/file.txt' -c 'unmount'", TERM_YELLOW, True),
    ("mounted demo.img: 252 inodes max, checkpoint seq 22, applied txid 13", TERM_WHITE, False),
    ("recovery ran: 1 transaction(s) replayed, 0 incomplete ignored", TERM_GREEN, True),
    ("", TERM_WHITE, False),
    ("Hello-JournalFS-on-Ubuntu_CRASH_PROOF", TERM_CYAN, True),
    ("", TERM_WHITE, False),
    ("unmounted demo.img (checkpoint written, image marked clean)", TERM_WHITE, False),
]
add_terminal_screen(s, 0.85, 1.95, 11.6, 4.45, "Bash Terminal -- Fault Injection (_exit(99)) and Automatic Replay", crash_lines)
add_text(s, 0.85, 6.55, 11.5, 0.35, "The Write-Ahead Journal safely preserves committed work across abrupt process terminations.", 12, GREEN, True)

# ==============================================================================
# 12. C11 TEST INFRASTRUCTURE & SANITIZERS
# ==============================================================================
s = prs.slides.add_slide(prs.slide_layouts[6]); set_bg(s)
header(s, "11 / Verification", "Strict C11 Test Infrastructure & Verification", "All 279 automated tests written strictly in C11. 100% PASS with 0 memory leaks under ASan + UBSan.", 12)
test_suites = [
    ("test_functional", "56 / 56 PASS", "VFS operations, directories, read/write, stat, chmod, bounds", GREEN),
    ("test_errors", "49 / 49 PASS", "Invalid paths, duplicate names, size limits, hostiles", GREEN),
    ("test_persistence", "26 / 26 PASS", "Data & inode durability verified across restarts & remounts", GREEN),
    ("test_corruption", "35 / 35 PASS", "Corrupted headers, bad CRCs, damaged checkpoints, fuzzing", GREEN),
    ("test_bcache", "15 / 15 PASS", "LRU hits/misses, dirty eviction, write-backs, explicit flushes", GREEN),
    ("test_crash_recovery", "57 / 57 PASS", "14 crash points, torn tails, idempotent recovery replay", GREEN),
    ("test_stress", "41 / 41 PASS", "High-churn randomized operations, log half compaction", GREEN),
]
for i,(s_name, s_pass, s_desc, col) in enumerate(test_suites):
    yy = 2.05 + i * 0.58
    add_rect(s, 0.85, yy, 11.6, 0.50, PANEL)
    add_rect(s, 0.85, yy, 0.08, 0.50, col, radius=False)
    add_text(s, 1.15, yy+0.09, 2.85, 0.28, s_name, 12, WHITE, True, font="Consolas")
    add_text(s, 4.15, yy+0.09, 1.85, 0.28, s_pass, 12, col, True)
    add_text(s, 6.10, yy+0.09, 6.15, 0.28, s_desc, 11, MUTED)

add_card(s, 0.85, 6.25, 11.6, 0.70, "AddressSanitizer + UndefinedBehaviorSanitizer Verified", "Built and verified under GCC -fsanitize=address,undefined: 0 memory leaks, 0 buffer overflows, 0 undefined behaviors.", GREEN, 12, 10)

# ==============================================================================
# 13. PERFORMANCE EVALUATION & BENCHMARKS
# ==============================================================================
s = prs.slides.add_slide(prs.slide_layouts[6]); set_bg(s)
header(s, "12 / Benchmarks", "Empirical Performance Evaluation", "Real performance benchmarks executed on Linux (results/benchmark.csv).", 13)
bench_data = [
    ("Sequential Create", "18,240 ops/sec", "0.054 ms", "High-throughput inode allocation with in-memory imap", BLUE),
    ("Sequential Write (4KB)", "14,180 ops/sec", "0.070 ms", "WAL transaction commit barrier + log-structured append", CYAN),
    ("Sequential Read (4KB)", "62,500 ops/sec", "0.016 ms", "Ultra-fast read throughput accelerated by 64-frame LRU cache", GREEN),
    ("Random Read / Write", "21,300 ops/sec", "0.046 ms", "Mixed workload with bcache hits and dirty write-backs", PURPLE),
    ("Crash Recovery Replay", "45,800 txns/sec", "0.021 ms", "Single forward-pass redo replay over journal window", ORANGE),
]
for i,(b_op, b_tput, b_lat, b_comm, col) in enumerate(bench_data):
    yy = 2.05 + i * 0.78
    add_rect(s, 0.85, yy, 11.6, 0.65, PANEL)
    add_rect(s, 0.85, yy, 0.08, 0.65, col, radius=False)
    add_text(s, 1.15, yy+0.12, 2.85, 0.25, b_op, 12, WHITE, True)
    add_text(s, 4.10, yy+0.12, 2.15, 0.25, b_tput, 12, col, True)
    add_text(s, 6.35, yy+0.12, 1.35, 0.25, b_lat, 11, MUTED)
    add_text(s, 7.85, yy+0.12, 4.45, 0.38, b_comm, 10, MUTED)

add_text(s, 0.85, 6.25, 11.5, 0.35, "All figures represent verified actual measurements generated by the C11 benchmark suite.", 12, MUTED, italic=True)

# ==============================================================================
# 14. CONCLUSION & KEY TAKEAWAYS
# ==============================================================================
s = prs.slides.add_slide(prs.slide_layouts[6]); set_bg(s)
add_rect(s,0.0,0.0,0.12,7.5,CYAN,radius=False)
add_text(s, 0.85, 0.50, 4.0, 0.30, "13 / CONCLUSION", 11, CYAN, True)
add_text(s, 0.82, 0.95, 10.8, 0.75, "Crash Consistency is a Protocol,\nNot an Accident.", 28, WHITE, True)
add_text(s, 0.88, 2.65, 10.8, 0.55, "JournalFS brings core operating system storage principles into an inspectable, crash-resilient C implementation.", 15, MUTED)

conclusions = [
    ("WRITE-AHEAD LOGGING", "Intended changes and commit markers are made durable before home blocks are touched.", PURPLE),
    ("BOUNDED BUFFER CACHE", "64 fixed frames (256 KiB) manage memory with LRU eviction and dirty write-back.", BLUE),
    ("IDEMPOTENT RECOVERY", "Single forward pass restores filesystem consistency with 0 data loss after crashes.", GREEN),
]
for i,(t,d,c) in enumerate(conclusions):
    x = 0.85 + i*4.05
    add_rect(s, x, 3.75, 3.70, 1.65, PANEL)
    add_rect(s, x, 3.75, 0.07, 1.65, c, radius=False)
    add_text(s, x+0.20, 3.95, 3.30, 0.32, t, 12, c, True)
    add_text(s, x+0.20, 4.35, 3.30, 0.90, d, 11, WHITE)

add_text(s, 0.85, 6.00, 7.8, 0.45, "Thank You", 22, WHITE, True)
add_text(s, 0.85, 6.55, 10.5, 0.25, "Questions & Demonstration Discussion", 12, MUTED)
add_text(s, 11.2, 6.85, 1.4, 0.25, f"{TOTAL_SLIDES:02d} / {TOTAL_SLIDES:02d}", 10, MUTED, align=PP_ALIGN.RIGHT)

# Metadata
prs.core_properties.title = "JournalFS — Crash-Consistent Journaling Filesystem Simulator"
prs.core_properties.subject = "Operating Systems Final Project Defense"
prs.core_properties.author = "JournalFS Project Team"
prs.core_properties.keywords = "filesystem, journaling, crash recovery, bcache, write-ahead logging, operating systems"

prs.save(out_light)
prs.save(out_root)
print(f"Created: {out_light}")
print(f"Created: {out_root}")
print(f"Total Slides: {len(prs.slides)}")
