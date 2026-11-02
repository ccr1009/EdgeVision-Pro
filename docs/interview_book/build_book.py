# -*- coding: utf-8 -*-
"""
把 interview_book/src/*.md 编译成一份排好版的 Word 文档。

用法:
    python build_book.py

依赖: python-docx (已装 1.2.0)

支持的 Markdown 子集:
    # / ## / ### 标题
    普通段落
    - 无序列表 / 1. 有序列表
    > 引用块(渲染成浅色提示框)
    ``` 代码块 ```
    | 表格 |
    **粗体** / *斜体* / `行内代码`

输出: ../RK3588端侧AI部署-面试问答详解.docx
"""
import os
import re
import sys
import glob

from docx import Document
from docx.shared import Pt, Cm, RGBColor
from docx.enum.text import WD_ALIGN_PARAGRAPH, WD_LINE_SPACING
from docx.enum.table import WD_TABLE_ALIGNMENT
from docx.enum.section import WD_SECTION
from docx.oxml.ns import qn
from docx.oxml import OxmlElement

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "src")
OUT = os.path.join(os.path.dirname(HERE), "RK3588端侧AI部署-面试问答详解.docx")

FONT_CN = "微软雅黑"
FONT_BODY = "微软雅黑"
FONT_CODE = "Consolas"
FONT_CODE_CN = "微软雅黑"

C_MAIN = RGBColor(0x1F, 0x36, 0x64)   # 深蓝
C_ACC = RGBColor(0xC0, 0x50, 0x4D)    # 砖红
C_GRAY = RGBColor(0x59, 0x59, 0x59)
FILL_CODE = "F4F5F7"
FILL_QUOTE = "FFF7E6"
FILL_QHEAD = "EAF1FB"
FILL_TH = "DCE6F1"


# ---------------------------------------------------------------- 低层工具
def set_run_font(run, name=FONT_BODY, size=None, bold=None, color=None,
                 east=None, italic=None):
    run.font.name = name
    rpr = run._element.get_or_add_rPr()
    rf = rpr.find(qn("w:rFonts"))
    if rf is None:
        rf = OxmlElement("w:rFonts")
        rpr.append(rf)
    rf.set(qn("w:ascii"), name)
    rf.set(qn("w:hAnsi"), name)
    rf.set(qn("w:eastAsia"), east or name)
    if size is not None:
        run.font.size = Pt(size)
    if bold is not None:
        run.font.bold = bold
    if italic is not None:
        run.font.italic = italic
    if color is not None:
        run.font.color.rgb = color


def shade(el, fill):
    """给段落 pPr / 单元格 tcPr 加底色"""
    pr = el.find(qn("w:pPr"))
    if pr is None:
        pr = el.find(qn("w:tcPr"))
    if pr is None:
        pr = OxmlElement("w:pPr")
        el.insert(0, pr)
    for old in pr.findall(qn("w:shd")):
        pr.remove(old)
    shd = OxmlElement("w:shd")
    shd.set(qn("w:val"), "clear")
    shd.set(qn("w:color"), "auto")
    shd.set(qn("w:fill"), fill)
    pr.append(shd)


def set_p_border(p, left=None, box=None, color="4472C4", sz="18", space="6"):
    pPr = p._p.get_or_add_pPr()
    for old in pPr.findall(qn("w:pBdr")):
        pPr.remove(old)
    pbdr = OxmlElement("w:pBdr")
    if left:
        e = OxmlElement("w:left")
        e.set(qn("w:val"), "single")
        e.set(qn("w:sz"), sz)
        e.set(qn("w:space"), space)
        e.set(qn("w:color"), color)
        pbdr.append(e)
    if box:
        for side in ("top", "left", "bottom", "right"):
            e = OxmlElement("w:" + side)
            e.set(qn("w:val"), "single")
            e.set(qn("w:sz"), "6")
            e.set(qn("w:space"), "2")
            e.set(qn("w:color"), color)
            pbdr.append(e)
    pPr.append(pbdr)


