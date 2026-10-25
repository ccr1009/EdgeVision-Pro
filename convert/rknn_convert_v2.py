# ==============================================================================
# EdgeVision-RK3588: RKNN 转换器 v2（DFL-Free + 量化精度修复）
# ------------------------------------------------------------------------------
# 相对 v1 (rnkn_convert.py) 的改动：
#   1. 支持 yolov8n_dflfree.onnx（2 输出：raw_box 64ch + cls_sig 80ch）
#   2. 量化算法可切 mmse / kl_divergence（默认 mmse，比 normal 更抗离群值）
#   3. optimization_level=3，提升 NPU 侧算子融合率
#   4. 转换前强制检查 toolkit 版本 <= 板端 runtime 版本（这是之前精度异常的隐蔽根因）
#
# 版本对齐原则（官方要求）：rknn-toolkit2 版本 <= 板端 librknnrt.so 版本
#   本项目：toolkit 1.6.0 + 板端 runtime 1.6.0（已升级，旧版备份 /root/librknnrt_152.bak.so）
#
# 必须在 Linux x86_64 + Python 3.10 下运行（toolkit 只发这个 wheel）。
# ==============================================================================
import os
import sys
import argparse

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
MODELS = os.path.join(PROJECT_ROOT, 'data', 'models')
DEFAULT_CALIB = os.path.join(PROJECT_ROOT, 'data', 'calib_v2', 'dataset.txt')

BOARD_RUNTIME = '2.3.0'   # 板端 librknnrt.so 版本（升级后；1.6.0 的备份在 /root/librknnrt_160.so）


