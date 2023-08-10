#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

typedef long long int lli;

bool running = true;
pthread_mutex_t running_mutex;


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


void signalHandler(int signal_number) {
    logError("Got signal. Exiting", __func__);
    // TODO: think there's an atomic variable meant to help with "signal caught" flags
    pthread_mutex_lock(&running_mutex);
    running = false;
    pthread_mutex_unlock(&running_mutex);
    logInfo("Signaled");
}

// TODO: clean up these variable names, error messages, etc
void heartbeat() {
    bool r;

    int sockfd;
    struct addrinfo hints, *temperatureAddr;
    int rv;
    int numbytes;
    // temperature stuff
    int cpuFd;
    char sTemperature[30];
    int iTemperature;
    char metric[500];
    char hostname[100];

    long ticks_per_second = sysconf(_SC_CLK_TCK);


    if (gethostname(hostname, 100) != 0) {
        logError("Failed to gethostname", __func__);
        snprintf(hostname, 100, "INVALID");
    }

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; // set to AF_INET to use IPv4
    hints.ai_socktype = SOCK_DGRAM;

    // Temperature metrics server info
    if ((rv = getaddrinfo("192.168.1.173", "8125", &hints, &temperatureAddr)) != 0) {
        fprintf(stdout, "[ERR] Failed to getaddrinfo for temperature metrics server: %s\n", gai_strerror(rv));
        return;
    }

    if ((sockfd = socket(temperatureAddr->ai_family, temperatureAddr->ai_socktype, temperatureAddr->ai_protocol)) == -1) {
        logError("Failed to create UDP socket for temperature metrics. Bailing on heartbeat.", __func__);
        return;
    }


    // Get initial CPU numbers
    unsigned int user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice;
    unsigned int user1, nice1, system1, idle1, iowait1, irq1, softirq1, steal1, guest1, guest_nice1;
    unsigned int user2, nice2, system2, idle2, iowait2, irq2, softirq2, steal2, guest2, guest_nice2;
    FILE *proc_stats = fopen("/proc/stat", "r");
    int bytes_read = fscanf(proc_stats, "cpu  %u %u %u %u %u %u %u %u %u %u", &user1, &nice1, &system1, &idle1, &iowait1, &irq1, &softirq1, &steal1, &guest1, &guest_nice1);
    fclose(proc_stats);

    pthread_mutex_lock(&running_mutex);
    r = running;
    pthread_mutex_unlock(&running_mutex);
    while (r) {

        cpuFd = open("/sys/class/thermal/thermal_zone0/temp", O_RDONLY);
        numbytes = read(cpuFd, sTemperature, 30);
        if (numbytes > 0) {
            // We only need 10s and 1s digits
            sTemperature[2] = 0;
            iTemperature = atoi(sTemperature);
            if (iTemperature >= 62) {
                logError("CPU temperature is above 62", __func__);
            }
        }
        close(cpuFd);

        iTemperature = 1;


        // MORE WORK TO DO ON CPU USAGE BELOW

        // CPU Utilization
        proc_stats = fopen("/proc/stat", "r");
        bytes_read = fscanf(proc_stats, "cpu  %u %u %u %u %u %u %u %u %u %u", &user2, &nice2, &system2, &idle2, &iowait2, &irq2, &softirq2, &steal2, &guest2, &guest_nice2);
        fclose(proc_stats);

        unsigned int cpu;
        if (bytes_read > 0) {
            user = (user2 - user1) / ticks_per_second;
            system = (system2 - system1) / ticks_per_second;
            idle = (idle2 - idle1) / ticks_per_second;

            cpu = (
                (float)((user2 - user1) + (system2 - system1))
                /
                (float)((user2 - user1) + (system2 - system1) + (idle2 - idle1))
            ) * 100;
            printf("cpu: %d\n", cpu);

            user1 = user2;
            nice1 = nice2;
            system1 = system2;
            idle1 = idle2;
            iowait1 = iowait2;
            irq1 = irq2;
            softirq1 = softirq2;
            steal1 = steal2;
            guest1 = guest2;
            guest_nice1 = guest_nice2;
        }


        numbytes = snprintf(metric, 200, "raspi.celsius.cpu,host=%s:%d|g\ncpu.usage_idle,host=%s2:%d|g", hostname, iTemperature, hostname, idle );

        //printf("Metric: %s\n", metric);
        if ((numbytes = sendto(sockfd, metric, numbytes, 0, temperatureAddr->ai_addr, temperatureAddr->ai_addrlen)) == -1) {
            fprintf(stderr, "Failed to send udp temperature metric. errno: %d\n", errno);
        }

        sleep(10);

        pthread_mutex_lock(&running_mutex);
        r = running;
        pthread_mutex_unlock(&running_mutex);
    }
    close(sockfd);
    freeaddrinfo(temperatureAddr);
    return;
}



int main(int argc, const char **argv) {
    pthread_mutex_init(&running_mutex, NULL);

    signal(SIGINT, signalHandler);

    // heartbeat loop
    heartbeat();
}
