# Third-party notices

HDR Hint bundles the components listed below. Everything else in the executable is the Windows SDK and the C++ standard library. External tools are run as separate processes and are neither bundled nor linked.

## nlohmann/json 3.11.3 (bundled)

`third_party/nlohmann/json.hpp`, compiled into `HdrHint.exe` and `hdrhint_tests.exe`.
Licence: MIT. The full text is in `third_party/nlohmann/LICENSE.MIT` and reproduced here:

```
MIT License

Copyright (c) 2013-2022 Niels Lohmann

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Adobe CSInterface.js v12.0.0 (bundled in the CEP panel)

`cep/com.everett.hdrhint/js/CSInterface.js`, taken unmodified and unminified from Adobe's public CEP-Resources repository (`CEP_12.x/CSInterface.js`, header `CSInterface - v12.0.0`). The file's own header notice governs its use and is kept intact in the shipped copy:

```
ADOBE SYSTEMS INCORPORATED
Copyright 2020 Adobe Systems Incorporated
All Rights Reserved.

NOTICE:  Adobe permits you to use, modify, and distribute this file in accordance with the
terms of the Adobe license agreement accompanying it.  If you have received this file from a
source other than Adobe, then your use, modification, or distribution of it requires the prior
written permission of Adobe.
```

The copy in this repository was obtained directly from Adobe's repository, so the first sentence of the notice applies.

## json2.js (bundled in the CEP panel)

`cep/com.everett.hdrhint/jsx/json2.js` by Douglas Crockford, dated 2023-05-10. Public domain, as stated in the file:

```
json2.js
2023-05-10
Public Domain.
NO WARRANTY EXPRESSED OR IMPLIED. USE AT YOUR OWN RISK.
```

It is loaded into AME's ExtendScript engine (an ES3 environment) to provide `JSON.parse` and `JSON.stringify`.

## MKVToolNix / mkvmerge (external tool, not redistributed)

HDR Hint runs `mkvmerge.exe` from a separately installed MKVToolNix (https://mkvtoolnix.download). MKVToolNix is copyright Moritz Bunkus and contributors and is licensed under the GNU General Public License, version 2. It is not bundled with, linked into, or modified by HDR Hint; HDR Hint starts it as a separate process with a command line and reads its output. Install it yourself: version 15 or newer is required, v82.0 is the version this project was tested with.

## FFmpeg ffprobe / ffmpeg (optional external tools, not redistributed)

When `ffprobe.exe` is present (`C:\ffmpeg\bin` or on `PATH`) HDR Hint uses it to read colour tags and in-band HDR10 SEI from an export; `scripts/make_test_hdr_mp4.ps1` uses `ffmpeg` and `ffprobe` to generate and verify test clips. FFmpeg (https://ffmpeg.org) is licensed under the GNU Lesser General Public License version 2.1 or later, or the GNU General Public License version 2 or later depending on how the build was configured. It is started as a separate process and is never bundled or linked. HDR Hint works without it.
