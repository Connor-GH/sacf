/* See LICENSE file for copyright and license details. */
#include <sys/types.h>
#include <sys/stat.h>
#include <arpa/inet.h>

#include <dirent.h>
#include <errno.h>

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

#ifdef __OpenBSD__
#include <sys/param.h>
#include <sys/sched.h>
#include <sys/sysctl.h>
#endif

#ifdef __FreeBSD__
#include <sys/param.h>
#include <sys/sysctl.h>
#include <devstat.h>
#endif

#include "util.h"
/* macros */
#define LENGTH(a)               (sizeof(a) / sizeof(a)[0])

#include "config.h"

static float
avgload(void)
{
	double avg;

	/* get the average load over 1 minute */
	if (getloadavg(&avg, 1) < 0) {
		//printf("getloadavg: Failed to obtain load average");
		return -1;
	}

	return avg;
}


static int
cpuperc(void)
{
	#ifdef __linux__
	static long double a[7];
	long double b[7], sum;

	memcpy(b, a, sizeof(b));
	/* cpu user nice system idle iowait irq softirq */
	if (pscanf("/proc/stat", "%*s %Lf %Lf %Lf %Lf %Lf %Lf %Lf",
	           &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6])
	    != 7)
		return -1;

	if (b[0] == 0)
		return -1;

	sum = (b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6]) -
	      (a[0] + a[1] + a[2] + a[3] + a[4] + a[5] + a[6]);

	if (sum == 0)
		return -1;

	return (int)(100 *
	               ((b[0] + b[1] + b[2] + b[5] + b[6]) -
	                (a[0] + a[1] + a[2] + a[5] + a[6])) / sum);
	#endif /* __linux__ */

	#ifdef __OpenBSD__
	//TODO openbsd support
	#endif /* __OpenBSD__ */

	#ifdef __FreeBSD__
	//TODO freebsd support
	#endif
}

static void
daemonize(void)
{
	/* child process */
	pid_t id = fork();

	if (id < 0)
		die("fork failed.");

	/* parent process */
	if (id > 0)
		exit(0); //kill the parent

	/* unmask the file mode */
	umask(0);

	/* new session */
	if (setsid() < 0)
		die("setsid failed:");

	/* close stdin, stdout and stderr */
	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);
}

static char
ischarging()
{
	char online;
	//TODO handle multiple AC online ?

	//there has to be a better way?
	FILE *fp = popen("/bin/grep . /sys/class/power_supply/A*/online 2>/dev/null", "r");
	if (fp == NULL)
		return -1;
		//die("Failed to run grep.");

	//while (fgets(online, sizeof(online), fp) != NULL) {
	//online = fgetc(fp);

	/* it's only one character (0 or 1) */
	online = getc(fp);
	pclose(fp);

	return online;
}

static unsigned int
nproc(void)
{
	unsigned int threads;
	FILE *fp = fopen("/proc/cpuinfo", "r");

	if (fp == NULL)
		return -1;

	while (!fscanf(fp, "siblings\t: %u", &threads))
		fscanf(fp, "%*[^s]");

	fclose(fp);

	return threads;
}

static const char *
turbopath(void)
{
	static const char intel[] = "/sys/devices/system/cpu/intel_pstate/no_turbo";
	static const char boost[] = "/sys/devices/system/cpu/cpufreq/boost";

	/* figure what path to use */
	if (access(intel, F_OK) != -1)
		return intel;
	else if (access(boost, F_OK) != -1)
		return boost;
	else /* cpu turbo it's not avaliable */
		return NULL;
}

static char
getturbo(void)
{
	//FIXME temporal solution, should be using fopen and fgetc
	char cmd[50], state;
	FILE *fp;

	if (turbopath() == NULL)
		return -1;

	snprintf(cmd, LENGTH(cmd), "cat %s", turbopath());

	if (!(fp = popen(cmd, "r")))
		return -1;

	/* it's only one character (0 or 1) */
	state = getc(fp);
	pclose(fp);

	return state;
}

