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

## SUZUKI PLAN - Z80 Emulator

- **Location:** `include/MegaDriveEnvironment/system/z80/suzukiplan/`
- **Upstream:** [https://github.com/suzukiplan/z80](https://github.com/suzukiplan/z80)
- **Author:** Yoji Suzuki
- **License:** MIT (see also `suzukiplan/LICENSE.txt`)

```
The MIT License (MIT)

Copyright (c) 2019 Yoji Suzuki.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
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