def stub_heavy_deps():
    """在板端(aarch64)跑转换时，rknn.api 顶层会 import torch，而 torch 的 aarch64
    wheel 有 1GB+。ONNX 转换路径实际只用 torch 来生成几个索引常量
    （tk2 2.3.0 的 gen_gather_for_change_split_shape 会调 torch.arange），
    所以这里用 numpy 支撑一个最小实现顶上，避免下载 1GB 的 torch。

    仅当真 torch 不存在时才注入；PC 上装了真 torch 不受影响。"""
    import sys
    import types
    try:
        __import__('torch')
        return []
    except Exception:
        pass

    import numpy as np

    class MiniTensor:
        """够 toolkit 做常量推导用的最小张量，底层就是 numpy。"""
        __slots__ = ('_a',)

        def __init__(self, a):
            self._a = np.asarray(a)

        def numpy(self): return self._a
        def tolist(self): return self._a.tolist()
        def item(self): return self._a.item()
        def to(self, *a, **k): return self
        def cpu(self): return self
        def detach(self): return self
        def clone(self): return MiniTensor(self._a.copy())
        def int(self): return MiniTensor(self._a.astype(np.int64))
        def long(self): return MiniTensor(self._a.astype(np.int64))
        def float(self): return MiniTensor(self._a.astype(np.float32))
        def reshape(self, *s): return MiniTensor(self._a.reshape(*s))
        def view(self, *s): return MiniTensor(self._a.reshape(*s))
        def size(self, d=None): return self._a.shape if d is None else self._a.shape[d]
        def dim(self): return self._a.ndim
        def __len__(self): return len(self._a)
        def __getitem__(self, k): return MiniTensor(self._a[k])
        def __array__(self, dtype=None): return self._a.astype(dtype) if dtype else self._a
        def __iter__(self): return iter(self._a.tolist())
        def __repr__(self): return f"MiniTensor({self._a!r})"

    def _fix_kw(k):
        """torch 用 dim=，numpy 用 axis=；dtype 要走 np.dtype。"""
        out = {}
        for kk, vv in k.items():
            if kk == 'dim':
                out['axis'] = vv
            elif kk == 'dtype':
                out['dtype'] = np.dtype(vv)
            else:
                out[kk] = vv
        return out

    def _mk(fn):
        def f(*a, **k):
            return MiniTensor(fn(*[_np(x) for x in a], **_fix_kw(k)))
        return f

    def _np(x):
        return x._a if isinstance(x, MiniTensor) else np.asarray(x)

    def _cat(tensors, dim=0, **k):
        return MiniTensor(np.concatenate([_np(t) for t in tensors], axis=dim))

    def _stack(tensors, dim=0, **k):
        return MiniTensor(np.stack([_np(t) for t in tensors], axis=dim))

    def _split(t, size, dim=0):
        a = _np(t)
        if isinstance(size, int):
            idx = list(range(size, a.shape[dim], size))
            parts = np.split(a, idx, axis=dim)
        else:
            parts = np.split(a, np.cumsum(size)[:-1], axis=dim)
        return tuple(MiniTensor(p) for p in parts)

    torch = types.ModuleType('torch')
    torch.arange = _mk(np.arange)
    torch.tensor = _mk(np.array)
    torch.zeros = _mk(np.zeros)
    torch.ones = _mk(np.ones)
    torch.full = _mk(np.full)
    torch.split = _split
    torch.cat = _cat
    torch.concat = _cat
    torch.stack = _stack
    torch.unsqueeze = _mk(np.expand_dims)
    torch.permute = _mk(np.transpose)
    torch.Tensor = MiniTensor
    torch.int64 = np.int64
    torch.int32 = np.int32
    torch.float32 = np.float32
    torch.float16 = np.float16
    torch.bool = np.bool_
    torch.contiguous = lambda t, *a, **k: t

    def _mod_getattr(name):
        """兜底：numpy 里同名的都能用（clamp/where/meshgrid/max/...）。"""
        if name.startswith('__'):
            raise AttributeError(name)
        npfn = getattr(np, name, None)
        if npfn is None:
            raise AttributeError(f"torch.{name} 未在 numpy 占位实现中提供")

        def wrapper(*a, **k):
            args = [_np(x) for x in a]
            return MiniTensor(npfn(*args, **_fix_kw(k)))
        return wrapper

    torch.__getattr__ = _mod_getattr

    nn = types.ModuleType('torch.nn')
    nn.Module = type('Module', (), {})
    torch.nn = nn
    fn_mod = types.ModuleType('torch.nn.functional')
    nn.functional = fn_mod

    for name, mod in (('torch', torch), ('torch.nn', nn),
                      ('torch.nn.functional', fn_mod),
                      ('torchvision', types.ModuleType('torchvision')),
                      ('torchvision.transforms', types.ModuleType('torchvision.transforms'))):
        mod.__path__ = []
        sys.modules[name] = mod

    print("[*] 已用 numpy 版 torch 占位（跳过 1GB torch 下载）")
    return ['torch(numpy-stub)']


def ver_tuple(s):
    out = []
    for p in s.split('.'):
        try:
            out.append(int(''.join(ch for ch in p if ch.isdigit()) or 0))
        except ValueError:
            out.append(0)
    return tuple(out)


def check_version(board_runtime=BOARD_RUNTIME):
    try:
        from rknn.api import RKNN
        import rknn
        tv = getattr(rknn, '__version__', None)
    except Exception as e:
        print(f"[-] 无法导入 rknn-toolkit2: {e}")
        print("    两条可选路径：")
        print("      A) 板端直跑（推荐，无需 WSL）：toolkit 2.x 提供 aarch64 wheel，")
        print("         在板子上建 venv 装 toolkit，跑本脚本时加 --stub-torch")
        print("      B) Windows + WSL：convert/setup_toolkit_wsl.sh（py3.10 + linux x86_64 wheel）")
        sys.exit(1)
    # rknn 包没有 __version__ 时，从 RKNN().version 拿
    try:
        tv = RKNN().version if not tv else tv
    except Exception:
        tv = tv or '0.0.0'
    print(f"[*] rknn-toolkit2 版本: {tv}   板端 runtime: {board_runtime}")
    if ver_tuple(tv) > ver_tuple(board_runtime):
        print(f"[!] 警告：toolkit {tv} > runtime {board_runtime}，官方不保证推理正确性！")
        print("    请升级板端 librknnrt.so，或把 toolkit 降到 <= runtime 的版本。")
        return False
    return True


