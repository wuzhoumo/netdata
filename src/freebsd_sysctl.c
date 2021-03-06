#include "common.h"

// NEEDED BY: struct vmstat
#include <sys/vmmeter.h>
// NEEDED BY: struct devstat
#include <sys/devicestat.h>
// NEEDED BY: do_disk_io
#define RRD_TYPE_DISK "disk"

// FreeBSD calculates load averages once every 5 seconds
#define MIN_LOADAVG_UPDATE_EVERY 5

int do_freebsd_sysctl(int update_every, unsigned long long dt) {
    (void)dt;

    static int do_cpu = -1, do_cpu_cores = -1, do_interrupts = -1, do_context = -1, do_forks = -1, do_processes = -1,
        do_loadavg = -1, do_all_processes = -1, do_disk_io = -1;

    if (unlikely(do_cpu == -1)) {
        do_cpu                  = config_get_boolean("plugin:freebsd:sysctl", "cpu utilization", 1);
        do_cpu_cores            = config_get_boolean("plugin:freebsd:sysctl", "per cpu core utilization", 1);
        do_interrupts           = config_get_boolean("plugin:freebsd:sysctl", "cpu interrupts", 1);
        do_context              = config_get_boolean("plugin:freebsd:sysctl", "context switches", 1);
        do_forks                = config_get_boolean("plugin:freebsd:sysctl", "processes started", 1);
        do_processes            = config_get_boolean("plugin:freebsd:sysctl", "processes running", 1);
        do_loadavg              = config_get_boolean("plugin:freebsd:sysctl", "enable load average", 1);
        do_all_processes        = config_get_boolean("plugin:freebsd:sysctl", "enable total processes", 1);

        do_disk_io              = config_get_boolean("plugin:freebsd:sysctl", "stats for all disks", 1);
    }

    RRDSET *st;

    int i;

// NEEDED BY: do_loadavg
    static unsigned long long last_loadavg_usec = 0;
    struct loadavg sysload;

// NEEDED BY: do_cpu, do_cpu_cores
    long cp_time[CPUSTATES];

// NEEDED BY: do_cpu_cores
    int ncpus;
    static long *pcpu_cp_time = NULL;
    char cpuid[8]; // no more than 4 digits expected

// NEEDED BY: do_all_processes, do_processes
    struct vmtotal vmtotal_data;

// NEEDED BY: do_context, do_forks
    u_int u_int_data;

// NEEDED BY: do_interrupts
    size_t intrcnt_size;
    unsigned long nintr = 0;
    static unsigned long *intrcnt = NULL;
    unsigned long long totalintr = 0;

// NEEDED BY: do_disk_io
    #define BINTIME_SCALE 5.42101086242752217003726400434970855712890625e-17 // this is 1000/2^64
    int numdevs;
    static void *devstat_data = NULL;
    struct devstat *dstat;
    struct cur_dstat {
        collected_number duration_read_ms;
        collected_number duration_write_ms;
        collected_number busy_time_ms;
    } cur_dstat;
    struct prev_dstat {
        collected_number bytes_read;
        collected_number bytes_write;
        collected_number operations_read;
        collected_number operations_write;
        collected_number duration_read_ms;
        collected_number duration_write_ms;
        collected_number busy_time_ms;
    } prev_dstat;

    // --------------------------------------------------------------------

    if (last_loadavg_usec <= dt) {
        if (likely(do_loadavg)) {
            if (unlikely(GETSYSCTL("vm.loadavg", sysload))) {
                do_loadavg = 0;
                error("DISABLED: system.load");
            } else {

                st = rrdset_find_bytype("system", "load");
                if (unlikely(!st)) {
                    st = rrdset_create("system", "load", NULL, "load", NULL, "System Load Average", "load", 100, (update_every < MIN_LOADAVG_UPDATE_EVERY) ? MIN_LOADAVG_UPDATE_EVERY : update_every, RRDSET_TYPE_LINE);
                    rrddim_add(st, "load1", NULL, 1, 1000, RRDDIM_ABSOLUTE);
                    rrddim_add(st, "load5", NULL, 1, 1000, RRDDIM_ABSOLUTE);
                    rrddim_add(st, "load15", NULL, 1, 1000, RRDDIM_ABSOLUTE);
                }
                else rrdset_next(st);

                rrddim_set(st, "load1", (collected_number) ((double)sysload.ldavg[0] / sysload.fscale * 1000));
                rrddim_set(st, "load5", (collected_number) ((double)sysload.ldavg[1] / sysload.fscale * 1000));
                rrddim_set(st, "load15", (collected_number) ((double)sysload.ldavg[2] / sysload.fscale * 1000));
                rrdset_done(st);
            }
        }

        last_loadavg_usec = st->update_every * 1000000ULL;
    }
    else last_loadavg_usec -= dt;

    // --------------------------------------------------------------------

    if (likely(do_all_processes | do_processes)) {
        if (unlikely(GETSYSCTL("vm.vmtotal", vmtotal_data))) {
            do_all_processes = 0;
            error("DISABLED: system.active_processes");
            do_processes = 0;
            error("DISABLED: system.processes");
        } else {
            if (likely(do_processes)) {

                st = rrdset_find_bytype("system", "active_processes");
                if (unlikely(!st)) {
                    st = rrdset_create("system", "active_processes", NULL, "processes", NULL, "System Active Processes", "processes", 750, update_every, RRDSET_TYPE_LINE);
                    rrddim_add(st, "active", NULL, 1, 1, RRDDIM_ABSOLUTE);
                }
                else rrdset_next(st);

                rrddim_set(st, "active", (vmtotal_data.t_rq + vmtotal_data.t_dw + vmtotal_data.t_pw + vmtotal_data.t_sl + vmtotal_data.t_sw));
                rrdset_done(st);
            }
            if (likely(do_processes)) {

                st = rrdset_find_bytype("system", "processes");
                if (unlikely(!st)) {
                    st = rrdset_create("system", "processes", NULL, "processes", NULL, "System Processes", "processes", 600, update_every, RRDSET_TYPE_LINE);

                    rrddim_add(st, "running", NULL, 1, 1, RRDDIM_ABSOLUTE);
                    rrddim_add(st, "blocked", NULL, -1, 1, RRDDIM_ABSOLUTE);
                }
                else rrdset_next(st);

                rrddim_set(st, "running", vmtotal_data.t_rq);
                rrddim_set(st, "blocked", (vmtotal_data.t_dw + vmtotal_data.t_pw));
                rrdset_done(st);
            }

        }
    }

    // --------------------------------------------------------------------

    if (likely(do_processes)) {

            st = rrdset_find_bytype("system", "processes");
            if (unlikely(!st)) {
                st = rrdset_create("system", "processes", NULL, "processes", NULL, "System Processes", "processes", 600, update_every, RRDSET_TYPE_LINE);

                rrddim_add(st, "running", NULL, 1, 1, RRDDIM_ABSOLUTE);
                rrddim_add(st, "blocked", NULL, -1, 1, RRDDIM_ABSOLUTE);
            }
            else rrdset_next(st);

            rrddim_set(st, "running", vmtotal_data.t_rq);
            rrddim_set(st, "blocked", (vmtotal_data.t_dw + vmtotal_data.t_pw));
            rrdset_done(st);
        }

    // --------------------------------------------------------------------

    if (likely(do_cpu)) {
        if (unlikely(CPUSTATES != 5)) {
            error("FREEBSD: There are %d CPU states (5 was expected)", CPUSTATES);
            do_cpu = 0;
            error("DISABLED: system.cpu");
        } else {
            if (unlikely(GETSYSCTL("kern.cp_time", cp_time))) {
                do_cpu = 0;
                error("DISABLED: system.cpu");
            } else {

                st = rrdset_find_bytype("system", "cpu");
                if (unlikely(!st)) {
                    st = rrdset_create("system", "cpu", NULL, "cpu", "system.cpu", "Total CPU utilization", "percentage", 100, update_every, RRDSET_TYPE_STACKED);

                    rrddim_add(st, "user", NULL, 1, 1, RRDDIM_PCENT_OVER_DIFF_TOTAL);
                    rrddim_add(st, "nice", NULL, 1, 1, RRDDIM_PCENT_OVER_DIFF_TOTAL);
                    rrddim_add(st, "system", NULL, 1, 1, RRDDIM_PCENT_OVER_DIFF_TOTAL);
                    rrddim_add(st, "interrupt", NULL, 1, 1, RRDDIM_PCENT_OVER_DIFF_TOTAL);
                    rrddim_add(st, "idle", NULL, 1, 1, RRDDIM_PCENT_OVER_DIFF_TOTAL);
                    rrddim_hide(st, "idle");
                }
                else rrdset_next(st);

                rrddim_set(st, "user", cp_time[0]);
                rrddim_set(st, "nice", cp_time[1]);
                rrddim_set(st, "system", cp_time[2]);
                rrddim_set(st, "interrupt", cp_time[3]);
                rrddim_set(st, "idle", cp_time[4]);
                rrdset_done(st);
            }
        }
    }

    // --------------------------------------------------------------------

    if (likely(do_cpu_cores)) {
        if (unlikely(CPUSTATES != 5)) {
            error("FREEBSD: There are %d CPU states (5 was expected)", CPUSTATES);
            do_cpu_cores = 0;
            error("DISABLED: cpu.cpuXX");
        } else {
            if (unlikely(GETSYSCTL("kern.smp.cpus", ncpus))) {
                do_cpu_cores = 0;
                error("DISABLED: cpu.cpuXX");
            } else {
                pcpu_cp_time = reallocz(pcpu_cp_time, sizeof(cp_time) * ncpus);

                for (i = 0; i < ncpus; i++) {
                    if (unlikely(getsysctl("kern.cp_times", pcpu_cp_time, sizeof(cp_time) * ncpus))) {
                        do_cpu_cores = 0;
                        error("DISABLED: cpu.cpuXX");
                        break;
                    }
                    if (unlikely(ncpus > 9999)) {
                        error("FREEBSD: There are more than 4 digits in cpu cores number");
                        do_cpu_cores = 0;
                        error("DISABLED: cpu.cpuXX");
                        break;
                    }
                    snprintfz(cpuid, 8, "cpu%d", i);

                    st = rrdset_find_bytype("cpu", cpuid);
                    if (unlikely(!st)) {
                        st = rrdset_create("cpu", cpuid, NULL, "utilization", "cpu.cpu", "Core utilization", "percentage", 1000, update_every, RRDSET_TYPE_STACKED);

                        rrddim_add(st, "user", NULL, 1, 1, RRDDIM_PCENT_OVER_DIFF_TOTAL);
                        rrddim_add(st, "nice", NULL, 1, 1, RRDDIM_PCENT_OVER_DIFF_TOTAL);
                        rrddim_add(st, "system", NULL, 1, 1, RRDDIM_PCENT_OVER_DIFF_TOTAL);
                        rrddim_add(st, "interrupt", NULL, 1, 1, RRDDIM_PCENT_OVER_DIFF_TOTAL);
                        rrddim_add(st, "idle", NULL, 1, 1, RRDDIM_PCENT_OVER_DIFF_TOTAL);
                        rrddim_hide(st, "idle");
                    }
                    else rrdset_next(st);

                    rrddim_set(st, "user", pcpu_cp_time[i * 5 + 0]);
                    rrddim_set(st, "nice", pcpu_cp_time[i * 5 + 1]);
                    rrddim_set(st, "system", pcpu_cp_time[i * 5 + 2]);
                    rrddim_set(st, "interrupt", pcpu_cp_time[i * 5 + 3]);
                    rrddim_set(st, "idle", pcpu_cp_time[i * 5 + 4]);
                    rrdset_done(st);
                }
            }
        }
    }

    // --------------------------------------------------------------------

    if (likely(do_interrupts)) {
        if (unlikely(sysctlbyname("hw.intrcnt", NULL, &intrcnt_size, NULL, 0) == -1)) {
            error("FREEBSD: sysctl(hw.intrcnt...) failed: %s", strerror(errno));
            do_interrupts = 0;
            error("DISABLED: system.intr");
        } else {
            nintr = intrcnt_size / sizeof(u_long);
            intrcnt = reallocz(intrcnt, nintr * sizeof(u_long));
            if (unlikely(getsysctl("hw.intrcnt", intrcnt, nintr * sizeof(u_long)))){
                do_interrupts = 0;
                error("DISABLED: system.intr");
            } else {
                for (i = 0; i < nintr; i++)
                    totalintr += intrcnt[i];

                st = rrdset_find_bytype("system", "intr");
                if (unlikely(!st)) {
                    st = rrdset_create("system", "intr", NULL, "interrupts", NULL, "Total Device Interrupts", "interrupts/s", 900, update_every, RRDSET_TYPE_LINE);
                    st->isdetail = 1;

                    rrddim_add(st, "interrupts", NULL, 1, 1, RRDDIM_INCREMENTAL);
                }
                else rrdset_next(st);

                rrddim_set(st, "interrupts", totalintr);
                rrdset_done(st);
            }
        }
    }

    // --------------------------------------------------------------------

    if (likely(do_context)) {
        if (unlikely(GETSYSCTL("vm.stats.sys.v_swtch", u_int_data))) {
            do_context = 0;
            error("DISABLED: system.ctxt");
        } else {

            st = rrdset_find_bytype("system", "ctxt");
            if (unlikely(!st)) {
                st = rrdset_create("system", "ctxt", NULL, "processes", NULL, "CPU Context Switches", "context switches/s", 800, update_every, RRDSET_TYPE_LINE);

                rrddim_add(st, "switches", NULL, 1, 1, RRDDIM_INCREMENTAL);
            }
            else rrdset_next(st);

            rrddim_set(st, "switches", u_int_data);
            rrdset_done(st);
        }
    }

    // --------------------------------------------------------------------

    if (likely(do_forks)) {
        if (unlikely(GETSYSCTL("vm.stats.vm.v_forks", u_int_data))) {
            do_forks = 0;
            error("DISABLED: system.forks");
        } else {

            st = rrdset_find_bytype("system", "forks");
            if (unlikely(!st)) {
                st = rrdset_create("system", "forks", NULL, "processes", NULL, "Started Processes", "processes/s", 700, update_every, RRDSET_TYPE_LINE);
                st->isdetail = 1;

                rrddim_add(st, "started", NULL, 1, 1, RRDDIM_INCREMENTAL);
            }
            else rrdset_next(st);

            rrddim_set(st, "started", u_int_data);
            rrdset_done(st);
        }
    }

    // --------------------------------------------------------------------

    if (likely(do_disk_io)) {
        if (unlikely(GETSYSCTL("kern.devstat.numdevs", numdevs))) {
            do_disk_io = 0;
            error("DISABLED: disk.io");
        } else {
            devstat_data = reallocz(devstat_data, sizeof(long) + sizeof(struct devstat) * numdevs); // there is generation number before devstat structures
            if (unlikely(getsysctl("kern.devstat.all", devstat_data, sizeof(long) + sizeof(struct devstat) * numdevs))) {
                do_disk_io = 0;
                error("DISABLED: disk.io");
            } else {
                dstat = devstat_data +sizeof(long); // skip generation number
                collected_number total_disk_reads = 0;
                collected_number total_disk_writes = 0;

                for (i = 0; i < numdevs; i++) {
                    if ((dstat[i].device_type == (DEVSTAT_TYPE_IF_SCSI | DEVSTAT_TYPE_DIRECT)) || (dstat[i].device_type == (DEVSTAT_TYPE_IF_IDE | DEVSTAT_TYPE_DIRECT))) {

                        // --------------------------------------------------------------------

                        st = rrdset_find_bytype(RRD_TYPE_DISK, dstat[i].device_name);
                        if (unlikely(!st)) {
                            st = rrdset_create(RRD_TYPE_DISK, dstat[i].device_name, NULL, dstat[i].device_name, "disk.io", "Disk I/O Bandwidth", "kilobytes/s", 2000, update_every, RRDSET_TYPE_AREA);

                            rrddim_add(st, "reads", NULL, 1, 1024, RRDDIM_INCREMENTAL);
                            rrddim_add(st, "writes", NULL, -1, 1024, RRDDIM_INCREMENTAL);
                        }
                        else rrdset_next(st);

                        total_disk_reads += dstat[i].bytes[DEVSTAT_READ];
                        total_disk_writes += dstat[i].bytes[DEVSTAT_WRITE];
                        prev_dstat.bytes_read = rrddim_set(st, "reads", dstat[i].bytes[DEVSTAT_READ]);
                        prev_dstat.bytes_write = rrddim_set(st, "writes", dstat[i].bytes[DEVSTAT_WRITE]);
                        rrdset_done(st);

                        // --------------------------------------------------------------------

                        st = rrdset_find_bytype("disk_ops", dstat[i].device_name);
                        if (unlikely(!st)) {
                            st = rrdset_create("disk_ops", dstat[i].device_name, NULL, dstat[i].device_name, "disk.ops", "Disk Completed I/O Operations", "operations/s", 2001, update_every, RRDSET_TYPE_LINE);
                            st->isdetail = 1;

                            rrddim_add(st, "reads", NULL, 1, 1, RRDDIM_INCREMENTAL);
                            rrddim_add(st, "writes", NULL, -1, 1, RRDDIM_INCREMENTAL);
                        }
                        else rrdset_next(st);

                        prev_dstat.operations_read = rrddim_set(st, "reads", dstat[i].operations[DEVSTAT_READ]);
                        prev_dstat.operations_write = rrddim_set(st, "writes", dstat[i].operations[DEVSTAT_WRITE]);
                        rrdset_done(st);

                        // --------------------------------------------------------------------

                        st = rrdset_find_bytype("disk_qops", dstat[i].device_name);
                        if (unlikely(!st)) {
                            st = rrdset_create("disk_qops", dstat[i].device_name, NULL, dstat[i].device_name, "disk.qops", "Disk Current I/O Operations", "operations", 2002, update_every, RRDSET_TYPE_LINE);
                            st->isdetail = 1;

                            rrddim_add(st, "operations", NULL, 1, 1, RRDDIM_ABSOLUTE);
                        }
                        else rrdset_next(st);

                        rrddim_set(st, "operations", dstat[i].start_count - dstat[i].end_count);
                        rrdset_done(st);

                        // --------------------------------------------------------------------

                        st = rrdset_find_bytype("disk_util", dstat[i].device_name);
                        if (unlikely(!st)) {
                            st = rrdset_create("disk_util", dstat[i].device_name, NULL, dstat[i].device_name, "disk.util", "Disk Utilization Time", "% of time working", 2004, update_every, RRDSET_TYPE_AREA);
                            st->isdetail = 1;

                            rrddim_add(st, "utilization", NULL, 1, 10, RRDDIM_INCREMENTAL);
                        }
                        else rrdset_next(st);

                        cur_dstat.busy_time_ms = dstat[i].busy_time.sec * 1000 + dstat[i].busy_time.frac * BINTIME_SCALE;
                        prev_dstat.busy_time_ms = rrddim_set(st, "utilization", cur_dstat.busy_time_ms);
                        rrdset_done(st);

                        // --------------------------------------------------------------------

                        st = rrdset_find_bytype("disk_iotime", dstat[i].device_name);
                        if (unlikely(!st)) {
                            st = rrdset_create("disk_iotime", dstat[i].device_name, NULL, dstat[i].device_name, "disk.iotime", "Disk Total I/O Time", "milliseconds/s", 2022, update_every, RRDSET_TYPE_LINE);
                            st->isdetail = 1;

                            rrddim_add(st, "reads", NULL, 1, 1, RRDDIM_INCREMENTAL);
                            rrddim_add(st, "writes", NULL, -1, 1, RRDDIM_INCREMENTAL);
                        }
                        else rrdset_next(st);

                        cur_dstat.duration_read_ms = dstat[i].duration[DEVSTAT_READ].sec * 1000 + dstat[i].duration[DEVSTAT_READ].frac * BINTIME_SCALE;
                        cur_dstat.duration_write_ms = dstat[i].duration[DEVSTAT_WRITE].sec * 1000 + dstat[i].duration[DEVSTAT_READ].frac * BINTIME_SCALE;
                        prev_dstat.duration_read_ms = rrddim_set(st, "reads", cur_dstat.duration_read_ms);
                        prev_dstat.duration_write_ms = rrddim_set(st, "writes", cur_dstat.duration_write_ms);
                        rrdset_done(st);

                        // --------------------------------------------------------------------
                        // calculate differential charts
                        // only if this is not the first time we run

                        if (likely(dt)) {

                            // --------------------------------------------------------------------

                            st = rrdset_find_bytype("disk_await", dstat[i].device_name);
                            if (unlikely(!st)) {
                                st = rrdset_create("disk_await", dstat[i].device_name, NULL, dstat[i].device_name, "disk.await", "Average Completed I/O Operation Time", "ms per operation", 2005, update_every, RRDSET_TYPE_LINE);
                                st->isdetail = 1;

                                rrddim_add(st, "reads", NULL, 1, 1, RRDDIM_ABSOLUTE);
                                rrddim_add(st, "writes", NULL, -1, 1, RRDDIM_ABSOLUTE);
                            }
                            else rrdset_next(st);

                            rrddim_set(st, "reads", (dstat[i].operations[DEVSTAT_READ] - prev_dstat.operations_read) ? 
                                (cur_dstat.duration_read_ms - prev_dstat.duration_read_ms) / (dstat[i].operations[DEVSTAT_READ] - prev_dstat.operations_read) : 0);
                            rrddim_set(st, "writes", (dstat[i].operations[DEVSTAT_WRITE] - prev_dstat.operations_write) ?
                                (cur_dstat.duration_write_ms - prev_dstat.duration_write_ms) / (dstat[i].operations[DEVSTAT_WRITE] - prev_dstat.operations_write) : 0);
                            rrdset_done(st);

                            // --------------------------------------------------------------------

                            st = rrdset_find_bytype("disk_avgsz", dstat[i].device_name);
                            if (unlikely(!st)) {
                                st = rrdset_create("disk_avgsz", dstat[i].device_name, NULL, dstat[i].device_name, "disk.avgsz", "Average Completed I/O Operation Bandwidth", "kilobytes per operation", 2006, update_every, RRDSET_TYPE_AREA);
                                st->isdetail = 1;

                                rrddim_add(st, "reads", NULL, 1, 1024, RRDDIM_ABSOLUTE);
                                rrddim_add(st, "writes", NULL, -1, 1024, RRDDIM_ABSOLUTE);
                            }
                            else rrdset_next(st);

                            rrddim_set(st, "reads", (dstat[i].operations[DEVSTAT_READ] - prev_dstat.operations_read) ?
                                (dstat[i].bytes[DEVSTAT_READ] - prev_dstat.bytes_read) / (dstat[i].operations[DEVSTAT_READ] - prev_dstat.operations_read) : 0);
                            rrddim_set(st, "writes", (dstat[i].operations[DEVSTAT_WRITE] - prev_dstat.operations_write) ?
                                (dstat[i].bytes[DEVSTAT_WRITE] - prev_dstat.bytes_write) / (dstat[i].operations[DEVSTAT_WRITE] - prev_dstat.operations_write) : 0);
                            rrdset_done(st);

                            // --------------------------------------------------------------------

                            st = rrdset_find_bytype("disk_svctm", dstat[i].device_name);
                            if (unlikely(!st)) {
                                st = rrdset_create("disk_svctm", dstat[i].device_name, NULL, dstat[i].device_name, "disk.svctm", "Average Service Time", "ms per operation", 2007, update_every, RRDSET_TYPE_LINE);
                                st->isdetail = 1;

                                rrddim_add(st, "svctm", NULL, 1, 1, RRDDIM_ABSOLUTE);
                            }
                            else rrdset_next(st);

                            rrddim_set(st, "svctm", ((dstat[i].operations[DEVSTAT_READ] - prev_dstat.operations_read) + (dstat[i].operations[DEVSTAT_WRITE] - prev_dstat.operations_write)) ?
                                (cur_dstat.busy_time_ms - prev_dstat.busy_time_ms) / ((dstat[i].operations[DEVSTAT_READ] - prev_dstat.operations_read) + (dstat[i].operations[DEVSTAT_WRITE] - prev_dstat.operations_write)) : 0);
                            rrdset_done(st);

                        }

                    }

                    // --------------------------------------------------------------------

                    st = rrdset_find_bytype("system", "io");
                    if (unlikely(!st)) {
                        st = rrdset_create("system", "io", NULL, "disk", NULL, "Disk I/O", "kilobytes/s", 150, update_every, RRDSET_TYPE_AREA);
                        rrddim_add(st, "in",  NULL,  1, 1024, RRDDIM_INCREMENTAL);
                        rrddim_add(st, "out", NULL, -1, 1024, RRDDIM_INCREMENTAL);
                    }
                    else rrdset_next(st);

                    rrddim_set(st, "in", total_disk_reads);
                    rrddim_set(st, "out", total_disk_writes);
                    rrdset_done(st);
                }
            }
        }
    }

    return 0;
}
