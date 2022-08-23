/* See LICENSE file for copyright and license details. */
#include <sys/stat.h>
#include <glob.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#if defined(__OpenBSD__) || defined(__FreeBSD__)
#include <sys/param.h>
#include <sys/sysctl.h>
#endif
#ifdef __OpenBSD__
#include <sys/sched.h>
#endif
#ifdef __FreeBSD__
#include <devstat.h>
#endif

#include "util.h"

/* enums */
enum { INTEL, CPUFREQ, BROKEN };

/* globals */
static const char *turbopath[] = {
	[INTEL]   = "/sys/devices/system/cpu/intel_pstate/no_turbo",
	[CPUFREQ] = "/sys/devices/system/cpu/cpufreq/boost",
	[BROKEN]  = "", /* no turbo boost support */
};
static size_t ti = BROKEN; /* turbo index */
static unsigned int cpus = 0;

#include "config.h"

static float
avgload(void)
{
	double avg;

	/* get the average load over 1 minute */
	if (getloadavg(&avg, 1) < 0) {
		//printf("getloadavg: Failed to obtain load average");
		return 0.0;
	}

	return avg;
}


static unsigned int
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
		return 0;

	if (b[0] == 0)
		return 0;

	sum = (b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6]) -
	      (a[0] + a[1] + a[2] + a[3] + a[4] + a[5] + a[6]);

	if (sum == 0)
		return 0;

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

//TODO remove this. `sacf &`, `setsid -f sacf`, `sacf & disown`, `nohup sacf`,
//... should do the trick
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

static int
ischarging()
{	//TODO handle multiple AC online ?
	FILE *fp;
	glob_t buf;
	int online;

	eglob("/sys/class/power_supply/A*/online", buf);

	if (!(fp = fopen(buf.gl_pathv[0], "r")))
		die("Error reading file '%s', fopen failed:", buf.gl_pathv[0]);

	online = getc(fp) - '0';
	fclose(fp);
	globfree(&buf);

	return online;
}

static unsigned int
nproc(void)
{
	unsigned int ret;
#ifdef _SC_NPROCESSORS_ONLN
	/* This works on glibc, Mac OS X 10.5, FreeBSD, AIX, OSF/1, Solaris,
	 * Cygwin, Haiku.  */
        ret = sysconf(_SC_NPROCESSORS_ONLN);
#else
	glob_t buf;

	eglob("/sys/devices/system/cpu/cpu[0-9]*", &buf);
	ret = buf.gl_pathc;
	globfree(&buf);
#endif
	return ret;
}

static int
getturbo(void)
{
	FILE *fp;
	int state;

	if (ti == BROKEN)
		return -1;

	if (!(fp = fopen(turbopath[ti], "r")))
		die("Error reading file '%s', fopen failed:", turbopath[ti]);

	/* it's only one character (0 or 1) */
	state = getc(fp) - '0';
	fclose(fp);

	return state;
}

static void
setgovernor(const char *governor)
{
	FILE *fp;
	const char path[] = "/sys/devices/system/cpu/cpu";
	const char end[] = "/cpufreq/scaling_governor";
	unsigned int i;
	char tmp[sizeof(path) + sizeof(i) + sizeof(end) + 5];

	for (i = 0; i < cpus; i++) {
		/* store the path of cpu i on tmp */
		snprintf(tmp, sizeof(tmp), "%s%u%s", path, i, end);

		/* set the governor of cpu i */
		if ((fp = fopen(tmp, "w")) != NULL)
			fprintf(fp, "%s\n", governor);
		else
			die("Error writing file '%s', fopen failed:", tmp);

		fclose(fp);
	}
}

static unsigned int
avgtemp(void)
{
	#ifdef __linux__
	//TODO on some systems, there could exist multiple paths, so get an avg
	//by using glob
	const char file[] = "/sys/class/thermal/thermal_zone0/temp";
	unsigned int temp;

	if (pscanf(file, "%u", &temp) != 1)
		return 0;

	return temp / 1000; /* value in celsius */

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

	/* do nothing if the turbo state is already as desired or turbo boost
	 * is not supported */
	if (i == -1 || i == on)
		return;

	if (!(fp = fopen(turbopath[ti], "w")))
		die("Error writing file '%s', fopen failed:", turbopath[ti]);

	/* change state of turbo boost */
	fprintf(fp, "%d\n", on);

	fclose(fp);
}

static void
run(void)
{
	float threshold;

	if (ischarging())
		setgovernor(acgovernor);
	else
		setgovernor(batgovernor);

	if (alwaysturbo) {
		turbo(1);
		return;
	}

	threshold = (75 * cpus) / 100;

	turbo(cpuperc() >= mincpu
	|| avgtemp() >= mintemp
	|| avgload() >= threshold ? 1 : 0);
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

	/* setup */
	if (access(turbopath[INTEL], F_OK) != -1)
		ti = INTEL; /* intel driver support */
	else if (access(turbopath[CPUFREQ], F_OK) != -1)
		ti = CPUFREQ; /* cpufreq driver support */
	cpus = nproc(); /* store the number of cpus */

	for (i = 1; i < argc; i++)
		/* these options take no arguments */
		if (!strcmp(argv[i], "-v")) {      /* prints version information */
			puts("sacf-"VERSION);
			exit(0);
		} else if (!strcmp(argv[i], "-l")) { /* info that sacf uses */
			fprintf(stdout, "Cores: %u\n", cpus);
			fprintf(stdout, "AC adapter status: %d\n", ischarging());
			fprintf(stdout, "Average system load: %0.2f\n", avgload());
			fprintf(stdout, "System temperature: %u °C\n", avgtemp());
			if (ti != BROKEN) {
				fprintf(stdout, "Turbo state: %d\n", getturbo());
				fprintf(stdout, "Turbo path: %s\n", turbopath[ti]);
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
		run();
		sleep(interval);
	}

	return 0;
}
