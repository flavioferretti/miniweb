# OpenBSD System Metrics - API Reference

## Quick Reference per Metriche di Sistema

---

## 1. CPU Statistics

### API: `sysctl(KERN_CPTIME)`

```c
#include <sys/sysctl.h>
#include <sys/sched.h>

int mib[2] = {CTL_KERN, KERN_CPTIME};
long cp_time[CPUSTATES];  // CPUSTATES = 5 su OpenBSD
size_t len = sizeof(cp_time);

sysctl(mib, 2, &cp_time, &len, NULL, 0);

// cp_time[CP_USER]   - User time
// cp_time[CP_NICE]   - Nice time
// cp_time[CP_SYS]    - System time
// cp_time[CP_INTR]   - Interrupt time
// cp_time[CP_IDLE]   - Idle time
```

**Calcolo Percentuali:**
```c
long total = 0;
for (int i = 0; i < CPUSTATES; i++) total += cp_time[i];
int user_pct = (cp_time[CP_USER] * 100) / total;
```

**Pledge:** `proc`

---

## 2. Memory Information

### API: `sysctl(HW_PHYSMEM64)` + `sysctl(VM_UVMEXP)`

```c
#include <sys/sysctl.h>
#include <uvm/uvmexp.h>

// Total Physical Memory
int mib[2] = {CTL_HW, HW_PHYSMEM64};
unsigned long physmem;
size_t len = sizeof(physmem);
sysctl(mib, 2, &physmem, &len, NULL, 0);

// Detailed Memory Stats
mib[0] = CTL_VM;
mib[1] = VM_UVMEXP;
struct uvmexp uvm;
len = sizeof(uvm);
sysctl(mib, 2, &uvm, &len, NULL, 0);

// Useful fields:
// uvm.pagesize   - Page size in bytes
// uvm.npages     - Total pages
// uvm.free       - Free pages
// uvm.active     - Active pages
// uvm.inactive   - Inactive pages
// uvm.wired      - Wired (locked) pages
// uvm.swpages    - Swap pages
// uvm.swpginuse  - Swap pages in use
```

**Calcolo in MB:**
```c
long free_mb = (uvm.free * uvm.pagesize) / (1024 * 1024);
```

**Pledge:** `proc`

---

## 3. Swap Information

### API: `swapctl()`

```c
#include <sys/swap.h>

// Get number of swap devices
int nswap = swapctl(SWAP_NSWAP, NULL, 0);

if (nswap > 0) {
    struct swapent *swdev = calloc(nswap, sizeof(*swdev));
    
    // Get swap statistics
    int rnswap = swapctl(SWAP_STATS, swdev, nswap);
    
    for (int i = 0; i < nswap; i++) {
        if (swdev[i].se_flags & SWF_ENABLE) {
            // swdev[i].se_nblks  - Total blocks (512-byte)
            // swdev[i].se_inuse  - Used blocks
            // swdev[i].se_path   - Device path
        }
    }
    
    free(swdev);
}
```

**Conversione a MB:**
```c
// I blocchi sono da 512 byte
long total_mb = (se_nblks * 512) / (1024 * 1024);
```

**Pledge:** `proc`

---

## 4. Disk Usage

### API: `getmntinfo()`

```c
#include <sys/mount.h>

struct statfs *mntbuf;
int mntsize = getmntinfo(&mntbuf, MNT_NOWAIT);

for (int i = 0; i < mntsize; i++) {
    struct statfs *fs = &mntbuf[i];
    
    // fs->f_mntfromname  - Device name (es. "/dev/sd0a")
    // fs->f_mntonname    - Mount point (es. "/")
    // fs->f_fstypename   - Filesystem type (es. "ffs")
    // fs->f_blocks       - Total blocks
    // fs->f_bfree        - Free blocks
    // fs->f_bavail       - Free blocks for non-root
    // fs->f_bsize        - Block size
    
    unsigned long total = fs->f_blocks * fs->f_bsize;
    unsigned long avail = fs->f_bavail * fs->f_bsize;
    unsigned long used = total - avail;
}
```

**Filtrare Filesystem Speciali:**
```c
if (strcmp(fs->f_fstypename, "tmpfs") == 0) continue;
if (strcmp(fs->f_fstypename, "procfs") == 0) continue;
if (strcmp(fs->f_fstypename, "devfs") == 0) continue;
```

**Pledge:** `rpath`

---

## 5. System Uptime

### API: `sysctl(KERN_BOOTTIME)`

```c
#include <sys/sysctl.h>
#include <sys/time.h>

int mib[2] = {CTL_KERN, KERN_BOOTTIME};
struct timeval boottime;
size_t len = sizeof(boottime);

sysctl(mib, 2, &boottime, &len, NULL, 0);

time_t now;
time(&now);
long uptime_seconds = difftime(now, boottime.tv_sec);

long days = uptime_seconds / 86400;
long hours = (uptime_seconds % 86400) / 3600;
long minutes = (uptime_seconds % 3600) / 60;
```

**Pledge:** `proc`

---

## 6. Load Average

### API: `getloadavg()`

```c
#include <stdlib.h>

double loadavg[3];
int n = getloadavg(loadavg, 3);

if (n >= 0) {
    double load_1min = loadavg[0];
    double load_5min = loadavg[1];
    double load_15min = loadavg[2];
}
```