def para(doc, text="", size=10.5, bold=False, color=None, align=None,
         space_before=0, space_after=4, indent_first=0, left_indent=0,
         line=1.45, name=FONT_BODY, italic=False, keep=False):
    p = doc.add_paragraph()
    pf = p.paragraph_format
    pf.space_before = Pt(space_before)
    pf.space_after = Pt(space_after)
    pf.line_spacing = line
    if indent_first:
        pf.first_line_indent = Pt(indent_first)
    if left_indent:
        pf.left_indent = Pt(left_indent)
    if align:
        p.alignment = align
    if keep:
        pf.keep_with_next = True
    if text:
        add_rich(p, text, size=size, bold=bold, color=color, name=name, italic=italic)
    return p


INLINE = re.compile(r"(\*\*.+?\*\*|`[^`]+?`|\*[^*\s][^*]*?\*)")

# 代码块里成段出现的「制表符画图」字符：如果用真正的等宽字体(Consolas)渲染，
# 中文字形会出现兜底字体，东亚字宽度变化会让框线对不齐；改用雅黑后整块字宽
# 更稳定。这里只是标记，不再按字符切 run（见 _add_code_runs）。
ART_CHARS = "─│┌┐└┘├┤┬┴┼►▼▲◄"


def _add_code_runs(p, line, color):
    """代码块的一行：整行一个 run。

    早期实现想对 ASCII 图逐字符换字体，结果每个 run 各自成段导致框线被拆开；
    现在统一用等宽字体 + 中文兜底，靠行首不折行保证对齐。
    """
    r = p.add_run(line if line else "")
    set_run_font(r, FONT_CODE, 9, color=color, east=FONT_CODE_CN)
    if any(ch in line for ch in ART_CHARS):
        rPr = r._element.get_or_add_rPr()
        if rPr.find(qn("w:noProof")) is None:
            rPr.append(OxmlElement("w:noProof"))
    return r


def add_rich(p, text, size=10.5, bold=False, color=None, name=FONT_BODY, italic=False):
    """解析 **粗体**、`代码`、*斜体*；粗体内部允许再嵌 `代码`（递归处理）"""
    buf = []

    def flush():
        if buf:
            r = p.add_run("".join(buf))
            set_run_font(r, name, size, bold, color, italic=italic)
            buf.clear()

    i, n = 0, len(text)
    while i < n:
        # **粗体**：找到闭合的 ** 后递归解析内部
        if text.startswith("**", i):
            j = text.find("**", i + 2)
            if j != -1 and j > i + 2:
                flush()
                add_rich(p, text[i + 2:j], size, True, color or C_ACC,
                         name, italic)
                i = j + 2
                continue
        # `代码`
        elif text[i] == "`":
            j = text.find("`", i + 1)
            if j != -1 and j > i + 1:
                flush()
                r = p.add_run(text[i + 1:j])
                set_run_font(r, FONT_CODE, size - 0.5, bold,
                             RGBColor(0xA3, 0x15, 0x15),
                             east=FONT_CODE_CN, italic=italic)
                i = j + 1
                continue
        # *斜体*：要求起始 * 后面不是空白，且闭合 * 前面不是空白
        elif text[i] == "*":
            j = text.find("*", i + 1)
            if (j != -1 and j > i + 1 and not text[i + 1].isspace()
                    and not text[j - 1].isspace()):
                flush()
                r = p.add_run(text[i + 1:j])
                set_run_font(r, name, size, bold, color, italic=True)
                i = j + 1
                continue
        buf.append(text[i])
        i += 1
    flush()


# ---------------------------------------------------------------- 文档骨架
def build_styles(doc):
    st = doc.styles["Normal"]
    st.font.name = FONT_BODY
    st.font.size = Pt(10.5)
    st.element.rPr.rFonts.set(qn("w:eastAsia"), FONT_BODY)


def page_setup(doc):
    s = doc.sections[0]
    s.page_width = Cm(21.0)
    s.page_height = Cm(29.7)
    s.top_margin = Cm(2.2)
    s.bottom_margin = Cm(2.0)
    s.left_margin = Cm(2.4)
    s.right_margin = Cm(2.2)
    s.header_distance = Cm(1.2)
    s.footer_distance = Cm(1.0)


