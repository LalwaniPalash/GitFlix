# GitFlix  

<div align="center">
  <img src="demo.gif" alt="Demo">
  
  <a href="https://twitter.com/PalashBuilds" target="_blank">
    <img src="https://img.shields.io/twitter/follow/PalashBuilds?style=social" alt="Follow on X">
  </a>
</div>


Store every frame of a 1080 p60 video inside a Git repository, then stream it back at **60 fps** with a single command.

---

## What this repo gives you
| Tool | One-liner | Purpose |
|---|---|---|
| **git-vid-convert** | `./git-vid-convert in.mp4 repo.git` | Turn any MP4 into a Git repo |
| **git-vid-play-metal** | `./git-vid-play-metal repo.git` | Watch it back at 60 fps (macOS) |
| **git-vid-play** | `git log --reverse --format=%H \| ./git-vid-play` | Cross-platform fallback |

---

## Quick start
```bash
git clone https://github.com/LalwaniPalash/GitFlix.git
cd GitFlix
make metal                    # build the Metal player
./git-vid-play-metal demo_video   # play the bundled demo
```

That’s it, no LFS, no external blobs.  
Every pixel is a Git object; every commit is a frame.

---

## Why the repo grows
- Raw 600-frame 1080 p60 ≈ **3.5 GB**  
- GitFlix (LZFSE + delta) shrinks it to **633 MB** (7 : 1 lossless)  
- H.264 source files look “bigger” after conversion because we’re storing **uncompressed pixels**—that’s the point of the experiment.

---

## Build
```bash
make           # everything
make metal     # macOS 60 fps build
```

---

License: MIT