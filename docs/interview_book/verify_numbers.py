# -*- coding: utf-8 -*-
"""数字出处核对：把 interview_book/src/*.md 里出现的数字，拿去 docs/ 下的源文件里搜。

用法: python verify_numbers.py
说明：只要能在一处源文件里找到同样的字面数字，就认为"有出处"；
      找不到的会列出来，需要人工确认是否为合理推导或需要改掉。
"""
import os
import re
import glob
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "src")
ROOT = os.path.abspath(os.path.join(HERE, ".."))          # docs/
WHITELIST_FILES = [
    os.path.join(ROOT, "BENCHMARKS.md"),
    os.path.join(ROOT, "CURRENT_STATUS.md"),
    os.path.join(ROOT, "TROUBLESHOOTING.md"),
    os.path.join(ROOT, "LEARNING_PLAN_4WEEKS.md"),
]
WHITELIST_DIRS = [
    os.path.join(ROOT, "interview"),
    os.path.join(ROOT, "tech_notes"),
    os.path.join(ROOT, ".."),          # 仓库根：README.md / AGENTS.md
]

# 数字：带小数点或纯整数，允许末尾 %，前面允许 x/×
NUM = re.compile(r"\d+(?:\.\d+)?")


def collect_source_corpus():
    corpus = []
    paths = []
    for f in WHITELIST_FILES:
        if os.path.isfile(f):
            paths.append(f)
    for d in WHITELIST_DIRS[:2]:
        for ext in ("*.md", "*.txt"):
            paths.extend(glob.glob(os.path.join(d, ext)))
    root = os.path.abspath(os.path.join(ROOT, ".."))
    for name in ("README.md", "AGENTS.md"):
        p = os.path.join(root, name)
        if os.path.isfile(p):
            paths.append(p)
    # 代码也可以作为出处：章节里会引 `lab/rkllm_embodied_agent.cpp:859-864` 这种行号
    for sub in ("lab", "deploy", "src", "scripts"):
        base = os.path.join(root, sub)
        if not os.path.isdir(base):
            continue
        for ext in ("*.cpp", "*.h", "*.hpp", "*.c", "*.py", "*.sh", "*.txt", "*.cmake", "CMakeLists.txt"):
            paths.extend(glob.glob(os.path.join(base, "**", ext), recursive=True))
    # 排除构建产物
    paths = [p for p in paths if os.sep + "build" not in p and os.sep + "3rdparty" not in p]
    for p in sorted(set(paths)):
        try:
            with open(p, encoding="utf-8-sig", errors="ignore") as fh:
                corpus.append(fh.read())
        except Exception:
            pass
    return "\n".join(corpus)


def main():
    corpus = collect_source_corpus()
    files = sorted(glob.glob(os.path.join(SRC, "*.md")))
    missing = []
    total = 0

    for f in files:
        name = os.path.basename(f)
        with open(f, encoding="utf-8-sig") as fh:
            text = fh.read()
        # 跳过代码块里的纯代码行？不跳，代码里的数字同样要真实
        for m in NUM.finditer(text):
            tok = m.group(0)
            total += 1
            # 太短的整数(0/1/2/3/4/5/6/7/8/9/10/16/32/64等)到处都是，跳过
            if "." not in tok and len(tok) <= 3 and int(tok) <= 256:
                continue
            if tok in corpus:
                continue
            ln = text[:m.start()].count("\n") + 1
            ctx = text[max(0, m.start() - 28):m.end() + 18].replace("\n", " ")
            missing.append((name, ln, tok, ctx))

    print("共扫描数字 token: %d" % total)
    print("未在源文件中找到字面匹配的: %d" % len(missing))
    for name, ln, tok, ctx in missing:
        print("  %s:%d  [%s]  ...%s..." % (name, ln, tok, ctx))
    return 0


if __name__ == "__main__":
    sys.exit(main())