**Pledge:** Nessuno richiesto (POSIX standard)

---

## 7. System Information

### API: `uname()`

```c
#include <sys/utsname.h>

struct utsname uts;
uname(&uts);

// uts.sysname  - "OpenBSD"
// uts.release  - "7.4"
// uts.version  - Versione dettagliata
// uts.machine  - "amd64"
// uts.nodename - Hostname
```

**Pledge:** Nessuno richiesto (POSIX standard)

---

## 8. Hostname

### API: `gethostname()`

```c
#include <unistd.h>

char hostname[256];
gethostname(hostname, sizeof(hostname));
```

**Pledge:** Nessuno richiesto (POSIX standard)

---

## 9. Network Interfaces

### API: `getifaddrs()`

```c
#include <sys/socket.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

struct ifaddrs *ifaddr, *ifa;
getifaddrs(&ifaddr);

for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == NULL) continue;
    
    // ifa->ifa_name       - Interface name (es. "em0")
    // ifa->ifa_flags      - Interface flags
    // ifa->ifa_addr       - Address
    
    // Check if UP and RUNNING
    if (ifa->ifa_flags & IFF_UP) {
        if (ifa->ifa_flags & IFF_RUNNING) {
            // Interface is up and running
        }
    }
    
    // Get IP address
    if (ifa->ifa_addr->sa_family == AF_INET) {
        struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));
    }
}

freeifaddrs(ifaddr);
```

**Pledge:** `inet`

---

## 10. Process Information

### API: `getpid()`, `getrusage()`

```c
#include <sys/resource.h>

// Current process ID
pid_t pid = getpid();

// Resource usage
struct rusage usage;
getrusage(RUSAGE_SELF, &usage);

// usage.ru_utime  - User CPU time
// usage.ru_stime  - System CPU time
// usage.ru_maxrss - Maximum RSS
// usage.ru_ixrss  - Shared memory size
// usage.ru_idrss  - Unshared data size
// usage.ru_isrss  - Unshared stack size
```

**Pledge:** Nessuno richiesto

---

## 11. I/O Statistics (via command)

### Command: `iostat`

```c
FILE *fp = popen("iostat -d 1 2 | tail -n +3", "r");
// Parse output: device KB/t tps MB/s
```

**Pledge:** `exec, proc`

**Alternativa (sysctl):**
```c
// Disponibile ma complesso - richiede parsing di kern.diskstats
int mib[2] = {CTL_HW, HW_DISKSTATS};
// Non raccomandato - usa iostat invece
```

---

## 12. Network Ports

### Command: `netstat`

```c
FILE *fp = popen("netstat -an -f inet | grep LISTEN", "r");
// Parse output per estrarre porte in ascolto
```

**Pledge:** `exec, proc`

**Nota:** Non esiste sysctl diretto per socket aperti

---

## Pledge Requirements Summary

| API/Comando | Pledge Richiesto |
|-------------|------------------|
| `sysctl(KERN_*)` | `proc` |
| `sysctl(VM_*)` | `proc` |
| `swapctl()` | `proc` |
| `getmntinfo()` | `rpath` |
| `getifaddrs()` | `inet` |
| `uname()`, `gethostname()` | Nessuno |
| `getloadavg()` | Nessuno |
| `popen(iostat)` | `exec, proc` |
| `popen(netstat)` | `exec, proc` |

### Pledge Minimo Consigliato per Metrics:
```c
pledge("stdio rpath inet proc exec", NULL);
```

---

## Best Practices

### 1. Ordine di Preferenza:
1. **Sysctl** (veloce, sicuro)
2. **Funzioni POSIX** (portabile)
3. **getmntinfo/statfs** (API BSD)
4. **popen/command** (ultimo ricorso)

### 2. Error Handling:
```c
if (sysctl(mib, 2, &data, &len, NULL, 0) == -1) {
    // Gestisci errore
    // Ritorna valori di default o -1
}
```

### 3. Buffer Safety:
```c
strlcpy(dest, src, sizeof(dest));  // OpenBSD safe string copy
snprintf(buf, sizeof(buf), ...);   // Safe printf
```

### 4. Memory Management:
```c
// Libera sempre le risorse allocate
freeifaddrs(ifaddr);
free(swdev);
pclose(fp);
```

---

## Man Pages Utili

```bash
man 3 sysctl         # System control
man 3 swapctl        # Swap control
man 3 getmntinfo     # Mount info
man 3 getifaddrs     # Network interfaces
man 3 getloadavg     # Load average
man 2 pledge         # Security restrictions
man 2 unveil         # Filesystem restrictions
man 8 iostat         # I/O statistics
man 8 netstat        # Network statistics
man 8 vmstat         # Virtual memory statistics
```

---

## Testing Commands

```bash
# CPU info
sysctl kern.cptime

# Memory info
sysctl vm.uvmexp | grep -E 'free|active|inactive|wired'

# Swap info
swapctl -l

# Disk usage
df -h

# Network interfaces
ifconfig

# System info
uname -a

# Load average
uptime

# I/O stats
iostat -d 1 2

# Network connections
netstat -an | grep LISTEN
```
