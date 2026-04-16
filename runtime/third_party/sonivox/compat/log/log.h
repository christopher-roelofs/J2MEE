/* Shim for Android's <log/log.h>, replaced with no-op macros for non-Android
 * builds of Sonivox EAS. Sonivox uses ALOGE/ALOGW/ALOGD/ALOGI/ALOGV for
 * debug reporting only — silencing them is safe. */

#ifndef SONIVOX_COMPAT_LOG_LOG_H
#define SONIVOX_COMPAT_LOG_LOG_H

/* Expand to a no-op that still typechecks variadic format args. */
#define ALOGE(...) ((void)0)
#define ALOGW(...) ((void)0)
#define ALOGI(...) ((void)0)
#define ALOGD(...) ((void)0)
#define ALOGV(...) ((void)0)

/* Android safety-net logger used in a handful of EAS bounds-check branches.
 * Keep as an inline no-op so vendor code compiles + links unchanged. */
static inline int android_errorWriteLog(int tag, const char *subTag) {
    (void)tag; (void)subTag;
    return 0;
}

#endif /* SONIVOX_COMPAT_LOG_LOG_H */