def convert(onnx_path, out_path, dataset, do_quant, algo, iterations, verbose):
    from rknn.api import RKNN

    print("=" * 72)
    print(f"[*] {os.path.basename(onnx_path)} -> {os.path.basename(out_path)}")
    print(f"    量化={do_quant} 算法={algo} iterations={iterations}")
    print("=" * 72)

    rknn = RKNN(verbose=verbose)
    cfg = dict(
        mean_values=[[0, 0, 0]],
        std_values=[[255, 255, 255]],
        target_platform='rk3588',
        optimization_level=3,
    )
    if do_quant:
        cfg.update(quantized_algorithm=algo)
        # 注：quantized_iteration 是 toolkit 1.x 的参数，2.x 已移除，传了会直接报错
    rknn.config(**cfg)

    if rknn.load_onnx(model=onnx_path) != 0:
        print("[-] load_onnx 失败")
        return False

    if do_quant:
        if not dataset or not os.path.exists(dataset):
            print(f"[-] 校准集不存在: {dataset}")
            return False
        n = sum(1 for line in open(dataset, encoding='utf-8') if line.strip())
        print(f"[*] 校准图 {n} 张")
        if rknn.build(do_quantization=True, dataset=dataset) != 0:
            print("[-] build 失败")
            return False
    else:
        if rknn.build(do_quantization=False) != 0:
            print("[-] build 失败")
            return False

    if rknn.export_rknn(out_path) != 0:
        print("[-] export_rknn 失败")
        return False

    print(f"[+] 已导出 {out_path}  ({os.path.getsize(out_path)/1e6:.2f} MB)")
    rknn.release()
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--onnx', default=os.path.join(MODELS, 'yolov8n_dflfree.onnx'))
    ap.add_argument('--calib', default=DEFAULT_CALIB)
    ap.add_argument('--algo', default='mmse', choices=['normal', 'mmse', 'kl_divergence'])
    ap.add_argument('--iterations', type=int, default=10)
    ap.add_argument('--tag', default='dflfree')
    ap.add_argument('--fp16-too', action='store_true', help='同时出一份 FP16 做精度基线')
    ap.add_argument('--verbose', action='store_true')
    ap.add_argument('--out-dir', default=MODELS,
                    help='输出目录（默认仓库 data/models；脚本被拷到别处跑时要显式指定）')
    ap.add_argument('--stub-torch', action='store_true',
                    help='板端(aarch64)转换时用占位模块跳过 torch/torchvision')
    ap.add_argument('--board-runtime', default=BOARD_RUNTIME,
                    help=f'板端 librknnrt.so 版本，用于校验版本对齐（默认 {BOARD_RUNTIME}）')
    args = ap.parse_args()

    if args.stub_torch:
        stub_heavy_deps()

    if not os.path.exists(args.onnx):
        print(f"[-] ONNX 不存在: {args.onnx}")
        print("    先跑: python convert/export_dflfree_onnx.py")
        sys.exit(1)

    check_version(args.board_runtime)

    os.makedirs(args.out_dir, exist_ok=True)
    int8_out = os.path.join(args.out_dir, f'yolov8n_{args.tag}_rk3588_i8.rknn')
    ok = convert(args.onnx, int8_out, args.calib, True, args.algo, args.iterations, args.verbose)

    if ok and args.fp16_too:
        fp_out = os.path.join(args.out_dir, f'yolov8n_{args.tag}_rk3588_fp.rknn')
        convert(args.onnx, fp_out, None, False, None, 0, args.verbose)

    print("\n[SUCCESS] 转换完成" if ok else "\n[FAILED] 转换失败")
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
