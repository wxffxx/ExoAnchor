# UVC 2.5.1 security backports

Parsing and printing sources are based on the upstream files from Espressif commit [477cb5a](https://github.com/espressif/esp-usb/commit/477cb5af9953bafd9351c4cb6ecbbb7e3e5a9f2f), the descriptor safety fix included in UVC 2.5.2. Apache-2.0 file notices are retained. A local parser change replaces device-controlled continuous-interval loops with two bounded arithmetic comparisons, avoiding integer wrap and unbounded iteration. CMake replaces the compiled source list only after validating the complete 2.5.1 input hashes; managed component files and lock hashes stay intact.

Both firmware profiles retain their existing MJPEG compatibility patches. Dev already returns interface release failures safely. Stable adds the same conservative close behavior: return an error and retain the stream when USB transfers may still refer to it, allowing its supervisor to recover.
