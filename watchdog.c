#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <sys/sysinfo.h>

#define min(a, b) ((a) < (b) ? (a) : (b))
#define WATCHDOG_LOG "/mnt/log/watchdog.log"
#define EARLY_REBOOT_LOG "/test/early_reboot.log"
/*keep watchdog file max size 30M*/
#define WATCHDOG_LOG_MAX_SIZE 30 
#define MAX_READ_BYTE 16384
#define CUT_OFF_RULE "\n\n\n\n"

static long s_timestamp = 0;
static int s_stop_feed_dog = 0;

static int si_mem_available()
{
    int free_mem = 0;
    int lru_active_file = 0;
    int lru_inactive_file = 0;
    int slab_reclaimable = 0;
    int totalreserve_pages = 0;
    int page_caches = 0;
    int mwater_low = 0;
    int mwater_high = 0;
    int available_mem = 0;
    int pagesize = 0;
    FILE *fp_meminfo = NULL;
    FILE *fp_zoneinfo = NULL;
    char *tmp = NULL;
    char *normal_zone = NULL;
    char *DMA_zone = NULL;
    char buf[4096] = {0};
    
    pagesize = getpagesize() / 1024; // KB

    fp_zoneinfo = fopen("/proc/zoneinfo", "r");
    if (fp_zoneinfo)
    {
        fread(buf, sizeof(buf), 1, fp_zoneinfo);
        /*DMA zone*/
        DMA_zone = strstr(buf, "DMA");
        if (DMA_zone)
        {
            tmp = strstr(DMA_zone, "low");
            if (tmp)
            {
                mwater_low += strtoul(tmp + strlen("low"), NULL, 0);
            }
            else
            {
                fclose(fp_zoneinfo);
                return -1;
            }

            tmp = strstr(DMA_zone, "high");
            if (tmp)
            {
                mwater_high += strtoul(tmp + strlen("high"), NULL, 0);
            }
            else
            {
                fclose(fp_zoneinfo);
                return -1;
            }
        }
        else
        {
            fclose(fp_zoneinfo);
            return -1;
        }

        /*Normal zone*/
        normal_zone = strstr(buf, "Normal");
        if (normal_zone)
        {
            tmp = strstr(normal_zone, "low");
            if (tmp)
            {
                mwater_low += strtoul(tmp + strlen("low"), NULL, 0);
            }
            else
            {
                fclose(fp_zoneinfo);
                return -1;
            }

            tmp = strstr(normal_zone, "high");
            if (tmp)
            {
                mwater_high += strtoul(tmp + strlen("high"), NULL, 0);
            }
            else
            {
                fclose(fp_zoneinfo);
                return -1;
            }
        }
        else
        {
            fclose(fp_zoneinfo);
            return -1;
        }

        fclose(fp_zoneinfo);
        mwater_low *= pagesize;
        mwater_high *= pagesize;
    }
    else
    {
        return -2;
    }

    fp_meminfo = fopen("/proc/meminfo", "r");
    if (fp_meminfo)
    {
        fread(buf, sizeof(buf), 1, fp_meminfo);
        tmp = strstr(buf, "MemFree:");
        if (tmp)
        {
            free_mem = strtoul(tmp + strlen("MemFree:"), NULL, 0);
        }
        else
        {
            fclose(fp_meminfo);
            return -1;
        }

        tmp = strstr(buf, "Active(file):");
        if (tmp)
        {
            lru_active_file = strtoul(tmp + strlen("Active(file):"), NULL, 0);
        }
        else
        {
            fclose(fp_meminfo);
            return -1;
        }

        tmp = strstr(buf, "Inactive(file):");
        if (tmp)
        {
            lru_inactive_file = strtoul(tmp + strlen("Inactive(file):"), NULL, 0);
        }
        else
        {
            fclose(fp_meminfo);
            return -1;
        }

        tmp = strstr(buf, "SReclaimable:");
        if (tmp)
        {
            slab_reclaimable = strtoul(tmp + strlen("SReclaimable:"), NULL, 0);
        }
        else
        {
            fclose(fp_meminfo);
            return -1;
        }
        fclose(fp_meminfo);
    }
    else
    {
        return -2;
    }
    /*
	 * Estimate the amount of memory available for userspace allocations,
	 * without causing swapping.
	 */
    totalreserve_pages = mwater_high;
    available_mem = free_mem - totalreserve_pages;

    /*
	 * Not all the page cache can be freed, otherwise the system will
	 * start swapping. Assume at least half of the page cache, or the
	 * low watermark worth of cache, needs to stay.
	 */
    page_caches = lru_active_file + lru_inactive_file;
    page_caches -= min(page_caches / 2, mwater_low);
	available_mem += page_caches;

    /*
	 * Part of the reclaimable slab consists of items that are in use,
	 * and cannot be freed. Cap this estimate at the low watermark.
	 */
    available_mem += slab_reclaimable - min(slab_reclaimable/2, mwater_low);


    if (available_mem < 0)
        available_mem = 0;
    return available_mem;
}

