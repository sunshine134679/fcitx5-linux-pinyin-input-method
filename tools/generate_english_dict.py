#!/usr/bin/env python3
"""
tools/generate_english_dict.py

Maintainer tool to parse, clean, and compile ECDICT english-to-chinese definitions
into a compact, sorted binary dictionary (`modernime-english-dict.bin`).

Binary format:
  Header (16 bytes):
    char magic[8] = "MODEDIC1"
    uint32_t entry_count
    uint32_t string_offset
  Index Entries (entry_count * 12 bytes, strictly sorted by lowercase word):
    uint32_t word_offset (relative to string pool)
    uint16_t word_len
    uint16_t def_len
    uint32_t def_offset  (relative to string pool)
  String Pool:
    Contiguous UTF-8 encoded words and cleaned definition strings.
"""

import argparse
import csv
import io
import os
import re
import struct
import sys
import urllib.request

POS_REGEX = re.compile(r'^(?:[a-zA-Z]+\.\s*|\[[^\]]+\]\s*)+')
BRACKET_REGEX = re.compile(r'\[[^\]]*\]|\([^\)]*\)|（[^）]*）')

def clean_definition(raw_translation: str, max_chars: int = 12) -> str:
    if not raw_translation:
        return ''
    lines = [line.strip() for line in raw_translation.replace('\\n', '\n').split('\n') if line.strip()]
    meanings = []
    for line in lines:
        line = POS_REGEX.sub('', line).strip()
        if not line:
            continue
        parts = re.split(r'[,，;；]', line)
        for part in parts:
            part = BRACKET_REGEX.sub('', part).strip()
            if re.search(r'[\u4e00-\u9fa5]', part):
                part = re.sub(r'^[a-zA-Z\s\.\,\-]+', '', part).strip()
                if part and part not in meanings:
                    meanings.append(part)
                    if len(meanings) >= 2:
                        break
        if len(meanings) >= 2:
            break

    if not meanings:
        return ''

    if len(meanings) == 1:
        res = meanings[0]
    else:
        res = f"{meanings[0]}；{meanings[1]}"

    if len(res) > max_chars:
        if len(meanings) > 1 and len(meanings[0]) <= max_chars:
            res = meanings[0]
        else:
            res = res[:max_chars]
    return res

def extract_core_english_words(cpp_file: str) -> set:
    words = set()
    if not os.path.isfile(cpp_file):
        print(f"Warning: {cpp_file} not found, proceeding without filter.")
        return words
    with open(cpp_file, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if line.startswith('"') and line.endswith('",'):
                word = line[1:-2].strip().lower()
                if word:
                    words.add(word)
    return words

def build_binary_dictionary(entries: list, output_path: str):
    """
    entries: list of (word, definition) sorted strictly ascending by word
    """
    entry_count = len(entries)
    index_bytes = bytearray()
    string_bytes = bytearray()

    for word, definition in entries:
        word_raw = word.encode('utf-8')
        def_raw = definition.encode('utf-8')

        word_offset = len(string_bytes)
        string_bytes.extend(word_raw)
        word_len = len(word_raw)

        def_offset = len(string_bytes)
        string_bytes.extend(def_raw)
        def_len = len(def_raw)

        index_bytes.extend(struct.pack('<IHHI', word_offset, word_len, def_len, def_offset))

    header_size = 16
    string_offset = header_size + len(index_bytes)
    header = struct.pack('<8sII', b'MODEDIC1', entry_count, string_offset)

    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
    with open(output_path, 'wb') as f:
        f.write(header)
        f.write(index_bytes)
        f.write(string_bytes)

    total_size = header_size + len(index_bytes) + len(string_bytes)
    print(f"Successfully compiled dictionary to {output_path}")
    print(f"  Total Entries:  {entry_count}")
    print(f"  Header Size:    {header_size} bytes")
    print(f"  Index Size:     {len(index_bytes)} bytes ({len(index_bytes)/1024:.2f} KB)")
    print(f"  String Size:    {len(string_bytes)} bytes ({len(string_bytes)/1024:.2f} KB)")
    print(f"  Total File Size: {total_size} bytes ({total_size/(1024*1024):.2f} MB)")

def main():
    parser = argparse.ArgumentParser(description="Generate ModernIME English Definition Binary Dictionary")
    parser.add_argument('--input-csv', help="Path to ecdict.csv. If omitted, downloads from GitHub.")
    parser.add_argument('--output', default="data/pinyin/modernime-english-dict.bin", help="Output binary path")
    parser.add_argument('--core-cpp', default="core/src/english_dictionary.cpp", help="Path to english_dictionary.cpp")
    parser.add_argument('--max-chars', type=int, default=12, help="Max characters per definition")
    args = parser.parse_args()

    core_words = extract_core_english_words(args.core_cpp)
    print(f"Loaded {len(core_words)} core English words from {args.core_cpp}")

    raw_stream = None
    if args.input_csv and os.path.isfile(args.input_csv):
        print(f"Reading ECDICT from local file: {args.input_csv}")
        raw_stream = open(args.input_csv, 'r', encoding='utf-8')
    else:
        url = 'https://raw.githubusercontent.com/skywind3000/ECDICT/master/ecdict.csv'
        print(f"Downloading ECDICT from: {url} ...")
        req = urllib.request.Request(url, headers={'User-Agent': 'Mozilla/5.0'})
        resp = urllib.request.urlopen(req, timeout=60)
        raw_stream = io.TextIOWrapper(resp, encoding='utf-8')

    reader = csv.DictReader(raw_stream)
    matched_entries = {}
    total_rows = 0

    for row in reader:
        total_rows += 1
        word = row.get('word', '').strip().lower()
        if not word:
            continue
        if core_words and word not in core_words:
            continue

        raw_trans = row.get('translation', '')
        clean_def = clean_definition(raw_trans, args.max_chars)
        if clean_def:
            matched_entries[word] = clean_def

    if hasattr(raw_stream, 'close'):
        raw_stream.close()

    print(f"Processed {total_rows} rows from ECDICT. Matched {len(matched_entries)} words with clean definitions.")

    # Sort strictly by word ascending
    sorted_entries = sorted(matched_entries.items(), key=lambda x: x[0])
    build_binary_dictionary(sorted_entries, args.output)

if __name__ == '__main__':
    main()