def add_footer_pagenum(doc):
    footer = doc.sections[0].footer
    p = footer.paragraphs[0]
    p.alignment = WD_ALIGN_PARAGRAPH.CENTER
    r = p.add_run()
    set_run_font(r, FONT_BODY, 9, color=C_GRAY)
    fld = OxmlElement("w:fldSimple")
    fld.set(qn("w:instr"), "PAGE")
    r._element.addnext(fld)


def add_header_text(doc, text):
    header = doc.sections[0].header
    p = header.paragraphs[0]
    p.alignment = WD_ALIGN_PARAGRAPH.RIGHT
    r = p.add_run(text)
    set_run_font(r, FONT_BODY, 8.5, color=C_GRAY)


# ---------------------------------------------------------------- 块渲染
def set_outline(p, level):
    """让 Word 导航窗格能识别层级，而不依赖 Heading 样式"""
    pPr = p._p.get_or_add_pPr()
    for old in pPr.findall(qn("w:outlineLvl")):
        pPr.remove(old)
    ol = OxmlElement("w:outlineLvl")
    ol.set(qn("w:val"), str(level))
    pPr.append(ol)


# ---------------------------------------------------- 书签 + 目录内链
_BM_SEQ = [0]
_CH_SEQ = [0]


def add_bookmark(p, name):
    """在段落上打一个书签，供目录超链接跳转"""
    _BM_SEQ[0] += 1
    bid = str(_BM_SEQ[0])
    start = OxmlElement("w:bookmarkStart")
    start.set(qn("w:id"), bid)
    start.set(qn("w:name"), name)
    end = OxmlElement("w:bookmarkEnd")
    end.set(qn("w:id"), bid)
    p._p.insert(0, start)
    p._p.append(end)


def add_anchor(p, text, anchor, size=11.5, bold=True, color=None):
    """往段落里追加一段指向书签的文字（Word/WPS 里可点击跳转）"""
    link = OxmlElement("w:hyperlink")
    link.set(qn("w:anchor"), anchor)
    r = OxmlElement("w:r")
    link.append(r)
    p._p.append(link)
    from docx.text.run import Run
    run = Run(r, p)
    run.text = text
    set_run_font(run, FONT_BODY, size, bold, color or C_MAIN)
    return run


def next_chapter_anchor():
    """章节序号，渲染顺序与目录顺序一致"""
    _CH_SEQ[0] += 1
    return "ch%02d" % _CH_SEQ[0]


def h1(doc, text):
    p = doc.add_paragraph()
    pf = p.paragraph_format
    pf.space_before = Pt(0)
    pf.space_after = Pt(2)
    pf.page_break_before = True
    pf.keep_with_next = True
    add_bookmark(p, next_chapter_anchor())
    add_rich(p, text, size=21, bold=True, color=C_MAIN)
    set_outline(p, 0)
    # 下划线
    pPr = p._p.get_or_add_pPr()
    pbdr = OxmlElement("w:pBdr")
    e = OxmlElement("w:bottom")
    e.set(qn("w:val"), "single")
    e.set(qn("w:sz"), "12")
    e.set(qn("w:space"), "4")
    e.set(qn("w:color"), "1F3864")
    pbdr.append(e)
    pPr.append(pbdr)


def h2(doc, text):
    p = doc.add_paragraph()
    pf = p.paragraph_format
    pf.space_before = Pt(16)
    pf.space_after = Pt(6)
    pf.keep_with_next = True
    add_rich(p, text, size=14.5, bold=True, color=C_MAIN)
    shade(p._p, "EDF2F9")
    set_outline(p, 1)
    pPr = p._p.get_or_add_pPr()
    pbdr = OxmlElement("w:pBdr")
    e = OxmlElement("w:left")
    e.set(qn("w:val"), "single")
    e.set(qn("w:sz"), "24")
    e.set(qn("w:space"), "6")
    e.set(qn("w:color"), "1F3864")
    pbdr.append(e)
    pPr.append(pbdr)


def h3(doc, text):
    """### Q：xxx"""
    p = doc.add_paragraph()
    pf = p.paragraph_format
    pf.space_before = Pt(13)
    pf.space_after = Pt(5)
    pf.keep_with_next = True
    pf.left_indent = Pt(0)
    add_rich(p, text, size=12, bold=True, color=RGBColor(0x0B, 0x3D, 0x91))
    shade(p._p, FILL_QHEAD)


