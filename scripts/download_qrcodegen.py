#!/usr/bin/env python3
"""
Script to download qrcodegen.c from nayuki/QR-Code-generator
"""
import urllib.request
import os
import sys

QRCODEGEN_URL = "https://raw.githubusercontent.com/nayuki/QR-Code-generator/master/c/qrcodegen.c"
OUTPUT_FILE = os.path.join(os.path.dirname(os.path.dirname(__file__)), "main", "qrcode", "qrcodegen.c")

def download_qrcodegen():
    """Download qrcodegen.c from GitHub"""
    print(f"Downloading qrcodegen.c from {QRCODEGEN_URL}...")
    
    try:
        urllib.request.urlretrieve(QRCODEGEN_URL, OUTPUT_FILE)
        print(f"✅ Successfully downloaded qrcodegen.c to {OUTPUT_FILE}")
        return True
    except Exception as e:
        print(f"❌ Error downloading qrcodegen.c: {e}")
        return False

if __name__ == "__main__":
    if download_qrcodegen():
        sys.exit(0)
    else:
        sys.exit(1)

