#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import glob
import hashlib
import socket
from distutils.version import LooseVersion
from flask import Flask, jsonify, send_file, abort, request
from flask_cors import CORS

app = Flask(__name__)
CORS(app)  # 允许跨域访问

# ====== 配置区域（请根据实际情况修改） ======
BIN_DIR = "/home/ub/OTA_Bin"          # 存放固件的目录
PORT = 5000                            # 服务端口
# 重要：将下面 IP 改为你的 Ubuntu 在局域网中的实际 IP（桥接模式下的静态IP）
BASE_URL = "http://192.168.4.16:5000"    # 请替换为你的实际 IP
# ==========================================

def get_sha256(filepath):
    """计算文件的 SHA256 哈希值"""
    sha256 = hashlib.sha256()
    with open(filepath, 'rb') as f:
        for chunk in iter(lambda: f.read(4096), b''):
            sha256.update(chunk)
    return sha256.hexdigest()

def get_latest_firmware_info():
    """
    扫描 BIN_DIR 目录，找出所有 .bin 文件，
    解析版本号，按版本号降序排列，返回最新的一条信息。
    """
    bin_files = glob.glob(os.path.join(BIN_DIR, "*.bin"))
    if not bin_files:
        return None

    firmware_list = []
    for f in bin_files:
        filename = os.path.basename(f)
        version = os.path.splitext(filename)[0]  # 去掉 .bin 后缀，作为版本号
        # 计算校验和
        sha256 = get_sha256(f)
        firmware_list.append({
            "version": version,
            "filename": filename,
            "sha256": sha256,
            "filepath": f
        })

    # 按版本号排序（使用 LooseVersion 支持语义化版本，如 v1.0.10 > v1.0.9）
    firmware_list.sort(key=lambda x: LooseVersion(x['version']), reverse=True)
    return firmware_list[0]  # 返回最新版本

@app.route('/ota/check', methods=['GET'])
def check_update():
    """ESP32 通过此接口获取最新固件信息"""
    latest = get_latest_firmware_info()
    if not latest:
        return jsonify({"error": "No firmware available"}), 404

    # 构造下载完整 URL
    download_url = f"{BASE_URL}/ota/download/{latest['filename']}"
    response = {
        "version": latest['version'],
        "sha256": latest['sha256'],
        "url": download_url
    }
    return jsonify(response)

@app.route('/ota/download/<filename>', methods=['GET'])
def download_firmware(filename):
    """提供固件文件下载（流式传输）"""
    # 防止路径穿越攻击：只允许下载文件名，禁止包含 '..' 或 '/'
    safe_filename = os.path.basename(filename)
    filepath = os.path.join(BIN_DIR, safe_filename)
    if not os.path.exists(filepath):
        abort(404, description="File not found")
    return send_file(filepath, as_attachment=True, download_name=safe_filename)

if __name__ == '__main__':
    # 打印本机 IP 提示，帮助用户设置 BASE_URL
    hostname = socket.gethostname()
    local_ip = socket.gethostbyname(hostname)
    print("="*60)
    print(f"当前本机 IP 为: {local_ip}")
    print("请确保上面 BASE_URL 中的 IP 与你的 Ubuntu 实际局域网 IP 一致！")
    print("如果是桥接模式，建议在 Ubuntu 中设置静态 IP。")
    print("="*60)
    # 监听所有网络接口，允许外部访问
    app.run(host='0.0.0.0', port=PORT, debug=False)