def body(doc, text):
    p = para(doc, "", space_after=5, line=1.55)
    add_rich(p, text, size=10.5)
    return p


def bullet(doc, text, ordered=False, idx=1):
    p = doc.add_paragraph()
    pf = p.paragraph_format
    pf.left_indent = Pt(20)
    pf.first_line_indent = Pt(-13)
    pf.space_after = Pt(3)
    pf.line_spacing = 1.45
    mark = "%d. " % idx if ordered else "• "
    r = p.add_run(mark)
    set_run_font(r, FONT_BODY, 10.5, True, C_MAIN)
    add_rich(p, text, size=10.5)
    return p


def quote_block(doc, lines):
    for i, ln in enumerate(lines):
        p = doc.add_paragraph()
        pf = p.paragraph_format
        pf.left_indent = Pt(10)
        pf.right_indent = Pt(4)
        pf.space_before = Pt(6 if i == 0 else 0)
        pf.space_after = Pt(6 if i == len(lines) - 1 else 0)
        pf.line_spacing = 1.45
        shade(p._p, FILL_QUOTE)
        set_p_border(p, left=True, color="E0A030", sz="18", space="8")
        add_rich(p, ln, size=10)
    # 让引用块不被分页切开
    return


def code_block(doc, lines):
    tbl = doc.add_table(rows=1, cols=1)
    tbl.alignment = WD_TABLE_ALIGNMENT.CENTER
    tbl.autofit = False
    cell = tbl.cell(0, 0)
    cell.width = Cm(15.8)
    # 底色 + 边框
    tcPr = cell._tc.get_or_add_tcPr()
    shd = OxmlElement("w:shd")
    shd.set(qn("w:val"), "clear")
    shd.set(qn("w:fill"), FILL_CODE)
    tcPr.append(shd)
    borders = OxmlElement("w:tcBorders")
    for side in ("top", "left", "bottom", "right"):
        e = OxmlElement("w:" + side)
        e.set(qn("w:val"), "single")
        e.set(qn("w:sz"), "4")
        e.set(qn("w:color"), "D6DAE0")
        borders.append(e)
    tcPr.append(borders)
    # 清掉默认空段落
    cell.paragraphs[0]._p.getparent().remove(cell.paragraphs[0]._p)
    for ln in lines:
        p = cell.add_paragraph()
        pf = p.paragraph_format
        pf.space_before = Pt(0)
        pf.space_after = Pt(0)
        pf.line_spacing = 1.15
        _add_code_runs(p, ln, RGBColor(0x1A, 0x1A, 0x1A))
        # ASCII 图（框线/箭头）不能被折行，否则会拆成两截
        if any(ch in ln for ch in "─│┌┐└┘►◄▲▼─→←↑↓"):
            for r in p.runs:
                rPr = r._element.get_or_add_rPr()
                if rPr.find(qn("w:noProof")) is None:
                    rPr.append(OxmlElement("w:noProof"))
    # 表格后留白
    sp = doc.add_paragraph()
    sp.paragraph_format.space_after = Pt(2)
    sp.paragraph_format.space_before = Pt(0)
    sp.paragraph_format.line_spacing = 1.0
    for r in sp.runs:
        r.font.size = Pt(4)


def _text_w(s):
    """粗略估算显示宽度：中文 2，其余 1"""
    w = 0
    for ch in s:
        w += 2 if ord(ch) > 0x2E7F else 1
    return w


