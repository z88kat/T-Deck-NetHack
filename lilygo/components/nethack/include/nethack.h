/* nethack.h -- the entire public surface of libnh.a.
 *
 * Mirrors what sys/libnh/README.md documents.  See win/shim/winshim.c for
 * the list of windowport names that come through the callback.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Engine entrypoint: configures NetHack and runs moveloop() until the
 * game ends.  argv may be NULL when argc is 0. */
int nhmain(int argc, char *argv[]);

/* Shim windowport callback type.
 *   name     - name of the windowport function being invoked
 *              (see doc/window.txt for the full list).
 *   ret_ptr  - pointer to a return slot.  Type encoded in fmt[0].
 *   fmt      - signature string.  fmt[0] is the return type;
 *              fmt[1..] describe the variadic args.  Type codes:
 *                v - void   i - int      s - string (const char *)
 *                c - char   b - boolean  p - pointer
 *                0 - opaque/zero-sized (skip)
 *                1 - coordxy (signed 8-bit)
 *                2 - short
 *   The rest are va_args matching the fmt tail.
 */
typedef void (*shim_callback_t)(const char *name, void *ret_ptr,
                                const char *fmt, ...);

void shim_graphics_set_callback(shim_callback_t cb);

/* Helpers for unpacking the per-cell info that shim_print_glyph hands us.
 * The shim sees `glyph_info *` as an opaque void pointer; these accessors
 * live inside libnh.a where the full struct is in scope.  Returns the
 * ASCII char NetHack would print on a tty (e.g. '.', '#', '@', 'd').
 * Returns ' ' if gi is NULL. */
int nh_glyph_info_char(const void *glyphinfo);

#ifdef __cplusplus
}
#endif
