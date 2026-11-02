#!/usr/bin/env python3
"""一键配置 RK3588 板子免密 SSH。

做的事:
  1. 本地缺失则生成 ed25519 密钥(~/.ssh/id_ed25519_rk3588)
  2. 把公钥装进板子的 ~/.ssh/authorized_keys
  3. 在 ~/.ssh/config 里写一段 Host 配置(别名 rk3588 / IP 直连都免密)
  4. 用「纯密钥」重连一次自检

用法: BOARD_IP=<板子IP> BOARD_PASSWORD=<密码> python setup_board_ssh.py
"""
import argparse
import os
import sys

import paramiko
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ed25519


def ensure_key(key_path: str) -> str:
    """确保本地有 ed25519 私钥，返回私钥路径。"""
    pub_path = key_path + ".pub"
    if os.path.exists(key_path) and os.path.exists(pub_path):
        print(f"[SKIP] 公钥已存在: {pub_path}")
        return key_path

    sk = ed25519.Ed25519PrivateKey.generate()
    os.makedirs(os.path.dirname(key_path), exist_ok=True)
    with open(key_path, "wb") as fh:
        fh.write(sk.private_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PrivateFormat.OpenSSH,
            encryption_algorithm=serialization.NoEncryption(),
        ))
    os.chmod(key_path, 0o600)

    pub_b64 = sk.public_key().public_bytes(
        encoding=serialization.Encoding.OpenSSH,
        format=serialization.PublicFormat.OpenSSH,
    ).decode()
    pub_line = f"{pub_b64} rk3588-board@pc"
    with open(pub_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(pub_line + "\n")
    print(f"[OK] 已生成密钥: {key_path}")
    return key_path


def write_ssh_config(host: str, key_path: str) -> None:
    """在 ~/.ssh/config 追加一段 Host 配置(幂等)。"""
    cfg_path = os.path.join(os.path.dirname(key_path), "config")
    marker = "# >>> rk3588-board (auto)"
    block = (
        f"{marker}\n"
        f"Host rk3588 {host}\n"
        f"    HostName {host}\n"
        f"    User root\n"
        f"    IdentityFile {key_path.replace(os.sep, '/')}\n"
        f"    StrictHostKeyChecking accept-new\n"
        f"# <<< rk3588-board\n"
    )
    old = ""
    if os.path.exists(cfg_path):
        with open(cfg_path, encoding="utf-8") as fh:
            old = fh.read()
    if marker in old:
        print("[SKIP] ~/.ssh/config 已有 rk3588 配置")
        return
    with open(cfg_path, "a", encoding="utf-8", newline="\n") as fh:
        if old and not old.endswith("\n"):
            fh.write("\n")
        fh.write(block)
    print(f"[OK] 已写入 {cfg_path} (Host rk3588)")


def install_pubkey(host: str, user: str, password: str, key_path: str) -> bool:
    pub = open(key_path + ".pub", encoding="utf-8").read().strip()

    cli = paramiko.SSHClient()
    cli.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    try:
        cli.connect(host, port=22, username=user, password=password,
                    timeout=10, look_for_keys=False, allow_agent=False)
    except Exception as exc:  # noqa: BLE001
        print(f"[FAIL] 密码登录失败: {exc}")
        return False

    cmd = (
        "mkdir -p ~/.ssh; chmod 700 ~/.ssh; touch ~/.ssh/authorized_keys; "
        f"grep -qF '{pub}' ~/.ssh/authorized_keys || echo '{pub}' >> ~/.ssh/authorized_keys; "
        "chmod 600 ~/.ssh/authorized_keys; "
        "echo \"authorized_keys 行数: $(wc -l < ~/.ssh/authorized_keys)\""
    )
    _, out, err = cli.exec_command(cmd, timeout=15)
    print("  " + out.read().decode("utf-8", "replace").strip())
    err_text = err.read().decode("utf-8", "replace").strip()
    if err_text:
        print(f"  [stderr] {err_text}")
    cli.close()
    print("[OK] 公钥已装到板子")
    return True


def verify(host: str, user: str, key_path: str) -> bool:
    cli = paramiko.SSHClient()
    cli.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    try:
        cli.connect(host, port=22, username=user, key_filename=key_path,
                    timeout=10, look_for_keys=False, allow_agent=False)
    except Exception as exc:  # noqa: BLE001
        print(f"[FAIL] 密钥认证自检失败: {exc}")
        return False
    _, out, _ = cli.exec_command("hostname; uname -r", timeout=15)
    info = out.read().decode("utf-8", "replace").strip().replace("\n", " | ")
    cli.close()
    print(f"[OK] 免密登录自检通过 -> {info}")
    return True


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=os.environ.get("BOARD_IP", ""))
    ap.add_argument("--user", default="root")
    ap.add_argument("--password", default=os.environ.get("BOARD_PASSWORD", ""))
    ap.add_argument("--key", default=os.path.expanduser("~/.ssh/id_ed25519_rk3588"))
    a = ap.parse_args()

    key_path = ensure_key(a.key)
    write_ssh_config(a.host, key_path)
    if not install_pubkey(a.host, a.user, a.password, key_path):
        return 3
    return 0 if verify(a.host, a.user, key_path) else 4


if __name__ == "__main__":
    sys.exit(main())