def table_block(doc, rows):
    """rows: list of list[str], 第一行是表头"""
    ncol = max(len(r) for r in rows)
    rows = [r + [""] * (ncol - len(r)) for r in rows]
    tbl = doc.add_table(rows=len(rows), cols=ncol)
    tbl.style = "Table Grid"
    tbl.alignment = WD_TABLE_ALIGNMENT.CENTER
    tbl.autofit = False

    # 固定布局 + 满宽表格
    tblPr = tbl._tbl.tblPr
    for old in tblPr.findall(qn("w:tblW")):
        tblPr.remove(old)
    tblW = OxmlElement("w:tblW")
    tblW.set(qn("w:w"), "5000")
    tblW.set(qn("w:type"), "pct")
    tblPr.append(tblW)
    layout = OxmlElement("w:tblLayout")
    layout.set(qn("w:type"), "fixed")
    tblPr.append(layout)

    # 按内容长度分配列宽（每列至少留 3 个宽度单位）
    weights = []
    for ci in range(ncol):
        mx = max(_text_w(r[ci]) for r in rows)
        weights.append(min(max(mx, 8), 46))
    total_w = sum(weights)
    usable = Cm(16.4)
    widths = [int(usable * w / total_w) for w in weights]

    grid = tbl._tbl.find(qn("w:tblGrid"))
    if grid is not None:
        for gc, w in zip(grid.findall(qn("w:gridCol")), widths):
            gc.set(qn("w:w"), str(int(w / 635)))

    for ri, row in enumerate(rows):
        # 整行不允许跨页断开
        trPr = tbl.rows[ri]._tr.get_or_add_trPr()
        cant = OxmlElement("w:cantSplit")
        trPr.append(cant)
        if ri == 0:
            hdr = OxmlElement("w:tblHeader")
            trPr.append(hdr)
        for ci, val in enumerate(row):
            cell = tbl.cell(ri, ci)
            cell.width = widths[ci]
            cell.paragraphs[0]._p.getparent().remove(cell.paragraphs[0]._p)
            p = cell.add_paragraph()
            pf = p.paragraph_format
            pf.space_before = Pt(2)
            pf.space_after = Pt(2)
            pf.line_spacing = 1.25
            add_rich(p, val, size=9.5, bold=(ri == 0),
                     color=(C_MAIN if ri == 0 else None))
            if ri == 0:
                tcPr = cell._tc.get_or_add_tcPr()
                shd = OxmlElement("w:shd")
                shd.set(qn("w:val"), "clear")
                shd.set(qn("w:fill"), FILL_TH)
                tcPr.append(shd)
    sp = doc.add_paragraph()
    sp.paragraph_format.space_after = Pt(2)


# ---------------------------------------------------------------- 解析
def split_table_row(line):
    """按未转义的 | 切列，并把 \\| 还原成 |（表格里允许写绝对值符号）"""
    s = line.strip()
    if s.startswith("|"):
        s = s[1:]
    if s.endswith("|") and not s.endswith("\\|"):
        s = s[:-1]
    parts = re.split(r"(?<!\\)\|", s)
    return [c.strip().replace("\\|", "|") for c in parts]


def is_table_sep(line):
    s = line.strip()
    return bool(re.fullmatch(r"\|?[\s:\-\|]+\|?", s)) and "-" in s


def render_md(doc, md, chapter_titles):
    lines = md.split("\n")
    i = 0
    while i < len(lines):
        raw = lines[i]
        line = raw.rstrip()
        s = line.strip()

        if not s:
            i += 1
            continue

        # 代码块
        if s.startswith("```"):
            i += 1
            buf = []
            while i < len(lines) and not lines[i].strip().startswith("```"):
                buf.append(lines[i].rstrip())
                i += 1
            i += 1
            code_block(doc, buf)
            continue

        # 表格
        if s.startswith("|") and i + 1 < len(lines) and is_table_sep(lines[i + 1]):
            rows = [split_table_row(s)]
            i += 2
            while i < len(lines) and lines[i].strip().startswith("|"):
                rows.append(split_table_row(lines[i]))
                i += 1
            table_block(doc, rows)
            continue

        # 引用块
        if s.startswith(">"):
            buf = []
            while i < len(lines) and lines[i].strip().startswith(">"):
                t = lines[i].strip()[1:].strip()
                buf.append(t)
                i += 1
            quote_block(doc, buf)
            continue

        # 标题
        if s.startswith("#### "):
            h3(doc, s[5:])
            i += 1
            continue
        if s.startswith("### "):
            h3(doc, s[4:])
            i += 1
            continue
        if s.startswith("## "):
            h2(doc, s[3:])
            i += 1
            continue
        if s.startswith("# "):
            h1(doc, s[2:])
            chapter_titles.append(s[2:])
            i += 1
            continue

        # 列表
        m = re.match(r"^-\s+(.*)$", s)
        if m:
            bullet(doc, m.group(1), ordered=False)
            i += 1
            continue
        m = re.match(r"^(\d+)\.\s+(.*)$", s)
        if m:
            bullet(doc, m.group(2), ordered=True, idx=int(m.group(1)))
            i += 1
            continue

        # 普通段落
        buf = [s]
        i += 1
        while i < len(lines):
            nxt = lines[i].strip()
            if (not nxt or nxt.startswith(("#", ">", "|", "- ", "```"))
                    or re.match(r"^\d+\.\s+", nxt)):
                break
            buf.append(nxt)
            i += 1
        body(doc, "".join(buf))


