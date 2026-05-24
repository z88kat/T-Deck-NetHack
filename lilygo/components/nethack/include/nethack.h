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

/* Returns NetHack's classic 16-colour index (CLR_BLACK=0..CLR_WHITE=15)
 * for this cell.  Returns 15 (CLR_WHITE) if gi is NULL or out of range. */
int nh_glyph_info_color(const void *glyphinfo);

/* Look up an extended-command name (e.g. "pray", "chat") in NetHack's
 * extcmdlist[].  Returns the command's index for shim_get_ext_cmd, or -1
 * if not found.  Names are case-sensitive lower-case. */
int nh_lookup_ext_cmd(const char *name);

/* Menu-selection helpers for shim_select_menu.  Allocate a menu_item[]
 * array of the given length and fill in individual entries by index.
 * `ident_ptr` is the `const ANY_P *identifier` argument that came in
 * via shim_add_menu (treat it as an opaque blob the shim cached).  The
 * returned array is owned by NetHack from the moment select_menu
 * stores it in *menu_list -- NetHack will free() it itself. */
void *nh_menu_alloc_list(int count);
void  nh_menu_set_item(void *list, int idx, const void *ident_ptr, long count);

/* sizeof(anything) -- so the shim can allocate per-item copies of the
 * identifier blob at add_menu time, before the source goes out of scope
 * on NetHack's caller stack.  Shim must reserve at least this many
 * bytes per menu item. */
int nh_anything_size(void);

/* Returns nonzero if the anything at *p is bitwise zero (NetHack's
 * `zeroany`).  Items added with a zeroany identifier are headers /
 * separators and must NOT be assigned an accelerator. */
int nh_anything_is_zero(const void *p);

#ifdef __cplusplus
}
#endif
