/*
 * DirectGlide - Debug logging
 */

#ifndef DG_LOG_H
#define DG_LOG_H

void dg_log_init(const char* filename);
void dg_log_close(void);
void dg_log(const char* fmt, ...);

/* Log a function call with its name */
#define DG_LOG_STUB(name) dg_log("STUB: %s\n", name)
#define DG_LOG_CALL(name) dg_log("CALL: %s\n", name)

/* Log a high-frequency call only the first N times */
#define DG_LOG_ONCE(name) do { \
    static int _count = 0; \
    if (_count < 3) { dg_log("CALL: %s\n", name); _count++; } \
} while(0)

#endif /* DG_LOG_H */