# ---------------------------------------------------------------- 封面/目录
def cover(doc):
    for _ in range(3):
        para(doc, "", space_after=0)
    p = para(doc, "", align=WD_ALIGN_PARAGRAPH.CENTER, space_after=6)
    add_rich(p, "端侧 AI 部署面试问答详解", size=30, bold=True, color=C_MAIN)
    p = para(doc, "", align=WD_ALIGN_PARAGRAPH.CENTER, space_after=30)
    add_rich(p, "—— 基于 RK3588 的真实项目：从 Linux 系统到 NPU 推理到大模型 Agent",
            size=13, bold=True, color=C_GRAY)

    p = para(doc, "", align=WD_ALIGN_PARAGRAPH.CENTER, space_after=4)
    add_rich(p, "EVM3588-B  /  RK3588  /  RKNN 2.3.0  /  RKLLM  /  Qwen3.5-2B", size=10.5, color=C_GRAY)

    for _ in range(4):
        para(doc, "", space_after=0)

    box = para(doc, "", space_before=8, space_after=8, left_indent=30)
    shade(box._p, FILL_QUOTE)
    set_p_border(box, left=True, color="E0A030", sz="24", space="10")
    add_rich(box, "这本书怎么用", size=12, bold=True, color=C_ACC)
    for t in ["第一遍：通读，把每章的「心智模型」和「记忆锚点」画成思维导图。",
              "第二遍：只看每个 Q 的「一句话答案」，答不出来的回头精读。",
              "第三遍：只看目录自问自答，卡住的去看「费曼讲解」。",
              "面试前一晚：只翻附录的「必背数字墙」和「记忆锚点」。"]:
        p = para(doc, "", left_indent=30, space_after=2, line=1.4)
        shade(p._p, FILL_QUOTE)
        add_rich(p, "· " + t, size=10)

    for _ in range(6):
        para(doc, "", space_after=0)
    p = para(doc, "", align=WD_ALIGN_PARAGRAPH.CENTER, space_after=2)
    add_rich(p, "2026 年秋招 · 个人整理 · 全部数据来自板端实测", size=9.5, color=C_GRAY)


def toc(doc, entries, sections):
    p = doc.add_paragraph()
    p.paragraph_format.page_break_before = True
    p.paragraph_format.space_before = Pt(0)
    p.paragraph_format.space_after = Pt(4)
    add_rich(p, "目　　录", size=22, bold=True, color=C_MAIN)
    p = para(doc, "", space_after=12)
    add_rich(p, "（蓝色章节名可点击跳转；也可用 Word「视图→导航窗格」按大纲直接定位）",
             size=9, color=C_GRAY)

    for idx, (title, subs) in enumerate(entries, start=1):
        pp = para(doc, "", space_before=9, space_after=2, keep=True)
        add_anchor(pp, title, "ch%02d" % idx, size=11.5, bold=True, color=C_MAIN)
        for sub in subs:
            ps = para(doc, "", space_before=0, space_after=1, left_indent=16, line=1.3)
            add_rich(ps, sub, size=9.5, color=C_GRAY)

    p = para(doc, "", space_before=22, space_after=8, keep=True)
    add_rich(p, "附：全书章节速览", size=11, bold=True, color=C_ACC)
    for key, desc in sections:
        pp = para(doc, "", space_after=2, line=1.35)
        add_rich(pp, "· " + key + " —— " + desc, size=9.5, color=C_GRAY)


