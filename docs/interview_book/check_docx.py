# -*- coding: utf-8 -*-
"""生成后的 docx 自检:骨架、样式、Markdown 残留、代码块字体、表格完整性。"""
import re
import sys
from docx import Document
from docx.shared import Pt

PATH = r"D:\a研究生\项目\rk3588项目\edgevision-rk3588\docs\RK3588端侧AI部署-面试问答详解.docx"

doc = Document(PATH)
paras = doc.paragraphs
problems = []

styles = {}
for p in paras:
    styles[p.style.name] = styles.get(p.style.name, 0) + 1

print("段落总数:", len(paras))
print("表格总数:", len(doc.tables))
print("样式分布:")
for k, v in sorted(styles.items(), key=lambda x: -x[1]):
    print("   %-22s %d" % (k, v))

h1 = [p.text for p in paras if p.style.name == "Heading 1"]
h2 = [p.text for p in paras if p.style.name == "Heading 2"]
h3 = [p.text for p in paras if p.style.name == "Heading 3"]
print("\nH1(%d):" % len(h1))
for t in h1:
    print("   ", t)
print("H2 数:", len(h2), " H3 数:", len(h3))

# 1) Markdown 残留扫描
leak_pat = re.compile(r"^(\s*[#>\-*+]\s|\*\*|!\[|\]\(|\|)")
leaks = []
for i, p in enumerate(paras):
    t = p.text
    if not t.strip():
        continue
    if p.style.name.startswith("Heading"):
        continue
    if leak_pat.match(t):
        leaks.append((i, p.style.name, t[:70]))
if leaks:
    print("\n[!] 疑似 Markdown 残留 %d 处(前 12):" % len(leaks))
    for i, s, t in leaks[:12]:
        print("   #%d [%s] %s" % (i, s, t))
else:
    print("\n[OK] 无 Markdown 语法残留")

# 2) 代码块检查:等宽字体段落的字体与着色
mono = [p for p in paras if p.style.name in ("Code", "Code Block", "代码")]
print("\n代码块段落数:", len(mono))
if mono:
    r = mono[0].runs[0] if mono[0].runs else None
    if r is not None:
        print("   字体:", r.font.name, " 字号:", r.font.size)

# 3) 表格完整性:每张表的行列数
bad_tbl = []
for idx, tb in enumerate(doc.tables):
    ncol = len(tb.columns)
    for ri, row in enumerate(tb.rows):
        if len(row.cells) != ncol:
            bad_tbl.append((idx, ri, len(row.cells), ncol))
print("表格数:", len(doc.tables), " 行列不齐:", len(bad_tbl))
if bad_tbl:
    print("   ", bad_tbl[:5])

# 4) 空标题 / 重复标题
seen = set()
dups = []
for t in h1 + h2 + h3:
    if t in seen:
        dups.append(t)
    seen.add(t)
print("重复标题:", len(dups), dups[:5])

# 5) 是否有空段落簇(连续 4 个以上)
run, maxrun = 0, 0
for p in paras:
    if not p.text.strip():
        run += 1
        maxrun = max(maxrun, run)
    else:
        run = 0
print("最长连续空段落:", maxrun)

# 6) 页数估算(用 sectPr 无法得页数,改看总字数)
total_chars = sum(len(p.text) for p in paras)
print("正文字符数(含空白):", total_chars)

print("\n===== 结论 =====")
if leaks or dups or bad_tbl:
    print("存在问题需要修")
    sys.exit(1)
print("docx 结构自检通过")