static void
setgovernor(const char *governor)
{
	FILE *fp;
	const char path[] = "/sys/devices/system/cpu/cpu";
	const char end[] = "/cpufreq/scaling_governor";
	unsigned int i;
	char tmp[LENGTH(path) + sizeof(i) + LENGTH(end)];

	for (i = 0; i < nproc(); i++) {
		/* store the path of cpu i on tmp */
		snprintf(tmp, LENGTH(tmp), "%s%u%s", path, i, end);

		/* set the governor of cpu i */
		if ((fp = fopen(tmp, "w")) != NULL)
			fprintf(fp, "%s\n", governor);
		else
			die("Error opening file, fopen failed:");

		fclose(fp);
	}
}

static int
avgtemp(void)
{
	#ifdef __linux__
	//uintmax_t temp;
	unsigned int temp;
	//TODO on some systems, there could exist multiple path, so get an avg
	char file[] = "/sys/class/thermal/thermal_zone0/temp";

	if (pscanf(file, "%u", &temp) != 1) {
		return -1;
	}

	/* value in celsius */
	return temp / 1000;
	#endif /* __linux__ */

	#ifdef __OpenBSD__
	//TODO openbsd support
	#endif /* __OpenBSD__ */

	#ifdef __FreeBSD__
	//TODO freebsd support
	#endif
}

static void
turbo(int on)
{
	FILE *fp;
	int i = getturbo();

	/* do nothing if the turbo state is already as desired */
	if (i != -1 && i == on)
		return;

	if (!(fp = fopen(turbopath(), "w")))
		return;

	/* change state of turbo boost */
	fprintf(fp, "%d\n", on);

	fclose(fp);
}

static void
run(void)
{
	const char epp[] = "/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference";
	const char intel[] = "/sys/devices/system/cpu/intel_pstate/hwp_dynamic_boost";
	int bat = ischarging();

	/* if energy_performance_preference exist and no intel_pstate, use
	 * balance governor */
	if (access(epp, F_OK) != -1 && access(intel, F_OK) == -1) {
		if (bat)
			setgovernor("balance_power");
		else
			setgovernor("balance_performance");
	} else { /* use default governors */
		if (bat)
			setgovernor(batgovernor);
		else
			setgovernor(acgovernor);
	}

	if (alwaysturbo) {
		turbo(1);
		return;
	}

	float load_threshold = (75 * nproc()) / 100;
	float sysload = avgload();

	turbo(cpuperc() >= mincpu
	|| avgtemp() >= mintemp
	|| sysload >= load_threshold ? 1 : 0);
}

static void
usage(void)
{
	die("usage: sacf [-blrtTv] [-g governor]");
}

int
main(int argc, char *argv[])
{
	int i;

	for (i = 1; i < argc; i++)
		/* these options take no arguments */
		if (!strcmp(argv[i], "-v")) {      /* prints version information */
			puts("sacf-"VERSION);
			exit(0);
		} else if (!strcmp(argv[i], "-l")) { /* info that sacf uses */
			const char *tp = turbopath();
			fprintf(stdout, "Cores: %u\n", nproc());
			if (ischarging() != -1)
				fprintf(stdout, "AC adapter status: %c\n", ischarging());
			else
				fprintf(stdout, "AC adapter status could not be retrieved.\n");
			fprintf(stdout, "Average system load: %0.2f\n", avgload());
			fprintf(stdout, "System temperature: %d °C\n", avgtemp());
			if (tp != NULL) {
				fprintf(stdout, "Turbo state: %c\n", getturbo());
				fprintf(stdout, "Turbo path: %s\n", tp);
			} else
				fprintf(stdout, "CPU turbo boost is not available.\n");
			exit(0);
		} else if (!strcmp(argv[i], "-t")) { /* turbo on */
			turbo(1);
			exit(0);
		} else if (!strcmp(argv[i], "-T")) { /* turbo off */
			turbo(0);
			exit(0);
		} else if (!strcmp(argv[i], "-r")) { /* run once */
			run();
			exit(0);
		} else if (!strcmp(argv[i], "-b")
			|| !strcmp(argv[i], "--daemon")) { /* daemon mode */
			daemonize();
		} else if (i + 1 == argc)
			usage();
		/* these options take one argument */
		else if (!strcmp(argv[i], "-g")) { /* set governor */
			setgovernor(argv[++i]);
			exit(0);
		} else
			usage();

	while (1) {
		/* use 1 seconds if the interval is set to 0 */
		sleep(interval <= 0 ? 1 : interval);
		run();
	}

	return 0;
}
