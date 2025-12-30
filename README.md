# xEdit

A lightweight, cross-platform terminal text editor written in C.

![License](https://img.shields.io/badge/license-GPL--3.0-blue.svg)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey.svg)
![Version](https://img.shields.io/badge/version-1.8.0-green.svg)

## Overview

xEdit is a simple yet powerful terminal-based text editor inspired by kilo and nano. It provides essential editing features with syntax highlighting, file management, and an intuitive interface — all in a single C file with no external dependencies.

## Features

- **Syntax Highlighting** — Support for 30+ programming languages
- **File Browser** — Built-in sidebar for easy file navigation
- **Undo System** — Multi-level undo with intelligent grouping
- **Selection & Clipboard** — Select text with Shift+Arrows, copy/cut/paste
- **Search** — Incremental search with match highlighting
- **Cross-Platform** — Works on Linux, macOS, and Windows
- **Lightweight** — Single file, ~2000 lines of code, no dependencies
- **UTF-8 Support** — Proper handling of Unicode text
- **Auto-Indent** — Smart indentation for new lines
- **Line Numbers** — Always visible line numbers

## Installation

### From Source

# Clone the repository
```bash
git clone https://github.com/yourusername/xedit.git
cd xedit
```

# Compile
```bash
gcc -O2 -o xedit xedit.c
```

# Optional: Install system-wide
```bash
sudo cp xedit /usr/local/bin/
sudo cp syntax.db /usr/local/share/xedit/
```
