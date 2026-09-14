# stream-player

Hardware-accelerated video renderer for Harman MHI2 (Nvidia Tegra 30, QNX 6.5.0) cluster displays.

`stream-player` decodes secondary Android Auto video frames received over local TCP loopback (`tcp://127.0.0.1:12346`) and renders them into the Volkswagen Virtual Cockpit (Displayable 3, Context 70 / MOST150 Display 4).

---

## Technical Highlights

1. **Hardware Blitter Acceleration:**
   The MIB2 Harman Tegra 30 GLES2 stack disables online shader compilation (`GL_SHADER_COMPILER == 0`). `stream-player` utilizes Nvidia's hardware blit extension `GL_NV_draw_texture` (`glDrawTextureNV`) for zero-copy presentation.
2. **End-to-End Hardware Flow Control:**
   After each frame is swapped onto the display surface via `eglSwapBuffers()`, `stream-player` writes a 1-byte ACK (`0x01`) to `/tmp/gal_ack.sock`. This provides hardware backpressure directly back to Android Auto, preventing buffer overflows and macroblock tearing.
3. **Multi-Threaded Decoding:**
   Configured with 2 FFmpeg worker threads (`FF_THREAD_FRAME`) and a 16-slot ring buffer pool to ensure stable 30 FPS playback under concurrent primary screen rendering.
4. **Resilient Lifecycle:**
   Automatically re-establishes TCP connections if the stream drops, and cleanly restores factory instrument dials (Context 33) upon termination.

---

## Attribution & Licenses

* **Base Architecture:** Derived and adapted from the `VcMOSTRenderMqb` project by **Andrew Leech**.
* **FFmpeg:** Video decoding relies on static builds of FFmpeg (`libavcodec`, `libavformat`, `libavutil`), licensed under LGPL v2.1+.
