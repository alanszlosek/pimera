#include <stdarg.h>
#include <stdio.h>
#include <time.h>

void logError(char const* msg, const char *func) {
    // time prefix
    char t[40];
    time_t rawtime;
    struct tm timeinfo;
    time(&rawtime);
    localtime_r(&rawtime, &timeinfo);
    strftime(t, 40, "%Y-%m-%d %H:%M:%S", &timeinfo);

    fprintf(stdout, "%s [ERR] in %s: %s\n", t, func, msg);
    fflush(stdout);
}
void logInfo(char const* fmt, ...) {
    // time prefix
    char t[40];
    time_t rawtime;
    struct tm timeinfo;
    va_list ap;
    time(&rawtime);
    localtime_r(&rawtime, &timeinfo);
    strftime(t, 40, "%Y-%m-%d %H:%M:%S", &timeinfo);

    fprintf(stdout, "%s [INFO] ", t);

    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fprintf(stdout, "\n");
    fflush(stdout);
}