CHAPTER_DESC = {
    "第 0 章": "这本书怎么用、三个月复习节奏、三分钟自我介绍模板",
    "第 1 章": "项目全貌、硬件实测参数、每一个技术选型背后的理由、诚实边界",
    "第 2 章": "fd 的本质、系统调用、进程与线程、僵尸进程与孤儿进程、fork+exec",
    "第 3 章": "管道/FIFO/共享内存/socketpair、信号、守护进程化、systemd 托管",
    "第 4 章": "mutex/条件变量/原子操作/内存序、生产者消费者、线程池、无锁队列",
    "第 5 章": "select/poll/epoll、LT 与 ET、粘包拆包、边缘网关、背压、心跳重连",
    "第 6 章": "RK3588 SoC 架构、NPU 三核、驱动与运行时、RKNN 工具链、板端编译",
    "第 7 章": "UVC 采集、MPP 硬解、letterbox、DFL 后处理、三核并行推流",
    "第 8 章": "怎么找瓶颈、图手术、编译选项、性能探针、以及几条负结论",
    "第 9 章": "INT8 量化原理、混合量化、真 mAP 评测口径与诚实表达",
    "第 10 章": "RKLLM 架构、W8A8、量化与转换流程、内存与性能实测",
    "第 11 章": "ReAct 循环、工具协议设计、Prompt 工程、失败恢复、具身 Agent",
    "第 12 章": "快慢双系统、故障注入、12 用例回归、占空比验收",
    "第 13 章": "strace/perf/gdb、37ms 破案全过程、CMAKE 坑、排障方法论",
    "第 14 章": "「这个项目的难点」完整答法 + 20 个追杀问题",
    "第 15 章": "被问倒怎么接、反问清单、HR 面与项目管理问题",
    "第 16 章": "Linux 方向简历逐条对应、三十秒口述版、ALSA/8088/现代 C++ 补课",
    "第 17 章": "端侧 AI 方向简历逐条对应、四处口径掰正、六个数字的追问三层",
    "附录": "必背数字墙、命令墙、术语表、简历对照矩阵、十二句口径不犯错",
}


def main():
    files = sorted(glob.glob(os.path.join(SRC, "*.md")))
    if not files:
        print("no source files in", SRC)
        return 1
    print("found %d chapters" % len(files))

    doc = Document()
    page_setup(doc)
    build_styles(doc)
    doc.core_properties.title = "端侧 AI 部署面试问答详解"
    doc.core_properties.subject = "RK3588 / RKNN / RKLLM / ReAct Agent"
    doc.core_properties.author = "个人整理"

    add_header_text(doc, "端侧 AI 部署面试问答详解 · RK3588")

    # 先预扫一遍标题，做目录
    entries = []
    for f in files:
        title = None
        subs = []
        with open(f, encoding="utf-8-sig") as fh:
            for ln in fh:
                if ln.startswith("# ") and title is None:
                    title = ln[2:].strip()
                elif ln.startswith("## ") and title is not None and len(subs) < 9:
                    subs.append(ln[3:].strip())
        if title:
            entries.append((title, subs))
    print("chapters:", len(entries))

    cover(doc)
    sections = []
    for t, _ in entries:
        if t.startswith("附录"):
            sections.append(("附录", CHAPTER_DESC.get("附录", "")))
        else:
            m = re.match(r"^(第\s*\d+\s*章)", t)
            key = m.group(1) if m else t
            desc = (CHAPTER_DESC.get(key)
                    or CHAPTER_DESC.get(key.replace(" ", ""), ""))
            sections.append((key, desc))
    toc(doc, entries, sections)

    total_chars = 0
    for f in files:
        with open(f, encoding="utf-8-sig") as fh:
            md = fh.read()
        total_chars += len(md)
        render_md(doc, md, [])
    print("total chars:", total_chars)

    add_footer_pagenum(doc)
    doc.save(OUT)
    print("saved ->", OUT)
    print("size: %.1f KB" % (os.path.getsize(OUT) / 1024.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
