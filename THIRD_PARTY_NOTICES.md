# Third-party notices

This project vendors or embeds the following third-party components. Their
license terms apply in addition to the project MIT license in
[`LICENSE`](LICENSE).

## ymfm (YM2612 FM synthesis)

- **Location:** `include/MegaDriveEnvironment/system/sound/mame_ymfm/`,
  `src/system/sound/mame_ymfm/`
- **Upstream:** [https://github.com/aaronsgiles/ymfm](https://github.com/aaronsgiles/ymfm)
- **Author:** Aaron Giles
- **License:** BSD 3-Clause

```
BSD 3-Clause License

Copyright (c) 2021, Aaron Giles
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## Z80 emulator core

- **Location:** `include/MegaDriveEnvironment/system/z80/mame_z80/`,
  `src/system/z80/mame_z80/`
- **Author:** Juergen Buchmueller (with later changes attributed in the source)
- **License:** freeware for non-commercial purposes; commercial use requires
  contacting the author (see the notice in `z80.c`)

```
Portable Z80 emulator V3.9

Copyright Juergen Buchmueller, all rights reserved.

- This source code is released as freeware for non-commercial purposes.
- You are free to use and redistribute this code in modified or
  unmodified form, provided you list me in the credits.
- If you modify this source code, you must add a notice to each modified
  source file that it has been changed.  If you're a nice person, you
  will clearly mark each change too.  :)
- If you wish to use this for commercial purposes, please contact me at
  pullmoll@t-online.de
- The author of this copywritten work reserves the right to change the
  terms of its usage and license at any time, including retroactively
- This entire notice must remain in the source code.
```

## Other runtime dependencies

These are not vendored in this repository; consumers obtain them via CMake
`find_package` / FetchContent and remain under their own licenses:

| Dependency | Typical license | Role |
| --- | --- | --- |
| [SDL3](https://libsdl.org/) | zlib | Window, input, audio output |
| [yaml-cpp](https://github.com/jbeder/yaml-cpp) | MIT | Controls configuration |
| [zlib](https://zlib.net/) | zlib | Compression (via libpng) |
| [libpng](http://www.libpng.org/pub/png/libpng.html) | libpng | PNG capture and art loading |
