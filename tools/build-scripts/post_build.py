#!/usr/bin/env python3
"""
PlatformIO post-build script to archive firmware binaries.
"""

Import("env")
import os
import json
import hashlib
from datetime import datetime
import re
import shutil

# Newest build_NNN.bin files kept per env cache directory (older ones are delta bases nobody uploads from)
FIRMWARE_CACHE_KEEP = 20

def prune_cache(cache_dir, keep=FIRMWARE_CACHE_KEEP, protect=None):
    """Delete all but the newest `keep` build_NNN.bin files in cache_dir; returns the removed paths.

    Only files directly in cache_dir named build_<digits>.bin are considered (by build number),
    and `protect` (the file just written) is never deleted.
    """
    builds = []
    for name in os.listdir(cache_dir):
        match = re.fullmatch(r"build_(\d+)\.bin", name)
        if match:
            builds.append((int(match.group(1)), os.path.join(cache_dir, name)))
    builds.sort(reverse=True)

    removed = []
    for _, path in builds[keep:]:
        if protect and os.path.abspath(path) == os.path.abspath(protect):
            continue
        try:
            os.remove(path)
            removed.append(path)
        except OSError as e:
            print(f"Warning: could not prune {path}: {e}")
    return removed


def get_build_number(env):
    """Get the current BUILD_NUMBER from git_info.h"""
    import re
    try:
        # Get project directory from env parameter
        project_dir = env.get("PROJECT_DIR")
        git_info_path = os.path.join(project_dir, "include", "git_info.h")
        with open(git_info_path, 'r') as f:
            content = f.read()
            match = re.search(r'#define BUILD_NUMBER (\d+)', content)
            if match:
                return int(match.group(1))
    except Exception as e:
        print(f"Warning: Could not read BUILD_NUMBER: {e}")
    return 0

def archive_firmware(source, target, env):
    """Archive firmware binary with build number naming"""
    print("=== Post-Build Firmware Archive ===")
    
    # Get project directory and build info
    project_dir = env.get("PROJECT_DIR")
    
    # Construct firmware path
    pioenv = env.get("PIOENV")
    build_dir = os.path.join(project_dir, ".pio", "build", pioenv)
    firmware_path = os.path.join(build_dir, "firmware.bin")
    
    if not os.path.exists(firmware_path):
        print(f"❌ Firmware binary not found: {firmware_path}")
        return
    
    # Cache per PlatformIO env so V1/V2 builds with the same build number never collide
    # (delta OTA patches must only be made against a base from the same env)
    cache_dir = os.path.join(project_dir, "firmware_cache", pioenv)
    
    # Get build number and firmware info
    build_number = get_build_number(env)
    firmware_size = os.path.getsize(firmware_path)
    
    print(f"📦 Firmware: {firmware_size:,} bytes ({firmware_size/1024:.1f} KB)")
    print(f"🔢 Build number: {build_number}")
    
    # Create cache directory if needed
    os.makedirs(cache_dir, exist_ok=True)
    
    # Archive the firmware with build number naming
    cached_firmware_path = os.path.join(cache_dir, f"build_{build_number:03d}.bin")
    
    try:
        shutil.copy2(firmware_path, cached_firmware_path)
        print(f"✅ Archived firmware: build_{build_number:03d}.bin")
        print(f"📁 Cache location: {cached_firmware_path}")

        removed = prune_cache(cache_dir, FIRMWARE_CACHE_KEEP, protect=cached_firmware_path)
        if removed:
            print(f"Pruned {len(removed)} old cached build(s), keeping the newest {FIRMWARE_CACHE_KEEP}")

    except Exception as e:
        print(f"❌ Failed to archive firmware: {e}")
        return
    
    print("==========================================")

# This function is called by PlatformIO
env.AddPostAction("buildprog", archive_firmware)
env.AddPostAction("upload", archive_firmware)