static void *thread_task(void *arg)
{
    FILE *fp_log = NULL;
    FILE *early_reboot = NULL;
    FILE *tmp_fp = NULL;
    int fd_log = 0;
    int len = 0;
    int available_mem = 0;
    time_t sec = 0;
    char time_str[64] = {0};
    char buf[MAX_READ_BYTE] = {0};
    char mem_available_str[64] = {0};
    struct stat logFileStat = {0};
    struct sysinfo sys_info = {0};
    
    fp_log = fopen(WATCHDOG_LOG, "a+");
    if (!fp_log)
    {
        return NULL;
    }

    while (1)
    {
        fd_log = fileno(fp_log);
        fstat(fd_log, &logFileStat);
        if (((logFileStat.st_size * 1.0) / (1024*1024)) > WATCHDOG_LOG_MAX_SIZE)
        {
            ftruncate(fd_log, 0);  /* truncate the log file to empty file */
            lseek(fd_log, 0, SEEK_SET);
        }

        /*timestamp*/
        sec = time(NULL);
        ctime_r(&sec, time_str);
        fwrite(time_str, strlen(time_str), 1, fp_log);

        tmp_fp = fopen("/proc/meminfo", "r");
        if (tmp_fp)
        {
           len = fread(buf, 1, MAX_READ_BYTE, tmp_fp);
           buf[len] = '\0';
           fprintf(fp_log, "/proc/meminfo\n%s\n\n\n\n", buf);
           fclose(tmp_fp);
        }

        tmp_fp = fopen("/proc/stat", "r");
        if (tmp_fp)
        {
           len = fread(buf, 1, MAX_READ_BYTE, tmp_fp);
           buf[len] = '\0';
           fprintf(fp_log, "/proc/stat\n%s\n\n\n\n", buf);
           fclose(tmp_fp);
        }

        tmp_fp = popen("uptime", "r");
        if (tmp_fp)
        {
           len = fread(buf, 1, MAX_READ_BYTE, tmp_fp);
           buf[len] = '\0';
           fprintf(fp_log, "uptime\n%s\n\n\n\n", buf);
           pclose(tmp_fp);
        }

        tmp_fp = popen("mpstat", "r");
        if (tmp_fp)
        {
           len = fread(buf, 1, MAX_READ_BYTE, tmp_fp);
           buf[len] = '\0';
           fprintf(fp_log, "mpstat\n%s\n\n\n\n", buf);
           pclose(tmp_fp);
        }

        tmp_fp = popen("iostat", "r");
        if (tmp_fp)
        {
           len = fread(buf, 1, MAX_READ_BYTE, tmp_fp);
           buf[len] = '\0';
           fprintf(fp_log, "iostat\n%s\n\n\n\n", buf);
           pclose(tmp_fp);
        }

        tmp_fp = popen("df -h", "r");
        if (tmp_fp)
        {
           len = fread(buf, 1, MAX_READ_BYTE, tmp_fp);
           buf[len] = '\0';
           fprintf(fp_log, "df -h\n%s\n\n\n\n", buf);
           pclose(tmp_fp);
        }

        tmp_fp = popen("ps -o comm,pid,time,stat,rss,vsz", "r");
        if (tmp_fp)
        {
           len = fread(buf, 1, MAX_READ_BYTE, tmp_fp);
           buf[len] = '\0';
           fprintf(fp_log, "ps\n%s\n\n\n\n", buf);
           pclose(tmp_fp);
        }

        available_mem = si_mem_available();
        fprintf(fp_log, "System available memory is %d KB\n\n", available_mem);
        if (available_mem < 5120)
        {
            fprintf(fp_log, "No available memory, reboot\n");
            fclose(fp_log);

            if ((early_reboot = fopen(EARLY_REBOOT_LOG, "a+")))
            {
                fprintf(early_reboot, "%s", time_str);
                fprintf(early_reboot, "No available memory(%d KB), reboot\n", available_mem);
                fclose(early_reboot);
                s_stop_feed_dog = 1;
            }
            sleep(2);
            /*
             *system("reboot");
             */
        }

        fflush(fp_log);
        sysinfo(&sys_info);
        s_timestamp = sys_info.uptime;
        sleep(30);

    }
    return NULL;
}

int main(void)
{
	int fd = open("/dev/watchdog", O_WRONLY);
	int ret = 0;
    pthread_t thread_id = 0;
    pthread_attr_t attr;
    struct sysinfo sys_info = {0};


	if (fd == -1) {
		perror("watchdog");
		exit(EXIT_FAILURE);
	}

    /*When OOM killer happen, kernel panic,then reboot system*/
    /*
     *system("echo 2 > /proc/sys/vm/panic_on_oom");
     *system("echo 10 > /proc/sys/kernel/panic");
     *system("echo 1 > /proc/sys/vm/oom_kill_allocating_task");
     */
    
    if (pthread_attr_init(&attr) == 0) 
    {
        pthread_attr_setstacksize(&attr, 2097152);
        ret = pthread_create(&thread_id, &attr, thread_task, NULL);
        if (ret == 0)
        {
            pthread_detach(thread_id);
        }
        pthread_attr_destroy(&attr); 
    }

	while (1) 
    {
		ret = write(fd, "\0", 1);
		if (ret != 1) 
        {
			ret = -1;
			break;
		}
        sysinfo(&sys_info);
        /*thread_task don't work */
        if ((sys_info.uptime - s_timestamp) > 180 && s_timestamp)
        {
            break;
        }
        if (s_stop_feed_dog)
        {
            break;
        }

        sleep(10);
    }
    close(fd);
    return ret;
}

