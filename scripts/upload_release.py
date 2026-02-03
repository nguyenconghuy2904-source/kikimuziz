#!/usr/bin/env python3
"""
Script to upload merged binary to GitHub Release
Requires GitHub token: GITHUB_TOKEN environment variable or --token argument
"""
import os
import sys
import requests
import json
from pathlib import Path

# GitHub repository
REPO_OWNER = "conghuy93"
REPO_NAME = "kikichatwiath_ai"

def get_project_version():
    """Read PROJECT_VER from CMakeLists.txt"""
    cmake_file = Path("CMakeLists.txt")
    if not cmake_file.exists():
        return None
    with cmake_file.open() as f:
        for line in f:
            if line.startswith("set(PROJECT_VER"):
                return line.split("\"")[1]
    return None

def create_release(token, version, tag_name=None):
    """Create a GitHub release"""
    if tag_name is None:
        tag_name = f"v{version}"
    
    url = f"https://api.github.com/repos/{REPO_OWNER}/{REPO_NAME}/releases"
    headers = {
        "Authorization": f"token {token}",
        "Accept": "application/vnd.github.v3+json"
    }
    
    # Check if release already exists
    response = requests.get(f"{url}/tags/{tag_name}", headers=headers)
    if response.status_code == 200:
        print(f"Release {tag_name} already exists")
        return response.json()
    
    data = {
        "tag_name": tag_name,
        "name": f"Release {tag_name}",
        "body": f"Firmware release {tag_name}\n\n## Features\n- QR code display for control panel\n- Auto-clear QR code after 30 seconds\n- Disabled auto-start webserver on boot\n- Updated ASR error handling",
        "draft": False,
        "prerelease": False
    }
    
    response = requests.post(url, headers=headers, json=data)
    if response.status_code != 201:
        print(f"Failed to create release: {response.status_code}")
        print(response.text)
        return None
    
    print(f"✅ Created release {tag_name}")
    return response.json()

def upload_asset(token, release_id, file_path):
    """Upload file to GitHub release"""
    url = f"https://api.github.com/repos/{REPO_OWNER}/{REPO_NAME}/releases/{release_id}/assets"
    headers = {
        "Authorization": f"token {token}",
        "Accept": "application/vnd.github.v3+json"
    }
    
    file_name = os.path.basename(file_path)
    file_size = os.path.getsize(file_path)
    
    print(f"Uploading {file_name} ({file_size / 1024 / 1024:.2f} MB)...")
    
    with open(file_path, 'rb') as f:
        files = {'file': (file_name, f, 'application/octet-stream')}
        params = {'name': file_name}
        
        response = requests.post(url, headers=headers, files=files, params=params)
        if response.status_code == 201:
            print(f"✅ Successfully uploaded {file_name}")
            return True
        else:
            print(f"❌ Failed to upload: {response.status_code}")
            print(response.text)
            return False

def main():
    import argparse
    parser = argparse.ArgumentParser(description="Upload firmware to GitHub Release")
    parser.add_argument("--token", help="GitHub personal access token")
    parser.add_argument("--file", help="File to upload (default: releases/v{VERSION}_kiki.zip)")
    parser.add_argument("--tag", help="Release tag (default: v{VERSION})")
    parser.add_argument("--merged-bin", action="store_true", help="Upload merged-binary.bin instead")
    
    args = parser.parse_args()
    
    # Get token
    token = args.token or os.environ.get("GITHUB_TOKEN")
    if not token:
        print("❌ Error: GitHub token required")
        print("Set GITHUB_TOKEN environment variable or use --token argument")
        print("\nTo create a token:")
        print("1. Go to https://github.com/settings/tokens")
        print("2. Generate new token with 'repo' scope")
        sys.exit(1)
    
    # Get version
    version = get_project_version()
    if not version:
        print("❌ Error: Could not read PROJECT_VER from CMakeLists.txt")
        sys.exit(1)
    
    # Determine file to upload
    if args.merged_bin:
        file_path = Path("build/merged-binary.bin")
    elif args.file:
        file_path = Path(args.file)
    else:
        file_path = Path(f"releases/v{version}_kiki.zip")
    
    if not file_path.exists():
        print(f"❌ Error: File not found: {file_path}")
        sys.exit(1)
    
    # Create release
    tag_name = args.tag or f"v{version}"
    release = create_release(token, version, tag_name)
    if not release:
        sys.exit(1)
    
    # Upload file
    if not upload_asset(token, release['id'], str(file_path)):
        sys.exit(1)
    
    print(f"\n✅ Successfully uploaded to GitHub Release: {tag_name}")
    print(f"   URL: {release['html_url']}")

if __name__ == "__main__":
    main()

