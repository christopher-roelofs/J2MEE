/* Empty shim. Sonivox's eas_hostmm.c #includes <media/MediaPlayerInterface.h>
 * but never uses any symbol from it — the include is a vestige of AOSP's
 * build environment. Providing an empty file lets the vendor TU compile. */
