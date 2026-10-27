# -*- coding: utf-8 -*-
"""检查 interview_book/src/*.md 是否符合 STYLE_SPEC 的语法与结构要求。

用法: python check_src.py
"""
import os
import re
import glob
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "src")

FORBIDDEN = [
    (re.compile(r"^#### ", re.M), "#### 四级标题"),
    (re.compile(r"^---+$", re.M), "--- 分隔线"),
    (re.compile(r"!\["), "图片 ![]()"),
    (re.compile(r"\[[^\]]+\]\([^)]+\)"), "Markdown 链接"),
    (re.compile(r"^\s{2,}[-*] ", re.M), "嵌套列表"),
    (re.compile(r"<[a-zA-Z/][^>]*>"), "HTML 标签"),
    (re.compile(r"[\U0001F300-\U0001FAFF\u2600-\u27BF]"), "emoji"),
    (re.compile(r"^~~~", re.M), "~~~ 代码块"),
]

# 代码块内允许出现的字面标签（ReAct 协议里的 XML 风格标记、模板占位符）
ALLOW_HTML_IN_CODE = re.compile(r"</?(tool_call|function|tool_response|think|observation)\b[^>]*>")

# 真正要拦的是 HTML 渲染标签（会干扰 docx 生成或本身就是错语法）
REAL_HTML = re.compile(
    r"</?(a|b|i|u|br|p|div|span|img|font|table|tr|td|th|ul|ol|li|h1|h2|h3|h4|h5|h6|"
    r"strong|em|code|pre|blockquote|hr|sup|sub|center|script|style)\b[^>]*>", re.I)


def strip_code_blocks(text):
    """把 ``` 代码块整段删掉，返回 (剩余正文, 每行是否在代码块内)。

    代码块在 Word 里是原样文本，里面的 <tool_call> 之类不是 HTML，不能误判。
    """
    out_lines = []
    in_code = False
    for ln in text.split("\n"):
        if ln.strip().startswith("```"):
            in_code = not in_code
            out_lines.append("")
            continue
        out_lines.append("" if in_code else ln)
    return "\n".join(out_lines)

EXPECT = {
    "00": ("# 第 0 章", ["一句话心智模型"]),
    "01": ("# 第 1 章", ["一句话心智模型"]),
    "02": ("# 第 2 章", ["一句话心智模型"]),
    "03": ("# 第 3 章", ["一句话心智模型"]),
    "04": ("# 第 4 章", ["一句话心智模型"]),
    "05": ("# 第 5 章", ["一句话心智模型"]),
    "06": ("# 第 6 章", ["一句话心智模型"]),
    "07": ("# 第 7 章", ["一句话心智模型"]),
    "08": ("# 第 8 章", ["一句话心智模型"]),
    "09": ("# 第 9 章", ["一句话心智模型"]),
    "10": ("# 第 10 章", ["一句话心智模型"]),
    "11": ("# 第 11 章", ["一句话心智模型"]),
    "12": ("# 第 12 章", ["一句话心智模型"]),
    "13": ("# 第 13 章", ["一句话心智模型"]),
    "14": ("# 第 14 章", ["一句话心智模型"]),
    "15": ("# 第 15 章", ["一句话心智模型"]),
    "appendix": ("# 附录", []),
}


def cn_count(text):
    return len(re.findall(r"[\u4e00-\u9fff]", text))


def main():
    files = sorted(glob.glob(os.path.join(SRC, "*.md")))
    problems = []
    rows = []
    total_cn = 0

    for f in files:
        name = os.path.basename(f)
        key = name.split("-")[0]
        with open(f, encoding="utf-8-sig") as fh:
            text = fh.read()
        lines = text.split("\n")

        first = lines[0].strip() if lines else ""
        exp = EXPECT.get(key)
        if exp:
            if not first.startswith(exp[0]):
                problems.append("%s: 首行应为 '%s'，实际 '%s'" % (name, exp[0], first[:40]))
            for kw in exp[1]:
                if kw not in text:
                    problems.append("%s: 缺少 '%s'" % (name, kw))

        for pat, desc in FORBIDDEN:
            # 代码块内的内容原样渲染，HTML 检查也要跳过
            scan = strip_code_blocks(text) if desc in ("HTML 标签", "Markdown 链接") else text
            hit = None
            for m in pat.finditer(scan):
                if desc == "HTML 标签":
                    # 只拦真正的 HTML 渲染标签；<tool_call> / <pid> 这类占位符和协议标记是合法正文
                    if ALLOW_HTML_IN_CODE.match(m.group(0)) or not REAL_HTML.match(m.group(0)):
                        continue
                hit = m
                break
            if hit:
                ln = scan[:hit.start()].count("\n") + 1
                problems.append("%s:%d 使用了禁用语法 [%s]  %s" % (name, ln, desc, hit.group(0)[:40]))
                continue

        # 结构检查
        n_q = len(re.findall(r"^### Q：", text, re.M))
        n_ans = len(re.findall(r"\*\*一句话答案：\*\*", text))
        n_exp = len(re.findall(r"\*\*展开讲：\*\*", text))
        n_feyn = len(re.findall(r"【费曼讲解】", text))
        n_anchor = "记忆锚点" in text
        n_down = "下刀" in text

        if n_q and key not in ("00", "appendix", "14", "15"):
            if n_ans < n_q * 0.8:
                problems.append("%s: %d 个 Q 但只有 %d 个「一句话答案」" % (name, n_q, n_ans))
            if n_exp < n_q * 0.8:
                problems.append("%s: %d 个 Q 但只有 %d 个「展开讲」" % (name, n_q, n_exp))
        if key not in ("00", "appendix") and not n_anchor:
            problems.append("%s: 缺少「记忆锚点」小节" % name)
        if key not in ("00", "appendix") and not n_down:
            problems.append("%s: 缺少「下刀预演」小节" % name)
        if key not in ("00", "appendix") and n_feyn < 2:
            problems.append("%s: 费曼讲解只有 %d 处（要求 >= 2）" % (name, n_feyn))

        c = cn_count(text)
        total_cn += c
        rows.append((name, c, n_q, n_feyn))

    print("%-42s %8s %6s %6s" % ("文件", "汉字", "Q数", "费曼"))
    print("-" * 70)
    for name, c, q, fe in rows:
        print("%-42s %8d %6d %6d" % (name, c, q, fe))
    print("-" * 70)
    print("总汉字数: %d   文件数: %d" % (total_cn, len(files)))

    if problems:
        print("\n=== 问题 (%d) ===" % len(problems))
        for p in problems:
            print(" ! " + p)
        return 1
    print("\n全部检查通过。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
